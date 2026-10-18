#
# Copyright (C) 2026 The Android Open Source Project
# Copyright (C) 2026 AERA Recovery Project contributors
#
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2026 The OrangeFox Recovery Project
# SPDX-License-Identifier: GPL-3.0-or-later
#

LOCAL_PATH := device/meizu/m2461

# Shipping API level
BOARD_SHIPPING_API_LEVEL := 34
PRODUCT_SHIPPING_API_LEVEL := 34
PRODUCT_TARGET_VNDK_VERSION := 34

# Dynamic partitions
PRODUCT_USE_DYNAMIC_PARTITIONS := true

# Enable Fuse Passthrough
PRODUCT_PROPERTY_OVERRIDES += persist.sys.fuse.passthrough.enable=true

# Logical-partition tooling.
# AERA copies lpmake/lpadd/lpflash/lpunpack/lptools/lpdump from the build
# output into the recovery ramdisk itself when PRODUCT_USE_DYNAMIC_PARTITIONS
# and the partition-tool switches are enabled (see the lptools block in
# bootable/recovery/prebuilt/Android.mk). Declaring these modules again in
# PRODUCT_PACKAGES would only duplicate that list for the recovery-only
# build, so the tree relies on the AERA copy path instead - the AERA reference
# tree also does not build them as product packages.
# No device-local AERA modules (audio bridge, GPU probe, WLAN MAC helper) are
# added: this tree ships no sources for them.

# OTA certs
# The AERA reference tree points PRODUCT_EXTRA_RECOVERY_KEYS at a device-local
# security/ directory. This tree does not ship one, so no extra keys are
# declared and the default recovery test keys are used.

# Recovery ramdisk overlay
# The build system copies <device tree>/recovery/root/ into the recovery ramdisk
# automatically once TARGET_RECOVERY_DEVICE_DIRS is not already populated by an
# inherited common tree. Clearing it guarantees our vendor HALs, init scripts
# and firmware end up in the ramdisk. (Same idiom as the AERA reference trees,
# e.g. device/xiaomi/garnet.)
TARGET_RECOVERY_DEVICE_DIRS :=
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/recovery/root/,$(TARGET_COPY_OUT_RECOVERY)/root/)

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += $(LOCAL_PATH)

# Recovery-specific settings
$(call inherit-product, $(LOCAL_PATH)/aera_m2461.mk)
#
