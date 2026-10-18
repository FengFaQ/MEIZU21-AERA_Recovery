# M2461 init scripts -- TWRP -> AERA deviation log

Device: Meizu 21 (codename `m2461`, SM8650 "pineapple", Flyme 12.6 / Android 16 base).
Source of truth for behaviour: the flash-tested TWRP tree
`adontoo/device_meizu_m2461-TWRP@twrp-16` (local copy `C:\Step\work\tr\twrp`, HEAD
`42177fb8e6ccf4b75299cf0eebc0a695b98cc717`), see `docs/FACTS.md`.
AERA conventions checked against `AERA-Recovery/android_device_oneplus_infiniti-AERA@aera-16.0`
(HEAD `13f0cb9d...`), `AERA-Recovery/android_device_xiaomi_garnet-AERA@aera-16.0` (HEAD `a693bca0...`),
`AERA-Recovery/android_bootable_recovery@aera-16.0` (HEAD `dea031e4...`, local copy `C:\Step\work\tr\aera`)
and `AERA-Recovery/android_system_core@aera-16.0` (HEAD `245ef44a...`, see sec.4).

Every file below lives in the tree under `device/meizu/m2461/recovery/root/` and lands
in the recovery ramdisk unchanged (see sec.1). Runtime paths are always spelled as they are
at runtime: `/vendor/...`, `/odm/...`, `/system/...`. **There is no `vendor64`
directory** -- the note in `docs/FACTS.md` sec.8 that mentions one is wrong; this document and
the two AERA reference trees supersede it.

---

## 1. AERA copy rule (evidence from `_vr_AERA_A16.sh`)

The A16 build script is a UTF-16LE text dump at `C:\Step\work\tr\_vr_AERA_A16.sh`
(converted to UTF-8 at `C:\Step\work\tr\_tmp_A16_utf8.sh` for reading; line numbers are
identical). What it actually says about the tree layout:

| # | Line(s) | Verbatim | Meaning |
|---|---|---|---|
| 1 | 250-251 | `RECOVERY_DIR="recovery"` / `AERA_VENDOR_PATH=vendor/$RECOVERY_DIR` | The **only** device-tree directory the script knows is `vendor/recovery` (AERA's own files: prebuilts, fonts, installer). It never mentions `recovery/root`. |
| 2 | 253-256 | `if [ "$AERA_VENDOR_CMD" = "Fox_Before_Recovery_Image" ]; then` ... `AERA_WORK="$TARGET_RECOVERY_ROOT_OUT"` / `AERA_RAMDISK="$TARGET_RECOVERY_ROOT_OUT"` | In the generated-image flow the script works directly inside the **ramdisk root**. Everything it copies there lands at the ramdisk root verbatim (`$RAMDISK_SBIN=/sbin`, `$RAMDISK_ETC=/etc`, `$RAMDISK_SYSTEM_BIN=/system/bin`, `$RAMDISK_SYSTEM_ETC=/system/etc`, lines 284-287 and the `/etc -> /system/etc` fix-up at 425-428). |
| 3 | 1112 | `$CP -pr $AERA_VENDOR_PATH/AERAExtras/* $AERA_RAMDISK/` | The script's own overlay source is `vendor/recovery/AERAExtras/*`, copied to the ramdisk root with no relocation. |

**Rule that follows:** a device tree path `recovery/root/<X>` becomes `/<X>` in the
ramdisk, unchanged. Therefore:

* `recovery/root/init.recovery.qcom.rc` -> `/init.recovery.qcom.rc`
  (AERA's `etc/init.rc:6` imports `/init.recovery.${ro.hardware}.rc`)
* `recovery/root/vendor/etc/init/*.rc` -> `/vendor/etc/init/*.rc` (init imports
  `/vendor/etc/init` automatically)
* `recovery/root/vendor/bin/...` -> `/vendor/bin/...`
* `recovery/root/system/...` -> `/system/...` (`/bin` and `/etc` are symlinks into it,
  so `recovery/root/system/etc` is also reachable as `/etc`)

No `vendor64`, `vendor_boot/`, `root/vendor/` or any other relocation exists in the rule.
The copy of `recovery/root` **itself** is not done by `_vr_AERA_A16.sh`; the script only
post-processes the already-staged ramdisk (`TARGET_RECOVERY_ROOT_OUT`). The staging is a
product-level copy; the AERA reference tree expresses it as (garnet `device.mk:43-53`):

```make
TARGET_RECOVERY_DEVICE_DIRS :=
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(DEVICE_PATH)/recovery/root/,$(TARGET_COPY_OUT_RECOVERY)/root/)
```

which preserves the tree layout exactly (`recovery/root/<X>` -> `recovery/root/<X>` in the
staging dir -> `/<X>` in the ramdisk). **Owner of the .mk files:** this file only records
the rule; the device `.mk` is out of scope for this task (sec.7 lists what it must contain).

`recovery/root/system/etc/recovery.fstab` is the AERA reference convention
(garnet `BoardConfig.mk:64` sets `TARGET_RECOVERY_FSTAB` to it). It is owned by the
`.mk` author and was **not** created here -- once the fstab lands there, the same
`find-copy-subdir-files` call installs it for free; no duplicate copy is needed.

---

## 2. File inventory (delivered by this task)

| AERA file (`recovery/root/...`) | Ramdisk path | Source (TWRP tree) | State |
|---|---|---|---|
| `init.recovery.qcom.rc` | `/init.recovery.qcom.rc` | `recovery/root/init.recovery.qcom.rc` | ported, sec.3 |
| `init.recovery.usb.rc` | `/init.recovery.usb.rc` | `recovery/root/init.recovery.usb.rc` | ported, sec.3 |
| `vendor/etc/init/qseecomd.rc` | `/vendor/etc/init/qseecomd.rc` | same name | ported, sec.3 |
| `vendor/etc/init/android.hardware.boot-service.qti.rc` | `/vendor/etc/init/...` | same name | ported + copyright, sec.3 |
| `vendor/etc/init/android.hardware.fastboot-service.example_recovery.rc` | `/vendor/etc/init/...` | same name | ported + copyright, sec.3 |
| `vendor/etc/init/android.hardware.gatekeeper-service-qti.rc` | `/vendor/etc/init/...` | same name | ported + copyright, sec.3 |
| `vendor/etc/init/android.hardware.health-service.qti.rc` | `/vendor/etc/init/...` | same name | ported (charger entry dropped), sec.3 |
| `vendor/etc/init/android.hardware.secure_element-service.qti.rc` | `/vendor/etc/init/...` | same name | ported + copyright, sec.3 |
| `vendor/etc/init/android.hardware.security.keymint-service-qti.rc` | `/vendor/etc/init/...` | same name | ported + copyright, sec.3 |
| `vendor/etc/init/vendor.qti.hardware.vibrator.service.rc` | `/vendor/etc/init/...` | same name | ported (service `disabled`), sec.3 |
| `vendor/etc/init/start_crypto_services.rc` | `/vendor/etc/init/...` | same name | ported, sec.3 |
| `vendor/bin/start_crypto_services.sh` | `/vendor/bin/start_crypto_services.sh` | same name | body verbatim, shebang changed, sec.3 |

Not delivered (deliberately): `init.recovery.wifi.rc` (sec.6), any rc for the shipped but
never-started SPU/weaver binaries (sec.5).

Pre-existing blobs in the same tree (copied earlier from TWRP, not touched by this task):
`vendor/bin/qseecomd`, `vendor/bin/hw/*`, `vendor/lib64/*`, `vendor/firmware*`,
`vendor/etc/ueventd.rc`, `vendor/odm/...`, `system/bin/*`, `system/etc/*`, `system/lib64/*`.

---

## 3. Script-by-script deviation table

Legend: **=** identical behaviour, **~** same behaviour with a cosmetic/carrier change,
**-** behaviour removed (with reason), **+** behaviour added (with reason).

### 3.1 `init.recovery.qcom.rc`

| TWRP section | AERA port | Status | Deviation and reason |
|---|---|---|---|
| `on init`: `setprop prepdecrypt.setpatch true` | same | = | `prepdecrypt` is consumed by the TWRP-derived AERA backend; no change. |
| `on init`: `start vendor.boot-qti` | same | = | |
| `on early-fs`: WALT sched_lib_name / mask | same (same two paths, same values) | = | `walt` sysfs is a Qualcomm scheduler feature; path is from the TWRP tree. |
| `on fs`: wait/symlink `bootdevice` | same | = | |
| `on fs`: `force_sysfs_fallback` write | same | = | |
| `on fs`: `/dev/qseecom`, `/dev/ion` chmod/chown | same | = | |
| `on fs`: `wait by-name/ssd 10`, `start vendor.qseecomd` | same | = | qseecomd stays `disabled` in its own rc and is started here (TWRP behaviour, avoids the "on init" race with `by-name/ssd`). |
| `on fs`: `/mnt/vendor/persist` mkdir + bind of `/tmp/persist` | same | = | |
| `on fs`: `install_keyring` | **removed** | - | See sec.4 -- AERA's init has no such builtin; keeping it fails the boot. **UNVERIFIED**: decryption must be re-tested on hardware. |
| `on boot`: `setprop sys.usb.config adb` | same | = | AERA's `etc/init.rc:148-149` also sets it for `ro.debuggable=1`; both agree. |
| `on property:vendor.sys.listeners.registered=true` -> gatekeeper_default, secure_element | same | = | |
| `on property:twrp.modules.loaded=true` -> `start start_crypto_services` | same | = | Property name is AERA's own (`aera/variables.h:372`: `TW_MODULES_MOUNTED_PROP "twrp.modules.loaded"`). |
| ... -> `mkdir /firmware`, `mount vfat .../modem${ro.boot.slot_suffix} /firmware ro` | same | = | |
| ... -> ADSP `boot`/`ssr` waits and writes, `wait /sys/class/power_supply/battery` | same | = | |
| ... -> `start vendor.health-default` | same | = | |
| ... -> `start vendor.qti.vibrator` | same (+ comment) | ~ | Documented here and in the vibrator rc: the service is `disabled` and started **only** by this hook. |
| `#umount /vendor` / `#symlink ...` comments | kept | = | |
| header | Qualcomm header kept, port note added | ~ | |

Runtime-path check: every sysfs/dev node used (`/proc/sys/walt/*`,
`/proc/sys/kernel/firmware_config/force_sysfs_fallback`, `/dev/qseecom`, `/dev/ion`,
`/dev/block/bootdevice/by-name/ssd`, `/sys/kernel/boot_adsp/{boot,ssr}`,
`/sys/class/power_supply/battery`) exists verbatim in the TWRP tree file; nothing new was
invented.

### 3.2 `init.recovery.usb.rc`

| TWRP rule | AERA port | Status | Deviation and reason |
|---|---|---|---|
| header | kept | = | |
| `on early-fs: setprop sys.usb.configfs 0` | same | ~ | Kept **on purpose**: AERA core `etc/init.rc:27` also sets `sys.usb.configfs 0`, and when `0` its configfs block (`init.rc:171-184`) is skipped, so this file stays the single owner of the gadget. |
| `on fs`: configfs mount, g1 gadget, `bcdUSB`, `os_desc`, 0x2717/0x4EE7, strings, `mass_storage.0`, `ffs.{mtp,adb,fastboot}`, `b1`/`b2` configs, MaxPower 900, MSFT100, functionfs mounts, `sys.usb.mtp.*`, `sys.usb.configfs 1` | same, rule by rule | = | |
| `on property:ro.boot.usbcontroller=*`: `wait .../ssusb/mode`, `mode idle`, `wait udc 0`, `mode peripheral`, `wait udc 1` | same | = | Same two paths (`/sys/bus/platform/devices/${ro.boot.usb.dwc3_msm:-a600000.ssusb}/mode`, `/sys/class/udc/${ro.boot.usbcontroller}`). |
| `sys.usb.config=none` reset block | same | = | |
| `init.svc.adbd=stopped` -> ffs.ready 0 | same | = | |
| adb / mtp,adb / sideload -> `start adbd` | same | = | |
| adb + sideload gadget: `idVendor 0x18D1` `idProduct 0x4EE7` | `0x${ro.recovery.usb.vid}` / `0x${ro.recovery.usb.adb.pid}` -> **18D1:D001** | ~ | TWRP's own system.prop declares these (`ro.recovery.usb.vid=18D1`, `ro.recovery.usb.adb.pid=D001`, `ro.recovery.usb.fastboot.pid=4EE0`) but its usb.rc never used them. Using them is the same `0x${prop}` form AERA core uses (`etc/init.rc:241/248/255`). Requires the three properties in `system.prop` (see sec.7). |
| fastboot gadget: `0x18D1` / `0xD00D` | `0x${ro.recovery.usb.vid}` / `0x${ro.recovery.usb.fastboot.pid}` -> **18D1:4EE0** | ~ | The device's stock fastboot gadget is 18D1:4EE0; 0xD00D is the generic AOSP fastbootd id. Cosmetic to the host tool, but matches the phone. |
| mtp / mtp,adb gadget: 0x2717/0xFF40, 0x2717/0xFF48 | same | = | Kept verbatim -- MTP was verified working in TWRP with these ids. |

### 3.3 `vendor/etc/init/*.rc`

| File (service) | Status | Deviation and reason |
|---|---|---|
| `qseecomd.rc` (`vendor.qseecomd`) | ~ | TWRP's copy has no copyright header; added Qualcomm's (`Copyright (c) 2019 Qualcomm Technologies, Inc.`, as in the infiniti tree). Kept `disabled` + `socket notify-topology` + LD_LIBRARY_PATH `/vendor/lib64:/vendor/lib64/hw:/system/lib64:/sbin`. |
| `android.hardware.boot-service.qti.rc` (`vendor.boot-qti`) | ~ | Header added (Qualcomm Innovation Center, BSD-3-Clause-Clear, as in the infiniti tree). No `disabled` -> started by `init.recovery.qcom.rc` on `init`. |
| `android.hardware.fastboot-service.example_recovery.rc` (`vendor.fastboot-default`) | ~ | Header added; `interface aidl android.hardware.fastboot.IFastboot/default` kept (AERA fastbootd needs it). |
| `android.hardware.gatekeeper-service-qti.rc` (`vendor.gatekeeper_default`) | ~ | Header added; `disabled`, started from `on property:vendor.sys.listeners.registered`. SPU variant deliberately not declared (system.prop: `vendor.gatekeeper.disable_spu=true`, `vendor.gatekeeper.is_security_level_spu=0`). |
| `android.hardware.health-service.qti.rc` (`vendor.health-default`) | - | **`vendor.charger` entry dropped.** Its section sets `seclabel u:r:charger_vendor:s0` and then overrides with `u:r:recovery:s0` -- only consistent on a permissive domain; AERA's charger path is AERA's own `/system/bin/charger`/recovery flow, and the bundle has no `charger_vendor` rules of its own. The battery HAL itself is unchanged and still started from the modules-loaded hook. |
| `android.hardware.secure_element-service.qti.rc` (`vendor.secure_element`) | ~ | Header added; `disabled` + `group nfc root` kept. |
| `android.hardware.security.keymint-service-qti.rc` (`vendor.keymint-qti`) | ~ | Header added; comment corrected: the HAL is started by `start_crypto_services.sh` (**not** by TWRP's `prepdecrypt.sh`, which AERA does not ship -- no `prepdecrypt.rc`/`prepdecrypt.sh` exists in either AERA reference tree). `disabled` kept -- the version properties must be fixed first. |
| `vendor.qti.hardware.vibrator.service.rc` (`vendor.qti.vibrator`) | ~ | Copyright header added (Qualcomm 2022). **Kept the HAPTIC/vibrator rc verbatim** (`late-init` chown/chmod of `/sys/class/qcom-haptics/primitive_duration`, `boot` chmod 0666/chown of `/dev/aac_richtap_dev`, `RICHTAP_DEVICE_PATH`, `ENV_RICHTAP_CONFIG_PATH=/odm/etc/aac_richtap.config`, `vendor.haptic.calibrate.done`). The service block stays `disabled`: **the vibrator service is started by `init.recovery.qcom.rc`'s module-loaded hook**, after the Meizu haptic modules from `AERA_LOAD_VENDOR_MODULES` are loaded. |
| `start_crypto_services.rc` | ~ | Header added; the service is started **by name** from `init.recovery.qcom.rc` (kept TWRP's `class early_hal`, which is inert here: AERA core only ever issues `class_start default` and `class_start hal`, `etc/init.rc:84-85`). |

### 3.4 `vendor/bin/start_crypto_services.sh`

Body compared line-by-line: **identical** to the TWRP file (`diff` on lines 2-61: no
differences). One change:

| TWRP | AERA port | Status | Reason |
|---|---|---|---|
| `#!/sbin/sh` | `#!/system/bin/sh` | ~ | AERA core `etc/init.rc:43-44` does `rmdir /sbin` then `symlink /system/bin /sbin`, so `/sbin/sh` = `/system/bin/sh` anyway; using the real path is correct and matches how the script is launched (`/vendor/bin/start_crypto_services.sh` is executed by init with the PATH from `init.rc:30`). Infiniti keeps `/sbin/sh` in its own `prepdecrypt.sh`; either works here. |

The script needs `strings`, `awk`, `getprop`, `resetprop`, `setprop`, `tee`, `sleep` --
all provided by AERA's recovery ramdisk (toybox + resetprop). `resetprop` is required:
AERA's Android.mk ships it when `AERA_INCLUDE_RESETPROP := true` (`Android.mk:635-638`).

---

## 4. `install_keyring` -- why it is gone (build-level side effect)

* TWRP's `init.recovery.qcom.rc:51` calls `install_keyring`. That is **not an AOSP init
  builtin**; it comes from TWRP's own `bootable/recovery/libinit` extension.
* AERA does **not** use AOSP `system/core`: `manifest/aera.xml:12` removes
  `platform/system/core` and line 27 adds `android_system_core` from the AERA remote.
  In `AERA-Recovery/android_system_core@aera-16.0` (commit `245ef44a`, `init/builtins.cpp`,
  keyword table lines ~1279-1345) the registered commands are the plain AOSP set --
  `installkey` (AOSP, no underscore) exists, `install_keyring` does not. AERA init is
  therefore a plain AOSP init build.
* Unknown commands are fatal at parse time (`Keyword: '<x>' not found in <file>`), so
  keeping the line would abort recovery's first-stage init -- this is a boot-stopper, not
  a cosmetic difference.
* What replaces it: AERA starts `keystore2` from its own `etc/init/keystore2.rc`
  (`on late-init: start keystore2`, workdir `/tmp/misc/keystore`) and pre-creates
  `/tmp/misc/keystore/` in `etc/init.rc:59-60`. The persistence/`/mnt/vendor/persist`
  handling that matters for decryption is still in `on fs` above.
* **UNVERIFIED**: whether AERA's keystore2 path covers the Meizu `/persist` keyring fully.
  First boot with an encrypted /data must be re-tested; the log to watch is
  `I:start_crypto_services.sh` in `/tmp/recovery.log` plus `init.svc.vendor.keymint-qti`.

**Build-level consequence for the .mk owner:** if the device `.mk`/BoardConfig inherits a
TWRP-style base that builds `libinit` (AOSP does not provide it), that module will not
exist in the AERA manifest. Do not reference `libinit` in `TARGET_RECOVERY_DEVICE_MODULES`
/ `PRODUCT_PACKAGES`.

---

## 5. Shipped but intentionally unstarted binaries

Present under `recovery/root/vendor/bin/hw/` (copied earlier from TWRP) but **no rc
declares them** -- same as in the TWRP tree, where nothing starts them either:

`android.hardware.gatekeeper-service-spu-qti`, `android.hardware.security.keymint-service-spu-qti`,
`android.hardware.weaver-service-spu-qti`.

Reason: `system.prop` sets `vendor.gatekeeper.disable_spu=true` /
`vendor.gatekeeper.is_security_level_spu=0`; the SPU (secure processing unit) path is
unused on the verified configuration. Adding rc files for them without any `start` would
just create services that never run, so they are intentionally omitted.

If space matters, their `lib64` dependencies (`libspu*.so`, `libGPQeSE.so`,
`libGPMTEEC_vendor.so`, `libjc_weaver_transport.so`, `android.hardware.weaver-V2-ndk.so`)
are candidates for removal -- that is a BLOBS.md decision, not this task.

---

## 6. Deliverable 5 (optional) -- `init.recovery.wifi.rc`: **not created**

Decision: skip. Evidence:

* TWRP ships no WLAN blobs at all (`TW_NO_NETWORK := true`, `BoardConfig.mk:181`;
  `docs/FACTS.md` sec.10.3) and no `init.recovery.wifi.rc`.
* The Meizu `AERA_LOAD_VENDOR_MODULES` set (touch/NFC/adsp_loader) contains no cnss/WLAN
  modules, and there is no `/vendor/etc/wifi` tree, no `WCNSS_qcom_cfg.ini`, no
  `wpa_supplicant` binary and no `libkeystore-engine-wifi-hidl.so` in this device tree.
* AERA only wires WLAN when `AERA_ENABLE_WLAN := 1` (`aera_build.mk:868-903`, needs
  `/vendor/etc/wifi/wpa_supplicant.conf`) and the driver bring-up (cnss2/icnss2
  `fs_ready` / `wpss_boot` sysfs node) is board-specific -- every reference value
  (`b0000000.qcom,cnss-peach`, `22800000.qcom,icnss`, `/sys/kernel/cnss/fs_ready`) is a
  different SoC's node. Guessing one for SM8650 would violate the "no invented sysfs
  paths" rule.
* Consequence: keep `AERA_ENABLE_WLAN` unset in BoardConfig for v1. If WLAN is added later
  it needs: the `vendor_dlkm` WLAN `.ko` set in `AERA_POST_DECRYPT_MODULES`, the
  `vendor/etc/wifi/*` blobs, `libkeystore-engine-wifi-hidl.so`, and the real cnss2 sysfs
  node read from this device's `/vendor/etc/init/hw/init.qcom.rc` or DT.

---

## 7. Build-level preconditions this port depends on (for the .mk owner)

1. `AERA_EXCLUDE_DEFAULT_USB_INIT := true` -- otherwise AERA core's own
   `etc/init.recovery.usb.rc` (sysfs `android_usb` gadget, `sys.usb.configfs 0`) is
   installed next to this one and both try to own the gadget. (AERA `Android.mk:700-702`
   only registers `init.recovery.usb.rc` in `TWRP_REQUIRED_MODULES` when this is not
   `true`; wiki flag `AERA_EXCLUDE_DEFAULT_USB_INIT`.)
2. `system.prop` must keep `ro.recovery.usb.vid=18D1`,
   `ro.recovery.usb.adb.pid=D001`, `ro.recovery.usb.fastboot.pid=4EE0` (see sec.3.2).
   Keep `ro.adb.secure=0` as in TWRP.
3. `AERA_INCLUDE_CRYPTO` / `AERA_INCLUDE_CRYPTO_FBE` / `AERA_INCLUDE_FBE_METADATA_DECRYPT`
   / `AERA_USE_FSCRYPT_POLICY := 2` (AERA names of TWRP's `TW_INCLUDE_CRYPTO*`), plus
   `AERA_INCLUDE_RESETPROP`/`AERA_INCLUDE_LIBRESETPROP` for the crypto script.
4. `AERA_INCLUDE_FASTBOOTD := true` (the `vendor.fastboot-default` rc is useless without it).
5. `AERA_SUPPORT_INPUT_AIDL_HAPTICS := true` and the Meizu Richtap sysfs/dev nodes kept in
   `vendor/etc/ueventd.rc` / `system/etc/ueventd.rc` (`/dev/aac_richtap_dev`, `/dev/hbtp_*`).
6. `TARGET_COPY_OUT_RECOVERY := recovery` (TWRP/AERA build semantics; defines the
   `$(TARGET_COPY_OUT_RECOVERY)/root/` destination used in sec.1) so the
   `find-copy-subdir-files` rule stages the tree correctly, and no `libinit` dependency
   (sec.4).
7. This tree still has no `Android.mk`/`Android.bp`/`device.mk`; init rc files are inert
   until those exist and copy `recovery/root/`.

---

## 8. Verification done for this task

* Every rc file re-parsed with a checker for the init language: unknown keywords, unknown
  service options, indentation/continuation, absolute `import` paths, BOM/CRLF/NUL bytes,
  non-ASCII characters. Result: 0 errors for all 11 files (allowed builtin list taken from
  the AERA `system/core` `builtins.cpp` keyword table).
* `vendor/bin/start_crypto_services.sh` diffed against the TWRP original: body identical,
  shebang only.
* `install_keyring` absence verified against AERA `system/core@aera-16.0` `init/builtins.cpp`.
* Nothing written outside: `recovery/root/init.recovery.qcom.rc`,
  `recovery/root/init.recovery.usb.rc`, `recovery/root/vendor/etc/init/*.rc` (9 files),
  `recovery/root/vendor/bin/start_crypto_services.sh`, and this document.

## 9. Unknowns / to re-verify on hardware

| # | Item | Marker |
|---|---|---|
| 1 | Decryption without `install_keyring` (sec.4) | UNVERIFIED |
| 2 | Adb id 18D1:D001 instead of TWRP's 18D1:4EE7 -- host tooling may show a different USB device string | UNVERIFIED (low risk) |
| 3 | Whether AERA sets `sys.usb.configfs` back to 1 in time for the `on fs` gadget block (AERA core sets 0 before `on fs`); TWRP relied on the same ordering and worked | UNVERIFIED (order copied verbatim from TWRP) |
| 4 | `vendor.charger` removal (sec.3.3) changes nothing at runtime unless the device is booted into charger mode | by design |
