# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0

# Public device configuration uses AERA_* exclusively. The recovery backend
# still consumes its established internal variable names, so keep that detail
# behind this translation layer instead of exposing it in device trees.
AERA_ENABLE_ROOT_MANAGER ?= 0

define aera-map-config
ifneq ($$(origin AERA_$(1)),undefined)
$(2) := $$(AERA_$(1))
endif
endef

$(eval $(call aera-map-config,AB_DEVICE_WITH_RECOVERY_PARTITION,OF_AB_DEVICE_WITH_RECOVERY_PARTITION))
$(eval $(call aera-map-config,ADVANCED_SECURITY,OF_ADVANCED_SECURITY))
$(eval $(call aera-map-config,ALLOW_DISABLE_NAVBAR,OF_ALLOW_DISABLE_NAVBAR))
$(eval $(call aera-map-config,BIND_MOUNT_SDCARD_ON_FORMAT,OF_BIND_MOUNT_SDCARD_ON_FORMAT))
$(eval $(call aera-map-config,BLOCK_OPERATIONS_AFTER_ROM_FLASH,OF_BLOCK_OPERATIONS_AFTER_ROM_FLASH))
$(eval $(call aera-map-config,CLOCK_POS,OF_CLOCK_POS))
$(eval $(call aera-map-config,DEFAULT_TIMEZONE,OF_DEFAULT_TIMEZONE))
$(eval $(call aera-map-config,DISABLE_ORS_AUTO_REBOOT,OF_DISABLE_ORS_AUTO_REBOOT))
$(eval $(call aera-map-config,DISPLAY_FORMAT_FILESYSTEMS_DEBUG_INFO,OF_DISPLAY_FORMAT_FILESYSTEMS_DEBUG_INFO))
$(eval $(call aera-map-config,DYNAMIC_FULL_SIZE,OF_DYNAMIC_FULL_SIZE))
$(eval $(call aera-map-config,ENABLE_ALL_PARTITION_TOOLS,OF_ENABLE_ALL_PARTITION_TOOLS))
ifneq ($(origin AERA_ENABLE_FS_COMPRESSION),undefined)
  ifneq ($(filter 1 true,$(AERA_ENABLE_FS_COMPRESSION)),)
    TW_ENABLE_FS_COMPRESSION := true
    OF_ENABLE_FS_COMPRESSION := 1
  else
    TW_ENABLE_FS_COMPRESSION := false
    OF_ENABLE_FS_COMPRESSION := 0
  endif
endif
$(eval $(call aera-map-config,ENABLE_LPTOOLS,OF_ENABLE_LPTOOLS))
$(eval $(call aera-map-config,ENABLE_WLAN,OF_ENABLE_WLAN))
$(eval $(call aera-map-config,FLASHLIGHT_ENABLE,OF_FLASHLIGHT_ENABLE))
$(eval $(call aera-map-config,FL_PATH1,OF_FL_PATH1))
$(eval $(call aera-map-config,FORCE_CASEFOLDING,OF_FORCE_CASEFOLDING))
$(eval $(call aera-map-config,FORCE_DATA_FORMAT_F2FS,OF_FORCE_DATA_FORMAT_F2FS))
$(eval $(call aera-map-config,FORCE_PREBUILT_KERNEL,OF_FORCE_PREBUILT_KERNEL))
$(eval $(call aera-map-config,HIDE_NOTCH,OF_HIDE_NOTCH))
$(eval $(call aera-map-config,LOOP_DEVICE_ERRORS_TO_LOG,OF_LOOP_DEVICE_ERRORS_TO_LOG))
$(eval $(call aera-map-config,NO_RELOAD_AFTER_DECRYPTION,OF_NO_RELOAD_AFTER_DECRYPTION))
$(eval $(call aera-map-config,NO_TREBLE_COMPATIBILITY_CHECK,OF_NO_TREBLE_COMPATIBILITY_CHECK))
$(eval $(call aera-map-config,OPTIONS_LIST_NUM,OF_OPTIONS_LIST_NUM))
$(eval $(call aera-map-config,QUICK_BACKUP_LIST,OF_QUICK_BACKUP_LIST))
$(eval $(call aera-map-config,SKIP_FBE_DECRYPTION,OF_SKIP_FBE_DECRYPTION))
$(eval $(call aera-map-config,UNBIND_SDCARD_F2FS,OF_UNBIND_SDCARD_F2FS))
$(eval $(call aera-map-config,UNMOUNT_SDCARDS_BEFORE_REBOOT,OF_UNMOUNT_SDCARDS_BEFORE_REBOOT))
$(eval $(call aera-map-config,USE_AIDL_BOOT_CONTROL,OF_USE_AIDL_BOOT_CONTROL))
$(eval $(call aera-map-config,USE_DMCTL,OF_USE_DMCTL))
$(eval $(call aera-map-config,USE_GREEN_LED,OF_USE_GREEN_LED))
$(eval $(call aera-map-config,USE_LOCKSCREEN_BUTTON,OF_USE_LOCKSCREEN_BUTTON))
$(eval $(call aera-map-config,USE_LZ4_COMPRESSION,OF_USE_LZ4_COMPRESSION))
$(eval $(call aera-map-config,USE_LZMA_COMPRESSION,OF_USE_LZMA_COMPRESSION))
$(eval $(call aera-map-config,WIPE_METADATA_AFTER_DATAFORMAT,OF_WIPE_METADATA_AFTER_DATAFORMAT))
$(eval $(call aera-map-config,WORKAROUND_BACKUP_BUG,OF_WORKAROUND_BACKUP_BUG))

# Public names must describe AERA behavior without carrying legacy product
# names. These map to the established backend switches only at this boundary.
$(eval $(call aera-map-config,COMPATIBILITY_MODE,OF_TWRP_COMPATIBILITY_MODE))
$(eval $(call aera-map-config,INCLUDE_RECOVERY_INJECTION,TW_INCLUDE_INJECTTWRP))
$(eval $(call aera-map-config,SKIP_LEGACY_PATCH_PROCESS,OF_SKIP_ORANGEFOX_PROCESS))
ifneq ($(origin AERA_NO_REFLASH_CURRENT_RECOVERY),undefined)
  ifneq ($(filter 1 true,$(AERA_NO_REFLASH_CURRENT_RECOVERY)),)
    OF_NO_REFLASH_CURRENT_ORANGEFOX := 1
    TW_NO_FLASH_CURRENT_TWRP := true
  else
    OF_NO_REFLASH_CURRENT_ORANGEFOX := 0
    TW_NO_FLASH_CURRENT_TWRP := false
  endif
endif

ifneq ($(origin AERA_USE_MEIZU_TOUCH_MAPPING),undefined)
  ifneq ($(filter 1 true,$(AERA_USE_MEIZU_TOUCH_MAPPING)),)
    OF_USE_MEIZU_TOUCH_MAPPING := 1
    TW_USE_MEIZU_TOUCH_MAPPING := true
  else
    OF_USE_MEIZU_TOUCH_MAPPING := 0
    TW_USE_MEIZU_TOUCH_MAPPING := false
  endif
endif

ifneq ($(origin AERA_USE_SAMSUNG_HAPTICS),undefined)
  ifneq ($(filter 1 true,$(AERA_USE_SAMSUNG_HAPTICS)),)
    OF_USE_SAMSUNG_HAPTICS := 1
    TW_USE_SAMSUNG_HAPTICS := true
  else
    OF_USE_SAMSUNG_HAPTICS := 0
    TW_USE_SAMSUNG_HAPTICS := false
  endif
endif

$(eval $(call aera-map-config,BATTERY_SYSFS_WAIT_SECONDS,TW_BATTERY_SYSFS_WAIT_SECONDS))
$(eval $(call aera-map-config,BRIGHTNESS_PATH,TW_BRIGHTNESS_PATH))
$(eval $(call aera-map-config,CUSTOM_CPU_TEMP_PATH,TW_CUSTOM_CPU_TEMP_PATH))
$(eval $(call aera-map-config,DEFAULT_BRIGHTNESS,TW_DEFAULT_BRIGHTNESS))
$(eval $(call aera-map-config,DEVICE_VERSION,TW_DEVICE_VERSION))
$(eval $(call aera-map-config,EXCLUDE_APEX,TW_EXCLUDE_APEX))
$(eval $(call aera-map-config,EXCLUDE_DEFAULT_USB_INIT,TW_EXCLUDE_DEFAULT_USB_INIT))
$(eval $(call aera-map-config,FRAMERATE,TW_FRAMERATE))
$(eval $(call aera-map-config,HAS_EDL_MODE,TW_HAS_EDL_MODE))
$(eval $(call aera-map-config,INCLUDE_CRYPTO,TW_INCLUDE_CRYPTO))
$(eval $(call aera-map-config,INCLUDE_CRYPTO_FBE,TW_INCLUDE_CRYPTO_FBE))
$(eval $(call aera-map-config,INCLUDE_FASTBOOTD,TW_INCLUDE_FASTBOOTD))
$(eval $(call aera-map-config,INCLUDE_FBE_METADATA_DECRYPT,TW_INCLUDE_FBE_METADATA_DECRYPT))
$(eval $(call aera-map-config,INCLUDE_FUSE_EXFAT,TW_INCLUDE_FUSE_EXFAT))
$(eval $(call aera-map-config,INCLUDE_FUSE_NTFS,TW_INCLUDE_FUSE_NTFS))
$(eval $(call aera-map-config,INCLUDE_LIBRESETPROP,TW_INCLUDE_LIBRESETPROP))
$(eval $(call aera-map-config,INCLUDE_LOGCAT,TWRP_INCLUDE_LOGCAT))
$(eval $(call aera-map-config,INCLUDE_LPDUMP,TW_INCLUDE_LPDUMP))
$(eval $(call aera-map-config,INCLUDE_LPTOOLS,TW_INCLUDE_LPTOOLS))
$(eval $(call aera-map-config,INCLUDE_NTFS_3G,TW_INCLUDE_NTFS_3G))
$(eval $(call aera-map-config,INCLUDE_OMAPI,TW_INCLUDE_OMAPI))
$(eval $(call aera-map-config,INCLUDE_REPACKTOOLS,TW_INCLUDE_REPACKTOOLS))
$(eval $(call aera-map-config,INCLUDE_RESETPROP,TW_INCLUDE_RESETPROP))
$(eval $(call aera-map-config,INPUT_BLACKLIST,TW_INPUT_BLACKLIST))
$(eval $(call aera-map-config,LOAD_VENDOR_MODULES,TW_LOAD_VENDOR_MODULES))
$(eval $(call aera-map-config,LOAD_VENDOR_MODULES_EXCLUDE_GKI,TW_LOAD_VENDOR_MODULES_EXCLUDE_GKI))
$(eval $(call aera-map-config,MAX_BRIGHTNESS,TW_MAX_BRIGHTNESS))
$(eval $(call aera-map-config,NO_EXFAT_FUSE,TW_NO_EXFAT_FUSE))
$(eval $(call aera-map-config,NO_HAPTICS,TW_NO_HAPTICS))
$(eval $(call aera-map-config,NO_SCREEN_BLANK,TW_NO_SCREEN_BLANK))
$(eval $(call aera-map-config,POST_DECRYPT_MODULES,TW_POST_DECRYPT_MODULES))
$(eval $(call aera-map-config,SCREEN_BLANK_ON_BOOT,TW_SCREEN_BLANK_ON_BOOT))
$(eval $(call aera-map-config,SKIP_ADDITIONAL_FSTAB,TW_SKIP_ADDITIONAL_FSTAB))
$(eval $(call aera-map-config,STATUS_ICONS_ALIGN,TW_STATUS_ICONS_ALIGN))
$(eval $(call aera-map-config,SUPPORT_INPUT_AIDL_HAPTICS,TW_SUPPORT_INPUT_AIDL_HAPTICS))
$(eval $(call aera-map-config,SUPPORT_INPUT_AIDL_HAPTICS_FQNAME,TW_SUPPORT_INPUT_AIDL_HAPTICS_FQNAME))
$(eval $(call aera-map-config,SUPPORT_INPUT_FF_HAPTICS,TW_SUPPORT_INPUT_FF_HAPTICS))
$(eval $(call aera-map-config,THEME,TW_THEME))
$(eval $(call aera-map-config,USE_FSCRYPT_POLICY,TW_USE_FSCRYPT_POLICY))
$(eval $(call aera-map-config,USE_SERIALNO_PROPERTY_FOR_DEVICE_ID,TW_USE_SERIALNO_PROPERTY_FOR_DEVICE_ID))
$(eval $(call aera-map-config,USE_TOOLBOX,TW_USE_TOOLBOX))

# Remaining supported legacy configuration knobs. These aliases keep the
# backend implementation private while allowing future device trees to use
# the same suffix with the AERA_ namespace.
aera-legacy-of-options := \
    ALLOW_EARLY_SETTINGS_LOAD \
    CHECK_OVERWRITE_ATTEMPTS \
    CLASSIC_LEDS_FUNCTION \
    CURRENT_BRANCH \
    DEFAULT_KEYMASTER_VERSION \
    DISABLE_EXTRA_ABOUT_PAGE \
    DISABLE_MIUI_OTA_BY_DEFAULT \
    DISABLE_MIUI_SPECIFIC_FEATURES \
    DISABLE_OTA_MENU \
    DONT_KEEP_LOG_HISTORY \
    DONT_PATCH_ENCRYPTED_DEVICE \
    DONT_PATCH_ON_FRESH_INSTALLATION \
    DONT_SUBSTITUTE_PERMISSIONS \
    ENABLE_LAB \
    ENABLE_USB_STORAGE \
    FBE_METADATA_MOUNT_IGNORE \
    FIX_DECRYPTION_ON_DATA_MEDIA \
    FIX_OTA_UPDATE_MANUAL_FLASH_ERROR \
    FL_PATH2 \
    FORCE_CHECK_RAMDISK_CHECKSUM \
    FORCE_DATA_FORMAT_EXT4 \
    FORCE_MAGISKBOOT_BOOT_PATCH_MIUI \
    FORCE_USE_RECOVERY_FSTAB \
    INCREMENTAL_OTA_BACKUP_SUPER \
    KEEP_DM_VERITY \
    KEEP_DM_VERITY_FORCED_ENCRYPTION \
    KEEP_FORCED_ENCRYPTION \
    LANDSCAPE_MODE \
    MANUAL_ROOT_VENDOR_ERROR_FIX \
    MASK_GET_FOLDER_SIZE_READ_ERRORS \
    MISCELLANEOUS_ROOT_DIRECTORY \
    NO_ADDITIONAL_MIUI_PROPS_CHECK \
    NO_KEYMASTER_VER_4X \
    NO_MIUI_OTA_VENDOR_BACKUP \
    NO_MIUI_PATCH_WARNING \
    NO_REBOOT_FASTBOOT \
    NO_SPLASH_CHANGE \
    OTA_BACKUP_STOCK_BOOT_IMAGE \
    OTA_RES_CHECK_MICROSD \
    PATCH_AVB20 \
    RECOVERY_AB_FULL_REFLASH_RAMDISK \
    REDUCE_DECRYPTION_TIMEOUT \
    REFRESH_ENCRYPTION_PROPS_BEFORE_FORMAT \
    REPORT_HARMLESS_MOUNT_ISSUES \
    SCREEN_H \
    SETTINGS_ROOT_DIRECTORY \
    SKIP_DECRYPTED_ADOPTED_STORAGE \
    SKIP_FBE_DECRYPTION_SDKVERSION \
    SKIP_PREBUILT_MODULES \
    SPLASH_MAX_SIZE \
    STATUS_H \
    STATUS_INDENT_LEFT \
    STATUS_INDENT_RIGHT \
    SUPPORT_ALL_BLOCK_OTA_UPDATES \
    SUPPORT_OZIP_DECRYPTION \
    SUPPORT_VBMETA_AVB2_PATCHING \
    USE_DATA_RECOVERY_FOR_SETTINGS \
    USE_LEGACY_BATTERY_SERVICES \
    USE_LEGACY_TIME_FIXUP \
    USE_MAGISKBOOT \
    USE_MAGISKBOOT_FOR_ALL_PATCHES \
    USE_NANO_EDITOR \
    VAB_ORS_WIPE_DATA_IS_FORMAT \
    WLAN_AP
$(foreach option,$(aera-legacy-of-options), \
    $(eval $(call aera-map-config,$(option),OF_$(option))))

aera-legacy-tw-options := \
    ADDITIONAL_APEX_FILES \
    ALWAYS_RMRF \
    BACKUP_EXCLUSIONS \
    CLOCK_OFFSET \
    CUSTOM_BATTERY_PATH \
    CUSTOM_BATTERY_POS \
    CUSTOM_CLOCK_POS \
    CUSTOM_CPU_POS \
    CUSTOM_POWER_BUTTON \
    CUSTOM_THEME \
    DELAY_TOUCH_INIT_MS \
    DISABLE_TTF \
    ENABLE_BLKDISCARD \
    ENABLE_NETWORK \
    EVENT_LOGGING \
    EXCLUDE_BASH \
    EXCLUDE_ENCRYPTED_BACKUPS \
    EXCLUDE_LIBXML2 \
    EXCLUDE_LPDUMP \
    EXCLUDE_LPTOOLS \
    EXCLUDE_MTP \
    EXCLUDE_NANO \
    EXCLUDE_TZDATA \
    EXCLUDE_ZIP \
    EXTERNAL_STORAGE_MOUNT_POINT \
    EXTERNAL_STORAGE_PATH \
    FBIOPAN \
    FORCE_CPUINFO_FOR_DEVICE_ID \
    FORCE_KEYMASTER_VER \
    FORCE_USE_BUSYBOX \
    HAPTICS_TSPDRV \
    HAS_DOWNLOAD_MODE \
    HAS_NO_BOOT_PARTITION \
    H_OFFSET \
    IGNORE_MAJOR_AXIS_0 \
    IGNORE_MT_POSITION_0 \
    INCLUDE_7ZA \
    INCLUDE_BLOBPACK \
    INCLUDE_FB2PNG \
    INCLUDE_JPEG \
    INCLUDE_PYTHON \
    INCLUDE_ZSTD \
    INTERNAL_STORAGE_MOUNT_POINT \
    INTERNAL_STORAGE_PATH \
    LIBTAR_DEBUG \
    LOAD_PREBUILT_MODULES_AT_FIRST \
    LOAD_VENDOR_BOOT_MODULES \
    MTP_DEVICE \
    NEVER_UNMOUNT_SYSTEM \
    NEW_ION_HEAP \
    NO_BATT_PERCENT \
    NO_BIND_SYSTEM \
    NO_CPU_TEMP \
    NO_EXFAT \
    NO_FASTBOOT_BOOT \
    NO_LEGACY_PROPS \
    NO_REBOOT_BOOTLOADER \
    NO_REBOOT_RECOVERY \
    NO_SCREEN_TIMEOUT \
    NO_USB_STORAGE \
    OEM_BUILD \
    OVERRIDE_PROPS_ADDITIONAL_PARTITIONS \
    OVERRIDE_SYSTEM_PROPS \
    OZIP_DECRYPT_KEY \
    PREPARE_DATA_MEDIA_EARLY \
    QCOM_ATS_OFFSET \
    RECOVERY_ADDITIONAL_RELINK_BINARY_FILES \
    RECOVERY_ADDITIONAL_RELINK_LIBRARY_FILES \
    RECOVERY_ADDITIONAL_RELINK_VENDOR_HW_BINARY_FILES \
    ROTATION \
    ROUND_SCREEN \
    SDEXT_NO_EXT4 \
    SECONDARY_BRIGHTNESS_PATH \
    SUPPORT_INPUT_1_2_HAPTICS \
    SUPPORT_INPUT_AIDL_HAPTICS_FIX_OFF \
    SYSTEM_BUILD_PROP_ADDITIONAL_PATHS \
    TARGET_USES_QCOM_BSP \
    THEME_VERSION \
    USE_KEY_CODE_TOUCH_SYNC \
    USE_MODEL_HARDWARE_ID_FOR_DEVICE_ID \
    USES_VENDOR_LIBS \
    WHITELIST_INPUT \
    W_OFFSET \
    X_OFFSET \
    Y_OFFSET
$(foreach option,$(aera-legacy-tw-options), \
    $(eval $(call aera-map-config,$(option),TW_$(option))))

aera-legacy-twrp-options := \
    CUSTOM_KEYBOARD \
    REQUIRED_MODULES
$(foreach option,$(aera-legacy-twrp-options), \
    $(eval $(call aera-map-config,$(option),TWRP_$(option))))
