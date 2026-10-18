/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "file_manager.hpp"
#include "../../../aera_shared_storage.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace aeraui::file_manager {
namespace {

struct Totals {
  uint64_t bytes = 0;
  size_t items = 0;
};

std::string Error(const std::string& action, const std::string& path) {
  return action + " " + path + ": " + std::strerror(errno);
}

std::string SanitizeDisplayText(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (size_t index = 0; index < input.size();) {
    const unsigned char first = static_cast<unsigned char>(input[index]);
    if (first < 0x80) {
      if (first == 0x1b && index + 1 < input.size() && input[index + 1] == '[') {
        index += 2;
        while (index < input.size()) {
          const unsigned char value = static_cast<unsigned char>(input[index++]);
          if (value >= 0x40 && value <= 0x7e) break;
        }
        continue;
      }
      if (first == '\n' || first == '\r' || first == '\t' || first >= 0x20)
        output.push_back(static_cast<char>(first));
      else
        output.push_back(' ');
      ++index;
      continue;
    }

    size_t length = 0;
    if (first >= 0xc2 && first <= 0xdf)
      length = 2;
    else if (first >= 0xe0 && first <= 0xef)
      length = 3;
    else if (first >= 0xf0 && first <= 0xf4)
      length = 4;

    bool valid = length != 0 && index + length <= input.size();
    for (size_t offset = 1; valid && offset < length; ++offset) {
      const unsigned char value = static_cast<unsigned char>(input[index + offset]);
      valid = (value & 0xc0) == 0x80;
    }
    if (valid && length == 3) {
      const unsigned char second = static_cast<unsigned char>(input[index + 1]);
      valid = (first != 0xe0 || second >= 0xa0) &&
              (first != 0xed || second < 0xa0);
    }
    if (valid && length == 4) {
      const unsigned char second = static_cast<unsigned char>(input[index + 1]);
      valid = (first != 0xf0 || second >= 0x90) &&
              (first != 0xf4 || second <= 0x8f);
    }
    if (valid) {
      output.append(input, index, length);
      index += length;
    } else {
      output.push_back('?');
      ++index;
    }
  }
  return output;
}

void SetText(Progress& progress, const std::string& status, const std::string& detail = {}) {
  std::lock_guard<std::mutex> lock(progress.text_mutex);
  progress.status = status;
  progress.detail = detail;
}

void UpdatePercent(Progress& progress) {
  const uint64_t total = progress.total_bytes.load();
  const uint64_t complete = progress.completed_bytes.load();
  if (total != 0) {
    progress.percent.store(static_cast<unsigned>(std::min<uint64_t>(99, complete * 100 / total)));
    return;
  }
  const size_t items = progress.total_items.load();
  const size_t done = progress.completed_items.load();
  progress.percent.store(
      items == 0 ? 0 : static_cast<unsigned>(std::min<size_t>(99, done * 100 / items)));
}

void AddError(Progress& progress, const std::string& message) {
  progress.errors.fetch_add(1);
  std::lock_guard<std::mutex> lock(progress.text_mutex);
  if (progress.detail.empty()) progress.detail = message;
}

bool MeasureNode(const std::string& path, Totals& totals, Progress& progress,
                 bool expose_progress) {
  if (progress.cancel.load()) return false;
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) {
    AddError(progress, Error("Cannot inspect", path));
    return false;
  }
  ++totals.items;
  if (S_ISREG(info.st_mode)) totals.bytes += static_cast<uint64_t>(info.st_size);
  if (expose_progress) progress.result_bytes.store(totals.bytes);
  if (!S_ISDIR(info.st_mode)) return true;

  DIR* directory = opendir(path.c_str());
  if (directory == nullptr) {
    AddError(progress, Error("Cannot open", path));
    return false;
  }
  bool okay = true;
  while (auto* item = readdir(directory)) {
    if (!std::strcmp(item->d_name, ".") || !std::strcmp(item->d_name, "..")) continue;
    if (!MeasureNode(Join(path, item->d_name), totals, progress, expose_progress)) {
      okay = false;
      if (progress.cancel.load()) break;
    }
  }
  closedir(directory);
  return okay;
}

bool RemoveNode(const std::string& path, Progress& progress, bool account = true) {
  if (progress.cancel.load()) return false;
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) return true;
    AddError(progress, Error("Cannot inspect", path));
    return false;
  }
  if (S_ISDIR(info.st_mode)) {
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) {
      AddError(progress, Error("Cannot open", path));
      return false;
    }
    bool okay = true;
    while (auto* item = readdir(directory)) {
      if (!std::strcmp(item->d_name, ".") || !std::strcmp(item->d_name, "..")) continue;
      if (!RemoveNode(Join(path, item->d_name), progress, account)) {
        okay = false;
        if (progress.cancel.load()) break;
      }
    }
    closedir(directory);
    if (!okay || progress.cancel.load()) return false;
    if (rmdir(path.c_str()) != 0) {
      AddError(progress, Error("Cannot remove", path));
      return false;
    }
  } else {
    if (unlink(path.c_str()) != 0) {
      AddError(progress, Error("Cannot remove", path));
      return false;
    }
    if (account && S_ISREG(info.st_mode))
      progress.completed_bytes.fetch_add(static_cast<uint64_t>(info.st_size));
  }
  if (account) {
    progress.completed_items.fetch_add(1);
    UpdatePercent(progress);
  }
  return true;
}

bool RemoveNodeQuiet(const std::string& path) {
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return unlink(path.c_str()) == 0;

  DIR* directory = opendir(path.c_str());
  if (directory == nullptr) return false;
  bool okay = true;
  while (auto* item = readdir(directory)) {
    if (!std::strcmp(item->d_name, ".") || !std::strcmp(item->d_name, "..")) continue;
    if (!RemoveNodeQuiet(Join(path, item->d_name))) okay = false;
  }
  closedir(directory);
  return okay && rmdir(path.c_str()) == 0;
}

void CopyExtendedAttributes(const std::string& source, const std::string& destination) {
  const ssize_t names_size = llistxattr(source.c_str(), nullptr, 0);
  if (names_size <= 0) return;
  std::vector<char> names(static_cast<size_t>(names_size));
  if (llistxattr(source.c_str(), names.data(), names.size()) != names_size) return;

  size_t offset = 0;
  while (offset < names.size()) {
    const char* name = names.data() + offset;
    const size_t length = std::strlen(name);
    if (length == 0) break;
    const ssize_t value_size = lgetxattr(source.c_str(), name, nullptr, 0);
    if (value_size >= 0) {
      std::vector<unsigned char> value(static_cast<size_t>(value_size));
      if (lgetxattr(source.c_str(), name, value.data(), value.size()) == value_size) {
        lsetxattr(destination.c_str(), name, value.data(), value.size(), 0);
      }
    }
    offset += length + 1;
  }
}

int CreateTemporaryFile(const std::string& destination, const char* label, mode_t mode,
                        std::string& path) {
  const std::string directory = Parent(destination);
  for (unsigned index = 0; index < 10000; ++index) {
    path = Join(directory, "." + std::string(label) + "-" +
                               std::to_string(static_cast<long long>(getpid())) + "-" +
                               std::to_string(index));
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (file >= 0 || errno != EEXIST) return file;
  }
  errno = EEXIST;
  return -1;
}

void SyncParentDirectory(const std::string& path) {
  const int directory = open(Parent(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) return;
  fsync(directory);
  close(directory);
}

bool CopyRegular(const std::string& source, const std::string& destination, const struct stat& info,
                 Progress& progress) {
  const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC);
  if (input < 0) {
    AddError(progress, Error("Cannot read", source));
    return false;
  }
  std::string temporary;
  const int output = CreateTemporaryFile(destination, "aera-part", 0600, temporary);
  if (output < 0) {
    AddError(progress, Error("Cannot create", destination));
    close(input);
    return false;
  }

  bool okay = true;
  std::vector<char> buffer(1024 * 1024);
  while (!progress.cancel.load()) {
    ssize_t count = read(input, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      AddError(progress, Error("Cannot read", source));
      okay = false;
      break;
    }
    if (count == 0) break;
    ssize_t offset = 0;
    while (offset < count) {
      ssize_t written = write(output, buffer.data() + offset, static_cast<size_t>(count - offset));
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) {
        AddError(progress, Error("Cannot write", destination));
        okay = false;
        break;
      }
      offset += written;
      progress.completed_bytes.fetch_add(static_cast<uint64_t>(written));
      UpdatePercent(progress);
    }
    if (!okay) break;
  }
  if (progress.cancel.load()) okay = false;
  if (okay && fsync(output) != 0) {
    AddError(progress, Error("Cannot sync", destination));
    okay = false;
  }
  if (okay) {
    fchown(output, info.st_uid, info.st_gid);
    fchmod(output, info.st_mode & 07777);
    const timespec times[2] = { info.st_atim, info.st_mtim };
    futimens(output, times);
  }
  close(output);
  close(input);
  if (okay && rename(temporary.c_str(), destination.c_str()) != 0) {
    AddError(progress, Error("Cannot finish", destination));
    okay = false;
  }
  if (okay) {
    CopyExtendedAttributes(source, destination);
    SyncParentDirectory(destination);
  }
  if (!okay) unlink(temporary.c_str());
  return okay;
}

bool CopyNode(const std::string& source, const std::string& destination, Progress& progress) {
  if (progress.cancel.load()) return false;
  struct stat info{};
  if (lstat(source.c_str(), &info) != 0) {
    AddError(progress, Error("Cannot inspect", source));
    return false;
  }
  SetText(progress, "Copying", source);
  bool okay = true;
  if (S_ISREG(info.st_mode)) {
    okay = CopyRegular(source, destination, info, progress);
  } else if (S_ISLNK(info.st_mode)) {
    std::array<char, PATH_MAX + 1> target{};
    const ssize_t count = readlink(source.c_str(), target.data(), PATH_MAX);
    if (count < 0) {
      AddError(progress, Error("Cannot read link", source));
      okay = false;
    } else {
      target[static_cast<size_t>(count)] = '\0';
      if (symlink(target.data(), destination.c_str()) != 0) {
        AddError(progress, Error("Cannot create link", destination));
        okay = false;
      } else {
        lchown(destination.c_str(), info.st_uid, info.st_gid);
        CopyExtendedAttributes(source, destination);
      }
    }
  } else if (S_ISDIR(info.st_mode)) {
    if (mkdir(destination.c_str(), info.st_mode & 07777) != 0 && errno != EEXIST) {
      AddError(progress, Error("Cannot create folder", destination));
      return false;
    }
    DIR* directory = opendir(source.c_str());
    if (directory == nullptr) {
      AddError(progress, Error("Cannot open", source));
      return false;
    }
    while (auto* item = readdir(directory)) {
      if (!std::strcmp(item->d_name, ".") || !std::strcmp(item->d_name, "..")) continue;
      if (!CopyNode(Join(source, item->d_name), Join(destination, item->d_name), progress)) {
        okay = false;
        if (progress.cancel.load()) break;
      }
    }
    closedir(directory);
    if (okay) {
      chown(destination.c_str(), info.st_uid, info.st_gid);
      chmod(destination.c_str(), info.st_mode & 07777);
      const timespec times[2] = { info.st_atim, info.st_mtim };
      utimensat(AT_FDCWD, destination.c_str(), times, AT_SYMLINK_NOFOLLOW);
      CopyExtendedAttributes(source, destination);
    }
  } else {
    AddError(progress, "Unsupported file type: " + source);
    okay = false;
  }
  if (okay) {
    progress.completed_items.fetch_add(1);
    UpdatePercent(progress);
  }
  return okay;
}

std::string Canonical(const std::string& path) {
  std::array<char, PATH_MAX> resolved{};
  return realpath(path.c_str(), resolved.data()) == nullptr ? std::string()
                                                            : std::string(resolved.data());
}

bool Inside(const std::string& parent, const std::string& candidate) {
  const std::string root = Canonical(parent);
  std::string child = Canonical(candidate);
  if (child.empty()) {
    const std::string base = Canonical(Parent(candidate));
    if (!base.empty()) child = Join(base, BaseName(candidate));
  }
  if (root.empty() || child.empty()) return false;
  return child == root || (child.size() > root.size() && child.compare(0, root.size(), root) == 0 &&
                           child[root.size()] == '/');
}

bool SameNode(const std::string& left, const std::string& right) {
  struct stat left_info{};
  struct stat right_info{};
  return lstat(left.c_str(), &left_info) == 0 && lstat(right.c_str(), &right_info) == 0 &&
         left_info.st_dev == right_info.st_dev && left_info.st_ino == right_info.st_ino;
}

std::string UniquePath(const std::string& path, bool directory) {
  if (!Exists(path)) return path;
  const std::string parent = Parent(path);
  const std::string base = BaseName(path);
  std::string stem = base;
  std::string extension;
  if (!directory) {
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
      stem = base.substr(0, dot);
      extension = base.substr(dot);
    }
  }
  for (unsigned index = 1; index < 10000; ++index) {
    const std::string candidate =
        Join(parent, stem + " (" + std::to_string(index) + ")" + extension);
    if (!Exists(candidate)) return candidate;
  }
  return {};
}

std::string ReplacementBackupPath(const std::string& path) {
  const std::string directory = Parent(path);
  for (unsigned index = 0; index < 10000; ++index) {
    const std::string suffix = ".aera-replaced-" +
                               std::to_string(static_cast<long long>(getpid())) + "-" +
                               std::to_string(index);
    const std::string candidate = Join(directory, suffix);
    if (!Exists(candidate)) return candidate;
  }
  return {};
}

bool PrepareDestination(const std::string& source, std::string& destination, const Request& request,
                        Progress& progress, std::string& replacement_backup) {
  const Metadata source_info = Inspect(source);
  if (!source_info.exists) {
    AddError(progress, "Source no longer exists: " + source);
    return false;
  }
  if (!Exists(destination)) {
    if (source_info.directory && Inside(source, destination)) {
      AddError(progress, "Cannot place a folder inside itself: " + source);
      return false;
    }
    return true;
  }
  if (SameNode(source, destination)) {
    if (request.operation == Operation::kMove) {
      progress.skipped.fetch_add(1);
      return false;
    }
    if (request.conflict != Conflict::kKeepBoth) {
      AddError(progress, "Source and destination are the same: " + source);
      return false;
    }
    destination = UniquePath(destination, source_info.directory);
    if (!destination.empty()) return true;
    AddError(progress, "Cannot choose a unique destination for " + source);
    return false;
  }
  if (source_info.directory && Inside(source, destination)) {
    AddError(progress, "Cannot place a folder inside itself: " + source);
    return false;
  }
  if (request.conflict == Conflict::kSkip) {
    progress.skipped.fetch_add(1);
    return false;
  }
  if (request.conflict == Conflict::kKeepBoth) {
    destination = UniquePath(destination, source_info.directory);
    if (destination.empty()) {
      AddError(progress, "Cannot choose a unique destination for " + source);
      return false;
    }
    return true;
  }
  replacement_backup = ReplacementBackupPath(destination);
  if (replacement_backup.empty()) {
    AddError(progress, "Cannot prepare replacement for " + destination);
    return false;
  }
  if (rename(destination.c_str(), replacement_backup.c_str()) == 0) return true;
  AddError(progress, Error("Cannot prepare replacement for", destination));
  replacement_backup.clear();
  return false;
}

void FinishReplacement(const std::string& backup, Progress& progress) {
  if (backup.empty()) return;
  if (!RemoveNodeQuiet(backup)) AddError(progress, "Cannot remove replacement backup: " + backup);
}

void RollBackReplacement(const std::string& destination, const std::string& backup,
                         Progress& progress) {
  if (!RemoveNodeQuiet(destination) && Exists(destination)) {
    AddError(progress, "Cannot clean incomplete destination: " + destination);
    return;
  }
  if (backup.empty()) return;
  if (rename(backup.c_str(), destination.c_str()) != 0)
    AddError(progress, Error("Cannot restore previous destination", destination));
}

}  // namespace

void Progress::Reset() {
  cancel.store(false);
  done.store(false);
  success.store(false);
  cancelled.store(false);
  percent.store(0);
  completed_bytes.store(0);
  total_bytes.store(0);
  result_bytes.store(0);
  completed_items.store(0);
  total_items.store(0);
  errors.store(0);
  skipped.store(0);
  std::lock_guard<std::mutex> lock(text_mutex);
  status.clear();
  detail.clear();
}

Snapshot Progress::Get() const {
  Snapshot value;
  value.percent = percent.load();
  value.done = done.load();
  value.cancelled = cancelled.load();
  value.success = success.load();
  value.completed_bytes = completed_bytes.load();
  value.total_bytes = total_bytes.load();
  value.result_bytes = result_bytes.load();
  value.completed_items = completed_items.load();
  value.total_items = total_items.load();
  value.errors = errors.load();
  value.skipped = skipped.load();
  std::lock_guard<std::mutex> lock(text_mutex);
  value.status = status;
  value.detail = detail;
  return value;
}

Metadata Inspect(const std::string& path) {
  Metadata result;
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) return result;
  result.exists = true;
  result.directory = S_ISDIR(info.st_mode);
  result.regular = S_ISREG(info.st_mode);
  result.symlink = S_ISLNK(info.st_mode);
  result.size = info.st_size < 0 ? 0 : static_cast<uint64_t>(info.st_size);
  result.mode = info.st_mode;
  result.uid = info.st_uid;
  result.gid = info.st_gid;
  result.modified = info.st_mtime;
  if (result.symlink) {
    std::array<char, PATH_MAX + 1> target{};
    const ssize_t count = readlink(path.c_str(), target.data(), PATH_MAX);
    if (count >= 0) {
      target[static_cast<size_t>(count)] = '\0';
      result.link_target = target.data();
    }
  }
  return result;
}

std::string TypeName(const Metadata& metadata) {
  if (metadata.directory) return "Folder";
  if (metadata.symlink) return "Symbolic link";
  if (metadata.regular) return "File";
  return "Special file";
}

std::string PermissionText(mode_t mode) {
  std::string value(9, '-');
  constexpr mode_t masks[] = { S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
                               S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH };
  constexpr char letters[] = { 'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x' };
  for (size_t index = 0; index < 9; ++index)
    if ((mode & masks[index]) != 0) value[index] = letters[index];
  char octal[8];
  std::snprintf(octal, sizeof(octal), "%04o", mode & 07777);
  return value + "  (" + octal + ")";
}

std::string BaseName(const std::string& path) {
  if (path.empty()) return {};
  size_t end = path.size();
  while (end > 1 && path[end - 1] == '/') --end;
  const size_t slash = path.rfind('/', end - 1);
  return slash == std::string::npos ? path.substr(0, end) : path.substr(slash + 1, end - slash - 1);
}

std::string Parent(const std::string& path) {
  if (path.empty() || path == "/") return "/";
  size_t end = path.size();
  while (end > 1 && path[end - 1] == '/') --end;
  const size_t slash = path.rfind('/', end - 1);
  return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

std::string Join(const std::string& directory, const std::string& name) {
  return directory.empty() || directory == "/" ? "/" + name : directory + "/" + name;
}

bool ValidName(const std::string& name, std::string& error) {
  if (name.empty())
    error = "Name cannot be empty.";
  else if (name == "." || name == "..")
    error = "This name is reserved.";
  else if (name.find('/') != std::string::npos)
    error = "A name cannot contain a slash.";
  else if (name.size() > NAME_MAX)
    error = "This name is too long.";
  else
    return true;
  return false;
}

bool Exists(const std::string& path) {
  struct stat info{};
  return lstat(path.c_str(), &info) == 0;
}

bool IsTextFile(const std::string& path, uint64_t maximum_bytes) {
  const Metadata metadata = Inspect(path);
  if (!metadata.regular) return false;
  const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) return false;
  std::array<unsigned char, 8192> buffer{};
  const size_t sample_size = static_cast<size_t>(std::min<uint64_t>(buffer.size(), maximum_bytes));
  const ssize_t count = read(file, buffer.data(), sample_size);
  close(file);
  if (count < 0) return false;
  for (ssize_t index = 0; index < count; ++index)
    if (buffer[static_cast<size_t>(index)] == 0) return false;
  return true;
}

bool ReadText(const std::string& path, uint64_t maximum_bytes, std::string& text, bool& truncated,
              std::string& error) {
  text.clear();
  truncated = false;
  const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) {
    error = Error("Cannot open", path);
    return false;
  }
  std::array<char, 65536> buffer{};
  while (text.size() < maximum_bytes) {
    const size_t wanted = std::min<uint64_t>(buffer.size(), maximum_bytes - text.size());
    ssize_t count = read(file, buffer.data(), wanted);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      error = Error("Cannot read", path);
      close(file);
      return false;
    }
    if (count == 0) break;
    if (std::memchr(buffer.data(), 0, static_cast<size_t>(count)) != nullptr) {
      error = "This is a binary file.";
      close(file);
      return false;
    }
    text.append(buffer.data(), static_cast<size_t>(count));
  }
  char extra;
  truncated = read(file, &extra, 1) == 1;
  close(file);
  text = SanitizeDisplayText(text);
  return true;
}

bool WriteTextAtomic(const std::string& path, const std::string& text, std::string& error) {
  Metadata metadata = Inspect(path);
  if (metadata.exists && !metadata.regular) {
    error = "Only regular files can be edited.";
    return false;
  }
  std::string temporary;
  const int file = CreateTemporaryFile(path, "aera-save",
                                       metadata.exists ? metadata.mode & 07777 : 0644, temporary);
  if (file < 0) {
    error = Error("Cannot create", temporary);
    return false;
  }
  bool okay = true;
  size_t offset = 0;
  while (offset < text.size()) {
    ssize_t count = write(file, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      error = Error("Cannot write", path);
      okay = false;
      break;
    }
    offset += static_cast<size_t>(count);
  }
  if (okay && fsync(file) != 0) {
    error = Error("Cannot sync", path);
    okay = false;
  }
  if (okay && metadata.exists) {
    fchown(file, metadata.uid, metadata.gid);
    fchmod(file, metadata.mode & 07777);
    CopyExtendedAttributes(path, temporary);
  }
  close(file);
  if (okay && rename(temporary.c_str(), path.c_str()) != 0) {
    error = Error("Cannot replace", path);
    okay = false;
  }
  if (okay) SyncParentDirectory(path);
  if (okay) aera::shared_storage::Normalize(path);
  if (!okay) unlink(temporary.c_str());
  return okay;
}

bool CreateDirectory(const std::string& path, std::string& error) {
  if (mkdir(path.c_str(), 0755) == 0) {
    aera::shared_storage::Normalize(path);
    return true;
  }
  error = Error("Cannot create folder", path);
  return false;
}

bool Rename(const std::string& source, const std::string& destination, std::string& error) {
  if (Exists(destination)) {
    error = "An item with this name already exists.";
    return false;
  }
  if (rename(source.c_str(), destination.c_str()) == 0) {
    aera::shared_storage::Normalize(destination, true);
    return true;
  }
  error = Error("Cannot rename", source);
  return false;
}

void Run(const Request& request, Progress& progress) {
  progress.Reset();
  SetText(progress, request.operation == Operation::kMeasure ? "Calculating size" : "Preparing");
  Totals totals;
  for (const auto& source : request.sources) {
    MeasureNode(source, totals, progress, request.operation == Operation::kMeasure);
    if (progress.cancel.load()) break;
  }
  progress.total_bytes.store(totals.bytes);
  progress.total_items.store(totals.items);
  if (request.operation == Operation::kMeasure) {
    progress.result_bytes.store(totals.bytes);
    progress.success.store(progress.errors.load() == 0 && !progress.cancel.load());
    progress.cancelled.store(progress.cancel.load());
    progress.percent.store(progress.cancel.load() ? 0 : 100);
    progress.done.store(true);
    return;
  }
  if (progress.cancel.load()) {
    progress.cancelled.store(true);
    progress.done.store(true);
    return;
  }

  for (const auto& source : request.sources) {
    if (progress.cancel.load()) break;
    SetText(progress,
            request.operation == Operation::kDelete ? "Deleting"
            : request.operation == Operation::kMove ? "Moving"
                                                    : "Copying",
            source);
    if (request.operation == Operation::kDelete) {
      RemoveNode(source, progress);
      continue;
    }

    std::string destination = Join(request.destination, BaseName(source));
    aera::shared_storage::RepairOnExit shared_permissions(destination);
    std::string replacement_backup;
    if (!PrepareDestination(source, destination, request, progress, replacement_backup)) continue;
    if (request.operation == Operation::kMove && rename(source.c_str(), destination.c_str()) == 0) {
      Totals moved;
      Progress ignored;
      ignored.Reset();
      MeasureNode(destination, moved, ignored, false);
      progress.completed_bytes.fetch_add(moved.bytes);
      progress.completed_items.fetch_add(moved.items);
      UpdatePercent(progress);
      FinishReplacement(replacement_backup, progress);
      continue;
    }
    const int move_error = errno;
    if (request.operation == Operation::kMove && move_error != EXDEV) {
      errno = move_error;
      AddError(progress, Error("Cannot move", source));
      RollBackReplacement(destination, replacement_backup, progress);
      continue;
    }
    if (!CopyNode(source, destination, progress)) {
      RollBackReplacement(destination, replacement_backup, progress);
      continue;
    }
    if (request.operation == Operation::kMove && !RemoveNode(source, progress, false)) {
      FinishReplacement(replacement_backup, progress);
      continue;
    }
    FinishReplacement(replacement_backup, progress);
  }

  progress.cancelled.store(progress.cancel.load());
  progress.success.store(!progress.cancel.load() && progress.errors.load() == 0);
  if (!progress.cancel.load()) progress.percent.store(100);
  SetText(progress, progress.cancel.load()        ? "Cancelled"
                    : progress.errors.load() == 0 ? "Complete"
                                                  : "Completed with errors");
  progress.done.store(true);
}

}  // namespace aeraui::file_manager
