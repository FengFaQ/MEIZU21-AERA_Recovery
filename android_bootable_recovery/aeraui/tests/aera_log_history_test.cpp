/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_log_history.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

int main() {
  char pattern[] = "/tmp/aera-log-retention-XXXXXX";
  const char *created = mkdtemp(pattern);
  assert(created);
  const std::string root(created);
  const auto make = [&](const std::string &name, time_t seconds) {
    const auto path = root + "/" + name;
    std::ofstream(path) << "test log";
    struct timespec times[2] = {{seconds, 0}, {seconds, 0}};
    assert(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
  };
  const auto exists = [&](const std::string &name) {
    struct stat st{}; return lstat((root + "/" + name).c_str(), &st) == 0;
  };
  std::vector<std::string> names;
  for (int i = 1; i <= 6; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "recovery_20261010_12000%d.log.zip", i);
    names.emplace_back(name);
    make(name, 100 + i);
  }
  make("lastrecoverylog.log", 1);
  make("releaseinfo.json", 1);
  make("my_logs.zip", 1);
  make("recovery_not-a-date.log.zip", 1);
  const std::string folder = "recovery_20261010_120008.log.zip";
  const std::string link = "recovery_20261010_120009.log.zip";
  assert(mkdir((root + "/" + folder).c_str(), 0700) == 0);
  assert(symlink("lastrecoverylog.log", (root + "/" + link).c_str()) == 0);
  assert(aera::PruneRecoveryLogArchives(root) == 0);
  for (int i = 0; i < 6; ++i) assert(exists(names[i]) == (i >= 3));
  for (const auto &name : {"lastrecoverylog.log", "releaseinfo.json", "my_logs.zip",
                           "recovery_not-a-date.log.zip"}) assert(exists(name));
  assert(exists(folder) && exists(link));
  // Modification time wins over a timestamp embedded in the filename.
  make(names[0], 200);
  assert(aera::PruneRecoveryLogArchives(root) == 0);
  assert(exists(names[0]) && exists(names[4]) && exists(names[5]));
  assert(!exists(names[3]));
  make("recovery_undated.log.zip", 300);
  assert(aera::PruneRecoveryLogArchives(root) == 0);
  assert(exists("recovery_undated.log.zip") && exists(names[0]) && exists(names[5]));
  assert(!exists(names[4]));
  assert(aera::PruneRecoveryLogArchives(root) == 0);
  assert(aera::PruneRecoveryLogArchives(root + "/missing") == 0);
  assert(!aera::IsRecoveryLogArchive("../recovery_20261010_120000.log.zip"));
  std::filesystem::remove_all(root);
  puts("Log retention: newest three, repeated cleanup, undated logs, and unrelated-file protection passed.");
}
