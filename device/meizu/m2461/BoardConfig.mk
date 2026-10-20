#
# Copyright (C) 2026 The Android Open Source Project
# Copyright (C) 2026 AERA Recovery Project contributors
#
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2026 The OrangeFox Recovery Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# AERA Recovery R1.0 device configuration for the Meizu 21 (m2461, SM8650
# "pineapple"). Every setting below is carried over from the flash-tested TWRP
# tree (adontoo/device_meizu_m2461-TWRP) unless a comment marks it UNVERIFIED.
# Values marked UNVERIFIED must be confirmed on the actual device before they
# are treated as facts.
#

DEVICE_PATH := device/meizu/m2461


# For building with minimal manifest
ALLOW_MISSING_DEPENDENCIES := true
BUILD_BROKEN_DUP_RULES := true
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

BUILD_BROKEN_NINJA_USES_ENV_VARS += RTIC_MPGEN
BUILD_BROKEN_PLUGIN_VALIDATION := soong-libaosprecovery_defaults soong-libguitwrp_defaults soong-libminuitwrp_defaults soong-vold_defaults

# Architecture
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_ABI2 :=
TARGET_CPU_VARIANT := generic

# Power
ENABLE_CPUSETS := true
ENABLE_SCHEDBOOST := true

# Battery
# The verified TWRP tree sets TW_CUSTOM_BATTERY_PATH and
# TW_BATTERY_SYSFS_WAIT_SECONDS; aera_config.mk knows the AERA_CUSTOM_BATTERY_PATH
# and AERA_BATTERY_SYSFS_WAIT_SECONDS spellings of both, but no documented AERA_*
# flag covers them, so the AERA defaults apply.

# Bootloader
PRODUCT_PLATFORM := pineapple
TARGET_BOOTLOADER_BOARD_NAME := $(PRODUCT_RELEASE_NAME)
TARGET_NO_BOOTLOADER := true

# Platform
TARGET_BOARD_PLATFORM := meizu_sm8650
TARGET_BOARD_PLATFORM_GPU := qcom-adreno750
QCOM_BOARD_PLATFORMS += meizu_sm8650

# Init
TARGET_PLATFORM_DEVICE_BASE := /devices/soc/

# Kernel
# The stock boot.img ships an uncompressed arm64 Image with a v4 header and
# 4096-byte pages. Keep the verified prebuilt kernel: a third-party kernel
# build bootlooped this device in the earlier GKI project.
TARGET_KERNEL_ARCH            := arm64
TARGET_KERNEL_HEADER_ARCH     := arm64
BOARD_KERNEL_IMAGE_NAME       := Image
BOARD_BOOT_HEADER_VERSION     := 4
BOARD_KERNEL_PAGESIZE         := 4096
TARGET_PREBUILT_KERNEL        := $(DEVICE_PATH)/prebuilt/kernel
BOARD_MKBOOTIMG_ARGS          += --header_version $(BOARD_BOOT_HEADER_VERSION)
BOARD_MKBOOTIMG_ARGS          += --pagesize $(BOARD_KERNEL_PAGESIZE)

# Ramdisk uses the LZ4 legacy format on this GKI device.
BOARD_RAMDISK_USE_LZ4 := true

# A/B
# This device is A/B *and* has a dedicated recovery partition.
BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE := true

AB_OTA_UPDATER := true
AB_OTA_PARTITIONS := \
    boot \
    init_boot \
    vendor_boot \
    dtbo \
    vbmeta \
    vbmeta_system \
    odm \
    product \
    system \
    system_ext \
    system_dlkm \
    vendor \
    vendor_dlkm

# Verified Boot
# AERA_AB_DEVICE_WITH_RECOVERY_PARTITION is intentionally NOT set here: a
# product makefile assignment would be clobbered by the BoardConfig parse
# order. It lives in aera_m2461.mk instead.
BOARD_AVB_ENABLE := true

# Partitions
BOARD_PROPERTY_OVERRIDES_SPLIT_ENABLED := true

# Workaround for error copying vendor files to recovery ramdisk
TARGET_COPY_OUT_VENDOR := vendor

TARGET_COPY_OUT_ODM := odm
BOARD_ODMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USES_VENDOR_DLKMIMAGE := true
TARGET_COPY_OUT_VENDOR_DLKM := vendor_dlkm
BOARD_VENDOR_DLKMIMAGE_FILE_SYSTEM_TYPE := ext4

# 100 MiB recovery partition of the stock layout.
# NOTE: AERA R1.0 (LVGL) is larger than TWRP, so the produced image size has to
# be checked against this budget after the first build.
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 104857600

# Dynamic partitions
# super is 9 GiB; every logical partition is EROFS.
BOARD_SUPER_PARTITION_SIZE := 9663676416
BOARD_SUPER_PARTITION_GROUPS := qti_dynamic_partitions
BOARD_QTI_DYNAMIC_PARTITIONS_SIZE := 9663676416
BOARD_QTI_DYNAMIC_PARTITIONS_PARTITION_LIST := system system_ext product vendor vendor_dlkm odm

BOARD_PARTITION_LIST := $(call to-upper, $(BOARD_QTI_DYNAMIC_PARTITIONS_PARTITION_LIST))
$(foreach p, $(BOARD_PARTITION_LIST), $(eval BOARD_$(p)IMAGE_FILE_SYSTEM_TYPE := erofs))
$(foreach p, $(BOARD_PARTITION_LIST), $(eval TARGET_COPY_OUT_$(p) := $(call to-lower, $(p))))

# File systems
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := f2fs
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USERIMAGES_USE_F2FS := true
BOARD_HAS_LARGE_FILESYSTEM := true
RECOVERY_SDCARD_ON_DATA := true
TARGET_USES_MKE2FS := true

# Crypto
# Qualcomm FBE with a dedicated metadata partition and fscrypt policy v2.
# AERA_INCLUDE_CRYPTO is the umbrella switch of the AERA flag catalogue. The
# TWRP tree additionally sets TW_INCLUDE_CRYPTO_FBE and
# TW_INCLUDE_FBE_METADATA_DECRYPT, for which the AERA flag catalogue lists no
# corresponding names; FBE is instead gated by BOARD_USES_QCOM_FBE_DECRYPTION
# and BOARD_USES_METADATA_PARTITION below.
BOARD_USES_METADATA_PARTITION := true
BOARD_USES_QCOM_FBE_DECRYPTION := true
AERA_INCLUDE_CRYPTO := true
AERA_USE_FSCRYPT_POLICY := 2

# Recovery
TARGET_RECOVERY_PIXEL_FORMAT := RGBX_8888
# AERA stores the fstab inside the recovery ramdisk overlay (recovery/root),
# which is copied verbatim to the ramdisk root at build time.
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/recovery/root/system/etc/recovery.fstab
AERA_INCLUDE_FASTBOOTD := true
# The verified TWRP tree sets TW_HAS_EDL_MODE := true. aera_config.mk maps
# AERA_HAS_EDL_MODE onto it, but no documented AERA_* flag covers it, so the
# EDL entry stays off for now.
AERA_SKIP_ADDITIONAL_FSTAB := true
AERA_UI_ADAPTIVE_RESOLUTION := true

# Extras
TARGET_SYSTEM_PROP += $(DEVICE_PATH)/system.prop

# Tool
AERA_INCLUDE_7ZA := true
AERA_INCLUDE_ZSTD := true
AERA_INCLUDE_LIBRESETPROP := true
AERA_INCLUDE_LPDUMP := true
AERA_INCLUDE_LPTOOLS := true
AERA_INCLUDE_REPACKTOOLS := true
AERA_INCLUDE_RESETPROP := true

# Debug
TARGET_USES_LOGD := true
AERA_INCLUDE_LOGCAT := true
TARGET_RECOVERY_DEVICE_MODULES += debuggerd
TARGET_RECOVERY_DEVICE_MODULES += strace
RECOVERY_BINARY_SOURCE_FILES += $(TARGET_OUT_EXECUTABLES)/debuggerd
RECOVERY_BINARY_SOURCE_FILES += $(TARGET_OUT_EXECUTABLES)/strace

# AERA display
# 1080 x 2340 AMOLED panel, 120 Hz. Backlight controls and their raw ranges
# are taken verbatim from the TWRP tree.
AERA_BRIGHTNESS_PATH := "/sys/class/backlight/panel0-backlight/brightness"
AERA_DEFAULT_BRIGHTNESS := 2048
AERA_MAX_BRIGHTNESS := 4095
# The verified TWRP tree sets TW_SCREEN_BLANK_ON_BOOT := true.
AERA_SCREEN_BLANK_ON_BOOT := true
# native AERA host metrics (1080-wide reference coordinate system).
# AERA_SCREEN_H is 2340 (native panel height). AERA_STATUS_H = 120 and the
# status indents below are reasonable starting values only: UNVERIFIED, tune
# them on the real panel in both clock modes.
AERA_SCREEN_H := 2340
# UNVERIFIED - starting value, tune on device.
AERA_STATUS_H := 120
# UNVERIFIED - starting value, tune on device.
AERA_STATUS_INDENT_LEFT := 90
# UNVERIFIED - starting value, tune on device.
AERA_STATUS_INDENT_RIGHT := 90

# AERA file system
AERA_ENABLE_FS_COMPRESSION := true
AERA_INCLUDE_NTFS_3G := true
AERA_NO_EXFAT_FUSE := true

# Version
PLATFORM_VERSION := 99.87.36
PLATFORM_VERSION_LAST_STABLE := $(PLATFORM_VERSION)
PLATFORM_SECURITY_PATCH := 2099-12-31
VENDOR_SECURITY_PATCH := $(PLATFORM_SECURITY_PATCH)
BOOT_SECURITY_PATCH := $(PLATFORM_SECURITY_PATCH)
# The "Meizu_21" revision string of the verified TWRP tree comes from
# TW_DEVICE_VERSION (aera_config.mk knows it as AERA_DEVICE_VERSION), but no
# documented AERA_* flag covers it, so it is not set here.

# Vibrator
# Meizu Richtap vibrator: the HAL binaries and init rc files ship inside the
# TWRP blob set (see recovery/root).
AERA_SUPPORT_INPUT_AIDL_HAPTICS := true

# Other AERA configurations
TARGET_RECOVERY_QCOM_RTC_FIX := true
AERA_CUSTOM_CPU_TEMP_PATH := "/sys/class/thermal/thermal_zone47/temp"
AERA_EXCLUDE_APEX := true
AERA_EXCLUDE_DEFAULT_USB_INIT := true
AERA_DEFAULT_LANGUAGE := zh_CN
AERA_EXTRA_LANGUAGES := true
AERA_USE_SERIALNO_PROPERTY_FOR_DEVICE_ID := true
AERA_INPUT_BLACKLIST := "hbtp_vm"

# Vendor ramdisk modules.
#
# Touch (goodix_ts), Meizu key/touch drivers, NFC and the ADSP loader come from
# the device's own vendor_boot, hence AERA_LOAD_VENDOR_BOOT_MODULES below.
#
# Wi-Fi was originally left out on the theory that the CNSS drivers live in
# vendor_dlkm and could not be loaded, which is why the shipped recovery had no
# wlan0 and the Wi-Fi panel stayed dead. The modules are now shipped inside
# this ramdisk (recovery/root/vendor/lib/modules/, extracted from the stock
# vendor_dlkm) and listed here, which is exactly how the AERA reference tree
# does it for OnePlus. Load order matters: cfg80211/mac80211 first, then the
# cnss prealloc/utils/ipc/nl helpers, then cnss2, and the platform driver last.
#
# Meizu 21 uses the Kiwi v2 WLAN chip (qca_cld3_kiwi_v2.ko); the reference tree
# uses peach_v2 for OnePlus. rfkill is built into the 6.1 kernel on this device
# and has no separate module.
AERA_LOAD_VENDOR_MODULES := "adsp_loader_dlkm.ko goodix_ts.ko cs_press_m68.ko cs_press_f71.ko nxp-nci.ko stm_st54se_gpio.ko stm_nfc_i2c.ko cfg80211.ko mac80211.ko cnss_prealloc.ko cnss_utils.ko cnss_plat_ipc_qmi_svc.ko cnss_nl.ko wlan_firmware_service.ko cnss2.ko qca_cld3_kiwi_v2.ko gsim.ko rmnet_mem.ko ipam.ko rmnet_wlan.ko"
AERA_LOAD_VENDOR_MODULES_EXCLUDE_GKI := true
AERA_LOAD_VENDOR_BOOT_MODULES := true

# The touch panel needs the Meizu coordinate fix-up.
# AERA maps this name to OF_USE_MEIZU_TOUCH_MAPPING + TW_USE_MEIZU_TOUCH_MAPPING
# in aera_config.mk and emits -DOF_USE_MEIZU_TOUCH_MAPPING=1 from
# aera_defaults.go, so the setting is fully consumed by the AERA build even
# though it is not one of the 142 names in the developer wiki.
AERA_USE_MEIZU_TOUCH_MAPPING := 1

# Panel refresh rate for the AERA UI.
# The verified TWRP spelling is TW_FRAMERATE := 120; aera_config.mk knows that
# knob as AERA_FRAMERATE, but no documented AERA_* flag covers it, so it is not
# used here. The documented knob is AERA_DRM_FIXED_REFRESH_RATE; it only takes
# effect when a matching 1080x2340 @ 120 Hz DRM mode exists (otherwise the
# preferred mode stays selected). UNVERIFIED - keep it off until the DRM mode
# list of this panel has been dumped on the device.
# AERA_DRM_FIXED_REFRESH_RATE := 120
