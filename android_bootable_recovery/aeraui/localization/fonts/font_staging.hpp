/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace aeraui::fonts {

// UI-thread only. Files live for the recovery session: FreeType may retain
// streams or mappings, including faces belonging to previews no longer shown.
// Never evict a backing file while a cached face can still reference it.
class FontStaging {
 public:
  std::string Stage(const std::string &source) {
    const auto cached = paths_.find(source);
    if (cached != paths_.end()) return cached->second;
    if (source.empty() || paths_.size() >= 64) return {};
    const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) return {};
    struct stat before{};
    if (fstat(input, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 || before.st_size > 32 * 1024 * 1024 ||
        bytes_ + before.st_size > 128 * 1024 * 1024) {
      close(input);
      return {};
    }
    if (directory_.empty()) {
      char name[] = "/tmp/aera-fonts-XXXXXX";
      if (mkdtemp(name)) directory_ = name;
    }
    if (directory_.empty()) { close(input); return {}; }
    std::string destination = directory_ + "/font-XXXXXX";
    const int output = mkstemp(destination.data());
    if (output < 0) { close(input); return {}; }
    fcntl(output, F_SETFD, FD_CLOEXEC);
    bool ok = true;
    off_t remaining = before.st_size;
    char buffer[16384];
    while (ok && remaining > 0) {
      const size_t amount = remaining < static_cast<off_t>(sizeof(buffer))
          ? static_cast<size_t>(remaining) : sizeof(buffer);
      const ssize_t count = read(input, buffer, amount);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { ok = false; break; }
      ssize_t offset = 0;
      while (offset < count) {
        const ssize_t written = write(output, buffer + offset, count - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        offset += written;
      }
      remaining -= count;
    }
    struct stat after{};
    ok = ok && fstat(input, &after) == 0 &&
         before.st_size == after.st_size &&
         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    close(input);
    if (fchmod(output, 0400) != 0) ok = false;
    if (close(output) != 0) ok = false;
    if (!ok) { unlink(destination.c_str()); return {}; }
    bytes_ += before.st_size;
    paths_.emplace(source, destination);
    return destination;
  }

 private:
  std::string directory_;
  std::map<std::string, std::string> paths_;
  off_t bytes_ = 0;
};

}  // namespace aeraui::fonts
