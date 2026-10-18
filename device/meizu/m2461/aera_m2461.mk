#
#	This file is part of the AERA Recovery Project
# 	Copyright (C) 2026 The AERA Recovery Project contributors
#
#	AERA Recovery is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	any later version.
#
#	AERA Recovery is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
# 	This software is released under GPL version 3 or any later version.
#	See <http://www.gnu.org/licenses/>.
#
# 	Please maintain this if you use this script or any part of it
#
# AERA-specific tuning for the Meizu 21 (m2461). Every AERA_* switch that is
# actually set here is a documented flag of the AERA 16.0 developer wiki, except
# AERA_DYNAMIC_FULL_SIZE, which is flagged inline at its assignment.
#

# --- Crypto / FBE ---------------------------------------------------------
# The verified TWRP tree decrypted userdata on this device with the full FBE +
# metadata-decryption path, so enable both AERA switches explicitly instead of
# relying on the implicit path through AERA_INCLUDE_CRYPTO.
AERA_INCLUDE_CRYPTO_FBE := true
AERA_INCLUDE_FBE_METADATA_DECRYPT := true

# --- Build identity -------------------------------------------------------
# Maintainer string shown in the recovery metadata.
AERA_MAINTAINER := FengFaQ
# Revision string: the verified TWRP tree shows "Meizu_21" via TW_DEVICE_VERSION
# (aera_config.mk knows it as AERA_DEVICE_VERSION). No documented AERA_* flag
# covers it, so it is NOT set. UNVERIFIED - add it back only together with a
# documented equivalent.

# --- Display / native UI metrics -----------------------------------------
# 1080 x 2340 panel of the Meizu 21.
AERA_SCREEN_H := 2340
# UNVERIFIED - starting value, tune on device.
AERA_STATUS_H := 120
# UNVERIFIED - starting values, tune on device.
AERA_STATUS_INDENT_LEFT := 90
AERA_STATUS_INDENT_RIGHT := 90

# --- Storage / partition handling ----------------------------------------
# A/B device with a real, dedicated recovery partition.
AERA_AB_DEVICE_WITH_RECOVERY_PARTITION := 1
# userdata is f2fs, metadata is f2fs: force f2fs on format.
AERA_FORCE_DATA_FORMAT_F2FS := 1
# metadata holds the metadata encryption key directory - wipe it together with
# userdata (requires AERA_INCLUDE_CRYPTO + BOARD_USES_METADATA_PARTITION).
AERA_WIPE_METADATA_AFTER_DATAFORMAT := 1
# The super group is 9663676416 bytes; used for the super-partition resizers.
# NOTE: this name is not one of the 142 documented flag names. It is required by
# the task brief and aera_config.mk does map it (AERA_DYNAMIC_FULL_SIZE ->
# OF_DYNAMIC_FULL_SIZE), so the backend does consume it.
AERA_DYNAMIC_FULL_SIZE := 9663676416
# Full logical-partition tooling (needs PRODUCT_USE_DYNAMIC_PARTITIONS).
AERA_ENABLE_ALL_PARTITION_TOOLS := 1

# --- Ramdisk / kernel -----------------------------------------------------
# LZ4 ramdisk, and never fall back to building a kernel from source.
AERA_USE_LZ4_COMPRESSION := 1
AERA_FORCE_PREBUILT_KERNEL := 1

# --- Installer behaviour --------------------------------------------------
# Meizu ROMs are not Treble-verified packages; skip the compatibility gate.
AERA_NO_TREBLE_COMPATIBILITY_CHECK := 1
# Show the "Keep current ABL" preference (Abl/bootloader preservation).
AERA_ENABLE_ABL_PRESERVATION := 1

# --- Wi-Fi ----------------------------------------------------------------
# Must be ON, even though this device has no usable WLAN in recovery.
#
# bootable/recovery/Android.mk gates aera_secrets/aera_secrets.cpp behind
# `ifeq ($(OF_ENABLE_WLAN),1)`, but aeraui/platform/aera_backend.cpp calls
# AeraSecrets::UsePrimaryWlan() (line 3114) and AeraSecrets::SetNasPrivateKey()
# (line 3966) unconditionally. With the switch off, the build fails at link
# time:
#   ld.lld: error: undefined symbol: AeraSecrets::UsePrimaryWlan(...)
#   ld.lld: error: undefined symbol: AeraSecrets::SetNasPrivateKey(...)
# That is an inconsistency in the AERA sources (the UI references symbols that
# only exist in the WLAN build), so the switch has to be on to link at all.
#
# Runtime note: the verified TWRP tree sets TW_NO_NETWORK because Meizu's CNSS
# drivers live in vendor_dlkm and no WLAN firmware ships in the ramdisk, so the
# Wi-Fi panel will most likely report "not available" on device. That is
# acceptable - it only costs a few hundred KB and is required for the build.
AERA_ENABLE_WLAN := 1
