LOCAL_PATH := $(call my-dir)

# Optional ARM64 payload. Keep it compressed inside the unchanged LZ4 ramdisk.
# The device product opts in by including aera-browser-runtime.
ifeq ($(TARGET_ARCH),arm64)
include $(CLEAR_VARS)
LOCAL_MODULE := aera-browser-runtime
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_STEM := runtime.xz
LOCAL_SRC_FILES := prebuilt/runtime.xz
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/system/etc/aera-browser
include $(BUILD_PREBUILT)
endif
