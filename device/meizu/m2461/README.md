# AERA Recovery Project device tree for Meizu 21

Device codename: `m2461` (also reports `meizu21` / `meizu_21_CN`)

Platform: Qualcomm SM8650 `pineapple` (`meizu_sm8650`, Adreno 750)

Recovery partition limit: 100 MiB (dedicated `recovery` partition on an A/B device)

This tree carries the Meizu 21 integration for AERA Recovery Project R1.0 on the
`aera-16.0` (Android 16) manifest. All hardware values were taken from the
flash-tested TWRP device tree for this exact device
([adontoo/device_meizu_m2461-TWRP](https://github.com/adontoo/device_meizu_m2461-TWRP))
and from `docs/FACTS.md` in the project repository.

## Device specifications

Basic   | Spec Sheet
-------:|:-------------------------
SoC     | Snapdragon 8 Gen 3 (SM8650 / pineapple)
CPU     | Octa-core, 1x Cortex-X4 + 5x Cortex-A720 + 2x Cortex-A520
GPU     | Qualcomm Adreno 750
Memory  | 8 / 12 GB LPDDR5X
Storage | 256 / 512 GB UFS
Battery | 4800 mAh (typ), non-removable
Display | 1080 x 2340, 6.55", AMOLED, 120 Hz
Shipped Android version | 14 with Flyme 10 (this tree targets Flyme 12.6 / Android 16)

## Partition facts used by this tree

Partition | Size (bytes) | Note
---------:|-------------:|-----
boot | 100663296 | GKI, header v4, no ramdisk
init_boot | 8388608 |
vendor_boot | 100663296 | vendor_ramdisk, LZ4 legacy compression
dtbo | 25165824 |
recovery | 104857600 | dedicated A/B recovery partition
super | 9663676416 | group `qti_dynamic_partitions`, EROFS logical partitions

Logical partitions: `system system_ext product vendor vendor_dlkm odm`
(plus `system_dlkm`, which is present in the fstab as a logical volume but is
not listed in the super group of the stock layout).

## Hardware support

Verified on the TWRP base this tree is derived from:

- Display, touch (with the Meizu coordinate mapping), vibrator
- Flashing, sideload, fastbootd, MTP, USB OTG
- File-based encryption / decryption (QCOM FBE, fscrypt policy 2)

Carried over to AERA but **not yet verified** on an AERA build:

- AERA native UI rendering on the 1080 x 2340 panel - `AERA_STATUS_H` and the
  status indents in `BoardConfig.mk` are marked `UNVERIFIED` starting values
- AERA haptics through the Meizu Richtap vibrator HAL
- AERA installer (payload, backup/restore, dynamic-partition tools)
- Recovery image size against the 100 MiB partition budget - AERA R1.0 with
  LVGL is larger than TWRP, so check `out/target/product/m2461/recovery.img`
  after the first build

Not supported:

- Wi-Fi - the device ships no WLAN firmware/supplicant setup for recovery and
  its CNSS drivers live in `vendor_dlkm`, so `AERA_ENABLE_WLAN` stays off. See
  `aera_m2461.mk` for the full reasoning.
- AERA device-local helper modules (audio bridge, GPU probe, WLAN MAC helper) -
  this tree ships no sources for them.

## Files

File | Purpose
-----|--------
`BoardConfig.mk` | Kernel, partitions, dynamic partitions, crypto, display and tool switches
`device.mk` | Shipping API, dynamic partitions, Soong namespace, inherits `aera_m2461.mk`
`aera_m2461.mk` | AERA tuning block (A/B recovery partition, f2fs, partition tools, LZ4)
`twrp_m2461.mk` | Product definition (name, brand, model, fingerprint)
`AndroidProducts.mk` | Lunch targets `twrp_m2461-{user,userdebug,eng}`
`recovery.fstab` | Unmodified copy of the verified TWRP fstab for this device
`system.prop` | ADB, gatekeeper, charger, USB ids
`recovery/root` | Proprietary HAL binaries, firmware and init scripts (TWRP blob set)
`prebuilt/kernel` | Verified stock `Image` (v4 header, 4096-byte pages)

## Build

```sh
repo init -u https://github.com/AERA-Recovery/android_manifest.git -b aera-16.0
repo sync -j8
# put this tree at device/meizu/m2461
source build/envsetup.sh
lunch twrp_m2461-bp2a-eng
mka adbd recoveryimage
```

The output is written to `out/target/product/m2461/recovery.img`.

## Flash

This device has a dedicated A/B recovery partition:

```sh
fastboot flash recovery_a out/target/product/m2461/recovery.img
fastboot flash recovery_b out/target/product/m2461/recovery.img
```

## Credits

- **adontoo** and **YuKongA** - the proven, flash-tested TWRP device tree for
  the Meizu 21 that every hardware value in this tree comes from
- The AERA Recovery Project for the R1.0 recovery backend and UI
- The OrangeFox Recovery Project, whose build system AERA inherits

The proprietary files in `recovery/root` were extracted from the matching
Meizu 21 stock firmware. Do not reuse them on another device.
