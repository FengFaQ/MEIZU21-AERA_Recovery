/*
	Copyright (C) 2020-2025 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
	
	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
	
	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with OrangeFox.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <unistd.h>
#include <iostream>
#include <fstream>

#include <string.h>
#include <stdio.h>
#include <cutils/properties.h>

#include <android-base/unique_fd.h>

#include "twcommon.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"

#include "otautil/sysutil.h"
#include <ziparchive/zip_archive.h>
#include "install/include/install/install.h"
#include "variables.h"
#include "data.hpp"
#include "partitions.hpp"
#include "twrpDigestDriver.hpp"
#include "twrpDigest/twrpDigest.hpp"
#include "twrpDigest/twrpMD5.hpp"
#include "twrp-functions.hpp"
#include "aeraui/platform/aera_ui_host.hpp"
#include "aeraui/platform/aera_screen_timer.hpp"
#include "aera_core.hpp"
#include "twrpRepacker.hpp"

// #include "legacy_property_service.h"

#include "twinstall.h"
#include "installcommand.h"
extern "C" {
	#include "aeraui/platform/aera_ui_host.h"
}

bool storage_is_encrypted()
{
  return DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0;
}

bool ors_is_active()
{
  return DataManager::GetStrValue("tw_action") == "openrecoveryscript";
}


string Fox_CheckForAsserts(ZipArchiveHandle Zip)
{
string ret = "";
#ifdef AERA_TARGET_DEVICES
string devices = TWFunc::get_assert_device_zip(AERA_TMP_PATH, Zip);
  if (devices.empty())
       return ret;

  string tmpstr = TWFunc::Fox_Property_Get("ro.product.device");
 
  if (tmpstr.empty())
	tmpstr = DataManager::GetStrValue("fox_product_device");

  if (devices == tmpstr)
       return ret;

  LOGINFO("AssertDevice=[%s] and CurrentDevice=[%s]\n", devices.c_str(), tmpstr.c_str());
  std::vector<std::string> assertResults = TWFunc::Split_String(devices, ",");
  std::vector <std::string> devs = TWFunc::Split_String(AERA_TARGET_DEVICES, ",");

  for (const std::string& deviceAssert : assertResults) {
        std::string assertName = TWFunc::trim(deviceAssert);
        if (!assertName.empty() && assertName == tmpstr) {
            return ret;
        }
        // target_devices
        else {
           for(const std::string& FoxDevice_x : devs) {
               std::string FoxName = TWFunc::trim(FoxDevice_x);
               if (!FoxName.empty() && !assertName.empty() && assertName == FoxName) {
            	   return FoxName;
               }
           }
        }
    }
#endif
  return ret;
}

bool Fox_Support_All_OTA()
{
  #ifdef OF_SUPPORT_ALL_BLOCK_OTA_UPDATES
  DataManager::SetValue(AERA_MIUI_ZIP_TMP, 1);
  return true;
  #else
  return false;
  #endif
}

void Fox_ProcessAsserts(string assert_device)
{
  #ifdef AERA_TARGET_DEVICES
    if (!assert_device.empty())
       {
         if (TWFunc::Fox_Property_Set("ro.product.device", assert_device))
           {
       	     //gui_print_color("warning",
       	     LOGINFO("\nDevice name temporarily switched to \"%s\".\n\n", assert_device.c_str());
       	     DataManager::SetValue("fox_processing_asserts", "1");
       	     usleep (64000);
           }
       }
  #endif
}

void set_miui_install_status(std::string install_status, bool verify)
{
  if (ors_is_active())
    {
      std::string last_status = "/cache/recovery/last_status";
      if (!PartitionManager.Mount_By_Path("/cache", true))
	return;
      if (!verify)
	{
	  if (zip_is_survival_trigger || zip_is_for_specific_build)
	    {
	      if (TWFunc::Path_Exists(last_status))
		unlink(last_status.c_str());

	      ofstream status;
	      status.open(last_status.c_str());
	      status << install_status;
	      status.close();
	      chmod(last_status.c_str(), 0755);
	    }
	}
      else
	{
	  if (TWFunc::Path_Exists(last_status))
	    unlink(last_status.c_str());

	  ofstream status;
	  status.open(last_status.c_str());
	  status << install_status;
	  status.close();
	  chmod(last_status.c_str(), 0755);
	}
    }
}

bool is_comment_line(const string Src)
{
  string str = TWFunc::trim(Src);
  return (str.front() == '#');
}

bool Fox_Skip_OTA() 
{
   #if defined(OF_DISABLE_MIUI_SPECIFIC_FEATURES)
   return true;
   #else
   return false;
   #endif
}

bool Fox_OTA_Backup_Stock_Boot_Image()
{
   #ifdef OF_OTA_BACKUP_STOCK_BOOT_IMAGE
   return true;
   #else
   return false;
   #endif
}

bool Fox_Fix_OTA_Update_Manual_Flash_Error()
{
   #ifdef OF_FIX_OTA_UPDATE_MANUAL_FLASH_ERROR
   return true;
   #else
   return false;
   #endif
}

bool Fox_OTA_RES_Check_MicroSD()
{
   #ifdef OF_OTA_RES_CHECK_MICROSD
   return true;
   #else
   return false;
   #endif
}

bool verify_incremental_package(string fingerprint, string metadatafp, string metadatadevice)
{
  if (metadatafp.size() > AERA_MIN_EXPECTED_FP_SIZE
      && fingerprint.size() > AERA_MIN_EXPECTED_FP_SIZE
      && metadatafp != fingerprint)
    return false;
  if (metadatadevice.size() >= 4
      && fingerprint.size() > AERA_MIN_EXPECTED_FP_SIZE
      && fingerprint.find(metadatadevice) == string::npos)
    return false;
  return (metadatadevice.size() >= 4
	  && metadatafp.size() > AERA_MIN_EXPECTED_FP_SIZE
	  && metadatafp.find(metadatadevice) == string::npos) ? false : true;
}

int TWinstall_Run_OTA_BAK (bool reportback) 
{
int result = 0;
#ifdef AERA_VANILLA_BUILD
   LOGINFO("- AERA: DEBUG: skipping the OTA_BAK process...\n");
   return result;
#endif
      if ((DataManager::GetIntValue(AERA_MIUI_ZIP_TMP) != 0) || (DataManager::GetIntValue(AERA_METADATA_PRE_BUILD) != 0))
      {
	  string ota_folder, ota_backup, loadedfp;
	  DataManager::GetValue(AERA_SURVIVAL_FOLDER_VAR, ota_folder);
	  DataManager::GetValue(AERA_SURVIVAL_BACKUP_NAME, ota_backup);
	  DataManager::GetValue(AERA_LOADED_FINGERPRINT, loadedfp);
	  ota_folder += "/" + ota_backup;
	  string ota_info = ota_folder + Aera_OTA_info;
	  if (TWFunc::Verify_Loaded_OTA_Signature(loadedfp, ota_folder)) {
	        gui_msg ("fox_incremental_ota_bak_skip=Detected OTA survival with the same ID - leaving");
		return result;
	    }
	    
	    if (TWFunc::Path_Exists(ota_folder))
		    TWFunc::removeDir(ota_folder, false);

	    DataManager::SetValue(AERA_RUN_SURVIVAL_BACKUP, 1);
	    gui_msg ("fox_incremental_ota_bak_run=Starting OTA_BAK process...");

	    if (PartitionManager.Run_OTA_Survival_Backup(false)) {
	           result = 1;
	           DataManager::SetValue("ota_bak_folder", ota_folder);
	           set_miui_install_status(OTA_SUCCESS, false);    
	           gui_msg("fox_incremental_ota_bak=Process OTA_BAK --- done!");
	           Aera_Zip_Installer_Code = DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE);
	           usleep(1024);
	           if (reportback)
	           {
		Aera_Zip_Installer_Code = DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE);
	           	// MIUI: 2, 3, 22, 23
			if (Aera_Zip_Installer_Code == 22) // Treble MIUI
		   DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 23);
	           	else
			if (Aera_Zip_Installer_Code == 2) // non-Treble MIUI
		   DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 3);
	           	else
	           	// custom: 1, 11, 12, 13
			if (Aera_Zip_Installer_Code == 11) // Treble Custom
		   DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 12);
	           	else
		   DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 13); // non-Treble custom
			usleep(1024);
		Aera_Zip_Installer_Code = DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE);
			LOGINFO("OTA_BAK status: [code=%i]\n", Aera_Zip_Installer_Code);
	           }
	           else
	           {
		DataManager::SetValue(AERA_RUN_SURVIVAL_BACKUP, 0);
	           }
	        }
	    else
	        {
		   set_miui_install_status(OTA_ERROR, false);
	           gui_print("Process OTA_BAK --- FAILED!\n");
	           LOGERR("OTA_BAK: Unable to finish OTA_BAK!\n");
	        }  

	    if ((TWFunc::Path_Exists(ota_folder)) && (!TWFunc::Path_Exists(ota_info)))
		{
		   TWFunc::create_fingerprint_file(ota_info, loadedfp);
		}
      } // AERA_MIUI_ZIP_TMP
      return result;
}

#ifdef USE_MINZIP
bool zip_EntryExists(ZipArchive Zip, const string& filename) 
{
  const ZipEntry* file_location = mzFindZipEntry(&Zip, filename.c_str());
  if (file_location != NULL)
  	return true;
  return false;
}
#else
bool zip_EntryExists(ZipArchiveHandle Zip, const string& filename) 
{
  std::string_view zip_string(filename.c_str());
  ZipEntry64 file_entry;
  if (FindEntry(Zip, zip_string, &file_entry) != 0)
	return false;
  return true;
}
#endif

#ifdef USE_MINZIP
bool zip_ExtractEntry(ZipArchive Zip, const string& source_file, const string& target_file, mode_t mode)
#else
bool zip_ExtractEntry(ZipArchiveHandle Zip, const string& source_file, const string& target_file, mode_t mode)
#endif
{
	unlink(target_file.c_str());
	int fd = creat(target_file.c_str(), mode);
	if (fd == -1) {
		return false;
	}
#ifdef USE_MINZIP
	const ZipEntry* file_entry = mzFindZipEntry(&Zip, source_file.c_str());
	if (file_entry == NULL) {
		printf("'%s' does not exist in zip '%s'\n", source_file.c_str(), zip_file.c_str());
		close(fd); // ??
		return false;
	}
	int ret_val = mzExtractZipEntryToFile(&Zip, file_entry, fd);
	close(fd);

	if (!ret_val) {
		printf("Could not extract '%s'\n", target_file.c_str());
		return false;
	}
#else
	std::string_view zip_string(source_file.c_str());
	ZipEntry64 file_entry;

	if (FindEntry(Zip, zip_string, &file_entry) != 0) {
		close(fd); // ??
		return false;
	}
	int32_t ret_val = ExtractEntryToFile(Zip, &file_entry, fd);
	close(fd);

	if (ret_val != 0) {
		printf("Could not extract '%s'\n", target_file.c_str());
		return false;
	}
#endif
	return true;
}

bool find_in_zip_installer(const string path, const string fname)
{
string str = TWFunc::find_phrase(path, fname);
	if (!str.empty() && !is_comment_line(str))
		return true;
	else
		return false;
}

bool Installing_ROM_Query(const string path, ZipArchiveHandle Zip)
{
bool boot_install = false;
string str = "";
string tmp = "";
  if (!TWFunc::Path_Exists(path))
     return false;

  // full ROM or block-based OTA
  if (TWFunc::CheckWord(path, "block_image_update") || TWFunc::CheckWord(path, "block_image_recover"))
  	return true;

  // check for certain ROM files
  if ((zip_EntryExists(Zip, "system.new.dat") || zip_EntryExists(Zip, "system.new.dat.br") || zip_EntryExists(Zip, "system.patch.dat") || zip_EntryExists(Zip, "system_ext.new.dat.br"))
  && zip_EntryExists(Zip, "system.transfer.list"))
     {
	if ((zip_EntryExists(Zip, "vendor.new.dat") || zip_EntryExists(Zip, "vendor.patch.dat") || zip_EntryExists(Zip, "vendor.new.dat.br"))
	&& zip_EntryExists(Zip, "vendor.transfer.list"))
  		return true;
     }

  // check for file-based OTA - make several checks
  usleep(1024);
  int i = 0;
  tmp = "boot.img";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && (!is_comment_line(str))
  	&& str.find("package_extract_file") != string::npos
  	&& zip_EntryExists(Zip, tmp))
  	{
     	   i++;
     	   boot_install = true;
     	}

  usleep(1024);
  tmp = "/dev/block/bootdevice/by-name/system";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && (!is_comment_line(str))
  	&& str.find("mount") != string::npos
  	&& str.find("EMMC") != string::npos
  	&& str.find("/system") != string::npos
  	&& zip_EntryExists(Zip, "system/build.prop"))
     i++;

  usleep(1024);
  tmp = "/dev/block/bootdevice/by-name/vendor";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && (!is_comment_line(str))
  	&& str.find("mount") != string::npos
  	&& str.find("EMMC") != string::npos
  	&& str.find("/vendor") != string::npos
  	&& zip_EntryExists(Zip, "vendor/build.prop"))
     i++;

  usleep(1024);
  if (TWFunc::CheckWord(path, "assert(update_dynamic_partitions(package_extract_file") 
    && TWFunc::CheckWord(path, "dynamic_partitions_op_list")
    && zip_EntryExists(Zip, "dynamic_partitions_op_list"))
  	i++;

  usleep(1024);
  if (zip_EntryExists(Zip, "system.transfer.list") && TWFunc::CheckWord(path, "system.transfer.list")
  && zip_EntryExists(Zip, "vendor.transfer.list") && TWFunc::CheckWord(path, "vendor.transfer.list")
  && zip_EntryExists(Zip, "product.transfer.list") && TWFunc::CheckWord(path, "product.transfer.list"))
  	i++;

  // if all these are true, then no need to go further
  usleep(1024);
  if (i > 2)
    return true;

  // boot image flash? else return false
  #if !defined(AB_OTA_UPDATER) && !defined(AERA_AB_DEVICE)
  if (!boot_install)
     return false;
  #endif

  // continue: now, we are in the legacy methods zone
  usleep(1024);
  tmp = "package_extract_dir(\"system\"";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && !is_comment_line(str))
     i++;

  usleep(1024);
  tmp = "package_extract_dir(\"vendor\"";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && !is_comment_line(str))
     i++;

  usleep(1024);
  tmp = "package_extract_file(\"firmware-update";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && !is_comment_line(str))
      i++;

  usleep(1024);
  tmp = "META-INF/com/miui/miui_update";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && !is_comment_line(str) && zip_EntryExists(Zip, tmp))
      i++;

  if (i > 3)
      return true;

  // look for other signs of legacy ROM installation methods
  usleep(1024);
  tmp = "/dev/block/bootdevice/by-name/cust";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && (!is_comment_line(str))
  	&& str.find("mount") != string::npos
  	&& str.find("EMMC") != string::npos
  	&& str.find("/cust") != string::npos)
     i++;
  
  usleep(1024);
  tmp = "/dev/block/bootdevice/by-name/cache";
  str = TWFunc::find_phrase(path, tmp);
  if (!str.empty() && (!is_comment_line(str))
  	&& str.find("mount") != string::npos
  	&& str.find("EMMC") != string::npos
  	&& str.find("/cache") != string::npos)
     i++;
  
  usleep(1024);
  str = TWFunc::find_phrase(path, "format(");
  if (!str.empty() && (!is_comment_line(str))
  	&& str.find("EMMC") != string::npos
  	&& (str.find("/system") != string::npos || str.find("/vendor") != string::npos || str.find("/cache") != string::npos || str.find("/cust") != string::npos)
  	)
     i++;

  if (i > 3)
      return true;
 
  // more checks for old file-based ROM installers
  usleep(1024);
  if (zip_EntryExists(Zip, "system/bin/sh") && zip_EntryExists(Zip, "system/etc/hosts"))
    i++;

  if (zip_EntryExists(Zip, "system/media/bootanimation.zip") && zip_EntryExists(Zip, "system/vendor/bin/perfd"))
    i++;

  if (zip_EntryExists(Zip, "system/priv-app/Browser/Browser.apk") && zip_EntryExists(Zip, "system/priv-app/FindDevice/FindDevice.apk"))
    i++;
 
  if (i > 3)
      return true;

  // if this is not an A/B device, then stop here
  #if !defined(AB_OTA_UPDATER) && !defined(AERA_AB_DEVICE)
	return false;
  #endif

  // deal with recent non-standards-compliant xiaomi.eu/hyperOS/other vAB zip installers
  i = 0;
  int j = 0;
  int k = 0;
  const int threshold_std = 7; // standard threshold
  const int threshold_higher = 9; // set a higher threshold for plain file searches (in "images/" and "firmware-update/")

	// file names found in typical non-standard ROM installers
	std::vector<string> img_names = {
		"super.img", "super.img.0", "vendor_boot.img", "boot.img", "init_boot.img",
		"vbmeta_system.img", "vbmeta_vendor.img", "vbmeta.img", "bluetooth.img", "cust.img",
		"keymaster.img", "dtbo.img", "cmnlib64.img", "xbl.img", "abl.img", "devcfg.img",
		 "custom.img", "userdata.img", "logo.img", "super_dummy.img", "recovery.img", "twrp.img"};

	std::vector<string> img_names_alt = {"abl.elf", "xbl.elf", "xbl_config.elf", "devcfg.mbn", "cmnlib64.mbn"};

	std::vector<string> cmd_names = {"package_extract_file", "package_flash_partition", "package_unsparse_file", "/system/bin/bootctl", "rm -f /data/cache/command"};

	// iterate through names
	for (auto fname : img_names) {

		// search for file existence in root, plus installation entry in updater-script
		if (zip_EntryExists(Zip, fname) && find_in_zip_installer(path, fname)) {
			i++;
		}
		else
		// search for file existence in images/ plus installation entry in updater-script
		if (zip_EntryExists(Zip, "images/" + fname) && find_in_zip_installer(path, "images/" + fname)) {
			i++;
		}
		else
		// search for file existence in firmware-update/
		if (zip_EntryExists(Zip, "firmware-update/" + fname)) {
			j++;
		}

		// if we pass a threshold, break
		if (i > threshold_std || j > threshold_higher) {
			break;
		}
	}

	// if we didn't reach either threshold, carry out more searches
	if (i < threshold_std && j < threshold_higher) {
		// search for some commands
		for (auto cmd : cmd_names) {
			if (find_in_zip_installer(path, cmd)) {
				i++;
			}
			if (i > threshold_std)
				break;
		}

		// search for some alternative names
		for (auto fname : img_names_alt) {
			if (zip_EntryExists(Zip, "firmware-update/" + fname)) {
				j++;
			}
			if (j > threshold_higher)
				break;
		}

		// search for file existence only in images/
		for (auto fname : img_names) {
			if (zip_EntryExists(Zip, "images/" + fname)) {
				k++;
			}
			if (k > threshold_higher)
				break;
		}
	}

	usleep(1024);

	// debug
	//#define DEBUGGING_CHECK 1
	#ifdef DEBUGGING_CHECK
		gui_print_color("warning", "DEBUG:images/=%i\nfirmware-update/=%i\nlast_resort=%i\n\n", i, j, k);
	#endif

	// if we crossed a relevant threshold, return true
	if (i > threshold_std || j > threshold_higher || k > threshold_higher) {
		// mark this as a non-standard vAB ROM installer using legacy methods - possibly a MIUI/HyperOS port or some other ROM installer
		DataManager::SetValue("found_non_standard_vAB_install", "1");
		gui_print("Non-standard vAB installer\n");
		return true;
	}
	else
		return false;
}

int Fox_Prepare_Update_Binary(const char *path, ZipArchiveHandle Zip) 
{
  string metadata_sg_path = "META-INF/com/android/metadata";
  string fingerprint_property = "ro.build.fingerprint";
  string pre_device = "pre-device";
  string pre_build = "pre-build";
  string mCheck = "";
  string miui_check1 = "ro.miui.ui.version";
  string check_command = "grep " + miui_check1 + " " + TMP_UPDATER_BINARY_PATH;
  string zip_name = path;
  int is_new_miui_update_binary = 0;
  string assert_device = "";
  zip_is_rom_package = false; 		// assume we are not installing a ROM
  zip_is_survival_trigger = false; 	// assume non-miui
  support_all_block_ota = false; 	// non-MIUI block-based OTA updates
  zip_is_for_specific_build = false;
  DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 0); // assume standard zip installer
  DataManager::SetValue(AERA_ZIP_INSTALLER_TREBLE, "0");
  DataManager::SetValue("found_fox_overwriting_rom", "0");
  TWFunc::Fox_Property_Set("found_fox_overwriting_rom", "");
  DataManager::SetValue("found_non_standard_vAB_install", "0");
  if (TWFunc::Block_Operations_Until_Reboot())
	return INSTALL_ERROR;

  if (DataManager::GetIntValue(AERA_INSTALL_PREBUILT_ZIP) != 1)
    {
      DataManager::SetValue(AERA_METADATA_PRE_BUILD, 0);
      DataManager::SetValue(AERA_MIUI_ZIP_TMP, 0);
      DataManager::SetValue(AERA_RUN_SURVIVAL_BACKUP, 0);
      DataManager::SetValue(AERA_INCREMENTAL_OTA_FAIL, 0);
      DataManager::SetValue(AERA_LOADED_FINGERPRINT, 0);

      gui_msg("fox_install_detecting=Detecting Current Package");
      
      if (zip_EntryExists(Zip, UPDATER_SCRIPT))
	{
	  if (zip_ExtractEntry(Zip, UPDATER_SCRIPT, AERA_TMP_PATH, 0644))
	    {
	      if (Installing_ROM_Query(AERA_TMP_PATH, Zip))
	        {
		  zip_is_rom_package = true;
		  DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 1); // standard ROM
		  
	          // check for embedded recovery installs
	          if  (
	                 (TWFunc::CheckWord(AERA_TMP_PATH, "/dev/block/bootdevice/by-name/recovery")
	              && (TWFunc::CheckWord(AERA_TMP_PATH, "recovery.img") || TWFunc::CheckWord(AERA_TMP_PATH, "twrp.img"))
	              && (zip_EntryExists(Zip, "recovery.img") || zip_EntryExists(Zip, "twrp.img") || zip_EntryExists(Zip, "recovery/twrp.img") || zip_EntryExists(Zip, "recovery/recovery.img"))
	                 )) {
	                  DataManager::SetValue("found_fox_overwriting_rom", "1");
	                  TWFunc::Fox_Property_Set("found_fox_overwriting_rom", "1");
	                  usleep(32);
	                  TWFunc::Check_OrangeFox_Overwrite_FromROM(true, path);
	             }
		} 
	      assert_device = Fox_CheckForAsserts(Zip);
	      unlink(AERA_TMP_PATH);
	    } 
	} 

   // try to identify MIUI ROM installer
      if (zip_is_rom_package == true) 
         {
            if (zip_EntryExists(Zip, AERA_MIUI_UPDATE_PATH)) // META-INF/com/miui/miui_update - if found, then this is a miui zip installer
              {
                zip_is_survival_trigger = true;
                support_all_block_ota = true;
                LOGINFO("AERA: Detected miui_update file [%s]\n", AERA_MIUI_UPDATE_PATH);
              }
            else
            if (zip_EntryExists(Zip, AERA_MIUI_UPDATE_PATH_EU) // META-INF/com/xiaomieu/xiaomieu.sh - if found, then this is a xiaomi.eu zip installer
            || (DataManager::GetStrValue("found_non_standard_vAB_install") == "1")) // some other non-standard ROM installer
              {
		zip_is_survival_trigger = true;
                if (zip_EntryExists(Zip, AERA_MIUI_UPDATE_PATH_EU)) {
			support_all_block_ota = true;
			LOGINFO("AERA: Detected xiaomi.eu file [%s]\n", AERA_MIUI_UPDATE_PATH_EU);
		} else {
			// this is some other non-standard ROM installer - do nothing
		}
              }
            else // do another check for miui
             {
              mCheck = TWFunc::Exec_With_Output(check_command);  // check for miui in update-binary             
              if (mCheck.size() > 0)
                { 
                  LOGINFO("AERA: the answer that I received is: [%s]\n",mCheck.c_str());
                  int not_found = mCheck.find("not found");
                  if (not_found == -1) // then we are miui
                    {    
                       LOGINFO("AERA: Detected new Xiaomi update-binary [Message=%s]\n", mCheck.c_str());
                       is_new_miui_update_binary = 1;
                       zip_is_survival_trigger = true;
                       support_all_block_ota = true;
                    }
                   else 
                     {
                        LOGINFO("AERA: Received a response from [%s], but did not detect a new Xiaomi update-binary -Message=[%s] and code=[%i]\n",
                    	    check_command.c_str(), mCheck.c_str(), not_found);
                     }
                } // mCheck.size > 0
              else
                {
                   LOGINFO("AERA: The output of [%s] came out empty\n", check_command.c_str());
                }  
             }
       }
   //

   if (zip_is_rom_package == true) 
   {
      if (zip_is_survival_trigger == true) // MIUI installer?
         {
		DataManager::SetValue(AERA_CALL_DEACTIVATION, 1);
		DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 2); // MIUI ROM?
		if (DataManager::GetStrValue("found_non_standard_vAB_install") == "1") { // some non-standard ROM installer (but don't change the AERA_ZIP_INSTALLER_CODE from MIUI)
			gui_msg ("fox_install_standard_detected=- Detected a non-standard A/B ROM installer");
			support_all_block_ota = Fox_Support_All_OTA();
		} else {
			gui_msg ("fox_install_miui_detected=- Detected MIUI Update Package");
			DataManager::SetValue(AERA_MIUI_ZIP_TMP, 1);
			support_all_block_ota = true;
		}
         }
      else
         {
	       DataManager::SetValue(AERA_CALL_DEACTIVATION, 1);
	       DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 1); // standard ROM
	       gui_msg ("fox_install_standard_detected=- Detected standard ROM zip installer");
	       support_all_block_ota = Fox_Support_All_OTA();
         }   
   }

   DataManager::SetValue("found_non_standard_vAB_install", "0");

   // treble ROM ?
   if (zip_is_rom_package == true) 
    {
      if ((zip_EntryExists(Zip, "vendor.new.dat")) || (zip_EntryExists(Zip, "vendor.new.dat.br"))) // we are installing a Treble ROM
         {
           DataManager::SetValue(AERA_ZIP_INSTALLER_TREBLE, "1");
           Aera_Zip_Installer_Code = DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE);
           usleep (32);
           
           if (Aera_Zip_Installer_Code == 1) // custom
                 DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 11); // custom Treble ROM
           
           if (Aera_Zip_Installer_Code == 2) // miui
                 DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 22); // miui Treble ROM
           
           Aera_Zip_Installer_Code = DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE);
           LOGINFO("AERA: detected Treble ROM installer. [code=%i] \n", Aera_Zip_Installer_Code);
        }
        else 
        if (TWFunc::Has_Vendor_Partition())
         {
           DataManager::SetValue(AERA_ZIP_INSTALLER_TREBLE, "1");
           Aera_Zip_Installer_Code = DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE);
           usleep (32);
           
           if (Aera_Zip_Installer_Code == 1) // custom
                 DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 11);
           
           if (Aera_Zip_Installer_Code == 2) // miui
                 DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 22);
           
           Aera_Zip_Installer_Code = DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE);
           LOGINFO("AERA: detected standard ROM installer, on a real Treble device!\n");
         }
    
    	if (Fox_OTA_Backup_Stock_Boot_Image())
    	  {
    	    if (support_all_block_ota && zip_EntryExists(Zip, "boot.img"))
      	     {
      		if (zip_ExtractEntry(Zip, "boot.img", boot_bak_img, 0644))
      		 {
      	    	   LOGINFO("Making a temporary copy of the stock boot image.\n");
      	   	}
      	     }
      	 }
    }
   // treble

   // do we need to do an OTA_RES first?
   if (Fox_Skip_OTA()) // no
   {
     // LOGINFO("OrangeFox: not executing incremental OTA restore (OTA_RES)\n");   
   } 
   else
   {
     if (zip_is_survival_trigger || support_all_block_ota)
	{
	  if (DataManager::GetIntValue(AERA_INCREMENTAL_PACKAGE) != 0)
	    gui_msg
	       ("fox_incremental_ota_status_enabled=Support Incremental package status: Enabled");
	  
	  if (zip_EntryExists(Zip, metadata_sg_path)) // META-INF/com/android/metadata is in zip
	    {
	      const string take_out_metadata = "/tmp/build.prop";
	      if (zip_ExtractEntry(Zip, metadata_sg_path, take_out_metadata, 0644))
		{
		  string metadata_fingerprint = TWFunc::File_Property_Get(take_out_metadata, pre_build); // look for "pre-build"
		  string metadata_device = TWFunc::File_Property_Get(take_out_metadata, pre_device);  // look for "pre-device"

		  string fingerprint = TWFunc::System_Property_Get(fingerprint_property); // try to get system fingerprint - ro.build.fingerprint
		  if (fingerprint.empty()) {
			fingerprint = TWFunc::Fox_Property_Get("aera.system.fingerprint");
			if (fingerprint.empty()) {
			    fingerprint = TWFunc::File_Property_Get(aera_runtime_cfg, "ROM_FINGERPRINT");
			}
		  }

		  // appropriate "pre-build" entry in META-INF/com/android/metadata ? == incremental block-based OTA zip installer
		  if (metadata_fingerprint.size() > AERA_MIN_EXPECTED_FP_SIZE)
		    {
		      gui_msg(Msg
			      ("fox_incremental_package_detected=Detected Incremental package '{1}'")
			      (path));
			      
		      if (DataManager::GetIntValue(AERA_INCREMENTAL_PACKAGE) == 0)
		      {
		  	CloseArchive(Zip);
		  	LOGERR("Incremental OTA is not enabled. Quitting the incremental OTA update.\n");
		  	set_miui_install_status(OTA_ERROR, false);
		  	return INSTALL_ERROR; 
		      }

		      // --- check for mismatching fingerprints when they should match
		      string metadata_prebuild_incremental = TWFunc::File_Property_Get(take_out_metadata, "pre-build-incremental");
		      string metadata_ota_type = TWFunc::File_Property_Get(take_out_metadata, "ota-type");
		      string aera_incremental = TWFunc::File_Property_Get(aera_runtime_cfg, "INCREMENTAL_VERSION");

		      if (metadata_fingerprint != fingerprint)
		      {
			    DataManager::GetValue(AERA_COMPATIBILITY_DEVICE, Aera_Current_Device);
			    if (metadata_device == Aera_Current_Device && metadata_prebuild_incremental == aera_incremental) {
				LOGINFO("- DEBUG: AERA: metadata_fingerprint != system_fingerprint. Trying to fix it.\n- Changing [%s] to [%s]\n",
           				fingerprint.c_str(), metadata_fingerprint.c_str());
       				string atmp = "\"";
       				usleep(4096);
				TWFunc::Exec_Cmd(Aera_ResetProp_Bin + " ro.build.fingerprint " + atmp + metadata_fingerprint + atmp);
       				usleep(250000);
				TWFunc::Exec_Cmd(Aera_ResetProp_Bin + " aera.system.fingerprint " + atmp + metadata_fingerprint + atmp);
       				usleep(100000);
   			     }
		        }
			// ---

		      zip_is_for_specific_build = true;
		      
		      DataManager::SetValue(AERA_METADATA_PRE_BUILD, 1);
		      
		      if ((fingerprint.size() > AERA_MIN_EXPECTED_FP_SIZE)
		      && (DataManager::GetIntValue("fox_verify_incremental_ota_signature") != 0))
			{
			  gui_msg
			    ("fox_incremental_ota_compatibility_chk=Verifying Incremental Package Signature...");

			    if (verify_incremental_package 
			         (fingerprint, 
			          metadata_fingerprint,
				  metadata_device))
			    {
			      gui_msg("fox_incremental_ota_compatibility_true=Incremental package is compatible.");
			      property_set(fingerprint_property.c_str(), metadata_fingerprint.c_str());
			      DataManager::SetValue(AERA_LOADED_FINGERPRINT, metadata_fingerprint);
			    }
			  else
			    {
			      set_miui_install_status(OTA_VERIFY_FAIL, false);
			      gui_err("fox_incremental_ota_compatibility_false=Incremental package isn't compatible with this ROM!");
			      return INSTALL_ERROR;
			    }
			}
		      else
			{
			  property_set(fingerprint_property.c_str(), metadata_fingerprint.c_str());
			}
		    
		      if (zip_is_for_specific_build)
			 {
			   if (Fox_Fix_OTA_Update_Manual_Flash_Error())
			   {
			   	if ((!ors_is_active()) && (zip_is_rom_package))
			       	   LOGERR("You must flash incremental OTA updates from the ROM's updater, because only the ROM can decrypt the zips.\n");
			   }
			 }			
		      unlink(take_out_metadata.c_str());		      
		    }
		}
	      else
		{
		  CloseArchive(Zip);
		  LOGERR("Could not extract '%s'\n", take_out_metadata.c_str());
		  set_miui_install_status(OTA_ERROR, false);
		  return INSTALL_ERROR;
		}
	    }
	}
      else
	{
            if (zip_is_rom_package == true) 	       
              gui_msg ("fox_incremental_ota_status_disabled=Support Incremental package status: Disabled");
	}

      string ota_location_folder, ota_location_backup, loadedfp;
      DataManager::GetValue(AERA_SURVIVAL_FOLDER_VAR, ota_location_folder);
      DataManager::GetValue(AERA_SURVIVAL_BACKUP_NAME, ota_location_backup);
      ota_location_folder += "/" + ota_location_backup;
      DataManager::GetValue(AERA_LOADED_FINGERPRINT, loadedfp);

      if (DataManager::GetIntValue(AERA_METADATA_PRE_BUILD) != 0
	  && !TWFunc::Verify_Loaded_OTA_Signature(loadedfp, ota_location_folder))
	{
	  TWPartition *survival_boot =
	    PartitionManager.Find_Partition_By_Path("/boot");

	  if (!survival_boot)
	    {
	      set_miui_install_status(OTA_ERROR, false);
	      LOGERR("OTA_Survival: Unable to find boot partition\n");
	      return INSTALL_ERROR;
	    }
	    
	  TWPartition *survival_sys =
	    PartitionManager.Find_Partition_By_Path(PartitionManager.Get_Android_Root_Path());
	    
	  if (!survival_sys)
	    {
	      set_miui_install_status(OTA_ERROR, false);
	      LOGERR("OTA_Survival: Unable to find system partition\n");
	      return INSTALL_ERROR;
	    }

	  std::string action;
	  DataManager::GetValue("tw_action", action);
	  if (action != "openrecoveryscript"
	      && DataManager::GetIntValue(AERA_MIUI_ZIP_TMP) != 0)
	    {
	      if (Fox_Fix_OTA_Update_Manual_Flash_Error())
	      {
	    	std::string cachefile = TWFunc::get_log_dir(); //"/cache/recovery/openrecoveryscript";
	    	if (cachefile == DATA_LOGS_DIR)
		   cachefile = "/data/cache";
		cachefile = cachefile + "/recovery/openrecoveryscript";

	    	gui_print_color("warning", "\n\n- You tried to flash OTA zip (%s) manually. Attempting to recover the situation...\n\n", path);
	    	TWFunc::CreateNewFile(cachefile);
	    	usleep(256);
	    	if (TWFunc::Path_Exists(cachefile))
	       	   {
	    	   	TWFunc::AppendLineToFile(cachefile, "install " + zip_name);
	    	   	usleep(256);
	    	   	CloseArchive(Zip);
	    	   	usleep(256);
	    	   	gui_print_color("warning", "\n- Rebooting into OTA install mode in 10 seconds. Please wait ...\n\n");
	    	   	sleep(10);
	    	   	TWFunc::tw_reboot(rb_recovery);
	    	   	return INSTALL_ERROR;
	       	   }
	      }
	      LOGERR("Please flash this package using the ROM's updater app!\n");
	      return INSTALL_ERROR;
	    }

	  string Boot_File = ota_location_folder + "/boot.emmc.win";
	  if (
	     (!storage_is_encrypted()) || (TWFunc::Path_Exists(Boot_File)) || (DataManager::GetIntValue("OTA_decrypted") == 1)
	     )
	    {
	      if (Fox_OTA_RES_Check_MicroSD())
	      {
	      	 if (!TWFunc::Path_Exists(Boot_File)) 
	      	  {
		    string atmp_ota = "/sdcard1/AERA/OTA/boot.emmc.win";
		    if (TWFunc::Path_Exists(atmp_ota)) {
		        // Found on the external card; copy it to the primary AERA OTA directory.
		        if (!TWFunc::Path_Exists(AERA_OTA_PATH))
		           TWFunc::Recursive_Mkdir(AERA_OTA_PATH, false);

		        if (TWFunc::copy_file(atmp_ota, Boot_File, 0644) == 0) {
			    gui_print("- OTA backup found in /sdcard1/AERA/OTA/ - copied it to %s/OTA/ ...\n", AERA_OTA_PATH.c_str());
		    	    #ifdef OF_INCREMENTAL_OTA_BACKUP_SUPER
			    if (TWFunc::Path_Exists("/sdcard1/AERA/OTA/super.emmc.win") && !TWFunc::Path_Exists(AERA_OTA_PATH + "/super.emmc.win")) {
				TWFunc::copy_file("/sdcard1/AERA/OTA/super.emmc.win", AERA_OTA_PATH + "/super.emmc.win", 0644);
		    	    }
		    	    #endif
		        } else { // copy did not succeed - use the /sdcard1/ location
		    	   Boot_File = atmp_ota;
                  ota_location_folder = "/sdcard1/AERA/OTA";
			   gui_print("- OTA backup found in /sdcard1/AERA/OTA/ - trying that instead.\n");
		    	}
		    }
	      	}
	      }
	      
	      if (TWFunc::Path_Exists(Boot_File))
		{
		  gui_msg("fox_incremental_ota_res_run=Running restore process of the current OTA file");
		  DataManager::SetValue(AERA_RUN_SURVIVAL_BACKUP, 1);
		  PartitionManager.Set_Restore_Files(ota_location_folder);
		  if (PartitionManager.Run_OTA_Survival_Restore(ota_location_folder))
		    {
		      gui_msg("fox_incremental_ota_res=Process OTA_RES -- done!!");
		    }
		  else
		    {
		      set_miui_install_status(OTA_ERROR, false);
		      LOGERR("OTA_Survival: Unable to finish OTA_RES!\n");
		      return INSTALL_ERROR;
		    }
		}
	      else
		{
		  set_miui_install_status(OTA_CORRUPT, false);
		  gui_err("fox_survival_does_not_exist=OTA Survival does not exist! Please flash a full ROM first!");
		  return INSTALL_ERROR;
		}
	    }
	  else
	    {
	      set_miui_install_status(OTA_CORRUPT, false);
	      gui_print ("Internal storage is encrypted! Please do decrypt first!\n");
	      return INSTALL_ERROR;
	    }
	}
    } // Fox_Skip_OTA()
    
    } // (DataManager::GetIntValue(AERA_INSTALL_PREBUILT_ZIP) != 1)

  if (aeraScreenTimer.IsScreenOff())
    {
      if (zip_EntryExists(Zip, AROMA_CONFIG))
	{
	  aeraScreenTimer.Toggle();
	  gui_changeOverlay("");
	}
    }

    Fox_ProcessAsserts(assert_device);

    return INSTALL_SUCCESS;
}

void Fox_Post_Zip_Install(const int result)
{
   if (result != INSTALL_SUCCESS)
   	return;

   Aera_Zip_Installer_Code = DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE);
   if (Aera_Zip_Installer_Code != 0) // a ROM was installed
     {
         usleep(16384);
         TWFunc::Deactivation_Process();
         DataManager::SetValue(AERA_CALL_DEACTIVATION, 0);

	 // disable avb2.0 by patching the boot image
         usleep(16384);
         TWFunc::Patch_AVB20(false);
         usleep(16384);

	 // disable avb2.0 by patching vbmeta/vbmeta_system (new devices)
	 #if defined(OF_SUPPORT_VBMETA_AVB2_PATCHING)
	 if (DataManager::GetIntValue(TW_AUTO_DISABLE_AVB2_VAR))
		PartitionManager.Disable_AVB2(true);
	 #endif

    	 // Run any custom script after ROM flashing
	 TWFunc::MIUI_ROM_SetProperty(Aera_Zip_Installer_Code);
	 TWFunc::RunFoxScript(AERA_AFTER_ROM_FLASH_SCRIPT, "");
         usleep(16384);

	//---- Virtual A/B: compensate for ROM installers that still use legacy methods for flashing, instead of payload.bin/update_engine ----//
	#if defined(AERA_VIRTUAL_AB_DEVICE) && !defined(AERA_VENDOR_BOOT_RECOVERY)
	/*
	* check for MIUI ROM installers, and other non-standard vAB installers
	* Really, this fix should not be needed, but some custom ROM installers fail to use the standard flashing method for A/B devices
	* (ie, with payload.bin/update_engine) meaning that we have to intervene here to fix problems created by their non-standard methods
	*/
	if (Aera_Zip_Installer_Code == 2 || Aera_Zip_Installer_Code == 3 || Aera_Zip_Installer_Code == 22 || Aera_Zip_Installer_Code == 23) {
		gui_warn("mount_vab_partitions=Devices on super may not mount until after rebooting recovery.");
		gui_warn("flash_ab_reboot=To flash additional zips, please reboot recovery to switch to the updated slot.");
	}
	#endif
	//---- Virtual A/B compensations ---- //
         PartitionManager.Update_System_Details();
	// Run any custom script after completion of all ROM flashing processes
	TWFunc::RunFoxScript("/system/bin/post_rom_flash_completion.sh", "");

#ifdef OF_BLOCK_OPERATIONS_AFTER_ROM_FLASH
	TWFunc::Fox_Property_Set("fox_block_operations_pending_reboot", "blocking");
	gui_print_color("warning", "\n\nYou must now reboot AERA immediately!\n\nDo NOT attempt anything else before rebooting AERA.\n");
#endif
     }
}

bool Fox_Skip_Treble_Compatibility_Check()
{
   #ifdef OF_NO_TREBLE_COMPATIBILITY_CHECK
   return true;
   #else
   return false;
   #endif
}

//
