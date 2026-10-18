# AERA PC Connection

Menu > PC connection enables a browser workspace at `https://aera.local`.
The page also shows an IP address when multicast DNS is unavailable. Another
recovery using that hostname causes a device-specific `.local` name instead.
PC connection is off by default. Auto-enable is saved with the ordinary recovery
preferences and runs after unlock, even without Wi-Fi, not before decryption.

This feature does not require a PC application, wireless ADB or a new device-tree
flag. It uses the existing WLAN build option, OpenSSL and JSON dependencies.

## Backup And Restore

The **Backups** tab controls native recovery backups and the installed **App
Backup Vault**. Choose **This computer** or **Device storage** independently of
USB or Wi-Fi access. Partition selection, compression, app selection, decrypted
Android users and Vault snapshots use the existing recovery engines.

- **Computer backup:** choose a writable folder in a browser supporting the
  File System Access directory picker (desktop Chromium browsers). Recovery
  exposes that folder temporarily through FUSE at `/pc`. File operations cross
  the authenticated HTTPS connection in bounded blocks of at most 8 MiB.
  No complete backup or disk-sized staging file is kept on the phone or in RAM.
  Keep the browser open until the operation finishes. Browser disk permission
  and device approval are separate; disk-full errors fail the operation.
  Reloading the page interrupts the stream. Folder handles are remembered
  locally by the browser; access is reused when permission is still granted,
  otherwise **Use previous folder** requests access again. Browser profiles
  and website origins have separate permissions.
  Small writes are combined into bounded 8 MiB blocks per open file and
  flushed on synchronization or close. Browser disk writes overlap the next
  network block within one global 16 MiB queue. Small reads use 4 MiB read-ahead
  block, including the existing native checksum reader. Completion is never
  reported before all outstanding writes have been delivered.
  Large-file close/rename commits show **Saving file on computer** with an
  animated progress indicator, rather than treating transferred 100% as success.
  They may take up to ten minutes while the browser storage pump sends a
  heartbeat; ninety seconds without a heartbeat fails the operation. Ordinary
  filesystem requests retain their 45-second timeout. Closing or reloading the
  browser still interrupts the stream; cancellation remains available.
  Backup block requests reuse HTTPS connections instead of negotiating TLS
  again for each block; acknowledgements carry the next request, avoiding a
  separate polling round trip for every block. Every request is still authenticated. Automatic
  native backup names refresh between attempts, without reloading the page.
  Status polling and storage heartbeats also reuse HTTPS connections, so a
  control request does not close a pooled data socket. Browser filesystem
  failures retain their original exception name/message in the recovery log
  and final operation result instead of only reporting a generic I/O error.
- **Device backup:** writes to the selected mounted storage using the ordinary
  recovery location, without sending backup data to the computer. Device mode
  remains available in browsers without the directory picker.
- **Transfer speed:** the phone and browser show the same four-second smoothed
  PC backup/restore rate in decimal MB/s (1 MB = 1,000,000 bytes). The browser
  also shows transferred MB. Counters track successfully exchanged file data,
  including checksum reads; they are not a forecast of the final backup size.
- **Cancellation:** backups can be stopped from the browser or the phone.
  Restore cancellation requires two explicit warnings; the phone uses swipe
  confirmations. Cancellation stops the backup worker, not the recovery UI.
  Restore cancellation is not rollback: remain in recovery and complete a
  full restore before booting Android. Native unfinished backups are marked
  incomplete; App Vault keeps Restic's existing transactional snapshot format.
- Native backups retain `.win`/split archives, `.info` files and digests at
  `AERA/BACKUPS/<device>/<backup-name>/`. They can be copied from the computer
  to ordinary device storage and restored with the native recovery UI.
  Unfinished computer backups retain `.aera-incomplete` and cannot be selected
  for restore. Remove the failed backup and create a new one rather than
  removing the marker and treating incomplete files as a valid backup.
- App backups retain the existing encrypted Restic repository at
  `AERA/AppBackupVault`, including the recovery password and user-specific
  snapshot tags. Copy the complete repository, not individual pack files.
  Restore still targets the original decrypted Android user ID; this feature
  does not add cross-user remapping or a different Vault format. Vault must be
  installed on the phone; no plugin update is required for PC controls.
  Without the plugin, the **Apps** backup type is disabled. App names and icons
  come from the same APK metadata reader used on the phone. Icons are loaded
  once per discovery, separately from progress polling; unreadable icons use
  an initial instead. Restore lists use installed app metadata when available.
- For **computer restore**, choose the original destination folder, an
  `AERA/BACKUPS` folder, a device backup folder or a specific native backup.
  Multiple native backups are listed for explicit selection; unfinished
  folders marked `.aera-incomplete` are excluded. The chosen backup is still
  inspected by the recovery engine before restore. App
  restore accepts either the original destination folder or its nested
  `AERA/AppBackupVault` repository; the resolved folder is shown in the browser.
  **Advanced > Restore method** chooses direct streaming
  or **Transfer first, then restore**. The latter needs device storage large
  enough for the full source, uses a unique `AERA/PCTransfers/backup-*` directory,
  and disconnects the PC filesystem before restore. Native digests are checked
  before native restore writes when **Verify backup checksums** is enabled,
  and Vault uses Restic's normal integrity checks.
  Staging on storage erased by the selected restore is rejected.
- A streaming restore is different from staged ZIP installation: a connection
  failure can leave partially restored data. Recovery reports failure, not
  success; remain in recovery and retry a complete restore. A staged restore
  can continue without the browser once transfer finishes. Neither mode
  automatically switches slots or reboots after restoring.
- Actual backup/restore jobs show the operation/progress page on the phone;
  discovery and inspection stay headless. Other critical actions remain
  blocked until the job finishes.
  App operations show engine preparation, repository creation, backup reading
  and backup/restore phases, with processed bytes, file counts and the current
  file when Restic reports it. They do not use installation status labels.
- **Preferences > Generate backup checksums** controls native checksum
  generation on device and computer storage, including its early preference
  cache. **Verify backup checksums** controls the separate native restore
  verification step. Disabling it removes this pre-write corruption check;
  it does not disable other archive or filesystem errors. Vault still uses Restic's
  intrinsic integrity checks and unchanged encrypted repository format.

The `/pc` storage target exists only during its owning browser's job and is
removed afterward. Other browsers cannot service its I/O requests. Only one
backup or other critical recovery operation may run at a time. Computer-folder
contents are not exposed by the general device file browser.

Computer backups remain Beta. An intermittent browser file-commit error has
been observed at the end of large backups; its underlying cause is not yet
resolved. The original browser error is reported and the incomplete backup is
not treated as successful. Do not use an incomplete backup for restore.

### Development Checks

`aeraui/tests` includes a host HTTPS fixture and a bounded-block/ownership test.
The HTTPS test simulates the installer and never opens device partitions. The
optional `pc_backup_browser_test.py` uses Playwright, desktop Chrome and an
in-memory PC folder to exercise streaming write/readback and both storage modes.
`pc_storage_fuse_test.cpp` checks real host FUSE I/O, close, unlink and disconnect;
run it only as root in a private mount namespace, never on a phone. These tests
do not replace an actual native backup/restore and Vault test on the recovery.

## USB Access

Enable Menu > PC connection, connect the USB cable, then run:

```sh
adb -d forward tcp:8443 tcp:443
```

Open `https://localhost:8443` on the computer and approve the browser on the phone.
The command and URL are displayed in recovery. `adb -d` selects the USB device;
if several USB devices are connected, use `adb -s SERIAL forward ...` instead.
If port 8443 is already used, choose another local port and use it in the URL.
Remove the forwarding rule afterward with `adb -d forward --remove tcp:8443`.
USB access does not require Wi-Fi or changes to the computer's DNS/hosts file.
USB and Wi-Fi share the same approvals, API, files and jobs; a Wi-Fi disconnect
or IP change does not stop the USB service or cancel installation. Browser
approval is origin-specific, so the first USB visit can require separate approval
from a browser previously used through `aera.local`.

The shared listener accepts connections only through loopback and the currently
connected Wi-Fi address. Multicast DNS and HTTP-to-HTTPS redirection run only on
the network side. Sideload and fastbootd replace the USB ADB transport; forwarding
may need to be set up again after reconnecting to recovery.

## Connection And Installation

- HTTPS uses a private device authority generated on the phone. The first browser
  visit requires accepting the certificate warning. Its SHA-256 fingerprint is
  displayed in recovery. The Device tab exports only the public authority, not
  a private key. Certificate import is manual: browsers cannot install trusted
  certificates themselves. The device authority remains stable across service
  and recovery restarts; server certificates include the current Wi-Fi IP,
  `aera.local`, `localhost` and loopback addresses. Network changes replace only
  the server certificate, never the device authority or browser sessions.
  Without persistent writable data the authority is temporary. A data wipe
  requires approving a new authority.
- A new browser requests approval on the device. Remembering a browser stores
  its key fingerprint in `/data/misc/aera/pc-connection/computers.json`; no
  plaintext browser secret is stored on the phone. If writable persistent data
  is unavailable, approval is session-only.
  Reloading the page resumes an approved browser's current session without a
  second physical approval, even when Remember was not selected. Remember is
  needed only to retain approval after restarting the service or recovery.
- A complete ZIP or IMG is written to the selected mounted storage under
  `AERA/PCTransfers/<random-id>/`. It is not streamed into an installer or partition.
- Size and SHA-256 must match before the browser can confirm installation.
  Transfer & install (or Transfer & flash for an IMG) opens the review directly
  after verification. Flashing still requires confirmation; file-browser uploads
  remain save-only operations.
  Cancelling the installation review discards its transfer and clears the selected
  file. An existing on-device source file is preserved. Cancellation is not offered
  after installation has been submitted.
  Verified ZIPs are inspected with the same payload/OTA inspector as the native
  install review. Version, device, Android, patch level, payload layout and full
  partition details are available before confirmation. Firmware ARB downgrade
  blocks and explicit upgrade acknowledgement follow the native review rules.
  Normal installation uses the existing ZIP installer, including recovery/ABL
  preservation. Full standalone payloads additionally offer Fast flash
  (experimental), which uses the same otaripper backend as the native package
  review and requires a separate explicit acknowledgement. It writes to the
  reviewed current slot without changing slots, running postinstall or using
  normal OTA rollback. Runtime slot, manifest, image hash, target and snapshot
  checks remain in the common backend.
  With Keep current ABL or Keep AERA installed enabled, the corresponding images
  are excluded from payload flashing instead of backed up and rewritten. The
  native and browser Advanced selections keep these images unchecked by default;
  Select all does not include them. Explicit manual selection remains possible
  and the confirmation warns when protected images will be overwritten. Pure
  extraction and explicit manual IMG flashing are also allowed. Fast flash never
  overrides protection. The browser displays its
  selected method, target slot and protection settings before confirmation.
  Fast and direct payload flashes also show the native live partition progress:
  stable rows with individual percentages, sizes in MB, write/readback phases,
  verified counts, failures and targets that were not flashed. This view uses the
  common progress store and does not change the flash or verification procedure.
- The first device approval is physical. Subsequent installation confirmations
  and structured AERA installer questions are handled in the authorized browser.
  This does not automate arbitrary third-party installers' hardware-key prompts.
- A queued or running PC installation wakes the display and can replace the
  idle swipe lock. It never bypasses the Android data-decryption screen.
- Browser or Wi-Fi disconnection cannot cancel an already started installation.
  Reconnect to read its progress, log and result. Only pending transfers/jobs can
  be discarded; a running flash cannot be cancelled remotely.
- The browser mirrors structured package/device/author/stage information from
  the native installer when provided. Finished activity remains visible without
  a time limit until changing tabs; active jobs remain visible across tab changes.
- Temporary files are removed after normal completion, failure or cancellation.
  A sudden reboot or power loss can leave a transfer folder for manual removal.

AERA Remote retains the listener on HTTP port 80 when screen mirroring is running.
PC Connection remains on HTTPS port 443. With Remote enabled, ordinary HTTP visits
open mirroring exactly as before; `/pc` explicitly redirects to the PC workspace.
With Remote disabled, the PC listener redirects HTTP visits to HTTPS. Existing
mirroring links with `?code=...` and the explicit `/remote` entry remain supported.
When Remote stops, the PC redirect listener can reclaim port 80. Redirects keep a
recognized advertised hostname, rather than changing the browser's remembered origin.

## Additional Tools

- Root uses the existing KernelSU, KernelSU Next and SukiSU backend. Select A or B,
  inspect its kernel, optionally fetch a compatible release, patch, restore the
  existing rollback backup, or stage a manager APK. The browser path checks KMI
  from the target slot's unpacked/decompressed boot kernel, not the running recovery
  kernel. It refuses an unknown/unsupported target and uses exact matching modules.
  This allows manual inactive-slot patching after an OTA without switching slots
  or rebooting automatically. Root still requires writable unlocked internal storage.
  Devices without the init_boot/ksud backend show an unavailable state.
  The active slot is selected initially. Browser root jobs retain operation guards
  but do not replace the phone's current page with an operation/completion screen.
  Target inspections display the target, kernel, detected root provider/version
  and patch verification separately from the available offline module. Inspection
  results are attached to their target slot, not whichever slot is selected later.
- Confirmed power actions use the native UI-thread reboot path. Android, recovery,
  bootloader, fastbootd and power-off are blocked while a critical operation is active.
  The HTTP acknowledgement is sent before a reboot can be consumed by the UI.
- Files browse available mounted storage with directory-first ordering and paging.
  Server downloads stream with a fixed-size buffer and single-use tickets. The
  browser saves files up to 64 MiB via an authenticated fetch/Blob, avoiding silent
  native HTTPS-download failures. Larger files stream to a file picker in supported
  browsers, or use a normal browser download requiring a trusted certificate.
  Choosing Upload file immediately verifies and saves the file into the current
  folder without switching to Install; saving does not flash anything. Rename and
  save never overwrite another file. Delete handles files and empty folders only.
  ZIP/IMG files already on mounted storage can be selected for installation or
  image flashing without uploading/copying them. They use the same review and
  confirmation; image target/slot rules still apply. Original files are never
  included in transfer cleanup. File identity/size/timestamps are checked again
  before execution to reject a changed selection.
  Delete does not recursively remove directories or storage roots. The private key
  directory is excluded even if a storage root would contain it.
- Device information includes slots, kernel, memory, battery, uptime and connection.
  Recovery-log, Logcat and kernel-log downloads are authorized snapshots. Logcat
  includes the last 4,000 records across available buffers. Diagnostic command
  capture has a ten-second timeout and four-MiB ceiling; no continuous logging
  daemon is added. Missing log commands return a visible error.

## Protocol

Image targets include native flashable/logical volumes and discovered physical
firmware block devices under by-name. Physical A/B pairs are grouped and use the
active slot unless both slots are explicitly selected. Native raw/sparse image
writers enforce target capacity. Super/COW/mapper devices and personal-data or
calibration partitions are not exposed through the generic firmware fallback.

Schema 1 endpoints use JSON except the raw file upload:

| Endpoint | Purpose |
| --- | --- |
| `GET /api/hello` | Device and protocol information |
| `POST /api/connect` | Request approval or resume a remembered browser |
| `POST /api/connect/status` | Poll the same approval request |
| `GET /api/status` | Storage, image targets, job progress, log and installer question |
| `POST /api/prepare` | Declare filename, byte count, SHA-256, storage and optional image target |
| `PUT /api/upload/<id>` | Transfer exactly the declared number of bytes |
| `POST /api/install` | Explicit confirmation of a verified file |
| `POST /api/cancel` | Discard a transfer or queued installation |
| `POST /api/prompt` | Answer the current structured installer question |
| `POST /api/forget` | Revoke the current browser's saved approval and sessions |
| `POST /api/root` | Queue a confirmed root action with explicit provider and slot |
| `POST /api/reboot` | Queue a confirmed native reboot/power action |
| `POST /api/files/list` | List a mounted-storage directory with paging |
| `POST /api/files/prepare` | Review an existing ZIP or IMG without copying it |
| `POST /api/files/download` | Create a short-lived single-use download ticket |
| `GET /api/files/content/<ticket>` | Stream the authorized regular file |
| `POST /api/files/mkdir`, `rename`, `delete` | Explicit non-recursive file changes |
| `GET /api/certificate` | Export the public device authority |
| `GET /api/log` | Recovery log snapshot |
| `GET /api/log/logcat`, `GET /api/log/kernel` | Bounded diagnostic command snapshot |

Approved endpoints require a bearer session token. A different approved browser
may observe the current job, but cannot install, discard or answer its prompts.
Network handlers never call the recovery partition manager or LVGL. The UI
thread snapshots available targets and starts confirmed jobs only when its
existing critical-operation guards allow it.
Root operations share a serialization lock with native Root Manager actions;
new PC jobs also wait while the native Root Manager page is open. The file tool
does not expose shell execution or block-device writes.

`POST /api/install` accepts `install_method`: `normal` (default), `fast`,
`selected`, `direct` or `extract`. Advanced methods take an explicit `partitions`
array of manifest names. Flash methods require `fast_acknowledged`; firmware
upgrades still require `arb_acknowledged`. Extraction does not write partitions
and therefore does not require firmware ARB acknowledgement. The same verified
manifest, selected-image and current-slot backend is used on the phone and PC.

## Dependencies And Tests

- `third_party/mdns/mdns.h`: mjansson/mdns, commit
  `a569c4759bd47e0f2a7bfc4d4c19620445782806`, Unlicense.
- `client/pc/sha256.js`: hash-wasm 4.12.0 SHA-256 build, MIT.
- Client icons: Lucide, ISC. License files are included with the client.

The `pc_connection_host` test target uses the real HTTPS/upload service with a
fake installer and loopback-only binding. It never accesses device partitions.
`pc_connection_test.py` covers physical approval, ownership, interrupted and
invalid uploads, image target restrictions, disconnect during installation,
remembered browser access and revocation. `ui_check --pc-settings` checks native
settings layout in six languages, four UI sizes and both orientations.

The test fixture's `--demo` flag automatically approves browsers for UI testing
only. It is not compiled into recovery and is not a production protocol option.
