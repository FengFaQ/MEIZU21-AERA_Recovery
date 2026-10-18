#!/usr/bin/env python3
"""Opt-in browser checks against the fake recovery service, never a device."""
import pathlib
import socket
import subprocess
import sys
import tempfile

from playwright.sync_api import sync_playwright
from pc_backup_browser_test import DIRECTORY


def main():
    with tempfile.TemporaryDirectory(prefix="pc-files-browser-", dir=pathlib.Path.home()) as root:
        folder = pathlib.Path(root)
        (folder / "one.txt").write_bytes(b"first file")
        (folder / "two.txt").write_bytes(b"second file")
        (folder / "empty-folder").mkdir()
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        process = subprocess.Popen([sys.argv[1], root, str(port), "--demo"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            assert process.stdout.readline().strip() == "READY"
            with sync_playwright() as p:
                browser = p.chromium.launch(executable_path="/opt/google/chrome/chrome",
                                            args=["--no-sandbox"], headless=True)
                page = browser.new_page(ignore_https_errors=True, bypass_csp=True,
                                        viewport={"width": 1280, "height": 900})
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.add_init_script(DIRECTORY + "\nwindow.testSlowClose=false;")
                page.goto(f"https://127.0.0.1:{port}")
                page.locator('#connectButton').click()
                page.locator('#workspace').wait_for(state="visible")
                process.stdin.write('busy\n'); process.stdin.flush()
                assert process.stdout.readline().strip() == 'OK busy'
                page.evaluate('async()=>await refresh()')
                page.locator('[data-tab="files"]').click()
                assert page.locator('#fileCount').inner_text() == 'Waiting for recovery'
                process.stdin.write('idle\n'); process.stdin.flush()
                assert process.stdout.readline().strip() == 'OK idle'
                page.get_by_role('checkbox', name='Select one.txt', exact=True).wait_for()
                page.get_by_role('checkbox', name='Select one.txt', exact=True).check()
                page.get_by_role('checkbox', name='Select two.txt', exact=True).check()
                assert page.locator('#selectedName').inner_text() == '2 selected'
                assert page.locator('#fileRename').is_hidden()
                assert page.locator('#fileInstall').is_hidden()
                for width in (1280, 390, 320):
                    page.set_viewport_size({"width": width, "height": 900})
                    assert not page.evaluate('document.documentElement.scrollWidth > innerWidth + 1')
                page.set_viewport_size({"width": 1280, "height": 900})
                page.screenshot(path='/tmp/aera-pc-files-selection.png', full_page=True)
                page.locator('#fileDownload').click()
                page.wait_for_function("document.querySelector('#notice').textContent.includes('2 / 2 files downloaded')")
                assert page.evaluate("testFolder.children.get('one.txt').data.length") == 10
                assert page.evaluate("testFolder.children.get('two.txt').data.length") == 11
                page.locator('#fileDownload').click()
                page.wait_for_function("document.querySelector('#notice').textContent.includes('Nothing was overwritten')")
                assert page.evaluate("testFolder.children.get('one.txt').data.length") == 10
                page.locator('#fileSelectAll').check()
                assert page.locator('#fileDownload').is_hidden()  # Folders are not downloaded as files.
                page.locator('#fileClearSelection').click()
                assert page.locator('#fileSelection').is_hidden()
                page.get_by_role('checkbox', name='Select one.txt', exact=True).check()
                page.get_by_role('checkbox', name='Select two.txt', exact=True).check()
                page.locator('#fileDelete').click()
                assert page.locator('#actionTitle').inner_text() == 'Delete 2 selected items?'
                assert (folder / 'one.txt').exists() and (folder / 'two.txt').exists()
                page.locator('#actionRun').click()
                page.wait_for_function("document.querySelector('#actionConfirm').open === false")
                assert not (folder / 'one.txt').exists() and not (folder / 'two.txt').exists()
                # Test normal Files save before throttling the upload for cancellation.
                page.locator('#storageFile').set_input_files({"name": "saved.txt", "mimeType": "text/plain", "buffer": b"saved upload"})
                page.wait_for_function("document.querySelector('#notice').textContent.includes('Saved to')")
                assert (folder / 'saved.txt').read_bytes() == b'saved upload'
                cdp = page.context.new_cdp_session(page)
                cdp.send('Network.enable')
                cdp.send('Network.emulateNetworkConditions', {"offline": False, "latency": 20,
                         "downloadThroughput": 1024 * 1024, "uploadThroughput": 512 * 1024})
                for native in (False, True):
                    page.locator('#storageFile').set_input_files({"name": "cancelled.txt", "mimeType": "text/plain",
                                                                "buffer": b'x' * (3 * 1024 * 1024)})
                    page.wait_for_function("state.receivingJob?.upload_transfer?.active", timeout=15000)
                    assert page.locator('#fileUploadCancel').is_visible()
                    if native:
                        process.stdin.write('cancel-upload\n'); process.stdin.flush()
                        while process.stdout.readline().strip() != 'OK cancel-upload':
                            assert process.poll() is None
                    else:
                        page.locator('#fileUploadCancel').click()
                    page.wait_for_function("!state.busy && state.job?.phase === 'cancelled'", timeout=15000)
                    assert not (folder / 'cancelled.txt').exists()
                    assert not list(folder.rglob('payload*'))
                cdp.send('Network.emulateNetworkConditions', {"offline": False, "latency": 0,
                         "downloadThroughput": -1, "uploadThroughput": -1})
                page.locator('[data-tab="install"]').click()
                page.locator('#fileInput').set_input_files({"name": "fixture.zip", "mimeType": "application/zip",
                                                          "buffer": b'fake installer' * 1024})
                page.locator('#transfer').click()
                page.locator('#confirm').wait_for(state='visible')
                page.locator('#confirmCancel').click()
                page.wait_for_function("document.querySelector('#confirm').open === false")
                assert page.evaluate("async()=> (await api('status')).job.phase") == 'cancelled'
                assert not list(folder.rglob('payload*'))
                assert not errors, errors
                browser.close()
                print('Browser: multi-selection, streamed batch downloads, confirmed deletion, Files save, both upload cancellations and ZIP cancellation passed.')
        finally:
            if process.poll() is None:
                process.stdin.write('quit\n'); process.stdin.flush()
                process.wait(timeout=10)
            process.stdin.close(); process.stdout.close()


if __name__ == '__main__':
    main()
