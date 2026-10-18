// SPDX-License-Identifier: Apache-2.0
#include "../../aera_shared_storage.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
mode_t Mode(const std::string &path) {
  struct stat info{};
  assert(lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}
void File(const std::string &path, mode_t mode) {
  std::ofstream(path) << "unchanged contents";
  assert(chmod(path.c_str(), mode) == 0);
}
void Repair(const std::string &root) {
  int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  assert(fd >= 0);
  struct stat info{};
  assert(fstat(fd, &info) == 0);
  const gid_t group = geteuid() == 0 ? 1023 : getgid();
  assert(!aera::shared_storage::detail::NormalizeFd(fd, info.st_dev + 1, true, group));
  assert(aera::shared_storage::detail::NormalizeFd(fd, info.st_dev, true, group));
  struct stat after{};
  assert(fstat(fd, &after) == 0);
  assert(after.st_uid == info.st_uid && after.st_gid == group);
  close(fd);
}
}  // namespace

int main() {
  using aera::shared_storage::detail::MediaPath;
  for (const auto *path : {"/data/media/0/AERA", "/data/media/10/AERA/AppBackupVault/config",
                          "/data/media/0/AERA/wifi.json", "/data/media/0/AERA/secrets.json"})
    assert(MediaPath(path) == path);
  assert(MediaPath("/data/media/0/AERA/") == "/data/media/0/AERA");
  for (const auto *path : {"/data/recovery/AERA", "/persist/AERA", "/pc/AERA",
                          "/tmp/AERA", "/data/app/AERA", "/data/media/0/AERA-other",
                          "/data/media/0/AERA/../private", "/data/media/0/AERA/./file",
                          "/data/media/no-user/AERA", "/data/media/0/AERA//file"})
    assert(MediaPath(path).empty());

  char pattern[] = "/tmp/aera-shared-permissions-XXXXXX";
  const char *made = mkdtemp(pattern);
  assert(made);
  const std::string temporary(made), root = temporary + "/AERA";
  std::filesystem::create_directories(temporary + "/media/10");
  assert(symlink((temporary + "/media/10").c_str(), (temporary + "/sdcard").c_str()) == 0);
  assert(aera::shared_storage::detail::MediaAlias(
      (temporary + "/sdcard").c_str(), temporary + "/media") == temporary + "/media/10");
  // A different spelling forces the inode-matching path used for bind mounts.
  assert(symlink((temporary + "/media").c_str(), (temporary + "/alias-media").c_str()) == 0);
  assert(aera::shared_storage::detail::MediaAlias(
      (temporary + "/sdcard").c_str(), temporary + "/alias-media") == temporary + "/alias-media/10");
  assert(aera::shared_storage::detail::MediaAlias(temporary.c_str(), temporary + "/media").empty());
  std::filesystem::create_directories(root + "/AppBackupVault/data");
  assert(chmod(root.c_str(), 0700) == 0);
  assert(chmod((root + "/AppBackupVault").c_str(), 0700) == 0);
  assert(chmod((root + "/AppBackupVault/data").c_str(), 0700) == 0);
  File(root + "/AppBackupVault/config", 0400);
  File(root + "/wifi.json", 0600);
  File(root + "/secrets.json", 0600);
  File(root + "/script.sh", 0700);
  File(temporary + "/outside", 0600);
  std::filesystem::create_directory(temporary + "/outside-directory");
  assert(chmod((temporary + "/outside-directory").c_str(), 0700) == 0);
  assert(symlink((temporary + "/outside").c_str(), (root + "/file-link").c_str()) == 0);
  assert(symlink((temporary + "/outside-directory").c_str(),
                 (root + "/directory-link").c_str()) == 0);
  assert(link((temporary + "/outside").c_str(), (root + "/hard-link").c_str()) == 0);
  assert(mkfifo((root + "/pipe").c_str(), 0600) == 0);

  Repair(root);
  assert(Mode(root) == 02770);
  assert(Mode(root + "/AppBackupVault/data") == 02770);
  assert(Mode(root + "/AppBackupVault/config") == 0660);
  assert(Mode(root + "/wifi.json") == 0660);
  assert(Mode(root + "/secrets.json") == 0660);
  assert(Mode(root + "/script.sh") == 0770);
  assert(Mode(temporary + "/outside") == 0600);
  assert(Mode(temporary + "/outside-directory") == 0700);
  assert(Mode(root + "/pipe") == 0600);
  Repair(root);
  std::string text;
  std::getline(std::ifstream(root + "/AppBackupVault/config"), text);
  assert(text == "unchanged contents");
  assert(!aera::shared_storage::Normalize(temporary + "/outside", true));
  assert(Mode(temporary + "/outside") == 0600);
  std::filesystem::remove_all(temporary);
  std::cout << "Shared storage permissions, private-path isolation and link handling passed.\n";
}
