#
# Copyright (C) 2026 The Android Open Source Project
# Copyright (C) 2026 AERA Recovery Project contributors
#
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2026 The OrangeFox Recovery Project
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Configure base.mk
$(call inherit-product, $(SRC_TARGET_DIR)/product/base.mk)

# Configure core_64_bit_only.mk
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)

# Configure virtual_ab compression.mk
$(call inherit-product, $(SRC_TARGET_DIR)/product/virtual_ab_ota/compression.mk)

# Configure emulated_storage.mk
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Configure twrp common.mk
$(call inherit-product, vendor/twrp/config/common.mk)

# Configure full_base_telephony.mk
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)


# Inherit from the m2461 device configuration
$(call inherit-product, device/meizu/m2461/device.mk)

# Release name
# Kernel cmdline / vendor_dlkm build.prop report "meizu21" / "meizu_21_CN".
PRODUCT_RELEASE_NAME := meizu21

## Device identifier
PRODUCT_DEVICE := m2461
PRODUCT_NAME := twrp_m2461
PRODUCT_BRAND := meizu
PRODUCT_MODEL := Meizu 21
PRODUCT_MANUFACTURER := meizu

# Assert
# Keep both identities the device actually reports so the installer accepts
# Flyme builds that expose either codename.
TARGET_OTA_ASSERT_DEVICE := $(PRODUCT_RELEASE_NAME),m2461

# Build fingerprint / build description
# Flyme 12.6 (Android 16 base). Only the prefix is verified from the extracted
# vendor_dlkm build.prop; the trailing build id/timestamp is a clearly marked
# placeholder. UNVERIFIED - replace the placeholder id below with the real
# build id before publishing an official build.
FLYME_BUILD_ID := 0000000000000

BUILD_FINGERPRINT := meizu/meizu_21_CN/meizu21:16/BQ2A.251016.001-BP2A.250605.031.A3/$(FLYME_BUILD_ID):user/release-keys

PRODUCT_BUILD_PROP_OVERRIDES += \
    PRIVATE_BUILD_DESC="meizu21-user 16 BQ2A.251016.001-BP2A.250605.031.A3 $(FLYME_BUILD_ID) release-keys"

# Theme
# The TWRP tree aligns and offsets the status bar through TW_STATUS_ICONS_ALIGN,
# TW_Y_OFFSET and TW_H_OFFSET; aera_config.mk knows those as
# AERA_STATUS_ICONS_ALIGN, AERA_Y_OFFSET and AERA_H_OFFSET, but no documented
# AERA_* flag covers them, so they are not used. AERA derives the status layout
# from AERA_STATUS_H and the status indents in BoardConfig.mk instead.
#
