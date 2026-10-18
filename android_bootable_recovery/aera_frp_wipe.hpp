// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace aera_frp {
struct Target {
  std::string path;
  dev_t device = 0;
  uint64_t bytes = 0;
};

inline bool Enabled() {
#if defined(AERA_ENABLE_FRP_WIPE) && AERA_ENABLE_FRP_WIPE == 1
  return true;
#else
  return false;
#endif
}

inline bool Inspect(const std::string& path, Target* target) {
  if (path.compare(0, 11, "/dev/block/") != 0) return false;
  char resolved[PATH_MAX]{};
  if (!realpath(path.c_str(), resolved) ||
      std::string(resolved).compare(0, 11, "/dev/block/") != 0) return false;
  const int fd = open(resolved, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st{};
  uint64_t bytes = 0;
  bool valid = fstat(fd, &st) == 0 && S_ISBLK(st.st_mode) &&
      ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes > 0 && bytes <= INT64_MAX;
  close(fd);
  // Never accept a whole disk, even if a bad property points at one.
  const std::string partition = "/sys/dev/block/" +
      std::to_string(major(st.st_rdev)) + ":" +
      std::to_string(minor(st.st_rdev)) + "/partition";
  if (!valid || access(partition.c_str(), F_OK) != 0) return false;
  *target = {resolved, st.st_rdev, bytes};
  return true;
}

inline bool Discover(const std::string& property, Target* target) {
  if (!Enabled()) return false;
  // An explicit platform property is authoritative; don't guess if it is bad.
  if (!property.empty()) return Inspect(property, target);
  Target found;
  for (const char* path : {"/dev/block/by-name/frp", "/dev/block/bootdevice/by-name/frp"}) {
    Target candidate;
    if (!Inspect(path, &candidate)) continue;
    if (!found.path.empty() && found.device != candidate.device) return false;
    found = candidate;
  }
  if (found.path.empty()) return false;
  *target = found;
  return true;
}

// Separate fd primitive allows tests on disposable regular files, not devices.
inline bool ZeroAndVerify(int fd, uint64_t bytes) {
  if (!bytes || bytes > INT64_MAX) return false;
  std::array<unsigned char, 64 * 1024> buffer{};
  uint64_t offset = 0;
  while (offset < bytes) {
    const size_t count = std::min<uint64_t>(buffer.size(), bytes - offset);
    const ssize_t n = pwrite(fd, buffer.data(), count, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    offset += n;
  }
  if (fsync(fd) != 0) return false;
  offset = 0;
  while (offset < bytes) {
    const size_t count = std::min<uint64_t>(buffer.size(), bytes - offset);
    const ssize_t n = pread(fd, buffer.data(), count, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || std::any_of(buffer.begin(), buffer.begin() + n,
                            [](unsigned char c) { return c != 0; })) return false;
    offset += n;
  }
  return true;
}

inline bool Wipe(const Target& target) {
  if (!Enabled() || target.path.empty()) return false;
  Target current;
  if (!Inspect(target.path, &current) || current.device != target.device ||
      current.bytes != target.bytes) return false;
  // O_EXCL refuses an in-use/mounted block device. No shell, format or discard.
  const int fd = open(target.path.c_str(), O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat st{};
  uint64_t bytes = 0;
  const bool valid = fstat(fd, &st) == 0 && S_ISBLK(st.st_mode) &&
      st.st_rdev == target.device && ioctl(fd, BLKGETSIZE64, &bytes) == 0 &&
      bytes == target.bytes;
  const bool ok = valid && ZeroAndVerify(fd, bytes);
  const int result = close(fd);
  return ok && result == 0;
}
} // namespace aera_frp
