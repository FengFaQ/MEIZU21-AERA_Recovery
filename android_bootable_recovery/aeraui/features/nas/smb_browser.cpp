// SPDX-License-Identifier: Apache-2.0
#include "smb_browser.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <json/json.h>

extern char **environ;

namespace aeraui::smb {
namespace {
constexpr const char *kNative = "/system/bin/aera-netfs";
constexpr size_t kMaxListingBytes = 4 * 1024 * 1024;

struct TemporaryFiles {
  std::string directory;
  std::string config, input, output, errors;
  TemporaryFiles() {
    char pattern[] = "/tmp/aera-smb-XXXXXX";
    if (!mkdtemp(pattern)) return;
    directory = pattern;
    config = directory + "/connection.json";
    input = directory + "/input";
    output = directory + "/output";
    errors = directory + "/errors";
  }
  ~TemporaryFiles() {
    if (directory.empty()) return;
    for (const auto &file : {config, input, output, errors}) unlink(file.c_str());
    rmdir(directory.c_str());
  }
};

bool WriteFile(const std::string &path, const std::string &value) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  size_t written = 0;
  while (written < value.size()) {
    const ssize_t count = write(fd, value.data() + written, value.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { close(fd); return false; }
    written += static_cast<size_t>(count);
  }
  close(fd);
  return true;
}

std::string ReadFile(const std::string &path, size_t maximum) {
  std::ifstream stream(path, std::ios::binary);
  std::string result(maximum + 1, '\0');
  stream.read(result.data(), result.size());
  result.resize(static_cast<size_t>(stream.gcount()));
  return result;
}

bool Run(const std::vector<std::string> &arguments, const TemporaryFiles &files,
         const std::atomic<bool> &cancel, std::string *error) {
  if (cancel.load()) return false;
  std::vector<char *> argv;
  for (const auto &argument : arguments)
    argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  int code = posix_spawn_file_actions_init(&actions);
  if (code != 0) { *error = "Could not start the network storage browser."; return false; }
  code = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                         files.input.c_str(), O_RDONLY, 0);
  if (!code) code = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
      files.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (!code) code = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
      files.errors.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  pid_t child = -1;
  if (!code) code = posix_spawn(&child, kNative, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (code != 0) { *error = "Could not start the network storage browser."; return false; }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno == EINTR) continue;
    if (waited < 0) {
      *error = "The network storage browser stopped unexpectedly.";
      return false;
    }
    struct stat info{};
    const bool oversized = stat(files.output.c_str(), &info) == 0 &&
        info.st_size > static_cast<off_t>(kMaxListingBytes);
    if (cancel.load() || oversized || std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      if (!cancel.load()) *error = oversized ? "The folder listing is too large."
          : "The server did not respond. Check Wi-Fi and try again.";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
  *error = ReadFile(files.errors, 2048);
  if (error->empty()) *error = "Check Wi-Fi, server settings and the recovery log, then try again.";
  return false;
}

bool ConfigValue(const std::string &value) {
  return value.find_first_of("\r\n") == std::string::npos &&
         value.find('\0') == std::string::npos;
}
}  // namespace

bool ParseListing(const std::string &json, Result *result) {
  result->directories.clear();
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::istringstream input(json);
  std::string errors;
  if (json.size() > kMaxListingBytes ||
      !Json::parseFromStream(builder, input, &root, &errors) || !root.isArray()) {
    result->error = "The server returned an invalid folder listing.";
    return false;
  }
  for (const auto &item : root) {
    if (!item.isObject() || !item["IsDir"].isBool() || !item["IsDir"].asBool() ||
        !item["Name"].isString()) continue;
    const std::string name = item["Name"].asString();
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos || name.find('\0') != std::string::npos) continue;
    result->directories.push_back(name);
  }
  std::sort(result->directories.begin(), result->directories.end());
  result->directories.erase(std::unique(result->directories.begin(),
      result->directories.end()), result->directories.end());
  result->error.clear();
  return true;
}

Result List(const NasConfig &config, const std::string &share,
            const std::string &path, const std::atomic<bool> &cancel) {
  Result result;
  if (cancel.load()) return result;
  if (config.host.empty() || !ConfigValue(config.host) || !ConfigValue(config.user) ||
      !ConfigValue(config.password) || !ConfigValue(config.domain)) {
    result.error = "Invalid network storage setting";
    return result;
  }
  TemporaryFiles files;
  if (files.directory.empty() || !WriteFile(files.input, "")) {
    result.error = "Could not prepare the network storage browser.";
    return result;
  }
  Json::Value settings;
  settings["version"] = 1;
  settings["type"] = "smb";
  settings["host"] = config.host;
  settings["user"] = config.user;
  settings["password"] = config.password;
  settings["domain"] = config.domain;
  settings["share"] = share;
  settings["path"] = path;
  settings["port"] = 445;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  if (!WriteFile(files.config, Json::writeString(writer, settings))) {
    result.error = "Could not prepare the network storage browser.";
    return result;
  }
  if (!Run({kNative, "list", "--config", files.config},
      files, cancel, &result.error)) return result;
  if (!cancel.load()) ParseListing(ReadFile(files.output, kMaxListingBytes), &result);
  return result;
}
}  // namespace aeraui::smb
