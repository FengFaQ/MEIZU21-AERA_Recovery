# AERA Recovery Project manifest

This is the Android 16 source manifest for AERA Recovery Project. It combines
the normal Android, LineageOS, and TeamWin sources with the public AERA forks
required to build AERA recovery for supported devices.

All AERA-owned projects are hosted in the
[`AERA-Recovery`](https://github.com/AERA-Recovery) GitHub organization.
Downloading and syncing use HTTPS; no GitHub account or SSH key is required.
The manifest keeps a separate SSH push URL for contributors. Pushing still
requires an authorized SSH key and write access to the target repository.

## Sync

```bash
mkdir -p ~/android/AERA_16.0
cd ~/android/AERA_16.0
repo init -u https://github.com/AERA-Recovery/android_manifest.git -b aera-16.0
repo sync -c --force-sync --no-clone-bundle --no-tags -j$(nproc)
```

The manifest intentionally keeps unchanged platform dependencies on their
normal upstream remotes. AERA repositories are used for every recovery fork and
for every source tree carrying an AERA-specific change.

Device trees are not included in the common manifest. Clone only the tree for
the device you intend to build.

## Build for OnePlus 13 (`dodge`)

```bash
cd ~/android/AERA_16.0
git clone -b aera-16.0 \
  https://github.com/AERA-Recovery/android_device_oneplus_dodge-AERA.git \
  device/oneplus/dodge
source build/envsetup.sh
lunch twrp_dodge-bp2a-eng
mka adbd recoveryimage
```

The recovery image is written under `out/target/product/dodge/`.

## Build for OnePlus 15 (`infiniti`)

```bash
cd ~/android/AERA_16.0
git clone -b aera-16.0 \
  https://github.com/AERA-Recovery/android_device_oneplus_infiniti-AERA.git \
  device/oneplus/infiniti
source build/envsetup.sh
lunch twrp_infiniti-bp2a-eng
mka adbd recoveryimage
```

The recovery image is written under `out/target/product/infiniti/`.

## Manifest layout

- `default.xml` pins the Android platform source set.
- `twrp-default.xml` overlays the recovery build system and dependencies.
- `aera.xml` contains the shared AERA recovery stack. Device trees are added
  separately for each build.
- `remove-minimal.xml` removes projects that are unnecessary for recovery.

## Contribution policy

Preserve original authorship when importing or adapting changes. Keep device-
specific work in its device tree and reusable recovery behavior in the
appropriate shared AERA project.
