// SPDX-License-Identifier: Apache-2.0
#include "nas/smb_browser.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <json/json.h>

static std::string g_executable;
extern "C" int __real_posix_spawn(pid_t *, const char *,
    const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
extern "C" int __wrap_posix_spawn(pid_t *pid, const char *,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const arguments[], char *const environment[]) {
  return __real_posix_spawn(pid, g_executable.c_str(), actions, attributes, arguments, environment);
}

static int FakeNative(int argc, char **argv) {
  std::string config;
  for (int i = 1; i < argc; ++i) {
    assert(std::string(argv[i]).find("test%password") == std::string::npos);
    if (std::string(argv[i]) == "--config" && i + 1 < argc) config = argv[i + 1];
  }
  struct stat info{};
  assert(stat(config.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
  assert(stat(std::filesystem::path(config).parent_path().c_str(), &info) == 0 &&
         (info.st_mode & 0777) == 0700);
  assert(std::string(argv[1]) == "list");
  std::ifstream file(config);
  const std::string contents((std::istreambuf_iterator<char>(file)), {});
  Json::Value profile;
  file.clear();file.seekg(0);file >> profile;
  assert(profile["password"].asString() == "test%password");
  const std::string remote = profile["share"].asString();
  if (remote == "Slow") { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
  if (remote == "Bad") { std::cerr << "Access denied\n"; return 5; }
  if (remote == "Hidden") {
    assert(profile["path"].asString() == "Android/AERA");
    std::cout << "[]\n"; return 0;
  }
  assert(remote.empty());
  std::cout << R"([{"Name":"Public","IsDir":true}])";
  return 0;
}

static size_t TemporaryDirectories() {
  size_t count = 0;
  for (const auto &entry : std::filesystem::directory_iterator("/tmp"))
    if (entry.path().filename().string().rfind("aera-smb-", 0) == 0) ++count;
  return count;
}

int main(int argc, char **argv) {
  if (argc > 1) return FakeNative(argc, argv);
  g_executable = std::filesystem::canonical(argv[0]);
  using namespace aeraui;
  smb::Result parsed;
  assert(smb::ParseListing(R"([{"Name":"z","IsDir":true},{"Name":"a","IsDir":true},{"Name":"a","IsDir":true},{"Name":"file","IsDir":false},{"Name":"..","IsDir":true}])", &parsed));
  assert((parsed.directories == std::vector<std::string>{"a", "z"}));
  assert(smb::ParseListing(R"([{"Name":"\u4e2d\u6587","IsDir":true},{"Name":"space & % folder","IsDir":true}])", &parsed));
  assert(parsed.directories.size() == 2);
  assert(parsed.directories[1] == "\xe4\xb8\xad\xe6\x96\x87");
  assert(!smb::ParseListing("not JSON", &parsed));
  assert(!parsed.error.empty());
  assert(!smb::ParseListing("{}", &parsed));
  assert(smb::ParseListing("[]", &parsed) && parsed.error.empty());
  NasConfig config;
  config.type = "smb";
  config.host = "test-server";
  config.user = "test-user";
  config.password = "test%password";
  std::atomic<bool> cancel{false};
  const size_t before = TemporaryDirectories();
  auto result = smb::List(config, "", "", cancel);
  assert(result.error.empty() && result.directories == std::vector<std::string>{"Public"});
  result = smb::List(config, "Hidden", "Android/AERA", cancel);
  assert(result.error.empty() && result.directories.empty());
  result = smb::List(config, "Bad", "", cancel);
  assert(result.error.find("Access denied") != std::string::npos);
  const auto start = std::chrono::steady_clock::now();
  std::thread worker([&] { result = smb::List(config, "Slow", "", cancel); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  cancel.store(true);
  worker.join();
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
  assert(TemporaryDirectories() == before);
  result = smb::List(config, "Slow", "", cancel);
  assert(result.error.empty());
  assert(TemporaryDirectories() == before);
  std::cout << "SMB JSON, paths, hidden shares, credentials, cancellation and cleanup passed\n";
}
