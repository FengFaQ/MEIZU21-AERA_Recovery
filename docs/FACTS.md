# M2461 (Meizu 21) - verified device facts

> Source of truth hierarchy:
> 1. **Verified TWRP device tree** `adontoo/device_meizu_m2461-TWRP@twrp-16`
>    (locally at `C:\Step\work\tr\twrp\`) - a real, flash-tested recovery for this
>    exact device. Its README claims display, touch, vibrator, flashing, sideload,
>    fastbootd, MTP, USB OTG and **decryption** all work.
> 2. **AERA Recovery / AERA wiki** (`C:\Step\work\tr\ref.json`,
>    `C:\Step\work\tr\ref_readable.txt`, source commit `2d147fb2`).
> 3. Main project memory (`C:\Step\记忆\`, esp. `05_MEIZU21_FACTS.md`).
>
> Anything not backed by 1-3 must be marked **UNVERIFIED** in code comments.

---

## 1. Identity

| Field | Value | Source |
|---|---|---|
| Marketing name | Meizu 21 | main project |
| Model code | M2461 | kernel cmdline, 9008 dump name |
| Device codenames | `meizu21`, `meizu_21_CN` | vendor_dlkm build.prop |
| SoC | Snapdragon 8 Gen 3 / **SM8650** / Qualcomm `pineapple` | TWRP BoardConfig |
| Platform (TWRP) | `PRODUCT_PLATFORM := pineapple`, `TARGET_BOARD_PLATFORM := meizu_sm8650`, `TARGET_BOARD_PLATFORM_GPU := qcom-adreno750` | TWRP `BoardConfig.mk` |
| Display | 1080 x 2340, 6.55", AMOLED, 120 Hz | TWRP README (says 21Pro in prose, numbers match Meizu 21) |
| RAM / storage | 8/12 GB LPDDR5X, 256/512 GB UFS | TWRP README |
| Product branding for tree | `PRODUCT_RELEASE_NAME := meizu21`, `PRODUCT_DEVICE := m2461`, `PRODUCT_MODEL := Meizu 21` | TWRP `twrp_m2461.mk` |

### Build fingerprint (Flyme 12.6, Android 16 base)
From the previous GKI project extraction of `vendor_dlkm` `build.prop`:

```
meizu/meizu_21_CN/meizu21:16/BQ2A.251016.001-BP2A.250605.031.A3/<timestamp>:user/release-keys
```

The exact trailing build id/timestamp is **NOT** recorded in memory; only the
prefix `meizu/meizu_21_CN/meizu21:16/BQ2A.251016.001-BP2A.250605.031.A3` is
verified. Use that prefix with a placeholder suffix and mark it UNVERIFIED.

---

## 2. Boot chain / partitions

| Partition | Size (bytes) | Notes |
|---|---|---|
| boot | 100,663,296 | header v4, os_version 14.0.0, patch 2023-10, **no ramdisk** (GKI) |
| vendor_boot | 100,663,296 | contains vendor_ramdisk (LZ4 legacy) with 335 `.ko` + `fstab.qcom` |
| dtbo | 25,165,824 | |
| init_boot | 8,388,608 | |
| **recovery** | **104,857,600** | dedicated recovery partition (A/B). TWRP flashes `recovery_a` + `recovery_b` |
| super | 9,663,676,416 | group `qti_dynamic_partitions` |
| metadata | - | f2fs, holds metadata encryption key dir |
| userdata | - | f2fs, FBE policy v2 (`TW_USE_FSCRYPT_POLICY := 2`) |
| misc | - | |
| logfs | - | vfat, Meizu-specific |
| mzlogfs | - | ext4, Meizu-specific |

Dynamic partitions (all **EROFS**): `system system_ext product vendor vendor_dlkm odm`
(TWRP tree also lists `system_dlkm` in `AB_OTA_PARTITIONS`; its fstab entry is
`system_dlkm /system_dlkm erofs ro wait,slotselect,logical`.)

Boot procedure used by the proven TWRP tree:

```
fastboot flash recovery_a  recovery.img
fastboot flash recovery_b  recovery.img
```

`BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE := true` is set by TWRP, then the
TWRP build still injects the prebuilt kernel. For AERA keep the same approach
(`BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE := true` + `TARGET_PREBUILT_KERNEL`)
because the boot header must be v4 with `--header_version 4 --pagesize 4096`.

---

## 3. Kernel

| Field | Value |
|---|---|
| Release string | `6.1.25-android14-11-maybe-dirty` |
| GKI branch | `android14-6.1.25-2023-10` |
| Header | v4, pagesize 4096, `Image` (arm64, EFI-stub `MZ` header) |
| Prebuilt kernel size | **33,892,864 bytes** (identical between stock boot.img and TWRP tree) |
| Compiler | Android clang 17.0.2 (r487747c) |
| ramdisk compression | **LZ4** (`BOARD_RAMDISK_USE_LZ4 := true`) |

`system_dlkm` modules live under `/system_dlkm/lib/modules/<release>/`, so the
kernel release string matters for module lookup. **Do not swap the prebuilt
kernel for a third-party build** - that was the root cause of the bootloop in
the GKI project (`C:\Step\uwuaosp\gki\交接文档.md` §7.5.2).

---

## 4. Vendor ramdisk modules to preload (from TWRP `TW_LOAD_VENDOR_MODULES`)

```
adsp_loader_dlkm.ko goodix_ts.ko cs_press_m68.ko cs_press_f71.ko
nxp-nci.ko stm_st54se_gpio.ko stm_nfc_i2c.ko
```

`TW_LOAD_VENDOR_MODULES_EXCLUDE_GKI := true`.

Touch: **Goodix** panel driver `goodix_ts.ko`; Meizu-specific touch coordinate
mapping is required (`TW_USE_MEIZU_TOUCH_MAPPING := true`). In AERA this is
`AERA_USE_MEIZU_TOUCH_MAPPING`.
Input blacklist: `hbtp_vm` (`TW_INPUT_BLACKLIST := "hbtp_vm"`).

Vibrator: custom Meizu **Richtap** path (`/dev/aac_richtap_dev`,
`/odm/etc/aac_richtap.config`, `libaacvibrator.so`,
`vendor.aac.hardware.richtap.vibrator-V1-ndk_platform.so`,
`vendor.qti.hardware.vibrator.service`, `TW_SUPPORT_INPUT_AIDL_HAPTICS := true`).

---

## 5. Display / sysfs paths (verified in TWRP tree)

| Path | Use |
|---|---|
| `/sys/class/backlight/panel0-backlight/brightness` | backlight (`TW_BRIGHTNESS_PATH`) |
| max brightness 4095, default 2048 | |
| `/sys/class/thermal/thermal_zone47/temp` | CPU temperature (`TW_CUSTOM_CPU_TEMP_PATH`) |
| `/sys/class/qcom-haptics/primitive_duration` | haptic tuning (chown 0600) |
| `/dev/aac_richtap_dev` | Meizu haptic device (chmod 0666 at boot) |
| `/dev/hbtp_vm`, `/dev/hbtp_input` | Meizu input devices (declare in ueventd, blacklist hbtp_vm) |

---

## 6. Encryption stack (from TWRP tree rc files + blobs)

* `BOARD_USES_QCOM_FBE_DECRYPTION`, `BOARD_USES_METADATA_PARTITION`,
  `TW_USE_FSCRYPT_POLICY := 2`, `TW_INCLUDE_FBE_METADATA_DECRYPT`.
* `vendor.gatekeeper.disable_spu=true`, `vendor.gatekeeper.is_security_level_spu=0`
  (system.prop).
* Sequence proven by TWRP:
  1. `on fs`: `wait /dev/block/bootdevice/by-name/ssd 10`, `start vendor.qseecomd`,
     `install_keyring`.
  2. `vendor.sys.listeners.registered=true` -> `start vendor.gatekeeper_default`,
     `start vendor.secure_element`.
  3. `twrp.modules.loaded=true` -> `start start_crypto_services`
     (script reads `ro.bootimage.build.version.release` / `security_patch` from the
     **boot partition strings** via `resetprop`, then `setprop ctl.start
     vendor.keymint-qti`).
  4. `crypto.ready=1` -> keymint is already handled by the script in TWRP; AERA
     tree may keep the same order.

---

## 7. Blob inventory available (verified TWRP tree)

173 files / 18 MB under `recovery/root`, notably:

* `vendor/bin/hw/`: qti boot, fastboot (example_recovery), gatekeeper (qti + spu),
  health (`_recovery`), secure_element, keymint (qti + spu), weaver-spu,
  qseecom service, qti vibrator — 11 binaries.
* `vendor/lib64/`: ~90 vendor `.so` (keymint, gatekeeper, weaver, ION, display
  config, vibrator, PAL, tinyalsa, xml2, crypto, ...).
* `vendor/lib64/hw`-style libs are flat in `vendor/lib64`.
* `vendor/firmware/`: `100_Haptic.bin`, `101_Haptic.bin`, `haptic_config.bin`,
  `goodix_cfg_group.bin`, `goodix_firmware.bin`.
* `vendor/firmware_mnt/image/`: signed firmware segments (`.b00`-`.b08`, `.mdt`)
  for a Meizu TA (`MEIZU OEM attestation CA`, `MEIZU OEM ROOT CA`) - needed by
  the attestation HAL when `/vendor/firmware_mnt` is not mounted.
* `vendor/etc/vintf/manifest/*.xml` + `compatibility_matrix.xml` + `manifest.xml`.
* `vendor/etc/init/*.rc` for every HAL (all `seclabel u:r:recovery:s0`).
* `vendor/etc/ueventd.rc` (257 `/dev` rules incl. Meizu nodes),
  `vendor/odm/etc/ueventd.rc` (TrustedUI/oemvm firmware handlers).
* `system/lib64/`: `libexpat.so`, `android.hardware.vibrator-V2-ndk.so`,
  `android.hardware.common-V2-ndk(_platform).so`.
* `system/bin/`: `busybox`, `lshal`.

---

## 8. AERA mapping rules (verified from `bootable/recovery/aera_config.mk`)

AERA public flags are `AERA_*`. The build bridges the ones the inherited
backend still reads (`TW_*`, `OF_*`). **Critical for this device:**

| AERA flag | bridges to | why it matters here |
|---|---|---|
| `AERA_USE_MEIZU_TOUCH_MAPPING` | `OF_USE_MEIZU_TOUCH_MAPPING` + `TW_USE_MEIZU_TOUCH_MAPPING` | required for correct touch |
| `AERA_SCREEN_H` | `OF_SCREEN_H` | 2340 |
| `AERA_BRIGHTNESS_PATH` | `TW_BRIGHTNESS_PATH` | panel0-backlight |
| `AERA_LOAD_VENDOR_MODULES` | `TW_LOAD_VENDOR_MODULES` | touch/nfc/haptic modules |
| `AERA_INPUT_BLACKLIST` | `TW_INPUT_BLACKLIST` | hbtp_vm |
| `AERA_SUPPORT_INPUT_AIDL_HAPTICS` | `TW_SUPPORT_INPUT_AIDL_HAPTICS` | Richtap vibrator |
| `AERA_USE_FSCRYPT_POLICY` | `TW_USE_FSCRYPT_POLICY` | policy 2 |
| `AERA_UI_ADAPTIVE_RESOLUTION` | soong cflag | native 1080x2340 layout |

AERA renames the boot-time `cache` partition to `recovery`
(`recovery/personality/`, `recovery/root/cache/`), so blobs that TWRP keeps in
`/cache/recovery` must be placed under `recovery/root/cache/`.

Vendor blob convention in AERA trees (verified against
`android_device_oneplus_infiniti-AERA`): vendor ramdisk files live at
**`recovery/root/vendor/...`** in the device tree and appear as `/vendor/...`
at runtime. There is no `vendor64` directory. System-side ramdisk files live at
`recovery/root/system/...`. The Meizu blobs therefore map 1:1:
`vendor/*` -> `recovery/root/vendor/*`, `system/*` -> `recovery/root/system/*`.

---

## 9. Build environment

* Manifest: `repo init -u https://github.com/AERA-Recovery/android_manifest.git -b aera-16.0`
* Device tree target dir: `device/meizu/m2461`
* `source build/envsetup.sh && lunch twrp_m2461-bp2a-eng && mka adbd recoveryimage`
* Output: `out/target/product/m2461/recovery.img`
* AOSP 16 single recovery target; needs ~120 GB disk and ~16 GB RAM.

---

## 10. Known unknowns (mark UNVERIFIED in code)

1. Exact Flyme 12.6 build fingerprint suffix.
2. Whether Meizu's touch mapping needs extra sysfs writes in AERA (TWRP patch
   `262b9aff` in `adontoo/android_bootable_recovery_twrp-14.1` was only needed for
   the TWRP fork; AERA has its own `OF_USE_MEIZU_TOUCH_MAPPING`).
3. WiFi: no WLAN blobs were shipped in the TWRP tree (`TW_NO_NETWORK := true`),
   and Meizu modules (`cnss2.ko` etc.) live in `vendor_dlkm`, not the ramdisk.
   Treat WiFi as **not supported** in v1 and keep `AERA_ENABLE_WLAN` off.
4. Size of the AERA recovery image vs. the 100 MB recovery partition - must be
   checked after the first build (AERA R1.0 with LVGL is bigger than TWRP).
