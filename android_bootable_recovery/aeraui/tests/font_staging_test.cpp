/* SPDX-License-Identifier: Apache-2.0 */
#include "../localization/fonts/font_staging.hpp"
#include <cassert>
#include <dirent.h>
#include <fstream>
#include <iterator>

int main() {
  char name[] = "/tmp/aera-font-source-XXXXXX";
  const int fd = mkstemp(name);
  assert(fd >= 0);
  assert(write(fd, "font-test", 9) == 9);
  close(fd);
  aeraui::fonts::FontStaging cache;
  const auto staged = cache.Stage(name);
  assert(staged.find("/tmp/aera-fonts-") == 0);
  assert(staged != name);
  assert(cache.Stage(name) == staged);
  // Staging itself must not retain a descriptor on the original filesystem.
  DIR *fds = opendir("/proc/self/fd");
  assert(fds);
  while (const auto *entry = readdir(fds)) {
    const std::string descriptor = std::string("/proc/self/fd/") + entry->d_name;
    char target[4096];
    const ssize_t length = readlink(descriptor.c_str(), target, sizeof(target));
    if (length >= 0) assert(std::string(target, length) != name);
  }
  closedir(fds);
  assert(unlink(name) == 0);
  // Formatting/removing the source must not prevent later font sizes loading.
  assert(cache.Stage(name) == staged);
  std::ifstream stream(staged);
  const std::string contents{std::istreambuf_iterator<char>(stream), {}};
  assert(contents == "font-test");
  stream.close();
  assert(cache.Stage("/dev/null").empty());
  assert(cache.Stage("/tmp").empty());
  assert(cache.Stage("").empty());
  assert(symlink(staged.c_str(), name) == 0);
  aeraui::fonts::FontStaging other;
  assert(other.Stage(name).empty());
  unlink(name);
  const int oversized = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(oversized >= 0);
  assert(ftruncate(oversized, 32 * 1024 * 1024 + 1) == 0);
  close(oversized);
  assert(other.Stage(name).empty());
  unlink(name);
  unlink(staged.c_str());
  rmdir(staged.substr(0, staged.find_last_of('/')).c_str());
}
