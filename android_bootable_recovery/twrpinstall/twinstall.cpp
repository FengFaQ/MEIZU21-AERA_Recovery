/*
	Copyright 2012 to 2017 bigbiff/Dees_Troy TeamWin
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
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include <string.h>
#include <stdio.h>
#include <linux/fs.h>
#include <openssl/sha.h>
#include <cutils/properties.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>

#include <android-base/unique_fd.h>

#include "twcommon.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"

#include "otautil/sysutil.h"
#include <ziparchive/zip_archive.h>
#include "twinstall/install.h"
#include "twinstall/verifier.h"
#include "variables.h"
#include "data.hpp"
#include "partitions.hpp"
#include "twrpDigestDriver.hpp"
#include "twrpDigest/twrpDigest.hpp"
#include "twrpDigest/twrpMD5.hpp"
#include "twrp-functions.hpp"
#include "aera_core.hpp"
#include "aera_flash_log.hpp"
#include "aeraui/platform/aera_ui_host.hpp"
#include "twinstall.h"
#include "installcommand.h"
#include "../twrpRepacker.hpp"
extern "C" {
	#include "aeraui/platform/aera_ui_host.h"
}

#define AB_OTA "payload_properties.txt"

enum zip_type {
	UNKNOWN_ZIP_TYPE = 0,
	UPDATE_BINARY_ZIP_TYPE,
	AB_OTA_ZIP_TYPE,
	TWRP_THEME_ZIP_TYPE
};

static void Append_Aera_Install_Status(const char* line) {
	constexpr size_t kBufferedLines = 256;
	std::string history = DataManager::GetStrValue("aera_install_status");
	if (!history.empty())
		history += '\n';
	history += line;

	size_t lines = 1;
	for (char c : history) {
		if (c == '\n')
			++lines;
	}
	while (lines > kBufferedLines) {
		const size_t next = history.find('\n');
		if (next == std::string::npos)
			break;
		history.erase(0, next + 1);
		--lines;
	}
	DataManager::SetValue("aera_install_status", history);
}

namespace {

constexpr char kPreservationDirectory[] = "/tmp/aera-partition-preservation";
constexpr uint64_t kPreservationHeadroom = 16ULL * 1024ULL * 1024ULL;

struct PreservedPartition {
	std::string label;
	std::string partition;
	std::string slot_a;
	std::string slot_b;
	std::string source;
	std::string backup;
	std::string digest;
	std::string before_a;
	std::string before_b;
	uint64_t bytes = 0;
};

void PreservationStatus(const std::string& message) {
	Append_Aera_Install_Status(message.c_str());
	gui_print("%s\n", message.c_str());
}

void PreservationError(const std::string& message) {
	const std::string full = "Partition protection failed: " + message;
	Append_Aera_Install_Status(full.c_str());
	gui_print_color("error", "%s\n", full.c_str());
}

std::string CurrentSlotSuffix() {
	std::string slot = android::base::GetProperty("ro.boot.slot_suffix", "");
	if (slot.empty())
		slot = android::base::GetProperty("ro.boot.slot", "");
	if (slot == "a" || slot == "_a") return "_a";
	if (slot == "b" || slot == "_b") return "_b";
	const std::string display = PartitionManager.Get_Active_Slot_Display();
	if (display == "A") return "_a";
	if (display == "B") return "_b";
	return {};
}

std::string ResolveSlotPartition(const std::string& partition,
		const std::string& suffix) {
	for (const char* root : {"/dev/block/bootdevice/by-name/",
						 "/dev/block/by-name/"}) {
		const std::string path = std::string(root) + partition + suffix;
		if (TWFunc::Path_Exists(path)) return path;
	}
	return {};
}

bool BlockDeviceSize(const std::string& path, uint64_t* bytes) {
	android::base::unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
	if (fd.get() < 0) return false;
	uint64_t size = 0;
	if (ioctl(fd.get(), BLKGETSIZE64, &size) != 0 || size == 0) return false;
	*bytes = size;
	return true;
}

bool CopyExact(const std::string& source, const std::string& destination,
		uint64_t bytes, bool destination_is_file) {
	android::base::unique_fd input(open(source.c_str(), O_RDONLY | O_CLOEXEC));
	if (input.get() < 0) return false;
	int flags = O_WRONLY | O_CLOEXEC;
	if (destination_is_file) flags |= O_CREAT | O_TRUNC;
	android::base::unique_fd output(open(destination.c_str(), flags, 0600));
	if (output.get() < 0) return false;

	std::vector<unsigned char> buffer(1024 * 1024);
	uint64_t remaining = bytes;
	while (remaining > 0) {
		const size_t request = static_cast<size_t>(
				std::min<uint64_t>(remaining, buffer.size()));
		ssize_t count;
		do {
			count = read(input.get(), buffer.data(), request);
		} while (count < 0 && errno == EINTR);
		if (count <= 0) return false;

		ssize_t written = 0;
		while (written < count) {
			ssize_t result;
			do {
				result = write(output.get(), buffer.data() + written,
						count - written);
			} while (result < 0 && errno == EINTR);
			if (result <= 0) return false;
			written += result;
		}
		remaining -= static_cast<uint64_t>(count);
	}
	return fsync(output.get()) == 0;
}

bool Sha256Exact(const std::string& path, uint64_t bytes, std::string* digest) {
	android::base::unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
	if (fd.get() < 0) return false;
	SHA256_CTX context;
	if (SHA256_Init(&context) != 1) return false;
	std::vector<unsigned char> buffer(1024 * 1024);
	uint64_t remaining = bytes;
	while (remaining > 0) {
		const size_t request = static_cast<size_t>(
				std::min<uint64_t>(remaining, buffer.size()));
		ssize_t count;
		do {
			count = read(fd.get(), buffer.data(), request);
		} while (count < 0 && errno == EINTR);
		if (count <= 0 || SHA256_Update(&context, buffer.data(), count) != 1)
			return false;
		remaining -= static_cast<uint64_t>(count);
	}
	unsigned char hash[SHA256_DIGEST_LENGTH];
	if (SHA256_Final(hash, &context) != 1) return false;
	std::ostringstream value;
	value << std::hex << std::setfill('0');
	for (unsigned char byte : hash)
		value << std::setw(2) << static_cast<unsigned int>(byte);
	*digest = value.str();
	return true;
}

bool ZipEntryContains(ZipArchiveHandle archive, const char* path,
		const char* marker) {
	ZipEntry64 entry;
	if (FindEntry(archive, path, &entry) != 0 || entry.uncompressed_length == 0 ||
			entry.uncompressed_length > 128 * 1024)
		return false;
	std::vector<uint8_t> contents(entry.uncompressed_length);
	if (ExtractToMemory(archive, &entry, contents.data(), contents.size()) != 0)
		return false;
	return std::string(reinterpret_cast<const char*>(contents.data()),
			contents.size()).find(marker) != std::string::npos;
}

bool IsAeraRecoveryPackage(ZipArchiveHandle archive) {
	ZipEntry64 recovery_image;
	return FindEntry(archive, "recovery.img", &recovery_image) == 0 &&
			ZipEntryContains(archive, "INSTALL.txt",
					"AERA Recovery Project installation") &&
			ZipEntryContains(archive, "META-INF/debug/fox_build_vars.txt",
					"AERA_PRODUCT_PREFIX=\"AERA\"");
}

class PartitionPreserver {
public:
	bool Prepare(bool allow_recovery_preservation) {
		Cleanup();
		const bool preserve_recovery =
				allow_recovery_preservation &&
				DataManager::GetIntValue(AERA_RECOVERY_PRESERVATION_SUPPORTED) != 0 &&
				DataManager::GetIntValue(AERA_PRESERVE_RECOVERY_VAR) != 0;
		const bool preserve_abl =
				DataManager::GetIntValue(AERA_ABL_PRESERVATION_SUPPORTED) != 0 &&
				DataManager::GetIntValue(AERA_PRESERVE_ABL_VAR) != 0;
		if (!preserve_recovery && !preserve_abl) return true;

		const std::string slot = CurrentSlotSuffix();
		if (slot.empty()) {
			PreservationError("the active slot could not be determined");
			return false;
		}
		if (preserve_recovery && !Add("AERA recovery", "recovery", slot)) {
			Cleanup();
			return false;
		}
		if (preserve_abl && !Add("current ABL", "abl", slot)) {
			Cleanup();
			return false;
		}

		uint64_t required = kPreservationHeadroom;
		for (const auto& target : targets_) {
			if (UINT64_MAX - required < target.bytes) {
				PreservationError("the required temporary size overflowed");
				Cleanup();
				return false;
			}
			required += target.bytes;
		}
		struct statvfs filesystem = {};
		if (statvfs("/tmp", &filesystem) != 0 ||
				static_cast<uint64_t>(filesystem.f_bavail) * filesystem.f_frsize < required) {
			PreservationError("not enough free RAM is available in /tmp");
			Cleanup();
			return false;
		}

		TWFunc::removeDir(kPreservationDirectory, false);
		if (mkdir(kPreservationDirectory, 0700) != 0 && errno != EEXIST) {
			PreservationError("the temporary backup directory could not be created");
			Cleanup();
			return false;
		}
		for (auto& target : targets_) {
			target.backup = std::string(kPreservationDirectory) + "/" +
					target.partition + ".img";
			PreservationStatus("Backing up protected " + target.label);
			if (!CopyExact(target.source, target.backup, target.bytes, true) ||
					!Sha256Exact(target.backup, target.bytes, &target.digest)) {
				PreservationError("the " + target.label + " backup could not be verified");
				Cleanup();
				return false;
			}
			if (target.source == target.slot_a) {
				target.before_a = target.digest;
				if (!Sha256Exact(target.slot_b, target.bytes, &target.before_b)) {
					PreservationError("the " + target.label + " state on slot B could not be recorded");
					Cleanup();
					return false;
				}
			} else {
				target.before_b = target.digest;
				if (!Sha256Exact(target.slot_a, target.bytes, &target.before_a)) {
					PreservationError("the " + target.label + " state on slot A could not be recorded");
					Cleanup();
					return false;
				}
			}
		}
		prepared_ = true;
		return true;
	}

	bool Finalize() {
		if (!prepared_) return true;
		PartitionManager.Unlock_Block_Partitions();
		bool success = true;
		for (const auto& target : targets_) {
			std::string after_a;
			std::string after_b;
			const bool read_a = Sha256Exact(target.slot_a, target.bytes, &after_a);
			const bool read_b = Sha256Exact(target.slot_b, target.bytes, &after_b);
			if (read_a && read_b && after_a == target.before_a &&
					after_b == target.before_b) {
				LOGINFO("Partition protection: %s was not changed; no restore needed\n",
						target.label.c_str());
				continue;
			}
			for (const auto& slot : std::vector<std::pair<const char*, std::string>>{
						{"A", target.slot_a}, {"B", target.slot_b}}) {
				const std::string& installed_digest =
						slot.first[0] == 'A' ? after_a : after_b;
				const bool digest_available = slot.first[0] == 'A' ? read_a : read_b;
				if (digest_available && installed_digest == target.digest)
					continue;
				PreservationStatus("Restoring protected " + target.label +
						" to slot " + slot.first);
				std::string restored_digest;
				if (!CopyExact(target.backup, slot.second, target.bytes, false) ||
						!Sha256Exact(slot.second, target.bytes, &restored_digest) ||
						restored_digest != target.digest) {
					PreservationError("the " + target.label + " restore to slot " +
							slot.first + " could not be verified");
					success = false;
				}
			}
		}
		if (success)
			PreservationStatus("Protected partitions verified on both slots");
		Cleanup();
		return success;
	}

private:
	bool Add(const std::string& label, const std::string& partition,
			const std::string& current_slot) {
		PreservedPartition target;
		target.label = label;
		target.partition = partition;
		target.slot_a = ResolveSlotPartition(partition, "_a");
		target.slot_b = ResolveSlotPartition(partition, "_b");
		target.source = current_slot == "_a" ? target.slot_a : target.slot_b;
		uint64_t size_a = 0, size_b = 0;
		if (target.slot_a.empty() || target.slot_b.empty() || target.source.empty() ||
				!BlockDeviceSize(target.slot_a, &size_a) ||
				!BlockDeviceSize(target.slot_b, &size_b) || size_a != size_b) {
			PreservationError("the " + partition + " A/B partitions are unavailable or incompatible");
			return false;
		}
		target.bytes = size_a;
		targets_.push_back(std::move(target));
		return true;
	}

	void Cleanup() {
		for (const auto& target : targets_)
			if (!target.backup.empty()) unlink(target.backup.c_str());
		rmdir(kPreservationDirectory);
		targets_.clear();
		prepared_ = false;
	}

	std::vector<PreservedPartition> targets_;
	bool prepared_ = false;
};

}  // namespace

static std::vector<std::string> Split_Aera_Installer_Fields(
		const char* payload, size_t maximum_fields) {
	std::vector<std::string> fields;
	if (payload == nullptr || maximum_fields == 0)
		return fields;
	std::string value(payload);
	while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
		value.pop_back();
	size_t begin = 0;
	while (fields.size() + 1 < maximum_fields) {
		const size_t separator = value.find('|', begin);
		if (separator == std::string::npos)
			break;
		fields.emplace_back(value.substr(begin, separator - begin));
		begin = separator + 1;
	}
	fields.emplace_back(value.substr(begin));
	return fields;
}

static int Aera_Installer_Integer(const std::string& value, int minimum,
		int maximum) {
	char* end = nullptr;
	const long parsed = strtol(value.c_str(), &end, 10);
	if (end == value.c_str() || *end != '\0')
		return minimum;
	return std::max(minimum, std::min(maximum, static_cast<int>(parsed)));
}

static int Install_Theme(const char* path, ZipArchiveHandle Zip) {
#ifdef TW_OEM_BUILD // We don't do custom themes in OEM builds
	return INSTALL_CORRUPT;
#else
	std::string binary_name("ui.xml");
	ZipEntry64 binary_entry;
	if (FindEntry(Zip, binary_name, &binary_entry) != 0) {
		return INSTALL_CORRUPT;
	}
	if (!PartitionManager.Mount_Settings_Storage(true))
		return INSTALL_ERROR;
	std::string theme_path = DataManager::GetCurrentStoragePath() + "/theme";
	if (!TWFunc::Path_Exists(theme_path)) {
		if (!TWFunc::Recursive_Mkdir(theme_path)) {
			return INSTALL_ERROR;
		}
	}
	theme_path += "/ui.zip";
	if (TWFunc::copy_file(path, theme_path, 0644) != 0) {
		return INSTALL_ERROR;
	}
	LOGINFO("Installing custom theme '%s' to '%s'\n", path, theme_path.c_str());
	LOGINFO("Custom XML themes are not loaded by AERA UI.\n");
	return INSTALL_SUCCESS;
#endif
}

static int Prepare_Update_Binary(const char *path, ZipArchiveHandle Zip) {
	if (TWFunc::Block_Operations_Until_Reboot())
		return INSTALL_ERROR;

	char arches[PATH_MAX];
	property_get("ro.product.cpu.abilist", arches, "error");
	if (strcmp(arches, "error") == 0)
		property_get("ro.product.cpu.abi", arches, "error");
	vector<string> split = TWFunc::split_string(arches, ',', true);
	std::vector<string>::iterator arch;
	std::string base_name = UPDATE_BINARY_NAME;
	base_name += "-";
	ZipEntry64 binary_entry;
	std::string update_binary_string(UPDATE_BINARY_NAME);
	if (FindEntry(Zip, update_binary_string, &binary_entry) != 0) {
		for (arch = split.begin(); arch != split.end(); arch++) {
			std::string temp = base_name + *arch;
			std::string binary_name(temp.c_str());
			if (FindEntry(Zip, binary_name, &binary_entry) != 0) {
				std::string binary_name(temp.c_str());
				break;
			}
		}
	}
	LOGINFO("Extracting updater binary '%s'\n", UPDATE_BINARY_NAME);
	unlink(TMP_UPDATER_BINARY_PATH);
	android::base::unique_fd fd(
		open(TMP_UPDATER_BINARY_PATH, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0755));
	if (fd == -1) {
		return INSTALL_ERROR;
	}
	int32_t err = ExtractEntryToFile(Zip, &binary_entry, fd);
	if (err != 0) {
		LOGERR("Could not extract '%s'\n", UPDATE_BINARY_NAME);
		return INSTALL_ERROR;
	}

	// -------------- OrangeFox: start ---------------- //
	int Fox_Ret = Fox_Prepare_Update_Binary(path, Zip);
	if (Fox_Ret != INSTALL_SUCCESS) {
	   return Fox_Ret;
	}
	// -------------- OrangeFox: end ---------------- //

	// If exists, extract file_contexts from the zip file
	std::string file_contexts("file_contexts");
	ZipEntry64 file_contexts_entry;
	if (FindEntry(Zip, file_contexts, &file_contexts_entry) != 0) {
		LOGINFO("Zip does not contain SELinux file_contexts file in its root.\n");
	} else {
		const string output_filename = "/file_contexts";
		LOGINFO("Zip contains SELinux file_contexts file in its root. Extracting to %s\n", output_filename.c_str());
		android::base::unique_fd fd(
			open(output_filename.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644));
		if (fd == -1) {
			return INSTALL_ERROR;
		}
		if (ExtractEntryToFile(Zip, &file_contexts_entry, fd)) {
			LOGERR("Could not extract '%s'\n", output_filename.c_str());
			return INSTALL_ERROR;
		}
	}
	return INSTALL_SUCCESS;
}


static int Run_Update_Binary(const char *path, int* wipe_cache, zip_type ztype) {
	int ret_val, pipe_fd[2], status, zip_verify;
	char buffer[1024];
	FILE* child_data;
	pipe(pipe_fd);

	std::vector<std::string> args;
    if (ztype == UPDATE_BINARY_ZIP_TYPE) {
		ret_val = update_binary_command(path, 0, pipe_fd[1], &args);
    } else if (ztype == AB_OTA_ZIP_TYPE) {
		ret_val = abupdate_binary_command(path, 0, pipe_fd[1], &args);
	} else {
		LOGERR("Unknown zip type %i\n", ztype);
		ret_val = INSTALL_CORRUPT;
	}
    if (ret_val) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return ret_val;
    }

	// Convert the vector to a NULL-terminated char* array suitable for execv.
	const char* chr_args[args.size() + 1];
	chr_args[args.size()] = NULL;
	for (size_t i = 0; i < args.size(); i++)
		chr_args[i] = args[i].c_str();

	pid_t pid = fork();
	if (pid == 0) {
		close(pipe_fd[0]);
		execve(chr_args[0], const_cast<char**>(chr_args), environ);
		printf("E:Can't execute '%s': %s\n", chr_args[0], strerror(errno));
		_exit(-1);
	}
	close(pipe_fd[1]);

	*wipe_cache = 0;

	DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
	child_data = fdopen(pipe_fd[0], "r");
	while (fgets(buffer, sizeof(buffer), child_data) != NULL) {
		char* command = strtok(buffer, " \n");
		if (command == NULL) {
			continue;
		} else if (strcmp(command, "progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			char* seconds_char = strtok(NULL, " \n");

			float fraction_float = strtof(fraction_char, NULL);
			int seconds_float = strtol(seconds_char, NULL, 10);

			if (zip_verify)
				DataManager::ShowProgress(fraction_float * (1 - VERIFICATION_PROGRESS_FRACTION), seconds_float);
			else
				DataManager::ShowProgress(fraction_float, seconds_float);
		} else if (strcmp(command, "set_progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			float fraction_float = strtof(fraction_char, NULL);
			DataManager::_SetProgress(fraction_float);
		} else if (strcmp(command, "ui_print") == 0) {
			char* display_value = strtok(NULL, "\n");
	  		if (display_value) {
				unsigned int super_percent = 0;
				unsigned long long super_written = 0;
				unsigned long long super_total = 0;
				const bool native_super_progress =
					DataManager::GetIntValue("aera_installer_native") == 1 &&
					sscanf(display_value,
					       "super.img progress: %u%% (%llu / %llu MiB)",
					       &super_percent, &super_written, &super_total) == 3;
				if (native_super_progress) {
					super_percent = std::min(super_percent, 100U);
					const int overall_progress =
						68 + static_cast<int>((super_percent * 27U) / 100U);
					char detail[160];
					snprintf(detail, sizeof(detail),
					         "Writing super image  •  %u%%  •  %llu / %llu MiB",
					         super_percent, super_written, super_total);
					DataManager::SetValue("ui_progress", overall_progress);
					DataManager::SetValue("aera_installer_stage_detail", detail);
					continue;
				}
			Append_Aera_Install_Status(display_value);
	      		    gui_print("%s", display_value);
	    		}
	  		else {
	      			gui_print("\n");
	    		}
		} else if (strcmp(command, "aera_package") == 0) {
			const auto fields = Split_Aera_Installer_Fields(strtok(NULL, "\n"), 4);
			if (fields.size() == 4) {
				DataManager::SetValue("aera_installer_package", fields[0]);
				DataManager::SetValue("aera_installer_device", fields[1]);
				DataManager::SetValue("aera_installer_author", fields[2]);
				DataManager::SetValue("aera_installer_packager", fields[3]);
				DataManager::SetValue("aera_installer_native", 1);
			}
		} else if (strcmp(command, "aera_stage") == 0) {
			const auto fields = Split_Aera_Installer_Fields(strtok(NULL, "\n"), 5);
			if (fields.size() == 5) {
				const int progress = Aera_Installer_Integer(fields[0], 0, 100);
				DataManager::SetValue("ui_progress", progress);
				DataManager::SetValue("aera_installer_stage",
					Aera_Installer_Integer(fields[1], 0, 99));
				DataManager::SetValue("aera_installer_stage_count",
					Aera_Installer_Integer(fields[2], 0, 99));
				DataManager::SetValue("aera_installer_stage_title", fields[3]);
				DataManager::SetValue("aera_installer_stage_detail", fields[4]);
				DataManager::SetValue("aera_installer_native", 1);
				Append_Aera_Install_Status(fields[3].c_str());
			}
		} else if (strcmp(command, "aera_status") == 0) {
			char* message = strtok(NULL, "\n");
			if (message != nullptr) {
				DataManager::SetValue("aera_installer_stage_detail", message);
				Append_Aera_Install_Status(message);
				DataManager::SetValue("aera_installer_native", 1);
			}
		} else if (strcmp(command, "aera_prompt") == 0) {
			const auto fields = Split_Aera_Installer_Fields(strtok(NULL, "\n"), 5);
			if (fields.size() == 5 && !fields[0].empty()) {
				DataManager::SetValue("aera_installer_prompt_id", fields[0]);
				DataManager::SetValue("aera_installer_prompt_title", fields[1]);
				DataManager::SetValue("aera_installer_prompt_message", fields[2]);
				DataManager::SetValue("aera_installer_prompt_accept", fields[3]);
				DataManager::SetValue("aera_installer_prompt_decline", fields[4]);
				DataManager::SetValue("aera_installer_native", 1);
				DataManager::SetValue("aera_installer_prompt_active", 1);
			}
		} else if (strcmp(command, "aera_complete") == 0) {
			const auto fields = Split_Aera_Installer_Fields(strtok(NULL, "\n"), 2);
			if (!fields.empty()) {
				DataManager::SetValue("aera_installer_stage_title", fields[0]);
				Append_Aera_Install_Status(fields[0].c_str());
			}
			if (fields.size() > 1)
				DataManager::SetValue("aera_installer_stage_detail", fields[1]);
			DataManager::SetValue("aera_installer_prompt_active", 0);
			DataManager::SetValue("aera_installer_native", 1);
			DataManager::SetValue("ui_progress", 100);
		} else if (strcmp(command, "aera_reboot") == 0) {
			const auto fields = Split_Aera_Installer_Fields(strtok(NULL, "\n"), 2);
			if (fields.size() == 2) {
				DataManager::SetValue("aera_installer_reboot_seconds",
					Aera_Installer_Integer(fields[0], 0, 60));
				DataManager::SetValue("aera_installer_reboot_target", fields[1]);
				DataManager::SetValue("aera_installer_rebooting", 1);
				DataManager::SetValue("aera_installer_native", 1);
				DataManager::SetValue("ui_progress", 100);
			}
		} else if (strcmp(command, "wipe_cache") == 0) {
			*wipe_cache = 1;
		} else if (strcmp(command, "clear_display") == 0) {
			// Do nothing, not supported by TWRP
		} else if (strcmp(command, "log") == 0) {
			printf("%s\n", strtok(NULL, "\n"));
		} else {
			LOGERR("unknown command [%s]\n", command);
		}
	}
	fclose(child_data);
	DataManager::SetValue("aera_installer_prompt_active", 0);

	int waitrc = TWFunc::Wait_For_Child(pid, &status, "Updater");


  	// if updater-script doesn't find the correct device
  	if (WEXITSTATUS (status) == TW_ERROR_WRONG_DEVICE) {
       		gui_print_color("error", "\nPossible causes of this error:\n  1. Wrong device\n  2. Wrong firmware\n  3. Corrupt zip\n  4. System not mounted\n  5. Bugged updater-script.\n\nSearch online for \"error %i\". ",
       			TW_ERROR_WRONG_DEVICE);
       		gui_print_color("error", "Check \"/tmp/recovery.log\", and look above, for the specific cause of this error.\n\n");
     	}

  	if (waitrc != 0) {
      		set_miui_install_status(OTA_CORRUPT, false);
      		return INSTALL_ERROR;
    	}

	return INSTALL_SUCCESS;
}

static int InstallZipImpl(const char *path, int *wipe_cache, bool check_for_digest)
{
  int ret_val, zip_verify = 1, unmount_system = 1, reflashtwrp = 0, unmount_vendor = 1;
  bool run_rom_scripts = false;

  if (TWFunc::Block_Operations_Until_Reboot())
	return INSTALL_CORRUPT;

  if (strcmp(path, "error") == 0)
    {
      LOGERR("Failed to get adb sideload file: '%s'\n", path);
      return INSTALL_CORRUPT;
    }

  if (DataManager::GetIntValue(AERA_INSTALL_PREBUILT_ZIP) == 1)
     {
         DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 0); // internal zip = standard zip installer
         DataManager::SetValue(AERA_ZIP_INSTALLER_TREBLE, 0);
     }    
  else   
    {
	gui_msg(Msg("installing_zip=Installing zip file '{1}'")(path));
	if (strlen(path) < 9 || strncmp(path, "/sideload", 9) != 0) {
		string digest_str;
		string Full_Filename = path;

		if (check_for_digest) {
			gui_msg("check_for_digest=Checking for Digest file...");
			if (*path != '@' && !twrpDigestDriver::Check_File_Digest(Full_Filename)) {
				LOGERR("Aborting zip install: Digest verification failed\n");
				return INSTALL_CORRUPT;
			}
		}
	}
    }

  DataManager::GetValue(TW_UNMOUNT_SYSTEM, unmount_system);
  DataManager::GetValue(TW_UNMOUNT_VENDOR, unmount_vendor);

#ifndef TW_OEM_BUILD
  DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
#endif

  DataManager::SetProgress(0);

	auto package = Package::CreateMemoryPackage(path);
	if (!package) {
		return INSTALL_CORRUPT;
	}

	if (zip_verify) {
		gui_msg("verify_zip_sig=Verifying zip signature...");
		static constexpr const char* CERTIFICATE_ZIP_FILE = "/system/etc/security/otacerts.zip";
		std::vector<Certificate> loaded_keys = LoadKeysFromZipfile(CERTIFICATE_ZIP_FILE);
		if (loaded_keys.empty()) {
			LOGERR("Failed to load keys\n");
			return -1;
		}
		LOGINFO("%zu key(s) loaded from %s\n", loaded_keys.size(), CERTIFICATE_ZIP_FILE);

		ret_val = verify_file(package.get(), loaded_keys, std::bind(&DataManager::SetProgress, std::placeholders::_1));
		if (ret_val != VERIFY_SUCCESS) {
			LOGINFO("Zip signature verification failed: %i\n", ret_val);
			gui_err("verify_zip_fail=Zip signature verification failed!");
#ifdef USE_MINZIP
			sysReleaseMap(&map);
#endif
			return -1;
		} else {
			gui_msg("verify_zip_done=Zip signature verified successfully.");
		}
    }
    
    ZipArchiveHandle Zip = package->GetZipArchiveHandle();
    if (!Zip) {
      set_miui_install_status(OTA_CORRUPT, true);
      gui_err("zip_corrupt=Zip file is corrupt!");
      return INSTALL_CORRUPT;
    }
	const bool aera_recovery_package = IsAeraRecoveryPackage(Zip);
	if (aera_recovery_package)
		PreservationStatus("AERA recovery package detected; allowing recovery update");

    if (unmount_system) {
	if (PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path())) {
		gui_msg("unmount_system=Unmounting System...");
		if (PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false)) {
			//unlink(PartitionManager.Get_Android_Root_Path().c_str());
			//mkdir(PartitionManager.Get_Android_Root_Path().c_str(), 0755);
		}
		else {
			gui_msg("unmount_system_err=Failed to unmount System");
		        return -1;
		}
	}
   }

   if (unmount_vendor) {
	if (PartitionManager.Is_Mounted_By_Path("/vendor")) {
		gui_msg("unmount_vendor=Unmounting Vendor...");
		if (PartitionManager.UnMount_By_Path("/vendor", false)) {
		   	//unlink("/vendor");
		   	//mkdir("/vendor", 0755);
		} else {
			gui_msg("unmount_vendor_err=Failed to unmount Vendor");
			return -1;
		}
	}
   }

   // DJ9, 20200622: try to avoid a situation where blockimg will bomb out when trying to create a stash
   if (TWFunc::Path_Exists("/cache/.") && !TWFunc::Path_Exists("/cache/recovery/.") && !TWFunc::Path_Exists("/data/cache/.")) {
	LOGINFO("Recreating the /cache/recovery/ folder ...\n");
	if (!TWFunc::Recursive_Mkdir("/cache/recovery", false))
	   LOGERR("Could not create /cache/recovery - blockimg may have problems with creating stashes\n");
   }
   // DJ9

  time_t start, stop;
  time(&start);
  PartitionPreserver preserver;

  std::string update_binary_name(UPDATE_BINARY_NAME);
  ZipEntry64 update_binary_entry;
  if (FindEntry(Zip, update_binary_name, &update_binary_entry) == 0) {
		LOGINFO("Update binary zip\n");
		// Additionally verify the compatibility of the package.
		if (!Fox_Skip_Treble_Compatibility_Check() && !verify_package_compatibility(Zip)) {
			gui_err("zip_compatible_err=Zip Treble compatibility error!");
			ret_val = INSTALL_CORRUPT;
		} else if (!preserver.Prepare(!aera_recovery_package)) {
			ret_val = INSTALL_ERROR;
		} else {
			ret_val = Prepare_Update_Binary(path, Zip);
			if (ret_val == INSTALL_SUCCESS) {
				usleep(32);
				run_rom_scripts = ((DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE) != 0) // only run after flashing a ROM
				&& (DataManager::GetIntValue(AERA_INSTALL_PREBUILT_ZIP) != 1)); // don't run for built-in zips

				if (run_rom_scripts && TWFunc::Path_Exists(AERA_PRE_ROM_FLASH_SCRIPT)) {
					TWFunc::RunFoxScript(AERA_PRE_ROM_FLASH_SCRIPT, path);
	  			}

				ret_val = Run_Update_Binary(path, wipe_cache, UPDATE_BINARY_ZIP_TYPE);

				if (DataManager::GetIntValue("fox_processing_asserts") != 0) {
					TWFunc::Fox_Property_Set("ro.product.device", DataManager::GetStrValue("fox_product_device"));
					DataManager::SetValue("fox_processing_asserts", "0");
					LOGINFO("\nDevice code name restored\n");
				}
			}
		}
	} else {
		std::string ab_binary_name(AB_OTA);
		ZipEntry64 ab_binary_entry;
		if (FindEntry(Zip, ab_binary_name, &ab_binary_entry) == 0) {
			LOGINFO("AB zip\n");
			if (!preserver.Prepare(!aera_recovery_package)) {
				ret_val = INSTALL_ERROR;
			} else {
			gui_msg(Msg(msg::kHighlight, "flash_ab_inactive=Flashing A/B zip to inactive slot: {1}")(PartitionManager.Get_Active_Slot_Display()=="A"?"B":"A"));
			// We need this so backuptool can do its magic
			bool system_mount_state = PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path());
			bool vendor_mount_state = PartitionManager.Is_Mounted_By_Path("/vendor");
			PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
			PartitionManager.Mount_By_Path("/vendor", false);
			TWFunc::copy_file("/system/bin/sh", "/tmp/sh", 0755);
			mount("/tmp/sh", "/system/bin/sh", "auto", MS_BIND, NULL);

			run_rom_scripts = true;
			usleep(32);

			if (run_rom_scripts && TWFunc::Path_Exists(AERA_PRE_ROM_FLASH_SCRIPT)) {
				TWFunc::RunFoxScript(AERA_PRE_ROM_FLASH_SCRIPT, path);
			}

			ret_val = Run_Update_Binary(path, wipe_cache, AB_OTA_ZIP_TYPE);

			DataManager::SetValue(AERA_ZIP_INSTALLER_CODE, 1); // mark as custom ROM install

			umount("/system/bin/sh");
			unlink("/tmp/sh");
			if (!vendor_mount_state)
				PartitionManager.UnMount_By_Path("/vendor", false);
			if (!system_mount_state)
				PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
			if (android::base::GetBoolProperty("ro.virtual_ab.enabled", false)) {
				PartitionManager.Unlock_Block_Partitions();
				PartitionManager.Prepare_All_Super_Volumes();
				gui_warn("mount_vab_partitions=Devices on super may not mount until after rebooting recovery.");
			}
			gui_warn("flash_ab_reboot=To flash additional zips, please reboot recovery to switch to the updated slot.");
			}
		} else {
			std::string binary_name("ui.xml");
			ZipEntry64 binary_entry;
			if (FindEntry(Zip, binary_name, &binary_entry) == 0) {
				LOGINFO("AERA theme zip\n");
				ret_val = Install_Theme(path, Zip);
			} else {
				ret_val = INSTALL_CORRUPT;
			}
		}
   }

  if (!preserver.Finalize()) ret_val = INSTALL_ERROR;

  time(&stop);
  int total_time = (int) difftime(stop, start);

  if (ret_val == INSTALL_CORRUPT)
    {
        set_miui_install_status(OTA_CORRUPT, true);
        gui_err("invalid_zip_format=Invalid zip file format!");
    }
  else
  if (ret_val == INSTALL_ERROR)
     {
	set_miui_install_status(OTA_ERROR, false);
     }
  else // success - so let us see whether we need to run OTA_BAK
  {
     // if MIUI-specific features have been disabled
     if (Fox_Skip_OTA()) // yes
     {
         //LOGINFO("OrangeFox: not running the incremental OTA backup (OTA_BAK).\n");
     }  
     else // else let us proceed with the OTA stuff
     if (DataManager::GetIntValue(AERA_INCREMENTAL_OTA_FAIL) != 1)
     {
	if (DataManager::GetIntValue(AERA_INCREMENTAL_PACKAGE) == 1 && DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE) != 0)
      	  {
	    if (TWinstall_Run_OTA_BAK (true)) // true, because the value of Aera_Zip_Installer_Code to be set
      	      {
	        if (Fox_OTA_Backup_Stock_Boot_Image()) // whether to create an additional backup of the stock boot image
	           {
	      		usleep(2048);
	      		string ota_folder = DataManager::GetStrValue("ota_bak_folder");
	      		usleep(2048);
	      		if (ota_folder.empty())
			   ota_folder = AERA_OTA_PATH;
			string ota_bootimg = ota_folder + "/boot.img";
			if (TWFunc::Path_Exists(boot_bak_img)) {
			   if (TWFunc::copy_file(boot_bak_img, ota_bootimg, 0644) == 0) {
                   LOGINFO("AERA: stock boot image extracted into the OTA directory.\n");
			     }
			   unlink(boot_bak_img.c_str());
		 	}
	           }
      	      }
      	  }

	DataManager::SetValue(AERA_METADATA_PRE_BUILD, 0);
	DataManager::SetValue(AERA_MIUI_ZIP_TMP, 0);
	DataManager::SetValue(AERA_INCREMENTAL_OTA_FAIL, 0);
	DataManager::SetValue(AERA_LOADED_FINGERPRINT, 0);
	DataManager::SetValue(AERA_RUN_SURVIVAL_BACKUP, 0);
      
     } // end of OTA stuff
    LOGINFO("Install took %i second(s).\n", total_time);
   }

   if (ret_val == INSTALL_SUCCESS)
      set_miui_install_status(OTA_SUCCESS, false);

   usleep(32);
   if (DataManager::GetIntValue(AERA_ZIP_INSTALLER_CODE) != 0) // just flashed a ROM
   {
      usleep(16);
      TWFunc::Check_OrangeFox_Overwrite_FromROM(false, path);
   }

   if (run_rom_scripts && TWFunc::Path_Exists(AERA_POST_ROM_FLASH_SCRIPT)) {
   	usleep(2048);
	TWFunc::RunFoxScript(AERA_POST_ROM_FLASH_SCRIPT, path);
   	sleep(1);
   	DataManager::SetValue("found_fox_overwriting_rom", "0");
   	TWFunc::Fox_Property_Set("found_fox_overwriting_rom", "");
   }

  return ret_val;
}

int TWinstall_zip(const char *path, int *wipe_cache, bool check_for_digest) {
  aera::flashlog::Recording recording(path ? path : "", "ZIP installer");
  return recording.Finish(InstallZipImpl(path, wipe_cache, check_for_digest));
}
