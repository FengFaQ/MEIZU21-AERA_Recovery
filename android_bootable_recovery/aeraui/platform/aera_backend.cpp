/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include <aeraui/backend.hpp>
#include "../../aera_remote/backup_cancel.hpp"
#include "aeraui/features/update/payload_inspector.hpp"
#include "aeraui/features/update/payload_arb.hpp"
#include "aeraui/features/update/payload_targets.hpp"
#include "aeraui/features/update/payload_full_flash.hpp"
#include "aeraui/features/update/payload_otaripper.hpp"
#include "aeraui/features/update/payload_parallel.hpp"
#include "aeraui/features/update/payload_otaripper_batch.hpp"
#include <android-base/unique_fd.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <limits.h>
#include <set>
#include <sstream>
#include <string>
#include <strings.h>
#include <vector>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <mntent.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <android-base/properties.h>

#include "data.hpp"
#include "aera_flash_log.hpp"
#include "aera_secrets/aera_secrets.hpp"
#include "aeraui/features/root/root_manager.hpp"
#include "partitions.hpp"
#include "twrp-functions.hpp"
#include "../../aera_shared_storage.hpp"
#include "variables.h"
#include <set_metadata.h>
#include "aeraui/platform/aera_ui_host.hpp"
#include "aera_remote/pc_connection.hpp"
#include "aera_remote/pc_storage.hpp"
#include "aeraui/features/backup/app_vault.hpp"
#include "aeraui/platform/image_targets.hpp"
#include "unencrypted_users.hpp"
#include "temperature_reader.hpp"
#include <twinstall.h>
#include <twinstall/adb_install.h>

#ifdef OF_ENABLE_WLAN
#include "aera_adbd.hpp"
#include "nas/NasManager.hpp"
#include "wlan.hpp"
#include "aera_wifi_dispatcher.hpp"
#endif

extern "C" int aeraui_install_package(const char *path) {
  if (!path || !path[0]) return 1;
  int wipe_cache = 0;
  TWFunc::SetPerformanceMode(true);
  int result = TWinstall_zip(path, &wipe_cache, false);
  if (result == 0 && wipe_cache)
    result = PartitionManager.Wipe_By_Path("/cache") ? 0 : 1;
  PartitionManager.Update_System_Details();
  TWFunc::SetPerformanceMode(false);
  return result;
}

extern "C" int aeraui_decrypt_data(const char *credential, int user_id) {
  if (!credential) return 1;
  const std::string password(credential);
  DataManager::SetValue("tw_crypto_password", password);
  DataManager::SetValue("tw_password_fail", 0);
  int result;
  if (DataManager::GetIntValue(TW_IS_FBE)) {
    DataManager::SetValue("tw_crypto_user_id", std::to_string(user_id));
    result = PartitionManager.Decrypt_Device(password, user_id);
    if (user_id != 0) return result == 0 ? 0 : 1;
  } else {
    result = PartitionManager.Decrypt_Device(password);
  }
  if (result != 0) {
    DataManager::SetValue("tw_password_fail", 1);
    return 1;
  }
  DataManager::SetValue(TW_IS_ENCRYPTED, 0);
  DataManager::SetValue(TW_IS_DECRYPTED, 1);
  DataManager::SetValue("data_decrypted", 1);
  DataManager::SetValue("tw_password_fail", 0);
  if (DataManager::GetIntValue(TW_HAS_DATA_MEDIA) != 0) {
    if (tw_get_default_metadata(DataManager::GetSettingsStoragePath().c_str()) != 0 &&
        tw_get_default_metadata(DataManager::GetCurrentStoragePath().c_str()) != 0)
      LOGINFO("Failed to get default contexts and file mode for storage files.\n");
  }
  PartitionManager.Decrypt_Adopted();
  PartitionManager.Update_System_Details();
  return 0;
}

int aeraui::RecoveryDecrypt(const std::string &credential, int user_id) {
  return aeraui_decrypt_data(credential.c_str(), user_id);
}

namespace aeraui {
namespace {
void LoadAeraPreferencesIfAvailable();
bool SaveAeraPreferences(bool require_early = false);
bool ResetAeraSettings(bool theme);
int DefaultHapticDuration(Haptic haptic);
std::atomic<bool> gSideloadActive{false};
std::atomic<bool> gSideloadCancelRequested{false};
std::atomic<uint64_t> gSideloadReceivedBytes{0};
std::atomic<uint64_t> gSideloadTotalBytes{0};
bool gLiveFastbootPostDecryptReady = false;
constexpr const char *kFastbootWifiPreference = "aera_fastboot_wifi";
constexpr const char *kFastbootProtocolProperty = "fastbootd.protocol";

bool FastbootWifiPrerequisites() {
#ifdef OF_ENABLE_WLAN
  const auto adb = AeraAdbd::GetStatus();
  // Secure adbd may not be running yet after a fresh recovery boot. A stored
  // pairing is the authorization prerequisite; StartSecure() is called after
  // fastbootd starts and activates the authenticated transport for it.
  return adb.wlan_connected && !adb.no_auth &&
      !AeraAdbd::ListDevices().empty();
#else
  return false;
#endif
}

bool RestartFastbootTransport(bool wireless) {
  if (android::base::GetProperty(TW_FASTBOOT_MODE_PROP, "0") != "1")
    return true;
  // A selector-enabled fastbootd owns both endpoints but accepts commands
  // only from the transport named by fastbootd.protocol. Switching that
  // property is enough; stopping the daemon here would tear down the active
  // Fastboot scene and drop the user back into recovery.
  if (android::base::GetProperty("init.svc.fastbootd", "") == "running") {
    property_set(kFastbootProtocolProperty, wireless ? "tcp" : "usb");
    return true;
  }
  // Fully detach the old FunctionFS transport before starting the selected
  // one. Merely restarting fastbootd leaves the USB endpoint readable and can
  // steal commands from a Wi-Fi operation (or vice versa).
  property_set("sys.usb.config", "none");
  for (int attempt = 0; attempt < 30; ++attempt) {
    if (android::base::GetProperty("init.svc.fastbootd", "") == "stopped")
      break;
    usleep(50000);
  }
  property_set(kFastbootProtocolProperty, wireless ? "tcp" : "usb");
  // The standard fastboot property trigger starts fastbootd. TCP mode does
  // not publish FunctionFS descriptors, so USB stays detached while Wi-Fi is
  // selected even if the cable remains plugged in.
  property_set("sys.usb.config", "fastboot");
  for (int attempt = 0; attempt < 60; ++attempt) {
    if (android::base::GetProperty("init.svc.fastbootd", "") == "running")
      return true;
    usleep(50000);
  }
  LOGERR("AERA fastboot Wi-Fi: fastbootd did not restart.\n");
  return false;
}

bool ResolveDirectBackupChild(const std::string &root,
                              const std::string &folder,
                              std::string *resolved_root,
                              std::string *resolved_folder,
                              std::string *name = nullptr) {
  if (root.empty() || folder.empty()) return false;
  char root_path[PATH_MAX] = {};
  char backup_path[PATH_MAX] = {};
  if (realpath(root.c_str(), root_path) == nullptr ||
      realpath(folder.c_str(), backup_path) == nullptr)
    return false;

  struct stat info {};
  if (lstat(backup_path, &info) != 0 || !S_ISDIR(info.st_mode) ||
      S_ISLNK(info.st_mode))
    return false;

  const std::string canonical_root(root_path);
  const std::string canonical_backup(backup_path);
  const std::string prefix = canonical_root + "/";
  if (canonical_backup.compare(0, prefix.size(), prefix) != 0)
    return false;
  const std::string child = canonical_backup.substr(prefix.size());
  if (child.empty() || child.find('/') != std::string::npos)
    return false;

  if (resolved_root) *resolved_root = canonical_root;
  if (resolved_folder) *resolved_folder = canonical_backup;
  if (name) *name = child;
  return true;
}

bool PathInside(const std::string &path, const std::string &parent) {
  return path == parent ||
      (path.size() > parent.size() &&
       path.compare(0, parent.size(), parent) == 0 &&
       path[parent.size()] == '/');
}

bool MeasureBackupTree(const std::string &path, uint64_t *bytes) {
  if (aera::pc::BackupCancelRequested()) return false;
  struct stat info {};
  if (lstat(path.c_str(), &info) != 0 || S_ISLNK(info.st_mode)) return false;
  if (S_ISREG(info.st_mode)) {
    *bytes += static_cast<uint64_t>(info.st_size);
    return true;
  }
  if (!S_ISDIR(info.st_mode)) return false;
  DIR *directory = opendir(path.c_str());
  if (!directory) return false;
  bool ok = true;
  while (ok) {
    dirent *entry = readdir(directory);
    if (!entry) break;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    ok = MeasureBackupTree(path + "/" + entry->d_name, bytes);
  }
  closedir(directory);
  return ok;
}

void SetBackupUploadProgress(uint64_t copied, uint64_t total, const char *label = "NAS upload") {
  const int percent = total == 0 ? 100 : static_cast<int>(
      std::min<uint64_t>(copied, total) * 100ULL / total);
  std::ostringstream detail;
  detail << label << ": " << copied / (1024ULL * 1024ULL) << " MB of "
         << total / (1024ULL * 1024ULL) << " MB (" << percent << "%)";
  DataManager::SetValue("tw_size_progress", detail.str());
  DataManager::SetProgress(static_cast<float>(percent) / 100.0f);
}

bool CopyBackupTree(const std::string &source, const std::string &destination,
                    uint64_t total, uint64_t *copied, const char *label = "NAS upload") {
  if (aera::pc::BackupCancelRequested()) return false;
  struct stat info {};
  if (lstat(source.c_str(), &info) != 0 || S_ISLNK(info.st_mode)) return false;
  if (S_ISDIR(info.st_mode)) {
    if (mkdir(destination.c_str(), info.st_mode & 0777) != 0 && errno != EEXIST)
      return false;
    DIR *directory = opendir(source.c_str());
    if (!directory) return false;
    bool ok = true;
    while (ok) {
      dirent *entry = readdir(directory);
      if (!entry) break;
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;
      ok = CopyBackupTree(source + "/" + entry->d_name,
                          destination + "/" + entry->d_name,
                          total, copied, label);
    }
    closedir(directory);
    return ok;
  }
  if (!S_ISREG(info.st_mode)) return false;

  const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC);
  if (input < 0) return false;
  const int output = open(destination.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          info.st_mode & 0777);
  if (output < 0) {
    close(input);
    return false;
  }
  std::vector<char> buffer(1024 * 1024);
  bool ok = true;
  while (ok) {
    if (aera::pc::BackupCancelRequested()) { ok = false; break; }
    const ssize_t read_bytes = read(input, buffer.data(), buffer.size());
    if (read_bytes == 0) break;
    if (read_bytes < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    ssize_t offset = 0;
    while (offset < read_bytes) {
      const ssize_t written = write(output, buffer.data() + offset,
                                    read_bytes - offset);
      if (written <= 0) {
        if (errno == EINTR) continue;
        ok = false;
        break;
      }
      offset += written;
      *copied += static_cast<uint64_t>(written);
      SetBackupUploadProgress(*copied, total, label);
    }
  }
  if (close(output) != 0) ok = false;
  close(input);
  if (!ok) unlink(destination.c_str());
  return ok;
}

}

std::vector<AndroidUser> RecoveryAndroidUsers() {
  std::vector<AndroidUser> result;
  const auto *users = PartitionManager.Get_Users_List();
  if (users != nullptr) {
    for (const auto &user : *users) {
      char *end = nullptr;
      const long id = strtol(user.userId.c_str(), &end, 10);
      if (end == user.userId.c_str() || *end != '\0' || id < 0 ||
          id > INT32_MAX)
        continue;
      std::string name = user.userName;
      if (name.empty() || name == user.userId)
        name = id == 0 ? "Owner" : "Android user " + std::to_string(id);
      result.push_back({static_cast<int>(id), std::move(name), user.type,
                        user.isDecrypted});
    }
  }
  if (result.empty()) {
    auto *data = PartitionManager.Find_Partition_By_Path("/data");
    if (data && data->Is_Mounted() && !RecoveryDataLocked())
      return UnencryptedAndroidUsers("/data", true,
                                     PartitionManager.Storage_Is_Encrypted());
  }
  std::sort(result.begin(), result.end(),
            [](const AndroidUser &left, const AndroidUser &right) {
              return left.id < right.id;
            });
  return result;
}

std::vector<Volume> RecoveryVolumes(const std::string &kind) {
  std::vector<PartitionList> source;
  PartitionManager.Get_Partition_List(kind, &source);
  std::vector<Volume> result;
  for (const auto &part : source) {
    std::string name = part.Display_Name;
    const auto suffix = name.find(" (");
    if (suffix != std::string::npos) name.resize(suffix);
    if (part.Mount_Point == "DALVIK") name = "Dalvik / ART cache";
    if (part.Mount_Point == "INTERNAL") name = "Internal storage";
    if (name.empty() || name.find('{') != std::string::npos) name = part.Mount_Point;
    result.push_back({name, part.Mount_Point, part.PartitionSize, part.selected != 0});
  }
  return result;
}

UsbFormatTarget RecoveryUsbFormatTarget(const std::string &path) {
  UsbFormatTarget result;
  result.path = path;
  const auto storage = RecoveryVolumes("storage");
  const auto wipe = RecoveryVolumes("wipe");
  const auto volume = std::find_if(storage.begin(), storage.end(),
      [&](const Volume &v) { return v.path == path; });
  if (volume == storage.end() || std::none_of(wipe.begin(), wipe.end(),
      [&](const Volume &v) { return v.path == path; })) return result;
  auto *partition = PartitionManager.Find_Partition_By_Path(path);
  if (!partition || partition->Get_Mount_Point() != path || !partition->Is_Present) return result;
  struct stat block{};
  char canonical[PATH_MAX]{};
  if (!realpath(partition->Actual_Block_Device.c_str(), canonical) ||
      stat(canonical, &block) || !S_ISBLK(block.st_mode)) return result;
  result.block = canonical;
  const std::string sys = "/sys/dev/block/" + std::to_string(major(block.st_rdev)) +
      ":" + std::to_string(minor(block.st_rdev));
  if (!realpath(sys.c_str(), canonical)) return result;
  const std::string device = canonical;
  // Only real USB-backed partitions, never internal UFS/eMMC, device-mapper,
  // network storage, or the parent disk containing a partition table.
  if (device.find("/usb") == std::string::npos ||
      access((device + "/partition").c_str(), F_OK)) return result;
  result.available = true;
  result.name = volume->name;
  result.filesystem = partition->Current_File_System;
  const int fd = open(result.block.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { result.error = "Could not open USB partition."; return result; }
  const bool size_ok = ioctl(fd, BLKGETSIZE64, &result.bytes) == 0 && result.bytes;
  close(fd);
  std::string sequence;
  std::ifstream(device.substr(0, device.find_last_of('/')) + "/diskseq") >> sequence;
  if (!size_ok || sequence.empty()) {
    result.error = "This device cannot safely identify the USB attachment. Formatting is unavailable.";
    return result;
  }
  result.identity = device + ":" + sequence + ":" + std::to_string(result.bytes);
  std::ifstream filesystems("/proc/filesystems");
  const std::string supported((std::istreambuf_iterator<char>(filesystems)), {});
  for (const auto &fs : std::vector<std::pair<std::string, std::string>>{
      {"vfat", "/system/bin/mkfs.fat"}, {"exfat", "/system/bin/mkexfatfs"},
      {"ext4", "/system/bin/mke2fs"}, {"f2fs", "/system/bin/make_f2fs"}}) {
    if (!access(fs.second.c_str(), X_OK) && supported.find(fs.first) != std::string::npos)
      result.filesystems.push_back(fs.first);
  }
  if (result.filesystems.empty()) result.error = "No supported USB formatting tools are available.";
  return result;
}

static int FormatUsb(const JobRequest &request) {
  const auto target = RecoveryUsbFormatTarget(request.path);
  if (request.confirmation != "format-usb" || !request.partitions.empty() ||
      !target.available || !target.error.empty() || target.identity.empty() ||
      target.identity != request.usb_identity ||
      std::find(target.filesystems.begin(), target.filesystems.end(), request.usb_filesystem) == target.filesystems.end()) {
    LOGERR("USB format refused: target changed or request is not authorized.\n");
    return 1;
  }
  auto *partition = PartitionManager.Find_Partition_By_Path(request.path);
  PartitionManager.Remove_MTP_Storage(request.path);
  const auto finish = [&](int result) {
    PartitionManager.Update_System_Details();
    if (partition->Is_Mounted()) PartitionManager.Add_MTP_Storage(request.path);
    return result;
  };
  if (!partition->UnMount(true)) return finish(1);
  const auto current = RecoveryUsbFormatTarget(request.path);
  if (current.identity != target.identity || !current.error.empty()) return finish(1);
  android::base::unique_fd fd(open(target.block.c_str(), O_RDWR | O_CLOEXEC));
  struct stat block{};
  if (fd.get() < 0 || fstat(fd.get(), &block)) return finish(1);
  uint64_t disk_sequence = 0, capacity = 0;
  constexpr unsigned long kGetDiskSequence = _IOR(0x12, 128, uint64_t);
  const auto sequence_end = target.identity.find_last_of(':');
  const auto sequence_start = target.identity.find_last_of(':', sequence_end - 1);
  if (ioctl(fd.get(), kGetDiskSequence, &disk_sequence) ||
      ioctl(fd.get(), BLKGETSIZE64, &capacity) || capacity != target.bytes ||
      std::to_string(disk_sequence) != target.identity.substr(sequence_start + 1,
          sequence_end - sequence_start - 1)) return finish(1);
  // Reject mounted aliases too; never force or lazily unmount a busy drive.
  FILE *mounts = setmntent("/proc/mounts", "r");
  if (!mounts) return finish(1);
  bool mounted = false;
  while (auto *entry = getmntent(mounts)) {
    struct stat source{};
    if (!stat(entry->mnt_fsname, &source) && S_ISBLK(source.st_mode) &&
        source.st_rdev == block.st_rdev) mounted = true;
  }
  endmntent(mounts);
  if (mounted || RecoveryUsbFormatTarget(request.path).identity != target.identity) return finish(1);
  // Pin the opened partition for the formatter instead of resolving a changing
  // /dev/block symlink. Only fixed tool names and numeric fd paths reach shell.
  const std::string pinned = "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(fd.get());
  const std::string command = request.usb_filesystem == "vfat" ? "/system/bin/mkfs.fat -F 32 " :
      request.usb_filesystem == "exfat" ? "/system/bin/mkexfatfs " :
      request.usb_filesystem == "f2fs" ? "/system/bin/make_f2fs -f " :
      "/system/bin/mke2fs -t ext4 -F ";
  DataManager::SetValue("tw_partition", request.path);
  if (TWFunc::Exec_Cmd(command + pinned) != 0 || fsync(fd.get()) != 0) return finish(1);
  if (RecoveryUsbFormatTarget(request.path).identity != target.identity) return finish(1);
  partition->Check_FS_Type();
  if (partition->Current_File_System != request.usb_filesystem || !partition->Mount(true)) {
    LOGERR("USB formatted, but filesystem verification or remount failed.\n");
    return finish(1);
  }
  DataManager::SetValue("ui_progress", 100);
  return finish(0);
}

static std::map<std::string, images::Block> NamedImageBlocks() {
  std::map<std::string, images::Block> blocks;
  for (const char *root : {"/dev/block/by-name", "/dev/block/bootdevice/by-name"}) {
    DIR *directory = opendir(root);
    if (!directory) continue;
    while (const auto *entry = readdir(directory)) {
      const std::string name = entry->d_name;
      if (!images::ValidName(name) || blocks.count(name)) continue;
      char canonical[PATH_MAX]{};
      const auto path = std::string(root) + "/" + name;
      struct stat info{};
      if (!realpath(path.c_str(), canonical) || stat(canonical, &info) || !S_ISBLK(info.st_mode)) continue;
      const int fd = open(canonical, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      uint64_t bytes = 0;
      if (fd >= 0 && ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes)
        blocks.emplace(name, images::Block{canonical, bytes});
      if (fd >= 0) close(fd);
    }
    closedir(directory);
  }
  return blocks;
}

std::vector<Volume> RecoveryImageVolumes() {
  const auto flashable = RecoveryVolumes("aera_flashimg");
  const auto blocks = NamedImageBlocks();
  std::set<std::string> represented;
  std::vector<Volume> result;
  for (auto volume : flashable) {
    TWPartition *partition = PartitionManager.Find_Partition_By_Path(volume.path);
    if (!partition) continue;
    volume.slot_select = partition->Is_SlotSelect();
    volume.logical = partition->Get_Super_Status();
    if (!volume.logical && partition->Current_File_System != "emmc" &&
        partition->Current_File_System != "mtd" && partition->Current_File_System != "bml") continue;
    if (!volume.logical) {
      represented.insert(images::BaseName(volume.path.substr(volume.path.find_last_of('/')+1)));
      for (const auto &[name, block] : blocks) {
        char actual[PATH_MAX]{};
        if (realpath(partition->Actual_Block_Device.c_str(), actual) && block.path == actual)
          represented.insert(images::BaseName(name));
      }
    }
    result.push_back(std::move(volume));
  }
  auto physical = images::PhysicalTargets(blocks, represented);
  result.insert(result.end(), physical.begin(), physical.end());
  std::stable_sort(result.begin(), result.end(),
                   [](const Volume &left, const Volume &right) {
                     if (left.logical != right.logical) return !left.logical;
                     return left.name < right.name;
                   });
  return result;
}

std::vector<PayloadFlashTarget> RecoveryPayloadTargets(const std::vector<std::string> &names) {
  const auto volumes = RecoveryImageVolumes();
  std::vector<PayloadFlashTarget> targets;
  const auto slot = RecoverySlot();
  for (const auto &name : names) {
    auto volume = std::find_if(volumes.begin(), volumes.end(),
        [&](const auto &v) { return PayloadTargetMatches(name, v.path); });
    if (volume != volumes.end())
      targets.push_back({name, volume->path, volume->bytes, false, 0, {}});
    else targets.push_back(payload::DiscoverRawTarget(name, slot));
  }
  return targets;
}

std::string RecoveryStorage() { return DataManager::GetCurrentStoragePath(); }
std::string RecoveryInternalStorage() {
  if (RecoveryDataLocked()) return {};
  const auto configured = DataManager::GetStrValue(TW_INTERNAL_PATH);
  if (!configured.empty()) return configured;
  return "/sdcard";
}
std::string RecoveryBackupRoot() { return DataManager::GetStrValue(TW_BACKUPS_FOLDER_VAR); }
bool RecoveryDeleteBackup(const std::string &folder) {
  std::string canonical_backup;
  if (!ResolveDirectBackupChild(RecoveryBackupRoot(), folder, nullptr,
                                &canonical_backup))
    return false;
  return TWFunc::removeDir(canonical_backup, false) == 0;
}

bool RecoveryBackupCanUpload(const std::string &folder) {
#ifdef OF_ENABLE_WLAN
  if (!NasManager::IsMounted()) return false;
  std::string canonical_root;
  if (!ResolveDirectBackupChild(RecoveryBackupRoot(), folder,
                                &canonical_root, nullptr))
    return false;
  char storage_path[PATH_MAX] = {};
  if (realpath(RecoveryStorage().c_str(), storage_path) == nullptr)
    return false;
  const std::string canonical_storage(storage_path);
  return canonical_storage != NasManager::Mount_Point &&
      PathInside(canonical_root, canonical_storage);
#else
  (void)folder;
  return false;
#endif
}

std::string RecoverySlot() { return PartitionManager.Get_Active_Slot_Display(); }
std::string RecoveryBootSlot() {
  const auto suffix = android::base::GetProperty("ro.boot.slot_suffix", "");
  if (suffix == "_a" || suffix == "_b") return suffix.substr(1);
  const auto slot = android::base::GetProperty("ro.boot.slot", "");
  return slot == "a" || slot == "b" ? slot : "";
}
std::string RecoveryVersion() { return DataManager::GetStrValue(TW_VERSION_VAR); }
std::string RecoveryBuildType() {
  return DataManager::GetStrValue(BUILD_TYPE_STR);
}
std::string RecoveryBuildStatus() {
  return DataManager::GetStrValue(AERA_BUILD_STATUS_STR);
}
std::string RecoveryDevice() {
  char model[PROPERTY_VALUE_MAX] = {};
  property_get("ro.product.model", model, "");
  if (model[0] != '\0' && std::string(model) != "AOSP on ARM64") return model;
  std::string device = DataManager::GetStrValue(AERA_COMPATIBILITY_DEVICE);
  if (device.empty()) device = DataManager::GetStrValue("fox_product_device");
  return device;
}
std::string RecoveryBuildDate() {
  return DataManager::GetStrValue("AERA_BUILD_DATE_REAL");
}
std::string RecoveryMaintainer() {
  return DataManager::GetStrValue(AERA_MAINTAINER_STR);
}
bool RecoverySetActiveSlot(const std::string &slot) {
  if (slot != "A" && slot != "B") return false;
  if (RecoverySlot() == slot) return true;

  // Match OrangeFox's setbootslot action: /vendor may hold resources from the
  // current slot and must be detached before PartitionManager updates it.
  if (PartitionManager.Find_Partition_By_Path("/vendor") &&
      !PartitionManager.UnMount_By_Path("/vendor", false)) {
    LOGINFO("AERA: /vendor did not unmount normally; using lazy unmount.\n");
    umount2("/vendor", MNT_DETACH);
  }
  PartitionManager.Set_Active_Slot(slot);
  return RecoverySlot() == slot;
}
SnapshotCowStatus RecoverySnapshotCowStatus() {
  SnapshotCowStatus status;
  status.supported = PartitionManager.Get_Super_Status();
  if (!status.supported) return status;

  std::vector<AeraSnapshotCowPartition> partitions;
  status.metadata_readable = PartitionManager.Get_Snapshot_Cow_Partitions(
      &partitions, &status.safe_to_remove);
  if (!status.metadata_readable) return status;
  for (const auto &partition : partitions) {
    status.bytes += partition.bytes;
    status.partitions.push_back(partition.name);
  }
  return status;
}
bool RecoveryDataLocked() {
  return DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0 &&
         DataManager::GetIntValue(TW_IS_DECRYPTED) == 0;
}

bool RecoveryEnterFastbootd() {
  if (!android::base::GetBoolProperty("ro.boot.dynamic_partitions", false)) {
    LOGERR("AERA live fastbootd: dynamic partitions are unavailable.\n");
    return false;
  }

  // Remember whether device-specific post-decryption resources were active.
  // Unmapping Super necessarily tears down stock vendor/odm bind mounts, so
  // the standard per-device hook must be replayed after returning.
  gLiveFastbootPostDecryptReady =
      android::base::GetBoolProperty("post.decrypt.modules", false);

  // Preserve whether MTP was enabled so the exact recovery USB state can be
  // restored when the user returns. Detach USB before removing dm devices.
  TWFunc::Toggle_MTP(false);
  property_set("sys.usb.config", "none");
  usleep(200000);
  if (!PartitionManager.Unmap_Super_Devices(true)) {
    LOGERR("AERA live fastbootd: could not release dynamic partitions.\n");
    PartitionManager.Setup_Super_Devices();
    PartitionManager.Prepare_All_Super_Volumes();
    PartitionManager.Restart_Quiesced_Dynamic_Services();
    property_set("sys.usb.config", "adb");
    TWFunc::Toggle_MTP(true);
    gLiveFastbootPostDecryptReady = false;
    return false;
  }

#ifdef AB_OTA_UPDATER
  DataManager::SetValue("tw_active_slot",
                        PartitionManager.Get_Active_Slot_Display());
#endif
  // Publish the initial selection before fastbootd starts. When wireless
  // Fastboot is available, initialize the selector transport so later pill
  // changes only update the protocol property and never restart the daemon.
  LoadAeraPreferencesIfAvailable();
  const bool wireless_fastboot =
      DataManager::GetIntValue(kFastbootWifiPreference) == 1 &&
      FastbootWifiPrerequisites();
  property_set(kFastbootProtocolProperty, wireless_fastboot ? "tcp" : "usb");
  android::base::SetProperty(TW_FASTBOOT_MODE_PROP, "1");
  property_set("ro.aera.fastbootd", "1");
  property_set("ro.boot.verifiedbootstate", "orange");
  TWFunc::RunFoxScript("/system/bin/postfastboot.sh", "");
  property_set("sys.usb.config", "fastboot");
  if (wireless_fastboot) {
#ifdef OF_ENABLE_WLAN
    // The extra TCP endpoint is loopback-only. Keep secure, paired ADB alive
    // so an authorized host can reach it exclusively through `adb forward`;
    // USB Fastboot remains available at the same time.
    if (!AeraAdbd::StartSecure(5555)) {
      LOGERR("AERA fastboot Wi-Fi: secure ADB failed; restoring USB fastboot.\n");
      RestartFastbootTransport(false);
    }
#endif
  }
  LOGINFO("AERA live fastbootd: service active; UI display context preserved.\n");
  return true;
}

bool RecoveryLeaveFastbootd(bool initialize_recovery) {
  const bool replay_post_decrypt = gLiveFastbootPostDecryptReady ||
      android::base::GetBoolProperty("post.decrypt.modules", false);
  property_set("sys.usb.config", "none");
  usleep(200000);
  property_set("ro.aera.fastbootd", "0");
  android::base::SetProperty(TW_FASTBOOT_MODE_PROP, "0");
  property_set("service.adb.aera_wifi_only", "0");
  property_set(kFastbootProtocolProperty, "usb");

  PartitionManager.Setup_Super_Devices();
  if (!PartitionManager.Prepare_All_Super_Volumes(initialize_recovery)) {
    LOGERR("AERA live fastbootd: could not restore dynamic partitions.\n");
    PartitionManager.Unmap_Super_Devices(true);
    property_set("sys.usb.config", "fastboot");
    property_set("ro.aera.fastbootd", "1");
    android::base::SetProperty(TW_FASTBOOT_MODE_PROP, "1");
    return false;
  }

  if (initialize_recovery) {
    LOGINFO("AERA live fastbootd: initializing cold recovery backend.\n");
    // Cold fastbootd intentionally skipped vendor/vendor_dlkm module work.
    // Do it now, after Super exists. On devices using the modules-loaded init
    // contract this also starts the crypto HAL dependency chain.
    PartitionManager.Prepare_Deferred_Recovery_Modules();
    const bool encrypted =
        android::base::GetProperty("ro.crypto.state", "") == "encrypted";
    const bool has_prepdecrypt =
        access("/vendor/bin/prepdecrypt.sh", R_OK) == 0 ||
        access("/vendor/etc/init/prepdecrypt.rc", R_OK) == 0 ||
        !android::base::GetProperty("prepdecrypt.setpatch", "").empty() ||
        !android::base::GetProperty("init.svc.prepdecrypt.vendor", "").empty();
    const bool has_delayed_crypto_hal =
        !android::base::GetProperty("init.svc.vendor.crypto-hal-delay", "").empty();
    if (encrypted && has_delayed_crypto_hal &&
        !android::base::GetBoolProperty("vendor.crypto_hal.ready", false)) {
      LOGINFO("AERA live fastbootd: waiting for deferred crypto HALs.\n");
      for (int attempt = 0; attempt < 150; ++attempt) {
        if (android::base::GetBoolProperty("vendor.crypto_hal.ready", false)) break;
        usleep(100000);
      }
      if (!android::base::GetBoolProperty("vendor.crypto_hal.ready", false))
        LOGERR("AERA live fastbootd: deferred crypto HAL readiness timed out.\n");
    }
    if (encrypted && has_prepdecrypt) {
      // prepdecrypt may already have completed while cold fastbootd had Super
      // intentionally unmapped. Re-run its init-managed service now so it
      // observes the fully restored recovery partition state.
      const bool can_restart_prepdecrypt =
          !android::base::GetProperty("init.svc.prepdecrypt.vendor", "").empty();
      if (can_restart_prepdecrypt) {
        property_set("crypto.ready", "0");
        property_set("ctl.restart", "prepdecrypt.vendor");
      }
      LOGINFO("AERA live fastbootd: waiting for prepdecrypt crypto readiness.\n");
      for (int attempt = 0; attempt < 150; ++attempt) {
        if (android::base::GetBoolProperty("crypto.ready", false)) break;
        usleep(100000);
      }
      if (!android::base::GetBoolProperty("crypto.ready", false))
        LOGERR("AERA live fastbootd: prepdecrypt readiness timed out.\n");
    }
    PartitionManager.Setup_Fstab_Partitions(true);
    PartitionManager.Fox_Set_Dynamic_Partition_Props();
    // Match normal recovery startup. Keystore2 must read the database from
    // recovery-owned storage after metadata decryption; an old copy left by
    // the fastbootd startup cannot unwrap the current synthetic-password key.
    PartitionManager.Prepare_Crypto_Keystore();
  }

  if (replay_post_decrypt) {
    // This is the common device-tree contract used to restore any stock
    // vendor/odm mounts and hardware services that depend on them. Toggling
    // the property creates a new init event even though decryption itself was
    // intentionally preserved across the live fastbootd session.
    property_set("post.decrypt.modules", "false");
    usleep(100000);
    property_set("post.decrypt.modules", "true");
    LOGINFO("AERA live fastbootd: replayed post-decryption hardware setup.\n");
  }
  // Super unmapping stops any init service whose process still maps files
  // from vendor/odm. Device hooks restore their normal dependency chains;
  // this exact replay also covers independent services such as vibrator HALs.
  PartitionManager.Restart_Quiesced_Dynamic_Services();
  gLiveFastbootPostDecryptReady = false;

  if (!initialize_recovery || !RecoveryDataLocked()) {
    if (!TWFunc::Toggle_MTP(true)) property_set("sys.usb.config", "adb");
  } else {
    property_set("sys.usb.config", "adb");
  }
  PartitionManager.Update_System_Details();
  LOGINFO("AERA live fastbootd: recovery backend restored without reboot.\n");
  return true;
}

void RecoveryCompleteColdStartup() {
  PartitionManager.Update_System_Details();
  RecoveryWifiInitialize();
  if (!RecoveryDataLocked()) {
    DataManager::SetValue("OTA_decrypted", "1");
    DataManager::ReadSettingsFile();
    if (!TWFunc::Toggle_MTP(true)) property_set("sys.usb.config", "adb");
  } else {
    property_set("sys.usb.config", "adb");
  }
  TWFunc::check_selinux_support();
  TWFunc::RunFoxScript("/system/bin/postrecoveryboot.sh", "");
  LOGINFO("AERA live fastbootd: cold recovery startup complete.\n");
}

int RecoveryCredentialType() {
  return DataManager::GetIntValue(TW_CRYPTO_PWTYPE);
}

bool RecoveryUsesFileBasedEncryption() {
  return DataManager::GetIntValue(TW_IS_FBE) != 0;
}

int RecoveryPatternGridSize() {
  const int size = DataManager::GetIntValue("tw_gui_pattern_grid_size");
  return size >= 3 && size <= 6 ? size : 3;
}

void RecoveryEnsurePcStorage() {
  PartitionManager.Add_PC_Storage();
}

int RecoveryRunPcBackup(const JobRequest &request) {
  using namespace aera::pc;
  const bool computer = request.backup_source == "computer";
  const bool apps = request.backup_kind == "apps";
  const bool backup = request.backup_action == "backup";
  const bool restore = request.backup_action == "restore";
  const std::string old_storage = RecoveryStorage();
  const std::string old_folder = DataManager::GetStrValue(TW_BACKUPS_FOLDER_VAR);
  bool mounted = false;
  bool virtual_storage = false;
  std::string staged;
  struct Cleanup {
    bool &mounted, &virtual_storage; const std::string &old_storage, &old_folder; std::string &staged;
    ~Cleanup() {
      DataManager::SetValue("tw_storage_path", old_storage);
      DataManager::SetValue(TW_BACKUPS_FOLDER_VAR, old_folder);
      if (mounted) storage::Unmount();
      storage::Channel().Stop();
      if (virtual_storage) {
        auto *part = PartitionManager.Find_Partition_By_Path(storage::kMount);
        PartitionManager.Remove_Partition_By_Path(storage::kMount);
        delete part;
      }
      if (!staged.empty()) TWFunc::removeDir(staged, false);
    }
  } cleanup{mounted, virtual_storage, old_storage, old_folder, staged};
  Json::Value result(Json::objectValue);
  auto fail = [&](const std::string &error) {
    std::string message = error;
    if (computer) {
      const auto detail = storage::Channel().LastError();
      if (!detail.empty()) message += "\nComputer storage: " + detail;
    }
    result["error"] = message; UpdateBackupResult(result);
    DataManager::SetValue("tw_file_progress", message); gui_print("%s\n", message.c_str()); return 1;
  };
  auto progress = [](unsigned value, const std::string &detail) {
    DataManager::SetProgress(value / 100.0f); DataManager::SetValue("tw_file_progress", detail);
  };
  if (request.backup_kind != "recovery" && !apps) return fail("Unknown backup type.");
  if (request.backup_action != "apps" && request.backup_action != "inspect" &&
      request.backup_action != "snapshots" && request.backup_action != "storage" &&
      !backup && !restore) return fail("Unknown backup operation.");
  if (!computer || (restore && request.backup_method == "transfer")) {
    std::string error;
    if (!RecoveryPrepareBackupStorage(request.backup_storage, &error)) return fail(error);
  }
  if (request.backup_action == "storage") {
    result["detail"] = "Storage ready";
    UpdateBackupResult(result); return 0;
  }
  if (computer && request.backup_action != "apps") {
    if (PartitionManager.Find_Partition_By_Path(storage::kMount)) return fail("Computer storage is already in use.");
    RecoveryEnsurePcStorage(); virtual_storage = true;
    std::string error;
    if (!storage::Mount(error)) return fail(error);
    mounted = true;
    auto *part = PartitionManager.Find_Partition_By_Path(storage::kMount);
    if (!part) return fail("Computer storage was not initialized.");
    part->Is_Present = true;
  }
  JobRequest native = request;
  if (restore && computer && request.backup_method == "transfer") {
    const auto destinations = RecoveryVolumes("storage");
    if (request.backup_storage == storage::kMount || std::none_of(destinations.begin(), destinations.end(),
          [&](const Volume &volume) { return volume.path == request.backup_storage; }) ||
        !PartitionManager.Mount_By_Path(request.backup_storage, true)) return fail("Choose mounted device storage for the transfer.");
    if (!apps) {
      auto *destination = PartitionManager.Find_Partition_By_Path(request.backup_storage);
      for (const auto &path : request.partitions) {
        auto *target = PartitionManager.Find_Partition_By_Path(path);
        if (!target) return fail("Select available restore partitions.");
        bool preserve_media = target->Get_Mount_Point() == "/data" &&
            DataManager::GetIntValue(TW_HAS_DATA_MEDIA) != 0;
#ifdef TW_OEM_BUILD
        preserve_media = false;
#endif
        if ((target == destination && !preserve_media) ||
            (path == "/storage" && destination && destination->Get_Mount_Point() == "/data"))
          return fail("The selected restore would erase its transfer folder. Use another storage device or direct streaming.");
      }
    }
    uint64_t bytes = 0, copied = 0; struct statvfs info{};
    progress(0, "Measuring the computer backup");
    if (!MeasureBackupTree(request.path, &bytes) || statvfs(request.backup_storage.c_str(), &info) ||
        static_cast<uint64_t>(info.f_bavail) * info.f_frsize < bytes + 16 * 1024 * 1024)
      return fail("Not enough device storage for a complete transfer. Direct streaming remains available.");
    const std::string parent = request.backup_storage + "/AERA/PCTransfers";
    if (!TWFunc::Recursive_Mkdir(parent)) return fail("Could not create the transfer directory.");
    std::string pattern = parent + "/backup-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
    if (!mkdtemp(name.data())) return fail("Could not allocate the transfer directory.");
    staged = name.data(); native.path = staged + "/backup";
    if (native.path.size() > 220) return fail("Choose a shorter storage path for the transfer.");
    if (!CopyBackupTree(request.path, native.path, bytes, &copied, "Backup transfer"))
      return fail("The transfer did not complete. Nothing was restored.");
    DataManager::SetValue("tw_storage_path", request.backup_storage);
    storage::Unmount(); mounted = false;
  }
  if (apps) {
    const auto app_progress = [](const AppVaultProgress &value) {
      DataManager::SetProgress(value.percent / 100.0f);
      DataManager::SetValue("tw_operation", value.stage);
      DataManager::SetValue("tw_size_progress", value.amount.empty() ? "Working..." : value.amount);
      std::string files;
      if (value.files_total) files = std::to_string(value.files_done) + " / " + std::to_string(value.files_total);
      if (!value.current_file.empty()) files += "\n" + value.current_file;
      DataManager::SetValue("tw_file_progress", files);
    };
    if (!RunAppVaultRemote(native, result, app_progress)) {
      UpdateBackupResult(result); return fail(result.get("error", "App operation failed.").asString());
    }
    if (backup && computer && storage::WriteError())
      return fail("The computer did not confirm all backup writes. Do not use this backup until a complete retry succeeds.");
    if (backup) result["folder"] = computer ? request.path.substr(strlen(storage::kMount)) : request.path;
  } else if (request.backup_action == "inspect") {
    result["partitions"] = Json::Value(Json::arrayValue);
    for (const auto &volume : RecoveryRestoreVolumes(request.path)) {
      Json::Value item; item["path"] = volume.path; item["name"] = volume.name;
      item["bytes"] = Json::UInt64(volume.bytes); result["partitions"].append(item);
    }
    if (result["partitions"].empty()) return fail("This folder does not contain a complete compatible recovery backup.");
  } else if (backup) {
    native.job = Job::kBackup;
    if (PartitionManager.Check_Backup_Name(native.name, true, false) != 0)
      return fail("Use a valid backup name.");
    if (!RecoverySetStorage(computer ? storage::kMount : request.backup_storage)) return fail("Backup storage is unavailable.");
    native.path = computer ? storage::kMount : request.backup_storage;
    native.digest = RecoveryPreference(Preference::kBackupChecksums);
    const auto folder = DataManager::GetStrValue(TW_BACKUPS_FOLDER_VAR) + "/" + native.name;
    struct stat exists{};
    if (native.name.empty() || !lstat(folder.c_str(), &exists)) return fail("Use a new backup name.");
    if (computer || aera::pc::BackupOperationActive()) {
      if (!TWFunc::Create_Dir_Recursive(folder, 0777, 0, 0)) return fail("Could not create the computer backup folder.");
      const int marker = open((folder + "/.aera-incomplete").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (marker < 0 || close(marker)) return fail("Could not mark the backup as incomplete.");
      // The incomplete marker stays until every backup file and digest is closed.
      DataManager::SetValue(TW_BACKUP_NAME, native.name);
      std::string selections;
      const auto available = RecoveryVolumes("backup");
      for (const auto &path : native.partitions) {
        if (path.find(';') != std::string::npos || std::none_of(available.begin(), available.end(),
              [&](const Volume &volume) { return volume.path == path; })) return fail("Select available backup partitions.");
        selections += path + ";";
      }
      DataManager::SetValue("tw_backup_list", selections);
      DataManager::SetValue(TW_USE_COMPRESSION_VAR, native.compression ? 1 : 0);
      DataManager::SetValue(TW_SKIP_DIGEST_GENERATE_VAR, native.digest ? 0 : 1);
      DataManager::SetValue("tw_encrypt_backup", 0);
      TWFunc::SetPerformanceMode(true);
      const int complete = PartitionManager.Run_Backup(false);
      TWFunc::SetPerformanceMode(false);
      if (complete != 1 || (computer && storage::WriteError()) || BackupCancelRequested()) return fail("Backup did not complete. Do not use the unfinished backup.");
      if (unlink((folder + "/.aera-incomplete").c_str())) return fail("Backup data was written, but completion could not be confirmed.");
    } else if (RecoveryRunJob(native)) return fail("Backup failed. Review the recovery log.");
    result["folder"] = computer ? folder.substr(strlen(storage::kMount)) : folder;
  } else if (restore) {
    native.job = Job::kRestore;
    native.digest = RecoveryPreference(Preference::kVerifyBackupChecksums);
    if (computer && request.backup_method != "transfer") DataManager::SetValue("tw_storage_path", storage::kMount);
    const auto available = RecoveryRestoreVolumes(native.path);
    std::string selections;
    for (const auto &path : native.partitions) {
      if (std::none_of(available.begin(), available.end(), [&](const Volume &volume) { return volume.path == path; }))
        return fail("Select partitions present in this backup.");
      selections += path + ";";
    }
    // The legacy variable name is inverted: Run_Restore verifies only when it is 1.
    DataManager::SetValue("tw_restore", native.path);
    DataManager::SetValue("tw_restore_selected", selections);
    DataManager::SetValue(TW_SKIP_DIGEST_CHECK_VAR, native.digest ? 1 : 0);
    TWFunc::SetPerformanceMode(true);
    const int restored = PartitionManager.Run_Restore(native.path);
    TWFunc::SetPerformanceMode(false);
    if (restored != 1 || BackupCancelRequested()) return fail("Restore failed. Some data may already have been written; remain in recovery and retry.");
  } else return fail("Unknown recovery backup operation.");
  result["detail"] = backup ? "Backup completed." : restore ? "Restore completed." : "Backup information loaded.";
  UpdateBackupResult(result); progress(100, result["detail"].asString()); return 0;
}

bool RecoverySetStorage(const std::string &path) {
  const auto volumes = RecoveryVolumes("storage");
  const auto found = std::find_if(volumes.begin(), volumes.end(),
      [&](const Volume &volume) { return volume.path == path; });
  if (found == volumes.end() || !PartitionManager.Mount_By_Path(path, true)) return false;
  DataManager::SetValue("tw_storage_path", path);
  return true;
}

std::vector<BackupStorage> RecoveryBackupStorages() {
  std::vector<BackupStorage> result;
  std::set<std::string> seen;
  const auto nas = RecoveryNasStatus();
  for (const auto &volume : RecoveryVolumes("storage")) {
    if (volume.path == "/pc" || volume.path == "/storage") continue;
    auto *part = PartitionManager.Find_Partition_By_Path(volume.path);
    const bool network = volume.path == "/mnt/nas";
    if (!part || (network ? (!nas.supported || nas.config.host.empty()) :
        (!part->Is_Present || (part->Get_Mount_Point() == "/data" && RecoveryDataLocked()))))
      continue;
    char canonical[PATH_MAX]{};
    const std::string identity = realpath(volume.path.c_str(), canonical) ? canonical : volume.path;
    if (!seen.insert(identity).second) continue;
    const bool mounted = network ? nas.mounted : part->Is_Mounted();
    struct statvfs info{};
    const uint64_t free = mounted && statvfs(volume.path.c_str(), &info) == 0
        ? uint64_t(info.f_bavail) * info.f_frsize : 0;
    result.push_back({volume.name, volume.path, mounted, free});
  }
  return result;
}

bool RecoveryBackupPathReady(const std::string &path) {
  auto *part = PartitionManager.Find_Partition_By_Path(path);
  if (!part || !part->Is_Mounted() ||
      (part->Get_Mount_Point() == "/data" && RecoveryDataLocked())) return false;
#ifdef OF_ENABLE_WLAN
  if (PartitionManager.Is_NAS_Path(path) && !NasManager::IsMounted()) return false;
#endif
  return true;
}

bool RecoveryPrepareBackupStorage(const std::string &path, std::string *error) {
  const auto storages = RecoveryBackupStorages();
  if (std::none_of(storages.begin(), storages.end(),
      [&](const BackupStorage &item) {
        if (item.path == path) return true;
        char left[PATH_MAX]{}, right[PATH_MAX]{};
        return realpath(item.path.c_str(), left) && realpath(path.c_str(), right) &&
            !strcmp(left, right);
      }) ||
      !PartitionManager.Mount_By_Path(path, true) || !RecoveryBackupPathReady(path) ||
      access(path.c_str(), R_OK | W_OK) != 0) {
    if (error) *error = "Storage unavailable";
    return false;
  }
  struct statvfs info{};
  if (statvfs(path.c_str(), &info) != 0 || (info.f_flag & ST_RDONLY)) {
    if (error) *error = "Storage unavailable";
    return false;
  }
  return true;
}

std::string RecoveryAppVaultStorage() {
  LoadAeraPreferencesIfAvailable();
  const auto path = DataManager::GetStrValue("aera_appvault_storage");
  if (!path.empty()) return path;
  for (const auto &storage : RecoveryBackupStorages()) {
    auto *part = PartitionManager.Find_Partition_By_Path(storage.path);
    if (part && part->Get_Mount_Point() == "/data") return storage.path;
  }
  return "/sdcard";
}

bool RecoveryRememberAppVaultStorage(const std::string &path) {
  if (path.empty() || path.front() != '/' || path.size() > 1024 ||
      path.find_first_of("\r\n") != std::string::npos) return false;
  DataManager::SetValue("aera_appvault_storage", path);
  return SaveAeraPreferences();
}

std::vector<Volume> RecoveryRestoreVolumes(const std::string &folder) {
  DataManager::SetValue("tw_restore_list", "");
  DataManager::SetValue("tw_restore_selected", "");
  PartitionManager.Set_Restore_Files(folder);
  if (DataManager::GetIntValue("tw_restore_encrypted") != 0) return {};
  auto volumes = RecoveryVolumes("restore");
  for (auto &volume : volumes) {
    if (volume.path != "ADB Backup")
      volume.bytes = PartitionManager.Get_Restore_Size(folder, volume.path);
  }
  return volumes;
}

SideloadStatus RecoverySideloadStatus() {
  SideloadStatus status;
  status.active = gSideloadActive.load(std::memory_order_acquire);
  status.cancel_requested =
      gSideloadCancelRequested.load(std::memory_order_acquire);
  status.received_bytes =
      gSideloadReceivedBytes.load(std::memory_order_acquire);
  status.total_bytes = gSideloadTotalBytes.load(std::memory_order_acquire);
  return status;
}

bool RecoveryCancelSideload() {
  if (!gSideloadActive.load(std::memory_order_acquire)) return false;
  gSideloadCancelRequested.store(true, std::memory_order_release);
  return CancelAdbSideload();
}

int RecoveryRunSideload() {
  gSideloadReceivedBytes.store(0, std::memory_order_release);
  gSideloadTotalBytes.store(0, std::memory_order_release);
  gSideloadCancelRequested.store(false, std::memory_order_release);
  gSideloadActive.store(true, std::memory_order_release);

  const bool mtp_was_enabled = TWFunc::Toggle_MTP(false);
  TWFunc::SetPerformanceMode(true);
  Device::BuiltinAction reboot_action = Device::REBOOT_BOOTLOADER;
  const int result = twrp_sideload(
      "/", &reboot_action, [](uint64_t received, uint64_t total) {
        gSideloadTotalBytes.store(total, std::memory_order_release);
        gSideloadReceivedBytes.store(received, std::memory_order_release);
      });
  TWFunc::Fox_Property_Set("ctl.start", "adbd");
  TWFunc::Toggle_MTP(mtp_was_enabled);
  PartitionManager.Update_System_Details();
  TWFunc::SetPerformanceMode(false);
  gSideloadActive.store(false, std::memory_order_release);
  return result == 0 ? 0 : 1;
}

namespace {
std::atomic<int> gPayloadFlashStep{-1};
std::atomic<int> gPayloadFlashCount{1};
// Logical images reserve the last fifth of the write stage for readback.
std::atomic<int> gPayloadVerifyStage{0}; // 0: combined raw writer, 1: write, 2: readback
std::atomic<int> gPayloadVerifyProgress{0};
payload::FlashProgressStore gPayloadProgress;
}
PayloadFlashProgress RecoveryPayloadFlashProgress() { return gPayloadProgress.Read(); }
static int RecoveryRunJobImpl(const JobRequest &request);
int RecoveryRunJob(const JobRequest &request) {
  const char *method = request.job == Job::kInstall ? "ZIP installation" :
      request.job == Job::kSideload ? "ADB sideload" :
      request.job == Job::kFlashPayload ? "Advanced payload flash" :
      request.job == Job::kFlashImage ? "Image flash" : nullptr;
  if (!method) return RecoveryRunJobImpl(request);
  std::string details = request.title;
  if (!request.name.empty()) details += " / file name: " + request.name;
  if (request.job == Job::kFlashImage) details += request.both_slots ? " / both slots" : " / current slot";
  if (request.job == Job::kFlashPayload)
    details += std::string(" / direct=") + (request.payload_direct ? "1" : "0") +
        " / full=" + (request.payload_full ? "1" : "0") +
        " / manual protection override=" + (request.payload_override_protection ? "1" : "0");
  for (const auto &partition : request.partitions) details += " / " + partition;
  aera::flashlog::Recording recording(request.path, method, details);
  return recording.Finish(RecoveryRunJobImpl(request));
}
static int RecoveryRunJobImpl(const JobRequest &request) {
  if (request.job == Job::kFormatData && !FormatDataAuthorized(request)) return 1;
  if (request.job == Job::kClearSnapshotCow &&
      !SnapshotCowCleanupAuthorized(request)) return 1;
  gPayloadProgress.Reset();
  DataManager::SetValue("ui_progress", 0);
  DataManager::SetValue("ui_portion_start", 0.0f);
  DataManager::SetValue("ui_portion_size",
      request.job == Job::kInstall || request.job == Job::kSideload
          ? 0.0f : 1.0f);
  DataManager::SetValue("ui_progress_portion", 0);
  DataManager::SetValue("ui_progress_frames", 0);
  DataManager::SetValue("tw_operation", request.title);
  DataManager::SetValue("tw_partition", "");
  DataManager::SetValue("tw_size_progress", "");
  DataManager::SetValue("tw_file_progress", "");
  DataManager::SetValue("aera_install_status", "");
  DataManager::SetValue("aera_installer_native", 0);
  DataManager::SetValue("aera_installer_package", "");
  DataManager::SetValue("aera_installer_device", "");
  DataManager::SetValue("aera_installer_author", "");
  DataManager::SetValue("aera_installer_stage_title", "");
  DataManager::SetValue("aera_installer_stage_detail", "");
  DataManager::SetValue("aera_installer_stage", 0);
  DataManager::SetValue("aera_installer_stage_count", 0);
  DataManager::SetValue("aera_installer_rebooting", 0);
  DataManager::SetValue("aera_installer_reboot_target", "");
  DataManager::SetValue("aera_installer_reboot_seconds", 0);
  DataManager::SetValue("aera_installer_prompt_active", 0);
  DataManager::SetValue("aera_installer_prompt_id", "");
  DataManager::SetValue("aera_installer_prompt_title", "");
  DataManager::SetValue("aera_installer_prompt_message", "");
  DataManager::SetValue("aera_installer_prompt_accept", "");
  DataManager::SetValue("aera_installer_prompt_decline", "");
  if (request.job == Job::kClearSnapshotCow) {
    const auto status = RecoverySnapshotCowStatus();
    if (!status.supported || !status.metadata_readable ||
        !status.safe_to_remove || status.partitions.empty()) return 1;

    std::ostringstream names;
    for (size_t index = 0; index < status.partitions.size(); ++index) {
      if (index != 0) names << ", ";
      names << status.partitions[index];
    }
    DataManager::SetValue("tw_partition", "Snapshot COWs");
    DataManager::SetValue("tw_size_progress",
        std::to_string(status.partitions.size()) + " partitions / " +
        std::to_string(status.bytes / (1024ULL * 1024ULL)) + " MB");
    DataManager::SetValue("tw_file_progress", names.str());
    DataManager::SetProgress(0.1f);
    const bool removed = PartitionManager.Remove_Snapshot_Cow_Partitions();
    if (removed) DataManager::SetProgress(1.0f);
    return removed ? 0 : 1;
  }
  if (request.job == Job::kExtractPayload || request.job == Job::kFlashPayload) {
    const bool flash = request.job == Job::kFlashPayload;
    auto fail = [](const std::string &message) {
      LOGERR("Advanced payload: %s\n", message.c_str());
      DataManager::SetValue("tw_size_progress", message);
      DataManager::SetValue("tw_file_progress", message);
      return 1;
    };
    if (request.partitions.empty() || request.payload_manifest_hash.size() != 32)
      return fail("Missing payload selection or reviewed manifest");
    auto info = payload::InspectZip(request.path, flash);
    if (!info.valid || info.incremental || info.manifest_hash != request.payload_manifest_hash)
      return fail("Package changed or is not a supported full payload; review it again");
    const auto protected_partitions = flash ? payload::ProtectedPartitions() : std::set<std::string>{};
    for (const auto &name : request.partitions)
      if (protected_partitions.count(name) && (request.payload_full || !request.payload_override_protection))
        return fail("Partition protection changed; review the package again without " + name);
    const auto targets = RecoveryPayloadTargets(request.partitions);
    if (request.payload_full) {
      std::vector<std::string> required;
      std::string error;
      if (!flash || !request.payload_direct ||
          !payload::PlanFullFlash(info, targets, required, error, protected_partitions))
        return fail(error.empty() ? "Invalid full-payload flash request" : error);
      if (required != request.partitions)
        return fail("Full-payload selection changed; review the package again");
      const auto snapshots = RecoverySnapshotCowStatus();
      if (snapshots.supported && (!snapshots.metadata_readable || !snapshots.safe_to_remove))
        return fail("Fast flash blocked: OTA snapshots are active or their state cannot be verified. No images written.");
    }
    std::vector<uint64_t> image_sizes;
    std::vector<std::string> image_hashes;
    std::set<std::string> unique_names;
    std::set<std::string> unique_paths;
    for (const auto &name : request.partitions) {
      auto found = std::find_if(info.partitions.begin(), info.partitions.end(),
          [&](const auto &p) { return p.name == name && p.extractable; });
      if (found == info.partitions.end() || !unique_names.insert(name).second)
        return fail("Unsupported or duplicate selected partition: " + name);
      image_sizes.push_back(found->bytes);
      if (found->sha256.size() != 32) return fail("Missing final image SHA-256: " + name);
      image_hashes.push_back(found->sha256);
      if (flash) {
        auto target = std::find_if(targets.begin(), targets.end(),
            [&](const auto &v) { return v.name == name; });
        if (target == targets.end() || target->path.empty()) return fail("No verified flash target for " + name);
        if (!unique_paths.insert(target->path).second) return fail("Duplicate selected flash target: " + name);
        if (target->raw && found->bytes > target->bytes) return fail("Image exceeds partition capacity: " + name);
      }
    }
    auto arb_allowed = [&] {
      const bool firmware = std::any_of(info.partitions.begin(), info.partitions.end(),
          [](const auto &p) { return p.name == "xbl_config"; });
      return payload::ArbAllowsInstall(payload::CompareArb(firmware,
          {info.arb_available, info.arb_index, info.arb_detail}, payload::ReadDeviceArb()),
          request.payload_arb_acknowledged);
    };
    if (flash && (request.payload_slot != RecoverySlot() || !arb_allowed()))
      return fail("Slot or anti-rollback state changed; review the package again");
    std::vector<std::string> batch_targets;
    for (const auto &target : targets) batch_targets.push_back(target.path);
    if (flash && request.payload_direct) {
      if (access(payload::OtaripperExecutable(), X_OK)) return fail("Direct flashing requires the bundled otaripper engine");
      struct DirectPerformance {
        DirectPerformance() { TWFunc::SetPerformanceMode(true); }
        ~DirectPerformance() { TWFunc::SetPerformanceMode(false); }
      } performance;
      DataManager::SetProgress(0.0f);
      DataManager::SetValue("tw_size_progress", "");
      std::string error;
      std::atomic<size_t> verified_count{0};
      bool writer_started = false;
      std::string current_partition;
      const bool ok = payload::WithValidatedPayload(request.path, request.payload_manifest_hash,
          request.partitions, error, [&](int package_fd, std::string &error) {
        const auto fresh = RecoveryPayloadTargets(request.partitions);
        if (fresh.size() != targets.size()) { error = "Targets changed"; return false; }
        std::set<uint64_t> devices;
        for (size_t i = 0; i < targets.size(); ++i) {
          const auto &t = targets[i];
          if (t.path != fresh[i].path || t.raw != fresh[i].raw || t.device != fresh[i].device ||
              (t.raw && t.bytes != fresh[i].bytes)) { error = "Flash target changed"; return false; }
          if (t.raw) {
            if (!devices.insert(t.device).second) { error = "Duplicate block target"; return false; }
          }
        }
        auto run_images = [&]() -> bool {
        const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        const unsigned budget = payload::AvailableWorkers();
        gPayloadProgress.Begin(request.partitions, image_sizes, budget);
        std::vector<std::string> blocks(targets.size());
        // Metadata, mapper changes and mount handling are strictly serial.
        // Complete every target's preparation before acquiring any writer fd.
        for (size_t i = 0; i < targets.size(); ++i) {
          gPayloadProgress.Set(i, PayloadPhase::Preparing);
          if (targets[i].raw) blocks[i] = targets[i].path;
          else if (!PartitionManager.Prepare_Image_Flash_Target(targets[i].path, image_sizes[i], &blocks[i])) {
            current_partition = request.partitions[i];
            error = "Cannot prepare target " + current_partition;
            gPayloadProgress.Set(i, PayloadPhase::Failed);
            return false;
          }
          gPayloadProgress.Set(i, PayloadPhase::Queued);
        }
        // All claims precede the first write and outlive all joined workers,
        // but are destroyed before the outer batch restores mounts/services.
        std::vector<android::base::unique_fd> held(targets.size());
        std::set<uint64_t> claimed;
        for (size_t i = 0; i < targets.size(); ++i) {
          held[i].reset(targets[i].raw ? payload::OpenRawTarget(targets[i], image_sizes[i], error) :
              payload::OpenDirectBlock(blocks[i], image_sizes[i], error));
          struct stat st = {};
          if (held[i].get() < 0 || fstat(held[i].get(), &st) || !claimed.insert(st.st_rdev).second) {
            current_partition = request.partitions[i];
            if (error.empty()) error = "Invalid or aliased block target";
            gPayloadProgress.Set(i, PayloadPhase::Failed);
            return false;
          }
        }
        // Read firmware ARB before writing starts, never while another worker
        // might be replacing the same firmware metadata being inspected.
        if (request.payload_slot != RecoverySlot() || !arb_allowed()) {
          error = "Slot or ARB state changed before direct flash"; return false;
        }
        std::vector<payload::DirectTarget> mapping;
        for (size_t i = 0; i < targets.size(); ++i)
          mapping.push_back({request.partitions[i], held[i].get(), image_sizes[i]});
        DataManager::SetValue("tw_file_progress", "Flashing payload with one shared worker pool");
        LOGINFO("Shared payload pool: %u online CPUs, %u total workers, all %zu targets, no partition windows, one process\n",
            unsigned(cpus > 0 ? cpus : 1), budget, targets.size());
        const auto write_start = std::chrono::steady_clock::now();
        const bool written = payload::RunOtaripperBatch(package_fd, mapping, request.payload_manifest_hash,
            budget, [&](size_t i, PayloadPhase phase, uint64_t done) {
              gPayloadProgress.Set(i, phase, done);
            }, error, writer_started);
        LOGINFO("Shared payload write phase: %.3f seconds, result=%d\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - write_start).count(), written);
        // All writer threads have exited. Independently verify every flushed
        // image, even if another image failed during the write phase.
        const auto state = gPayloadProgress.Read();
        std::vector<size_t> ready;
        for (size_t i = 0; i < targets.size(); ++i) {
          if (state.partitions[i].phase == PayloadPhase::Written) ready.push_back(i);
          else if (state.partitions[i].phase == PayloadPhase::Writing)
            gPayloadProgress.Set(i, PayloadPhase::Failed);
        }
        std::vector<std::string> errors(targets.size());
        const auto verify_start = std::chrono::steady_clock::now();
        const bool joined = ready.empty() || payload::RunParallelImages(ready.size(), budget, [&](size_t task) {
          const size_t i = ready[task];
          const auto start = std::chrono::steady_clock::now();
          gPayloadProgress.Set(i, PayloadPhase::Verifying);
          const bool verified = payload::VerifyWrittenFd(held[i].get(), image_sizes[i], image_hashes[i], errors[i],
              [&](uint64_t done, uint64_t) { gPayloadProgress.Set(i, PayloadPhase::Verifying, done); });
          if (verified) {
            ++verified_count;
            gPayloadProgress.Set(i, PayloadPhase::Verified, image_sizes[i]);
            LOGINFO("Shared payload readback verified: %s, %llu bytes, %.3f seconds\n",
                request.partitions[i].c_str(), static_cast<unsigned long long>(image_sizes[i]),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
          } else {
            if (errors[i].empty()) errors[i] = "Partition verification failed";
            gPayloadProgress.Set(i, PayloadPhase::Failed);
            LOGERR("Shared payload failed: %s: %s\n", request.partitions[i].c_str(), errors[i].c_str());
          }
          return true; // Check other completed targets even after a mismatch.
        });
        LOGINFO("Shared payload readback phase: %.3f seconds, %zu/%zu verified\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - verify_start).count(),
            verified_count.load(), targets.size());
        // Every worker is joined. It is now safe to release claims and restore.
        for (size_t i = 0; i < errors.size(); ++i) if (!errors[i].empty()) {
          if (!error.empty()) error += "; ";
          error += request.partitions[i] + ": " + errors[i];
        }
        if (!joined) error += " Could not start readback workers";
        return written && joined && verified_count.load() == targets.size();
        };
        return PartitionManager.Run_Image_Flash_Batch(batch_targets, run_images);
      });
      gPayloadProgress.Finish();
      if (!ok) {
        if (error.empty()) error = DataManager::GetStrValue("tw_size_progress");
        if (error.empty()) error = "Could not prepare the selected partition for flashing";
        std::string detail = "Direct flash stopped" + (current_partition.empty() ? std::string() : " at " + current_partition) + ": " + error;
        if (writer_started) detail += ". Failed targets may be incomplete. Do not reboot; reflash known-good images.";
        else if (!verified_count.load()) detail += ". Stopped before writing image data.";
        else detail += ". " + std::to_string(verified_count.load()) +
            " images were flashed and verified. Remaining images were not flashed.";
        return fail(detail);
      }
      DataManager::SetValue("tw_file_progress", request.payload_full ?
          "Full payload flashed and verified; active slot unchanged" :
          "Selected images flashed and verified; active slot unchanged");
      DataManager::SetProgress(1.0f);
      return 0;
    }
    const auto root = RecoveryStorage() + "/AERA/Extracted";
    if (!TWFunc::Create_Dir_Recursive(root, 0700)) return fail("Cannot create extraction directory");
    std::string pattern = root + "/payload-XXXXXX";
    std::vector<char> folder(pattern.begin(), pattern.end()); folder.push_back('\0');
    if (!mkdtemp(folder.data())) return fail("Cannot create a new extraction folder");
    const std::string directory(folder.data());
    aera::shared_storage::RepairOnExit shared_extraction_permissions(directory);
    struct Performance {
      Performance() { TWFunc::SetPerformanceMode(true); }
      ~Performance() {
        gPayloadFlashStep.store(-1);
        gPayloadVerifyStage.store(0);
        gPayloadVerifyProgress.store(0);
        TWFunc::SetPerformanceMode(false);
      }
    } performance;
    DataManager::SetProgress(0.0f);
    std::string error;
    if (!payload::ExtractImages(request.path, request.payload_manifest_hash, request.partitions,
        directory, error, [flash](const std::string &stage, uint64_t done, uint64_t total) {
          DataManager::SetValue("tw_file_progress", stage);
          DataManager::SetProgress(total ? float(double(done) / total * (flash ? 0.7 : 1.0)) : 0.0f);
        })) {
      rmdir(directory.c_str());
      return fail(error);
    }
    if (!flash) {
      LOGINFO("Advanced payload images saved to %s\n", directory.c_str());
      DataManager::SetValue("tw_file_progress", "Images saved to " + directory);
      DataManager::SetValue("tw_size_progress", directory);
      DataManager::SetProgress(1.0f);
      return 0;
    }
    // Every selected image is on disk and hash-verified before the first write.
    // No slot switch and no normal OTA postinstall/activation are performed.
    if (request.payload_slot != RecoverySlot() || !arb_allowed())
      return fail("Slot or anti-rollback state changed; verified images kept in " + directory);
    // Resolve again after extraction and claim ALL additional block devices
    // before any write. O_EXCL rejects mounted partitions / mapped holders.
    const auto fresh_targets = RecoveryPayloadTargets(request.partitions);
    if (fresh_targets.size() != targets.size()) return fail("Flash targets changed");
    std::set<uint64_t> raw_devices;
    for (size_t i = 0; i < targets.size(); ++i) {
      const auto &target = targets[i];
      const auto &fresh = fresh_targets[i];
      if (target.path != fresh.path || target.raw != fresh.raw || target.device != fresh.device ||
          (target.raw && target.bytes != fresh.bytes)) return fail("Flash target changed: " + target.name);
      if (!target.raw) continue;
      if (!raw_devices.insert(target.device).second) return fail("Duplicate block target");
    }
    gPayloadFlashCount.store(request.partitions.size());
    DataManager::SetValue("tw_size_progress", "");
    bool normal_write_attempted = false;
    auto run_images = [&]() -> bool {
    std::vector<android::base::unique_fd> raw_handles(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) if (targets[i].raw) {
      raw_handles[i].reset(payload::OpenRawTarget(targets[i], image_sizes[i], error));
      if (raw_handles[i].get() < 0) return false;
    }
    for (size_t i = 0; i < request.partitions.size(); ++i) {
      if (request.payload_slot != RecoverySlot()) { error = "Active slot changed; stopping partial flash"; return false; }
      JobRequest image;
      image.job = Job::kFlashImage;
      image.title = "Flash selected partitions";
      image.path = directory + "/" + request.partitions[i] + ".img";
      image.partitions = {targets[i].path};
      image.expected_image_sha256 = image_hashes[i];
      image.expected_image_bytes = image_sizes[i];
      DataManager::SetValue("tw_file_progress", "Flashing " + request.partitions[i]);
      DataManager::SetValue("ui_progress", 0);
      gPayloadVerifyStage.store(targets[i].raw ? 0 : 1);
      gPayloadVerifyProgress.store(0);
      gPayloadFlashStep.store(i);
      normal_write_attempted = true;
      const int result = targets[i].raw ? (payload::WriteRawImage(raw_handles[i].get(), targets[i],
          image.path, image_sizes[i], image_hashes[i], error, [&](uint64_t done, uint64_t total) {
            if (done >= total / 2)
              DataManager::SetValue("tw_file_progress", "Verifying written image: " + request.partitions[i]);
            DataManager::SetValue("ui_progress", total ? int(100.0 * done / total) : 0);
          }) ? 0 : 1) : RecoveryRunJob(image);
      if (result) {
        if (error.empty()) error = DataManager::GetStrValue("tw_size_progress");
        error = "Stopped flashing " + request.partitions[i] + ": " + error;
        return false;
      }
    }
    return true;
    };
    if (!PartitionManager.Run_Image_Flash_Batch(batch_targets, run_images)) {
      if (error.empty()) error = DataManager::GetStrValue("tw_size_progress");
      return fail(error + (normal_write_attempted ? ". Do not reboot; selected flashing did not complete." :
          ". Stopped before writing image data.") + " Verified images kept in " + directory);
    }
    PartitionManager.Update_System_Details();
    DataManager::SetValue("ui_progress", 100);
    gPayloadFlashStep.store(-1);
    // Remove only the exact temporary images this operation created.
    for (const auto &name : request.partitions) unlink((directory + "/" + name + ".img").c_str());
    rmdir(directory.c_str());
    DataManager::SetValue("tw_file_progress", "Selected images flashed; active slot unchanged");
    DataManager::SetProgress(1.0f);
    return 0;
  }
  if (request.job == Job::kInstall) return aeraui_install_package(request.path.c_str());
  if (request.job == Job::kPcBackupOperation) return RecoveryRunPcBackup(request);
  if (request.job == Job::kNetworkStorage) {
    std::string error;
    if (request.nas_action == "disconnect")
      return RecoveryRunNas({NasOperation::kUnmount});
    const auto current = RecoveryNasStatus();
    if (!current.supported || current.mounted || RecoveryDataLocked()) return 1;
    auto config = request.nas_config;
    const auto remembered = RecoverySavedNasConfig(config.type);
    if (!request.nas_password_update && config.host == remembered.host &&
        config.user == remembered.user && config.auth == remembered.auth)
      config.password = remembered.password;
    if (!request.nas_private_key.empty() &&
        !RecoveryImportNasKey(request.nas_private_key, config.key_name, &error)) {
      DataManager::SetValue("tw_file_progress", error); return 1;
    }
    if (!RecoverySetNasConfig(config, &error)) {
      DataManager::SetValue("tw_file_progress", error); return 1;
    }
    if (request.nas_action == "save") return 0;
    if (request.nas_action != "connect") return 1;
    const int result = RecoveryRunNas({NasOperation::kMountAndUse});
    if (result) DataManager::SetValue("tw_file_progress", RecoveryNasStatus().error);
    return result;
  }
  if (request.job == Job::kRootOperation) {
    if (!root::Enabled()) return 1;
    root::Request operation;
    if (request.root_slot != "a" && request.root_slot != "b") return 1;
    operation.slot = request.root_slot;
    operation.verify_target_kernel = true;
    if (request.root_provider == "kernelsu") operation.provider = root::Provider::kKernelSU;
    else if (request.root_provider == "next") operation.provider = root::Provider::kKernelSUNext;
    else if (request.root_provider == "sukisu") operation.provider = root::Provider::kSukiSU;
    else return 1;
    if (request.root_action == "inspect") operation.job = root::Job::kInspectTarget;
    else if (request.root_action == "refresh") operation.job = root::Job::kRefreshRelease;
    else if (request.root_action == "patch") operation.job = root::Job::kPatch;
    else if (request.root_action == "rollback") operation.job = root::Job::kRollback;
    else if (request.root_action == "manager") operation.job = root::Job::kInstallManager;
    else return 1;
    root::Progress progress;
    std::atomic<bool> done{false};
    bool success = false;
    std::thread worker([&] {
      success = root::Run(operation, progress);
      done.store(true, std::memory_order_release);
    });
    std::string previous;
    do {
      std::string text;
      {
        std::lock_guard<std::mutex> lock(progress.text_mutex);
        text = progress.status;
        if (!progress.detail.empty()) text += "\n" + progress.detail;
      }
      DataManager::SetProgress(progress.value.load() / 100.0f);
      DataManager::SetValue("tw_file_progress", text);
      if (text != previous) { gui_print("%s\n", text.c_str()); previous = text; }
      if (done.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (true);
    worker.join();
    if (success && progress.has_inspection) aera::pc::UpdateRootInspection(progress);
    return success ? 0 : 1;
  }
  if (request.job == Job::kSideload) return RecoveryRunSideload();
  if (request.job == Job::kUploadBackup) {
#ifdef OF_ENABLE_WLAN
    std::string canonical_root;
    std::string canonical_backup;
    std::string backup_name;
    if (!NasManager::IsMounted() ||
        !ResolveDirectBackupChild(RecoveryBackupRoot(), request.path,
                                  &canonical_root, &canonical_backup,
                                  &backup_name))
      return 1;

    char storage_path[PATH_MAX] = {};
    if (realpath(RecoveryStorage().c_str(), storage_path) == nullptr)
      return 1;
    const std::string canonical_storage(storage_path);
    if (canonical_storage == NasManager::Mount_Point ||
        !PathInside(canonical_root, canonical_storage))
      return 1;

    const std::string relative_root =
        canonical_root.substr(canonical_storage.size());
    if (relative_root.empty() || relative_root[0] != '/') return 1;
    const std::string destination_root =
        NasManager::Mount_Point + relative_root;
    const std::string destination = destination_root + "/" + backup_name;
    struct stat destination_info {};
    if (lstat(destination.c_str(), &destination_info) == 0 || errno != ENOENT) {
      DataManager::SetValue("tw_size_progress",
                            "A backup with this name already exists on NAS");
      return 1;
    }

    uint64_t total = 0;
    if (!MeasureBackupTree(canonical_backup, &total) ||
        !TWFunc::Create_Dir_Recursive(destination_root, 0777))
      return 1;

    DataManager::SetValue("tw_partition", "NAS upload");
    DataManager::SetProgress(0.0f);
    SetBackupUploadProgress(0, total);
    if (!NasManager::ResetTransferStats()) return 1;
    uint64_t copied = 0;
    TWFunc::SetPerformanceMode(true);
    bool ok = CopyBackupTree(canonical_backup, destination, total, &copied);
    if (ok) ok = NasManager::WaitForPendingUploads(0, total);
    TWFunc::SetPerformanceMode(false);
    if (!ok) {
      TWFunc::removeDir(destination, false);
      return 1;
    }
    SetBackupUploadProgress(total, total);
    return 0;
#else
    return 1;
#endif
  }
  if (request.job == Job::kFlashImage) {
    if (request.partitions.size() != 1 || request.path.empty() ||
        request.path.find_first_of("'\r\n") != std::string::npos) return 1;
    struct stat image_info {};
    if (stat(request.path.c_str(), &image_info) != 0 ||
        !S_ISREG(image_info.st_mode) || image_info.st_size <= 0) return 1;
    if (!request.expected_image_sha256.empty() &&
        (request.expected_image_sha256.size() != 32 || !request.expected_image_bytes ||
         uint64_t(image_info.st_size) != request.expected_image_bytes)) return 1;
    std::string lower_path = request.path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(),
                   [](unsigned char value) {
                     return static_cast<char>(std::tolower(value));
                   });
    if (lower_path.size() < 4 ||
        lower_path.compare(lower_path.size() - 4, 4, ".img") != 0) return 1;

    const auto targets = RecoveryImageVolumes();
    const std::string &target = request.partitions.front();
    if (target.find(';') != std::string::npos ||
        std::none_of(targets.begin(), targets.end(),
                     [&](const Volume &volume) {
                       return volume.path == target;
                     })) return 1;
    const size_t slash = request.path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= request.path.size()) return 1;
    std::string directory = slash == 0 ? "/" : request.path.substr(0, slash);
    std::string filename = request.path.substr(slash + 1);
    const auto volume = std::find_if(targets.begin(), targets.end(),
        [&](const Volume &item) { return item.path == target; });
    if (request.both_slots && (volume->logical || !volume->slot_select)) return 1;
    if (target.rfind("/block/", 0) == 0) {
      const auto name = target.substr(7);
      std::vector<std::string> names{name};
      if (volume->slot_select) {
        const auto slot = PartitionManager.Get_Active_Slot_Suffix();
        if (slot != "_a" && slot != "_b") return 1;
        names = request.both_slots ? std::vector<std::string>{name+slot, name+(slot == "_a" ? "_b" : "_a")} :
                                   std::vector<std::string>{name+slot};
      }
      const auto blocks = NamedImageBlocks();
      for (const auto &part : names) if (!blocks.count(part)) return 1;
      for (const auto &part : names)
        if (!PartitionManager.Flash_Image_To_Block(directory, filename, blocks.at(part).path, part, true)) return 1;
      DataManager::SetValue("tw_partition", name);
      TWFunc::SetPerformanceMode(true);
      bool success = true;
      for (const auto &part : names) {
        if (!PartitionManager.Flash_Image_To_Block(directory, filename, blocks.at(part).path, part)) {
          success = false; break;
        }
      }
      TWFunc::SetPerformanceMode(false);
      PartitionManager.Update_System_Details();
      return success ? 0 : 1;
    }
    TWPartition *flash_partition = PartitionManager.Find_Partition_By_Path(target);
    const bool logical = flash_partition && flash_partition->Get_Super_Status();
    if (!flash_partition || (request.both_slots && (logical || !flash_partition->Is_SlotSelect()))) return 1;
    DataManager::SetValue("tw_flash_partition", target + ";");
    DataManager::SetValue("tw_flash_both_slots", request.both_slots ? 1 : 0);
    DataManager::SetValue("tw_partition", target);
    TWFunc::SetPerformanceMode(true);
    int result = 1;
    std::function<bool(const std::string &, uint64_t)> verify;
    if (!request.expected_image_sha256.empty()) {
      DataManager::SetValue("tw_size_progress", "");
      verify = [&](const std::string &block, uint64_t expanded_bytes) {
        gPayloadVerifyProgress.store(0);
        gPayloadVerifyStage.store(2);
        DataManager::SetValue("tw_file_progress", "Verifying written image: " + target);
        const auto started = std::chrono::steady_clock::now();
        std::string error;
        const bool ok = expanded_bytes == request.expected_image_bytes &&
            payload::VerifyWrittenBlock(block, request.expected_image_bytes, request.expected_image_sha256,
                error, [](uint64_t done, uint64_t total) {
                  gPayloadVerifyProgress.store(total ? int(100.0 * done / total) : 0);
                });
        if (!ok) {
          if (error.empty()) error = "Written image size changed";
          LOGERR("Payload readback verification failed for %s: %s\n", block.c_str(), error.c_str());
          DataManager::SetValue("tw_size_progress", error);
          return false;
        }
        LOGINFO("Payload readback SHA-256 verified: %s, %llu bytes, %.3f seconds\n", block.c_str(),
            static_cast<unsigned long long>(request.expected_image_bytes),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        return true;
      };
    }
    if (request.both_slots) {
      const std::string original_slot = PartitionManager.Get_Active_Slot_Display();
      const bool first_slot_ok = PartitionManager.Flash_Image(directory, filename, verify);
      bool second_slot_ok = false;
      if (first_slot_ok) {
        PartitionManager.Override_Active_Slot(original_slot == "A" ? "B" : "A");
        second_slot_ok = PartitionManager.Flash_Image(directory, filename, verify);
      }
      PartitionManager.Override_Active_Slot(original_slot);
      result = first_slot_ok && second_slot_ok ? 0 : 1;
    } else {
      result = PartitionManager.Flash_Image(directory, filename, verify) ? 0 : 1;
    }
    DataManager::SetValue("tw_flash_both_slots", 0);
    if (!PartitionManager.Is_Image_Flash_Batch()) PartitionManager.Update_System_Details();
    TWFunc::SetPerformanceMode(false);
    return result;
  }
  if (request.job == Job::kFormatUsb) return FormatUsb(request);
  if (request.job == Job::kFormatData) {
    DataManager::SetValue("tw_partition", "/data");
    char fastboot_mode[PROPERTY_VALUE_MAX] = {};
    property_get(TW_FASTBOOT_MODE_PROP, fastboot_mode, "0");
    const bool restore_fastboot = fastboot_mode[0] == '1' &&
                                  fastboot_mode[1] == '\0';
    // Formatting must never race a host-side fastboot write. Detach the USB
    // function and stop fastbootd for the duration, then publish it again once
    // the partition manager has finished updating storage state.
    if (restore_fastboot) {
      property_set("sys.usb.config", "none");
      usleep(300000);
    }
    TWFunc::SetPerformanceMode(true);
    const int result = PartitionManager.Format_Data() ? 0 : 1;
    PartitionManager.Update_System_Details();
    TWFunc::SetPerformanceMode(false);
    if (restore_fastboot) property_set("sys.usb.config", "fastboot");
    return result;
  }
  if (request.job == Job::kMount || request.job == Job::kUnmount) {
    const auto mounts = RecoveryVolumes("mount");
    const auto found = std::find_if(mounts.begin(), mounts.end(),
        [&](const Volume &volume) { return volume.path == request.path; });
    if (found == mounts.end()) return 1;
    return (request.job == Job::kMount
        ? PartitionManager.Mount_By_Path(request.path, true)
        : PartitionManager.UnMount_By_Path(request.path, true)) ? 0 : 1;
  }
  if (request.partitions.empty()) return 1;
  std::vector<Volume> allowed = request.job == Job::kRestore
      ? RecoveryRestoreVolumes(request.path)
      : RecoveryVolumes(request.job == Job::kBackup ? "backup" : "wipe");
  std::string selections;
  for (const auto &path : request.partitions) {
    if (std::none_of(allowed.begin(), allowed.end(),
        [&](const Volume &volume) { return volume.path == path; }) ||
        path.find(';') != std::string::npos) return 1;
    selections += path + ";";
  }
  int result = 1;
  TWFunc::SetPerformanceMode(true);
  if (request.job == Job::kBackup) {
    if (RecoverySetStorage(request.path)) {
      char name[80];
      std::string backup_name = request.name;
      if (backup_name.empty()) {
        time_t now = time(nullptr);
        struct tm local = {};
        localtime_r(&now, &local);
        strftime(name, sizeof(name), "AERA-%Y-%m-%d-%H-%M-%S", &local);
        backup_name = name;
      }
      DataManager::SetValue(TW_BACKUP_NAME, backup_name);
      DataManager::SetValue("tw_backup_list", selections);
      DataManager::SetValue(TW_USE_COMPRESSION_VAR, request.compression ? 1 : 0);
      DataManager::SetValue(TW_SKIP_DIGEST_GENERATE_VAR,
                            request.digest && RecoveryPreference(Preference::kBackupChecksums) ? 0 : 1);
      DataManager::SetValue("tw_encrypt_backup", 0);
      if (PartitionManager.Check_Backup_Name(backup_name, true, true) == 0)
        result = PartitionManager.Run_Backup(false) ? 0 : 1;
    }
  } else if (request.job == Job::kRestore) {
    DataManager::SetValue("tw_restore", request.path);
    DataManager::SetValue("tw_restore_selected", selections);
    DataManager::SetValue(TW_SKIP_DIGEST_CHECK_VAR,
                          request.digest && RecoveryPreference(Preference::kVerifyBackupChecksums) ? 1 : 0);
    result = PartitionManager.Run_Restore(request.path) ? 0 : 1;
  } else if (request.job == Job::kWipe) {
    result = 0;
    for (const auto &path : request.partitions) {
      int ok;
      if (path == "DALVIK") ok = PartitionManager.Wipe_Dalvik_Cache();
      else if (path == "INTERNAL") ok = PartitionManager.Wipe_Media_From_Data();
      else if (path == "/and-sec") ok = PartitionManager.Wipe_Android_Secure();
      else ok = PartitionManager.Wipe_By_Path(path);
      if (!ok) { result = 1; break; }
    }
  }
  PartitionManager.Update_System_Details();
  TWFunc::SetPerformanceMode(false);
  return result;
}

int RecoveryProgress() {
  const auto payload = gPayloadProgress.Read();
  if (payload.available) return aeraui::payload::OverallProgress(payload);
  int raw = std::clamp(atoi(DataManager::GetStrValue("ui_progress").c_str()), 0, 100);
  const int step = gPayloadFlashStep.load();
  if (step >= 0) {
    const int phase = gPayloadVerifyStage.load();
    if (phase == 1) raw = raw * 80 / 100;
    else if (phase == 2) raw = 80 + std::clamp(gPayloadVerifyProgress.load(), 0, 100) * 20 / 100;
    return 70 + (30 * step + 30 * raw / 100) / std::max(1, gPayloadFlashCount.load());
  }
  return raw;
}
std::string RecoveryOperationDetail() {
  std::string text = DataManager::GetStrValue("tw_operation");
  const auto partition = DataManager::GetStrValue("tw_partition");
  if (!partition.empty()) text += " / " + partition;
  const auto size = DataManager::GetStrValue("tw_size_progress");
  if (!size.empty()) text += "\n" + size;
  const auto files = DataManager::GetStrValue("tw_file_progress");
  if (!files.empty()) text += "\n" + files;
  return text;
}
std::string RecoveryInstallerStatus() {
  return DataManager::GetStrValue("aera_install_status");
}

InstallerPresentation RecoveryInstallerPresentation() {
  InstallerPresentation state;
  state.active = DataManager::GetIntValue("aera_installer_native") == 1;
  state.package_name = DataManager::GetStrValue("aera_installer_package");
  state.device = DataManager::GetStrValue("aera_installer_device");
  state.author = DataManager::GetStrValue("aera_installer_author");
  state.stage_title =
      DataManager::GetStrValue("aera_installer_stage_title");
  state.stage_detail =
      DataManager::GetStrValue("aera_installer_stage_detail");
  state.rebooting =
      DataManager::GetIntValue("aera_installer_rebooting") == 1;
  state.reboot_target =
      DataManager::GetStrValue("aera_installer_reboot_target");
  state.reboot_seconds =
      DataManager::GetIntValue("aera_installer_reboot_seconds");
  state.stage = DataManager::GetIntValue("aera_installer_stage");
  state.stage_count = DataManager::GetIntValue("aera_installer_stage_count");
  return state;
}

InstallerPrompt RecoveryInstallerPrompt() {
  InstallerPrompt prompt;
  prompt.active =
      DataManager::GetIntValue("aera_installer_prompt_active") == 1;
  prompt.id = DataManager::GetStrValue("aera_installer_prompt_id");
  prompt.title = DataManager::GetStrValue("aera_installer_prompt_title");
  prompt.message =
      DataManager::GetStrValue("aera_installer_prompt_message");
  prompt.accept = DataManager::GetStrValue("aera_installer_prompt_accept");
  prompt.decline = DataManager::GetStrValue("aera_installer_prompt_decline");
  return prompt;
}

bool RecoveryAnswerInstallerPrompt(bool accepted) {
  const auto prompt = RecoveryInstallerPrompt();
  if (!prompt.active || prompt.id.empty() || prompt.id.size() > 48) return false;
  for (const unsigned char c : prompt.id) {
    if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
  }

  constexpr const char *kDirectory = "/tmp/aera-installer";
  if (mkdir(kDirectory, 0700) != 0 && errno != EEXIST) return false;
  const std::string path = std::string(kDirectory) + "/response." + prompt.id;
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                                      O_NOFOLLOW,
                      0600);
  if (fd < 0) return false;
  const char *answer = accepted ? "yes\n" : "no\n";
  const size_t length = strlen(answer);
  const bool written = write(fd, answer, length) ==
                       static_cast<ssize_t>(length);
  fsync(fd);
  close(fd);
  if (!written) return false;
  DataManager::SetValue("aera_installer_prompt_active", 0);
  return true;
}
bool RecoveryScreenTimeoutSupported() {
  return DataManager::GetIntValue("tw_no_screen_timeout") == 0;
}
int RecoveryScreenTimeout() {
  LoadAeraPreferencesIfAvailable();
  const int seconds = DataManager::GetIntValue("tw_screen_timeout_secs");
  return seconds >= 30 && seconds <= 300 ? seconds : 60;
}
void RecoverySetScreenTimeout(int seconds) {
  if (RecoveryScreenTimeoutSupported())
    DataManager::SetValue("tw_screen_timeout_secs", std::clamp(seconds, 30, 300));
}
int RecoveryBrightness() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("tw_brightness_pct");
}
void RecoverySetBrightness(int percent) {
  percent = std::max(10, std::min(100, percent));
  const int maximum = DataManager::GetIntValue("tw_brightness_max");
  if (maximum <= 0) return;
  DataManager::SetValue("tw_brightness_pct", percent);
  DataManager::SetValue("tw_brightness", maximum * percent / 100);
  TWFunc::Set_Brightness(DataManager::GetStrValue("tw_brightness"));
}

bool RecoveryFlashlightSupported() {
  return DataManager::GetIntValue(OF_FLASHLIGHT_ENABLE_STR) == 1;
}
bool RecoveryFlashlightEnabled() {
  return RecoveryFlashlightSupported() &&
         DataManager::GetIntValue("of_flash_on") == 1;
}
bool RecoverySetFlashlight(bool enabled) {
  if (!RecoveryFlashlightSupported()) return false;
  if (RecoveryFlashlightEnabled() == enabled) return true;

  std::string first = DataManager::GetStrValue("of_fl_path_1");
  std::string second = DataManager::GetStrValue("of_fl_path_2");
  if (first.empty() && second.empty()) {
    if (TWFunc::Path_Exists("/sys/class/leds/flashlight/brightness")) {
      first = "/sys/class/leds/flashlight";
    } else {
      first = "/sys/class/leds/led:torch_0";
      second = "/sys/class/leds/led:switch_0";
    }
  } else if (first.empty()) {
    first = std::move(second);
  }

  bool changed = false;
  for (const auto &root : std::vector<std::string>{first, second}) {
    if (root.empty()) continue;
    const std::string brightness = root + "/brightness";
    if (!TWFunc::Path_Exists(brightness)) continue;
    std::string maximum = "1";
    const std::string maximum_path = root + "/max_brightness";
    if (TWFunc::Path_Exists(maximum_path)) {
      std::string detected;
      if (TWFunc::read_file(maximum_path, detected) == 0 && !detected.empty())
        maximum = detected;
    }
    if (TWFunc::write_to_file(brightness, enabled ? maximum : "0"))
      changed = true;
  }
  if (changed) DataManager::SetValue("of_flash_on", enabled ? "1" : "0");
  return changed && RecoveryFlashlightEnabled() == enabled;
}
bool RecoveryMtpEnabled() {
  // The UI bridge is built independently from the recovery executable and
  // does not consistently inherit TW_HAS_MTP.  The shared recovery state is
  // updated by Enable_MTP()/Disable_MTP() on every supported build, so use it
  // here instead of compiling the status query into a permanent false value.
  return DataManager::GetIntValue("tw_mtp_enabled") == 1;
}
bool RecoverySetMtp(bool enabled) {
  const bool result = enabled ? PartitionManager.Enable_MTP() : PartitionManager.Disable_MTP();
  if (result) DataManager::SetValue("tw_mtp_enabled", enabled ? 1 : 0);
  return result;
}
static const char *PreferenceVariable(Preference preference) {
  switch (preference) {
    case Preference::kClock24: return "tw_military_time";
    case Preference::kHiddenFiles: return "tw_hidden_files";
    case Preference::kCompression: return TW_USE_COMPRESSION_VAR;
    case Preference::kSha256: return TW_USE_SHA2;
    case Preference::kVerifyZip: return TW_SIGNED_ZIP_VERIFY_VAR;
    case Preference::kPluginAutoUpdate: return "aera_plugin_auto_update";
    case Preference::kUpdateNightly: return "aera_update_nightly";
    case Preference::kRecents: return "aera_recents_enabled";
    case Preference::kPreserveRecovery: return AERA_PRESERVE_RECOVERY_VAR;
    case Preference::kPreserveAbl: return AERA_PRESERVE_ABL_VAR;
    case Preference::kBackupChecksums: return TW_SKIP_DIGEST_GENERATE_VAR;
    case Preference::kVerifyBackupChecksums: return "aera_verify_backup_checksums";
  }
  return nullptr;
}
bool RecoveryPreference(Preference preference) {
  LoadAeraPreferencesIfAvailable();
  const char *variable = PreferenceVariable(preference);
  if (preference == Preference::kBackupChecksums)
    return DataManager::GetIntValue(TW_SKIP_DIGEST_GENERATE_VAR) == 0;
  if (preference == Preference::kVerifyBackupChecksums)
    return DataManager::GetStrValue(variable) != "0";
  return variable && DataManager::GetIntValue(variable) != 0;
}
bool RecoveryPreservationSupported() {
  return DataManager::GetIntValue(AERA_RECOVERY_PRESERVATION_SUPPORTED) != 0;
}
bool RecoveryAblPreservationSupported() {
  return DataManager::GetIntValue(AERA_ABL_PRESERVATION_SUPPORTED) != 0;
}
bool RecoverySha256Available() { return DataManager::GetIntValue(TW_NO_SHA2) == 0; }
bool RecoverySetPreference(Preference preference, bool enabled) {
  const char *variable = PreferenceVariable(preference);
  if (!variable || (preference == Preference::kSha256 && !RecoverySha256Available()) ||
      (preference == Preference::kPreserveAbl && !RecoveryAblPreservationSupported())) return false;
  if (DataManager::SetValue(variable,
      preference == Preference::kBackupChecksums ? (enabled ? 0 : 1) : (enabled ? 1 : 0)) != 0)
    return false;
  if (preference == Preference::kVerifyBackupChecksums)
    return DataManager::SetValue(TW_SKIP_DIGEST_CHECK_VAR, enabled ? 1 : 0) == 0;
  return true;
}
int RecoveryUtcOffset() {
  LoadAeraPreferencesIfAvailable();
  time_t now = time(nullptr);
  struct tm local = {};
  return localtime_r(&now, &local) ? static_cast<int>(local.tm_gmtoff / 60) : 0;
}
bool RecoverySetUtcOffset(int minutes) {
  if (minutes < -720 || minutes > 840 || minutes % 15 != 0) return false;
  char zone[32];
  const int magnitude = std::abs(minutes);
  snprintf(zone, sizeof(zone), "UTC%c%d:%02d", minutes < 0 ? '+' : '-',
           magnitude / 60, magnitude % 60);
  if (DataManager::SetValue(TW_TIME_ZONE_VAR, zone) != 0) return false;
  DataManager::update_tz_environment_variables();
  return true;
}
uint32_t RecoveryAccentColor() {
  LoadAeraPreferencesIfAvailable();
  constexpr uint32_t kDefault = 0x16c8ff;
  const std::string value = DataManager::GetStrValue("aera_theme_accent");
  if (value.empty()) return kDefault;
  char *end = nullptr;
  const unsigned long parsed = strtoul(value.c_str(), &end, 16);
  if (end == value.c_str() || *end != '\0' || parsed == 0 ||
      parsed > 0x00ffffffUL) return kDefault;
  return static_cast<uint32_t>(parsed);
}
bool RecoverySetAccentColor(uint32_t rgb) {
  rgb &= 0x00ffffffu;
  if (rgb == 0) return false;
  char value[7];
  snprintf(value, sizeof(value), "%06x", rgb);
  // Mark the value persistent immediately; RecoverySavePreferences() controls
  // when the in-memory settings map is flushed to storage.
  return DataManager::SetValue("aera_theme_accent", value, 1) == 0;
}
AppearanceMode RecoveryAppearanceMode() {
  LoadAeraPreferencesIfAvailable();
  const std::string theme = DataManager::GetStrValue("aera_theme_mode");
  if (theme == "amoled" ||
      DataManager::GetIntValue("aera_surface_style") == 4)
    return AppearanceMode::kAmoled;
  return theme == "light" ? AppearanceMode::kLight
                           : AppearanceMode::kGraphite;
}
bool RecoverySetAppearanceMode(AppearanceMode mode) {
  const char *value = nullptr;
  switch (mode) {
    case AppearanceMode::kGraphite: value = "graphite"; break;
    case AppearanceMode::kLight: value = "light"; break;
    case AppearanceMode::kAmoled: value = "amoled"; break;
  }
  return value != nullptr &&
      DataManager::SetValue("aera_theme_mode", value, 1) == 0;
}
std::string RecoveryWallpaperPath() {
  LoadAeraPreferencesIfAvailable();
  const std::string path = DataManager::GetStrValue("aera_wallpaper_path");
  const bool valid = path.size() <= 4096 &&
      std::none_of(path.begin(), path.end(), [](unsigned char character) {
        return character < 32 || character == 127;
      });
  return valid ? path : std::string{};
}
LockTheme RecoveryLockTheme() {
  LoadAeraPreferencesIfAvailable();
  return DecodeLockTheme(DataManager::GetStrValue("aera_lock_theme"));
}
bool RecoverySetLockTheme(const LockTheme &theme) {
  return ValidLockTheme(theme) &&
      DataManager::SetValue("aera_lock_theme", EncodeLockTheme(theme), 1) == 0;
}
bool RecoverySetWallpaperPath(const std::string &path) {
  if (path.size() > 4096 ||
      std::any_of(path.begin(), path.end(), [](unsigned char character) {
        return character < 32 || character == 127;
      })) return false;
  return DataManager::SetValue("aera_wallpaper_path", path, 1) == 0;
}
bool RecoveryLightMode() {
  return RecoveryAppearanceMode() == AppearanceMode::kLight;
}
bool RecoverySetLightMode(bool enabled) {
  return RecoverySetAppearanceMode(enabled ? AppearanceMode::kLight
                                           : AppearanceMode::kGraphite);
}
SurfaceStyle RecoverySurfaceStyle() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_surface_style");
  if (stored.empty()) return SurfaceStyle::kSolid;
  const int value = atoi(stored.c_str());
  return static_cast<SurfaceStyle>(value >= 0 && value <= 3 ? value : 0);
}
bool RecoverySetSurfaceStyle(SurfaceStyle style) {
  const int value = static_cast<int>(style);
  return value >= 0 && value <= 3 &&
      DataManager::SetValue("aera_surface_style", value, 1) == 0;
}
bool RecoveryTintedIconBackgrounds() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("aera_tinted_icon_backgrounds") != 0;
}
bool RecoveryThemedHomeIcons() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("aera_themed_home_icons") != 0;
}
bool RecoverySetThemedHomeIcons(bool enabled) {
  return DataManager::SetValue("aera_themed_home_icons", enabled ? 1 : 0, 1) == 0;
}
bool RecoverySetTintedIconBackgrounds(bool enabled) {
  return DataManager::SetValue("aera_tinted_icon_backgrounds",
                               enabled ? 1 : 0, 1) == 0;
}
std::string RecoveryUiFont() {
  LoadAeraPreferencesIfAvailable();
  const std::string id = DataManager::GetStrValue("aera_ui_font");
  if (id.size() > 64 ||
      !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::islower(c) || std::isdigit(c) || c == '-' || c == '.';
      }))
    return {};
  return id;
}
bool RecoverySetUiFont(const std::string &plugin_id) {
  if (plugin_id.size() > 64 ||
      !std::all_of(plugin_id.begin(), plugin_id.end(), [](unsigned char c) {
        return std::islower(c) || std::isdigit(c) || c == '-' || c == '.';
      }))
    return false;
  return DataManager::SetValue("aera_ui_font", plugin_id, 1) == 0;
}
InterfaceSize RecoveryInterfaceSize() {
  LoadAeraPreferencesIfAvailable();
  const int value = std::clamp(
      DataManager::GetIntValue("aera_interface_size"), 0, 3);
  // DataManager returns zero for an unset integer. Preserve today's UI as the
  // default by storing human-readable names and treating empty as Normal.
  const std::string stored = DataManager::GetStrValue("aera_interface_size");
  return stored.empty() ? InterfaceSize::kNormal
                        : static_cast<InterfaceSize>(value);
}
bool RecoverySetInterfaceSize(InterfaceSize size) {
  const int value = static_cast<int>(size);
  return value >= 0 && value <= 3 &&
      DataManager::SetValue("aera_interface_size", value, 1) == 0;
}
KeyboardLayout RecoveryKeyboardLayout() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetStrValue("aera_keyboard_layout") == "qwertz"
      ? KeyboardLayout::kQwertz : KeyboardLayout::kQwerty;
}
bool RecoverySetKeyboardLayout(KeyboardLayout layout) {
  if (layout != KeyboardLayout::kQwerty &&
      layout != KeyboardLayout::kQwertz) return false;
  return DataManager::SetValue(
      "aera_keyboard_layout",
      layout == KeyboardLayout::kQwertz ? "qwertz" : "qwerty", 1) == 0;
}
int RecoveryHomeGridColumns() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_home_grid_columns");
  if (stored.empty()) return 3;
  const int columns = atoi(stored.c_str());
  return columns >= 2 && columns <= 5 ? columns : 3;
}
bool RecoverySetHomeGridColumns(int columns) {
  if (columns < 2 || columns > 5) return false;
  return DataManager::SetValue("aera_home_grid_columns", columns, 1) == 0;
}
DockLayout RecoveryDockLayout() {
  LoadAeraPreferencesIfAvailable();
  const int value = std::clamp(DataManager::GetIntValue("aera_dock_layout"), 0, kDockLayoutMax);
  return static_cast<DockLayout>(value);
}
bool RecoverySetDockLayout(DockLayout layout) {
  const int value = static_cast<int>(layout);
  return value >= 0 && value <= kDockLayoutMax &&
      DataManager::SetValue("aera_dock_layout", value, 1) == 0;
}
int RecoveryDockTransparency() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_dock_transparency");
  return stored.empty() ? 60 : std::clamp(atoi(stored.c_str()), 0, 100);
}
bool RecoverySetDockTransparency(int percent) {
  return DataManager::SetValue("aera_dock_transparency",
                               std::clamp(percent, 0, 100), 1) == 0;
}
int RecoveryDockBlur() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_dock_blur");
  return stored.empty() ? 24 : std::clamp(atoi(stored.c_str()), 0, 100);
}
bool RecoverySetDockBlur(int percent) {
  return DataManager::SetValue("aera_dock_blur",
                               std::clamp(percent, 0, 100), 1) == 0;
}
bool RecoveryDockHideInApps() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("aera_dock_hide_apps") != 0;
}
bool RecoverySetDockHideInApps(bool enabled) {
  return DataManager::SetValue("aera_dock_hide_apps", enabled ? 1 : 0, 1) == 0;
}
std::string RecoveryLanguage() {
  LoadAeraPreferencesIfAvailable();
  const std::string language = DataManager::GetStrValue("tw_language");
  return language.empty() ? AERA_DEFAULT_LANGUAGE : language;
}
bool RecoverySetLanguage(const std::string &language) {
  if (language.empty() || language.size() > 16) return false;
  for (const unsigned char character : language) {
    if (!(std::isalnum(character) || character == '_' || character == '-'))
      return false;
  }
  return DataManager::SetValue("tw_language", language, 1) == 0;
}
std::string RecoveryBrowserHomepage() {
  LoadAeraPreferencesIfAvailable();
  const std::string value = DataManager::GetStrValue("aera_browser_homepage");
  return value.empty() ? "aera://start" : value;
}
bool RecoverySetBrowserHomepage(const std::string &homepage) {
  if (homepage.empty() || homepage.size() > 2040 ||
      std::any_of(homepage.begin(), homepage.end(),
                  [](unsigned char c) { return c < 32 || c == 127; }))
    return false;
  return DataManager::SetValue("aera_browser_homepage", homepage, 1) == 0;
}
int RecoveryBrowserZoom() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_browser_zoom");
  return stored.empty() ? 100 : std::clamp(atoi(stored.c_str()), 50, 300);
}
bool RecoverySetBrowserZoom(int percent) {
  if (percent < 50 || percent > 300) return false;
  return DataManager::SetValue("aera_browser_zoom", percent, 1) == 0;
}
BrowserCookiePolicy RecoveryBrowserCookiePolicy() {
  LoadAeraPreferencesIfAvailable();
  const std::string stored = DataManager::GetStrValue("aera_browser_cookies");
  const int value = stored.empty() ? 1 : std::clamp(atoi(stored.c_str()), 0, 2);
  return static_cast<BrowserCookiePolicy>(value);
}
bool RecoverySetBrowserCookiePolicy(BrowserCookiePolicy policy) {
  const int value = static_cast<int>(policy);
  if (value < 0 || value > 2) return false;
  return DataManager::SetValue("aera_browser_cookies", value, 1) == 0;
}
bool RecoverySavePreferences() { return SaveAeraPreferences(); }
bool RecoveryAutoSavePreferences() { return SaveAeraPreferences(true); }
bool RecoveryPcAutoEnable() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("aera_pc_auto_enable") == 1;
}
bool RecoverySetPcAutoEnable(bool enabled) {
  const int previous = DataManager::GetIntValue("aera_pc_auto_enable");
  if (DataManager::SetValue("aera_pc_auto_enable", enabled ? 1 : 0, 1) != 0) return false;
  if (SaveAeraPreferences()) return true;
  DataManager::SetValue("aera_pc_auto_enable", previous, 1);
  return false;
}
bool RecoveryResetThemeSettings() { return ResetAeraSettings(true); }
bool RecoveryResetPreferences() { return ResetAeraSettings(false); }
int RecoveryDefaultHapticDuration(Haptic haptic) {
  return DefaultHapticDuration(haptic);
}

namespace {
const char *HapticVariable(Haptic haptic) {
  switch (haptic) {
    case Haptic::kTouch: return "tw_button_vibrate";
    case Haptic::kKeyboard: return "tw_keyboard_vibrate";
    case Haptic::kAction: return "tw_action_vibrate";
  }
  return nullptr;
}
}

bool RecoveryHapticsAvailable() {
  return DataManager::GetIntValue("tw_disable_haptics") == 0;
}

int RecoveryHapticDuration(Haptic haptic) {
  LoadAeraPreferencesIfAvailable();
  const char *variable = HapticVariable(haptic);
  return variable ? std::max(0, DataManager::GetIntValue(variable)) : 0;
}

bool RecoverySetHapticDuration(Haptic haptic, int milliseconds) {
  if (!RecoveryHapticsAvailable()) return false;
  const char *variable = HapticVariable(haptic);
  if (variable == nullptr) return false;
  const int maximum = haptic == Haptic::kAction ? 500 : 300;
  milliseconds = std::clamp(milliseconds, 0, maximum);
  return DataManager::SetValue(variable, milliseconds) == 0;
}

void RecoveryVibrate(Haptic haptic) {
  if (!RecoveryHapticsAvailable()) return;
  const char *variable = HapticVariable(haptic);
  if (variable != nullptr) DataManager::Vibrate(variable);
}

namespace {
constexpr const char *kAeraPreferencesPath =
    "/data/media/0/AERA/preferences.conf";
constexpr const char *kNasProfilePath =
    "/data/media/0/AERA/network_storage.conf";

#ifdef OF_ENABLE_WLAN
bool gEarlyWifiInitialized = false;
AeraSecrets::WlanSession gEarlyWifiDefaults;
#endif
bool gPrimaryWifiPreferencesLoaded = false;

#ifdef OF_ALLOW_EARLY_SETTINGS_LOAD
#ifdef OF_SETTINGS_ROOT_DIRECTORY
constexpr const char* kAeraEarlyPreferencesRoot = OF_SETTINGS_ROOT_DIRECTORY;
#else
constexpr const char* kAeraEarlyPreferencesRoot = "/data/media/0";
#endif

std::string EarlyPreferencesDirectory() {
  std::string root = kAeraEarlyPreferencesRoot;
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  return root + "/AERA";
}

const std::string kAeraEarlyPreferencesDirectory = EarlyPreferencesDirectory();
const std::string kAeraEarlyPreferencesPath =
    kAeraEarlyPreferencesDirectory + "/early-ui.conf";
const std::string kAeraEarlyWifiPath =
    kAeraEarlyPreferencesDirectory + "/early-wifi.json";

bool PrepareEarlyPreferencesRoot() {
  // Mount the backing recovery partition when the configured path belongs to
  // one. Custom roots on an already available filesystem need no special
  // handling and are created below like any other directory.
  if (PartitionManager.Find_Partition_By_Path(kAeraEarlyPreferencesRoot) == nullptr) return true;
  return PartitionManager.Mount_By_Path(kAeraEarlyPreferencesRoot, false);
}
#endif

struct EarlyUiPreferences {
  std::string lock_theme;
  int clock24;
  std::string timezone;
  int brightness;
  int screen_timeout = 60;
  std::string accent;
  std::string theme;
  int surface_style;
  int tinted_icon_backgrounds;
  int themed_home_icons;
  int interface_size;
  std::string language;
  std::string keyboard_layout;
  int home_grid_columns;
  int dock_layout;
  int dock_transparency;
  int dock_blur;
  int dock_hide_apps;
  int recents;
  int haptic_touch;
  int haptic_keyboard;
  int haptic_action;
  int preserve_recovery;
  int preserve_abl;
  int backup_checksums;
  int verify_backup_checksums;
  int wifi_show_speed;
  int wifi_show_name;
  int battery_style;
  int battery_circle_size;
  int battery_percentage;
  TemperatureDisplay temperature;
};

bool gEarlyPreferencesAttempted = false;
bool gEarlyDefaultsCaptured = false;
EarlyUiPreferences gEarlyDefaults;

bool ParseInteger(const std::string& value, int minimum, int maximum, int* parsed) {
  if (parsed == nullptr || value.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const long result = strtol(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0' || result < minimum || result > maximum)
    return false;
  *parsed = static_cast<int>(result);
  return true;
}

bool ValidPlainText(const std::string& value, size_t maximum_length, bool allow_empty = false) {
  if ((!allow_empty && value.empty()) || value.size() > maximum_length) return false;
  return std::none_of(value.begin(), value.end(),
                      [](unsigned char character) { return character < 32 || character == 127; });
}

bool ValidLanguage(const std::string& value) {
  if (value.empty() || value.size() > 16) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isalnum(character) || character == '_' || character == '-';
  });
}

bool ValidAccent(const std::string& value) {
  if (value.size() != 6 || value == "000000") return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char character) { return std::isxdigit(character); });
}

EarlyUiPreferences DefaultEarlyUiPreferences() {
  EarlyUiPreferences preferences{};
  preferences.clock24 = 0;
  preferences.timezone = DataManager::GetStrValue(TW_TIME_ZONE_VAR);
  if (!ValidPlainText(preferences.timezone, 96)) preferences.timezone = "UTC0";
  preferences.brightness = 100;
#ifdef TW_DEFAULT_BRIGHTNESS
  const int maximum = DataManager::GetIntValue("tw_brightness_max");
  if (maximum > 0)
    preferences.brightness = std::clamp(TW_DEFAULT_BRIGHTNESS * 100 / maximum, 10, 100);
#endif
  preferences.accent = "16c8ff";
  preferences.theme = "graphite";
  preferences.surface_style = 0;
  preferences.tinted_icon_backgrounds = 0;
  preferences.themed_home_icons = 0;
  preferences.interface_size = 1;
  preferences.language = AERA_DEFAULT_LANGUAGE;
  preferences.keyboard_layout = "qwerty";
  preferences.home_grid_columns = 3;
  preferences.dock_layout = 0;
  preferences.dock_transparency = 60;
  preferences.dock_blur = 24;
  preferences.dock_hide_apps = 0;
  preferences.recents = 1;
  preferences.lock_theme = EncodeLockTheme({});
#ifdef TW_NO_HAPTICS
  preferences.haptic_touch = 0;
  preferences.haptic_keyboard = 0;
  preferences.haptic_action = 0;
#else
  preferences.haptic_touch = 40;
  preferences.haptic_keyboard = 40;
  preferences.haptic_action = 160;
#endif
  preferences.preserve_recovery =
      DataManager::GetIntValue(AERA_RECOVERY_PRESERVATION_SUPPORTED) != 0 ? 1 : 0;
  preferences.preserve_abl = 0;
  preferences.backup_checksums = 1;
  preferences.verify_backup_checksums = 1;
  preferences.wifi_show_speed = 1;
  preferences.wifi_show_name = 1;
  preferences.battery_style = 0;
  preferences.battery_circle_size = kBatteryCircleSizeDefault;
  preferences.battery_percentage = 1;
  preferences.temperature = {};
  return preferences;
}

EarlyUiPreferences CaptureCurrentEarlyUiPreferences() {
  EarlyUiPreferences preferences{};
  preferences.clock24 = DataManager::GetIntValue("tw_military_time") != 0 ? 1 : 0;
  preferences.timezone = DataManager::GetStrValue(TW_TIME_ZONE_VAR);
  if (!ValidPlainText(preferences.timezone, 96)) preferences.timezone = "UTC0";
  preferences.brightness = DataManager::GetIntValue("tw_brightness_pct");
  const int timeout = DataManager::GetIntValue("tw_screen_timeout_secs");
  preferences.screen_timeout = timeout >= 30 && timeout <= 300 ? timeout : 60;
  if (preferences.brightness < 10 || preferences.brightness > 100) preferences.brightness = 100;
  preferences.accent = DataManager::GetStrValue("aera_theme_accent");
  if (!ValidAccent(preferences.accent)) preferences.accent = "16c8ff";
  preferences.theme = DataManager::GetStrValue("aera_theme_mode");
  if (preferences.theme != "light" && preferences.theme != "graphite" &&
      preferences.theme != "amoled")
    preferences.theme = "graphite";
  const std::string surface_style =
      DataManager::GetStrValue("aera_surface_style");
  preferences.surface_style = surface_style.empty()
      ? 0 : std::clamp(atoi(surface_style.c_str()), 0, 4);
  if (preferences.surface_style == 4) {
    preferences.theme = "amoled";
    preferences.surface_style = 0;
  }
  preferences.tinted_icon_backgrounds =
      DataManager::GetIntValue("aera_tinted_icon_backgrounds") != 0 ? 1 : 0;
  preferences.themed_home_icons =
      DataManager::GetIntValue("aera_themed_home_icons") != 0 ? 1 : 0;
  const std::string interface_size = DataManager::GetStrValue("aera_interface_size");
  preferences.interface_size =
      interface_size.empty() ? 1 : std::clamp(atoi(interface_size.c_str()), 0, 3);
  preferences.language = DataManager::GetStrValue("tw_language");
  if (!ValidLanguage(preferences.language)) preferences.language = AERA_DEFAULT_LANGUAGE;
  preferences.keyboard_layout =
      DataManager::GetStrValue("aera_keyboard_layout") == "qwertz" ? "qwertz" : "qwerty";
  const int home_grid_columns =
      DataManager::GetIntValue("aera_home_grid_columns");
  preferences.home_grid_columns =
      home_grid_columns >= 2 && home_grid_columns <= 5
          ? home_grid_columns : 3;
  preferences.dock_layout = std::clamp(DataManager::GetIntValue("aera_dock_layout"), 0, kDockLayoutMax);
  const std::string dock_transparency = DataManager::GetStrValue("aera_dock_transparency");
  preferences.dock_transparency =
      dock_transparency.empty() ? 60 : std::clamp(atoi(dock_transparency.c_str()), 0, 100);
  const std::string dock_blur = DataManager::GetStrValue("aera_dock_blur");
  preferences.dock_blur = dock_blur.empty() ? 24 : std::clamp(atoi(dock_blur.c_str()), 0, 100);
  preferences.dock_hide_apps = DataManager::GetIntValue("aera_dock_hide_apps") != 0 ? 1 : 0;
  preferences.recents = DataManager::GetIntValue("aera_recents_enabled") != 0 ? 1 : 0;
  preferences.lock_theme = EncodeLockTheme(DecodeLockTheme(DataManager::GetStrValue("aera_lock_theme")));
  preferences.haptic_touch = std::clamp(DataManager::GetIntValue("tw_button_vibrate"), 0, 300);
  preferences.haptic_keyboard = std::clamp(DataManager::GetIntValue("tw_keyboard_vibrate"), 0, 300);
  preferences.haptic_action = std::clamp(DataManager::GetIntValue("tw_action_vibrate"), 0, 500);
  preferences.preserve_recovery =
      DataManager::GetIntValue(AERA_PRESERVE_RECOVERY_VAR) != 0 ? 1 : 0;
  preferences.preserve_abl =
      DataManager::GetIntValue(AERA_PRESERVE_ABL_VAR) != 0 ? 1 : 0;
  preferences.backup_checksums = DataManager::GetIntValue(TW_SKIP_DIGEST_GENERATE_VAR) == 0 ? 1 : 0;
  preferences.verify_backup_checksums = DataManager::GetStrValue("aera_verify_backup_checksums") != "0" ? 1 : 0;
  preferences.wifi_show_speed = DataManager::GetStrValue("aera_wifi_show_speed") != "0" ? 1 : 0;
  preferences.wifi_show_name = DataManager::GetStrValue("aera_wifi_show_name") != "0" ? 1 : 0;
  preferences.battery_style = DataManager::GetIntValue("aera_battery_style") == 1 ? 1 : 0;
  preferences.battery_circle_size = kBatteryCircleSizeDefault;
  ParseInteger(DataManager::GetStrValue("aera_battery_circle_size"), kBatteryCircleSizeMin,
               kBatteryCircleSizeMax, &preferences.battery_circle_size);
  preferences.battery_percentage = DataManager::GetStrValue("aera_battery_percentage") != "0" ? 1 : 0;
  preferences.temperature.cpu = DataManager::GetIntValue("aera_temperature_cpu") == 1;
  preferences.temperature.battery = DataManager::GetIntValue("aera_temperature_battery") == 1;
  preferences.temperature.cpu_load = DataManager::GetIntValue("aera_status_cpu_load") == 1;
  preferences.temperature.ram = DataManager::GetIntValue("aera_status_ram") == 1;
  preferences.temperature.fahrenheit = DataManager::GetIntValue("aera_temperature_fahrenheit") == 1;
  preferences.temperature.decimals = DataManager::GetIntValue("aera_temperature_decimals") == 1;
  preferences.temperature.labels = DataManager::GetStrValue("aera_temperature_labels") != "0";
  preferences.temperature.colors = DataManager::GetStrValue("aera_temperature_colors") != "0";
  ParseInteger(DataManager::GetStrValue("aera_temperature_cpu_warm"), 30, 109, &preferences.temperature.cpu_warm);
  ParseInteger(DataManager::GetStrValue("aera_temperature_cpu_hot"), 31, 110, &preferences.temperature.cpu_hot);
  ParseInteger(DataManager::GetStrValue("aera_temperature_battery_warm"), 25, 59, &preferences.temperature.battery_warm);
  ParseInteger(DataManager::GetStrValue("aera_temperature_battery_hot"), 26, 60, &preferences.temperature.battery_hot);
  preferences.temperature.cpu_hot = std::max(preferences.temperature.cpu_hot, preferences.temperature.cpu_warm + 1);
  preferences.temperature.battery_hot = std::max(preferences.temperature.battery_hot, preferences.temperature.battery_warm + 1);
  ParseInteger(DataManager::GetStrValue("aera_temperature_size"), 20, 32, &preferences.temperature.size);
  ParseInteger(DataManager::GetStrValue("aera_temperature_position"), 0, 100, &preferences.temperature.position);
  return preferences;
}

void CaptureEarlyDefaultsIfNeeded() {
  if (gEarlyDefaultsCaptured) return;
  gEarlyDefaults = DefaultEarlyUiPreferences();
  gEarlyDefaultsCaptured = true;
}

int DefaultHapticDuration(Haptic haptic) {
  CaptureEarlyDefaultsIfNeeded();
  switch (haptic) {
    case Haptic::kTouch: return gEarlyDefaults.haptic_touch;
    case Haptic::kKeyboard: return gEarlyDefaults.haptic_keyboard;
    case Haptic::kAction: return gEarlyDefaults.haptic_action;
  }
  return 0;
}

bool ResetAeraSettings(bool theme) {
  LoadAeraPreferencesIfAvailable();
  const auto &defaults = gEarlyDefaults;
  bool restored = true;
  auto set = [&restored](const char *key, const auto &value) {
    restored &= DataManager::SetValue(key, value, 1) == 0;
  };
  // Reset only the selected settings group; keep language, network and plugins.
  if (theme) {
    set("aera_theme_accent", defaults.accent);
    set("aera_theme_mode", defaults.theme);
    set("aera_wallpaper_path", std::string());
    set("aera_surface_style", defaults.surface_style);
    set("aera_tinted_icon_backgrounds", defaults.tinted_icon_backgrounds);
    set("aera_themed_home_icons", defaults.themed_home_icons);
    set("aera_interface_size", defaults.interface_size);
    set("aera_keyboard_layout", defaults.keyboard_layout);
    set("aera_home_grid_columns", defaults.home_grid_columns);
    set("aera_dock_layout", defaults.dock_layout);
    set("aera_dock_transparency", defaults.dock_transparency);
    set("aera_dock_blur", defaults.dock_blur);
    set("aera_dock_hide_apps", defaults.dock_hide_apps);
    set("aera_ui_font", std::string());
    set("aera_lock_theme", defaults.lock_theme);
  } else {
    set("tw_military_time", defaults.clock24);
    set("aera_battery_style", defaults.battery_style);
    set("aera_battery_circle_size", defaults.battery_circle_size);
    set("aera_battery_percentage", defaults.battery_percentage);
    set("aera_temperature_cpu", defaults.temperature.cpu);
    set("aera_temperature_battery", defaults.temperature.battery);
    set("aera_status_cpu_load", defaults.temperature.cpu_load);
    set("aera_status_ram", defaults.temperature.ram);
    set("aera_temperature_fahrenheit", defaults.temperature.fahrenheit);
    set("aera_temperature_decimals", defaults.temperature.decimals);
    set("aera_temperature_labels", defaults.temperature.labels);
    set("aera_temperature_colors", defaults.temperature.colors);
    set("aera_temperature_cpu_warm", defaults.temperature.cpu_warm);
    set("aera_temperature_cpu_hot", defaults.temperature.cpu_hot);
    set("aera_temperature_battery_warm", defaults.temperature.battery_warm);
    set("aera_temperature_battery_hot", defaults.temperature.battery_hot);
    set("aera_temperature_size", defaults.temperature.size);
    set("aera_temperature_position", defaults.temperature.position);
    set(TW_TIME_ZONE_VAR, defaults.timezone);
    set("aera_recents_enabled", defaults.recents);
    set("tw_hidden_files", 0);
    set(TW_SIGNED_ZIP_VERIFY_VAR, 0);
    set(TW_USE_COMPRESSION_VAR, 0);
    set(TW_SKIP_DIGEST_GENERATE_VAR, 0);
    set("aera_verify_backup_checksums", 1);
    if (RecoverySha256Available()) set(TW_USE_SHA2, 1);
    if (RecoveryPreservationSupported())
      set(AERA_PRESERVE_RECOVERY_VAR, defaults.preserve_recovery);
    if (RecoveryAblPreservationSupported())
      set(AERA_PRESERVE_ABL_VAR, defaults.preserve_abl);
    set("tw_button_vibrate", defaults.haptic_touch);
    set("tw_keyboard_vibrate", defaults.haptic_keyboard);
    set("tw_action_vibrate", defaults.haptic_action);
    DataManager::update_tz_environment_variables();
    RecoverySetBrightness(defaults.brightness);
    RecoverySetScreenTimeout(defaults.screen_timeout);
  }
  return restored;
}

bool SetEarlyUiValue(EarlyUiPreferences* preferences, const std::string& key,
                     const std::string& value, bool* recognized = nullptr) {
  if (recognized != nullptr) *recognized = true;
  int parsed = 0;
  if (key == "clock24") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->clock24 = parsed;
  } else if (key == "timezone") {
    if (!ValidPlainText(value, 96)) return false;
    preferences->timezone = value;
  } else if (key == "screen_timeout") {
    if (!ParseInteger(value, 30, 300, &parsed)) return false;
    preferences->screen_timeout = parsed;
  } else if (key == "brightness") {
    if (!ParseInteger(value, 10, 100, &parsed)) return false;
    preferences->brightness = parsed;
  } else if (key == "accent") {
    if (!ValidAccent(value)) return false;
    preferences->accent = value;
  } else if (key == "theme") {
    if (value != "light" && value != "graphite" && value != "amoled")
      return false;
    preferences->theme = value;
  } else if (key == "surface_style") {
    if (!ParseInteger(value, 0, 4, &parsed)) return false;
    preferences->surface_style = parsed;
  } else if (key == "tinted_icon_backgrounds") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->tinted_icon_backgrounds = parsed;
  } else if (key == "themed_home_icons") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->themed_home_icons = parsed;
  } else if (key == "interface_size") {
    if (!ParseInteger(value, 0, 3, &parsed)) return false;
    preferences->interface_size = parsed;
  } else if (key == "language") {
    if (!ValidLanguage(value)) return false;
    preferences->language = value;
  } else if (key == "keyboard_layout") {
    if (value != "qwerty" && value != "qwertz") return false;
    preferences->keyboard_layout = value;
  } else if (key == "home_grid_columns") {
    if (!ParseInteger(value, 2, 5, &parsed)) return false;
    preferences->home_grid_columns = parsed;
  } else if (key == "dock_layout") {
    // IDs 5 and 6 were in a local preview build. Restore them as Text Only
    // without discarding the rest of a saved theme's preferences.
    if (!ParseInteger(value, 0, 6, &parsed)) return false;
    preferences->dock_layout = std::min(parsed, kDockLayoutMax);
  } else if (key == "dock_transparency") {
    if (!ParseInteger(value, 0, 100, &parsed)) return false;
    preferences->dock_transparency = parsed;
  } else if (key == "dock_blur") {
    if (!ParseInteger(value, 0, 100, &parsed)) return false;
    preferences->dock_blur = parsed;
  } else if (key == "dock_hide_apps") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->dock_hide_apps = parsed;
  } else if (key == "recents") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->recents = parsed;
  } else if (key == "lock_theme") {
    preferences->lock_theme = EncodeLockTheme(DecodeLockTheme(value));
  } else if (key == "haptic_touch") {
    if (!ParseInteger(value, 0, 300, &parsed)) return false;
    preferences->haptic_touch = parsed;
  } else if (key == "haptic_keyboard") {
    if (!ParseInteger(value, 0, 300, &parsed)) return false;
    preferences->haptic_keyboard = parsed;
  } else if (key == "haptic_action") {
    if (!ParseInteger(value, 0, 500, &parsed)) return false;
    preferences->haptic_action = parsed;
  } else if (key == "preserve_recovery") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->preserve_recovery = parsed;
  } else if (key == "preserve_abl") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->preserve_abl = parsed;
  } else if (key == "backup_checksums") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->backup_checksums = parsed;
  } else if (key == "verify_backup_checksums") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->verify_backup_checksums = parsed;
  } else if (key == "wifi_show_speed") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->wifi_show_speed = parsed;
  } else if (key == "wifi_show_name") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->wifi_show_name = parsed;
  } else if (key == "battery_style") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->battery_style = parsed;
  } else if (key == "battery_circle_size") {
    if (!ParseInteger(value, kBatteryCircleSizeMin, kBatteryCircleSizeMax, &parsed)) return false;
    preferences->battery_circle_size = parsed;
  } else if (key == "battery_percentage") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->battery_percentage = parsed;
  } else if (key == "temperature_cpu") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->temperature.cpu = parsed;
  } else if (key == "temperature_battery") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->temperature.battery = parsed;
  } else if (key == "status_cpu_load") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->temperature.cpu_load = parsed;
  } else if (key == "status_ram") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->temperature.ram = parsed;
  } else if (key == "temperature_fahrenheit") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->temperature.fahrenheit = parsed;
  } else if (key == "temperature_decimals") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->temperature.decimals = parsed;
  } else if (key == "temperature_labels") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->temperature.labels = parsed;
  } else if (key == "temperature_size") {
    if (!ParseInteger(value, 20, 32, &parsed)) return false;
    preferences->temperature.size = parsed;
  } else if (key == "temperature_colors") {
    if (!ParseInteger(value, 0, 1, &parsed)) return false;
    preferences->temperature.colors = parsed;
  } else if (key == "temperature_cpu_warm") {
    if (!ParseInteger(value, 30, 109, &parsed)) return false;
    preferences->temperature.cpu_warm = parsed;
  } else if (key == "temperature_cpu_hot") {
    if (!ParseInteger(value, 31, 110, &parsed)) return false;
    preferences->temperature.cpu_hot = parsed;
  } else if (key == "temperature_battery_warm") {
    if (!ParseInteger(value, 25, 59, &parsed)) return false;
    preferences->temperature.battery_warm = parsed;
  } else if (key == "temperature_battery_hot") {
    if (!ParseInteger(value, 26, 60, &parsed)) return false;
    preferences->temperature.battery_hot = parsed;
  } else if (key == "temperature_position") {
    if (!ParseInteger(value, 0, 100, &parsed)) return false;
    preferences->temperature.position = parsed;
  } else {
    if (recognized != nullptr) *recognized = false;
    return false;
  }
  return true;
}

void ApplyEarlyUiPreferences(const EarlyUiPreferences& preferences) {
  DataManager::SetValue("tw_military_time", preferences.clock24);
  DataManager::SetValue(TW_TIME_ZONE_VAR, preferences.timezone);
  DataManager::SetValue("tw_brightness_pct", preferences.brightness);
  RecoverySetScreenTimeout(preferences.screen_timeout);
  DataManager::SetValue("aera_theme_accent", preferences.accent);
  DataManager::SetValue("aera_theme_mode",
                        preferences.surface_style == 4 ? "amoled"
                                                       : preferences.theme);
  DataManager::SetValue("aera_surface_style",
                        preferences.surface_style == 4 ? 0
                                                       : preferences.surface_style);
  DataManager::SetValue("aera_tinted_icon_backgrounds",
                        preferences.tinted_icon_backgrounds);
  DataManager::SetValue("aera_themed_home_icons", preferences.themed_home_icons);
  DataManager::SetValue("aera_interface_size", preferences.interface_size);
  DataManager::SetValue("tw_language", preferences.language);
  DataManager::SetValue("aera_keyboard_layout", preferences.keyboard_layout);
  DataManager::SetValue("aera_home_grid_columns", preferences.home_grid_columns);
  DataManager::SetValue("aera_dock_layout", preferences.dock_layout);
  DataManager::SetValue("aera_dock_transparency", preferences.dock_transparency);
  DataManager::SetValue("aera_dock_blur", preferences.dock_blur);
  DataManager::SetValue("aera_dock_hide_apps", preferences.dock_hide_apps);
  DataManager::SetValue("aera_recents_enabled", preferences.recents);
  DataManager::SetValue("aera_lock_theme", preferences.lock_theme, 1);
  DataManager::SetValue("tw_button_vibrate", preferences.haptic_touch);
  DataManager::SetValue("tw_keyboard_vibrate", preferences.haptic_keyboard);
  DataManager::SetValue("tw_action_vibrate", preferences.haptic_action);
  DataManager::SetValue(AERA_PRESERVE_RECOVERY_VAR,
                        preferences.preserve_recovery);
  DataManager::SetValue(AERA_PRESERVE_ABL_VAR, preferences.preserve_abl);
  DataManager::SetValue(TW_SKIP_DIGEST_GENERATE_VAR, preferences.backup_checksums ? 0 : 1);
  DataManager::SetValue("aera_verify_backup_checksums", preferences.verify_backup_checksums);
  DataManager::SetValue(TW_SKIP_DIGEST_CHECK_VAR, preferences.verify_backup_checksums);
  DataManager::SetValue("aera_wifi_show_speed", preferences.wifi_show_speed);
  DataManager::SetValue("aera_wifi_show_name", preferences.wifi_show_name);
  DataManager::SetValue("aera_battery_style", preferences.battery_style);
  DataManager::SetValue("aera_battery_circle_size", preferences.battery_circle_size);
  DataManager::SetValue("aera_battery_percentage", preferences.battery_percentage);
  DataManager::SetValue("aera_temperature_cpu", preferences.temperature.cpu ? 1 : 0);
  DataManager::SetValue("aera_temperature_battery", preferences.temperature.battery ? 1 : 0);
  DataManager::SetValue("aera_status_cpu_load", preferences.temperature.cpu_load ? 1 : 0);
  DataManager::SetValue("aera_status_ram", preferences.temperature.ram ? 1 : 0);
  DataManager::SetValue("aera_temperature_fahrenheit", preferences.temperature.fahrenheit ? 1 : 0);
  DataManager::SetValue("aera_temperature_decimals", preferences.temperature.decimals ? 1 : 0);
  DataManager::SetValue("aera_temperature_labels", preferences.temperature.labels ? 1 : 0);
  DataManager::SetValue("aera_temperature_colors", preferences.temperature.colors ? 1 : 0);
  DataManager::SetValue("aera_temperature_cpu_warm", preferences.temperature.cpu_warm);
  DataManager::SetValue("aera_temperature_cpu_hot", preferences.temperature.cpu_hot);
  DataManager::SetValue("aera_temperature_battery_warm", preferences.temperature.battery_warm);
  DataManager::SetValue("aera_temperature_battery_hot", preferences.temperature.battery_hot);
  DataManager::SetValue("aera_temperature_size", preferences.temperature.size);
  DataManager::SetValue("aera_temperature_position", preferences.temperature.position);
}

void ApplyEarlyUiSideEffects() {
  DataManager::update_tz_environment_variables();
  const int percent = std::clamp(DataManager::GetIntValue("tw_brightness_pct"), 10, 100);
  const int maximum = DataManager::GetIntValue("tw_brightness_max");
  if (maximum > 0) {
    DataManager::SetValue("tw_brightness", maximum * percent / 100);
    TWFunc::Set_Brightness(DataManager::GetStrValue("tw_brightness"));
  }
}

bool WriteFileAtomically(const std::string& path, const std::string& contents) {
  const std::string temporary = path + ".tmp";
  const int fd =
      open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  if (fchmod(fd, 0600) != 0) {
    close(fd);
    unlink(temporary.c_str());
    return false;
  }

  size_t offset = 0;
  bool ok = true;
  while (offset < contents.size()) {
    const ssize_t written = write(fd, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<size_t>(written);
  }
  if (ok && fsync(fd) != 0) ok = false;
  if (close(fd) != 0) ok = false;
  if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  chmod(path.c_str(), 0600);
  aera::shared_storage::Normalize(path);
  return true;
}

std::string SerializeEarlyUiPreferences(const EarlyUiPreferences& preferences) {
  std::ostringstream output;
  output << "schema=1\n"
         << "clock24=" << preferences.clock24 << '\n'
         << "timezone=" << preferences.timezone << '\n'
         << "brightness=" << preferences.brightness << '\n'
         << "screen_timeout=" << preferences.screen_timeout << '\n'
         << "accent=" << preferences.accent << '\n'
         << "theme=" << preferences.theme << '\n'
         << "surface_style=" << preferences.surface_style << '\n'
         << "tinted_icon_backgrounds="
         << preferences.tinted_icon_backgrounds << '\n'
         << "themed_home_icons=" << preferences.themed_home_icons << '\n'
         << "interface_size=" << preferences.interface_size << '\n'
         << "language=" << preferences.language << '\n'
         << "keyboard_layout=" << preferences.keyboard_layout << '\n'
         << "home_grid_columns=" << preferences.home_grid_columns << '\n'
         << "dock_layout=" << preferences.dock_layout << '\n'
         << "dock_transparency=" << preferences.dock_transparency << '\n'
         << "dock_blur=" << preferences.dock_blur << '\n'
         << "dock_hide_apps=" << preferences.dock_hide_apps << '\n'
         << "recents=" << preferences.recents << '\n'
         << "lock_theme=" << preferences.lock_theme << '\n'
         << "haptic_touch=" << preferences.haptic_touch << '\n'
         << "haptic_keyboard=" << preferences.haptic_keyboard << '\n'
         << "haptic_action=" << preferences.haptic_action << '\n'
         << "preserve_recovery=" << preferences.preserve_recovery << '\n'
         << "preserve_abl=" << preferences.preserve_abl << '\n'
         << "backup_checksums=" << preferences.backup_checksums << '\n'
         << "verify_backup_checksums=" << preferences.verify_backup_checksums << '\n'
         << "wifi_show_speed=" << preferences.wifi_show_speed << '\n'
         << "wifi_show_name=" << preferences.wifi_show_name << '\n'
         << "battery_style=" << preferences.battery_style << '\n'
         << "battery_circle_size=" << preferences.battery_circle_size << '\n'
         << "battery_percentage=" << preferences.battery_percentage << '\n'
         << "temperature_cpu=" << preferences.temperature.cpu << '\n'
         << "temperature_battery=" << preferences.temperature.battery << '\n'
         << "status_cpu_load=" << preferences.temperature.cpu_load << '\n'
         << "status_ram=" << preferences.temperature.ram << '\n'
         << "temperature_fahrenheit=" << preferences.temperature.fahrenheit << '\n'
         << "temperature_decimals=" << preferences.temperature.decimals << '\n'
         << "temperature_labels=" << preferences.temperature.labels << '\n'
         << "temperature_colors=" << preferences.temperature.colors << '\n'
         << "temperature_cpu_warm=" << preferences.temperature.cpu_warm << '\n'
         << "temperature_cpu_hot=" << preferences.temperature.cpu_hot << '\n'
         << "temperature_battery_warm=" << preferences.temperature.battery_warm << '\n'
         << "temperature_battery_hot=" << preferences.temperature.battery_hot << '\n'
         << "temperature_size=" << preferences.temperature.size << '\n'
         << "temperature_position=" << preferences.temperature.position << '\n';
  return output.str();
}

bool SaveEarlyUiPreferences() {
#ifndef OF_ALLOW_EARLY_SETTINGS_LOAD
  return true;
#else
  if (!PrepareEarlyPreferencesRoot() ||
      !TWFunc::Recursive_Mkdir(kAeraEarlyPreferencesDirectory, false))
    return false;
  return WriteFileAtomically(kAeraEarlyPreferencesPath,
                             SerializeEarlyUiPreferences(CaptureCurrentEarlyUiPreferences()));
#endif
}

bool SaveEarlyWifiPreferences() {
#if defined(OF_ALLOW_EARLY_SETTINGS_LOAD) && defined(OF_ENABLE_WLAN)
  if (!PrepareEarlyPreferencesRoot() ||
      !TWFunc::Recursive_Mkdir(kAeraEarlyPreferencesDirectory, false)) return false;
  AeraSecrets::WlanSession session;
  session.auto_enable = DataManager::GetIntValue("of_wlan_auto_enable") == 1;
  session.auto_connect = DataManager::GetIntValue("of_wlan_auto_connect") == 1;
  session.last_ssid = DataManager::GetStrValue("of_wlan_last_ssid");
  return AeraSecrets::SaveEarlyWlan(kAeraEarlyWifiPath, session);
#else
  return true;
#endif
}

void LoadEarlyUiPreferencesIfAvailable() {
#ifndef OF_ALLOW_EARLY_SETTINGS_LOAD
  return;
#else
  if (gEarlyPreferencesAttempted) return;
  CaptureEarlyDefaultsIfNeeded();
  std::ifstream input(kAeraEarlyPreferencesPath);
  if (!input) {
    if (!PrepareEarlyPreferencesRoot()) return;
    input.clear();
    input.open(kAeraEarlyPreferencesPath);
  }
  gEarlyPreferencesAttempted = true;
  if (!input) return;

  EarlyUiPreferences preferences = gEarlyDefaults;
  bool valid_schema = false;
  bool valid_contents = true;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      valid_contents = false;
      continue;
    }
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (key == "schema") {
      valid_schema = value == "1";
      continue;
    }
    bool recognized = false;
    if (!SetEarlyUiValue(&preferences, key, value, &recognized) && recognized)
      valid_contents = false;
  }
  if (!valid_schema || !valid_contents) {
    LOGINFO("AERA: ignored an invalid early UI preference cache.\n");
    return;
  }

  ApplyEarlyUiPreferences(preferences);
  ApplyEarlyUiSideEffects();
  LOGINFO("AERA: restored early UI preferences from %s.\n",
          kAeraEarlyPreferencesPath.c_str());
#endif
}

void LoadAeraPreferencesIfAvailable() {
  // Fastboot over Wi-Fi grants destructive partition access and is therefore
  // intentionally session-only. Remember paired computers, but require an
  // explicit opt-in after every recovery process start.
  static bool fastboot_session_initialized = false;
  if (!fastboot_session_initialized) {
    DataManager::SetValue(kFastbootWifiPreference, 0);
    fastboot_session_initialized = true;
  }
  CaptureEarlyDefaultsIfNeeded();
  LoadEarlyUiPreferencesIfAvailable();
  static bool loaded = false;
  static std::atomic<bool> shared_repair_started{false};
  struct stat shared_info{};
  if (!shared_repair_started &&
      lstat("/data/media/0/AERA", &shared_info) == 0 && S_ISDIR(shared_info.st_mode) &&
      !shared_repair_started.exchange(true)) {
    std::thread([] { aera::shared_storage::NormalizeAll(); }).detach();
  }
  if (loaded) return;
  std::ifstream input(kAeraPreferencesPath);
  // Before decryption /data/media/0 is intentionally unavailable. Leave the
  // flag clear so the first post-decryption scene retries automatically.
  if (!input) return;

  EarlyUiPreferences full_ui = gEarlyDefaults;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    bool early_key = false;
    SetEarlyUiValue(&full_ui, key, value, &early_key);
    if (early_key) continue;
    if (key == "hidden")
      DataManager::SetValue("tw_hidden_files", value);
    else if (key == "compression") DataManager::SetValue(TW_USE_COMPRESSION_VAR, value);
    else if (key == "sha256") DataManager::SetValue(TW_USE_SHA2, value);
    else if (key == "verify_zip") DataManager::SetValue(TW_SIGNED_ZIP_VERIFY_VAR, value);
    else if (key == "preserve_recovery")
      DataManager::SetValue(AERA_PRESERVE_RECOVERY_VAR, value);
    else if (key == "preserve_abl")
      DataManager::SetValue(AERA_PRESERVE_ABL_VAR, value);
    else if (key == "plugin_auto_update") DataManager::SetValue("aera_plugin_auto_update", value);
    else if (key == "pc_auto_enable") DataManager::SetValue("aera_pc_auto_enable", value);
    else if (key == "update_nightly")
      DataManager::SetValue("aera_update_nightly", value);
    else if (key == "appvault_storage" && ValidPlainText(value, 1024, true))
      DataManager::SetValue("aera_appvault_storage", value);
    else if (key == "browser_homepage") DataManager::SetValue("aera_browser_homepage", value);
    else if (key == "browser_zoom") DataManager::SetValue("aera_browser_zoom", value);
    else if (key == "browser_cookies")
      DataManager::SetValue("aera_browser_cookies", value);
    else if (key == "ui_font") DataManager::SetValue("aera_ui_font", value);
    else if (key == "wallpaper" && ValidPlainText(value, 4096, true))
      DataManager::SetValue("aera_wallpaper_path", value);
    else if (key == "wifi_auto_enable") DataManager::SetValue("of_wlan_auto_enable", value);
    else if (key == "wifi_auto_connect") DataManager::SetValue("of_wlan_auto_connect", value);
    else if (key == "wifi_last_ssid") DataManager::SetValue("of_wlan_last_ssid", value);
  }
  ApplyEarlyUiPreferences(full_ui);
  loaded = true;
  gPrimaryWifiPreferencesLoaded = true;
  if (AeraSecrets::UsePrimaryWlan(DataManager::GetStrValue("wlan_connected_name")))
    DataManager::SetValue("aera_wlan_reconnect", 1);
  ApplyEarlyUiSideEffects();
  if (!SaveEarlyUiPreferences()) LOGERR("AERA: could not refresh the early UI preference cache.\n");
  if (!SaveEarlyWifiPreferences()) LOGERR("AERA: could not refresh the early WLAN cache.\n");
  LOGINFO("AERA: restored preferences from shared storage.\n");
}

bool SaveAeraPreferences(bool require_early) {
  constexpr const char *directory = "/data/media/0/AERA";
  if (!TWFunc::Recursive_Mkdir(directory, false)) return false;
  std::ostringstream output;
  output << "clock24=" << DataManager::GetIntValue("tw_military_time") << '\n'
         << "hidden=" << DataManager::GetIntValue("tw_hidden_files") << '\n'
         << "compression=" << DataManager::GetIntValue(TW_USE_COMPRESSION_VAR) << '\n'
         << "sha256=" << DataManager::GetIntValue(TW_USE_SHA2) << '\n'
         << "backup_checksums=" << (DataManager::GetIntValue(TW_SKIP_DIGEST_GENERATE_VAR) == 0 ? 1 : 0) << '\n'
         << "verify_backup_checksums=" << (DataManager::GetStrValue("aera_verify_backup_checksums") != "0" ? 1 : 0) << '\n'
         << "verify_zip=" << DataManager::GetIntValue(TW_SIGNED_ZIP_VERIFY_VAR) << '\n'
         << "preserve_recovery=" << DataManager::GetIntValue(AERA_PRESERVE_RECOVERY_VAR) << '\n'
         << "preserve_abl=" << DataManager::GetIntValue(AERA_PRESERVE_ABL_VAR) << '\n'
         << "plugin_auto_update=" << DataManager::GetIntValue("aera_plugin_auto_update") << '\n'
         << "pc_auto_enable=" << DataManager::GetIntValue("aera_pc_auto_enable") << '\n'
         << "update_nightly=" << DataManager::GetIntValue("aera_update_nightly") << '\n'
         << "appvault_storage=" << DataManager::GetStrValue("aera_appvault_storage") << '\n'
         << "recents=" << DataManager::GetIntValue("aera_recents_enabled") << '\n'
         << "lock_theme=" << EncodeLockTheme(RecoveryLockTheme()) << '\n'
         << "timezone=" << DataManager::GetStrValue(TW_TIME_ZONE_VAR) << '\n'
         << "brightness=" << DataManager::GetIntValue("tw_brightness_pct") << '\n'
         << "screen_timeout=" << CaptureCurrentEarlyUiPreferences().screen_timeout << '\n'
         << "accent=" << DataManager::GetStrValue("aera_theme_accent") << '\n'
         << "theme="
         << (RecoveryAppearanceMode() == AppearanceMode::kAmoled ? "amoled"
             : RecoveryAppearanceMode() == AppearanceMode::kLight ? "light"
                                                                  : "graphite")
         << '\n'
         << "surface_style=" << static_cast<int>(RecoverySurfaceStyle()) << '\n'
         << "tinted_icon_backgrounds="
         << (RecoveryTintedIconBackgrounds() ? 1 : 0) << '\n'
         << "themed_home_icons=" << (RecoveryThemedHomeIcons() ? 1 : 0) << '\n'
         << "interface_size=" << static_cast<int>(RecoveryInterfaceSize()) << '\n'
         << "keyboard_layout="
         << (RecoveryKeyboardLayout() == KeyboardLayout::kQwertz
                 ? "qwertz" : "qwerty") << '\n'
         << "home_grid_columns=" << RecoveryHomeGridColumns() << '\n'
         << "dock_layout=" << DataManager::GetIntValue("aera_dock_layout") << '\n'
         << "dock_transparency=" << RecoveryDockTransparency() << '\n'
         << "dock_blur=" << RecoveryDockBlur() << '\n'
         << "dock_hide_apps=" << (RecoveryDockHideInApps() ? 1 : 0) << '\n'
         << "language=" << RecoveryLanguage() << '\n'
         << "ui_font=" << RecoveryUiFont() << '\n'
         << "wallpaper=" << RecoveryWallpaperPath() << '\n'
         << "browser_homepage=" << RecoveryBrowserHomepage() << '\n'
         << "browser_zoom=" << RecoveryBrowserZoom() << '\n'
         << "browser_cookies="
         << static_cast<int>(RecoveryBrowserCookiePolicy()) << '\n'
         << "haptic_touch=" << DataManager::GetIntValue("tw_button_vibrate") << '\n'
         << "haptic_keyboard=" << DataManager::GetIntValue("tw_keyboard_vibrate") << '\n'
         << "haptic_action=" << DataManager::GetIntValue("tw_action_vibrate") << '\n'
         << "wifi_auto_enable=" << DataManager::GetIntValue("of_wlan_auto_enable") << '\n'
         << "wifi_auto_connect=" << DataManager::GetIntValue("of_wlan_auto_connect") << '\n'
         << "wifi_last_ssid=" << DataManager::GetStrValue("of_wlan_last_ssid") << '\n'
         << "wifi_show_speed=" << (DataManager::GetStrValue("aera_wifi_show_speed") != "0" ? 1 : 0) << '\n'
         << "wifi_show_name=" << (DataManager::GetStrValue("aera_wifi_show_name") != "0" ? 1 : 0) << '\n'
         << "battery_style=" << static_cast<int>(RecoveryBatteryStyle()) << '\n'
         << "battery_circle_size=" << RecoveryBatteryCircleSize() << '\n'
         << "battery_percentage=" << (RecoveryBatteryPercentage() ? 1 : 0) << '\n';
  const auto temperature = RecoveryTemperatureDisplay();
  output << "temperature_cpu=" << temperature.cpu << '\n'
         << "temperature_battery=" << temperature.battery << '\n'
         << "status_cpu_load=" << temperature.cpu_load << '\n'
         << "status_ram=" << temperature.ram << '\n'
         << "temperature_fahrenheit=" << temperature.fahrenheit << '\n'
         << "temperature_decimals=" << temperature.decimals << '\n'
         << "temperature_labels=" << temperature.labels << '\n'
         << "temperature_colors=" << temperature.colors << '\n'
         << "temperature_cpu_warm=" << temperature.cpu_warm << '\n'
         << "temperature_cpu_hot=" << temperature.cpu_hot << '\n'
         << "temperature_battery_warm=" << temperature.battery_warm << '\n'
         << "temperature_battery_hot=" << temperature.battery_hot << '\n'
         << "temperature_size=" << temperature.size << '\n'
         << "temperature_position=" << temperature.position << '\n';
  if (!WriteFileAtomically(kAeraPreferencesPath, output.str())) return false;
  const bool early_saved = SaveEarlyUiPreferences();
  if (!SaveEarlyWifiPreferences())
    LOGERR("AERA: saved full preferences but could not update the early WLAN cache.\n");
  if (!early_saved)
    LOGERR("AERA: saved full preferences but could not update the early UI cache.\n");
  LOGINFO("AERA: saved preferences to shared storage.\n");
  return !require_early || early_saved;
}

bool LoadNasProfile(NasConfig *config, const std::string &protocol = "") {
  if (config == nullptr) return false;
  if (!protocol.empty() && protocol != "smb" && protocol != "sftp") return false;
  const auto path = protocol.empty() ? std::string(kNasProfilePath)
      : std::string(kNasProfilePath) + "." + protocol;
  std::ifstream input(path);
  if (!input && !protocol.empty()) {
    NasConfig legacy;
    if (!LoadNasProfile(&legacy) || legacy.type != protocol) return false;
    *config = std::move(legacy);
    return true;
  }
  if (!input) return false;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "type") config->type = value;
    else if (key == "host") config->host = value;
    else if (key == "port") config->port = value;
    else if (key == "share") config->share = value;
    else if (key == "path") config->path = value;
    else if (key == "user") config->user = value;
    else if (key == "domain") config->domain = value;
    else if (key == "cache") config->cache_mode = value;
    else if (key == "auth") config->auth = value;
    else if (key == "key_name") config->key_name = value;
  }
  if (config->type != "smb" && config->type != "sftp") return false;
  if (!protocol.empty() && config->type != protocol) return false;
  if (protocol.empty()) {
    // The legacy password belongs only to the profile recorded alongside it.
    if (!AeraSecrets::MigrateNasPassword(config->type)) return false;
  }
  return true;
}

bool SaveNasProfile(const NasConfig &config) {
  constexpr const char *directory = "/data/media/0/AERA";
  // OrangeFox's Recursive_Mkdir returns 1 on success and 0 on failure.
  if (!TWFunc::Recursive_Mkdir(directory, false)) return false;
  NasConfig previous;
  if (LoadNasProfile(&previous) && previous.type != config.type) {
    const auto previous_path = std::string(kNasProfilePath) + "." + previous.type;
    if (access(previous_path.c_str(), F_OK) != 0) {
      std::ifstream legacy(kNasProfilePath);
      std::ostringstream contents;
      contents << legacy.rdbuf();
      if (!legacy.good() || !WriteFileAtomically(previous_path, contents.str()))
        return false;
    }
  }
  const std::string profile = std::string(kNasProfilePath) + "." + config.type;
  const std::string temporary = profile + ".tmp";
  std::ofstream output(temporary, std::ios::out | std::ios::trunc);
  if (!output) return false;
  output << "type=" << config.type << '\n'
         << "host=" << config.host << '\n'
         << "port=" << config.port << '\n'
         << "share=" << config.share << '\n'
         << "path=" << config.path << '\n'
         << "user=" << config.user << '\n'
         << "domain=" << config.domain << '\n'
         << "cache=" << config.cache_mode << '\n'
         << "auth=" << config.auth << '\n'
         << "key_name=" << config.key_name << '\n';
  output.flush();
  if (!output) return false;
  output.close();
  if (rename(temporary.c_str(), profile.c_str()) != 0) return false;
  chmod(profile.c_str(), 0600);
  aera::shared_storage::Normalize(profile);
  std::ifstream saved(profile);
  std::ostringstream content;
  content << saved.rdbuf();
  return saved.good() && WriteFileAtomically(kNasProfilePath, content.str());
}

std::string ReadText(const std::string &path) {
  std::ifstream input(path);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}
std::vector<std::string> Lines(const std::string &text) {
  std::vector<std::string> result;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) result.push_back(line);
  }
  return result;
}
}  // namespace

void RecoveryWifiInitialize() {
#ifdef OF_ENABLE_WLAN
  LoadAeraPreferencesIfAvailable();
  AeraSecrets::UsePrimaryWlan();
  if (gEarlyWifiInitialized && !gPrimaryWifiPreferencesLoaded) {
    DataManager::SetValue("of_wlan_auto_enable", gEarlyWifiDefaults.auto_enable ? 1 : 0);
    DataManager::SetValue("of_wlan_auto_connect", gEarlyWifiDefaults.auto_connect ? 1 : 0);
    DataManager::SetValue("of_wlan_last_ssid", gEarlyWifiDefaults.last_ssid);
  }
  const bool early_initialized = gEarlyWifiInitialized;
  gEarlyWifiInitialized = false;
  if (early_initialized && DataManager::GetIntValue("of_wlan_auto_enable") != 1) {
    AeraWifiDispatcher::Submit(AeraWifiDispatcher::Request::StopRadio);
    return;
  }
  AeraWifiDispatcher::Submit(AeraWifiDispatcher::Request::RestoreSession);
#endif
}

void RecoveryWifiInitializeEarly() {
#if defined(OF_ALLOW_EARLY_SETTINGS_LOAD) && defined(OF_ENABLE_WLAN)
  if (gEarlyWifiInitialized || !RecoveryDataLocked() ||
      !PrepareEarlyPreferencesRoot()) return;
  AeraSecrets::WlanSession session;
  if (!AeraSecrets::LoadEarlyWlan(kAeraEarlyWifiPath, session)) return;
  gEarlyWifiDefaults.auto_enable = DataManager::GetIntValue("of_wlan_auto_enable") == 1;
  gEarlyWifiDefaults.auto_connect = DataManager::GetIntValue("of_wlan_auto_connect") == 1;
  gEarlyWifiDefaults.last_ssid = DataManager::GetStrValue("of_wlan_last_ssid");
  DataManager::SetValue("of_wlan_auto_enable", session.auto_enable ? 1 : 0);
  DataManager::SetValue("of_wlan_auto_connect", session.auto_connect ? 1 : 0);
  DataManager::SetValue("of_wlan_last_ssid", session.last_ssid);
  gEarlyWifiInitialized = true;
  if (session.auto_enable)
    AeraWifiDispatcher::Submit(AeraWifiDispatcher::Request::RestoreEarlySession);
#endif
}

WifiConnection RecoveryWifiConnection() {
  WifiConnection connection;
#ifdef OF_ENABLE_WLAN
  connection.connected = DataManager::GetIntValue("tw_wlan_connected") == 1;
  connection.ssid = DataManager::GetStrValue("wlan_connected_name");
  ParseInteger(DataManager::GetStrValue("aera_wlan_rssi"), -127, -1, &connection.signal_dbm);
#endif
  return connection;
}

WifiTraffic RecoveryWifiTraffic() {
  WifiTraffic traffic;
#ifdef OF_ENABLE_WLAN
  if (DataManager::GetIntValue("tw_wlan_connected") != 1) return traffic;
  traffic.interface = DataManager::GetStrValue("tw_wlan_iface");
  if (traffic.interface.empty()) traffic.interface = "wlan0";
  if (traffic.interface.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos ||
      traffic.interface == "." || traffic.interface == "..") return traffic;
  const std::string directory = "/sys/class/net/" + traffic.interface;
  std::ifstream received(directory + "/statistics/rx_bytes");
  std::ifstream sent(directory + "/statistics/tx_bytes");
  std::ifstream index(directory + "/ifindex");
  traffic.valid = bool(received >> traffic.received_bytes) &&
      bool(sent >> traffic.sent_bytes) && bool(index >> traffic.interface_index);
#endif
  return traffic;
}

WifiStatusDisplay RecoveryWifiStatusDisplay() {
  LoadAeraPreferencesIfAvailable();
  WifiStatusDisplay display;
  display.show_speed = DataManager::GetStrValue("aera_wifi_show_speed") != "0";
  display.show_name = DataManager::GetStrValue("aera_wifi_show_name") != "0";
  return display;
}

bool RecoverySetWifiStatusDisplay(const WifiStatusDisplay &display, bool save) {
  const auto previous = RecoveryWifiStatusDisplay();
  auto apply = [](const WifiStatusDisplay &value) {
    return DataManager::SetValue("aera_wifi_show_speed", value.show_speed ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_wifi_show_name", value.show_name ? 1 : 0) == 0;
  };
  if (apply(display) && (!save || SaveAeraPreferences())) return true;
  apply(previous);
  return false;
}

TemperatureDisplay RecoveryTemperatureDisplay() {
  LoadAeraPreferencesIfAvailable();
  return CaptureCurrentEarlyUiPreferences().temperature;
}

bool RecoverySetTemperatureDisplay(const TemperatureDisplay &display, bool save) {
  const auto previous = RecoveryTemperatureDisplay();
  auto apply = [](TemperatureDisplay value) {
    value.size = std::clamp(value.size, 20, 32);
    value.position = std::clamp(value.position, 0, 100);
    value.cpu_warm = std::clamp(value.cpu_warm, 30, 109);
    value.cpu_hot = std::clamp(value.cpu_hot, value.cpu_warm + 1, 110);
    value.battery_warm = std::clamp(value.battery_warm, 25, 59);
    value.battery_hot = std::clamp(value.battery_hot, value.battery_warm + 1, 60);
    return DataManager::SetValue("aera_temperature_cpu", value.cpu ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_temperature_battery", value.battery ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_status_cpu_load", value.cpu_load ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_status_ram", value.ram ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_temperature_fahrenheit", value.fahrenheit ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_temperature_decimals", value.decimals ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_temperature_labels", value.labels ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_temperature_colors", value.colors ? 1 : 0) == 0 &&
        DataManager::SetValue("aera_temperature_cpu_warm", value.cpu_warm) == 0 &&
        DataManager::SetValue("aera_temperature_cpu_hot", value.cpu_hot) == 0 &&
        DataManager::SetValue("aera_temperature_battery_warm", value.battery_warm) == 0 &&
        DataManager::SetValue("aera_temperature_battery_hot", value.battery_hot) == 0 &&
        DataManager::SetValue("aera_temperature_size", value.size) == 0 &&
        DataManager::SetValue("aera_temperature_position", value.position) == 0;
  };
  if (apply(display) && (!save || SaveAeraPreferences())) return true;
  apply(previous);
  return false;
}

StatusReadings RecoveryStatusReadings() {
  static TemperatureSampler sampler;
  const auto display = RecoveryTemperatureDisplay();
  return sampler.Snapshot(display.cpu || display.battery || display.cpu_load || display.ram,
                          display.cpu || display.battery);
}
Temperatures RecoveryTemperatures() { return RecoveryStatusReadings().temperature; }
SystemMetrics RecoverySystemMetrics() { return RecoveryStatusReadings().system; }

BatteryStyle RecoveryBatteryStyle() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("aera_battery_style") == 1 ? BatteryStyle::kCircle : BatteryStyle::kClassic;
}

bool RecoverySetBatteryStyle(BatteryStyle style) {
  const auto previous = RecoveryBatteryStyle();
  if (style != BatteryStyle::kClassic && style != BatteryStyle::kCircle) return false;
  if (DataManager::SetValue("aera_battery_style", static_cast<int>(style)) == 0 && SaveAeraPreferences()) return true;
  DataManager::SetValue("aera_battery_style", static_cast<int>(previous));
  return false;
}

int RecoveryBatteryCircleSize() {
  LoadAeraPreferencesIfAvailable();
  int size = kBatteryCircleSizeDefault;
  ParseInteger(DataManager::GetStrValue("aera_battery_circle_size"), kBatteryCircleSizeMin,
               kBatteryCircleSizeMax, &size);
  return size;
}

bool RecoverySetBatteryCircleSize(int size, bool save) {
  const int previous = RecoveryBatteryCircleSize();
  size = std::clamp(size, kBatteryCircleSizeMin, kBatteryCircleSizeMax);
  if (DataManager::SetValue("aera_battery_circle_size", size) == 0 && (!save || SaveAeraPreferences())) return true;
  DataManager::SetValue("aera_battery_circle_size", previous);
  return false;
}

bool RecoveryBatteryPercentage() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetStrValue("aera_battery_percentage") != "0";
}

bool RecoverySetBatteryPercentage(bool show) {
  const bool previous = RecoveryBatteryPercentage();
  if (DataManager::SetValue("aera_battery_percentage", show ? 1 : 0) == 0 && SaveAeraPreferences()) return true;
  DataManager::SetValue("aera_battery_percentage", previous ? 1 : 0);
  return false;
}

WifiStatus RecoveryWifiStatus() {
  WifiStatus status;
#ifdef OF_ENABLE_WLAN
  status.supported = true;
  status.enabled = Wlan::IsEnabled();
  status.connected = DataManager::GetIntValue("tw_wlan_connected") == 1;
  status.state = DataManager::GetStrValue("wlan_state");
  status.ssid = DataManager::GetStrValue("wlan_connected_name");
  status.ip_address = DataManager::GetStrValue("wlan_info_ip");
  status.busy = status.state == "enabling" || status.state == "disabling" ||
                status.state == "disconnecting" ||
                status.state == "scanning" || status.state == "connecting";
  std::set<std::string> saved;
  for (const auto &ssid : Lines(ReadText("/tmp/wlan/saved.txt"))) saved.insert(ssid);
  for (const auto &ssid : Lines(ReadText("/tmp/wlan/list.txt"))) {
    WifiNetwork network;
    network.ssid = ssid;
    const auto security = Lines(ReadText(Wlan::NetworkMetadataPath(ssid)));
    network.security = security.empty() ? "UNKNOWN" : security.front();
    network.saved = saved.count(ssid) != 0;
    network.connected = status.connected && status.ssid == ssid;
    status.networks.push_back(std::move(network));
  }
  for (const auto &ssid : saved) {
    if (std::none_of(status.networks.begin(), status.networks.end(),
        [&](const WifiNetwork &network) { return network.ssid == ssid; }))
      status.networks.push_back({ssid, "SAVED", true, status.connected && status.ssid == ssid});
  }
  std::stable_sort(status.networks.begin(), status.networks.end(),
      [](const WifiNetwork &a, const WifiNetwork &b) {
        if (a.connected != b.connected) return a.connected;
        if (a.saved != b.saved) return a.saved;
        return strcasecmp(a.ssid.c_str(), b.ssid.c_str()) < 0;
      });
  for (int i = 1; i <= 5; ++i) {
    const std::string line = DataManager::GetStrValue("wlan_test_line" + std::to_string(i));
    if (!line.empty()) status.test_result += (status.test_result.empty() ? "" : "\n") + line;
  }
#endif
  return status;
}

int RecoveryRunWifi(const WifiRequest &request) {
#ifndef OF_ENABLE_WLAN
  (void)request;
  return 1;
#else
  bool result = false;
  switch (request.operation) {
    case WifiOperation::kEnable:
      DataManager::SetValue("wlan_state", "enabling");
      result = Wlan::Enable();
      if (result) {
        DataManager::SetValue("wlan_state", "scanning");
        Wlan::Scan();
      }
      break;
    case WifiOperation::kDisable:
      RecoverySetFastbootOverWifi(false);
      DataManager::SetValue("wlan_state", "disabling");
      result = Wlan::Disable();
      break;
    case WifiOperation::kDisconnect:
      DataManager::SetValue("wlan_state", "disconnecting");
      result = Wlan::Disconnect();
      break;
    case WifiOperation::kScan:
      DataManager::SetValue("wlan_state", "scanning");
      result = Wlan::Scan();
      break;
    case WifiOperation::kConnect:
      DataManager::SetValue("wlan_state", "connecting");
      DataManager::SetValue("wlanselectedid", request.ssid);
      DataManager::SetValue("wlan_password", request.password);
      result = request.use_saved_credentials ? Wlan::ConnectSaved() : Wlan::Connect();
      DataManager::SetValue("wlan_password", "");
      if (result) SaveAeraPreferences();
      break;
    case WifiOperation::kForget:
      DataManager::SetValue("wlanselectedid", request.ssid);
      result = Wlan::ForgetSaved();
      break;
    case WifiOperation::kTest:
      result = Wlan::TestConnection();
      if (result) {
        result = DataManager::GetStrValue("wlan_test_line3") == "Internet: OK" &&
                 DataManager::GetStrValue("wlan_test_line4") == "DNS: OK";
      }
      break;
    case WifiOperation::kEnableAdb:
      result = RecoverySetAdbOverWifi(true);
      break;
    case WifiOperation::kDisableAdb:
      result = RecoverySetAdbOverWifi(false);
      break;
    case WifiOperation::kEnableFastbootWifi:
      result = RecoverySetFastbootOverWifi(true);
      break;
    case WifiOperation::kDisableFastbootWifi:
      result = RecoverySetFastbootOverWifi(false);
      break;
  }
  const bool enabled = Wlan::IsEnabled();
  const bool connected = DataManager::GetIntValue("tw_wlan_connected") == 1;
  DataManager::SetValue("wlan_state", !enabled ? "disabled" :
                        connected ? "connected" : result ? "enabled" : "error");
  return result ? 0 : 1;
#endif
}
bool RecoveryWifiAutoEnable() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("of_wlan_auto_enable") == 1;
}
bool RecoveryWifiAutoConnect() {
  LoadAeraPreferencesIfAvailable();
  return DataManager::GetIntValue("of_wlan_auto_connect") == 1;
}
bool RecoverySetWifiAutoEnable(bool enabled) {
  LoadAeraPreferencesIfAvailable();
  if (DataManager::SetValue("of_wlan_auto_enable", enabled ? 1 : 0) != 0)
    return false;
  return SaveAeraPreferences();
}
bool RecoverySetWifiAutoConnect(bool enabled) {
  LoadAeraPreferencesIfAvailable();
  if (DataManager::SetValue("of_wlan_auto_connect", enabled ? 1 : 0) != 0)
    return false;
  return SaveAeraPreferences();
}

bool RecoveryAdbOverWifi() {
#ifdef OF_ENABLE_WLAN
  return android::base::GetProperty("persist.adb.tls_server.enable", "0") == "1" ||
         android::base::GetProperty("service.adb.tcp.port", "0") != "0";
#else
  return false;
#endif
}

bool RecoverySetAdbOverWifi(bool enabled) {
#ifdef OF_ENABLE_WLAN
  if (!enabled) {
    const bool fastboot_disabled = RecoverySetFastbootOverWifi(false);
    return AeraAdbd::StopAll() && fastboot_disabled;
  }
  if (!RecoveryWifiConnection().connected) return false;
  return AeraAdbd::StartSecure(5555);
#else
  (void)enabled;
  return false;
#endif
}

AdbWifiStatus RecoveryAdbWifiStatus() {
  AdbWifiStatus result;
#ifdef OF_ENABLE_WLAN
  const auto status = AeraAdbd::GetStatus();
  result.enabled = status.enabled;
  result.secure = status.secure;
  result.no_auth = status.no_auth;
  result.wifi_connected = status.wlan_connected;
  result.pairing = status.pairing;
  result.ip_address = status.ip;
  result.connect_port = status.connect_port;
  result.connect_command = status.connect_command;
  result.pairing_command = status.pairing_command;
  result.pairing_code = status.pairing_code;
#endif
  return result;
}

std::vector<AdbPairedDevice> RecoveryAdbPairedDevices() {
  std::vector<AdbPairedDevice> result;
#ifdef OF_ENABLE_WLAN
  for (const auto &device : AeraAdbd::ListDevices()) {
    result.push_back({device.fingerprint, device.name, device.last_seen});
  }
#endif
  return result;
}

bool RecoveryForgetAdbDevice(const std::string &fingerprint) {
#ifdef OF_ENABLE_WLAN
  if (!AeraAdbd::ForgetDevice(fingerprint)) return false;
  if (AeraAdbd::ListDevices().empty()) RecoverySetFastbootOverWifi(false);
  return true;
#else
  (void)fingerprint;
  return false;
#endif
}

bool RecoveryStartAdbPairing() {
#ifdef OF_ENABLE_WLAN
  if (!RecoveryWifiConnection().connected) return false;
  return AeraAdbd::StartPairing(120);
#else
  return false;
#endif
}

bool RecoveryStopAdbPairing() {
#ifdef OF_ENABLE_WLAN
  return AeraAdbd::StopPairing();
#else
  return false;
#endif
}

FastbootWifiStatus RecoveryFastbootWifiStatus() {
  FastbootWifiStatus result;
  LoadAeraPreferencesIfAvailable();
  result.enabled = DataManager::GetIntValue(kFastbootWifiPreference) == 1;
  result.fastboot_mode =
      android::base::GetProperty(TW_FASTBOOT_MODE_PROP, "0") == "1";
  result.active_wifi = result.fastboot_mode &&
      android::base::GetProperty(kFastbootProtocolProperty, "usb") == "tcp";
#ifdef OF_ENABLE_WLAN
  const auto adb = AeraAdbd::GetStatus();
  const bool paired = !AeraAdbd::ListDevices().empty();
  result.available = adb.wlan_connected && !adb.no_auth && paired;
  if (!adb.wlan_connected) {
    result.reason = "Connect Wi-Fi first";
  } else if (!paired) {
    result.reason = "Pair a computer first";
  } else if (adb.no_auth) {
    result.reason = "Disable unauthenticated wireless ADB first";
  } else if (!adb.secure) {
    result.reason = "Ready to enable through paired ADB";
  } else {
    result.reason = result.enabled ? "Paired-computer tunnel is ready"
                                   : "Off until you enable it";
  }
  if (!adb.connect_command.empty()) {
    const std::string serial = adb.ip + ":" + adb.connect_port;
    result.connect_command = adb.connect_command;
    result.forward_command = "adb -s " + serial +
        " forward tcp:5554 tcp:5554";
    // `fastboot devices` enumerates discoverable transports and does not list
    // a manually addressed TCP endpoint, even when that endpoint is working.
    // Use a harmless command that actually opens and verifies the tunnel.
    result.fastboot_command =
        "fastboot -s tcp:127.0.0.1:5554 getvar product";
  }
#else
  result.reason = "Wi-Fi is unavailable on this build";
#endif
  return result;
}

bool RecoverySetFastbootOverWifi(bool enabled) {
  LoadAeraPreferencesIfAvailable();
#ifndef OF_ENABLE_WLAN
  (void)enabled;
  return false;
#else
  if (enabled) {
    if (!RecoveryWifiConnection().connected || AeraAdbd::ListDevices().empty())
      return false;
    if (!AeraAdbd::StartSecure(5555)) return false;
    const auto adb = AeraAdbd::GetStatus();
    if (!adb.secure || adb.no_auth) return false;
  }

  if (DataManager::SetValue(kFastbootWifiPreference, enabled ? 1 : 0) != 0)
    return false;
  if (!RestartFastbootTransport(enabled)) {
    DataManager::SetValue(kFastbootWifiPreference, enabled ? 0 : 1);
    return false;
  }
  if (enabled && !AeraAdbd::StartSecure(5555)) {
    RestartFastbootTransport(false);
    DataManager::SetValue(kFastbootWifiPreference, 0);
    return false;
  }
  return true;
#endif
}

bool RecoverySelectFastbootTransport(bool wireless) {
#ifndef OF_ENABLE_WLAN
  (void)wireless;
  return false;
#else
  LoadAeraPreferencesIfAvailable();
  if (android::base::GetProperty(TW_FASTBOOT_MODE_PROP, "0") != "1")
    return false;
  if (wireless && !FastbootWifiPrerequisites()) return false;

  // RecoveryEnterFastbootd starts paired wireless ADB once and leaves it
  // available while either command transport is selected. Do not restart it
  // on every pill tap: stopping/starting recovery services from an auxiliary
  // thread was the source of the selector crash. A cold fastbootd entry may
  // not have prepared it yet, so initialize it only when genuinely absent.
  if (wireless) {
    const auto adb = AeraAdbd::GetStatus();
    if ((!adb.secure || adb.no_auth) && !AeraAdbd::StartSecure(5555))
      return false;
  }
  return RestartFastbootTransport(wireless);
#endif
}

std::vector<HomePluginPreference> RecoveryHomePluginLayout() {
  if (RecoveryDataLocked()) return {};
  constexpr const char *path = "/data/media/0/AERA/home_plugins.json";
  struct stat info{};
  if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size <= 0 || info.st_size > 65536) return {};
  std::ifstream input(path);
  Json::CharReaderBuilder reader;
  Json::Value root;
  std::string error;
  if (!Json::parseFromStream(reader, input, &root, &error) ||
      !root.isObject() || !root["schema"].isInt() || root["schema"].asInt() != 1 ||
      !root["plugins"].isArray() || root["plugins"].size() > 512) return {};
  std::vector<HomePluginPreference> result;
  std::set<std::string> ids;
  for (const auto &value : root["plugins"]) {
    if (!value.isObject() || !value["id"].isString() || !value["size"].isInt()) return {};
    HomePluginPreference item{value["id"].asString(), value["size"].asInt()};
    if (!ValidHomePluginPreference(item) || !ids.insert(item.id).second) return {};
    result.push_back(std::move(item));
  }
  return result;
}

bool RecoverySaveHomePluginLayout(const std::vector<HomePluginPreference> &layout) {
  if (RecoveryDataLocked() || layout.size() > 512) return false;
  Json::Value root(Json::objectValue);
  root["schema"] = 1;
  root["plugins"] = Json::Value(Json::arrayValue);
  std::set<std::string> ids;
  for (const auto &item : layout) {
    if (!ValidHomePluginPreference(item) || !ids.insert(item.id).second) return false;
    Json::Value value(Json::objectValue);
    value["id"] = item.id; value["size"] = item.size;
    root["plugins"].append(value);
  }
  if (!TWFunc::Recursive_Mkdir("/data/media/0/AERA", false)) return false;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return WriteFileAtomically("/data/media/0/AERA/home_plugins.json", Json::writeString(writer, root));
}

NasStatus RecoveryNasStatus() {
  NasStatus status;
#ifdef OF_ENABLE_WLAN
  status.supported = true;
  NasManager::RefreshStatus();
  // Stock NAS variables are session-scoped. Restore AERA's last profile only
  // when this fresh recovery process has no host configured yet.
  if (DataManager::GetStrValue(TW_NAS_HOST).empty()) {
    NasConfig remembered;
    if (LoadNasProfile(&remembered)) {
      DataManager::SetValue(TW_NAS_TYPE, remembered.type);
      DataManager::SetValue(TW_NAS_HOST, remembered.host);
      DataManager::SetValue(TW_NAS_PORT, remembered.port);
      DataManager::SetValue(TW_NAS_SHARE, remembered.share);
      DataManager::SetValue(TW_NAS_PATH, remembered.path);
      DataManager::SetValue(TW_NAS_USER, remembered.user);
      DataManager::SetValue(TW_NAS_DOMAIN, remembered.domain);
      DataManager::SetValue(TW_NAS_CACHE_MODE, remembered.cache_mode);
      DataManager::SetValue("aera_nas_auth", remembered.auth);
      DataManager::SetValue("aera_nas_key_name", remembered.key_name);
    }
  }
  status.mounted = NasManager::IsMounted();
  status.selected = status.mounted &&
      DataManager::GetStrValue("tw_storage_path") == TW_NAS_MOUNT_POINT;
  status.status = DataManager::GetStrValue(TW_NAS_STATUS_TEXT);
  status.error = NasManager::GetLastError();
  status.config.type = DataManager::GetStrValue(TW_NAS_TYPE);
  status.config.host = DataManager::GetStrValue(TW_NAS_HOST);
  status.config.port = DataManager::GetStrValue(TW_NAS_PORT);
  status.config.share = DataManager::GetStrValue(TW_NAS_SHARE);
  status.config.path = DataManager::GetStrValue(TW_NAS_PATH);
  status.config.user = DataManager::GetStrValue(TW_NAS_USER);
  status.config.password = DataManager::GetStrValue(TW_NAS_PASS);
  if (status.config.password.empty()) {
    std::string saved_password;
    if (AeraSecrets::GetNasPassword(saved_password, status.config.type)) {
      status.config.password = saved_password;
      // NasManager consumes the session value; the persistent copy remains in
      // the encrypted secret store rather than the plain settings map.
      DataManager::SetValue(TW_NAS_PASS, saved_password);
    }
  }
  status.config.domain = DataManager::GetStrValue(TW_NAS_DOMAIN);
  status.config.cache_mode = DataManager::GetStrValue(TW_NAS_CACHE_MODE);
  status.config.auth = DataManager::GetStrValue("aera_nas_auth") == "key" ? "key" : "password";
  status.config.key_name = DataManager::GetStrValue("aera_nas_key_name");
  std::string private_key;
  status.key_available = AeraSecrets::GetNasPrivateKey(private_key);
  OPENSSL_cleanse(private_key.data(), private_key.size());
  if (status.config.type != "smb") status.config.type = "sftp";
  if (status.config.port.empty()) status.config.port = "22";
  if (status.config.share.empty()) status.config.share = "Backups";
  if (status.config.domain.empty()) status.config.domain = "WORKGROUP";
  if (status.config.cache_mode != "data") status.config.cache_mode = "off";
#endif
  return status;
}

NasConfig RecoverySavedNasConfig(const std::string &protocol) {
  NasConfig config;
  config.type = protocol == "smb" ? "smb" : "sftp";
  config.port = config.type == "smb" ? "445" : "22";
  config.share = "Backups";
  config.domain = "WORKGROUP";
  config.cache_mode = "off";
  config.auth = "password";
#ifdef OF_ENABLE_WLAN
  LoadNasProfile(&config, config.type);
  AeraSecrets::GetNasPassword(config.password, config.type);
#endif
  return config;
}

bool RecoverySetNasConfig(const NasConfig &config, std::string *error) {
#ifndef OF_ENABLE_WLAN
  if (error) *error = "This recovery was built without network storage support.";
  return false;
#else
  const auto invalid = [](const std::string &value, size_t maximum) {
    return value.size() > maximum || value.find('\n') != std::string::npos ||
           value.find('\r') != std::string::npos;
  };
  if (config.type != "sftp" && config.type != "smb") {
    if (error) *error = "Choose either SFTP or SMB.";
    return false;
  }
  if ((config.auth != "password" && config.auth != "key") || invalid(config.key_name, 128) ||
      invalid(config.host, 128) || invalid(config.user, 128) ||
      invalid(config.password, 254) || invalid(config.share, 128) ||
      invalid(config.path, 256) || invalid(config.domain, 128)) {
    if (error) *error = "A field is too long or contains a line break.";
    return false;
  }
  if (config.port.empty() || config.port.size() > 5 ||
      !std::all_of(config.port.begin(), config.port.end(),
                   [](unsigned char character) {
                     return std::isdigit(character) != 0;
                   })) {
    if (error) *error = "Port must be a number from 1 to 65535.";
    return false;
  }
  const long port = strtol(config.port.c_str(), nullptr, 10);
  if (port < 1 || port > 65535) {
    if (error) *error = "Port must be a number from 1 to 65535.";
    return false;
  }
  if (config.cache_mode != "off" && config.cache_mode != "data") {
    if (error) *error = "Choose either no cache or /data write cache.";
    return false;
  }
  bool saved = true;
  saved &= DataManager::SetValue(TW_NAS_TYPE, config.type) == 0;
  saved &= DataManager::SetValue(TW_NAS_HOST, config.host) == 0;
  saved &= DataManager::SetValue(TW_NAS_PORT, config.port) == 0;
  saved &= DataManager::SetValue(TW_NAS_SHARE, config.share) == 0;
  saved &= DataManager::SetValue(TW_NAS_PATH, config.path) == 0;
  saved &= DataManager::SetValue(TW_NAS_USER, config.user) == 0;
  saved &= DataManager::SetValue(TW_NAS_DOMAIN, config.domain) == 0;
  saved &= DataManager::SetValue(TW_NAS_CACHE_MODE, config.cache_mode) == 0;
  saved &= DataManager::SetValue("aera_nas_auth", config.auth) == 0;
  saved &= DataManager::SetValue("aera_nas_key_name", config.key_name) == 0;
  // Keep the live value for NasManager, but never mark it persistent here.
  saved &= DataManager::SetValue(TW_NAS_PASS, config.password) == 0;
  const bool secret_saved = config.password.empty()
      ? AeraSecrets::ClearNasPassword(config.type)
      : AeraSecrets::SetNasPassword(config.password, config.type);
  if (!secret_saved) {
    if (error) *error = "Could not save the password to AERA's encrypted credential store.";
    return false;
  }
  if (!saved || !SaveNasProfile(config)) {
    if (error) *error = "Could not write the connection settings to recovery storage.";
    return false;
  }
  return true;
#endif
}

bool RecoveryImportNasKey(const std::string &key, const std::string &name,
                          std::string *error) {
  if (key.empty() || key.size() > 65536 || key.find('\0') != std::string::npos ||
      name.empty() || name.size() > 128 || name.find_first_of("\r\n/") != std::string::npos ||
      key.find("-----BEGIN ") != 0 || key.find("PRIVATE KEY-----") == std::string::npos) {
    if (error) *error = "Select an OpenSSH or PEM private key, not a public key or PuTTY PPK file.";
    return false;
  }
  if (!AeraSecrets::SetNasPrivateKey(key)) {
    if (error) *error = "Could not save the private key to AERA's encrypted credential store.";
    return false;
  }
  return true;
}

int RecoveryRunNas(const NasRequest &request) {
#ifndef OF_ENABLE_WLAN
  (void)request;
  return 1;
#else
  bool result = false;
  switch (request.operation) {
    case NasOperation::kMountAndUse:
      result = NasManager::Mount() && NasManager::SelectAsStorage();
      break;
    case NasOperation::kUse:
      result = NasManager::SelectAsStorage();
      break;
    case NasOperation::kUnmount:
      result = NasManager::Unmount();
      break;
  }
  NasManager::RefreshStatus();
  return result ? 0 : 1;
#endif
}
}  // namespace aeraui
