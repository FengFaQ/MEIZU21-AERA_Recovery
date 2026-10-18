/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace aera {
inline bool IsRecoveryLogArchive(const std::string &name) {
  if (name == "recovery_undated.log.zip") return true;
  if (name.size() != 32 || name.compare(0, 9, "recovery_") ||
      name[17] != '_' || name.compare(24, 8, ".log.zip")) return false;
  for (size_t i = 9; i < 24; ++i)
    if (i != 17 && (name[i] < '0' || name[i] > '9')) return false;
  return true;
}

// Only auto-generated regular archives in this directory; never recurse or
// follow archive symlinks. Preserve plain logs and user-created ZIPs.
inline int PruneRecoveryLogArchives(const std::string &path) {
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT ? 0 : errno;
  DIR *directory = fdopendir(fd);
  if (!directory) { const int error = errno; close(fd); return error; }
  struct Archive { std::string name; struct stat st; };
  std::vector<Archive> archives;
  int error = 0;
  while (true) {
    errno = 0;
    auto *entry = readdir(directory);
    if (!entry) { error = errno; break; }
    if (!IsRecoveryLogArchive(entry->d_name)) continue;
    struct stat st{};
    if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
      if (errno != ENOENT) { error = errno; break; }
      continue;
    }
    if (S_ISREG(st.st_mode)) archives.push_back({entry->d_name, st});
  }
  if (error) { closedir(directory); return error; }
  std::sort(archives.begin(), archives.end(), [](const Archive &a, const Archive &b) {
    if (a.st.st_mtim.tv_sec != b.st.st_mtim.tv_sec)
      return a.st.st_mtim.tv_sec > b.st.st_mtim.tv_sec;
    if (a.st.st_mtim.tv_nsec != b.st.st_mtim.tv_nsec)
      return a.st.st_mtim.tv_nsec > b.st.st_mtim.tv_nsec;
    return a.name > b.name;
  });
  for (size_t i = 3; i < archives.size(); ++i) {
    const auto &archive = archives[i];
    struct stat current{};
    if (fstatat(fd, archive.name.c_str(), &current, AT_SYMLINK_NOFOLLOW)) {
      if (errno != ENOENT && !error) error = errno;
      continue;
    }
    if (!S_ISREG(current.st_mode) || current.st_dev != archive.st.st_dev ||
        current.st_ino != archive.st.st_ino || current.st_size != archive.st.st_size ||
        current.st_mtim.tv_sec != archive.st.st_mtim.tv_sec ||
        current.st_mtim.tv_nsec != archive.st.st_mtim.tv_nsec) continue;
    if (unlinkat(fd, archive.name.c_str(), 0) && errno != ENOENT && !error) error = errno;
  }
  closedir(directory);
  return error;
}
}
