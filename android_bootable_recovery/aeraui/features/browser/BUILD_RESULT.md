# Verified local development build — 2026-09-06

Product: `twrp_dodge-ap2a-eng`  
Command: `mka adbd recoveryimage`  
Result: successful (01:32 final rebuild). No device files were changed/flashed.

`out/target/product/dodge/recovery.img`:

- Padded partition size: 104857600 bytes (100 MiB)
- Original image: 97759232 bytes
- Maximum original image with AVB overhead: 104787968 bytes
- Remaining: 7028736 bytes (6.703 MiB)
- LZ4 ramdisk: 97751474 bytes; kernel: 0 bytes, header version 4
- SHA-256: `5345a8e2e8aa7528e3a3a2943aacd5977a9e92a5784301cc3cbabbafc7202c01`

Compressed browser payload: 32466816 bytes (30.963 MiB), expanded
161589660 bytes, 403 members. SHA-256:
`c64e2be0ef0285ab85003365ec39476d242715f56e8344a21aaee403a1b4a2d7`.

The image's embedded ramdisk matches `ramdisk-recovery.img`. A streaming CPIO
inspection verified the payload's bytes/hash inside that ramdisk. AVB footer
and partition hash verification passed (existing algorithm NONE, not a claim
of signature/authenticity). The AERA-named image equals `recovery.img`.

Host UI regression tests passed, including ASan/UBSan-instrumented native C++
scene code. Browser parser/protocol/session tests passed with ASan/UBSan:
real payload extraction, compressed and expanded hashes, corruption/truncation,
cancellation, paths, musl alias identity, sealed pixel buffers, copied frames,
ACK/input handling, malformed frames, synthetic LVGL viewport and disconnect
return. The ARM64 software worker compiles and its dependencies are in the
payload. These are **not** on-device browser rendering or performance tests.

Browsing remains unavailable. A compatible, audited isolated launcher and
end-to-end device validation are required. Wi-Fi was intentionally untouched.
No commit, push or public release was made. See README.md for distribution gates.
