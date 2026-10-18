/* SPDX-License-Identifier: Apache-2.0 */
#include "operations.hpp"
#include "../../../aera_shared_storage.hpp"
#include <aeraui/i18n.hpp>

#include "../../../aera_remote/aera_remote.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace aeraui::plugin_api {
namespace {
std::atomic<MirrorMode> gMirrorMode{MirrorMode::kOff};
constexpr char kAeraDirectory[] = "/data/media/0/AERA";
constexpr char kBackupDirectory[] = "SettingsBackups";
constexpr char kAndroidBackupDirectory[] = "AndroidSettings";
constexpr char kAndroidSettingsDirectory[] = "/data/system/users/0";
constexpr char kAndroidRestoreMarker[] = ".aera-settings-restore";
constexpr char kSettingsFile[] = "preferences.conf";
constexpr char kBackupFile[] = "recovery-preferences.conf";
constexpr char kAndroidManifest[] = "snapshot.v2";
constexpr char kAndroidManifestWithoutLineage[] =
    "AERA_ANDROID_SETTINGS_V2\n"
    "settings_system.xml\n"
    "settings_secure.xml\n"
    "settings_global.xml\n"
    "lineagesettings.db=0\n";
constexpr char kAndroidManifestWithLineage[] =
    "AERA_ANDROID_SETTINGS_V2\n"
    "settings_system.xml\n"
    "settings_secure.xml\n"
    "settings_global.xml\n"
    "lineagesettings.db=1\n";
constexpr char kLineageSettingsFile[] = "lineagesettings.db";
constexpr size_t kMaximumSettingsBytes = 64 * 1024;
constexpr size_t kMaximumAndroidSettingsBytes = 2 * 1024 * 1024;
constexpr uid_t kAndroidSystemUid = 1000;
constexpr gid_t kAndroidSystemGid = 1000;
constexpr mode_t kAndroidSettingsMode = 0600;

constexpr std::array<const char*, 3> kAndroidSettingsFiles = {
  "settings_system.xml",
  "settings_secure.xml",
  "settings_global.xml",
};
constexpr std::array<const char*, 4> kManagedAndroidSettingsFiles = {
  "settings_system.xml",
  "settings_secure.xml",
  "settings_global.xml",
  kLineageSettingsFile,
};

enum class Validation { kNone, kSettingsXml, kSqlite };

class Fd final {
 public:
  explicit Fd(int value = -1) : value_(value) {}
  ~Fd() {
    if (value_ >= 0) close(value_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : value_(other.value_) {
    other.value_ = -1;
  }
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      if (value_ >= 0) close(value_);
      value_ = other.value_;
      other.value_ = -1;
    }
    return *this;
  }
  int get() const {
    return value_;
  }

 private:
  int value_;
};

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t written = write(fd, bytes + offset, size - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool ReadExactly(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t count = read(fd, bytes + offset, size - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

bool LooksLikeSettingsXml(int fd, off_t size) {
  if (size <= 0 || static_cast<uint64_t>(size) > kMaximumAndroidSettingsBytes) return false;
  std::array<char, 1024> head{};
  std::array<char, 1024> tail{};
  const ssize_t head_size = pread(fd, head.data(), head.size(), 0);
  const off_t tail_offset =
      size > static_cast<off_t>(tail.size()) ? size - static_cast<off_t>(tail.size()) : 0;
  const ssize_t tail_size = pread(fd, tail.data(), tail.size(), tail_offset);
  if (head_size <= 0 || tail_size <= 0) return false;
  const std::string_view beginning(head.data(), static_cast<size_t>(head_size));
  const std::string_view ending(tail.data(), static_cast<size_t>(tail_size));
  return beginning.find("<?xml") != std::string_view::npos &&
         beginning.find("<settings") != std::string_view::npos &&
         ending.find("</settings>") != std::string_view::npos;
}

bool LooksLikeSqlite(int fd, off_t size) {
  constexpr char kSqliteHeader[] = "SQLite format 3";
  std::array<char, 16> header{};
  return size >= 100 && static_cast<uint64_t>(size) <= kMaximumAndroidSettingsBytes &&
         pread(fd, header.data(), header.size(), 0) == static_cast<ssize_t>(header.size()) &&
         memcmp(header.data(), kSqliteHeader, sizeof(kSqliteHeader)) == 0 && header.back() == '\0';
}

bool ValidContents(int fd, off_t size, Validation validation) {
  switch (validation) {
    case Validation::kNone:
      return true;
    case Validation::kSettingsXml:
      return LooksLikeSettingsXml(fd, size);
    case Validation::kSqlite:
      return LooksLikeSqlite(fd, size);
  }
  return false;
}

bool ValidSource(int directory, const char* name, size_t maximum, Validation validation) {
  Fd source(openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  return source.get() >= 0 && fstat(source.get(), &info) == 0 && S_ISREG(info.st_mode) &&
         info.st_size > 0 && static_cast<uint64_t>(info.st_size) <= maximum &&
         ValidContents(source.get(), info.st_size, validation);
}

bool CopyBounded(int source_directory, const char* source_name, int destination_directory,
                 const char* destination_name, size_t maximum, Validation validation,
                 bool android_metadata) {
  Fd source(openat(source_directory, source_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  if (source.get() < 0 || fstat(source.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > maximum ||
      !ValidContents(source.get(), info.st_size, validation))
    return false;

  const std::string temporary = std::string(destination_name) + ".new";
  unlinkat(destination_directory, temporary.c_str(), 0);
  Fd destination(openat(destination_directory, temporary.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        android_metadata ? kAndroidSettingsMode : 0600));
  if (destination.get() < 0) return false;
  if (android_metadata && (fchown(destination.get(), kAndroidSystemUid, kAndroidSystemGid) != 0 ||
                           fchmod(destination.get(), kAndroidSettingsMode) != 0)) {
    unlinkat(destination_directory, temporary.c_str(), 0);
    return false;
  }

  std::array<uint8_t, 4096> buffer{};
  uint64_t total = 0;
  while (true) {
    const ssize_t count = read(source.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return false;
    if (count == 0) break;
    total += static_cast<uint64_t>(count);
    if (total > maximum || !WriteAll(destination.get(), buffer.data(), static_cast<size_t>(count)))
      return false;
  }
  if (fsync(destination.get()) != 0 || total == 0) return false;
  if (renameat(destination_directory, temporary.c_str(), destination_directory, destination_name) !=
      0) {
    unlinkat(destination_directory, temporary.c_str(), 0);
    return false;
  }
  return fsync(destination_directory) == 0;
}

bool OpenDirectories(Fd& aera, Fd& backup) {
  aera = Fd(open(kAeraDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (aera.get() < 0) return false;
  if (mkdirat(aera.get(), kBackupDirectory, 0700) != 0 && errno != EEXIST) return false;
  backup =
      Fd(openat(aera.get(), kBackupDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  return backup.get() >= 0 && fstat(backup.get(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool OpenAndroidDirectories(int backup_directory, Fd& android_settings, Fd& android_backup) {
  android_settings =
      Fd(open(kAndroidSettingsDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (android_settings.get() < 0) return false;
  if (mkdirat(backup_directory, kAndroidBackupDirectory, 0700) != 0 && errno != EEXIST)
    return false;
  android_backup = Fd(openat(backup_directory, kAndroidBackupDirectory,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  return android_backup.get() >= 0 && fstat(android_backup.get(), &info) == 0 &&
         S_ISDIR(info.st_mode);
}

bool WriteAndroidManifest(int directory, bool lineage_settings) {
  constexpr char kTemporary[] = "snapshot.v2.new";
  const std::string_view contents = lineage_settings
                                        ? std::string_view(kAndroidManifestWithLineage)
                                        : std::string_view(kAndroidManifestWithoutLineage);
  unlinkat(directory, kTemporary, 0);
  Fd file(
      openat(directory, kTemporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (file.get() < 0 || !WriteAll(file.get(), contents.data(), contents.size()) ||
      fsync(file.get()) != 0 || renameat(directory, kTemporary, directory, kAndroidManifest) != 0) {
    unlinkat(directory, kTemporary, 0);
    return false;
  }
  return fsync(directory) == 0;
}

bool ReadAndroidManifest(int directory, bool& lineage_settings) {
  Fd file(openat(directory, kAndroidManifest, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  if (file.get() < 0 || fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size <= 0 || info.st_size > 256)
    return false;
  std::array<char, 256> buffer{};
  const size_t size = static_cast<size_t>(info.st_size);
  if (!ReadExactly(file.get(), buffer.data(), size)) return false;
  const std::string_view contents(buffer.data(), size);
  if (contents == std::string_view(kAndroidManifestWithLineage)) {
    lineage_settings = true;
    return true;
  }
  if (contents == std::string_view(kAndroidManifestWithoutLineage)) {
    lineage_settings = false;
    return true;
  }
  return false;
}

void RemoveStagedFiles(int system_directory) {
  for (const char* name : kManagedAndroidSettingsFiles) {
    unlinkat(system_directory, (std::string(name) + ".aera-new").c_str(), 0);
    unlinkat(system_directory, (std::string(name) + ".aera-new.new").c_str(), 0);
  }
}

bool PathExists(int directory, const char* name) {
  struct stat info{};
  return fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(info.st_mode);
}

bool CreateRestoreMarker(int system_directory) {
  unlinkat(system_directory, kAndroidRestoreMarker, 0);
  Fd marker(openat(system_directory, kAndroidRestoreMarker,
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kAndroidSettingsMode));
  return marker.get() >= 0 && fchown(marker.get(), kAndroidSystemUid, kAndroidSystemGid) == 0 &&
         fsync(marker.get()) == 0 && fsync(system_directory) == 0;
}

bool RepairInterruptedRestore(int system_directory) {
  RemoveStagedFiles(system_directory);
  const bool interrupted = PathExists(system_directory, kAndroidRestoreMarker);
  for (const char* name : kManagedAndroidSettingsFiles) {
    const std::string previous = std::string(name) + ".aera-previous";
    struct stat previous_info{};
    if (fstatat(system_directory, previous.c_str(), &previous_info, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) continue;
      return false;
    }
    if (!S_ISREG(previous_info.st_mode)) return false;
    struct stat current_info{};
    if (interrupted) {
      if (unlinkat(system_directory, name, 0) != 0 && errno != ENOENT) return false;
      if (renameat(system_directory, previous.c_str(), system_directory, name) != 0) return false;
    } else if (fstatat(system_directory, name, &current_info, AT_SYMLINK_NOFOLLOW) == 0) {
      if (unlinkat(system_directory, previous.c_str(), 0) != 0) return false;
    } else if (errno != ENOENT ||
               renameat(system_directory, previous.c_str(), system_directory, name) != 0) {
      return false;
    }
  }
  if (interrupted && unlinkat(system_directory, kAndroidRestoreMarker, 0) != 0 && errno != ENOENT)
    return false;
  return fsync(system_directory) == 0;
}

bool BackupAndroidSettings(int system_directory, int backup_directory) {
  unlinkat(backup_directory, kAndroidManifest, 0);
  for (const char* name : kAndroidSettingsFiles) {
    if (!CopyBounded(system_directory, name, backup_directory, name, kMaximumAndroidSettingsBytes,
                     Validation::kSettingsXml, false))
      return false;
  }
  struct stat lineage_info{};
  const int lineage_status =
      fstatat(system_directory, kLineageSettingsFile, &lineage_info, AT_SYMLINK_NOFOLLOW);
  if (lineage_status != 0 && errno != ENOENT) return false;
  const bool lineage_settings = lineage_status == 0;
  if (lineage_settings &&
      (!S_ISREG(lineage_info.st_mode) ||
       !CopyBounded(system_directory, kLineageSettingsFile, backup_directory, kLineageSettingsFile,
                    kMaximumAndroidSettingsBytes, Validation::kSqlite, false)))
    return false;
  if (!lineage_settings) unlinkat(backup_directory, kLineageSettingsFile, 0);
  return WriteAndroidManifest(backup_directory, lineage_settings);
}

bool RestoreAndroidSettings(int system_directory, int backup_directory) {
  bool lineage_settings = false;
  if (!ReadAndroidManifest(backup_directory, lineage_settings)) return false;
  for (const char* name : kAndroidSettingsFiles) {
    if (!ValidSource(backup_directory, name, kMaximumAndroidSettingsBytes,
                     Validation::kSettingsXml))
      return false;
  }
  if (lineage_settings && !ValidSource(backup_directory, kLineageSettingsFile,
                                       kMaximumAndroidSettingsBytes, Validation::kSqlite))
    return false;

  if (!RepairInterruptedRestore(system_directory)) return false;
  for (const char* name : kAndroidSettingsFiles) {
    const std::string staged = std::string(name) + ".aera-new";
    if (!CopyBounded(backup_directory, name, system_directory, staged.c_str(),
                     kMaximumAndroidSettingsBytes, Validation::kSettingsXml, true)) {
      RemoveStagedFiles(system_directory);
      return false;
    }
  }
  if (lineage_settings) {
    const std::string staged = std::string(kLineageSettingsFile) + ".aera-new";
    if (!CopyBounded(backup_directory, kLineageSettingsFile, system_directory, staged.c_str(),
                     kMaximumAndroidSettingsBytes, Validation::kSqlite, true)) {
      RemoveStagedFiles(system_directory);
      return false;
    }
  }

  if (!CreateRestoreMarker(system_directory)) {
    RemoveStagedFiles(system_directory);
    return false;
  }
  const size_t target_count = kAndroidSettingsFiles.size() + (lineage_settings ? 1 : 0);
  size_t saved = 0;
  for (size_t target = 0; target < target_count; ++target) {
    const char* name = kManagedAndroidSettingsFiles[target];
    const std::string previous = std::string(name) + ".aera-previous";
    if (renameat(system_directory, name, system_directory, previous.c_str()) != 0) {
      for (size_t index = 0; index < saved; ++index) {
        const std::string rollback =
            std::string(kManagedAndroidSettingsFiles[index]) + ".aera-previous";
        renameat(system_directory, rollback.c_str(), system_directory,
                 kManagedAndroidSettingsFiles[index]);
      }
      unlinkat(system_directory, kAndroidRestoreMarker, 0);
      RemoveStagedFiles(system_directory);
      fsync(system_directory);
      return false;
    }
    ++saved;
  }

  size_t installed = 0;
  for (size_t target = 0; target < target_count; ++target) {
    const char* name = kManagedAndroidSettingsFiles[target];
    const std::string staged = std::string(name) + ".aera-new";
    if (renameat(system_directory, staged.c_str(), system_directory, name) != 0) {
      for (size_t index = 0; index < installed; ++index)
        unlinkat(system_directory, kManagedAndroidSettingsFiles[index], 0);
      for (size_t index = 0; index < target_count; ++index) {
        const char* rollback_name = kManagedAndroidSettingsFiles[index];
        const std::string previous = std::string(rollback_name) + ".aera-previous";
        renameat(system_directory, previous.c_str(), system_directory, rollback_name);
      }
      unlinkat(system_directory, kAndroidRestoreMarker, 0);
      RemoveStagedFiles(system_directory);
      fsync(system_directory);
      return false;
    }
    ++installed;
  }

  if (fsync(system_directory) != 0) return false;
  if (unlinkat(system_directory, kAndroidRestoreMarker, 0) != 0 || fsync(system_directory) != 0)
    return false;
  for (size_t target = 0; target < target_count; ++target) {
    const char* name = kManagedAndroidSettingsFiles[target];
    const std::string previous = std::string(name) + ".aera-previous";
    unlinkat(system_directory, previous.c_str(), 0);
  }
  if (lineage_settings) {
    unlinkat(system_directory, "lineagesettings.db-wal", 0);
    unlinkat(system_directory, "lineagesettings.db-shm", 0);
    unlinkat(system_directory, "lineagesettings.db-journal", 0);
  }
  return fsync(system_directory) == 0;
}
}  // namespace

MirrorMode ActiveMirrorMode() {
  const auto mode = gMirrorMode.load(std::memory_order_acquire);
  if (mode != MirrorMode::kOff && !aera::remote::Running()) {
    gMirrorMode.store(MirrorMode::kOff, std::memory_order_release);
    return MirrorMode::kOff;
  }
  return mode;
}

bool OperationAllowed(const plugins::Plugin& plugin, Operation operation) {
  if (!plugins::IsGeneric(plugin)) return false;
  switch (operation) {
    case Operation::kBackupSettings:
      return plugins::HasPermission(plugin, "settings-backup");
    case Operation::kRestoreSettings:
      return plugins::HasPermission(plugin, "settings-restore");
    case Operation::kBackupAndroidSettings:
      return plugins::HasPermission(plugin, "android-settings-backup");
    case Operation::kRestoreAndroidSettings:
      return plugins::HasPermission(plugin, "android-settings-restore");
    case Operation::kStartMirror:
    case Operation::kStopMirror:
    case Operation::kStartWifiMirror:
      return plugins::HasPermission(plugin, "screen-mirror");
  }
  return false;
}

bool RunOperation(const plugins::Plugin& plugin, Operation operation, std::string& result) {
  if (!OperationAllowed(plugin, operation)) {
    result = "Permission denied by the AERA host.";
    return false;
  }
  if (operation == Operation::kStartMirror) {
    const bool success = aera::remote::Start(80);
    if (success)
      gMirrorMode.store(MirrorMode::kUsb, std::memory_order_release);
    result = success
                 ? "USB mirror ready. Open it with the AERA Mirror desktop launcher."
                 : "Could not start the AERA USB mirror.";
    return success;
  }
  if (operation == Operation::kStartWifiMirror) {
#ifdef OF_ENABLE_WLAN
    const std::string address = aera::remote::Address();
    if (address.empty() || address == "0.0.0.0") {
      result = "Connect AERA to Wi-Fi first, then return here and start the mirror.";
      return false;
    }
    const bool success = aera::remote::Start(80);
    if (success)
      gMirrorMode.store(MirrorMode::kWifi, std::memory_order_release);
    result = success
        ? i18n::Format("Wi-Fi mirror ready: http://%s/?code=%s",
                       address.c_str(), aera::remote::AccessCode().c_str())
        : "Could not start the AERA Wi-Fi mirror.";
    return success;
#else
    result = "Wi-Fi mirroring is not available in this recovery build.";
    return false;
#endif
  }
  if (operation == Operation::kStopMirror) {
    gMirrorMode.store(MirrorMode::kOff, std::memory_order_release);
    aera::remote::Stop();
    result = "AERA Mirror stopped.";
    return true;
  }
  Fd aera;
  Fd backup;
  if (!OpenDirectories(aera, backup)) {
    result = "Shared recovery storage is locked or unavailable.";
    return false;
  }
  aera::shared_storage::RepairOnExit shared_backup_permissions(
      std::string(kAeraDirectory) + "/" + kBackupDirectory);
  aera::shared_storage::RepairOnExit shared_preferences_permissions(
      std::string(kAeraDirectory) + "/" + kSettingsFile);

  if (operation == Operation::kBackupSettings || operation == Operation::kRestoreSettings) {
    const bool success = operation == Operation::kBackupSettings
                             ? CopyBounded(aera.get(), kSettingsFile, backup.get(), kBackupFile,
                                           kMaximumSettingsBytes, Validation::kNone, false)
                             : CopyBounded(backup.get(), kBackupFile, aera.get(), kSettingsFile,
                                           kMaximumSettingsBytes, Validation::kNone, false);
    if (success) {
      result = operation == Operation::kBackupSettings
                   ? "Recovery preferences backed up to AERA/SettingsBackups."
                   : "Recovery preferences restored. Reopen recovery to reload them.";
    } else {
      result = operation == Operation::kBackupSettings
                   ? "Could not create the recovery settings backup."
                   : "No valid recovery settings backup was found.";
    }
    return success;
  }

  Fd android_settings;
  Fd android_backup;
  if (!OpenAndroidDirectories(backup.get(), android_settings, android_backup)) {
    result = "Android user 0 settings are locked or unavailable.";
    return false;
  }
  const bool success = operation == Operation::kBackupAndroidSettings
                           ? BackupAndroidSettings(android_settings.get(), android_backup.get())
                           : RestoreAndroidSettings(android_settings.get(), android_backup.get());
  if (success) {
    result = operation == Operation::kBackupAndroidSettings
                 ? "Android and ROM settings backed up to AERA/SettingsBackups/AndroidSettings."
                 : "Android and ROM settings restored. They apply on the next Android boot.";
  } else {
    result = operation == Operation::kBackupAndroidSettings
                 ? "Could not create a complete Android settings snapshot."
                 : "No complete, valid Android settings snapshot was found.";
  }
  return success;
}

const char* OperationTitle(Operation operation) {
  switch (operation) {
    case Operation::kBackupSettings:
      return "Back up recovery settings?";
    case Operation::kRestoreSettings:
      return "Restore recovery settings?";
    case Operation::kBackupAndroidSettings:
      return "Back up Android settings?";
    case Operation::kRestoreAndroidSettings:
      return "Restore Android settings?";
    case Operation::kStartMirror:
      return "Start USB Mirror?";
    case Operation::kStopMirror:
      return "Stop AERA Mirror?";
    case Operation::kStartWifiMirror:
      return "Start Wi-Fi Mirror?";
  }
  return "Run plugin operation?";
}

const char* OperationPrompt(Operation operation) {
  switch (operation) {
    case Operation::kBackupSettings:
      return "This isolated plugin is asking AERA to copy recovery preferences "
             "into AERA/SettingsBackups. The plugin cannot read the file itself.";
    case Operation::kRestoreSettings:
      return "This isolated plugin is asking AERA to replace current recovery "
             "preferences with the saved copy. The plugin cannot write the file itself.";
    case Operation::kBackupAndroidSettings:
      return "AERA will save user 0's Android system, secure, global, and available "
             "Lineage settings. This includes Infinity-X customization values. "
             "Lock credentials, accounts, app data, and SettingsProvider SSAIDs are excluded.";
    case Operation::kRestoreAndroidSettings:
      return "AERA will replace user 0's saved Android and ROM settings. Restore only "
             "onto the same ROM and Android version. "
             "The isolated plugin never receives direct /data access.";
    case Operation::kStartMirror:
      return "AERA will share the recovery display and input with the desktop client "
             "over the connected USB/ADB cable.";
    case Operation::kStopMirror:
      return "Stop both the USB stream and Wi-Fi browser server.";
    case Operation::kStartWifiMirror:
      return "AERA will share the recovery display and input on the connected Wi-Fi "
             "network. Enter the phone address shown afterward in any browser.";
  }
  return "The plugin requested an unknown operation.";
}
}  // namespace aeraui::plugin_api
