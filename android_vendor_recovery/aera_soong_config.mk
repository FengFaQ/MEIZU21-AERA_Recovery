# AERA Soong build-variable bridge
#
# Soong (the .go *_defaults under bootable/recovery) reads established backend
# feature names through the "twrpVarsPlugin" SOONG_CONFIG namespace. Public
# AERA_* device configuration is translated by bootable/recovery/aera_config.mk
# before this bridge runs.
#
# This list lives in vendor/recovery (AERA-owned) rather than in
# vendor/twrp/config/BoardConfigSoong.mk so that the upstream TWRP config stays
# untouched. It is included from build/make/core/config.mk, right after
# BoardConfigTWRP.mk (which creates the twrpVarsPlugin namespace and runs after
# the device BoardConfig, so the backend values are already set by the time we read
# them below).

# Export the translated backend flags and native AERA values needed by Soong.
# getMakeVars() in the recovery *_defaults reads exactly these names.
$(call add_soong_config_var,twrpVarsPlugin,\
    AERA_ENABLE_ROOT_MANAGER \
    AERA_UI_ADAPTIVE_RESOLUTION \
    AERA_SCREEN_H \
    AERA_STATUS_H \
    AERA_STATUS_INDENT_LEFT \
    AERA_STATUS_INDENT_RIGHT \
    AERA_DEFAULT_LANGUAGE \
    AERA_EXTRA_LANGUAGES \
    TW_SUPPORT_INPUT_FF_HAPTICS \
    OF_ENABLE_WLAN \
    OF_ENABLE_LAB \
    OF_LANDSCAPE_MODE \
    OF_SUPPORT_OZIP_DECRYPTION \
    OF_USE_NANO_EDITOR \
    OF_ALLOW_EARLY_SETTINGS_LOAD \
    OF_SETTINGS_ROOT_DIRECTORY \
    OF_MISCELLANEOUS_ROOT_DIRECTORY \
    OF_USE_DATA_RECOVERY_FOR_SETTINGS \
    OF_USE_MEIZU_TOUCH_MAPPING)
