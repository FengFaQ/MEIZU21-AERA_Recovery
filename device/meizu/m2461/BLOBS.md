# M2461 (Meizu 21) - ported blobs manifest

Machine-readable manifest of every prebuilt blob copied into this device tree,
ported from the **verified, flash-tested TWRP device tree**.

| Field | Value |
|---|---|
| Origin (all blobs + kernel) | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| Blob staging source | `work/tr/blob_twrp/recovery_root` (staged copy of that tree's `recovery/root`) |
| Kernel source | `work/tr/twrp/prebuilt/kernel` |
| Copy method | `cp -a` / `cp -an`, permission bits preserved, line endings untouched, binaries never edited |
| Integrity check | every listed file verified byte-identical to its source with `cmp` |
| Permission model | 14 files `-rwxr-xr-x`, 147 files `-rw-r--r--`; no `0600` outliers |

## Summary

| Item | Value |
|---|---|
| Blob files under `recovery/root/` | **161** |
| Bytes under `recovery/root/` | **18073132** (~17.24 MiB) |
| Kernel files | 1 |
| Kernel bytes | **33892864** |
| **Total files** | **162** |
| **Total bytes** | **51965996** (~49.56 MiB) |
| **On-disk footprint of the whole device tree** (all files incl. this manifest) | **52012870** (~49.60 MiB) |

Blobs by destination group:

| Destination group | Files |
|---|---|
| `recovery/root/vendor/bin` (incl. `bin/hw`) | 12 (hw: 11) |
| `recovery/root/vendor/lib64` | 105 |
| `recovery/root/vendor/etc` (vintf manifest, ueventd.rc, configs) | 14 |
| `recovery/root/vendor/firmware` (haptic + goodix bins) | 5 |
| `recovery/root/vendor/firmware_mnt/image` (Meizu TA signed segments) | 11 |
| `recovery/root/vendor/odm/etc` (richtap + ueventd) | 2 |
| `recovery/root/system/bin` | 2 |
| `recovery/root/system/etc` | 6 |
| `recovery/root/system/lib64` | 4 |

## Kernel

| Field | Value |
|---|---|
| Source | `work/tr/twrp/prebuilt/kernel` |
| Destination | `prebuilt/kernel` |
| Size | **33892864** bytes (exactly the documented 33,892,864) |
| SHA-256 | `9d8eef56d8d64d2512d0cc76a2f933a0c601fb5435a05ba955b734ea3cc3ee13` |
| Magic | `4d 5a` = `MZ` (Meizu EFI-stub arm64 `Image`) |
| Origin | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |

## Layout mapping applied

This is the **flat AERA reference layout** (same as
`android_device_oneplus_infiniti-AERA`): the vendor ramdisk keeps a real
`vendor/` directory inside `recovery/root`, with the same depth as the
running system, so `DT_NEEDED` entries and absolute `/vendor/...` paths inside
the prebuilt binaries stay valid.

| Source (TWRP `recovery/root`) | Destination (this tree) |
|---|---|
| `vendor/*` | `recovery/root/vendor/*` |
| `vendor/odm/*` | `recovery/root/vendor/odm/*` |
| `vendor/firmware/*` | `recovery/root/vendor/firmware/*` |
| `vendor/firmware_mnt/*` | `recovery/root/vendor/firmware_mnt/*` |
| `system/bin/*` | `recovery/root/system/bin/*` |
| `system/etc/*` (incl. `vintf`) | `recovery/root/system/etc/*` |
| `system/lib64/*` | `recovery/root/system/lib64/*` |

In other words every copied path is unchanged; only the four paths listed below
are withheld because another task authors them natively.

## Deliberately NOT copied (owned by task-2)

| Path | Reason |
|---|---|
| `init.recovery.qcom.rc` | recovery init file, authored natively for AERA |
| `init.recovery.usb.rc` | recovery USB init file, authored natively for AERA |
| `vendor/bin/start_crypto_services.sh` | crypto bootstrap script, rewritten for AERA |
| `vendor/etc/init/*.rc` (9 files) | HAL init files, rewritten for AERA's service model |

Do not re-add them blindly: they reference TWRP-only properties and services.
If they already exist when the port is re-run, they are protected by
`cp --no-clobber` so the natively authored versions win.

## Executable bits

`chmod 0755` applied to every ELF binary under `vendor/bin`, `vendor/bin/hw`
and `system/bin`, and to every `*.sh` in the tree:

| Binary | Mode |
|---|---|
| `busybox` | `-rwxr-xr-x` |
| `hw/android.hardware.boot-service.qti` | `-rwxr-xr-x` |
| `hw/android.hardware.fastboot-service.example_recovery` | `-rwxr-xr-x` |
| `hw/android.hardware.gatekeeper-service-qti` | `-rwxr-xr-x` |
| `hw/android.hardware.gatekeeper-service-spu-qti` | `-rwxr-xr-x` |
| `hw/android.hardware.health-service.qti_recovery` | `-rwxr-xr-x` |
| `hw/android.hardware.secure_element-service.qti` | `-rwxr-xr-x` |
| `hw/android.hardware.security.keymint-service-qti` | `-rwxr-xr-x` |
| `hw/android.hardware.security.keymint-service-spu-qti` | `-rwxr-xr-x` |
| `hw/android.hardware.weaver-service-spu-qti` | `-rwxr-xr-x` |
| `hw/vendor.qti.hardware.qseecom@1.0-service` | `-rwxr-xr-x` |
| `hw/vendor.qti.hardware.vibrator.service` | `-rwxr-xr-x` |
| `lshal` | `-rwxr-xr-x` |
| `qseecomd` | `-rwxr-xr-x` |

All `.so` files under `vendor/lib64` and `system/lib64` stay `0644`, as in the
verified TWRP tree.

## Licensing

These blobs are **extracted from the stock Meizu Flyme firmware for the Meizu 21
(M2461)** and are shipped here only so that a recovery image built for this same
device can drive its own hardware. They are proprietary, device-specific vendor
firmware and vendor userspace; they are **not relicensable**, no open-source
license applies to them, and AERA's license does not extend to them. No license
is granted or implied beyond running them on the matching hardware. Do not
redistribute them outside a build targeting this exact device.

---

## File manifest

`source -> destination | size (bytes) | sha256 | origin`

| Source | Destination | Size | SHA-256 | Origin |
|---|---|---|---|---|
| `work/tr/twrp/prebuilt/kernel` | `prebuilt/kernel` | 33892864 | `9d8eef56d8d64d2512d0cc76a2f933a0c601fb5435a05ba955b734ea3cc3ee13` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/bin/busybox` | `recovery/root/system/bin/busybox` | 2149248 | `1d762e89641c991aecc60bbef37c2e97542ba1f7f693f2c90b405de1d17ea7c8` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/bin/lshal` | `recovery/root/system/bin/lshal` | 204240 | `5528a93d0023be05ac765ce0da27ae1b2f3aa2097819e606a86a0b330e9d6e4f` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/etc/event-log-tags` | `recovery/root/system/etc/event-log-tags` | 37818 | `88d462cde533ed125a941473303b58cf86d3ffbbb4afbd8a67df7e9c401e1ac2` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/etc/task_profiles.json` | `recovery/root/system/etc/task_profiles.json` | 16625 | `d813bbb1d1bd00adfb7f14c01c2da4857e29df885fd9b9d5e58bfac204479271` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/etc/twrp.flags` | `recovery/root/system/etc/twrp.flags` | 4439 | `1b73a5937d8158d0414f446f90fc0915d49be97a5a0db043229422b99d2bd8b6` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/etc/ueventd.rc` | `recovery/root/system/etc/ueventd.rc` | 3164 | `448643d97931883b908e8989aed594c9d31bcb8295da39c954ca3879b26ca5e5` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/etc/vintf/compatibility_matrix.device.xml` | `recovery/root/system/etc/vintf/compatibility_matrix.device.xml` | 52824 | `397cebda8a9bdd9690dd2fa6ec12959c8e22811be237f3318a2a8335c8eed57c` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/etc/vintf/manifest.xml` | `recovery/root/system/etc/vintf/manifest.xml` | 2368 | `a14473ae84d4eb310970f237d42900836be90b15bb3bd9538d98cc1229f4c392` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/lib64/android.hardware.common-V2-ndk.so` | `recovery/root/system/lib64/android.hardware.common-V2-ndk.so` | 24168 | `74be6aab35932dab83036c70d317f88edd9c7bc56acc79086dfeb8d0943493a5` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/lib64/android.hardware.common-V2-ndk_platform.so` | `recovery/root/system/lib64/android.hardware.common-V2-ndk_platform.so` | 24168 | `e63d9137b7357aa8f33da84e8394c888ba89e57f511dd9aedd1d9f4c8482fa64` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/lib64/android.hardware.vibrator-V2-ndk.so` | `recovery/root/system/lib64/android.hardware.vibrator-V2-ndk.so` | 131888 | `28c3f0a0d4581949201a7924461b75da7170f0d21d7337d8e081c769e228946f` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `system/lib64/libexpat.so` | `recovery/root/system/lib64/libexpat.so` | 167768 | `e24ccc9e35b97136f38c2f642730a0321fa1fd55076bf8f4ad72ed6327a1ccd5` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.boot-service.qti` | `recovery/root/vendor/bin/hw/android.hardware.boot-service.qti` | 33856 | `6f0c4657dd6116f3c94ff7ab8cfda3da0fadf51521c2ea11eca5be1359be1779` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.fastboot-service.example_recovery` | `recovery/root/vendor/bin/hw/android.hardware.fastboot-service.example_recovery` | 29576 | `2af951592ea874343be1c06d55d7ae9b67f54aac3758875401f5b0a15b094b5c` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.gatekeeper-service-qti` | `recovery/root/vendor/bin/hw/android.hardware.gatekeeper-service-qti` | 15056 | `61b2d6643ede5a859d6798ffd344b1f1ee8314757f113cd518a7bd550383b6af` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.gatekeeper-service-spu-qti` | `recovery/root/vendor/bin/hw/android.hardware.gatekeeper-service-spu-qti` | 15056 | `aa8408bc3522dfe26e5b4942638ae8aa30e0437712106d80e340517fde8d3ef2` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.health-service.qti_recovery` | `recovery/root/vendor/bin/hw/android.hardware.health-service.qti_recovery` | 112096 | `e88dbb74b720bdcb18da4649b3447ed96fb13e8ccf0c9587ffe143de0a9162f1` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.secure_element-service.qti` | `recovery/root/vendor/bin/hw/android.hardware.secure_element-service.qti` | 62432 | `950c430c7710658b1535c11ea52f98e9575ac093123bf09ecea823b7c6876288` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.security.keymint-service-qti` | `recovery/root/vendor/bin/hw/android.hardware.security.keymint-service-qti` | 45312 | `28435ee7b70c30db6b6bcc6c2726ee8430ffd09d05389bf027e061150976f8f1` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.security.keymint-service-spu-qti` | `recovery/root/vendor/bin/hw/android.hardware.security.keymint-service-spu-qti` | 37072 | `35073bfb5f5318c45b0137afd46d23d023934d99340f703c8e83d5cd030e2803` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/android.hardware.weaver-service-spu-qti` | `recovery/root/vendor/bin/hw/android.hardware.weaver-service-spu-qti` | 45616 | `2efb6503d2eb593adbe760716e6757581a19c1e46bc76a4caf1d0dfcd43348b1` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/vendor.qti.hardware.qseecom@1.0-service` | `recovery/root/vendor/bin/hw/vendor.qti.hardware.qseecom@1.0-service` | 36808 | `8fffbbe2784b582868e7a70fd75b2984064cd52fc3ce5ce5b05fd4c27eb6ad01` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/hw/vendor.qti.hardware.vibrator.service` | `recovery/root/vendor/bin/hw/vendor.qti.hardware.vibrator.service` | 37936 | `f5aeabc63d8ff36b350acf9d080f0b1bd8c9ccaf059984f0e8ff6db0fee6dbf8` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/bin/qseecomd` | `recovery/root/vendor/bin/qseecomd` | 45768 | `29b816bcb022331307201221eb2136edbc9ec3f17884df1f34a1879fb981bd2a` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/gpfspath_oem_config.xml` | `recovery/root/vendor/etc/gpfspath_oem_config.xml` | 2129 | `f33b193963bbbee24df9de611384139959aa3080f956b6e908abab4cee616f07` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/hal_uuid_map_config.xml` | `recovery/root/vendor/etc/hal_uuid_map_config.xml` | 2668 | `694c6dc8e57775aba1d853884316c999b0b75b276ed267b1323c3148e1ff207e` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/ueventd.rc` | `recovery/root/vendor/etc/ueventd.rc` | 33723 | `8046cac9dca49bed1f37035c4bc5038bef3608cbe34aeda7cdee719db1eca24b` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/compatibility_matrix.xml` | `recovery/root/vendor/etc/vintf/compatibility_matrix.xml` | 2071 | `e1194ac9b5fc67d4144e2faa4bc98d5b425b10b46e3996d2bf3a22afa45039ef` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest.xml` | `recovery/root/vendor/etc/vintf/manifest.xml` | 4661 | `bca2bcd6b19bcee6cc84b7a4bcbf1f44180df28d467ae2fa295f3838c7da8778` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/android.hardware.fastboot-service.example.xml` | `recovery/root/vendor/etc/vintf/manifest/android.hardware.fastboot-service.example.xml` | 206 | `a1a662b3b8a52785a10691b68f53efda98086be93e0141af6999b69d76a24fc1` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/android.hardware.health-service.qti.xml` | `recovery/root/vendor/etc/vintf/manifest/android.hardware.health-service.qti.xml` | 337 | `a4715413c4c5a245e485c4149a4bef0ab34a3a45f5fe1ce6ee81b3c3437ec17f` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/android.hardware.secure_element.xml` | `recovery/root/vendor/etc/vintf/manifest/android.hardware.secure_element.xml` | 409 | `9c5c4a158b619cf6b753deece237d3828838defc0c3edee2e0f716fe3363f787` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/android.hardware.security.keymint-service-qti.xml` | `recovery/root/vendor/etc/vintf/manifest/android.hardware.security.keymint-service-qti.xml` | 1971 | `f8e9f5f0ef1dd420ce4064af5d4449a0e5a25a61838766e3f60ab894b042f846` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/android.hardware.security.keymint-service-spu-qti.xml` | `recovery/root/vendor/etc/vintf/manifest/android.hardware.security.keymint-service-spu-qti.xml` | 2017 | `8927e0bbb1ce2a504a1affb0e04d9d5ef4e79bfb086b775c9c65bfa71c960cc8` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/boot-service.qti.xml` | `recovery/root/vendor/etc/vintf/manifest/boot-service.qti.xml` | 301 | `9c51ca5638efe36fab9d4e02f929fd7badc6d6ba265f251a7d0af2889e033290` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/secure_element-service.xml` | `recovery/root/vendor/etc/vintf/manifest/secure_element-service.xml` | 278 | `67f8e51a11b54cd74b0f6dca1296ae36078053a5e332de22f55a39a64fbcac10` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/vendor.qti.hardware.qseecom@1.0-service.xml` | `recovery/root/vendor/etc/vintf/manifest/vendor.qti.hardware.qseecom@1.0-service.xml` | 565 | `eac6f47975b0225784e5da37716d9ceb03cf8fcabd01e411851cac645c3c6d34` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/etc/vintf/manifest/vendor.qti.hardware.vibrator.service.xml` | `recovery/root/vendor/etc/vintf/manifest/vendor.qti.hardware.vibrator.service.xml` | 1728 | `ba06b2192b9c2b28a7f12e1caf41ba48beb6be2e5e6c8edfbe54101351ec4e86` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware/100_Haptic.bin` | `recovery/root/vendor/firmware/100_Haptic.bin` | 5941 | `a24a71ca7976194427fdf9aa81896072cea9461be1f52c8ca34c79296133df72` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware/101_Haptic.bin` | `recovery/root/vendor/firmware/101_Haptic.bin` | 121 | `02e64290c9c7ad7194f28044191219d17072f7ec22201489932461d273174937` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware/goodix_cfg_group.bin` | `recovery/root/vendor/firmware/goodix_cfg_group.bin` | 1879 | `4d63b8786fd5193941d1d4ad63e5fbb58cdd87b27d90136c24b5753579242b3a` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware/goodix_firmware.bin` | `recovery/root/vendor/firmware/goodix_firmware.bin` | 223744 | `94d1366fa6cd266435068a8e46a5f66db3e692b7c7ba4ba02a6f06b4ac68133c` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware/haptic_config.bin` | `recovery/root/vendor/firmware/haptic_config.bin` | 414 | `7cc9d827002e25d77fc111e499a8cfaf24ac25c7ab94c59e0cddd537eaf855f8` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b00` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b00` | 568 | `de548e7fb1fbadab99dc188d5714ef38db26ce4c667dbc57461724ff0e42f2ad` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b01` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b01` | 48334 | `9fdf25f495144a7a6cbb5ba54c314d03173dc8349525d65ad9f8d912cc05e132` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b02` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b02` | 257 | `a7b44936a6293b01228ae3db6a551d785437bc2da9748932515d6162fc657e64` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b03` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b03` | 728 | `a88c941bb068f5d2ca957cfa50cca85ce1b1b3282f67bc39f84bee42912c3a62` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b04` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b04` | 3792 | `c6a822638e49a51058a464bfd739d99ab8b5e6652d1c6b00a0fd2cf5cbbaa34a` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b05` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b05` | 304 | `feaba5bbca184b3fc2c5e4c79dd2b2aa582be1efce6887a9397f933ae3768a6f` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b06` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b06` | 8665 | `9cc40c8117ed3ee0b7d4f98acd3d84bbb8863a86783562ce0ceec0fbdc7b5c92` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b07` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b07` | 728 | `a88c941bb068f5d2ca957cfa50cca85ce1b1b3282f67bc39f84bee42912c3a62` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b08` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.b08` | 4184 | `ee5d968d6dc6e41124927f852c2a50c9b5834e78ab6afbb9b2c9affc813272d6` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.mdt` | `recovery/root/vendor/firmware_mnt/image/05B04A44-BF30-42DF-9E2F-B366B980ED19.mdt` | 4752 | `afcb9b1aec0781a3278434ed8ac3bf1cf8721798bad396719a3a6777df16f967` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/firmware_mnt/image/FD719D50-FFFB-11EB-9A03-0242AC130003.b01` | `recovery/root/vendor/firmware_mnt/image/FD719D50-FFFB-11EB-9A03-0242AC130003.b01` | 56219 | `a4f4339d19c2e3f39f8a0e4bff0fe44d74e0d97238685e896a3169f59e1d071d` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.frameworks.sensorservice-V1-ndk.so` | `recovery/root/vendor/lib64/android.frameworks.sensorservice-V1-ndk.so` | 89776 | `3733243dd88a003a5377c26ab5740c4c99eea84f49ebcf4e856fb04a0de3a090` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.frameworks.sensorservice@1.0.so` | `recovery/root/vendor/lib64/android.frameworks.sensorservice@1.0.so` | 228680 | `3d6ca3e1b76bb084e3746c5307c761ae20277a2b911c3bb06d8668c40c4323fc` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.boot-V1-ndk.so` | `recovery/root/vendor/lib64/android.hardware.boot-V1-ndk.so` | 54760 | `224b1caadc4562d96d527e39425c6622def395518e24d5a24d92195f0fb3e7b2` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.boot@1.0.so` | `recovery/root/vendor/lib64/android.hardware.boot@1.0.so` | 136032 | `3f8acd47faa3f32753c07b41ecc5c020cc6122621905ce0eb127d76951d6b3f5` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.boot@1.1.so` | `recovery/root/vendor/lib64/android.hardware.boot@1.1.so` | 127232 | `c163b382c831aadd45ea0bb093b1f85a877c675751adb3dd4172b24174a0a612` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.common.fmq-V1-ndk.so` | `recovery/root/vendor/lib64/android.hardware.common.fmq-V1-ndk.so` | 15448 | `92bc9a224e3ec52745e51424eb68eb562541380795d0f96c05e3784855a56a30` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.gatekeeper-V1-ndk.so` | `recovery/root/vendor/lib64/android.hardware.gatekeeper-V1-ndk.so` | 50720 | `663129849c8bdfdd14f7482b1a9b32364280d50856b05477ac1008dd350d7d2c` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.gatekeeper@1.0.so` | `recovery/root/vendor/lib64/android.hardware.gatekeeper@1.0.so` | 123424 | `7aa333fe6e9ed6d94f2a1ef41bd8715f6abaf8376904de7f84abcf9fc9e8b5de` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.health-V2-ndk.so` | `recovery/root/vendor/lib64/android.hardware.health-V2-ndk.so` | 97424 | `6da09b150bc4699c598eabf059250cecbeb9b9a59a0eb1079b5285be47fc2779` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.keymaster@3.0.so` | `recovery/root/vendor/lib64/android.hardware.keymaster@3.0.so` | 186440 | `aba51a4cdcf1ad360d6db6dcb722ad4d604ff65851db9a1b632a3befe3eecf5e` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.keymaster@4.0.so` | `recovery/root/vendor/lib64/android.hardware.keymaster@4.0.so` | 214000 | `61d6d0338d33a25f200b9243f4d6c00b2739ca687f0c32d42c2f82484bd6405b` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.keymaster@4.1.so` | `recovery/root/vendor/lib64/android.hardware.keymaster@4.1.so` | 151192 | `ef3c94d27350b9046e8f8306ff3a353e2bd35b17b72672077606e4a083becc12` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.nfc-V1-ndk.so` | `recovery/root/vendor/lib64/android.hardware.nfc-V1-ndk.so` | 76552 | `43b1c8a021d36c99e00c8ba742264f80fb09ea150a89e7c24f971bc2b801ad69` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.secure_element-V1-ndk.so` | `recovery/root/vendor/lib64/android.hardware.secure_element-V1-ndk.so` | 72256 | `b45d7789a31d491c339b12462c9558af6bd2c0d97cfa94d630e588cca1948087` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.secure_element@1.0.so` | `recovery/root/vendor/lib64/android.hardware.secure_element@1.0.so` | 195904 | `27335f39e15394a74adb00339eccd034590e2ddacb5444ea960bfa4f7b235043` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.security.keymint-V3-ndk.so` | `recovery/root/vendor/lib64/android.hardware.security.keymint-V3-ndk.so` | 136920 | `4aba269babf078e1944faa25cdea7d45838dfc06cadf2f3ce324a5befa5cfb19` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.security.rkp-V3-ndk.so` | `recovery/root/vendor/lib64/android.hardware.security.rkp-V3-ndk.so` | 59080 | `02f625f01288565a26a837338abac64d1eeb9a7ed77f9153d497c5898f2e73d7` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.security.secureclock-V1-ndk.so` | `recovery/root/vendor/lib64/android.hardware.security.secureclock-V1-ndk.so` | 50408 | `400360e5a46dac7a59cb4f0176c16c9da0f1dbf9efbb275a2d607cc08313debc` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.security.sharedsecret-V1-ndk.so` | `recovery/root/vendor/lib64/android.hardware.security.sharedsecret-V1-ndk.so` | 46576 | `8d845e99501d674d09c4225399c3678f6f39fcfcca2f77318057c77fb6791dd9` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.security.sharedsecret-V2-ndk.so` | `recovery/root/vendor/lib64/android.hardware.security.sharedsecret-V2-ndk.so` | 46576 | `f7c3cfb6de7ae3826bce946d3dba030645781c8906db7956714f394f9a311df3` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.sensors-V2-ndk.so` | `recovery/root/vendor/lib64/android.hardware.sensors-V2-ndk.so` | 122736 | `755eb4acca46915a680c1216c27b0bf4ee34b165978960bf9114958921478ea5` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.sensors@1.0.so` | `recovery/root/vendor/lib64/android.hardware.sensors@1.0.so` | 149040 | `40b262aace0fde6dc707a14ab6e39d5f9493b51a5480d7abfa509b252b004427` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.sensors@2.0.so` | `recovery/root/vendor/lib64/android.hardware.sensors@2.0.so` | 208776 | `299d6b36661d9a9b8b852bcabc110d140ec7c46741c8f89739c326f3990d8c11` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/android.hardware.weaver-V2-ndk.so` | `recovery/root/vendor/lib64/android.hardware.weaver-V2-ndk.so` | 50592 | `733c94b53b0004259b66aadff2b7a214288a8b7681dc994f65a6edcedffd3a04` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libEseUtils.so` | `recovery/root/vendor/lib64/libEseUtils.so` | 14856 | `d880670f15d17ee25b582ee8843d54fdfd7ea15f7c189ee131f5727daac34bcc` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libGPMTEEC_vendor.so` | `recovery/root/vendor/lib64/libGPMTEEC_vendor.so` | 54408 | `5b6885cd4af62cab0fe87eda132d5dfbf1064f9e177baa6b20d1a671412f7ae6` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libGPQeSE.so` | `recovery/root/vendor/lib64/libGPQeSE.so` | 41040 | `c0ddb025904f05d38f93678d7b340a5fb626872cd79a7e58b881913587403ad6` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libGPTEE_vendor.so` | `recovery/root/vendor/lib64/libGPTEE_vendor.so` | 32528 | `3d0f6191951dbb07fec9e2c14ea3d159837026e11996cf35aa9b641664014471` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libGPreqcancel.so` | `recovery/root/vendor/lib64/libGPreqcancel.so` | 27768 | `e1a9674282200276cfaaee0e7654b24816b7ff2de4fa3d3f6022052ae01d53bb` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libGPreqcancel_svc.so` | `recovery/root/vendor/lib64/libGPreqcancel_svc.so` | 32048 | `df120975d6b04c5e7062421e2689c9030c7ee6cf759ca83a4762014377eef787` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libQSEEComAPI.so` | `recovery/root/vendor/lib64/libQSEEComAPI.so` | 70568 | `76deb0c8e8ae672dcd1a15fe87103c44231bcf347f67b71a6e63653f9de51e77` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/lib_android_keymaster_keymint_utils.so` | `recovery/root/vendor/lib64/lib_android_keymaster_keymint_utils.so` | 25064 | `eb6b40a89500b3338863087c75fe99421b0c9f8391c7b531563facd3d5f3692d` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libaacvibrator.so` | `recovery/root/vendor/lib64/libaacvibrator.so` | 106024 | `246810a10f6d4eba14fae12c98eeb9f788b43e955e6f35d81934543ed1771437` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libagmmixer.so` | `recovery/root/vendor/lib64/libagmmixer.so` | 44864 | `50b417e2d9a4e4b5352ae78a850c359d1398c43f1fbf55e920a233c1ffa838d4` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libbinder_ndk.so` | `recovery/root/vendor/lib64/libbinder_ndk.so` | 120656 | `19da5bc10c4eac7afd25775b7b9c959f556904c2fc4b9fa41dfb1e250296a338` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libboot_control_qti.so` | `recovery/root/vendor/lib64/libboot_control_qti.so` | 171936 | `1f18aa4b895f04e10d7386f667c8b7a21923f2880b3f1c5f1902c2da66288b42` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libcppbor_external.so` | `recovery/root/vendor/lib64/libcppbor_external.so` | 65984 | `d3a5e6f6ed723ec4bdb1d1dac802702d2884954fce1e649ef3e7fe4a5a6f13da` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libcppcose_rkp.so` | `recovery/root/vendor/lib64/libcppcose_rkp.so` | 79752 | `3e2ff36b5687c3988cae5ade8332a886d62bf18d6c3570f0a712452a2165218d` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libcrypto.so` | `recovery/root/vendor/lib64/libcrypto.so` | 1387464 | `5f67e6ae6d4650eaa4147f2c500b2bd63332062c3122435359f684b36c75e957` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libdiag.so` | `recovery/root/vendor/lib64/libdiag.so` | 317920 | `e17110db77010046631e65b3a6f2d4296af0286df17b721498f0da70ff594f6e` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libdisplayconfig.qti.so` | `recovery/root/vendor/lib64/libdisplayconfig.qti.so` | 152328 | `bb0c2b77ab1fb360f1c995c283a0718cf18c7d367cfc9af85f08689d86ab64ca` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libdmabufheap.so` | `recovery/root/vendor/lib64/libdmabufheap.so` | 95952 | `b7eeead8d1e7b62f3a7991b4c9aeefbc923640d8a70b2367980b32f165abe180` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libdrm.so` | `recovery/root/vendor/lib64/libdrm.so` | 94744 | `28cb1c84cb0b59bc6f9dda0fefef7a553747ce6f1384327ebdf6b16f3408fa62` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libdrmfs.so` | `recovery/root/vendor/lib64/libdrmfs.so` | 48904 | `3caae3b4c89c2e2d9ecd06906ce8f59eb01feb33f00a815de3b24f233bdc24bf` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libdrmtime.so` | `recovery/root/vendor/lib64/libdrmtime.so` | 14904 | `f5f8903fe0f0bfdb943ecccd65c1a299a60fa7b607bffe581299b5adebf43e3e` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libfmq.so` | `recovery/root/vendor/lib64/libfmq.so` | 19704 | `8f986619d0a23c17b85e9e0be89ec5c42a0acbb2f000c36935c4daa3f9c03dfb` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libgatekeeper.so` | `recovery/root/vendor/lib64/libgatekeeper.so` | 32192 | `689242f35e30bdbf98aac537737cae5a1c11a1bb4d243c7fe75df6c332c14508` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libgpt.so` | `recovery/root/vendor/lib64/libgpt.so` | 40448 | `8a6e46903b870068276abed89bd7bce01da3cfc0aa3e60ef82e3815aae7288f7` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libhidl-gen-hash.so` | `recovery/root/vendor/lib64/libhidl-gen-hash.so` | 122576 | `94bac797547b3aba31f463808e0942e0837e651be288852cb9834985e9102426` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libion.so` | `recovery/root/vendor/lib64/libion.so` | 14840 | `b308fd2c22fb4e94f243dac6fd89d9738a26939e34098666adb6e0c770021ec5` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libjc_weaver_transport.so` | `recovery/root/vendor/lib64/libjc_weaver_transport.so` | 85512 | `897044ec27e57e57e5044f9bc379d5e2f5300c93898813f09a4892a534f0c940` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libkeymaster_messages.so` | `recovery/root/vendor/lib64/libkeymaster_messages.so` | 122144 | `0e4175e7c7fec98501bcbb7c8986c54b47aae78e24a46eecdee4db97b13632df` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libkeymaster_portable.so` | `recovery/root/vendor/lib64/libkeymaster_portable.so` | 553576 | `32b462def13836c11d372bef17895c667d3c0f841743690e2b15836f64789f08` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libkeymasterdeviceutils.so` | `recovery/root/vendor/lib64/libkeymasterdeviceutils.so` | 36184 | `5e7a41ff0b6d5f81bbc6c65ac22a66a24dede61342f8e3ca19fc216b7cd1c1fa` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libkeymasterprovision.so` | `recovery/root/vendor/lib64/libkeymasterprovision.so` | 111104 | `0260402abf3a1456d29ad2c850e64f737e0f85bb80733ec6905879a6b4055b33` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libkeymasterutils.so` | `recovery/root/vendor/lib64/libkeymasterutils.so` | 259976 | `baa4a23952575831ddade9ef404798bc2278db16a7489b77b307eb17bde2f65b` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libmemunreachable.so` | `recovery/root/vendor/lib64/libmemunreachable.so` | 217784 | `24e75b17d2e8fea762e87b20a6a5c1f97ea40821c1296da7e9f7e8eff41c4921` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libminkdescriptor.so` | `recovery/root/vendor/lib64/libminkdescriptor.so` | 54352 | `10ea80d7928476e76a8a94c0facf45ecac71d551d33ded23f25ee33ae8f5c808` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libminksocket_vendor.so` | `recovery/root/vendor/lib64/libminksocket_vendor.so` | 120896 | `b51f847c0c370d0adc54ab1336a0e47110e0dbe3af02b5deda024ce521d1d7fb` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libops.so` | `recovery/root/vendor/lib64/libops.so` | 32312 | `8047a50de8b138bd6b95c11efd9e9044d0a8e87f4eb71587764cf739bcecf034` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libpalclient.so` | `recovery/root/vendor/lib64/libpalclient.so` | 135792 | `6778fc1c9e5daad7ce55bf649e86afe3ac74905ed578f1b1bffd425109d5fa81` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqcbor.so` | `recovery/root/vendor/lib64/libqcbor.so` | 28096 | `7973f24411c2eb510b64004a0a4e5b930efb446f9743ab82def830d6bdba3418` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqdutils.so` | `recovery/root/vendor/lib64/libqdutils.so` | 41032 | `64637e3396d8871e2566c921f8ea82b64d7b1dfd5d09c6af201fbaa6bf208fe8` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqisl.so` | `recovery/root/vendor/lib64/libqisl.so` | 40584 | `52eed4da8f8b5c47f23b42d49b6d031d084dec22dc1858762bbe7a22f66f1122` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqrtr.so` | `recovery/root/vendor/lib64/libqrtr.so` | 28264 | `6130f28b48146f0e2510de01ee110fa42fd8c02040145057f6d5d9b7a5da84cd` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqservice.so` | `recovery/root/vendor/lib64/libqservice.so` | 74320 | `a1c5ca9496a2738ead199c02d5dcbea949bd8080496e85cfd80c6a8073e59d46` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqtigatekeeper.so` | `recovery/root/vendor/lib64/libqtigatekeeper.so` | 70720 | `cc3b55ecaaf20c80e81d9c7bcf9d03919aa0f6ab51d072eb5722cb6b3765cf9f` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqtikeymaster4.so` | `recovery/root/vendor/lib64/libqtikeymaster4.so` | 241088 | `f05ade9981077680132041814a4c92943a2898d53cdbd2d6528e3589cff730e6` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqtikeymint.so` | `recovery/root/vendor/lib64/libqtikeymint.so` | 287856 | `a0abda196b06aaf7d12f32ca49075efd282c78b9037bf81a78805da1c26ee6a1` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libqtivibratoreffect.so` | `recovery/root/vendor/lib64/libqtivibratoreffect.so` | 15168 | `3bbb7dad9e1b929f2b2baeead1d04366bf9ed8e64b6a5944fed7d04de93e8f4e` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/librecovery_updater.so` | `recovery/root/vendor/lib64/librecovery_updater.so` | 141720 | `0907b5a1469394a9b6e5dfc257e7df1d602e7ad3310944f81981738251b3e30f` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/librecovery_updater_msm.so` | `recovery/root/vendor/lib64/librecovery_updater_msm.so` | 61544 | `da8ef87eb6073191c2049f4f83b1a1f17ec70959cc4e4d254df2e5d60c66e9a4` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/librpmb.so` | `recovery/root/vendor/lib64/librpmb.so` | 50256 | `5b881d3e30e05021cfc67697e265df8b6eca9530e890ff7f4ed98e25eb01d731` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libsoc_helper.so` | `recovery/root/vendor/lib64/libsoc_helper.so` | 15104 | `e29ea2f271292fb5831b6a319c6171d275fe8fa4855a80f2d735f58cea4b0dd9` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libspcom.so` | `recovery/root/vendor/lib64/libspcom.so` | 149480 | `55ef480d07a305b4809ef0ff9e8e9cce5828b0c72ce3074f2fcd26a5674762e3` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libspl.so` | `recovery/root/vendor/lib64/libspl.so` | 15120 | `929b1c1c4c2e0914c479e1a901299e5f73fda62f483ad529c1849074f4ec4875` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libspukeymint.so` | `recovery/root/vendor/lib64/libspukeymint.so` | 242208 | `e197dae1f77178ac22ad3e52514010c78949820fe1926bc353c5e98ab2fec7d1` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libspukeymintdeviceutils.so` | `recovery/root/vendor/lib64/libspukeymintdeviceutils.so` | 73928 | `752ed11b0a88b0610779dd789562c12942e1520bcc2d40623266c8a9c2e1cad2` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libspukeymintprovision.so` | `recovery/root/vendor/lib64/libspukeymintprovision.so` | 98560 | `95ea5ebe972a996b4120ae04fd57e1e92801f71e83c3d2549f20e94d7ad17039` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libspukeymintutils.so` | `recovery/root/vendor/lib64/libspukeymintutils.so` | 260024 | `f4381e626244b415adb9f62c56e293c859df272772c5b986dcc3fbafb080edee` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libspuqtigatekeeper.so` | `recovery/root/vendor/lib64/libspuqtigatekeeper.so` | 70712 | `066da9d1555deb8af4797d5441d9b2d84c9522c09c784ad382d20ae392a946fd` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libssd.so` | `recovery/root/vendor/lib64/libssd.so` | 15192 | `667b6ec360b6a14538a231379966726ea6b1e4c2a209d808aa4a0d509679b253` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libtime_genoff.so` | `recovery/root/vendor/lib64/libtime_genoff.so` | 23680 | `93a863da3ed2cbb99028f22d75cba5eba4f840ce8832e4c764219cf418a48654` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libtinyalsa.so` | `recovery/root/vendor/lib64/libtinyalsa.so` | 45128 | `156e9c78b7e04217d45f9c0bde9665aae16855e74d27365867dc5d3203ccbaa7` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libvmmem.so` | `recovery/root/vendor/lib64/libvmmem.so` | 40832 | `3112ecaa20139417703486041eeab17ccb82a00a4b23260cfe6e269a3ebd92b1` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/libxml2.so` | `recovery/root/vendor/lib64/libxml2.so` | 1298136 | `9b1b89edf548ceb1d852dda3a8acd187f26cf1ac1908950fd21b1a8082a7156c` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.aac.hardware.richtap.vibrator-V1-ndk_platform.so` | `recovery/root/vendor/lib64/vendor.aac.hardware.richtap.vibrator-V1-ndk_platform.so` | 67720 | `53d06a2bd84598ab7767b1120c396501f2eac0479afc63070735869e4281187b` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.0.so` | `recovery/root/vendor/lib64/vendor.display.config@1.0.so` | 176064 | `b68681b58dc273ef1bfab23054c23b6455641cba01e82adc88ffe3c5aaefadf0` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.1.so` | `recovery/root/vendor/lib64/vendor.display.config@1.1.so` | 140816 | `43a5520b42a44a72b8a4c9c8c008f728813b47dee2514fbbf538e53e12f1ad7a` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.10.so` | `recovery/root/vendor/lib64/vendor.display.config@1.10.so` | 295000 | `0c7ca7bf0f939972b02a1fde1d9bd643da4837e6064beb3cc8ca6916c8d764ea` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.11.so` | `recovery/root/vendor/lib64/vendor.display.config@1.11.so` | 231400 | `513761c0a8f0145a01e5a93b9266eddfacdd1939f7167e0dc46c6c4713b82027` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.2.so` | `recovery/root/vendor/lib64/vendor.display.config@1.2.so` | 145808 | `4e9e26a5dbe15f2760794371b41bacff396cd5c690de37aabbf969bf16ab995d` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.3.so` | `recovery/root/vendor/lib64/vendor.display.config@1.3.so` | 150912 | `7de923ffc630b5fee790c456edee34ca165b579ff0cd580fe96456965b37b448` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.4.so` | `recovery/root/vendor/lib64/vendor.display.config@1.4.so` | 160712 | `f8bf312a607e4befc11b267ce770e643f0e3c196efa8514243ca8c3fcc049254` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.5.so` | `recovery/root/vendor/lib64/vendor.display.config@1.5.so` | 165480 | `8877defabbb954c3e6a6cb2baabdb5e48f0e0029ec08bbe22b4948badd487e9b` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.6.so` | `recovery/root/vendor/lib64/vendor.display.config@1.6.so` | 174816 | `1cfd4528b8bb4dee34a1589430d189d2dc4a5f8e0982a7bc2b325901972ef92a` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.8.so` | `recovery/root/vendor/lib64/vendor.display.config@1.8.so` | 202960 | `986e6b96309325f1b2e2ef6d5346832965edf08b5766d9888acdb27e864bc966` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@1.9.so` | `recovery/root/vendor/lib64/vendor.display.config@1.9.so` | 211960 | `72f0d9bd1ee8fd136d5eaebd64b1743439d8099070094ea01e90df6ffa5158c6` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.display.config@2.0.so` | `recovery/root/vendor/lib64/vendor.display.config@2.0.so` | 178312 | `ae7485ce1c8f86cc1402abc00e06dcdff71e61a3b40556b0f8a7256c810c2fc5` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.qti.hardware.display.config-V7-ndk.so` | `recovery/root/vendor/lib64/vendor.qti.hardware.display.config-V7-ndk.so` | 157008 | `d55b5f5430df5a9a5f4e3a3a8c79af173ac2001a50f810342cd4ad55e69a537b` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.qti.hardware.pal@1.0-impl.so` | `recovery/root/vendor/lib64/vendor.qti.hardware.pal@1.0-impl.so` | 151024 | `83de04453a3a853501b7ef9a7e49d008faad519545a9cbcfb9f0c28060a74279` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.qti.hardware.pal@1.0.so` | `recovery/root/vendor/lib64/vendor.qti.hardware.pal@1.0.so` | 338720 | `31edd8066ce1b245a3a816ec781bcc49836983cfe20ea732e7c34e3906ee602b` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.qti.hardware.qseecom-V1-ndk.so` | `recovery/root/vendor/lib64/vendor.qti.hardware.qseecom-V1-ndk.so` | 93504 | `e9b37e695b94b14cb1a7bc08afe3e5b5c266af5a45b31aab4aa955b854ae257d` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.qti.hardware.qseecom@1.0-impl.so` | `recovery/root/vendor/lib64/vendor.qti.hardware.qseecom@1.0-impl.so` | 70800 | `e3d32d461201d342064dba72cb0f14533064ee61be7bf6a14761a97ed4f49123` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/lib64/vendor.qti.hardware.vibrator.impl.so` | `recovery/root/vendor/lib64/vendor.qti.hardware.vibrator.impl.so` | 45608 | `459f451e1de42636953ef855b04a1b5055b7d4243f14ea6ed21da90ceae2e9df` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/odm/etc/aac_richtap.config` | `recovery/root/vendor/odm/etc/aac_richtap.config` | 81940 | `88b23dace5896e8e20df2baea79c4c17e12db60fa87251fcd56c44ba8739b304` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
| `vendor/odm/etc/ueventd.rc` | `recovery/root/vendor/odm/etc/ueventd.rc` | 5172 | `1451f1ae85a9418e2ac6ba58e3272c8df0506a81f71db5f90f93261a67a0724a` | `adontoo/device_meizu_m2461-TWRP@twrp-16 twrp-16 (flash-tested TWRP)` |
