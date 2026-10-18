/* SPDX-License-Identifier: Apache-2.0 */
#include "aera_flash_log.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <android-base/properties.h>
#include <ziparchive/zip_writer.h>

#include "data.hpp"
#include "partitions.hpp"
#include "twrp-functions.hpp"
#include "variables.h"

namespace aera::flashlog {
namespace {
constexpr const char *temporary = "/tmp/AERA/logs/FlashLogs";
std::string Settings() {
  std::ostringstream output;
  size_t bytes = 0;
  for (const auto &entry : DataManager::GetDiagnosticSettings()) {
    const auto line = SingleLine(entry.first) + "=" +
        (SensitiveSetting(entry.first) ? "[REDACTED]" : SingleLine(entry.second)) + "\n";
    if ((bytes += line.size()) > 256 * 1024) { output << "[settings snapshot size limit reached]\n"; break; }
    output << line;
  }
  // These controls must be explicit even on devices where their defaults are
  // runtime-only or where the ABL option is hidden by the device configuration.
  output << "\nKeep AERA installed: " << DataManager::GetIntValue(AERA_PRESERVE_RECOVERY_VAR)
         << "\nKeep current ABL: " << DataManager::GetIntValue(AERA_PRESERVE_ABL_VAR) << '\n';
  return output.str();
}
bool MakePath(const std::string &path, bool media = false) {
  for (size_t end = 1; end <= path.size(); ++end) {
    if (end != path.size() && path[end] != '/') continue;
    const auto parent = path.substr(0, end);
    const int created = mkdir(parent.c_str(), media ? 0770 : 0700);
    if (created && errno != EEXIST) return false;
    if (!created && media) TWFunc::set_media_rw_permissions(parent);
  }
  return true;
}
bool WritableLogs(const std::string &path) {
  auto *partition = PartitionManager.Find_Partition_By_Path(path);
  if (!partition || !partition->Is_Mounted()) return false;
  if (partition->Get_Mount_Point() == "/data" && DataManager::GetIntValue(TW_IS_ENCRYPTED) &&
      !DataManager::GetIntValue(TW_IS_DECRYPTED)) return false;
  struct statvfs space{};
  if (statvfs(partition->Get_Mount_Point().c_str(), &space) ||
      static_cast<uint64_t>(space.f_bavail) * space.f_frsize < 64 * 1024 * 1024) return false;
  return MakePath(path, true) && access(path.c_str(), W_OK) == 0;
}
bool Archive(const std::string &root, const std::string &destination) {
  const int fd = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  FILE *file = fdopen(fd, "wb");
  if (!file) { close(fd); return false; }
  bool okay = true;
  {
    ZipWriter writer(file);
    auto add = [&](const char *name, std::initializer_list<const char *> sources) {
      if (!okay) return;
      okay = writer.StartEntry(name, ZipWriter::kCompress | ZipWriter::kDefaultCompression) == 0;
      std::array<char, 65536> buffer{};
      size_t remaining = 32 * 1024 * 1024;
      for (const auto *source : sources) {
        if (!okay) break;
        const int input = open((root + "/" + source).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (input < 0) continue;
        struct stat st{};
        if (fstat(input, &st) || !S_ISREG(st.st_mode)) { close(input); okay = false; break; }
        while (okay && remaining) {
          const auto count = read(input, buffer.data(), std::min(buffer.size(), remaining));
          if (count < 0 && errno == EINTR) continue;
          if (count < 0) { okay = false; break; }
          if (!count) break;
          remaining -= count;
          okay = writer.WriteBytes(buffer.data(), count) == 0;
        }
        close(input);
      }
      if (okay) okay = writer.FinishEntry() == 0;
    };
    add("summary.txt", {"summary.txt"});
    add("recovery.log", {"recovery.previous.log", "recovery.log"});
    add("logcat.txt", {"logcat.previous.txt", "logcat.txt"});
    add("kernel.log", {"kernel.previous.log", "kernel.log"});
    if (okay) okay = writer.Finish() == 0;
  }
  if (fflush(file) || fsync(fd)) okay = false;
  if (fclose(file)) okay = false;
  return okay;
}
}  // namespace

std::vector<std::string> PlatformDirectories() {
  return {Aera_Logs_Dir + "/FlashLogs", temporary};
}
Options PlatformOptions(const std::string &file, const std::string &method, const std::string &details) {
  Options options;
  const auto root = Aera_Logs_Dir + "/FlashLogs";
  // Never keep /data or removable storage open while an installer may unmount
  // or format it. Resolve the persistent destination only after flashing.
  if (MakePath(temporary)) options.directory = temporary;
  if (file.empty() && method.empty() && WritableLogs(root)) options.archive_directory = root;
  if (!options.directory.empty()) {
    chmod(options.directory.c_str(), 0700);
  }
  options.archive = Archive;
  options.permissions = [](const std::string &path) {
    chmod(path.c_str(), 0660); TWFunc::set_media_rw_permissions(path);
  };
  const time_t context_time = time(nullptr) - 5;
  struct tm context{}; localtime_r(&context_time, &context);
  char context_start[40]; strftime(context_start, sizeof(context_start), "%m-%d %H:%M:%S.000", &context);
  options.logcat = {"/system/bin/logcat", "-b", "all", "-v", "threadtime", "-T", context_start};
  options.kernel = {"/system/bin/toybox", "dmesg"};
  if (!file.empty() || !method.empty()) {
    std::ostringstream summary;
    const time_t now = time(nullptr); struct tm utc{}; gmtime_r(&now, &utc);
    char date[40]; strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S UTC", &utc);
    summary << "AERA flash diagnostic report\nStarted: " << date
            << "\nFile: " << SingleLine(file) << "\nMethod: " << SingleLine(method)
            << "\nDetails: " << SingleLine(details)
            << "\nAERA version: " << DataManager::GetStrValue(TW_VERSION_VAR)
            << "\nBuild type: " << AERA_BUILD_TYPE << "\nBuild status: " << AERA_BUILD_STATUS
            << "\nCommit: " << AERA_CURRENT_DEV_STR
            << "\nDevice: " << DataManager::GetStrValue(AERA_COMPATIBILITY_DEVICE)
            << "\nSlot before: " << PartitionManager.Get_Active_Slot_Display() << '\n';
    for (const auto *property : {"ro.product.device", "ro.product.model", "ro.boot.hardware",
        "ro.boot.slot_suffix", "ro.boot.serialno", "ro.build.version.release",
        "ro.build.version.security_patch", "ro.vendor.build.security_patch", "ro.crypto.state", "ro.crypto.type"})
      summary << property << '=' << SingleLine(android::base::GetProperty(property, "")) << '\n';
    summary << "\nRecovery settings at start (credentials omitted):\n" << Settings();
    options.summary = summary.str();
    options.final_summary = [] {
      const time_t now = time(nullptr); struct tm utc{}; gmtime_r(&now, &utc);
      char date[40]; strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S UTC", &utc);
      return "Finished: " + std::string(date) + "\nSlot after: " + PartitionManager.Get_Active_Slot_Display() +
          "\n\nRecovery settings at completion (credentials omitted):\n" + Settings();
    };
  }
  return options;
}
}  // namespace aera::flashlog
