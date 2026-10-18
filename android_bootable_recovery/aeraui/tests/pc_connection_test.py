#!/usr/bin/env python3
"""Exercise the real HTTPS service against a fake installer, never a device."""
import hashlib
import base64
import http.client
import http.server
import io
import json
import os
import pathlib
import shutil
import socket
import ssl
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile


class PcConnectionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # /tmp may be tmpfs, which the production upload path intentionally rejects.
        cls.temporary = tempfile.TemporaryDirectory(prefix="pc-storage-", dir=pathlib.Path.home())
        cls.root = pathlib.Path(cls.temporary.name)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            cls.port = sock.getsockname()[1]
        cls.process = subprocess.Popen([sys.argv[1], str(cls.root), str(cls.port)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert cls.process.stdout.readline().strip() == "READY"
        cls.command("forget")
        cls.context = ssl._create_unverified_context()  # Fixture certificate only.
        cls.key = "a" * 64
        cls.token = ""

    @classmethod
    def tearDownClass(cls):
        if cls.process.poll() is None:
            cls.command("quit")
            cls.process.wait(timeout=10)
        cls.process.stdin.close()
        cls.process.stdout.close()
        cls.temporary.cleanup()

    @classmethod
    def command(cls, text):
        cls.process.stdin.write(text + "\n")
        cls.process.stdin.flush()
        if text == "quit":
            return
        while True:
            line = cls.process.stdout.readline().strip()
            if line == "OK " + text:
                return
            if not line:
                raise AssertionError("Host fixture unexpectedly stopped")

    @classmethod
    def request(cls, path, body=None, method=None, token=None, raw=False):
        raw_output = raw
        headers = {}
        active_token = cls.token if token is None else token
        if active_token:
            headers["Authorization"] = "Bearer " + active_token
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPSConnection("127.0.0.1", cls.port, context=cls.context, timeout=8)
        connection.request(method or ("POST" if body is not None else "GET"), path, body, headers)
        response = connection.getresponse()
        raw = response.read()
        status = response.status
        connection.close()
        return status, raw if raw_output else json.loads(raw) if raw else {}

    def wait_phase(self, phase):
        until = time.monotonic() + 8
        while time.monotonic() < until:
            status, result = self.request("/api/status")
            self.assertEqual(status, 200)
            if result["job"]["phase"] == phase:
                return result["job"]
            time.sleep(.05)
        self.fail(f"Did not reach {phase}: {result}")

    def prepare(self, data, **extra):
        body = dict(name="test.zip", bytes=len(data), kind="zip", storage=str(self.root),
                    sha256=hashlib.sha256(data).hexdigest())
        body.update(extra)
        return self.request("/api/prepare", body)

    def test_01_approval_is_required(self):
        self.assertEqual(self.request("/api/status", token="")[0], 401)
        self.assertEqual(self.request("/api/install", {"id": "x", "confirm": True}, token="")[0], 401)
        status, connection = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        self.assertEqual((status, connection["state"]), (202, "pending"))
        self.command("remember")
        status, approved = self.request("/api/connect/status", {"key": self.key, "request": connection["request"]}, token="")
        self.assertEqual((status, approved["state"]), (200, "allowed"))
        type(self).token = approved["token"]
        self.assertTrue(self.request("/api/status")[1]["remembered"])

    def test_01a_usb_survives_wifi_and_address_changes(self):
        status, initial = self.request("/api/status")
        self.assertEqual(status, 200)
        self.assertEqual(initial["ip_address"], "")
        self.assertEqual(initial["usb_address"], f"https://localhost:{self.port}")
        _, certificate = self.request("/api/certificate", raw=True)
        trusted = ssl.create_default_context(cadata=certificate.decode())
        for command, address in (("wifi-on", "127.0.0.2"), ("wifi-change", "127.0.0.3"), ("wifi-off", "")):
            self.command(command)
            status, current = self.request("/api/status")
            self.assertEqual(status, 200)  # The original USB session is still authorized.
            self.assertEqual(current["certificate"], initial["certificate"])
            self.assertEqual(current["ip_address"], f"https://{address}:{self.port}" if address else "")
            if address:
                connection = http.client.HTTPSConnection(address, self.port, context=trusted, timeout=8)
                connection.request("GET", "/api/hello")
                self.assertEqual(connection.getresponse().status, 200)
                connection.close()
        connection = http.client.HTTPSConnection("localhost", self.port, context=trusted, timeout=8)
        connection.request("GET", "/api/hello")
        self.assertEqual(connection.getresponse().status, 200)
        connection.close()

    def test_01b_status_reuses_the_same_tls_socket(self):
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=8)
        connection.connect()
        original_socket = connection.sock
        try:
            for _ in range(3):
                connection.request("GET", "/api/status", headers={"Authorization": "Bearer " + self.token})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(response.getheader("Connection"), "keep-alive")
                response.read()
                self.assertIs(connection.sock, original_socket)
        finally:
            connection.close()

    def test_01c_terminal_shell_and_operation_lock(self):
        if not self.request("/api/status")[1].get("terminal_available"):
            self.skipTest("AERA_TEST_TTYD is not configured")
        self.assertEqual(self.request("/api/terminal/open", {}, token="")[0], 401)
        self.assertEqual(self.request("/terminal/", raw=True, token="")[0], 403)
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=5)
        connection.request("POST", "/api/terminal/open", b"{}",
                           {"Authorization": "Bearer " + self.token})
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        cookie_header = response.getheader("Set-Cookie")
        self.assertIn("Secure; HttpOnly; SameSite=Strict", cookie_header)
        cookie = cookie_header.split(";", 1)[0]
        self.assertEqual(json.loads(response.read())["url"], "/terminal/")
        connection.close()

        def terminal_get(path, cookie_value=cookie):
            conn = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=5)
            conn.request("GET", path, headers={"Cookie": cookie_value})
            reply = conn.getresponse()
            status, payload = reply.status, reply.read()
            conn.close()
            return status, payload

        status, page = terminal_get("/terminal/")
        self.assertEqual(status, 200)
        self.assertGreater(len(page), 10000)
        self.assertEqual(terminal_get("/terminal/", "aera_terminal=" + "0" * 64)[0], 403)
        status, payload = terminal_get("/terminal/token")
        self.assertEqual(status, 200)
        credential = json.loads(payload)["token"]
        sock = self.context.wrap_socket(socket.create_connection(("127.0.0.1", self.port)),
                                        server_hostname="localhost")
        sock.settimeout(5)
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall((f"GET /terminal/ws HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\n"
                      f"Origin: https://127.0.0.1:{self.port}\r\nCookie: {cookie}\r\n"
                      "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                      f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Protocol: tty\r\n\r\n").encode())
        buffered = bytearray()

        def take(count):
            while len(buffered) < count:
                data = sock.recv(32768)
                if not data:
                    raise EOFError("Terminal disconnected")
                buffered.extend(data)
            result = bytes(buffered[:count])
            del buffered[:count]
            return result

        while b"\r\n\r\n" not in buffered:
            buffered.extend(sock.recv(32768))
        headers, remainder = buffered.split(b"\r\n\r\n", 1)
        buffered[:] = remainder
        self.assertIn(b"101", headers.split(b"\r\n", 1)[0])

        def send_frame(data):
            mask = os.urandom(4)
            length = len(data)
            header = bytes((0x82, 0x80 | length)) if length < 126 else bytes((0x82, 0xfe)) + length.to_bytes(2, "big")
            sock.sendall(header + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(data)))

        def output_until(expected):
            output = b""
            while expected not in output:
                head = take(2)
                length = head[1] & 127
                if length == 126:
                    length = int.from_bytes(take(2), "big")
                elif length == 127:
                    length = int.from_bytes(take(8), "big")
                self.assertLess(length, 1024 * 1024)
                payload = take(length)
                if head[0] & 15 == 8:
                    raise EOFError("Terminal closed its WebSocket")
                if payload[:1] == b"0":
                    output += payload[1:]
            return output

        try:
            send_frame(json.dumps({"AuthToken": credential, "columns": 90, "rows": 24}).encode())
            send_frame(b"0printf '\\101\\105\\122\\101\\137\\117\\113\\n'\r")
            self.assertIn(b"AERA_OK", output_until(b"AERA_OK"))
            send_frame(b'1{"columns":100,"rows":30}')
            send_frame(b"0stty size\r")
            self.assertIn(b"30 100", output_until(b"30 100"))
            send_frame(b"0sleep 30\r")
            time.sleep(.1)
            send_frame(b"0\x03")
            send_frame(b"0printf '\\101\\106\\124\\105\\122\\137\\111\\116\\124\\n'\r")
            self.assertIn(b"AFTER_INT", output_until(b"AFTER_INT"))
            self.command("busy")
            self.assertEqual(self.request("/api/terminal/open", {})[0], 200)
            self.assertEqual(terminal_get("/terminal/")[0], 200)
            send_frame(b"0printf '\\102\\125\\123\\131\\137\\117\\113\\n'\r")
            self.assertIn(b"BUSY_OK", output_until(b"BUSY_OK"))
        finally:
            sock.close()
            self.command("idle")
            self.assertEqual(self.request("/api/terminal/close", {})[0], 200)

    def test_02_partial_or_bad_upload_cannot_install(self):
        data = b"fixture-data" * 8192
        status, job = self.prepare(data, sha256="0" * 64)
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 409)
        self.assertEqual(self.request("/api/upload/" + job["id"], data, "PUT")[0], 409)
        self.wait_phase("failed")
        self.assertFalse(list(self.root.rglob("payload*")))
        status, job = self.prepare(data)
        self.assertEqual(status, 200)
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=8)
        connection.connect()
        connection.putrequest("PUT", "/api/upload/" + job["id"])
        connection.putheader("Authorization", "Bearer " + self.token)
        connection.putheader("Content-Length", str(len(data)))
        connection.endheaders()
        connection.send(data[:4096])
        connection.close()
        self.wait_phase("failed")
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 409)
        self.assertFalse(list(self.root.rglob("payload*")))

    def test_02a_upload_progress_and_cancellation(self):
        data = b"upload fixture" * 65536
        for native in (False, True):
            status, job = self.prepare(data, kind="file", name="cancelled.txt", folder=str(self.root))
            self.assertEqual(status, 200)
            connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=8)
            connection.connect()
            connection.putrequest("PUT", "/api/upload/" + job["id"])
            connection.putheader("Authorization", "Bearer " + self.token)
            connection.putheader("Content-Length", str(len(data)))
            connection.endheaders()
            try:
                for offset in range(0, 4 * 65536, 65536):
                    time.sleep(.3)
                    connection.send(data[offset:offset + 65536])
                self.wait_phase("uploading")
                status, state = self.request("/api/status")
                self.assertFalse(state["busy"])
                progress = state["job"]
                self.assertEqual(progress["bytes"], len(data))
                self.assertGreater(progress["received"], 0)
                self.assertLess(progress["received"], len(data))
                self.assertTrue(progress["upload_transfer"]["active"])
                self.assertGreater(progress["upload_transfer"]["bytes_per_second"], 0)
                self.assertGreater(progress["upload_transfer"]["remaining_seconds"], 0)
                if native:
                    self.command("cancel-upload")
                else:
                    self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
                cancelled = self.wait_phase("cancelled")
                self.assertTrue(cancelled["cancel_requested"])
                self.assertFalse(cancelled["upload_transfer"]["active"])
            finally:
                connection.close()
            self.assertFalse((self.root / "cancelled.txt").exists())
            self.assertFalse(list(self.root.rglob("payload*")))
            self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 409)

    def test_02b_only_owned_transfers_from_an_earlier_boot_are_cleaned(self):
        current = pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        previous = '11111111-1111-1111-1111-111111111111'
        if previous == current:
            previous = '22222222-2222-2222-2222-222222222222'
        parent = self.root / 'AERA' / 'PCTransfers'
        parent.mkdir(parents=True, exist_ok=True)
        fixtures = []

        def transfer(char, boot=previous, marked=True):
            directory = parent / (char * 64)
            directory.mkdir(); fixtures.append(directory)
            (directory / 'payload.zip').write_bytes(b'simulated upload')
            if marked:
                (directory / '.transfer.json').write_text(json.dumps({
                    'schema': 1, 'owner': 'AERA-PC-TRANSFER', 'id': directory.name,
                    'boot_id': boot, 'payload': 'payload.zip'}))
            return directory

        try:
            stale = transfer('1')
            (stale / 'payload.zip.part').write_bytes(b'incomplete upload')
            same_boot = transfer('2', current)
            legacy = transfer('3', marked=False)
            unknown = transfer('4')
            (unknown / 'user-file.txt').write_text('must stay')
            linked = transfer('5')
            (linked / 'payload.zip').unlink()
            (linked / 'payload.zip').symlink_to(unknown / 'user-file.txt')
            invalid = transfer('6', 'not-a-boot-id')
            backup = parent / 'backup-fixture'
            backup.mkdir(); fixtures.append(backup)
            (backup / 'keep.txt').write_text('staged restore backup')
            self.command('busy'); self.command('cleanup-transfers')
            self.assertTrue(stale.exists())
            self.command('idle')
            status, job = self.prepare(b'active transfer')
            self.assertEqual(status, 200)
            self.assertTrue((parent / job['id'] / '.transfer.json').exists())
            self.command('cleanup-transfers')
            self.assertTrue(stale.exists())  # Never clean during any live transfer.
            self.assertEqual(self.request('/api/cancel', {'id': job['id']})[0], 200)
            self.assertFalse((parent / job['id']).exists())
            self.command('cleanup-transfers')
            self.assertFalse(stale.exists())
            for directory in (same_boot, legacy, unknown, linked, invalid, backup):
                self.assertTrue(directory.exists())
            self.assertEqual((unknown / 'user-file.txt').read_text(), 'must stay')
            self.command('cleanup-transfers')
            self.assertTrue(same_boot.exists())
        finally:
            self.command('idle')
            for directory in fixtures:
                if directory.exists():
                    shutil.rmtree(directory)

    def test_03_verified_install_survives_disconnect(self):
        data = b"simulated ZIP, not a flashable package" * 32768
        status, job = self.prepare(data)
        self.assertEqual(status, 200)
        status, verified = self.request("/api/upload/" + job["id"], data, "PUT")
        self.assertEqual((status, verified["phase"]), (200, "ready"))
        self.assertEqual(next(self.root.rglob("payload.zip")).read_bytes(), data)
        self.assertEqual(self.prepare(data)[0], 409)
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": False})[0], 409)
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 200)
        self.command("run")
        self.wait_phase("installing")
        self.command("wifi-on")
        self.command("wifi-off")
        self.assertEqual(self.request("/api/status")[1]["job"]["phase"], "installing")
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 409)
        self.command("presentation")
        presentation = self.request("/api/status")[1]["job"]["presentation"]
        self.assertEqual(presentation["package"], "Simulated payload ZIP")
        self.assertEqual((presentation["stage"], presentation["stage_count"]), (2, 5))
        self.command("prompt")
        self.assertEqual(self.request("/api/prompt", {"id": job["id"], "prompt": "wrong", "accepted": True})[0], 409)
        self.assertEqual(self.request("/api/prompt", {"id": job["id"], "prompt": "test-question", "accepted": True})[0], 200)
        # Stop the complete network service, not just a browser tab.
        self.command("off")
        time.sleep(3.1)
        self.assertFalse(list(self.root.rglob("payload*")))
        self.command("on")
        _, approved = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        self.assertEqual(approved["state"], "allowed")
        type(self).token = approved["token"]
        self.wait_phase("completed")
        self.assertFalse(list(self.root.rglob("payload*")))
        self.command("hold")

    def test_04_image_targets_and_storage_are_validated(self):
        data = b"image fixture" * 8192
        self.command("current-usb")
        try:
            info = self.request("/api/status")[1]
            self.assertEqual(info["transfer_storage"]["path"], str(self.root))
            self.assertTrue(any(item["path"] == str(self.root / "usb") for item in info["storages"]))
            self.assertEqual(next(item["name"] for item in info["storages"]
                                  if item["path"] == str(self.root / "usb")), "USB storage")
            self.assertNotIn("Current storage", [item["name"] for item in info["storages"]])
            self.assertEqual(self.prepare(data, storage=str(self.root / "usb"))[0], 409)
            self.command("lock")
            self.assertIsNone(self.request("/api/status")[1]["transfer_storage"])
            self.assertEqual(self.prepare(data)[0], 409)
        finally:
            self.command("unlock")
            self.command("current-internal")
        self.assertEqual(self.prepare(data, storage="/tmp")[0], 409)
        self.assertEqual(self.prepare(data, kind="img", partition="/bad", both_slots=False)[0], 400)
        self.assertEqual(self.prepare(data, kind="img", partition="/system", both_slots=True)[0], 400)
        status, job = self.prepare(data, name="boot.img", kind="img", partition="/boot", both_slots=True)
        self.assertEqual(status, 200)
        self.assertEqual(job["partition"], "/boot")
        self.assertTrue(job["both_slots"])
        self.assertEqual(self.request("/api/upload/" + job["id"], data, "PUT")[0], 200)
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        self.assertFalse(list(self.root.rglob("payload*")))

    def test_05_other_computer_cannot_install_or_discard(self):
        data = b"owned fixture"
        status, job = self.prepare(data)
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/upload/" + job["id"], data, "PUT")[0], 200)
        _, pending = self.request("/api/connect", {"key": "b" * 64, "name": "Second computer"})
        self.command("approve")
        _, approved = self.request("/api/connect/status", {"key": "b" * 64, "request": pending["request"]})
        second = approved["token"]
        self.assertFalse(self.request("/api/status", token=second)[1]["remembered"])
        for _ in range(12):
            status, resumed = self.request("/api/connect", {"key": "b" * 64, "name": "Second computer"}, token="")
            self.assertEqual((status, resumed["state"], resumed["token"]), (200, "allowed", second))
        self.assertEqual(self.request("/api/status")[0], 200)
        self.assertFalse(self.request("/api/status", token=second)[1]["job"]["owned"])
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True}, token=second)[0], 403)
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]}, token=second)[0], 403)
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        self.assertEqual(self.request("/api/forget", {}, token=second)[0], 200)
        self.assertEqual(self.request("/api/status", token=second)[0], 401)
        _, pending = self.request("/api/connect", {"key": "b" * 64, "name": "Second computer"}, token="")
        self.assertEqual(pending["state"], "pending")
        self.command("deny")

    def test_05a_file_browser_and_verified_file_save(self):
        folder = self.root / "browser-files"
        status, _ = self.request("/api/files/mkdir", {"path": str(self.root), "name": folder.name, "confirm": True})
        self.assertEqual(status, 200)
        self.assertEqual(stat.S_IMODE(folder.stat().st_mode), 0o775)
        self.assertEqual((folder.stat().st_uid, folder.stat().st_gid), (os.getuid(), os.getgid()))
        self.assertEqual(self.request("/api/files/list", {"path": "/proc"})[0], 409)
        self.assertEqual(self.request("/api/files/mkdir", {"path": str(folder), "name": "../escape", "confirm": True})[0], 409)
        data = b"plain file data, not an installer" * 4096
        status, job = self.prepare(data, kind="file", name="notes.txt", folder=str(folder))
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/upload/" + job["id"], data, "PUT")[0], 200)
        staged = self.root / "AERA" / "PCTransfers" / job["id"] / "payload.img"
        self.assertEqual(stat.S_IMODE(staged.stat().st_mode), 0o600)
        self.assertFalse((folder / "notes.txt").exists())
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[1]["phase"], "completed")
        self.assertEqual((folder / "notes.txt").read_bytes(), data)
        saved = (folder / "notes.txt").stat()
        self.assertEqual(stat.S_IMODE(saved.st_mode), 0o664)
        self.assertEqual((saved.st_uid, saved.st_gid), (os.getuid(), os.getgid()))
        status, empty = self.prepare(b"", kind="file", name="empty.txt", folder=str(folder))
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/upload/" + empty["id"], b"", "PUT")[0], 200)
        self.assertEqual(self.request("/api/install", {"id": empty["id"], "confirm": True})[1]["phase"], "completed")
        self.assertEqual((folder / "empty.txt").read_bytes(), b"")
        self.assertEqual(stat.S_IMODE((folder / "empty.txt").stat().st_mode), 0o664)
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder / "empty.txt"), "confirm": True})[0], 200)
        self.assertEqual(self.prepare(data, kind="file", name="notes.txt", folder=str(folder))[0], 409)
        status, listing = self.request("/api/files/list", {"path": str(folder)})
        self.assertEqual((status, listing["entries"][0]["name"]), (200, "notes.txt"))
        _, ticket = self.request("/api/files/download", {"path": str(folder / "notes.txt")})
        self.assertEqual(self.request(ticket["url"], token="", raw=True), (200, data))
        self.assertEqual(self.request(ticket["url"], token="")[0], 403)
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder), "confirm": True})[0], 409)
        self.command("busy")
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder / "notes.txt"), "confirm": True})[0], 409)
        self.assertEqual(self.request("/api/reboot", {"target": "system", "confirm": True})[0], 409)
        self.command("idle")
        self.assertEqual(self.request("/api/files/rename", {"path": str(folder / "notes.txt"), "name": "renamed.txt", "confirm": True})[0], 200)
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder / "renamed.txt"), "confirm": True})[0], 200)
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder), "confirm": True})[0], 200)

    def test_05b_certificate_is_public_stable_and_valid_for_ip(self):
        for path in ("/api/log/logcat", "/api/log/kernel"):
            self.assertEqual(self.request(path, token="")[0], 401)
            status, output = self.request(path, raw=True)
            self.assertEqual(status, 200)
            self.assertIn(b"Simulated diagnostic snapshot", output)
        _, hello = self.request("/api/hello", token="")
        status, certificate = self.request("/api/certificate", raw=True)
        self.assertEqual(status, 200)
        self.assertIn(b"BEGIN CERTIFICATE", certificate)
        self.assertNotIn(b"PRIVATE KEY", certificate)
        der = ssl.PEM_cert_to_DER_cert(certificate.decode())
        self.assertEqual(hashlib.sha256(der).hexdigest(), hello["certificate"])
        trusted = ssl.create_default_context(cadata=certificate.decode())
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=trusted, timeout=8)
        connection.request("GET", "/api/hello")
        self.assertEqual(connection.getresponse().status, 200)
        connection.close()
        self.command("restart")
        self.assertEqual(self.request("/api/hello", token="")[1]["certificate"], hello["certificate"])
        _, approved = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        type(self).token = approved["token"]

    def test_05bb_flash_reports_are_authenticated_bounded_downloads(self):
        folder = self.root / "AERA" / "logs" / "FlashLogs"
        folder.mkdir(parents=True, exist_ok=True)
        names = []
        for index, result in enumerate(("SUCCESS", "FAILED", "INCOMPLETE")):
            name = f"flash_20261015_10000{index}_ABCDEF_{result}.zip"
            names.append(name)
            with zipfile.ZipFile(folder / name, "w") as report:
                report.writestr("summary.txt", f"Result: {result}\nKeep current ABL: 1\n")
                report.writestr("logcat.txt", "diagnostic fixture\n")
            os.utime(folder / name, (100 + index, 100 + index))
        unrelated = folder / "keep.zip"
        unrelated.write_bytes(b"not an automatic report")
        self.assertEqual(self.request("/api/log/flashes", token="")[0], 401)
        self.assertEqual(self.request("/api/log/flash/" + names[-1], token="")[0], 401)
        status, result = self.request("/api/log/flashes")
        self.assertEqual(status, 200)
        self.assertEqual([entry["name"] for entry in result["reports"]], list(reversed(names[1:])))
        self.assertFalse((folder / names[0]).exists())
        self.assertTrue(unrelated.exists())
        status, content = self.request("/api/log/flash/" + names[-1], raw=True)
        self.assertEqual(status, 200)
        with zipfile.ZipFile(io.BytesIO(content)) as report:
            self.assertIn(b"Keep current ABL: 1", report.read("summary.txt"))
        self.assertEqual(self.request("/api/log/flash/keep.zip")[0], 404)
        self.assertNotEqual(self.request("/api/log/flash/../keep.zip")[0], 200)
        self.command("busy")
        self.assertEqual(self.request("/api/log/flashes")[0], 409)
        self.assertEqual(self.request("/api/log/flash/" + names[-1])[0], 409)
        self.command("idle")

    def test_05b_network_storage_metadata_and_queue(self):
        self.assertEqual(self.request("/api/nas", {"action": "save"}, token="")[0], 401)
        status, info = self.request("/api/status")
        self.assertEqual(status, 200)
        self.assertTrue(info["nas"]["supported"])
        self.assertTrue(info["nas"]["password_saved"])
        self.assertNotIn("never-return-this-secret", json.dumps(info))
        self.assertNotIn("password", info["nas"])
        self.assertNotIn("private_key", info["nas"])
        self.assertGreaterEqual(len(info["backup_storages"]), 2)
        for action in ({}, {"action": {}}, {"action": "invalid"}, {"action": "save", "host": {}}):
            self.assertEqual(self.request("/api/nas", action)[0], 400)
        status, job = self.request("/api/nas", {"action": "save", "type": "sftp", "host": "test-server",
                                              "password": "secret-test", "private_key": "private-test"})
        self.assertEqual(status, 200)
        self.assertNotIn("secret-test", json.dumps(job))
        self.assertNotIn("private-test", json.dumps(job))
        self.command("run")
        self.wait_phase("completed")
        self.command("hold")

    def test_05c_root_and_reboot_are_confirmed_queued_jobs(self):
        self.command("run")
        self.assertEqual(self.request("/api/root", {"action": "patch", "provider": "next", "slot": "b"})[0], 400)
        info = self.request("/api/status")[1]
        if not info["root_enabled"]:
            self.assertFalse(info["root_available"])
            status, error = self.request("/api/root", {"action": "inspect", "provider": "next", "slot": "b", "confirm": True})
            self.assertEqual(status, 409)
            self.assertIn("disabled", error["error"])
            return
        self.assertTrue(info["root_available"])
        self.assertEqual(self.request("/api/root", {"action": "patch", "provider": "next", "slot": "bad", "confirm": True})[0], 400)
        status, job = self.request("/api/root", {"action": "inspect", "provider": "next", "slot": "b", "confirm": True})
        self.assertEqual((status, job["root_action"], job["root_slot"]), (200, "inspect", "b"))
        result = self.wait_phase("completed")["root_inspection"]
        self.assertEqual((result["target"], result["kernel"]), ("init_boot_b", "android16-6.12"))
        self.assertTrue(result["inspected"] and result["patched"] and result["verified"])
        self.assertEqual((result["provider"], result["version"]), ("KernelSU Next", "v3.4.0"))
        self.assertTrue(result["offline_available"])
        self.assertEqual((result["offline_provider"], result["offline_version"]), ("next", "v3.3.0"))
        self.assertEqual(self.request("/api/reboot", {"target": "system"})[0], 400)
        self.assertEqual(self.request("/api/reboot", {"target": "bad", "confirm": True})[0], 400)
        self.assertEqual(self.request("/api/reboot", {"target": "recovery", "confirm": True})[0], 200)
        self.assertTrue(self.request("/api/status")[1]["busy"])
        time.sleep(1.1)
        self.command("reboot")
        self.assertFalse(self.request("/api/status")[1]["busy"])

    def test_05d_payload_review_and_arb_confirmation(self):
        self.command("hold")
        for mode in ("same", "upgrade", "downgrade", "malformed"):
            data = ("AERA-test-OTA\n" + mode + "\n").encode()
            status, job = self.prepare(data, name="Infinity-X-fixture.zip")
            self.assertEqual(status, 200)
            status, uploaded = self.request("/api/upload/" + job["id"], data, "PUT")
            self.assertEqual(status, 200)
            info = uploaded["package_info"]
            self.assertTrue(info["is_payload"])
            self.assertEqual(info["build"], "Infinity-X Test")
            self.assertEqual(info["android"], "Android 16")
            self.assertEqual(info["security_patch"], "2026-10-01")
            self.assertEqual([p["name"] for p in info["partitions"]], ["system", "vendor", "xbl_config"])
            body = {"id": job["id"], "confirm": True}
            if mode == "same":
                self.assertEqual(self.request("/api/install", body)[0], 200)
            else:
                self.assertEqual(self.request("/api/install", body)[0], 409)
                body["arb_acknowledged"] = True
                self.assertEqual(self.request("/api/install", body)[0], 200 if mode == "upgrade" else 409)
            self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)

    def test_05da_explicit_fast_flash_selection(self):
        self.command("hold")
        for mode in ("same", "protected", "upgrade", "incremental", "unsupported", "downgrade"):
            data = ("AERA-test-OTA\n" + mode + "\n").encode()
            status, prepared = self.prepare(data)
            self.assertEqual(status, 200)
            _, job = self.request("/api/upload/" + prepared["id"], data, "PUT")
            info = job["package_info"]
            self.assertEqual(info["fast_available"], mode not in ("incremental", "unsupported"))
            self.assertEqual(info["fast_slot"], "A")
            self.assertEqual(info["fast_protected"], ["abl", "recovery"] if mode == "protected" else [])
            self.assertEqual(job["install_method"], "normal")
            body = {"id": job["id"], "confirm": True, "install_method": "fast"}
            self.assertEqual(self.request("/api/install", body)[0], 409)
            body["fast_acknowledged"] = True
            status, result = self.request("/api/install", body)
            if mode == "upgrade":
                self.assertEqual(status, 409)
                body["arb_acknowledged"] = True
                status, result = self.request("/api/install", body)
            self.assertEqual(status, 200 if mode in ("same", "protected", "upgrade") else 409)
            if status == 200:
                self.assertEqual(result["install_method"], "fast")
                self.assertEqual(result["partition"], "")
                self.assertEqual(result["payload_partitions"], ["system", "vendor", "xbl_config"])
                self.assertNotIn("payload_progress", result)
                if mode == "same":
                    self.command("run")
                    live = self.wait_phase("installing")["payload_progress"]
                    self.assertTrue(live["available"])
                    self.assertEqual(live["lanes"], 2)
                    parts = live["partitions"]
                    self.assertEqual(parts[0], {"name": "system", "bytes": 200000000,
                                              "done": 100000000, "phase": "writing", "percent": 50})
                    self.assertEqual(parts[1]["phase"], "verifying")
                    self.assertEqual(parts[1]["percent"], 25)
                    self.assertEqual(parts[2]["percent"], 100)
                    self.assertEqual(parts[4]["phase"], "not_flashed")
                    self.assertEqual(parts[5]["phase"], "failed")
                    self.assertEqual(self.wait_phase("completed")["payload_progress"], live)
                    self.command("hold")
                    continue
            self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        status, prepared = self.prepare(b"not a payload")
        self.assertEqual(status, 200)
        self.request("/api/upload/" + prepared["id"], b"not a payload", "PUT")
        self.assertEqual(self.request("/api/install", {
            "id": prepared["id"], "confirm": True, "install_method": "fast", "fast_acknowledged": True})[0], 409)
        self.assertEqual(self.request("/api/install", {
            "id": prepared["id"], "confirm": True, "install_method": "unknown"})[0], 400)
        self.request("/api/cancel", {"id": prepared["id"]})

    def test_05db_advanced_selection_and_manual_protection_override(self):
        self.command("hold")
        for method in ("selected", "direct", "extract"):
            data = b"AERA-test-OTA\nprotected\n"
            _, prepared = self.prepare(data)
            _, job = self.request("/api/upload/" + prepared["id"], data, "PUT")
            abl = next(p for p in job["package_info"]["partitions"] if p["name"] == "abl")
            self.assertTrue(abl["protected"])
            self.assertTrue(abl["flash_available"])
            body = {"id": job["id"], "confirm": True, "install_method": method, "partitions": ["abl"]}
            if method != "extract":
                self.assertEqual(self.request("/api/install", body)[0], 409)
                body["fast_acknowledged"] = True
            status, result = self.request("/api/install", body)
            self.assertEqual(status, 200)
            self.assertEqual(result["install_method"], method)
            self.assertEqual(result["payload_partitions"], ["abl"])
            self.assertEqual(result["manual_protection_override"], method != "extract")
            self.request("/api/cancel", {"id": job["id"]})
        data = b"AERA-test-OTA\ndowngrade\n"
        _, prepared = self.prepare(data)
        _, job = self.request("/api/upload/" + prepared["id"], data, "PUT")
        self.assertEqual(self.request("/api/install", {
            "id": job["id"], "confirm": True, "install_method": "extract", "partitions": ["system"]})[0], 200)
        self.request("/api/cancel", {"id": job["id"]})
        data = b"AERA-test-OTA\nsame\n"
        package = self.root / "advanced-existing.zip"
        package.write_bytes(data)
        _, job = self.request("/api/files/prepare", {"path": str(package)})
        for selected in ([], ["../abl"], ["system", "system"]):
            self.assertNotEqual(self.request("/api/install", {"id": job["id"], "confirm": True,
                "install_method": "direct", "partitions": selected, "fast_acknowledged": True})[0], 200)
        status, result = self.request("/api/install", {"id": job["id"], "confirm": True,
            "install_method": "fast", "fast_acknowledged": True})
        self.assertEqual(status, 200)
        self.assertEqual(result["install_method"], "fast")
        self.assertFalse(result["manual_protection_override"])
        self.request("/api/cancel", {"id": job["id"]})
        self.assertEqual(package.read_bytes(), data)

    def test_05e_existing_packages_preserve_original_files(self):
        self.command("hold")
        package = self.root / "existing-ota.zip"
        original = b"AERA-test-OTA\nsame\n"
        package.write_bytes(original)
        self.assertEqual(self.request("/api/files/prepare", {"path": "/proc/version"})[0], 409)
        status, job = self.request("/api/files/prepare", {"path": str(package)})
        self.assertEqual((status, job["phase"], job["source_path"]), (200, "ready", str(package)))
        self.assertTrue(job["package_info"]["is_payload"])
        self.assertEqual(self.request("/api/upload/" + job["id"], original, "PUT")[0], 409)
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        self.assertEqual(package.read_bytes(), original)
        image = self.root / "modem.img"
        image.write_bytes(b"simulated firmware image")
        self.assertEqual(self.request("/api/files/prepare", {"path": str(image)})[0], 400)
        for partition, both_slots in (("/system", True), ("/block/splash", True)):
            self.assertEqual(self.request("/api/files/prepare", {"path": str(image), "partition": partition, "both_slots": both_slots})[0], 400)
        status, job = self.request("/api/files/prepare", {"path": str(image), "partition": "/block/modem", "both_slots": True})
        self.assertEqual((status, job["partition"], job["both_slots"]), (200, "/block/modem", True))
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        self.assertTrue(image.exists())
        _, job = self.request("/api/files/prepare", {"path": str(package)})
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 200)
        self.command("run")
        self.wait_phase("completed")
        self.assertEqual(package.read_bytes(), original)
        self.command("hold")
        _, job = self.request("/api/files/prepare", {"path": str(image), "partition": "/block/modem", "both_slots": False})
        image.write_bytes(b"changed!! firmware image")
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 200)
        self.command("run")
        self.wait_phase("failed")
        self.assertTrue(image.exists())
        self.assertTrue(package.exists())
        self.command("hold")

    def test_05f_backup_stream_uses_browser_blocks(self):
        body = dict(action="backup", kind="recovery", source="computer", method="stream",
                    name="PC-Test", partitions=["/data"], folder_ready=True, confirm=True)
        self.assertEqual(self.request("/api/backups", {**body, "confirm": False})[0], 400)
        self.assertEqual(self.request("/api/backups", {**body, "user": "wrong"})[0], 400)
        self.assertEqual(self.request("/api/backups", {**body, "folder_ready": False})[0], 400)
        self.assertEqual(self.request("/api/backups", {**body, "partitions": []})[0], 400)
        status, job = self.request("/api/backups", body)
        self.assertEqual((status, job["phase"], job["backup_action"]), (200, "queued", "backup"))
        self.command("run")
        self.wait_phase("installing")
        self.assertEqual(self.request("/api/forget", {})[0], 409)
        total = 0
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=8)
        transport = None
        def storage_request(path, body=None, raw=False):
            nonlocal transport
            headers = {"Authorization": "Bearer " + self.token}
            if body is not None:
                body = json.dumps(body).encode();headers["Content-Type"] = "application/json"
            connection.request("POST" if body is not None else "GET", path, body, headers)
            response = connection.getresponse();data = response.read()
            if response.status == 200:
                self.assertEqual(response.getheader("Connection"), "keep-alive")
                if transport is None:transport = connection.sock
                self.assertIs(connection.sock, transport)
            return response.status, data if raw else json.loads(data) if data else {}
        while True:
            status, request = storage_request("/api/backups/io")
            if status == 409:
                break
            self.assertEqual(status, 200)
            if not request:
                continue
            reply = {}
            if request["op"] == "stat":
                reply = dict(directory=True, size=0, mtime=0)
            elif request["op"] == "write":
                status, data = storage_request("/api/backups/io/" + str(request["id"]), raw=True)
                self.assertEqual(status, 200)
                self.assertEqual(data, bytes([ord("a") + total // 8388608]) * 8388608)
                total += len(data)
            self.assertEqual(storage_request("/api/backups/io/" + str(request["id"]), reply)[0], 200)
        connection.close()
        completed = self.wait_phase("completed")
        self.assertEqual(completed["backup_transfer"]["bytes"], 8 * 8388608)
        self.assertEqual(completed["backup_transfer"]["bytes_per_second"], 0)
        self.assertEqual(total, 8 * 8388608)
        self.assertEqual(completed["backup_result"]["folder"], "AERA/BACKUPS/test-device/PC-Test")
        status, job = self.request("/api/backups", {**body, "kind": "apps", "action": "apps", "folder_ready": False})
        self.assertEqual(status, 200)
        self.assertEqual(self.wait_phase("completed")["backup_result"]["apps"][0]["package"], "test.app")

    def test_05g_backup_and_restore_cancellation(self):
        body = dict(action="backup", kind="recovery", source="computer", method="stream",
                    name="Cancel-Test", partitions=["/data"], folder_ready=True, confirm=True)
        self.command("run")
        for restore in [False, True]:
            status, job = self.request("/api/backups", {**body, "action": "restore" if restore else "backup"})
            self.assertEqual(status, 200)
            self.wait_phase("installing")
            self.assertEqual(self.request("/api/backups/cancel", {"id": 123, "confirm": True})[0], 400)
            self.assertEqual(self.request("/api/backups/cancel", {"id": "stale", "confirm": True})[0], 403)
            if restore:
                self.assertEqual(self.request("/api/backups/cancel", {"id": job["id"], "confirm": True})[0], 409)
            status, cancelled = self.request("/api/backups/cancel", {
                "id": job["id"], "confirm": True, "acknowledge_partial_restore": restore})
            self.assertEqual(status, 200)
            final = self.wait_phase("cancelled")
            self.assertTrue(final["cancel_requested"])
            self.assertNotEqual(final["result"], 0)
            self.assertIn("partially restored" if restore else "unfinished", final["detail"])

    def test_05h_direct_link_downloads(self):
        fixture = b"PK\x03\x04" + b"AERA download fixture" * 4096

        class Downloads(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                if self.path == "/redirect":
                    self.send_response(302)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Location", "/download.php?file=test.zip&download=true")
                    self.end_headers()
                    return
                html = self.path == "/page"
                unknown = self.path == "/unknown"
                slow = self.path == "/slow"
                data = b"<html>Click Download</html>" if html else fixture
                self.send_response(200)
                self.send_header("Content-Type", "text/html" if html else "application/octet-stream")
                self.send_header("Content-Disposition", 'attachment; filename="Real release.zip"')
                if not unknown:
                    self.send_header("Content-Length", str(len(data) * (100 if slow else 1)))
                self.end_headers()
                try:
                    for _ in range(100 if slow else 1):
                        self.wfile.write(data)
                        self.wfile.flush()
                        if slow:
                            time.sleep(.05)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Downloads)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        base = f"http://127.0.0.1:{server.server_port}"
        request = {"url": base + "/download.php?file=test.zip&download=true", "name": "Download.zip",
                   "kind": "zip", "storage": str(self.root)}
        try:
            self.command("wifi-off")
            self.assertFalse(self.request("/api/status")[1]["url_download_available"])
            self.assertEqual(self.request("/api/download", request)[0], 409)
            self.command("wifi-on")
            self.assertTrue(self.request("/api/status")[1]["url_download_available"])
            self.assertEqual(self.request("/api/download", dict(request, url="file:///etc/passwd"))[0], 400)
            for path in ("/download.php?file=test.zip&download=true", "/redirect", "/unknown"):
                status, job = self.request("/api/download", dict(request, url=base + path))
                self.assertEqual(status, 200)
                for _ in range(100):
                    job = self.request("/api/status")[1]["job"]
                    if job["phase"] != "uploading":
                        break
                    time.sleep(.02)
                self.assertEqual(job["phase"], "ready", job)
                self.assertEqual(job["name"], "Real release.zip")
                self.assertEqual(job["bytes"], len(fixture))
                self.assertEqual(job["received"], len(fixture))
                directory = self.root / "AERA/PCTransfers" / job["id"]
                self.assertEqual((directory / "payload.zip").read_bytes(), fixture)
                self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
                self.assertFalse(directory.exists())
            for path in ("/page", "/slow"):
                status, job = self.request("/api/download", dict(request, url=base + path))
                self.assertEqual(status, 200)
                if path == "/slow":
                    time.sleep(.1)
                    self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
                for _ in range(100):
                    current = self.request("/api/status")[1]["job"]
                    if current["phase"] != "uploading":
                        break
                    time.sleep(.02)
                self.assertEqual(current["phase"], "cancelled" if path == "/slow" else "failed", current)
                self.assertFalse((self.root / "AERA/PCTransfers" / job["id"]).exists())
        finally:
            self.command("wifi-off")
            server.shutdown()
            server.server_close()
            worker.join()

    def test_06_remember_restart_and_forget(self):
        self.command("restart")
        self.assertEqual(self.request("/api/status")[0], 401)
        _, approved = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        self.assertEqual(approved["state"], "allowed")
        type(self).token = approved["token"]
        self.assertEqual(self.request("/api/forget", {})[0], 200)
        self.assertEqual(self.request("/api/status")[0], 401)
        _, pending = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        self.assertEqual(pending["state"], "pending")
        self.command("deny")
        self.assertEqual(self.request("/api/connect/status", {"key": self.key, "request": pending["request"]}, token="")[1]["state"], "denied")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
