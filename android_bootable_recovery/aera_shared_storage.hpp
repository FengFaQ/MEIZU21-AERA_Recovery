// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <selinux/selinux.h>
#endif

namespace aera::shared_storage {
namespace detail {
inline bool UserId(const std::string &text) {
  if (text.empty()) return false;
  for (char c : text) if (c < '0' || c > '9') return false;
  return true;
}

inline std::string MediaAlias(const char *alias, const std::string &base = "/data/media") {
  char *resolved = realpath(alias, nullptr);
  if (!resolved) return {};
  const std::string physical(resolved);
  free(resolved);
  if (physical.compare(0, base.size() + 1, base + "/") == 0 &&
      UserId(physical.substr(base.size() + 1))) return physical;
  // TWRP-style /sdcard is commonly a bind mount, not a symbolic link.
  struct stat storage{};
  if (stat(alias, &storage) != 0 || !S_ISDIR(storage.st_mode)) return {};
  DIR *users = opendir(base.c_str());
  if (!users) return {};
  std::string root;
  while (dirent *entry = readdir(users)) {
    if (!UserId(entry->d_name)) continue;
    const std::string candidate = base + "/" + entry->d_name;
    struct stat info{};
    if (lstat(candidate.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
        info.st_dev == storage.st_dev && info.st_ino == storage.st_ino) {
      root = candidate;
      break;
    }
  }
  closedir(users);
  return root;
}

// Resolve storage aliases only, never a child inside AERA.
inline std::string MediaPath(const std::string &path) {
  std::string result = path;
  const char *alias = nullptr;
  if (path == "/sdcard/AERA" || path.compare(0, 13, "/sdcard/AERA/") == 0)
    alias = "/sdcard";
  else if (path == "/storage/emulated/0/AERA" ||
           path.compare(0, 25, "/storage/emulated/0/AERA/") == 0)
    alias = "/storage/emulated/0";
  if (alias) {
    const std::string root = MediaAlias(alias);
    if (root.empty()) return {};
    result = root + path.substr(std::string(alias).size());
  }
  while (result.size() > 1 && result.back() == '/') result.pop_back();
  const std::string prefix = "/data/media/";
  if (result.compare(0, prefix.size(), prefix) != 0) return {};
  const size_t end = result.find('/', prefix.size());
  if (end == std::string::npos || !UserId(result.substr(prefix.size(), end - prefix.size())))
    return {};
  if (result.substr(end) != "/AERA" && result.compare(end, 6, "/AERA/") != 0)
    return {};
  size_t start = 1;
  while (start < result.size()) {
    const size_t next = result.find('/', start);
    const std::string part = result.substr(start, next - start);
    if (part.empty() || part == "." || part == "..") return {};
    if (next == std::string::npos) break;
    start = next + 1;
  }
  return result;
}

inline bool NormalizeFd(int fd, dev_t device, bool recursive, gid_t group,
                        unsigned depth = 0) {
  struct stat info{};
  if (fstat(fd, &info) != 0 || info.st_dev != device || depth > 128) return false;
  const bool directory = S_ISDIR(info.st_mode);
  if (!directory && (!S_ISREG(info.st_mode) || info.st_nlink != 1)) return false;
  // Keep the owner: Downloads may still belong to the running browser UID.
  const mode_t mode = directory ? 02770 : (0660 | ((info.st_mode & 0111) ? 0110 : 0));
  bool ok = true;
  if (info.st_gid != group && fchown(fd, static_cast<uid_t>(-1), group) != 0) ok = false;
  if ((info.st_mode & 07777) != mode && fchmod(fd, mode) != 0) ok = false;
#ifdef __ANDROID__
  constexpr const char *label = "u:object_r:media_rw_data_file:s0";
  char *current = nullptr;
  if (fgetfilecon(fd, &current) < 0 || !current || std::string(current) != label) {
    if (fsetfilecon(fd, label) != 0) ok = false;
  }
  if (current) freecon(current);
#endif
  if (!recursive || !directory) return ok;
  const int copy = dup(fd);
  DIR *entries = copy < 0 ? nullptr : fdopendir(copy);
  if (!entries) { if (copy >= 0) close(copy); return false; }
  for (;;) {
    errno = 0;
    dirent *entry = readdir(entries);
    if (!entry) { if (errno) ok = false; break; }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    struct stat child{};
    if (fstatat(fd, name.c_str(), &child, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT) ok = false;
      continue;
    }
    if (!S_ISDIR(child.st_mode) && !S_ISREG(child.st_mode)) continue;
    if (child.st_dev != device || (S_ISREG(child.st_mode) && child.st_nlink != 1)) continue;
    const int item = openat(fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK |
                           O_CLOEXEC | (S_ISDIR(child.st_mode) ? O_DIRECTORY : 0));
    if (item < 0) { if (errno != ENOENT) ok = false; continue; }
    if (!NormalizeFd(item, device, true, group, depth + 1)) ok = false;
    close(item);
  }
  closedir(entries);
  return ok;
}
}  // namespace detail

// No-op outside physical Android shared storage, including /pc and /persist.
inline bool Normalize(const std::string &path, bool recursive = false) {
  const std::string media = detail::MediaPath(path);
  if (media.empty()) return false;
  const size_t root_end = media.find('/', 12);
  if (root_end == std::string::npos) return false;
  int fd = open("/data/media", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  const std::string user = media.substr(12, root_end - 12);
  int next = openat(fd, user.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(fd);
  if (next < 0) return false;
  fd = next;
  struct stat info{};
  if (fstat(fd, &info) != 0) { close(fd); return false; }
  bool ok = true;
  size_t start = root_end + 1;
  while (start < media.size()) {
    size_t end = media.find('/', start);
    if (end == std::string::npos) end = media.size();
    struct stat child{};
    if (fstatat(fd, media.substr(start, end - start).c_str(), &child,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        (!S_ISDIR(child.st_mode) && !S_ISREG(child.st_mode))) {
      close(fd);
      return false;
    }
    next = openat(fd, media.substr(start, end - start).c_str(),
                  O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
                  (end < media.size() ? O_DIRECTORY : 0));
    close(fd);
    if (next < 0) return false;
    fd = next;
    if (!detail::NormalizeFd(fd, info.st_dev, recursive && end == media.size(), 1023)) ok = false;
    start = end + 1;
  }
  close(fd);
  return ok;
}

inline void NormalizeAll() {
  DIR *users = opendir("/data/media");
  if (!users) return;
  while (dirent *entry = readdir(users)) {
    if (detail::UserId(entry->d_name))
      Normalize(std::string("/data/media/") + entry->d_name + "/AERA", true);
  }
  closedir(users);
}

class RepairOnExit {
 public:
  explicit RepairOnExit(std::string path) : path_(std::move(path)) {}
  ~RepairOnExit() { Normalize(path_, true); }
  RepairOnExit(const RepairOnExit &) = delete;
  RepairOnExit &operator=(const RepairOnExit &) = delete;
 private:
  std::string path_;
};
}  // namespace aera::shared_storage
