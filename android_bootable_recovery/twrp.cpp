/*
	Copyright 2012-2020 TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	Copyright (C) 2018-2025 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>
#include "recovery_utils/battery_utils.h"
#include "aeraui/platform/aera_message.hpp"
#include "cutils/properties.h"

#ifdef ANDROID_RB_RESTART
#include "cutils/android_reboot.h"
#else
#include <sys/reboot.h>
#endif

extern "C" {
#include "aeraui/platform/aera_ui_host.h"
}
#include "set_metadata.h"
#include "aeraui/platform/aera_ui_host.hpp"
#include "twcommon.h"
#include "twrp-functions.hpp"
#include "data.hpp"

#include "partitions.hpp"
#ifdef __ANDROID_API_N__
#include <android-base/strings.h>
#else
#include <base/strings.h>
#endif
#include "openrecoveryscript.hpp"
#include "variables.h"
#include "startupArgs.hpp"
#include "twrpAdbBuFifo.hpp"
#include <aeraui/runner.hpp>
#ifdef TW_USE_NEW_MINADBD
// #include "minadbd/minadbd.h"
#else
extern "C" {
#include "minadbd21/adb.h"
}
#endif

#ifdef TW_INCLUDE_CRYPTO
#include "FsCrypt.h"
#include "Decrypt.h"
#endif

//extern int adb_server_main(int is_daemon, int server_port, int /* reply_fd */);

TWPartitionManager PartitionManager;
int Log_Offset;
bool datamedia;

static void Print_Prop(const char *key, const char *name, void *cookie) {
	printf("%s=%s\n", key, name);
}

static void Decrypt_Page(bool SkipDecryption, bool datamedia) {
	// Offer to decrypt if the device is encrypted
	if (DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0) {
		if (SkipDecryption) {
			LOGINFO("Skipping decryption\n");
			PartitionManager.Update_System_Details();
		} else if (DataManager::GetIntValue(TW_CRYPTO_PWTYPE) != 0) {
			DataManager::SetValue(AERA_ENCRYPTED_DEVICE, "1");
			LOGINFO("Is encrypted, do decrypt page first\n");
			const bool file_based = DataManager::GetIntValue(TW_IS_FBE) != 0;
			if (file_based)
				DataManager::SetValue("tw_crypto_user_id", "0");
			const auto native_result = aeraui::RunAeraUiDecryption(
				DataManager::GetIntValue(TW_CRYPTO_PWTYPE), file_based, 0,
				DataManager::GetIntValue("tw_gui_pattern_grid_size"));
			if (native_result == aeraui::DecryptionResult::kUnavailable) {
				if (gui_startPage("decrypt", 1, 1) != 0)
					LOGERR("Failed to start decrypt GUI page.\n");
				else {
					DataManager::SetValue("OTA_decrypted", "1");
					usleep(16);
				}
			} else if (native_result == aeraui::DecryptionResult::kSuccess) {
				// OrangeFox - make note of this decryption
				DataManager::SetValue("OTA_decrypted", "1");
				usleep(16);
			} else if (native_result == aeraui::DecryptionResult::kTerminalAction) {
				// Preserve reboot and power-off actions raised by the early UI.
				// Starting another decrypt renderer here would consume the
				// action and re-open an abandoned Qualcomm scanout context.
				LOGINFO("Native decrypt UI requested a terminal action\n");
			} else {
				LOGINFO("User continued without decrypting data\n");
			}
		}
	} else if (datamedia) {
		PartitionManager.Update_System_Details();
		if (tw_get_default_metadata(DataManager::GetSettingsStoragePath().c_str()) != 0 && tw_get_default_metadata(DataManager::GetCurrentStoragePath().c_str()) != 0) {
			LOGINFO("Failed to get default contexts and file mode for storage files.\n");
		} else {
			LOGINFO("Got default contexts and file mode for storage files.\n");
		}
	}
}

static void process_fastbootd_mode() {
		LOGINFO("starting fastboot\n");

		// Cold fastbootd uses the normal recovery partition/module setup so
		// device hardware is initialized consistently. Release the logical
		// devices only after that setup, immediately before fastboot takes over.
		if (android::base::GetBoolProperty("ro.boot.dynamic_partitions", false) &&
				!PartitionManager.Unmap_Super_Devices(true)) {
			LOGERR("Failed to release dynamic partitions for fastbootd.\n");
			return;
		}

#ifdef AB_OTA_UPDATER
		DataManager::SetValue("tw_active_slot", PartitionManager.Get_Active_Slot_Display());
#endif

		gui_msg(Msg("fastboot_console_msg=Entered Fastboot mode..."));
		property_set("ro.aera.fastbootd", "1");
		property_set("ro.boot.verifiedbootstate", "orange");
		TWFunc::RunFoxScript("/system/bin/runatboot.sh", "");
		TWFunc::RunFoxScript("/system/bin/postfastboot.sh", "");
		if (gui_startPage("fastboot", 1, 1) != 0) {
			LOGERR("Failed to start fastbootd page.\n");
		}
		property_set("ro.aera.fastbootd", "0");
}

static void process_recovery_mode(twrpAdbBuFifo* adb_bu_fifo,
		bool skip_decryption, bool soft_switch) {
	char crash_prop_val[PROPERTY_VALUE_MAX];
	int crash_counter;

	if (soft_switch) {
		// Replacing recovery userspace is intentional, not a crash. Preserve
		// normal MTP and startup behavior when returning from fastbootd.
		crash_counter = 0;
	} else {
		property_get("aera.crash_counter", crash_prop_val, "-1");
		crash_counter = atoi(crash_prop_val) + 1;
	}
	snprintf(crash_prop_val, sizeof(crash_prop_val), "%d", crash_counter);
	property_set("aera.crash_counter", crash_prop_val);

	if (crash_counter == 0) {
		property_list(Print_Prop, NULL);
		printf("\n");
	} else {
		printf("aera.crash_counter=%d\n", crash_counter);
	}

	// Set the props for OrangeFox dynamic partitions
	PartitionManager.Fox_Set_Dynamic_Partition_Props(); // don't move this from here!

// We are doing this here to allow super partition to be set up prior to overriding properties
#if defined(TW_INCLUDE_LIBRESETPROP)
	std::vector<std::string> build_date_props = {"ro.build.date.utc", "ro.bootimage.build.date.utc", "ro.vendor.build.date.utc", "ro.system.build.date.utc", "ro.system_ext.build.date.utc", "ro.product.build.date.utc", "ro.odm.build.date.utc"};
	std::string val = "0";
	#ifdef AERA_BUGGED_AOSP_ARB_WORKAROUND
	val = AERA_BUGGED_AOSP_ARB_WORKAROUND;
	#endif
	for (auto prop : build_date_props) {
		TWFunc::Property_Override(prop, val);
		LOGINFO("Overriding %s with value: \"%s\"\n", prop.c_str(), val.c_str());
	}
#if defined(TW_OVERRIDE_SYSTEM_PROPS)
	stringstream override_props(EXPAND(TW_OVERRIDE_SYSTEM_PROPS));
	string current_prop;

	std::vector<std::string> partition_list;
	partition_list.push_back (PartitionManager.Get_Android_Root_Path().c_str());
#ifdef TW_OVERRIDE_PROPS_ADDITIONAL_PARTITIONS
	std::vector<std::string> additional_partition_list = TWFunc::Split_String(TW_OVERRIDE_PROPS_ADDITIONAL_PARTITIONS, " ");
	partition_list.insert(partition_list.end(), additional_partition_list.begin(), additional_partition_list.end());
#endif
	std::vector<std::string> build_prop_list = {"build.prop"};
#ifdef TW_SYSTEM_BUILD_PROP_ADDITIONAL_PATHS
	std::vector<std::string> additional_build_prop_list = TWFunc::Split_String(TW_SYSTEM_BUILD_PROP_ADDITIONAL_PATHS, ";");
	build_prop_list.insert(build_prop_list.end(), additional_build_prop_list.begin(), additional_build_prop_list.end());
#endif
	while (getline(override_props, current_prop, ';')) {
		string other_prop;
		if (current_prop.find("=") != string::npos) {
			other_prop = current_prop.substr(current_prop.find("=") + 1);
			current_prop = current_prop.substr(0, current_prop.find("="));
		} else {
			other_prop = current_prop;
		}
		other_prop = android::base::Trim(other_prop);
		current_prop = android::base::Trim(current_prop);

		for (auto&& partition_mount_point:partition_list) {
			for (auto&& prop_file:build_prop_list) {
				string sys_val = TWFunc::Partition_Property_Get(other_prop, PartitionManager, partition_mount_point.c_str(), prop_file);
				if (!sys_val.empty()) {
					if (partition_mount_point == "/system_root") {
						LOGINFO("Overriding %s with value: \"%s\" from property %s in /system/%s\n", current_prop.c_str(), sys_val.c_str(), other_prop.c_str(),
							prop_file.c_str());
					} else {
						LOGINFO("Overriding %s with value: \"%s\" from property %s in /%s/%s\n", current_prop.c_str(), sys_val.c_str(), other_prop.c_str(),
							partition_mount_point.c_str(), prop_file.c_str());
					}
					int error = TWFunc::Property_Override(current_prop, sys_val);
					if (error) {
						LOGERR("Failed overriding property %s, error_code: %d\n", current_prop.c_str(), error);
					}
					if (partition_mount_point == partition_list.back()) {
						PartitionManager.UnMount_By_Path(partition_mount_point, false);
					}
					goto exit;
				} else {
					if (partition_mount_point == "/system_root") {
						LOGINFO("Unable to override property %s: property not found in /system/%s\n", current_prop.c_str(), prop_file.c_str());
					} else {
						LOGINFO("Unable to override property %s: property not found in /%s/%s\n", current_prop.c_str(), partition_mount_point.c_str(), prop_file.c_str());
					}
				}
			}
			PartitionManager.UnMount_By_Path(partition_mount_point, false);
		}
		exit:
		continue;
	}
#endif // defined(TW_OVERRIDE_SYSTEM_PROPS)
#endif // defined(TW_INCLUDE_LIBRESETPROP)

        // use the ROM's fingerprint?
        TWFunc::RunStartupScript();
        TWFunc::UseSystemFingerprint();
	TWFunc::RunFoxScript("/system/bin/runatboot.sh", "");

#ifdef TW_INCLUDE_INJECTTWRP
	// Back up TWRP Ramdisk if needed:
	TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
	LOGINFO("Backing up TWRP ramdisk...\n");
	if (Boot == NULL || Boot->Current_File_System != "emmc")
		TWFunc::Exec_Cmd("injecttwrp --backup /tmp/backup_recovery_ramdisk.img");
	else {
		string injectcmd = "injecttwrp --backup /tmp/backup_recovery_ramdisk.img bd=" + Boot->Actual_Block_Device;
		TWFunc::Exec_Cmd(injectcmd);
	}
	LOGINFO("Backup of AERA ramdisk done.\n");
#endif

#ifdef OF_ADVANCED_SECURITY
	property_set("ctl.stop", "adbd");
	property_set("aera.adb.status", "0");
#endif

#ifdef TW_INCLUDE_CRYPTO
	android::keystore::copySqliteDb();
#endif
	Decrypt_Page(skip_decryption, datamedia);

	// Check for and load custom theme if present
	TWFunc::check_selinux_support();
	gui_loadCustomResources();
#ifdef OF_ALLOW_EARLY_SETTINGS_LOAD
	// Reset Mount_Read_Only flag here to follow tw_mount_system_ro var
	if (PartitionManager.Get_Super_Status())
		PartitionManager.Mount_Super_Toggle(DataManager::GetStrValue("tw_mount_system_ro"));
#endif
	PartitionManager.Output_Partition_Logging();

	// Fixup the RTC clock on devices which require it
	if (crash_counter == 0)
		TWFunc::Fixup_Time_On_Boot();

	TWFunc::Update_Log_File();
#ifndef OF_ALLOW_EARLY_SETTINGS_LOAD
	DataManager::ReadSettingsFile();
#endif

	// Run any outstanding OpenRecoveryScript
	std::string cacheDir = TWFunc::get_log_dir();
	if (cacheDir == DATA_LOGS_DIR)
		cacheDir = "/data/cache";
	std::string orsFile = cacheDir + "/recovery/openrecoveryscript";
	if ((DataManager::GetIntValue(TW_IS_ENCRYPTED) == 0 || skip_decryption || !PartitionManager.Storage_Is_Encrypted()) && (TWFunc::Path_Exists(SCRIPT_FILE_TMP) || TWFunc::Path_Exists(orsFile))) {
		OpenRecoveryScript::Run_OpenRecoveryScript();
	}

  	// call OrangeFox startup code
  	TWFunc::OrangeFox_Startup();
  	
#ifdef OF_ADVANCED_SECURITY
	LOGINFO("ADB & MTP disabled by maintainer\n");
	DataManager::SetValue("fox_advanced_security", "1");
  	DataManager::SetValue("tw_mtp_enabled", 0);
#else
#ifdef TW_HAS_MTP
	char mtp_crash_check[PROPERTY_VALUE_MAX];
	property_get("mtp.crash_check", mtp_crash_check, "0");
	if (DataManager::GetIntValue("tw_mtp_enabled")
			&& !strcmp(mtp_crash_check, "0") && !crash_counter
			&& (!DataManager::GetIntValue(TW_IS_ENCRYPTED) || DataManager::GetIntValue(TW_IS_DECRYPTED))) {
		property_set("mtp.crash_check", "1");
		LOGINFO("Starting MTP\n");
		if (!PartitionManager.Enable_MTP())
			PartitionManager.Disable_MTP();
		else
			gui_msg("mtp_enabled=MTP Enabled");
		property_set("mtp.crash_check", "0");
	} else if (strcmp(mtp_crash_check, "0")) {
		gui_warn("mtp_crash=MTP Crashed, not starting MTP on boot.");
		DataManager::SetValue("tw_mtp_enabled", 0);
		PartitionManager.Disable_MTP();
	} else if (crash_counter == 1) {
		LOGINFO("AERA crashed; disabling MTP as a precaution.\n");
		PartitionManager.Disable_MTP();
	}
#endif
#endif

#ifndef TW_OEM_BUILD
	// Check if system has never been changed
	TWPartition* sys = PartitionManager.Find_Partition_By_Path(PartitionManager.Get_Android_Root_Path());
	TWPartition* ven = PartitionManager.Find_Partition_By_Path("/vendor");
	if (sys) {
		if (sys->Get_Super_Status()) {
#ifdef TW_INCLUDE_CRYPTO
			std::string recoveryLogDir(DATA_LOGS_DIR);
			recoveryLogDir += "/recovery";
			if (!TWFunc::Path_Exists(recoveryLogDir)) {
				bool created = PartitionManager.Recreate_Logs_Dir();
				if (!created)
					LOGERR("Unable to create log directory for TWRP\n");
			}
#ifndef OF_ALLOW_EARLY_SETTINGS_LOAD
			DataManager::ReadSettingsFile();
			PartitionManager.Mount_Super_Toggle(DataManager::GetStrValue("tw_mount_system_ro"));
#endif
#endif
		} else {
			if ((DataManager::GetIntValue("tw_mount_system_ro") == 0 && sys->Check_Lifetime_Writes() == 0) || DataManager::GetIntValue("tw_mount_system_ro") == 2) {
				if (DataManager::GetIntValue("tw_never_show_system_ro_page") == 0) {
					DataManager::SetValue("tw_back", "main");
					if (gui_startPage("system_readonly", 1, 1) != 0) {
						LOGERR("Failed to start system_readonly GUI page.\n");
					}
				} else if (DataManager::GetIntValue("tw_mount_system_ro") == 0) {
					sys->Change_Mount_Read_Only(false);
					if (ven)
						ven->Change_Mount_Read_Only(false);
				}
			} else if (DataManager::GetIntValue("tw_mount_system_ro") == 1) {
				// Do nothing, user selected to leave system read only
			} else {
				sys->Change_Mount_Read_Only(false);
				if (ven)
					ven->Change_Mount_Read_Only(false);
			}
		}
	}
#endif

	TWFunc::Update_Log_File();

	adb_bu_fifo->threadAdbBuFifo();

	// run the postrecoveryboot script here
	TWFunc::RunFoxScript("/system/bin/postrecoveryboot.sh", "");
#ifndef OF_DEVICE_WITHOUT_PERSIST
	DataManager::RestorePasswordBackup();
#endif

#ifndef TW_OEM_BUILD
	// Disable flashing of stock recovery
	TWFunc::Disable_Stock_Recovery_Replace();
#endif
}

static void reboot() {
	gui_msg(Msg("rebooting=Rebooting..."));
	TWFunc::Update_Log_File();
	string Reboot_Arg;

#ifdef TW_INCLUDE_CRYPTO
//	fscrypt_lock_user_key(0);
#endif
	DataManager::GetValue("tw_reboot_arg", Reboot_Arg);
	if (Reboot_Arg == "recovery")
		TWFunc::tw_reboot(rb_recovery);
	else if (Reboot_Arg == "poweroff")
		TWFunc::tw_reboot(rb_poweroff);
	else if (Reboot_Arg == "bootloader")
		TWFunc::tw_reboot(rb_bootloader);
	else if (Reboot_Arg == "download")
		TWFunc::tw_reboot(rb_download);
	else if (Reboot_Arg == "edl")
		TWFunc::tw_reboot(rb_edl);
	else if (Reboot_Arg == "fastboot")
		TWFunc::tw_reboot(rb_fastboot);
	else
		TWFunc::tw_reboot(rb_system);
}

// The native UI intentionally abandons its Qualcomm EGL objects for terminal
// actions because destroying a live scanout context can abort inside the
// vendor driver.  A hardware reboot releases those objects in the kernel, but
// an exec-based mode switch does not close descriptors lacking FD_CLOEXEC.
// Mark every recovery-owned descriptor so exec releases DRM/EGL, input,
// Binder and old USB endpoints atomically, without invoking vendor cleanup.
static bool Aera_Close_Runtime_Fds_On_Exec() {
 constexpr unsigned int kCloseRangeCloexec = 1U << 2;
 if (syscall(SYS_close_range, 3U, ~0U, kCloseRangeCloexec) == 0) {
  LOGINFO("AERA userspace handoff: runtime descriptors armed for exec cleanup.\n");
  return true;
 }

 const int close_range_error = errno;
 DIR* descriptors = opendir("/proc/self/fd");
 if (!descriptors) {
  LOGERR("AERA userspace handoff: could not enumerate runtime descriptors: %s\n",
   strerror(errno));
  return false;
 }

 const int descriptors_fd = dirfd(descriptors);
 bool success = true;
 errno = 0;
 while (dirent* entry = readdir(descriptors)) {
  char* end = nullptr;
  const long value = strtol(entry->d_name, &end, 10);
  if (!end || *end != '\0' || value <= STDERR_FILENO ||
   value == descriptors_fd)
   continue;

  const int fd = static_cast<int>(value);
  const int flags = fcntl(fd, F_GETFD);
  if (flags < 0) {
   if (errno != EBADF) success = false;
   continue;
  }
  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
   success = false;
 }
 closedir(descriptors);

 if (!success) {
  LOGERR("AERA userspace handoff: descriptor cleanup could not be guaranteed.\n");
  return false;
 }
 LOGINFO("AERA userspace handoff: close_range unavailable (%s); used /proc fallback.\n",
  strerror(close_range_error));
 return true;
}

// Replace only recovery userspace when moving between recovery and fastbootd.
// The kernel, ramdisk, mounted userdata and framebuffer remain alive. A fresh
// recovery process rebuilds the partition manager, which is required because
// entering fastbootd deliberately destroys its mapped logical-partition
// objects. If exec fails, the caller falls back to the normal hardware reboot.
static bool Aera_Soft_Switch(bool fastboot_mode) {
	if (fastboot_mode &&
		!android::base::GetBoolProperty("ro.boot.dynamic_partitions", false)) {
		LOGERR("AERA userspace handoff: fastbootd is unavailable on this device.\n");
		return false;
	}

	LOGINFO("AERA userspace handoff: switching to %s without rebooting the device.\n",
		fastboot_mode ? "fastbootd" : "recovery");
	TWFunc::Update_Log_File();
	sync();

	// Detach the current FunctionFS endpoint before the replacement process
	// selects adb/MTP or fastboot. This also stops fastbootd on the return path.
	android::base::SetProperty("sys.usb.config", "none");
	usleep(200000);
	if (!Aera_Close_Runtime_Fds_On_Exec())
		return false;
	android::base::SetProperty(AERA_SOFT_SWITCH_PROP, "1");
	android::base::SetProperty(TW_FASTBOOT_MODE_PROP,
		fastboot_mode ? "1" : "0");

	char recovery[] = "/system/bin/recovery";
	char fastboot[] = "--fastboot";
	char transient[] = "--aera-soft-switch";
	char *fastboot_argv[] = {recovery, fastboot, transient, nullptr};
	char *recovery_argv[] = {recovery, transient, nullptr};
	execv(recovery, fastboot_mode ? fastboot_argv : recovery_argv);

	LOGERR("AERA userspace handoff: exec failed: %s\n", strerror(errno));
	android::base::SetProperty(AERA_SOFT_SWITCH_PROP, "0");
	return false;
}

// check whether we should reload the themes
static bool Fox_CheckReload_Themes() {
  if (DataManager::GetStrValue("data_decrypted") == "1" 
  || DataManager::GetIntValue(TW_IS_FBE) == 1 
  || TWFunc::Fox_Property_Get("aera.mount_to_decrypt") == "1") {
	DataManager::SetValue(AERA_ENCRYPTED_DEVICE, "1");
    }
#if defined(OF_ALLOW_EARLY_SETTINGS_LOAD) && defined(OF_SETTINGS_ROOT_DIRECTORY)
  return false;
#else
  return (TWFunc::Path_Exists(AERA_THEME_PATH) || TWFunc::Path_Exists(AERA_NAVBAR_PATH));
#endif
}

// remove a problem file injected into vAB recovery ramdisk by some QPR2 ROMs
static void Fox_Remove_Problematic_File() {
#ifdef AERA_VIRTUAL_AB_DEVICE
	std::string f = "/system/etc/vintf/manifest/boot-service.qti.xml";
	android::base::RemoveFileIfExists(f);
#endif
}

// main()
int main(int argc, char **argv) {
	// avoid Android 15 QPR2 problem with rogue xml file
	Fox_Remove_Problematic_File();

	// Recovery needs to install world-readable files, so clear umask
	// set by init
	umask(0);

	Log_Offset = 0;

	// Set up temporary log file (/tmp/recovery.log)
	freopen(TMP_LOG_FILE, "a", stdout);
	setbuf(stdout, NULL);
	freopen(TMP_LOG_FILE, "a", stderr);
	setbuf(stderr, NULL);

	signal(SIGPIPE, SIG_IGN);

	// Handle ADB sideload
	if (argc == 3 && strcmp(argv[1], "--adbd") == 0) {
		property_set("ctl.stop", "adbd");
#ifdef TW_USE_NEW_MINADBD
		//adb_server_main(0, DEFAULT_ADB_PORT, -1); TODO fix this for android8
		// minadbd_main();
#else
		adb_main(argv[2]);
#endif
		return 0;
	}

#ifdef RECOVERY_SDCARD_ON_DATA
	datamedia = true;
#endif

	// Fox stuff
	TWFunc::Set_Sbin_Dir_Executable_Flags();
	property_set("ro.aera.boot", "1");
	property_set("ro.aera.type", AERA_BUILD_STATUS);
	property_set("ro.aera.status", AERA_BUILD_STATUS);
	property_set("ro.aera.variant", AERA_VARIANT);
	property_set("ro.aera.build", "aera");
	property_set("ro.aera.release.version", AERA_BUILD);
	property_set("ro.aera.build.status", AERA_BUILD_STATUS);
	property_set("ro.aera.release.channel", AERA_BUILD_TYPE);
	TWFunc::Fox_Property_Set("ro.aera.boot.header.version", std::to_string(BOARD_BOOT_HEADER_VERSION));

	#ifdef AERA_TARGET_DEVICES
	property_set("ro.twrp.target.devices", AERA_TARGET_DEVICES);
  	#endif

	#ifdef OF_DYNAMIC_FULL_SIZE
	TWFunc::Fox_Property_Set("ro.dynamic.full_size", OF_DYNAMIC_FULL_SIZE);
	#endif

	#ifdef OF_SKIP_FBE_DECRYPTION
	TWFunc::Fox_Property_Set("of_skip_fbe_decryption", "true");
	#endif

	#ifdef OF_DEFAULT_KEYMASTER_VERSION
	android::base::SetProperty(TW_KEYMASTER_VERSION_PROP, OF_DEFAULT_KEYMASTER_VERSION);
	#endif

	#ifdef AERA_VIRTUAL_AB_DEVICE
	property_set("ro.aera.virtual_ab", "1");
	#endif

	#ifdef AERA_VANILLA_BUILD
	property_set("ro.aera.vanilla", "1");
	#endif

	#ifdef TW_INCLUDE_CRYPTO
	property_set("ro.aera.crypto_enabled", "1");
	#else
	property_set("ro.aera.crypto_enabled", "0");
	#endif

	#ifdef OF_DONT_SUBSTITUTE_PERMISSIONS
	property_set("ro.aera.substitute_permissions", "0");
	#else
	property_set("ro.aera.substitute_permissions", "1");
	#endif

	string aera_cfg = Aera_Cfg;
	if (!TWFunc::Path_Exists(aera_cfg))
	    aera_cfg = "/system" + Aera_Cfg;
	string aera_build_date = TWFunc::File_Property_Get (aera_cfg, "AERA_BUILD_DATE");
	if (aera_build_date == "") {
		aera_build_date = TWFunc::File_Property_Get ("/default.prop", "ro.bootimage.build.date");
		if (aera_build_date == "") {
			aera_build_date = TWFunc::File_Property_Get ("/default.prop", "ro.build.date");
			if (aera_build_date == "")
				aera_build_date = "[no date!]";
         	}
     	}
		 
	DataManager::SetValue("AERA_BUILD_DATE_REAL", aera_build_date);

  	// Set the start date to the recovery's build date
  	TWFunc::Reset_Clock();

	DataManager::GetValue(AERA_COMPATIBILITY_DEVICE, Aera_Current_Device);
	printf("Starting AERA Recovery %s [%s, core: %s] (built on %s for %s [dev_ver: %s (branch: %s)]; pid %d)\n",
		AERA_BUILD, AERA_VARIANT, AERA_MAIN_VERSION_STR, aera_build_date.c_str(), Aera_Current_Device.c_str(), AERA_CURRENT_DEV_STR, OF_CURRENT_BRANCH, getpid());

	// refresh the specific device codename if we have a generic unified codename
	TWFunc::Fox_Set_Current_Device_CodeName();

	// Load default values to set DataManager constants and handle ifdefs
	DataManager::SetDefaultValues();

	startupArgs startup;
	startup.parse(&argc, &argv);
	android::base::SetProperty(TW_FASTBOOT_MODE_PROP, startup.Get_Fastboot_Mode() ? "1" : "0");
	android::base::SetProperty(AERA_SOFT_SWITCH_PROP,
		startup.Get_Aera_Soft_Switch() ? "1" : "0");
	printf("=> Linking mtab\n");
	symlink("/proc/mounts", "/etc/mtab");
	std::string fstab_filename = "/etc/twrp.fstab";
	if (!TWFunc::Path_Exists(fstab_filename)) {
		fstab_filename = "/etc/recovery.fstab";
	}
	printf("=> Processing %s\n", fstab_filename.c_str());
	if (!PartitionManager.Process_Fstab(fstab_filename, 1, !startup.Get_Fastboot_Mode())) {
		LOGERR("Failing out of recovery due to problem with fstab.\n");
		return -1;
	}

	// Set the props for OrangeFox dynamic partitions
	//PartitionManager.Fox_Set_Dynamic_Partition_Props();

	// start the UI
	printf("Starting the UI...\n");
	gui_init();

	if (!startup.Get_Fastboot_Mode()) PartitionManager.Setup_Fstab_Partitions(true);

	// Load up all the resources
	gui_loadResources();

	std::string value;
	static char charging = ' ';
	static int lastVal = -1;

	// Function to monitor battery in the background
	auto monitorBatteryInBackground = [&]() {
		while (true) {
#ifdef TW_USE_LEGACY_BATTERY_SERVICES
			char cap_s[4];
#ifdef TW_CUSTOM_BATTERY_PATH
			string capacity_file = EXPAND(TW_CUSTOM_BATTERY_PATH);
			capacity_file += "/capacity";
			FILE * cap = fopen(capacity_file.c_str(),"rt");
#else
			FILE * cap = fopen("/sys/class/power_supply/battery/capacity","rt");
#endif
			if (cap) {
				fgets(cap_s, 4, cap);
				fclose(cap);
				lastVal = atoi(cap_s);
				if (lastVal > 100)	lastVal = 101;
				if (lastVal < 0)	lastVal = 0;
			}
#ifdef TW_CUSTOM_BATTERY_PATH
			string status_file = EXPAND(TW_CUSTOM_BATTERY_PATH);
			status_file += "/status";
			cap = fopen(status_file.c_str(),"rt");
#else
			cap = fopen("/sys/class/power_supply/battery/status","rt");
#endif
			if (cap) {
				fgets(cap_s, 2, cap);
				fclose(cap);
				if (cap_s[0] == 'C')
					charging = '+';
				else
					charging = ' ';
			}
#else // TW_USE_LEGACY_BATTERY_SERVICES
			auto battery_info = GetBatteryInfo();
			if (battery_info.charging) {
				charging = '+';
			} else {
				charging = ' ';
			}
			lastVal = battery_info.capacity;
#endif // TW_USE_LEGACY_BATTERY_SERVICES
			// Format the value based on the background updates
			value = std::to_string(lastVal);

			DataManager::SetValue("tw_battery_charge", value + "%" + charging);
			DataManager::SetValue("tw_battery", value);
			DataManager::SetValue("charging_now", (charging == '+') ? "1" : "0");

			// Check usb_otg status
			PartitionManager.Check_UsbOtg_Status();

			// Sleep for a specified interval (e.g., 1 second) before checking again
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	};

	// Create a thread for battery monitoring
	static std::thread battery_monitor(monitorBatteryInBackground);

	twrpAdbBuFifo *adb_bu_fifo = new twrpAdbBuFifo();
	TWFunc::Clear_Bootloader_Message();

	if (startup.Get_Fastboot_Mode()) {
		process_fastbootd_mode();
		delete adb_bu_fifo;
		TWFunc::Update_Intent_File(startup.Get_Intent());
		reboot();
		return 0;
	} else {
		process_recovery_mode(
			adb_bu_fifo,
			startup.Should_Skip_Decryption() || startup.Get_Aera_Soft_Switch(),
			startup.Get_Aera_Soft_Switch());
	}
	// Fox extra setup
  	TWFunc::Setup_Verity_Forced_Encryption();

	// Launch the native AERA interface. It owns settings and language reloads;
	// there is no XML package to refresh.
	if (Fox_CheckReload_Themes()) {
		DataManager::SetValue("of_reload_back", "main");
	}
	gui_start();

	delete adb_bu_fifo;
	TWFunc::Update_Intent_File(startup.Get_Intent());

	reboot();

	return 0;
}
