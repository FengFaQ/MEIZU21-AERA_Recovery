#!/bin/bash
#
#	This file is part of the OrangeFox Recovery Project
# 	Copyright (C) 2018-2026 The OrangeFox Recovery Project
#
#	OrangeFox is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	any later version.
#
#	OrangeFox is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
# 	This software is released under GPL version 3 or any later version.
#	See <http://www.gnu.org/licenses/>.
#
# 	Please maintain this if you use this script or any part of it
#
# ******************************************************************************
# 17 August 2026
#
# *** This script is for the AERA Recovery Project Android 16.0 manifest ***
#
# Optional device and build settings use the public AERA_* namespace. The
# compatibility bridge in bootable/recovery/aera_config.mk translates values
# still consumed by inherited recovery code.
#
#

# automatically use magiskboot - overrides anything to the contrary in device trees
# other methods for patching recovery/boot images are no longer supported
export OF_USE_MAGISKBOOT_FOR_ALL_PATCHES=1
export OF_USE_MAGISKBOOT=1
export AERA_INTERNAL_RELEASE=R1.0
# Legacy compatibility: the inherited recovery build system still consumes
# AERA_INTERNAL_RELEASE internally.
export AERA_INTERNAL_RELEASE="$AERA_INTERNAL_RELEASE"

# device name
AERA_DEVICE=$(cut -d'_' -f2 <<<$TARGET_PRODUCT)

# The name of this script
THIS_SCRIPT=$(basename $0)

# environment/build-var imports
FOXENV=/tmp/$AERA_DEVICE/fox_env.sh
if [ -f "$FOXENV" ]; then
   source "$FOXENV"
else
   echo "** WARNING: $FOXENV is not found. Your build vars will probably not be implemented. **"
   echo "** You need an up-to-date AERA patch for the AOSP 16.0 manifest. **"
fi

# whether to print extra debug messages
if [ -z "$AERA_BUILD_DEBUG_MESSAGES" ]; then
   export AERA_BUILD_DEBUG_MESSAGES="0"
elif [ "$AERA_BUILD_DEBUG_MESSAGES" = "1" ]; then
   export AERA_BUILD_DEBUG_MESSAGES="1"
   set -o xtrace
fi

# should we use an updated magiskboot binary?
UPDATED=""
if [ "$AERA_USE_UPDATED_MAGISKBOOT" = "1" ]; then
   UPDATED="_updated"
fi

# some colour codes
RED='\033[0;31m'
GREEN='\033[0;32m'
ORANGE='\033[0;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
GREY='\033[0;37m'
LIGHTGREY='\033[0;38m'
WHITEONBLACK='\033[0;40m'
WHITEONRED='\033[0;41m'
WHITEONGREEN='\033[0;42m'
WHITEONORANGE='\033[0;43m'
WHITEONBLUE='\033[0;44m'
WHITEONPURPLE='\033[0;46m'
NC='\033[0m'
TMP_SCRATCH=/tmp/fox_build_000tmp.txt
WORKING_TMP=/tmp/Fox_000_tmp

# make sure we know exactly which commands we are running
CP=/bin/cp
[ ! -x "$CP" ] && CP=cp

UUIDGEN=/usr/bin/uuidgen
[ ! -x "$UUIDGEN" ] && UUIDGEN=uuidgen

# exit function (cleanup first), and return status code
abort() {
  [ -d "$WORKING_TMP" ] && rm -rf "$WORKING_TMP"
  [ -f "$TMP_SCRATCH" ] && rm -f "$TMP_SCRATCH"
  exit "$1"
}

# whether a build var is enabled (accepts "1" or greater, and "true")
# used in AERA_CUSTOM_BINS_TO_SDCARD
function enabled() {
local s="$1"
  if [ -z "$s" -o "$s" = "0" -o "$s" = "false" ]; then
     echo "0"
     return
  fi

  if [ "$s" = "true" ]; then
     echo "1"
     return
  fi
 
  # accept whole numbers only
  if [[ ! "$s" =~ ^[0-9]+$ ]]; then
     echo "0"
     return
  fi

  if [ "$s" -gt "0" ]; then
     echo "1"
  else
     echo "0"
  fi
}

# file_getprop <file> <property>
file_getprop() {
  local F=$(grep -m1 "^$2=" "$1" | cut -d= -f2)
  echo $F | sed 's/ *$//g'
}

# size of file
filesize() {
  [ -z "$1" -o -d "$1" ] && { echo "0"; return; }
  [ ! -e "$1" -a ! -h "$1" ] && { echo "0"; return; }
  stat -c %s "$1"
}

# generate a randomised build id
generate_build_id() {
local cmd="$UUIDGEN -r"
  [ -z "$(which $UUIDGEN)" ] && cmd="cat /proc/sys/kernel/random/uuid"
  $cmd
}

# check out some incompatible settings
if [ "$(enabled $AERA_CUSTOM_BINS_TO_SDCARD)" = "1" ]; then
   if [ "$AERA_USE_GREP_BINARY" = "1" ]; then
      export AERA_USE_GREP_BINARY=0
      echo -e "${WHITEONRED}-- 'AERA_CUSTOM_BINS_TO_SDCARD': ignoring incompatible build var 'AERA_USE_GREP_BINARY' ... ${NC}"
   fi

   if [ "$AERA_USE_BASH_SHELL" = "1" -o "$AERA_ASH_IS_BASH" = "1" -o "$AERA_DYNAMIC_SAMSUNG_FIX" = "1" ]; then
	echo -e "${WHITEONRED}-- ERROR! 'AERA_CUSTOM_BINS_TO_SDCARD' is incompatible with AERA_USE_BASH_SHELL or AERA_ASH_IS_BASH or AERA_DYNAMIC_SAMSUNG_FIX !${NC}"
     	echo -e "${WHITEONRED}-- Sort out your build vars! Quitting ... ${NC}"
     	abort 97
   fi
fi

# export whatever has been passed on by build/core/Makefile (we expect at least 4 arguments)
if [ -n "$4" ]; then
   echo "#########################################################################"
   echo "Variables exported from build/core/Makefile:"
   echo "$@"
   export "$@"
   if [ "$AERA_VENDOR_CMD" = "Fox_Before_Recovery_Image" ]; then
      echo "# - save the vars that we might need later - " &> $TMP_SCRATCH
      echo "MKBOOTFS=\"$MKBOOTFS\"" >>  $TMP_SCRATCH
      echo "TARGET_OUT=\"$TARGET_OUT\"" >>  $TMP_SCRATCH
      echo "TARGET_RECOVERY_ROOT_OUT=\"$TARGET_RECOVERY_ROOT_OUT\"" >>  $TMP_SCRATCH
      echo "COMPRESSION_COMMAND=\"$COMPRESSION_COMMAND\"" >>  $TMP_SCRATCH
      echo "INTERNAL_KERNEL_CMDLINE=\"$INTERNAL_KERNEL_CMDLINE\"" >>  $TMP_SCRATCH
      echo "INTERNAL_RECOVERYIMAGE_ARGS='$INTERNAL_RECOVERYIMAGE_ARGS'" >>  $TMP_SCRATCH
      echo "INTERNAL_MKBOOTIMG_VERSION_ARGS=\"$INTERNAL_MKBOOTIMG_VERSION_ARGS\"" >>  $TMP_SCRATCH
      echo "BOARD_MKBOOTIMG_ARGS='$BOARD_MKBOOTIMG_ARGS'" >>  $TMP_SCRATCH
      echo "BOARD_USES_RECOVERY_AS_BOOT=\"$BOARD_USES_RECOVERY_AS_BOOT\"" >>  $TMP_SCRATCH
      echo "recovery_ramdisk=\"$recovery_ramdisk\"" >>  $TMP_SCRATCH
      echo "recovery_uncompressed_ramdisk=\"$recovery_uncompressed_ramdisk\"" >>  $TMP_SCRATCH
      echo "#" >>  $TMP_SCRATCH
   fi
   echo "#########################################################################"
else
   echo -e "${WHITEONRED}-- Build AERA: FATAL ERROR! ${NC}"
   echo -e "${WHITEONRED}-- You cannot build AERA without patching build/core/Makefile in the build system. Aborting! ${NC}"
   abort 100
fi

# extra check: do we have a properly patched build system?
if [ -z "$AERA_VENDOR_CMD" ]; then
   echo -e "${WHITEONRED}-- AERA build: Fatal ERROR! ${NC}"
   echo -e "${WHITEONRED}-- Your build system is not properly patched for AERA. Quitting ... ${NC}"
   abort 100
fi

# vendor_boot as recovery
IS_VENDOR_BOOT_RECOVERY=0
if [ "$AERA_VENDOR_BOOT_RECOVERY" = "1" -o "$BOARD_INCLUDE_RECOVERY_RAMDISK_IN_VENDOR_BOOT" = "true" -o "$BOARD_MOVE_RECOVERY_RESOURCES_TO_VENDOR_BOOT" = "true" -o -n "$INSTALLED_VENDOR_BOOTIMAGE_TARGET" ]; then
   IS_VENDOR_BOOT_RECOVERY=1
fi

# A/B
IS_AB_DEVICE=0
if [ "$AERA_AB_DEVICE" = "1" -o "$OF_AB_DEVICE" = "1" -o "$AB_OTA_UPDATER" = "true" -o "$BOARD_USES_RECOVERY_AS_BOOT" = "true" ]; then
   IS_AB_DEVICE=1
fi

# virtual A/B (VAB)
IS_VIRTUAL_AB_DEVICE=0
if [ "$AERA_VIRTUAL_AB_DEVICE" = "1" -o "$OF_VIRTUAL_AB_DEVICE" = "1" -o "$AERA_VENDOR_BOOT_RECOVERY" = "1" -o "$PRODUCT_VIRTUAL_AB_OTA" = "true" ]; then
   IS_VIRTUAL_AB_DEVICE=1
   IS_AB_DEVICE=1
fi

if [ "$IS_AB_DEVICE" = "1" ]; then
    if [ -n "$BOARD_BOOT_HEADER_VERSION" ]; then
       [ "$BOARD_BOOT_HEADER_VERSION" -gt 2 ] && IS_VIRTUAL_AB_DEVICE="1"
    fi
fi

# Disable the init.d addon for vAB devices
if [ "$IS_VIRTUAL_AB_DEVICE" = "1" ]; then
    echo -e "${GREEN}-- The device is a virtual A/B device  - disabling the init.d addon .... ${NC}"
    export AERA_DELETE_INITD_ADDON=1
fi

# Virtual A/B and vendor_boot recovery devices?
if  [ "$IS_VENDOR_BOOT_RECOVERY" = "1" ]; then
    COMPILED_IMAGE_FILE="vendor_boot.img"
elif [ "$BOARD_USES_RECOVERY_AS_BOOT" = "true" ]; then
    COMPILED_IMAGE_FILE="boot.img"
else
    COMPILED_IMAGE_FILE="recovery.img"
fi

# vanilla build
IS_VANILLA_BUILD=0
if [ "$AERA_VANILLA_BUILD" = "1" -o "$OF_VANILLA_BUILD" = "1" ]; then
   IS_VANILLA_BUILD=1
fi

# initd - disable by default
if [ -z "$AERA_DELETE_INITD_ADDON" ]; then
	export AERA_DELETE_INITD_ADDON=1
fi

RECOVERY_DIR="recovery"
AERA_VENDOR_PATH=vendor/$RECOVERY_DIR
#
if [ "$AERA_VENDOR_CMD" = "Fox_Before_Recovery_Image" ]; then
	echo -e "${RED}Building AERA Recovery Project...${NC}"
	AERA_WORK="$TARGET_RECOVERY_ROOT_OUT"
	AERA_RAMDISK="$TARGET_RECOVERY_ROOT_OUT"
	DEFAULT_PROP_ROOT="$TARGET_RECOVERY_ROOT_OUT/../../root/default.prop"
else
	AERA_WORK=$OUT/AERA_AIK
	AERA_RAMDISK="$AERA_WORK/ramdisk"
	DEFAULT_PROP_ROOT="$AERA_WORK/../root/default.prop"
fi
echo -e "${BLUE}-- Setting up environment variables${NC}"

# transitional after renaming of build vars
if [ -n "$OF_SAMSUNG_DEVICE" -a -z "$AERA_SAMSUNG_DEVICE" ]; then
   export AERA_SAMSUNG_DEVICE=$OF_SAMSUNG_DEVICE
   echo -e "${RED}OF_SAMSUNG_DEVICE has been deprecated. Use AERA_SAMSUNG_DEVICE ${NC}"
fi

if [ -n "$OF_DISABLE_UPDATEZIP" -a -z "$AERA_DISABLE_UPDATEZIP" ]; then
   export AERA_DISABLE_UPDATEZIP=$OF_DISABLE_UPDATEZIP
   echo -e "${RED}OF_DISABLE_UPDATEZIP has been deprecated. Use AERA_DISABLE_UPDATEZIP ${NC}"
fi

# default prop
if [ -n "$TARGET_RECOVERY_ROOT_OUT" -a -e "$TARGET_RECOVERY_ROOT_OUT/default.prop" ]; then
   DEFAULT_PROP="$TARGET_RECOVERY_ROOT_OUT/default.prop"
else
   [ -e "$AERA_RAMDISK/prop.default" ] && DEFAULT_PROP="$AERA_RAMDISK/prop.default" || DEFAULT_PROP="$AERA_RAMDISK/default.prop"
fi

# some things are changing in native Android 10.0 and higher devices
RAMDISK_SBIN=/sbin
RAMDISK_ETC=/etc
RAMDISK_SYSTEM_BIN=/system/bin
RAMDISK_SYSTEM_ETC=/system/etc
PROP_DEFAULT="$DEFAULT_PROP"

# identify the build SDK version (not used yet)
if [ -e "$AERA_RAMDISK/prop.default" ]; then
   BUILD_SDK=$(file_getprop "$AERA_RAMDISK/prop.default" "ro.build.version.sdk")
   PROP_DEFAULT="$AERA_RAMDISK/prop.default"
elif [ -e "$AERA_RAMDISK/default.prop" ]; then
   BUILD_SDK=$(file_getprop "$AERA_RAMDISK/default.prop" "ro.build.version.sdk")
   PROP_DEFAULT="$AERA_RAMDISK/default.prop"
else
   BUILD_SDK=$(file_getprop "$DEFAULT_PROP" "ro.build.version.sdk")
fi
[ -z "$BUILD_SDK" ] && BUILD_SDK=30

# there are too many prop files around!
if [ "$DEFAULT_PROP" != "$PROP_DEFAULT" ]; then
   if [ $(filesize $PROP_DEFAULT) -gt $(filesize $DEFAULT_PROP) ]; then
      DEFAULT_PROP=$PROP_DEFAULT
   fi
fi

# Release channel. This keeps the familiar recovery build-type model while
# allowing AERA to distinguish release maturity from project authorization.
if [ -z "$AERA_BUILD_TYPE" ]; then
   export AERA_BUILD_TYPE=Stable
fi
case "$AERA_BUILD_TYPE" in
   Alpha|Beta|Nightly|Stable) ;;
   *)
      echo "ERROR: AERA_BUILD_TYPE must be Alpha, Beta, Nightly, or Stable." >&2
      exit 1
      ;;
esac

# Build status is independent from the release channel.
if [ -z "$AERA_BUILD_STATUS" ]; then
   export AERA_BUILD_STATUS=Unofficial
fi
case "$AERA_BUILD_STATUS" in
   Official|Unofficial) ;;
   *)
      echo "ERROR: AERA_BUILD_STATUS must be Official or Unofficial." >&2
      exit 1
      ;;
esac

# build name
AERA_BUILD="$AERA_INTERNAL_RELEASE"
if [ -n "$AERA_MAINTAINER_PATCH_VERSION" ]; then
  AERA_BUILD=$AERA_BUILD"_"$AERA_MAINTAINER_PATCH_VERSION
fi

# variant
if [ -z "$AERA_VARIANT" ]; then
   export AERA_VARIANT="default"
fi

# sort out the out_name
AERA_PRODUCT_PREFIX="${AERA_PRODUCT_PREFIX:-AERA}"
# Keep AERA_PRODUCT_PREFIX as a downstream-compatible override.
AERA_PRODUCT_PREFIX="${AERA_PRODUCT_PREFIX:-$AERA_PRODUCT_PREFIX}"
if [ "$AERA_VARIANT" = "default" ]; then
   AERA_OUT_NAME="$AERA_PRODUCT_PREFIX-$AERA_BUILD-$AERA_BUILD_TYPE-$AERA_BUILD_STATUS-$AERA_DEVICE"
else
   AERA_OUT_NAME="$AERA_PRODUCT_PREFIX-${AERA_BUILD}_${AERA_VARIANT}-$AERA_BUILD_TYPE-$AERA_BUILD_STATUS-$AERA_DEVICE"
fi

RECOVERY_IMAGE="$OUT/$AERA_OUT_NAME.img"
TMP_VENDOR_PATH="$OUT/../../../../vendor/$RECOVERY_DIR"
DEFAULT_INSTALL_PARTITION="/dev/block/bootdevice/by-name/recovery" # !! DON'T change!!!

# target_arch - default to arm64
if [ -z "$TARGET_ARCH" ]; then
   echo "Arch not detected, using arm64"
   TARGET_ARCH="arm64"
fi

# tmp for "AERA_CUSTOM_BINS_TO_SDCARD"
AERA_BIN_tmp=$OUT/tmp_bin/AERAFiles

# alternative devices
if [ -z "$TARGET_DEVICE_ALT" ]; then
   if [ -n "$AERA_TARGET_DEVICES" ]; then
	export TARGET_DEVICE_ALT="$AERA_TARGET_DEVICES"
   elif [ -n "$OF_TARGET_DEVICES" ]; then
   	export TARGET_DEVICE_ALT="$OF_TARGET_DEVICES"
   fi
fi

# copy recovery.img/boot.img
[ -f $OUT/$COMPILED_IMAGE_FILE ] && $CP $OUT/$COMPILED_IMAGE_FILE $RECOVERY_IMAGE

# extreme reduction
if [ "$AERA_EXTREME_SIZE_REDUCTION" = "1" ]; then
   export AERA_DRASTIC_SIZE_REDUCTION=1
fi

# remove all extras if AERA_DRASTIC_SIZE_REDUCTION is defined
if [ "$AERA_DRASTIC_SIZE_REDUCTION" = "1" -a "$(enabled $AERA_CUSTOM_BINS_TO_SDCARD)" != "1" ]; then
   export AERA_USE_BASH_SHELL=0
   export AERA_ASH_IS_BASH=0
   export AERA_USE_TAR_BINARY=0
   export AERA_USE_SED_BINARY=0
   export AERA_USE_GREP_BINARY=0
   export AERA_USE_NANO_EDITOR=0
   export BUILD_2GB_VERSION=0
   export AERA_USE_XZ_UTILS=0
   export AERA_USE_ZSTD_BINARY=0
   export AERA_USE_LZ4_BINARY=0
   export AERA_USE_FSCK_EROFS_BINARY=0
   export AERA_USE_PATCHELF_BINARY=0
   export AERA_REMOVE_BASH=1
   export AERA_REMOVE_AAPT=1
   export AERA_REMOVE_ZIP_BINARY=1
   export AERA_EXCLUDE_NANO_EDITOR=1
   export AERA_REMOVE_BUSYBOX_BINARY=1
   [ -z "$AERA_COMPRESS_EXECUTABLES" ] && export AERA_COMPRESS_EXECUTABLES=1
fi

if [ -n "$AERA_SETTINGS_ROOT_DIRECTORY" -a -z "$AERA_MISCELLANEOUS_ROOT_DIRECTORY" ]; then
   export AERA_MISCELLANEOUS_ROOT_DIRECTORY="$AERA_SETTINGS_ROOT_DIRECTORY";
fi

if [ "$AERA_USE_DATA_RECOVERY_FOR_SETTINGS" = "1" ]; then
   export AERA_SETTINGS_ROOT_DIRECTORY="/data/recovery"
   export AERA_MISCELLANEOUS_ROOT_DIRECTORY="/data/recovery"
fi

# exports
export AERA_DEVICE TMP_VENDOR_PATH AERA_OUT_NAME AERA_RAMDISK AERA_WORK

# create working tmp
if [ "$AERA_VENDOR_CMD" = "Fox_Before_Recovery_Image" ]; then
   rm -rf $WORKING_TMP
   mkdir -p $WORKING_TMP
fi

# check whether the /etc/ directory is a symlink to /system/etc/
if [ -h "$AERA_RAMDISK/$RAMDISK_ETC" -a -d "$AERA_RAMDISK/$RAMDISK_SYSTEM_ETC" ]; then
   RAMDISK_ETC=$RAMDISK_SYSTEM_ETC
fi

# workaround for some Samsung bugs
if [ "$AERA_DYNAMIC_SAMSUNG_FIX" = "1" ]; then
   if [ "$AERA_VENDOR_CMD" = "Fox_Before_Recovery_Image" ]; then
      echo -e "${WHITEONGREEN} - Dealing with bugged Samsung exynos dynamic stuff - removing stuff... ${NC}"
      echo -e "${WHITEONGREEN} - Make sure that you are doing a clean build. ${NC}"
   fi
   export AERA_REMOVE_BASH=1
   export AERA_REMOVE_AAPT=1
   unset AERA_USE_BASH_SHELL
   unset AERA_ASH_IS_BASH
   unset AERA_USE_NANO_EDITOR
   unset AERA_USE_XZ_UTILS
   unset AERA_USE_TAR_BINARY
   unset AERA_USE_GREP_BINARY
   unset AERA_USE_ZSTD_BINARY
   unset AERA_USE_LZ4_BINARY
fi

# disable all nano editor stuff
if [ "$AERA_EXCLUDE_NANO_EDITOR" = "1" ]; then
   unset AERA_USE_NANO_EDITOR
fi

# ****************************************************
# --- embedded functions
# ****************************************************

# to save the build date, and (if desired) patch bugged alleged anti-rollback on some ROMs
Save_Build_Date() {
local DT="$1"
local F="$DEFAULT_PROP"

   grep -q "ro.build.date.utc=" $F && \
   	sed -i -e "s/ro.build.date.utc=.*/ro.build.date.utc=$DT/g" $F || \
   	echo "ro.build.date.utc=$DT" >> $F

   [ -n "$2" ] && DT="$2" # don't change the true bootimage build date
   grep -q "ro.bootimage.build.date.utc=" $F && \
   	sed -i -e "s/ro.bootimage.build.date.utc=.*/ro.bootimage.build.date.utc=$DT/g" $F || \
   	echo "ro.bootimage.build.date.utc=$DT" >> $F

}

# if there is an ALT device, cater for it in update-binary
Add_Target_Alt() {
local D="$AERA_TMP_WORKING_DIR"
local F="$D/META-INF/com/google/android/update-binary"
   if [ -n "$TARGET_DEVICE_ALT" ]; then
      sed -i -e "s/TARGET_DEVICE_ALT=.*/TARGET_DEVICE_ALT=\"$TARGET_DEVICE_ALT\"/" $F
   fi
}

# whether this is a system-as-root build
SAR_BUILD() {
  local C=$(file_getprop "$DEFAULT_PROP" "ro.build.system_root_image")
  [ "$C" = "true" ] && { echo "1"; return; }

  C=$(file_getprop "$DEFAULT_PROP" "ro.boot.dynamic_partitions")
  [ "$C" = "true" ] && { echo "1"; return; }

  C=$(cat "$AERA_RAMDISK/$RAMDISK_SYSTEM_ETC/twrp.fstab" 2>/dev/null | grep -s ^"/system_root")
  [ -n "$C" ] && { echo "1"; return; }

  C=$(cat "$AERA_RAMDISK/$RAMDISK_SYSTEM_ETC/recovery.fstab" 2>/dev/null | grep -s ^"/system_root")
  [ -n "$C" ] && { echo "1"; return; }

  [ -d "$AERA_RAMDISK/system_root/" ] && echo "1" || echo "0"
}

# expand a directory path
fullpath() {
local T1=$PWD
  [ -z "$1" ] && return
  [ ! -d "$1" ] && {
    echo "$1"
    return
  }
  cd "$1"
  local T2=$PWD
  cd $T1
  echo "$T2"
}

# expand
expand_vendor_path() {
  AERA_VENDOR_PATH=$(fullpath "$TMP_VENDOR_PATH")
  [ ! -d $AERA_VENDOR_PATH/installer ] && {
     local T="${BASH_SOURCE%/*}"
     T=$(fullpath $T)
     [ -x $T/$THIS_SCRIPT ] && AERA_VENDOR_PATH=$T
  }
}

# save build vars
save_build_vars() {
local F=$1
   export | grep "AERA_" > $F
   export | grep "OF_" >> $F
   sed -i '/AERA_BUILD_LOG_FILE/d' $F
   sed -i '/AERA_BUILD_DEVICE/d' $F
   sed -i '/AERA_LOCAL_CALLBACK_SCRIPT/d' $F
   sed -i '/AERA_MANIFEST_ROOT/d' $F
   sed -i '/AERA_PORTS_TMP/d' $F
   sed -i '/AERA_RAMDISK/d' $F
   sed -i '/AERA_WORK/d' $F
   sed -i '/AERA_VENDOR_DIR/d' $F
   sed -i '/AERA_VENDOR_CMD/d' $F
   sed -i '/AERA_VENDOR/d' $F
   sed -i '/OF_MAINTAINER/d' $F
   sed -i '/OLDPWD/d' $F
   sed -i '/ORANGEFOX_CALLING_CARD/d' $F
   sed -i "s/declare -x //g" $F
}


# create zip file
do_create_update_zip() {
local tmp=""
local F=""
local isVB_V3=0
local TDT=$(date "+%d %B %Y")
  echo -e "${BLUE}-- Creating the AERA zip installer ...${NC}"
  FILES_DIR=$AERA_VENDOR_PATH/RecoveryFiles
  INST_DIR=$AERA_VENDOR_PATH/installer

  # names of output zip file(s)
  ZIP_FILE=$OUT/$AERA_OUT_NAME.zip
  ZIP_FILE_GO=$OUT/$AERA_OUT_NAME"_lite.zip"
  echo "- Creating $ZIP_FILE for deployment ..."

  # clean any existing files
  rm -rf $AERA_TMP_WORKING_DIR
  rm -f $ZIP_FILE_GO $ZIP_FILE

  # recreate dir
  mkdir -p $AERA_TMP_WORKING_DIR
  cd $AERA_TMP_WORKING_DIR

  # create some others
  mkdir -p $AERA_TMP_WORKING_DIR/sdcard/AERA/Files
  mkdir -p $AERA_TMP_WORKING_DIR/META-INF/debug

  # copy busybox
#  $CP -p $AERA_VENDOR_PATH/Files/busybox .

  # copy documentation
  $CP -p $AERA_VENDOR_PATH/Files/INSTALL.txt .

  # copy recovery image/boot image to recovery.img in the zip
  $CP -p $RECOVERY_IMAGE ./recovery.img

  # vendor_boot as recovery - add the ramdisk image to the zip
  if [ "$IS_VENDOR_BOOT_RECOVERY" = "1" -a -x $AERA_VENDOR_PATH/tools/magiskboot ]; then
     local VBtmp=/tmp/VBOOT_stuff
     mkdir -p $VBtmp/
     $CP -p $RECOVERY_IMAGE $VBtmp/tmp.img
     $CP -p $AERA_VENDOR_PATH/Files/flash-* .
     cd $VBtmp/

     if [ -n "$BOARD_BOOT_HEADER_VERSION" ]; then
	if [ "$BOARD_BOOT_HEADER_VERSION" -lt 4 ]; then
		isVB_V3=1
	else
		[ -z "$AERA_INSTALLER_VENDOR_BOOT_RAMDISK_INSTALL" ] && export AERA_INSTALLER_VENDOR_BOOT_RAMDISK_INSTALL=1
	fi
     fi

     $AERA_VENDOR_PATH/tools/magiskboot unpack -n tmp.img
     F="vendor_ramdisk_recovery.cpio"; #v4+ header
     [ ! -f $F ] && F="vendor_ramdisk/recovery.cpio"; #v4+ header+latest magiskboot canary
     [ ! -f $F ] && F="ramdisk.cpio"; #v3 or earlier header
     if [ -f $F ]; then
	if [ "$F" = "vendor_ramdisk/recovery.cpio" ]; then
		$CP -p $F "$AERA_TMP_WORKING_DIR/vendor_ramdisk_recovery.cpio";
	else
		$CP -p $F $AERA_TMP_WORKING_DIR/$F
	fi
	# check for v3 header and compensate
     	if [ "$F" = "ramdisk.cpio" ]; then
	   isVB_V3=1
	   sed -i -e "s/vendor_ramdisk_recovery.cpio/$F/" $AERA_TMP_WORKING_DIR/flash-*
     	fi
     fi

     cd $AERA_TMP_WORKING_DIR
     rm -rf $VBtmp/
  fi # vendor_boot

  # copy the Samsung .tar file if it exists
  if [ -f $RECOVERY_IMAGE".tar" ]; then
     $CP -p $RECOVERY_IMAGE".tar" .
  fi

  # copy installer bins and script
  $CP -pr $INST_DIR/* .

  # Copy recovery extras to /sdcard/AERA/Files/.
  $CP -a $FILES_DIR/. sdcard/AERA/Files/

  # Copy any custom binaries to /sdcard/AERA/Files/bin/.
  if [ "$(enabled $AERA_CUSTOM_BINS_TO_SDCARD)" = "1" -a -d "$AERA_BIN_tmp/bin" ]; then
     chmod +x $AERA_BIN_tmp/bin/*
     $CP -a $AERA_BIN_tmp/. sdcard/AERA/Files/
     rm -rf $AERA_BIN_tmp
  fi

  # any local changes to a port's installer directory?
  if [ -n "$AERA_PORTS_INSTALLER" ] && [ -d "$AERA_PORTS_INSTALLER" ]; then
     $CP -pr $AERA_PORTS_INSTALLER/* .
  fi

  # patch update-binary (which is a script) to run only for the current device
  F="$AERA_TMP_WORKING_DIR/META-INF/com/google/android/update-binary"
  sed -i -e "s|^TARGET_DEVICE=.*|TARGET_DEVICE=\"$AERA_DEVICE\"|" $F

  # embed the release version
  sed -i -e "s/RELEASE_VER/$AERA_BUILD/" $F

  # embed the build date
  sed -i -e "s/TODAY/$TDT/" $F

  # embed the recovery partition
  if [ -n "$AERA_RECOVERY_INSTALL_PARTITION" ]; then
     echo -e "${RED}-- Changing the recovery install partition to \"$AERA_RECOVERY_INSTALL_PARTITION\" ${NC}"
     sed -i -e "s|^RECOVERY_PARTITION=.*|RECOVERY_PARTITION=\"$AERA_RECOVERY_INSTALL_PARTITION\"|" $F
  fi

  # embed the system partition
  if [ -n "$AERA_RECOVERY_SYSTEM_PARTITION" ]; then
     echo -e "${RED}-- Changing the recovery system partition to \"$AERA_RECOVERY_SYSTEM_PARTITION\" ${NC}"
     sed -i -e "s|^SYSTEM_PARTITION=.*|SYSTEM_PARTITION=\"$AERA_RECOVERY_SYSTEM_PARTITION\"|" $F
  fi

  # embed the VENDOR partition
  if [ -n "$AERA_RECOVERY_VENDOR_PARTITION" ]; then
     echo -e "${RED}-- Changing the recovery vendor partition to \"$AERA_RECOVERY_VENDOR_PARTITION\" ${NC}"
     sed -i -e "s|^VENDOR_PARTITION=.*|VENDOR_PARTITION=\"$AERA_RECOVERY_VENDOR_PARTITION\"|" $F
  fi

  # embed the BOOT partition
  if [ -n "$AERA_RECOVERY_BOOT_PARTITION" ]; then
     echo -e "${RED}-- Changing the recovery boot partition to \"$AERA_RECOVERY_BOOT_PARTITION\" ${NC}"
     sed -i -e "s|^BOOT_PARTITION=.*|BOOT_PARTITION=\"$AERA_RECOVERY_BOOT_PARTITION\"|" $F
  fi

  # embed the VENDOR_BOOT partition
  if [ -n "$AERA_RECOVERY_VENDOR_BOOT_PARTITION" ]; then
     echo -e "${RED}-- Changing the recovery vendor_boot partition to \"$AERA_RECOVERY_VENDOR_BOOT_PARTITION\" ${NC}"
     sed -i -e "s|^VENDOR_BOOT_PARTITION=.*|VENDOR_BOOT_PARTITION=\"$AERA_RECOVERY_VENDOR_BOOT_PARTITION\"|" $F
  fi

  # debug mode for the installer? (just for testing purposes - don't ship the recovery with this enabled)
  if [ "$AERA_INSTALLER_DEBUG_MODE" = "1" -o "$AERA_INSTALLER_DEBUG_MODE" = "true" ]; then
     echo -e "${WHITEONRED}-- Enabling debug mode in the zip installer! You must disable \"AERA_INSTALLER_DEBUG_MODE\" before release! ${NC}"
     sed -i -e "s|^AERA_INSTALLER_DEBUG_MODE=.*|AERA_INSTALLER_DEBUG_MODE=\"1\"|" $F
  fi

  # A/B devices
  if [ "$IS_AB_DEVICE" = "1" ]; then
     echo -e "${RED}-- A/B device - copying magiskboot to zip installer ... ${NC}"
     tmp=$AERA_RAMDISK/$RAMDISK_SBIN/magiskboot
     [ ! -e "$tmp" ] && tmp=$AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/magiskboot"$UPDATED"
     [ ! -e "$tmp" ] && tmp=/tmp/fox_build_tmp/magiskboot
     [ ! -e "$tmp" ] && {
       echo -e "${WHITEONRED}-- I cannot find magiskboot. Quitting! ${NC}"
       abort 200
     }
     $CP -pf $tmp ./magiskboot
     chmod 0755 ./magiskboot
     sed -i -e "s/^AERA_AB_DEVICE=.*/AERA_AB_DEVICE=\"1\"/" $F
  fi
  rm -rf /tmp/fox_build_tmp/

  # vendor_boot
  if [ "$IS_VENDOR_BOOT_RECOVERY" = "1" ]; then
     echo -e "${RED}-- Vendor_boot device - enabling vendor_boot mode for the installer ... ${NC}"
     sed -i -e "s/^AERA_VENDOR_BOOT_RECOVERY=.*/AERA_VENDOR_BOOT_RECOVERY=\"1\"/" $F
     sed -i -e "s/^AERA_VENDOR_BOOT_RECOVERY_V3_HDR=.*/AERA_VENDOR_BOOT_RECOVERY_V3_HDR=\"$isVB_V3\"/" $F

     if [ "$AERA_INSTALLER_VENDOR_BOOT_RAMDISK_INSTALL" = "1" ] ; then
     	echo -e "${RED}-- Vendor_boot: - enabling vendor_boot ramdisk flash mode for the installer ... ${NC}"
	sed -i -e "s/^AERA_INSTALLER_VENDOR_BOOT_RAMDISK_INSTALL=.*/AERA_INSTALLER_VENDOR_BOOT_RAMDISK_INSTALL=\"1\"/" $F
     fi
  fi

  # virtual A/B
  if [ "$IS_VIRTUAL_AB_DEVICE" = "1" ]; then
     echo -e "${RED}-- Saving the vAB flag ... ${NC}"
     sed -i -e "s/^AERA_VIRTUAL_AB_DEVICE=.*/AERA_VIRTUAL_AB_DEVICE=\"1\"/" $F
  fi

  # whether to enable magisk 24+ patching of vbmeta
  if [ "$AERA_PATCH_VBMETA_FLAG" = "1" -o "$OF_PATCH_VBMETA_FLAG" = "1" ]; then
     echo -e "${RED}-- Enabling PATCHVBMETAFLAG for the installation... ${NC}"
     sed -i -e "s/^AERA_PATCH_VBMETA_FLAG=.*/AERA_PATCH_VBMETA_FLAG=\"1\"/" $F
  fi

  # Reset Settings
  if [ "$AERA_RESET_SETTINGS" = "disabled" ]; then
     echo -e "${WHITEONRED}-- Instructing the zip installer to NOT reset AERA settings (NOT recommended!) ... ${NC}"
     sed -i -e "s/^AERA_RESET_SETTINGS=.*/AERA_RESET_SETTINGS=\"disabled\"/" $F
  fi

  # skip all patches ?
  if [ "$IS_VANILLA_BUILD" = "1" ]; then
     echo -e "${RED}-- This build will skip all AERA patches ... ${NC}"
     sed -i -e "s/^AERA_VANILLA_BUILD=.*/AERA_VANILLA_BUILD=\"1\"/" $F
  fi

  # Use the configured AERA home for add-ons, logs, and backups.
  if [ -n "$AERA_MISCELLANEOUS_ROOT_DIRECTORY" ]; then
     echo -e "${RED}-- This build will use $AERA_MISCELLANEOUS_ROOT_DIRECTORY for its stuff ... ${NC}"
     sed -i -e "s|^AERA_MISCELLANEOUS_ROOT_DIRECTORY=.*|AERA_MISCELLANEOUS_ROOT_DIRECTORY=\"$AERA_MISCELLANEOUS_ROOT_DIRECTORY\"|" $F
  fi

  # save settings root
  if [ -n "$AERA_SETTINGS_ROOT_DIRECTORY" ]; then
     echo -e "${RED}-- This build will use $AERA_SETTINGS_ROOT_DIRECTORY for its settings ... ${NC}"
     sed -i -e "s|^AERA_SETTINGS_ROOT_DIRECTORY=.*|AERA_SETTINGS_ROOT_DIRECTORY=\"$AERA_SETTINGS_ROOT_DIRECTORY\"|" $F
  fi

  # disable auto-reboot after installing AERA?
  if [ "$AERA_INSTALLER_DISABLE_AUTOREBOOT" = "1" ]; then
     echo -e "${RED}-- This build will skip all AERA patches ... ${NC}"
     sed -i -e "s/^AERA_INSTALLER_DISABLE_AUTOREBOOT=.*/AERA_INSTALLER_DISABLE_AUTOREBOOT=\"1\"/" $F
  fi


  # OF_initd
  if [ "$AERA_DELETE_INITD_ADDON" = "1" ]; then
     echo -e "${GREEN}-- Deleting the initd addon ...${NC}"
     rm -f $AERA_TMP_WORKING_DIR/sdcard/AERA/Files/OF_initd*.zip
  else
     echo -e "${GREEN}-- Copying the initd addon ...${NC}"
  fi
  
  # alternative/additional device codename?
  if [ -n "$TARGET_DEVICE_ALT" ]; then
     echo -e "${GREEN}-- Adding the alternative device codename(s): \"$TARGET_DEVICE_ALT\" ${NC}"
     Add_Target_Alt;
  fi

  # AERA launches a dedicated native installer entry. It is generated from
  # the already-patched compatibility installer so device aliases, partition
  # paths, build metadata, and every safety flag remain exactly identical.
  # Other recoveries continue to execute update-binary normally.
  tmp="$AERA_TMP_WORKING_DIR/META-INF/com/google/android/aera-installer"
  $CP -pf "$F" "$tmp"
  chmod 0755 "$tmp"

  # if a local callback script is declared, run it, passing to it the temporary working directory (Last call)
  # "--last-call" = just before creating the AERA update zip file
  if [ -n "$AERA_LOCAL_CALLBACK_SCRIPT" -a -f "$AERA_LOCAL_CALLBACK_SCRIPT" ]; then
	bash $AERA_LOCAL_CALLBACK_SCRIPT "$AERA_TMP_WORKING_DIR" "--last-call"
  fi

  # save the build vars
  save_build_vars "$AERA_TMP_WORKING_DIR/META-INF/debug/fox_build_vars.txt"
  tmp="$AERA_RAMDISK/prop.default"
  [ ! -e "$tmp" ] && tmp="$DEFAULT_PROP"
  [ ! -e "$tmp" ] && tmp="$AERA_RAMDISK/default.prop"
  [ -e "$tmp" ] && $CP "$tmp" "$AERA_TMP_WORKING_DIR/META-INF/debug/default.prop"

  # create update zip
  ZIP_CMD="zip --exclude=*.git* -r9 $ZIP_FILE ."
  echo "- Running ZIP command: $ZIP_CMD"
  $ZIP_CMD -z < $AERA_VENDOR_PATH/Files/INSTALL.txt

  #  sign zip installer
  local JAVA8
  if [ -e "$AERA_VENDOR_PATH/signature/zipsigner-3.0.jar" ]; then
     JAVA8=$(which "java")
  else
     JAVA8="/usr/lib/jvm/java-8-openjdk-amd64/jre/bin/java"
  fi
  [ -n "$AERA_JAVA8_PATH" ] && JAVA8="$AERA_JAVA8_PATH"
  if [ ! -x "$JAVA8" ]; then
     JAVA8="/usr/lib/jvm/java-8-openjdk/jre/bin/java"
     [ ! -x "$JAVA8" ] && JAVA8=""
  fi

  export JAVA8
  
  if [ -z "$JAVA8" ]; then
     echo -e "${WHITEONRED}-- java-8 cannot be found! The zip file will NOT be signed! ${NC}"
     echo -e "${WHITEONRED}-- This build CANNOT be released officially! ${NC}"
  elif [ -f $ZIP_FILE  ]; then
     ZIP_CMD="$AERA_VENDOR_PATH/signature/sign_zip.sh -z $ZIP_FILE"
     echo "- Running ZIP command: $ZIP_CMD"
     $ZIP_CMD
     # this breaks the signature
     #echo "- Adding comments (again):"
     #zip $ZIP_FILE -z <$AERA_VENDOR_PATH/Files/INSTALL.txt > /dev/null 2>&1
  fi

  # Creating ZIP md5
  echo -e "${BLUE}-- Creating md5 for $ZIP_FILE${NC}"
  cd "$OUT" && md5sum "$ZIP_FILE" > "$ZIP_FILE.md5" && cd - > /dev/null 2>&1

  # list files
  echo "- Finished:"
  echo "---------------------------------"
  echo " $(/bin/ls -laFt $ZIP_FILE)"
  echo "---------------------------------"

  # export the filenames
  echo "ZIP_FILE=$ZIP_FILE">/tmp/oFox00.tmp
  echo "RECOVERY_IMAGE=$RECOVERY_IMAGE">>/tmp/oFox00.tmp
  [ -f $RECOVERY_IMAGE".tar" ] && echo "RECOVERY_ODIN=$RECOVERY_IMAGE.tar" >>/tmp/oFox00.tmp

  # delete OF Working dir
  rm -rf $AERA_TMP_WORKING_DIR
} # function

# are we using toolbox/toybox?
uses_toolbox() {
 [ "$TW_USE_TOOLBOX" = "true" ] && { echo "1"; return; }
 local T=$(filesize $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/toybox)
 [ "$T" = "0" ] && { echo "0"; return; }
 #local B=$(filesize $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/busybox)
 #[ $T -gt $B ] && { echo "1"; return; }
 T=$(readlink "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/yes")
 [ "$T" = "toybox" ] && echo "1" || echo "0"
}

# drastic size reduction
# This can reduce the recovery image size by up to 3 MB
reduce_ramdisk_size() {
local FFil="$AERA_RAMDISK/AERA/Files"

      echo -e "${GREEN}-- Pruning the ramdisk to reduce the size ... ${NC}"

      # remove some large files
      if [ "$(enabled $AERA_CUSTOM_BINS_TO_SDCARD)" != "1" ]; then
      	 rm -rf $FFil/nano
	 rm -f $AERA_RAMDISK/sbin/aapt
	 rm -f $AERA_RAMDISK/sbin/zip
	 rm -f $AERA_RAMDISK/sbin/nano
	 rm -f $AERA_RAMDISK/sbin/gnutar
	 rm -f $AERA_RAMDISK/sbin/zstd
	 [ "${AERA_ENABLE_ROOT_MANAGER:-0}" = "1" ] || rm -f $AERA_RAMDISK/sbin/ksud
	 rm -f $AERA_RAMDISK/sbin/lz4
	 rm -f $AERA_RAMDISK/sbin/gnused
	 rm -f $AERA_RAMDISK/sbin/gnudate
	 rm -f $AERA_RAMDISK/sbin/bash
	 rm -f $AERA_RAMDISK/sbin/busybox
	 rm -f $AERA_RAMDISK/sbin/patchelf
	 rm -f $AERA_RAMDISK/sbin/fsck.erofs
	 rm -f $AERA_RAMDISK/etc/bash.bashrc
	 rm -rf $AERA_RAMDISK/$RAMDISK_ETC/terminfo
      	 rm -rf $FFil/Tools
      	 if [ "$IS_VANILLA_BUILD" = "1" ]; then
           rm -rf $FFil/OF_avb20
           rm -rf $FFil/OF_verity_crypt
      	 fi
      fi

}

# is this an executable arm binary?
is_arm() {
	local i=$(file $1 | grep "ARM" | grep "ELF");
	[ -n "$i" ] && echo "1" || echo "0";
}

# compress some binaries with upx
compress_some_executables() {
local upx_bin=$AERA_VENDOR_PATH/tools/upx;
    if [ -x "$upx_bin" ]; then
	local min=131072; # only process binaries bigger than 128kb
	[ "$AERA_DRASTIC_SIZE_REDUCTION" = "1" ] && min=65536; # except when drastic size reduction is enabled
	local HERE=$PWD;
	local size;
	local bins;
	local dir;
	for bin_dirs in $AERA_COMPRESS_EXECUTABLES
	do
		dir=$AERA_RAMDISK/$bin_dirs;
		if [ -d $dir ]; then
			cd $dir;
			bins=$(ls ./);
			for i in $bins
			do
				size=$(filesize $i);
				if [ $size -gt $min -a "$(is_arm $i)" = "1" ]; then
					echo -e "${WHITEONRED}-- Compressing \"$i\" with upx ... ${NC}";
					chmod 0755 $i; # if the executable bit is not set, upx will reject it
					$upx_bin --lzma $i;
				fi
			done
		fi
	done # for bin_dirs
	cd $HERE;
    fi
}

# Have some large binaries in /sdcard/AERA/Files/bin/.
process_custom_bins_to_sdcard() {
local tmp1
local tmp2
local ramdisk_sbindir=$AERA_RAMDISK/sbin
local ramdisk_sbindir_10=$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN
local sdcard_bin=/sdcard/AERA/Files/bin
local mksync="3"

  if [ "$(enabled $AERA_CUSTOM_BINS_TO_SDCARD)" != "1" ]; then
     return
  fi

  echo -e "${WHITEONRED}-- 'AERA_CUSTOM_BINS_TO_SDCARD' used; Ensure that you are doing a CLEAN BUILD, else, it *WILL* all go pear-shaped!!${NC}"

  [ -d $AERA_BIN_tmp/bin/ ] && rm -rf $AERA_BIN_tmp/bin/
  mkdir -p $AERA_BIN_tmp/bin/

  if [ -f $ramdisk_sbindir/bash -a ! -h $ramdisk_sbindir/bash ]; then
	mv -f $ramdisk_sbindir/bash $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && ln -sf $sdcard_bin/bash $ramdisk_sbindir/bash
  elif [ "$AERA_BUILD_BASH" = "1" -a -f $ramdisk_sbindir_10/bash -a ! -h $ramdisk_sbindir_10/bash ]; then
	mv -f $ramdisk_sbindir_10/bash $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && ln -sf $sdcard_bin/bash $ramdisk_sbindir_10/bash
  fi

  [ -f $ramdisk_sbindir/zip -a ! -h $ramdisk_sbindir/zip ] && {
	mv -f $ramdisk_sbindir/zip $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && ln -sf $sdcard_bin/zip $ramdisk_sbindir/zip
  }

  [ -f $ramdisk_sbindir/gnutar ] && {
	mv -f $ramdisk_sbindir/gnutar $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && ln -sf $sdcard_bin/gnutar $ramdisk_sbindir/gnutar
  }

  [ -f $ramdisk_sbindir/gnudate ] && {
	mv -f $ramdisk_sbindir/gnudate $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && ln -sf $sdcard_bin/gnudate $ramdisk_sbindir/gnudate
  }

  [ -f $ramdisk_sbindir/gnused ] && {
	mv -f $ramdisk_sbindir/gnused $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && ln -sf $sdcard_bin/gnused $ramdisk_sbindir/gnused
  }

  [ -f $ramdisk_sbindir/aapt ] && {
	mv -f $ramdisk_sbindir/aapt $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && ln -sf $sdcard_bin/aapt $ramdisk_sbindir/aapt
  }

  [ "$AERA_USE_XZ_UTILS" = "1" -a -f $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/xz ] && {
     $CP -pf $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/xz $AERA_BIN_tmp/bin/lzma
     [ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && {
     	 rm -f $ramdisk_sbindir_10/lzma $ramdisk_sbindir_10/xz $ramdisk_sbindir/lzma $ramdisk_sbindir/xz
     	 ln -sf $sdcard_bin/lzma $ramdisk_sbindir/lzma
     	 ln -sf lzma $ramdisk_sbindir/xz
     }
  }

  if [ "$AERA_USE_NANO_EDITOR" = "1" ]; then
     [ "$ramdisk_sbindir/nano" ] && {
     	tmp1=$ramdisk_sbindir/nano
	$CP -af $AERA_VENDOR_PATH/Files/nano/ $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" != "1" ] && sed -i -e "s|^NANO_DIR=.*|NANO_DIR=$sdcard_bin/nano|" $tmp1
     }
  elif [ "$AERA_EXCLUDE_NANO_EDITOR" != "1" -a -f $ramdisk_sbindir_10/nano -a ! -h $ramdisk_sbindir_10/nano ]; then
	mv -f $ramdisk_sbindir_10/nano $AERA_BIN_tmp/bin/
	[ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ] && ln -sf $sdcard_bin/nano $ramdisk_sbindir_10/nano
  fi

 # 1=copy at runtime; 2=symlinks at runtime; 3=symlinks at build time ($mksync)
 if [ "$AERA_CUSTOM_BINS_TO_SDCARD" = "$mksync" ]; then
    return
 fi
 
 # create the helper scripts
 echo -e "${GREEN}-- AERA_CUSTOM_BINS_TO_SDCARD: creating script to process $sdcard_bin/* for the recovery ramdisk ... ${NC}"

 if [ "$AERA_CUSTOM_BINS_TO_SDCARD" = "1" ]; then
    tmp2="cp"
 else
    tmp2="sym"
 fi

# create the script
tmp1=$AERA_BIN_tmp/bin/sdcard_to_bin.sh
rm -f $tmp1
cat << EOF >> "$tmp1"
#!/system/bin/sh -x
   	cmd="$tmp2"
   	chmod +x $sdcard_bin/*
   	if [ "\$cmd" = "cp" ]; then
           [ -d $sdcard_bin/nano/ ] && mv /sbin/nano /sbin/nano_script
           cp -af $sdcard_bin/* /sbin/
           [ -d $sdcard_bin/nano/ ] && { cp -af $sdcard_bin/nano/ /AERA/Files/nano/; rm -rf /sbin/nano/; mv -f /sbin/nano_script /sbin/nano; }
           [ -f $sdcard_bin/nano ] && cp -af $sdcard_bin/nano /system/bin/
   	else
	   files="aapt bash gnused gnutar gnudate lzma zip zstd lz4 ksud"
	   set -- \$files
	   while [ -n "\$1" ]
  	   do
     	      i="\$1"
     	      [ -f $sdcard_bin/"\$i" ] && { rm -f /sbin/\$i; ln -sf $sdcard_bin/\$i /sbin/\$i; }
     	      shift
  	   done
  	   [ -f $sdcard_bin/nano ] && { rm -f /system/bin/nano; ln -sf $sdcard_bin/nano /system/bin/nano; }
   	fi
   	[ -f $sdcard_bin/lzma ] && { rm -f /sbin/xz; ln -sf lzma /sbin/xz; }
	exit 0;
EOF
chmod 0755 $tmp1
 
# Run the script that exposes /sdcard/AERA/Files/bin/* at runtime.
tmp1=$ramdisk_sbindir/aera_sdcard_to_bin.sh
rm -f $tmp1
cat << EOF >> "$tmp1"
files_dir=$sdcard_bin;
files_script=\$files_dir/sdcard_to_bin.sh;
if [ -f \$files_script ]; then
   chmod +x \$files_dir/*;
   echo "I: Running \$files_script !" >> /tmp/recovery.log;
   \$files_script;
fi
rm -f "/sbin/aera_sdcard_to_bin.sh"
rm -f "/sbin/sdcard_to_bin.sh"
EOF
chmod 0755 $tmp1
} # end function process_custom_bins_to_sdcard()

# ****************************************************
# *** now the real work starts!
# ****************************************************

# get the full AERA_VENDOR_PATH
expand_vendor_path

# did we export the temporary directory for AERA ports?
[ -n "$AERA_PORTS_TMP" ] && AERA_TMP_WORKING_DIR="$AERA_PORTS_TMP" || AERA_TMP_WORKING_DIR="/tmp/fox_zip_tmp"

# is the working directory still there from a previous build? If so, remove it
if [ "$AERA_VENDOR_CMD" != "Fox_Before_Recovery_Image" ]; then
   if [ -d "$AERA_WORK" ]; then
      echo -e "${BLUE}-- Working folder found (\"$AERA_WORK\"). Cleaning up...${NC}"
      rm -rf "$AERA_WORK"
   fi

   mkdir -p "$AERA_WORK"

  # perhaps we don't need some "Tools" ?
  if [ "$(SAR_BUILD)" = "1" ]; then
     echo -e "${GREEN}-- This is a system-as-root build ...${NC}"
  else
     echo -e "${GREEN}-- This is NOT a system-as-root build - removing the system_sar_mount directory ...${NC}"
     rm -rf "$AERA_RAMDISK/AERA/Files/Tools/system_sar_mount/"
  fi
fi

###############################################################
# copy stuff to the ramdisk and do all necessary patches before the build system creates the recovery image
if [ "$AERA_VENDOR_CMD" = "Fox_Before_Recovery_Image" ]; then
  case "$TARGET_ARCH" in
  "arm")
      echo -e "${GREEN}-- ARM arch detected. Copying ARM binaries${NC}"
      $CP "$AERA_VENDOR_PATH/prebuilt/arm/magiskboot$UPDATED" "$AERA_RAMDISK/$RAMDISK_SBIN/magiskboot"
      ;;
  "arm64")
      echo -e "${GREEN}-- ARM64 arch detected. Copying ARM64 binaries${NC}"
      $CP "$AERA_VENDOR_PATH/prebuilt/arm64/magiskboot$UPDATED" "$AERA_RAMDISK/$RAMDISK_SBIN/magiskboot"
      ;;
  "x86")
      echo -e "${GREEN}-- x86 arch detected. Copying x86 binaries${NC}"
      $CP "$AERA_VENDOR_PATH/prebuilt/x86/mkbootimg" "$AERA_RAMDISK/$RAMDISK_SBIN/"
      $CP "$AERA_VENDOR_PATH/prebuilt/x86/unpackbootimg" "$AERA_RAMDISK/$RAMDISK_SBIN/"
      ;;
  "x86_64")
      echo -e "${GREEN}-- x86_64 arch detected. Copying x86_64 binaries${NC}"
      $CP "$AERA_VENDOR_PATH/prebuilt/x86_64/mkbootimg" "$AERA_RAMDISK/$RAMDISK_SBIN/"
      $CP "$AERA_VENDOR_PATH/prebuilt/x86_64/unpackbootimg" "$AERA_RAMDISK/$RAMDISK_SBIN/"
      ;;
    *) echo -e "${RED}-- Couldn't detect current device architecture or it is not supported${NC}" ;;
  esac

  # build standard (3GB) version
  # Copy AERA recovery files and sbin helpers before creating the boot image.
  #[ "$AERA_BUILD_DEBUG_MESSAGES" = "1" ] && echo "- AERA_BUILD_DEBUG_MESSAGES: Copying: $AERA_VENDOR_PATH/AERAExtras/* to $AERA_RAMDISK/"
  $CP -pr $AERA_VENDOR_PATH/AERAExtras/* $AERA_RAMDISK/

  # FRP wiping is now a build-gated native operation, not an installer add-on.
  # Incremental builds may retain the retired ZIP in the staging ramdisk.
  if [ -n "$AERA_RAMDISK" ] && [ "$AERA_RAMDISK" != "/" ]; then
    rm -f "$AERA_RAMDISK/AERA/Files/OF_DelFRP/OF_DelFRP.zip"
    rmdir "$AERA_RAMDISK/AERA/Files/OF_DelFRP" 2>/dev/null || true
    # Remove retired bundled installers left by previous incremental builds.
    # magiskboot is a separate boot-image tool and must remain available.
    rm -f "$AERA_RAMDISK/AERA/Files/OF_Magisk/Magisk.zip" \
          "$AERA_RAMDISK/AERA/Files/OF_Magisk/uninstall.zip" \
          "$AERA_RAMDISK/AERA/Files/Magisk.zip" \
          "$AERA_RAMDISK/AERA/Files/uninstall.zip" \
          "$AERA_RAMDISK/AERA/Files/AromaFM/AromaFM.zip" \
          "$AERA_RAMDISK/AERA/Files/AromaFM/AromaFM.zip.cfg"
    rmdir "$AERA_RAMDISK/AERA/Files/OF_Magisk" \
          "$AERA_RAMDISK/AERA/Files/AromaFM" 2>/dev/null || true
  fi

  # if these directories don't already exist
  mkdir -p $AERA_RAMDISK/$RAMDISK_ETC/
  mkdir -p $AERA_RAMDISK/$RAMDISK_SBIN/

  # resetprop (symlink the inline built version)
  echo -e "${GREEN}-- Symlinking \"resetprop\" ...${NC}"
  ln -sf /system/bin/resetprop "$AERA_RAMDISK/$RAMDISK_SBIN/resetprop"

  # deal with magiskboot
  echo -e "${GREEN}-- This build will use magiskboot for patching boot images ...${NC}"
  echo -e "${GREEN}-- Backing up $AERA_RAMDISK/$RAMDISK_SBIN/magiskboot to: /tmp/fox_build_tmp/ ...${NC}"
  mkdir -p /tmp/fox_build_tmp/
  $CP -pf $AERA_RAMDISK/$RAMDISK_SBIN/magiskboot /tmp/fox_build_tmp/magiskboot

  # AERA's clean-room native CLI is installed as /system/bin/aera. Remove any
  # stale FoxCLI artifact left by an incremental build and expose the AERA
  # client from /sbin for interactive recovery shells.
  rm -f "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/fox" "$AERA_RAMDISK/$RAMDISK_SBIN/fox"
  if [ -x "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/aera" ]; then
     ln -sf /system/bin/aera "$AERA_RAMDISK/$RAMDISK_SBIN/aera"
  fi

  # symlink for /sbin/magiskboot in /system/bin/
  if [ -f "$AERA_RAMDISK/$RAMDISK_SBIN/magiskboot" ]; then
     rm -f "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/magiskboot"
     ln -sf /sbin/magiskboot "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/magiskboot"
  fi

  # dmsetup - for killing userdata before formatting data
  if [ "$AERA_USE_DMSETUP" = "1" ]; then
	echo -e "${GREEN}-- Copying the \"dmsetup\" binary ...${NC}"
	$CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/dmsetup $AERA_RAMDISK/$RAMDISK_SBIN/
	chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/dmsetup
  fi

  # try to fix toolbox egrep/fgrep symlink bug
  if [ "$(uses_toolbox)" = "1" ]; then
     rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/egrep $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/fgrep
     ln -sf grep $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/egrep
     ln -sf grep $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/fgrep
  fi

  # replace any built-in lzma (and "xz") with our own
  # use the full "xz" binary for lzma, and for xz - smaller in size, and does the same job
  if [ "$AERA_USE_XZ_UTILS" = "1" ]; then
     if [ "$AERA_DYNAMIC_SAMSUNG_FIX" != "1" ]; then
     	echo -e "${GREEN}-- Replacing any built-in \"lzma\" command with our own full version ...${NC}"
	rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/lzma
	rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/xz
	if [ "$(enabled $AERA_CUSTOM_BINS_TO_SDCARD)" != "1" ]; then
	   $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/xz $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/lzma
           ln -s lzma $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/xz
     	else
	   [ "$AERA_CUSTOM_BINS_TO_SDCARD" = "1" ] && ln -s lzma $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/xz
     	fi
     fi
  fi

  # remove the green LED setting?
  if [ "$OF_USE_GREEN_LED" = "0" ]; then
     echo -e "${GREEN}-- Removing the \"green LED\" setting ...${NC}"
  fi

  # remove extra "More..." link in the "About" screen?
  if [ "$OF_DISABLE_EXTRA_ABOUT_PAGE" = "1" ]; then
     echo -e "${GREEN}-- Disabling the \"More...\" link in the \"About\" page ...${NC}"
  fi


  # Include bash shell ?
  if [ "$AERA_REMOVE_BASH" = "1" ]; then

     if [ "$AERA_BUILD_BASH" != "1" ]; then
         export AERA_USE_BASH_SHELL="0"
         rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/bash
     fi

     # remove the /sbin/ bash if it is there from a previous build
     rm -f $AERA_RAMDISK/$RAMDISK_SBIN/bash
     rm -f $AERA_RAMDISK/$RAMDISK_ETC/bash.bashrc
  else
     echo -e "${GREEN}-- Copying bash ...${NC}"
     $CP -p $AERA_VENDOR_PATH/Files/aera.bashrc $AERA_RAMDISK/$RAMDISK_ETC/bash.bashrc
     $CP -p $AERA_VENDOR_PATH/Files/aera.bashrc $AERA_RAMDISK/AERA/Files/aera.mkshrc
     
     if [ "$AERA_BUILD_BASH" = "1" ]; then
        local aera_home="/sdcard/AERA"
        if [ -n "$AERA_MISCELLANEOUS_ROOT_DIRECTORY" ]; then
           aera_home=$AERA_MISCELLANEOUS_ROOT_DIRECTORY"/AERA"
        fi
        if [ -z "$(cat $AERA_RAMDISK/$RAMDISK_SYSTEM_ETC/bash/bashrc | grep AERA)" ]; then
           echo " " >> "$AERA_RAMDISK/$RAMDISK_SYSTEM_ETC/bash/bashrc"
           echo "# AERA Recovery Project" >> "$AERA_RAMDISK/$RAMDISK_SYSTEM_ETC/bash/bashrc"
           echo "[ -f $aera_home/aera.bashrc ] && source $aera_home/aera.bashrc" >> "$AERA_RAMDISK/$RAMDISK_SYSTEM_ETC/bash/bashrc"
        fi
        echo "[ ! -f $aera_home/aera.bashrc -a -f /AERA/Files/aera.mkshrc ] && source /AERA/Files/aera.mkshrc" >> "$AERA_RAMDISK/$RAMDISK_SYSTEM_ETC/bash/bashrc"
     else
	rm -f $AERA_RAMDISK/$RAMDISK_SBIN/bash
	rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/bash
	[ "$AERA_BASH_TO_SYSTEM_BIN" = "1" ] && F=$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/bash || F=$AERA_RAMDISK/$RAMDISK_SBIN/bash
	$CP -pf $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/bash $F
	chmod 0775 $F
     fi

     if [ "$AERA_ASH_IS_BASH" = "1" ]; then
        export AERA_USE_BASH_SHELL="1"
     fi
  fi

  # replace system "sh" with bash ?
  if [ "$AERA_BUILD_BASH" = "1" ]; then
     BASH_BIN=$RAMDISK_SYSTEM_BIN/bash
  else
     BASH_BIN=$RAMDISK_SBIN/bash
  fi

  if [ "$AERA_USE_BASH_SHELL" = "1" ]; then
        echo -e "${GREEN}-- Replacing the \"sh\" applet with bash ...${NC}"
	rm -f $AERA_RAMDISK/$RAMDISK_SBIN/sh
	ln -s $BASH_BIN $AERA_RAMDISK/$RAMDISK_SBIN/sh
  else
        echo -e "${GREEN}-- Cleaning up any bash stragglers...${NC}"
	# cleanup any stragglers
	if [ -h $AERA_RAMDISK/$RAMDISK_SBIN/sh ]; then
	    T=$(readlink $AERA_RAMDISK/$RAMDISK_SBIN/sh)
	    [ "$(basename $T)" = "bash" ] && rm -f $AERA_RAMDISK/$RAMDISK_SBIN/sh
	fi

	# if there is no symlink for /sbin/sh create one
	if [ ! -e $AERA_RAMDISK/$RAMDISK_SBIN/sh -a ! -h $AERA_RAMDISK/$RAMDISK_SBIN/sh ]; then
	   ln -s $RAMDISK_SYSTEM_BIN/sh $AERA_RAMDISK/$RAMDISK_SBIN/sh
	fi
  fi

# do the same for "ash"?
  if [ "$AERA_ASH_IS_BASH" = "1" ]; then
     echo -e "${GREEN}-- Replacing the \"ash\" applet with bash ...${NC}"
     rm -f $AERA_RAMDISK/$RAMDISK_SBIN/ash
     ln -s $BASH_BIN $AERA_RAMDISK/$RAMDISK_SBIN/ash
     rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/ash
     ln -s $BASH_BIN $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/ash
  else
        echo -e "${GREEN}-- Cleaning up any ash stragglers...${NC}"
	# cleanup any stragglers
	if [ -h $AERA_RAMDISK/$RAMDISK_SBIN/ash ]; then
	    T=$(readlink $AERA_RAMDISK/$RAMDISK_SBIN/ash)
	    [ "$(basename $T)" = "bash" ] && rm -f $AERA_RAMDISK/$RAMDISK_SBIN/ash
	fi
  fi

  # create symlink for /sbin/bash of missing?
  if [ -f "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/bash" -a ! -e "$AERA_RAMDISK/$RAMDISK_SBIN/bash" ]; then
     echo -e "${GREEN}-- Creating a bash symbolic link: /sbin/bash -> /system/bin/bash ...${NC}"
     ln -sf $RAMDISK_SYSTEM_BIN/bash $AERA_RAMDISK/$RAMDISK_SBIN/bash
  fi

  # Include nano editor ?
  if [ "$AERA_USE_NANO_EDITOR" = "1" ]; then
      echo -e "${GREEN}-- Copying nano editor ...${NC}"
      mkdir -p $AERA_RAMDISK/AERA/Files/nano/bin/
      $CP -af $AERA_VENDOR_PATH/Files/nano/sbin/nano $AERA_RAMDISK/$RAMDISK_SBIN/
      if [ "$(enabled $AERA_CUSTOM_BINS_TO_SDCARD)" != "1" ]; then
	$CP -af $AERA_VENDOR_PATH/Files/nano/ $AERA_RAMDISK/AERA/Files/
	$CP -af $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/nano.bin $AERA_RAMDISK/AERA/Files/nano/bin/nano.bin
      fi
  else
      if [ -d $AERA_RAMDISK/AERA/Files/nano/ ]; then
         echo -e "${GREEN}-- Removing the dangling \"$AERA_RAMDISK/AERA/Files/nano/\" ...${NC}"
         rm -rf $AERA_RAMDISK/AERA/Files/nano/
      fi
  fi

  # exclude all species of nano?
  if [ "$AERA_EXCLUDE_NANO_EDITOR" = "1" ]; then
      echo -e "${RED}-- Removing the nano files from the build ...${NC}"
      [ -d $AERA_RAMDISK/AERA/Files/nano/ ] && rm -rf $AERA_RAMDISK/AERA/Files/nano/
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/nano
      rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/nano
      rm -f $AERA_RAMDISK/$RAMDISK_ETC/init/nano*
      rm -rf $AERA_RAMDISK/$RAMDISK_ETC/nano
      if [ -d $AERA_RAMDISK/$RAMDISK_ETC/terminfo -a "$AERA_DRASTIC_SIZE_REDUCTION" != "1" ]; then
         echo -e "${WHITEONRED}-- Do a clean build, or remove \"$AERA_RAMDISK/$RAMDISK_ETC/terminfo\" ...${NC}"
      fi
  fi

  # Include standalone "tar" binary ?
  if [ "$AERA_USE_TAR_BINARY" = "1" ]; then
      echo -e "${GREEN}-- Copying the GNU \"tar\" binary (gnutar) ...${NC}"
      $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/gnutar $AERA_RAMDISK/$RAMDISK_SBIN/
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/gnutar
  else
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/gnutar
  fi

  # Include standalone "sed" binary ?
  if [ "$AERA_USE_SED_BINARY" = "1" ]; then
      echo -e "${GREEN}-- Copying the GNU \"sed\" binary (gnused) ...${NC}"
      $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/gnused $AERA_RAMDISK/$RAMDISK_SBIN/
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/gnused
  else
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/gnused
  fi

  # Include standalone "grep" binary ?
  if [ "$AERA_USE_GREP_BINARY" = "1"  -a -x $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/grep ]; then
      echo -e "${GREEN}-- Copying the GNU \"grep\" binary ...${NC}"
      rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/grep $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/egrep $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/fgrep
      $CP -pf $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/grep $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/
      echo '#!/system/bin/sh' &> "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/fgrep"
      echo '#!/system/bin/sh' &> "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/egrep"
      echo 'exec grep -F "$@"' >> "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/fgrep"
      echo 'exec grep -E "$@"' >> "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/egrep"
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/grep $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/fgrep $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/egrep
  fi

  # Include standalone "zstd" binary ?
  if [ "$AERA_USE_ZSTD_BINARY" = "1" ]; then
      echo -e "${GREEN}-- Copying the \"zstd\" binary ...${NC}"
      $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/zstd $AERA_RAMDISK/$RAMDISK_SBIN/
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/zstd
  else
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/zstd
  fi

  # Include standalone "lz4" binary ?
  if [ "$AERA_USE_LZ4_BINARY" = "1" ]; then
      echo -e "${GREEN}-- Copying the \"lz4\" binary ...${NC}"
      $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/lz4 $AERA_RAMDISK/$RAMDISK_SBIN/
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/lz4
  else
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/lz4
  fi

  # Include standalone "date" binary ?
  if [ "$AERA_USE_DATE_BINARY" = "1" ]; then
      echo -e "${GREEN}-- Copying the GNU \"date\" binary (gnudate) ...${NC}"
      $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/gnudate $AERA_RAMDISK/$RAMDISK_SBIN/
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/gnudate
  else
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/gnudate
  fi

  # Include our own "zip" binary ?
  if [ -f "$AERA_VENDOR_PATH/../../external/zip/Android.mk" -a "$AERA_EXCLUDE_ZIP" != "1" ]; then
         echo -e "${RED}-- Using the InfoZip \"zip\" built from source ...${NC}"
  elif [ "$AERA_REMOVE_ZIP_BINARY" = "1" ]; then
      [ -e $AERA_RAMDISK/$RAMDISK_SBIN/zip ] && {
         echo -e "${RED}-- Removing the AERA InfoZip \"zip\" binary ...${NC}"
         rm -f $AERA_RAMDISK/$RAMDISK_SBIN/zip
      }
  else
      if [ "$AERA_SKIP_ZIP_BINARY" != "1" ]; then
         echo -e "${GREEN}-- Copying the AERA InfoZip \"zip\" binary ...${NC}"
         if [ -e $AERA_RAMDISK/$RAMDISK_SBIN/zip ]; then
            rm -f $AERA_RAMDISK/$RAMDISK_SBIN/zip
         fi
         $CP -pf $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/zip $AERA_RAMDISK/$RAMDISK_SBIN/
         chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/zip
      fi
  fi

  # Include standalone "fsck.erofs" binary ?
  if [ "$AERA_USE_FSCK_EROFS_BINARY" = "1" ]; then
      echo -e "${GREEN}-- Copying the \"fsck.erofs\" binary ...${NC}"
      $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/fsck.erofs $AERA_RAMDISK/$RAMDISK_SBIN/
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/fsck.erofs
  else
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/fsck.erofs
  fi

  # Include standalone "patchelf" binary ?
  if [ "$AERA_USE_PATCHELF_BINARY" = "1" ]; then
      echo -e "${GREEN}-- Copying the \"patchelf\" binary ...${NC}"
      $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/patchelf $AERA_RAMDISK/$RAMDISK_SBIN/
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/patchelf
  else
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/patchelf
  fi

  # if zip is built from source (in /system/bin/) create a symlink to it if necessary
  if [ -x "$AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/zip" ]; then
     [ ! -e "$AERA_RAMDISK/$RAMDISK_SBIN/zip" ] && ln -sf /system/bin/zip $AERA_RAMDISK/$RAMDISK_SBIN/zip
  fi

  # Replace the toolbox "getprop" with "resetprop" ?
  if [ "$AERA_REPLACE_TOOLBOX_GETPROP" = "1" ]; then
     echo -e "${GREEN}-- Replacing the toolbox \"getprop\" command with a fuller version ...${NC}"
     rm -f $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/getprop
     ln -sf /system/bin/resetprop $AERA_RAMDISK/$RAMDISK_SYSTEM_BIN/getprop
  fi

  # Embed the system partition in aerastart.sh.
  F=$AERA_RAMDISK/$RAMDISK_SBIN/aerastart.sh
  if [ -n "$AERA_RECOVERY_SYSTEM_PARTITION" ]; then
     echo -e "${RED}-- Changing the recovery system partition to \"$AERA_RECOVERY_SYSTEM_PARTITION\" ${NC}"
     sed -i -e "s|^SYSTEM_BLOCK=.*|SYSTEM_BLOCK=\"$AERA_RECOVERY_SYSTEM_PARTITION\"|" $F
  fi

  # Embed the vendor partition in aerastart.sh.
  F=$AERA_RAMDISK/$RAMDISK_SBIN/aerastart.sh
  if [ -n "$AERA_RECOVERY_VENDOR_PARTITION" ]; then
     echo -e "${RED}-- Changing the recovery vendor partition to \"$AERA_RECOVERY_VENDOR_PARTITION\" ${NC}"
     sed -i -e "s|^VENDOR_BLOCK=.*|VENDOR_BLOCK=\"$AERA_RECOVERY_VENDOR_PARTITION\"|" $F
  fi

  # Embed the boot partition in aerastart.sh.
  F=$AERA_RAMDISK/sbin/aerastart.sh
  if [ -n "$AERA_RECOVERY_BOOT_PARTITION" ]; then
     echo -e "${RED}-- Changing the recovery boot partition to \"$AERA_RECOVERY_BOOT_PARTITION\" ${NC}"
     sed -i -e "s|^BOOT_BLOCK=.*|BOOT_BLOCK=\"$AERA_RECOVERY_BOOT_PARTITION\"|" $F
  fi

  # Embed the AERA_SETTINGS_ROOT_DIRECTORY build variable.
  F=$AERA_RAMDISK/sbin/aerastart.sh
  if [ -n "$AERA_SETTINGS_ROOT_DIRECTORY" ]; then
     echo -e "${RED}-- This build will use $AERA_SETTINGS_ROOT_DIRECTORY for its internal settings ... ${NC}"
     sed -i -e "s|^AERA_SETTINGS_ROOT_DIRECTORY=.*|AERA_SETTINGS_ROOT_DIRECTORY=\"$AERA_SETTINGS_ROOT_DIRECTORY\"|" $F
  fi

  if [ -n "$AERA_MISCELLANEOUS_ROOT_DIRECTORY" ]; then
     echo -e "${RED}-- This build will use $AERA_MISCELLANEOUS_ROOT_DIRECTORY for its stuff ... ${NC}"
     sed -i -e "s|^AERA_MISCELLANEOUS_ROOT_DIRECTORY=.*|AERA_MISCELLANEOUS_ROOT_DIRECTORY=\"$AERA_MISCELLANEOUS_ROOT_DIRECTORY\"|" $F
  fi

  # mark whether this is a vAB or vanilla build
  if [ "$IS_VIRTUAL_AB_DEVICE" = "1" -o "$IS_VANILLA_BUILD=1" = "1" ]; then
	F=$AERA_RAMDISK/sbin/aerastart.sh
	sed -i -e "s/^VIRTUAL_AB_OR_VANILLA=.*/VIRTUAL_AB_OR_VANILLA=\"1\"/" $F
  fi

  # Include mmgui
  $CP -p $AERA_VENDOR_PATH/Files/mmgui $AERA_RAMDISK/$RAMDISK_SBIN/mmgui
  chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/mmgui

  # Include aapt (1.7mb!) ?
  if [ "$AERA_REMOVE_AAPT" = "1" ]; then
     echo -e "${GREEN}-- Omitting the aapt binary ...${NC}"
     # remove aapt if it is there from a previous build
     rm -f $AERA_RAMDISK/$RAMDISK_SBIN/aapt
  else
	$CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/aapt $AERA_RAMDISK/$RAMDISK_SBIN/aapt
	#$CP -p $AERA_VENDOR_PATH/Files/aapt $AERA_RAMDISK/$RAMDISK_SBIN/aapt
	chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/aapt
  fi

  # Clear obsolete installer addons from incremental ramdisks as well.
  rm -rf $AERA_RAMDISK/AERA/Files/KernelSU/
  if [ "${AERA_ENABLE_ROOT_MANAGER:-0}" = "1" ] && [ "$TARGET_ARCH" = "arm64" ]; then
      echo -e "${GREEN}-- Copying the AERA Root Manager ksud binary ...${NC}"
      $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/ksud $AERA_RAMDISK/$RAMDISK_SBIN/
      chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/ksud
  else
      rm -f $AERA_RAMDISK/$RAMDISK_SBIN/ksud
  fi

  # enable the app manager?
  if [ "$AERA_ENABLE_APP_MANAGER" = "1" ]; then
     echo -e "${GREEN}-- Enabling the App Manager ...${NC}"
  else
     echo -e "${GREEN}-- Omitting the aapt binary (it is useless if the app manager is not enabled) ...${NC}"
     # remove aapt also, as it would be redundant
     rm -f $AERA_RAMDISK/$RAMDISK_SBIN/aapt
  fi

  # fox_10 and later - include busybox
  if [ "$AERA_USE_BUSYBOX_BINARY" = "1" ]; then
     $CP -p $AERA_VENDOR_PATH/prebuilt/$TARGET_ARCH/busybox $AERA_RAMDISK/$RAMDISK_SBIN/busybox
     chmod 0755 $AERA_RAMDISK/$RAMDISK_SBIN/busybox
  else
     rm -f $AERA_RAMDISK/$RAMDISK_SBIN/busybox
  fi

  # MiSans font (for proper display of Chinese language)
  if [ "$AERA_USE_MISANS_FONTS" = "1" ]; then
     echo -e "${GREEN}-- Replacing the 'InterDisplay' fonts with 'MiSans' ...${NC}"
     $CP -p $AERA_VENDOR_PATH/Files/MiSans-Regular.ttf $AERA_RAMDISK/twres/fonts/InterDisplay-Regular.ttf
     $CP -p $AERA_VENDOR_PATH/Files/MiSans-Medium.ttf $AERA_RAMDISK/twres/fonts/InterDisplay-Medium.ttf
  fi

#########################################################################################
  if [ "$(enabled $AERA_CUSTOM_BINS_TO_SDCARD)" = "1" ]; then
     process_custom_bins_to_sdcard;
  fi
#########################################################################################
  
  # if a local callback script is declared, run it, passing to it the ramdisk directory (first call)
  if [ -n "$AERA_LOCAL_CALLBACK_SCRIPT" -a -f "$AERA_LOCAL_CALLBACK_SCRIPT" ]; then
	bash $AERA_LOCAL_CALLBACK_SCRIPT "$AERA_RAMDISK" "--first-call"
  fi

  # compress some executables?
  if [ -n "$AERA_COMPRESS_EXECUTABLES" -a "$AERA_COMPRESS_EXECUTABLES" != "0" ]; then
	[  "$AERA_COMPRESS_EXECUTABLES" = "1" ] && export AERA_COMPRESS_EXECUTABLES="/sbin /system/bin";
	compress_some_executables;
  fi

  # reduce ramdisk size drastically?
  if [ "$AERA_DRASTIC_SIZE_REDUCTION" = "1" ]; then
     echo -e "${WHITEONRED}-- Going to do some drastic size reductions! ${NC}"
     echo -e "${WHITEONRED}-- Don't worry if you see some resource errors in the recovery's debug screen. ${NC}"
     reduce_ramdisk_size;
  fi

  # save the build date
  BUILD_DATE=$(date -u "+%c")
  BUILD_DATE_UTC=$(date "+%s")
  [ ! -e "$DEFAULT_PROP" ] && DEFAULT_PROP="$AERA_RAMDISK/default.prop"
  [ ! -e "$DEFAULT_PROP_ROOT" ] && DEFAULT_PROP_ROOT="$DEFAULT_PROP"

  # if we need to work around the bugged aosp alleged anti-rollback protection
  if [ -n "$AERA_BUGGED_AOSP_ARB_WORKAROUND" ]; then
     echo -e "${WHITEONGREEN}-- Dealing with bugged AOSP alleged anti-ARB: setting build date to \"$AERA_BUGGED_AOSP_ARB_WORKAROUND\" (instead of the true date: \"$BUILD_DATE_UTC\") ...${NC}"
     Save_Build_Date "$AERA_BUGGED_AOSP_ARB_WORKAROUND" "$BUILD_DATE_UTC"
  else
     Save_Build_Date "$BUILD_DATE_UTC"
  fi

  # ensure that we have a proper record of the actual build date/time
  grep -q "ro.build.date.utc_aera=" $DEFAULT_PROP_ROOT && \
	sed -i -e "s/ro.build.date.utc_aera=.*/ro.build.date.utc_aera=$BUILD_DATE_UTC/g" $DEFAULT_PROP_ROOT || \
	echo "ro.build.date.utc_aera=$BUILD_DATE_UTC" >> $DEFAULT_PROP_ROOT

  grep -q "ro.bootimage.build.date.utc_aera=" $DEFAULT_PROP_ROOT && \
	sed -i -e "s/ro.bootimage.build.date.utc_aera=.*/ro.bootimage.build.date.utc_aera=$BUILD_DATE_UTC/g" $DEFAULT_PROP_ROOT || \
	echo "ro.bootimage.build.date.utc_aera=$BUILD_DATE_UTC" >> $DEFAULT_PROP_ROOT

  # also update prop.default
  grep -q "ro.build.date.utc_aera=" $DEFAULT_PROP && \
	sed -i -e "s/ro.build.date.utc_aera=.*/ro.build.date.utc_aera=$BUILD_DATE_UTC/g" $DEFAULT_PROP || \
	echo "ro.build.date.utc_aera=$BUILD_DATE_UTC" >> $DEFAULT_PROP

  grep -q "ro.bootimage.build.date.utc_aera=" $DEFAULT_PROP && \
	sed -i -e "s/ro.bootimage.build.date.utc_aera=.*/ro.bootimage.build.date.utc_aera=$BUILD_DATE_UTC/g" $DEFAULT_PROP || \
	echo "ro.bootimage.build.date.utc_aera=$BUILD_DATE_UTC" >> $DEFAULT_PROP

  # Save the AERA runtime metadata in /etc/aera.cfg.
  echo "AERA_BUILD_DATE=$BUILD_DATE" > $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg
  [ -z "$AERA_CURRENT_DEV_STR" ] && AERA_CURRENT_DEV_STR=$(git -C $AERA_VENDOR_PATH/../../bootable/recovery log -1 --format='%ad (%h)' --date=short) > /dev/null 2>&1
  if [ -n "$AERA_CURRENT_DEV_STR" ]; then
    export AERA_CURRENT_DEV_STR
    echo "AERA_CODE_BASE=$AERA_CURRENT_DEV_STR" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg
  fi

  echo "ro.build.date.utc_aera=$BUILD_DATE_UTC" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg
  echo "ro.bootimage.build.date.utc_aera=$BUILD_DATE_UTC" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg
  if [ -n "$AERA_RECOVERY_SYSTEM_PARTITION" ]; then
     echo "SYSTEM_PARTITION=$AERA_RECOVERY_SYSTEM_PARTITION" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg
  fi
  if [ -n "$AERA_RECOVERY_INSTALL_PARTITION" ]; then
     echo "RECOVERY_PARTITION=$AERA_RECOVERY_INSTALL_PARTITION" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg
  fi
  if [ -n "$AERA_RECOVERY_VENDOR_PARTITION" ]; then
     echo "VENDOR_PARTITION=$AERA_RECOVERY_VENDOR_PARTITION" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg
  fi
  if [ -n "$AERA_RECOVERY_BOOT_PARTITION" ]; then
     echo "BOOT_PARTITION=$AERA_RECOVERY_BOOT_PARTITION" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg
  fi

  # save the codebase information
  grep -q "ro.build.aera_codebase=" $DEFAULT_PROP && \
	sed -i -e "s/ro.build.aera_codebase=.*/ro.build.aera_codebase=$AERA_CURRENT_DEV_STR/g" $DEFAULT_PROP || \
	echo "ro.build.aera_codebase=$AERA_CURRENT_DEV_STR" >> $DEFAULT_PROP

  grep -q "ro.build.aera_codebase=" $DEFAULT_PROP_ROOT && \
	sed -i -e "s/ro.build.aera_codebase=.*/ro.build.aera_codebase=$AERA_CURRENT_DEV_STR/g" $DEFAULT_PROP_ROOT || \
	echo "ro.build.aera_codebase=$AERA_CURRENT_DEV_STR" >> $DEFAULT_PROP_ROOT

  # save the build id
   echo -e "${GREEN}-- Generating the build ID ${NC}"
   tmp1=$(generate_build_id)
   grep -q "ro.build.aera_id=" $DEFAULT_PROP_ROOT && \
	sed -i -e "s/ro.build.aera_id=.*/ro.build.aera_id=$tmp1/g" $DEFAULT_PROP_ROOT || \
	echo "ro.build.aera_id=$tmp1" >> $DEFAULT_PROP_ROOT

  # also update prop.default
   grep -q "ro.build.aera_id=" $DEFAULT_PROP && \
	sed -i -e "s/ro.build.aera_id=.*/ro.build.aera_id=$tmp1/g" $DEFAULT_PROP || \
	echo "ro.build.aera_id=$tmp1" >> $DEFAULT_PROP

   echo "ro.build.aera_id=$tmp1" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg

   # stamp our identity in the prop
   sed -i -e "s/$TARGET_PRODUCT/aera_$AERA_DEVICE/g" $DEFAULT_PROP

   # save some original file sizes
   echo -e "${GREEN}-- Saving some original file sizes ${NC}"
   [ -n "$recovery_uncompressed_ramdisk" ] && F=$(filesize $recovery_uncompressed_ramdisk) || F=0
   echo "ramdisk_size=$F" >> $AERA_RAMDISK/$RAMDISK_ETC/aera.cfg

  # let's be clear where we are ...
  if [ "$AERA_VENDOR_CMD" = "Fox_Before_Recovery_Image" ]; then
     echo -e "${RED}-- Building the recovery image, using the official AOSP recovery image builder ...${NC}"
  fi

fi

# this is the final stage after the recovery image has been created
# process the recovery image where necessary (and repack where necessary)
if [ "$AERA_VENDOR_CMD" = "Fox_After_Recovery_Image" ]; then

     if [ "$AERA_SAMSUNG_DEVICE" = "1" -o "$AERA_SAMSUNG_DEVICE" = "true" ]; then
        SAMSUNG_DEVICE="samsung"
     else
        SAMSUNG_DEVICE=$(file_getprop "$DEFAULT_PROP" "ro.product.manufacturer")
     fi

     if [ -z "$SAMSUNG_DEVICE" ]; then
        SAMSUNG_DEVICE=$(grep "manufacturer=samsung" "$DEFAULT_PROP")
        [ -n "$SAMSUNG_DEVICE" ] && SAMSUNG_DEVICE="samsung"
     fi

     if [ -z "$INSTALLED_RECOVERYIMAGE_TARGET" ]; then
        if [ -n "$INSTALLED_BOOTIMAGE_TARGET" ]; then
           INSTALLED_RECOVERYIMAGE_TARGET="$INSTALLED_BOOTIMAGE_TARGET"
        fi
     fi

     if [ "$IS_VENDOR_BOOT_RECOVERY" = "1" ]; then
        if [ -n "$INSTALLED_VENDOR_BOOTIMAGE_TARGET" ]; then
           INSTALLED_RECOVERYIMAGE_TARGET="$INSTALLED_VENDOR_BOOTIMAGE_TARGET"
        else
           INSTALLED_RECOVERYIMAGE_TARGET="$OUT/$COMPILED_IMAGE_FILE"
        fi
     fi

     # copy
     echo -e "${GREEN}-- Copying recovery: \"$INSTALLED_RECOVERYIMAGE_TARGET\" --> \"$RECOVERY_IMAGE\" ${NC}"
     $CP -p "$INSTALLED_RECOVERYIMAGE_TARGET" "$RECOVERY_IMAGE"

     # samsung stuff?
     if [ "$SAMSUNG_DEVICE" = "samsung" -a "$AERA_USE_SAMSUNG_SPECIAL" = "1" ]; then
     	echo -e "${RED}-- Appending SEANDROIDENFORCE to $RECOVERY_IMAGE ${NC}"
     	echo -n "SEANDROIDENFORCE" >> $RECOVERY_IMAGE
     fi

     # md5sum
     cd "$OUT" && md5sum "$RECOVERY_IMAGE" > "$RECOVERY_IMAGE.md5" && cd - > /dev/null 2>&1

     # more samsung stuff
     if [ "$SAMSUNG_DEVICE" = "samsung" ]; then
     	echo -e "${RED}-- Creating Odin flashable recovery tar ($RECOVERY_IMAGE.tar) ... ${NC}"

     	# make sure that the image being tarred is the correct one
     	$CP -pf "$RECOVERY_IMAGE" $INSTALLED_RECOVERYIMAGE_TARGET
     	tar -C $(dirname "$RECOVERY_IMAGE") -H ustar -c $COMPILED_IMAGE_FILE > $RECOVERY_IMAGE".tar"
     fi

   # create update zip installer
   if [ "$AERA_DISABLE_UPDATEZIP" != "1" ]; then
      	do_create_update_zip
   else
	echo -e "${RED}-- Skip creating recovery zip${NC}"
   fi

   #Info
   echo -e ""
   echo -e ""
   cat $AERA_VENDOR_PATH/Files/AERABanner
   echo -e ""
   echo -e ""
   echo -e "=================${BLUE}Finished building AERA R1.0${NC}================="
   echo -e ""
   echo -e "${GREEN}Recovery image:${NC} $RECOVERY_IMAGE"
   echo -e "          MD5: $RECOVERY_IMAGE.md5"
   export RECOVERY_IMAGE

   if [ "$AERA_DISABLE_UPDATEZIP" != "1" ]; then
	echo -e ""
	echo -e "${GREEN}Recovery zip:${NC} $ZIP_FILE"
	echo -e "          MD5: $ZIP_FILE.md5"
   	echo -e ""
   	export ZIP_FILE
   fi

   echo -e "=================================================================="

   # clean up, with success code
   abort 0
fi
# end!
