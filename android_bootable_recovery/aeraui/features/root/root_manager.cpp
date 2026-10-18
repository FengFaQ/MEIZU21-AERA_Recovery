/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "root_manager.hpp"
#include "../../../aera_shared_storage.hpp"
#include "kernel_kmi.hpp"
#include "module_assets.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <limits.h>
#include <json/json.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <selinux/selinux.h>

#include <map>
#include <mutex>

#include "android_icon.hpp"
#include <aeraui/i18n.hpp>
#include "twcommon.h"

namespace aeraui::root {
namespace {

constexpr char kKsud[] = "/sbin/ksud";
constexpr char kMagiskboot[] = "/sbin/magiskboot";
constexpr char kRoot[] = "/sdcard/AERA/RootManager";
constexpr char kCache[] = "/sdcard/AERA/RootManager/cache";
constexpr char kBackups[] = "/sdcard/AERA/RootManager/backups";
constexpr char kReceipts[] = "/sdcard/AERA/RootManager/receipts";
constexpr char kWork[] = "/tmp/aera-root-manager";
constexpr char kBundledRoot[] = "/system/etc/aera/root-providers";
constexpr char kBundledCatalog[] = "/system/etc/aera/root-providers/providers.json";
constexpr char kManagerStaging[] = "/data/adb/aera/root-manager";
constexpr char kManagerModule[] = "/data/adb/modules/aera-manager-installer";
constexpr uint64_t kMaxMetadata = 2 * 1024 * 1024;
constexpr uint64_t kMaxAsset = 8 * 1024 * 1024;
constexpr uint64_t kMaxManagerApk = 64 * 1024 * 1024;
constexpr uint64_t kMaxModuleZip = 256 * 1024 * 1024;

std::mutex gMutex;
std::mutex gOperationMutex;
std::map<Provider, Release> gReleases;
std::vector<Module> gModules;

void SetText(Progress &progress, const std::string &status,
             const std::string &detail = {}) {
  {
    std::lock_guard<std::mutex> lock(progress.text_mutex);
    progress.status = status;
    progress.detail = detail;
  }
  LOGINFO("AERA Root Manager: %s%s%s\n", status.c_str(),
          detail.empty() ? "" : " — ", detail.c_str());
}

std::string Trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) ++first;
  return value.substr(first);
}

std::vector<uint64_t> VersionParts(const std::string &version) {
  std::vector<uint64_t> parts;
  size_t offset = 0;
  while (offset < version.size()) {
    while (offset < version.size() &&
           !std::isdigit(static_cast<unsigned char>(version[offset])))
      ++offset;
    if (offset == version.size()) break;
    uint64_t value = 0;
    while (offset < version.size() &&
           std::isdigit(static_cast<unsigned char>(version[offset]))) {
      const unsigned digit = static_cast<unsigned>(version[offset] - '0');
      value = value > (UINT64_MAX - digit) / 10
                  ? UINT64_MAX : value * 10 + digit;
      ++offset;
    }
    parts.push_back(value);
  }
  while (!parts.empty() && parts.back() == 0) parts.pop_back();
  return parts;
}

int CompareVersions(const std::string &left, const std::string &right) {
  const auto a = VersionParts(left);
  const auto b = VersionParts(right);
  const size_t count = std::max(a.size(), b.size());
  for (size_t index = 0; index < count; ++index) {
    const uint64_t av = index < a.size() ? a[index] : 0;
    const uint64_t bv = index < b.size() ? b[index] : 0;
    if (av != bv) return av > bv ? 1 : -1;
  }
  return 0;
}

std::vector<std::string> Lines(const std::string &text) {
  std::vector<std::string> result;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    line = Trim(line);
    if (!line.empty()) result.push_back(std::move(line));
  }
  return result;
}

int64_t JsonInteger(const Json::Value &value, int64_t fallback = 0) {
  if (value.isInt64()) return value.asInt64();
  if (value.isUInt64()) {
    const uint64_t number = value.asUInt64();
    return number <= static_cast<uint64_t>(INT64_MAX)
               ? static_cast<int64_t>(number) : fallback;
  }
  if (value.isString()) {
    const std::string text = value.asString();
    if (text.empty()) return fallback;
    char *end = nullptr;
    errno = 0;
    const long long number = strtoll(text.c_str(), &end, 10);
    return errno == 0 && end != text.c_str() && *end == '\0'
               ? static_cast<int64_t>(number) : fallback;
  }
  return fallback;
}

bool JsonBoolean(const Json::Value &value, bool fallback) {
  if (value.isBool()) return value.asBool();
  if (value.isInt() || value.isUInt()) return value.asInt() != 0;
  if (value.isString()) {
    std::string text = value.asString();
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (text == "true" || text == "1" || text == "yes") return true;
    if (text == "false" || text == "0" || text == "no") return false;
  }
  return fallback;
}

bool EnsureDirectory(const std::string &path) {
  if (path.empty() || path.front() != '/') return false;
  std::string current;
  size_t start = 1;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const std::string part = path.substr(start, slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    current += "/" + part;
    struct stat info{};
    if (lstat(current.c_str(), &info) == 0) {
      if (!S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode)) return false;
    } else if (errno != ENOENT || mkdir(current.c_str(), 0700) != 0) {
      return false;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

bool SafeId(const std::string &id) {
  return !id.empty() && id.size() <= 128 &&
      std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
      });
}

bool SafeHttps(const std::string &url) {
  if (url.size() < 12 || url.size() > 2048 ||
      url.compare(0, 8, "https://") != 0) return false;
  return std::none_of(url.begin(), url.end(), [](unsigned char c) {
    return c <= 0x20 || c == 0x7f;
  });
}

bool OfficialUrl(const std::string &url) {
  return SafeHttps(url) &&
      (url.compare(0, 23, "https://api.github.com/") == 0 ||
       url.compare(0, 19, "https://github.com/") == 0);
}

bool RunCapture(const std::vector<std::string> &arguments, std::string *output,
                int output_fd = -1, const char *working_directory = nullptr) {
  if (arguments.empty() || arguments.front().empty()) return false;
  int pipefd[2] = {-1, -1};
  if (output_fd < 0 && pipe(pipefd) != 0) return false;
  const pid_t child = fork();
  if (child < 0) {
    if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
    return false;
  }
  if (child == 0) {
    if (working_directory && chdir(working_directory) != 0) _exit(126);
    if (output_fd >= 0) {
      if (dup2(output_fd, STDOUT_FILENO) < 0) _exit(126);
    } else {
      close(pipefd[0]);
      if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
    }
    if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) _exit(126);
    if (pipefd[1] > STDERR_FILENO) close(pipefd[1]);
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &item : arguments)
      argv.push_back(const_cast<char *>(item.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  if (output_fd >= 0) close(output_fd);
  std::string captured;
  if (pipefd[0] >= 0) {
    close(pipefd[1]);
    std::array<char, 4096> buffer{};
    for (;;) {
      const ssize_t count = read(pipefd[0], buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      if (captured.size() < 1024 * 1024)
        captured.append(buffer.data(), static_cast<size_t>(count));
    }
    close(pipefd[0]);
  }
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); }
  while (waited < 0 && errno == EINTR);
  if (output) *output = captured;
  return (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
         (waited < 0 && errno == ECHILD);
}

std::string CaptureKsud(std::initializer_list<const char *> arguments) {
  std::vector<std::string> command{kKsud};
  for (const char *argument : arguments) command.emplace_back(argument);
  std::string output;
  return RunCapture(command, &output) ? Trim(output) : std::string();
}

bool ReadJson(const std::string &path, Json::Value &value) {
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info{};
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<uint64_t>(info.st_size) > kMaxMetadata) {
    close(fd); return false;
  }
  std::string text(static_cast<size_t>(info.st_size), '\0');
  size_t done = 0;
  while (done < text.size()) {
    const ssize_t count = read(fd, text.data() + done, text.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { close(fd); return false; }
    done += static_cast<size_t>(count);
  }
  close(fd);
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::string errors;
  std::istringstream input(text);
  return Json::parseFromStream(builder, input, &value, &errors);
}

bool ReadText(const std::string &path, std::string &text,
              uint64_t maximum = 32 * 1024 * 1024) {
  text.clear();
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info{};
  if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) ||
      info.st_size < 0 || static_cast<uint64_t>(info.st_size) > maximum) {
    if (fd >= 0) close(fd);
    return false;
  }
  text.resize(static_cast<size_t>(info.st_size));
  size_t done = 0;
  while (done < text.size()) {
    const ssize_t count = read(fd, text.data() + done, text.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      close(fd);
      text.clear();
      return false;
    }
    done += static_cast<size_t>(count);
  }
  close(fd);
  return true;
}

bool WriteText(const std::string &path, const std::string &text, mode_t mode) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW |
                O_CLOEXEC, mode);
  if (fd < 0) return false;
  bool ok = fchmod(fd, mode) == 0 && fchown(fd, 0, 0) == 0;
  size_t done = 0;
  while (ok && done < text.size()) {
    const ssize_t count = write(fd, text.data() + done, text.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) ok = false;
    else done += static_cast<size_t>(count);
  }
  if (ok) ok = fsync(fd) == 0;
  close(fd);
  if (!ok) unlink(path.c_str());
  return ok;
}

bool HashFile(const std::string &path, uint64_t expected_size,
              const std::string &expected_hash, std::string *actual_out = nullptr) {
  std::string open_path = path;
  const bool trusted_block_alias =
      path.compare(0, sizeof("/dev/block/by-name/") - 1,
                   "/dev/block/by-name/") == 0;
  if (trusted_block_alias) {
    char resolved[PATH_MAX]{};
    if (!realpath(path.c_str(), resolved) ||
        strncmp(resolved, "/dev/block/", sizeof("/dev/block/") - 1) != 0)
      return false;
    open_path = resolved;
  }
  // User-controlled files must never be followed through a symlink.  The one
  // exception is Android's trusted by-name block alias, resolved above to a
  // concrete /dev/block node and validated again with fstat below.
  int fd = open(open_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info{};
  if (fd < 0 || fstat(fd, &info) ||
      (!S_ISREG(info.st_mode) && !S_ISBLK(info.st_mode)) ||
      (trusted_block_alias && !S_ISBLK(info.st_mode)) ||
      (S_ISREG(info.st_mode) && info.st_size <= 0) ||
      (S_ISREG(info.st_mode) && expected_size &&
       static_cast<uint64_t>(info.st_size) != expected_size)) {
    if (fd >= 0) close(fd); return false;
  }
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  std::array<unsigned char, 65536> buffer{};
  while (ok) {
    const ssize_t count = read(fd, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) { ok = false; break; }
    if (count == 0) break;
    ok = EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(count)) == 1;
  }
  close(fd);
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  ok = ok && EVP_DigestFinal_ex(context, digest.data(), &length) == 1 && length == 32;
  EVP_MD_CTX_free(context);
  constexpr char hex[] = "0123456789abcdef";
  std::string actual;
  for (unsigned i = 0; ok && i < length; ++i) {
    actual.push_back(hex[digest[i] >> 4]);
    actual.push_back(hex[digest[i] & 15]);
  }
  if (actual_out) *actual_out = actual;
  std::string expected = expected_hash;
  std::transform(expected.begin(), expected.end(), expected.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return ok && (expected.empty() || actual == expected);
}

bool VerifyBlockWrite(const std::string &path, uint64_t expected_size,
                      const std::string &expected_hash,
                      std::string *actual_out = nullptr) {
  std::string actual;
  for (unsigned attempt = 0; attempt < 10; ++attempt) {
    // Some UFS/block drivers acknowledge fsync before a newly written boot
    // partition is visible through a separately opened read descriptor.  Ask
    // the block layer to flush and invalidate its buffers before each bounded
    // readback attempt.  Unsupported ioctls are harmless; the retries remain.
    sync();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      ioctl(fd, BLKFLSBUF);
      close(fd);
    }
    actual.clear();
    if (HashFile(path, expected_size, expected_hash, &actual)) {
      if (actual_out) *actual_out = actual;
      return true;
    }
    if (attempt + 1 < 10) usleep(250000);
  }
  if (actual_out) *actual_out = actual;
  return false;
}

bool Download(const std::string &url, const std::string &path, uint64_t limit,
              Progress &progress, unsigned from, unsigned to,
              uint64_t expected_size, bool official) {
  if ((official ? !OfficialUrl(url) : !SafeHttps(url)) || limit == 0) return false;
  unlink(path.c_str());
  int completion[2];
  if (pipe(completion) != 0) return false;
  const pid_t child = fork();
  if (child < 0) { close(completion[0]); close(completion[1]); return false; }
  if (child == 0) {
    close(completion[0]);
    const int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(null_fd, STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0)
      _exit(126);
    if (null_fd > STDERR_FILENO) close(null_fd);
    rlimit maximum{limit, limit};
    if (setrlimit(RLIMIT_FSIZE, &maximum) != 0) _exit(126);
    umask(077);
    const char *busybox = access("/sbin/busybox", X_OK) == 0
                              ? "/sbin/busybox" : "/system/bin/busybox";
    const char *argv[] = {busybox, "wget", "-q", "-T", "15", "-t", "2",
                          "-O", path.c_str(), url.c_str(), nullptr};
    execv(busybox, const_cast<char *const *>(argv));
    _exit(127);
  }
  close(completion[1]);
  progress.downloaded.store(0);
  progress.total.store(expected_size);
  pollfd event{completion[0], POLLIN | POLLHUP, 0};
  timespec started{};
  clock_gettime(CLOCK_MONOTONIC, &started);
  const uint64_t timeout_ms = limit <= kMaxMetadata ? 30000 :
      (limit <= kMaxAsset ? 120000 : 600000);
  bool timed_out = false;
  while (!progress.cancel.load()) {
    const int result = poll(&event, 1, 100);
    if (result < 0 && errno == EINTR) continue;
    if (result < 0 || (event.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t elapsed_ms = static_cast<int64_t>(now.tv_sec - started.tv_sec) * 1000 +
        static_cast<int64_t>(now.tv_nsec - started.tv_nsec) / 1000000;
    if (elapsed_ms >= static_cast<int64_t>(timeout_ms)) {
      timed_out = true;
      kill(child, SIGKILL);
      break;
    }
    struct stat info{};
    if (!lstat(path.c_str(), &info) && S_ISREG(info.st_mode) && info.st_size >= 0) {
      const uint64_t amount = static_cast<uint64_t>(info.st_size);
      progress.downloaded.store(amount);
      if (expected_size) progress.value.store(from + static_cast<unsigned>(
          std::min(amount, expected_size) * (to - from) / expected_size));
    }
  }
  if (progress.cancel.load()) kill(child, SIGKILL);
  close(completion[0]);
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); }
  while (waited < 0 && errno == EINTR);
  struct stat info{};
  const bool exited = (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
                      (waited < 0 && errno == ECHILD);
  const bool valid = exited && !timed_out && !progress.cancel.load() &&
      lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_size > 0 && static_cast<uint64_t>(info.st_size) <= limit &&
      (!expected_size || static_cast<uint64_t>(info.st_size) == expected_size);
  if (!valid) unlink(path.c_str());
  else progress.value.store(to);
  return valid;
}

const char *Repository(Provider provider) {
  switch (provider) {
    case Provider::kKernelSU: return "tiann/KernelSU";
    case Provider::kKernelSUNext: return "KernelSU-Next/KernelSU-Next";
    case Provider::kSukiSU: return "SukiSU-Ultra/SukiSU-Ultra";
  }
  return "";
}

const char *ProviderId(Provider provider) {
  switch (provider) {
    case Provider::kKernelSU: return "kernelsu";
    case Provider::kKernelSUNext: return "kernelsu-next";
    case Provider::kSukiSU: return "sukisu-ultra";
  }
  return "";
}

const char *ManagerPackage(Provider provider) {
  switch (provider) {
    case Provider::kKernelSU: return "me.weishu.kernelsu";
    case Provider::kKernelSUNext: return "com.rifsxd.ksunext";
    case Provider::kSukiSU: return "com.sukisu.ultra";
  }
  return "";
}

std::string ManagerStagingPath(Provider provider) {
  return std::string(kManagerStaging) + "/" + ProviderId(provider) + ".apk";
}

bool ManagerInstallerPending(Provider provider) {
  const std::string service = std::string(kManagerModule) + "/service.sh";
  const std::string removed = std::string(kManagerModule) + "/remove";
  struct stat apk_info{};
  struct stat service_info{};
  const std::string apk = ManagerStagingPath(provider);
  if (lstat(apk.c_str(), &apk_info) || !S_ISREG(apk_info.st_mode) ||
      apk_info.st_size <= 0 || lstat(service.c_str(), &service_info) ||
      !S_ISREG(service_info.st_mode) || access(removed.c_str(), F_OK) == 0)
    return false;

  std::string script;
  if (!ReadText(service, script, 64 * 1024)) return false;
  return script.find("APK='" + apk + "'") != std::string::npos &&
      script.find("PACKAGE='" + std::string(ManagerPackage(provider)) + "'") !=
          std::string::npos;
}

std::string ManagerRemovalMarker(Provider provider) {
  return std::string(kManagerStaging) + "/" + ProviderId(provider) +
         ".uninstall";
}

bool ManagerRemovalPending(Provider provider) {
  const std::string service = std::string(kManagerModule) + "/service.sh";
  const std::string removed = std::string(kManagerModule) + "/remove";
  const std::string marker = ManagerRemovalMarker(provider);
  struct stat marker_info{};
  struct stat service_info{};
  if (lstat(marker.c_str(), &marker_info) || !S_ISREG(marker_info.st_mode) ||
      lstat(service.c_str(), &service_info) || !S_ISREG(service_info.st_mode) ||
      access(removed.c_str(), F_OK) == 0)
    return false;
  std::string script;
  if (!ReadText(service, script, 64 * 1024)) return false;
  return script.find("ACTION='uninstall'") != std::string::npos &&
      script.find("PACKAGE='" + std::string(ManagerPackage(provider)) + "'") !=
          std::string::npos &&
      script.find("MARKER='" + marker + "'") != std::string::npos;
}

bool ManagerAssetName(Provider provider, const std::string &name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".apk") ||
      lower.find("spoof") != std::string::npos) return false;
  switch (provider) {
    case Provider::kKernelSU:
      return lower.compare(0, 9, "kernelsu_") == 0 &&
             lower.find("next") == std::string::npos &&
             lower.find("release") != std::string::npos;
    case Provider::kKernelSUNext:
      return lower.compare(0, 14, "kernelsu_next_") == 0 &&
             lower.find("release") != std::string::npos;
    case Provider::kSukiSU:
      return lower.compare(0, 7, "sukisu_") == 0 &&
             lower.find("release") != std::string::npos;
  }
  return false;
}

bool PackageListed(const std::string &package_name) {
  std::string packages;
  if (ReadText("/data/system/packages.list", packages, 16 * 1024 * 1024)) {
    size_t offset = 0;
    while (offset < packages.size()) {
      const size_t end = packages.find('\n', offset);
      const size_t length =
          (end == std::string::npos ? packages.size() : end) - offset;
      if (length > package_name.size() &&
          packages.compare(offset, package_name.size(), package_name) == 0 &&
          std::isspace(static_cast<unsigned char>(
              packages[offset + package_name.size()]))) return true;
      if (end == std::string::npos) break;
      offset = end + 1;
    }
  }
  if (!ReadText("/data/system/packages.xml", packages)) return false;
  return packages.find("name=\"" + package_name + "\"") != std::string::npos;
}

bool FetchManagerApk(Provider provider, Progress &progress, Release &release) {
  release = {};
  release.provider = provider;
  if (!EnsureDirectory(kWork)) {
    release.error = "Could not create the temporary root workspace.";
    return false;
  }
  const std::string metadata = std::string(kWork) + "/manager-release.json";
  const std::string url = std::string("https://api.github.com/repos/") +
                          Repository(provider) + "/releases/latest";
  SetText(progress, "Finding manager app", Repository(provider));
  progress.value.store(2);
  if (!Download(url, metadata, kMaxMetadata, progress, 2, 18, 0, true)) {
    release.error = "Could not download the official release metadata.";
    return false;
  }
  Json::Value root;
  if (!ReadJson(metadata, root) || !root.isObject() ||
      !root["assets"].isArray()) {
    release.error = "GitHub returned malformed release metadata.";
    return false;
  }
  release.version = root.get("tag_name", "").asString();
  unsigned matches = 0;
  for (const auto &asset : root["assets"]) {
    const std::string name = asset.get("name", "").asString();
    if (!ManagerAssetName(provider, name)) continue;
    ++matches;
    release.asset_name = name;
    release.asset_url = asset.get("browser_download_url", "").asString();
    release.size = asset.get("size", Json::UInt64(0)).asUInt64();
    release.sha256 = asset.get("digest", "").asString();
    if (release.sha256.compare(0, 7, "sha256:") == 0)
      release.sha256.erase(0, 7);
  }
  const bool hash_ok = release.sha256.size() == 64 &&
      std::all_of(release.sha256.begin(), release.sha256.end(),
                  [](unsigned char c) { return std::isxdigit(c); });
  if (matches != 1 || release.version.empty() ||
      !OfficialUrl(release.asset_url) || release.size < 64 * 1024 ||
      release.size > kMaxManagerApk || !hash_ok) {
    release.error = matches == 0
        ? "The latest release does not contain a standard manager APK."
        : "The official manager APK is ambiguous or has no SHA-256 digest.";
    return false;
  }
  release.available = true;
  progress.value.store(20);
  return true;
}

bool FetchRelease(Provider provider, const Status &device, Progress &progress,
                  Release &release) {
  release = {};
  release.provider = provider;
  if (!device.kmi_supported) {
    release.error = "The running kernel KMI is not supported by ksud.";
    return false;
  }
  if (!EnsureDirectory(kWork)) {
    release.error = "Could not create temporary root workspace."; return false;
  }
  const std::string metadata = std::string(kWork) + "/release.json";
  const std::string url = std::string("https://api.github.com/repos/") +
                          Repository(provider) + "/releases/latest";
  SetText(progress, "Checking official release", Repository(provider));
  progress.value.store(2);
  if (!Download(url, metadata, kMaxMetadata, progress, 2, 15, 0, true)) {
    release.error = "Could not download the official GitHub release metadata.";
    return false;
  }
  Json::Value root;
  if (!ReadJson(metadata, root) || !root.isObject() || !root["assets"].isArray()) {
    release.error = "GitHub returned malformed release metadata."; return false;
  }
  release.version = root["tag_name"].asString();
  release.kmi = device.kmi;
  const auto accepted = ModuleAssetNames(provider, device.kmi);
  unsigned matches = 0;
  for (const auto &wanted : accepted) {
    matches = 0;
    for (const auto &asset : root["assets"]) {
      if (asset["name"].asString() != wanted) continue;
      ++matches;
      release.asset_name = wanted;
      release.asset_url = asset["browser_download_url"].asString();
      release.size = asset["size"].asUInt64();
      std::string digest = asset.get("digest", "").asString();
      if (digest.compare(0, 7, "sha256:") == 0) digest.erase(0, 7);
      release.sha256 = digest;
    }
    if (matches != 0) break;
  }
  release.archive = provider == Provider::kSukiSU;
  const bool hash_ok = release.sha256.size() == 64 &&
      std::all_of(release.sha256.begin(), release.sha256.end(), [](unsigned char c) {
        return std::isxdigit(c);
      });
  if (matches != 1 || release.version.empty() || !OfficialUrl(release.asset_url) ||
      release.size == 0 || release.size > kMaxAsset || !hash_ok) {
    release.error = matches == 0
        ? i18n::Format(
              "No exact %s aarch64 module exists in the latest release.",
              device.kmi.c_str())
        : "The matching release asset is ambiguous or has no trusted SHA-256.";
    return false;
  }
  release.available = true;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gReleases[provider] = release;
  }
  progress.value.store(18);
  SetText(progress, "Compatible release found",
          std::string(ProviderName(provider)) + " " + release.version + " • " + device.kmi);
  return true;
}

Release LoadBundledRelease(Provider provider, const std::string &kmi) {
  Release release;
  release.provider = provider;
  release.kmi = kmi;
  release.bundled = true;
  Json::Value catalog;
  if (!ReadJson(kBundledCatalog, catalog) || catalog.get("schema", 0).asInt() != 1 ||
      !catalog["providers"].isArray()) {
    release.error = "The bundled root provider catalog is missing or invalid.";
    return release;
  }
  const std::string wanted_id = ProviderId(provider);
  const auto accepted = ModuleAssetNames(provider, kmi);
  for (const auto &item : catalog["providers"]) {
    if (item.get("id", "").asString() != wanted_id || !item["assets"].isArray()) continue;
    release.version = item.get("version", "").asString();
    for (const auto &asset : item["assets"]) {
      const std::string wanted_asset = asset.get("name", "").asString();
      if (asset.get("kmi", "").asString() != kmi ||
          std::find(accepted.begin(), accepted.end(), wanted_asset) == accepted.end()) continue;
      release.asset_name = wanted_asset;
      release.size = asset.get("size", Json::UInt64(0)).asUInt64();
      release.sha256 = asset.get("sha256", "").asString();
      release.archive = asset.get("archive", false).asBool();
      release.local_path = std::string(kBundledRoot) + "/" + wanted_id + "/" + wanted_asset;
      struct stat info{};
      const bool hash_shape = release.sha256.size() == 64 &&
          std::all_of(release.sha256.begin(), release.sha256.end(), [](unsigned char c) {
            return std::isxdigit(c);
          });
      release.available = !release.version.empty() && release.size > 0 &&
          release.size <= kMaxAsset && hash_shape &&
          lstat(release.local_path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
          static_cast<uint64_t>(info.st_size) == release.size;
      if (!release.available)
        release.error = "The exact bundled module failed its catalog size check.";
      return release;
    }
    release.error = i18n::Format(
        "No bundled module exists for exact KMI %s.", kmi.c_str());
    return release;
  }
  release.error = "The selected provider is missing from the bundled catalog.";
  return release;
}

bool CopyExact(const std::string &source, const std::string &target,
               uint64_t expected, Progress &progress, unsigned from, unsigned to) {
  int input = open(source.c_str(), O_RDONLY | O_CLOEXEC);
  const bool block_target = target.compare(0, 11, "/dev/block/") == 0;
  int output = open(target.c_str(), O_WRONLY | O_CLOEXEC |
                    (block_target ? 0 : O_CREAT | O_TRUNC), 0600);
  if (input < 0 || output < 0) {
    if (input >= 0) close(input); if (output >= 0) close(output); return false;
  }
  std::array<unsigned char, 256 * 1024> buffer{};
  uint64_t done = 0;
  bool ok = true;
  while (done < expected && !progress.cancel.load()) {
    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), expected - done));
    ssize_t count = read(input, buffer.data(), wanted);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ok = false; break; }
    size_t offset = 0;
    while (offset < static_cast<size_t>(count)) {
      const ssize_t written = write(output, buffer.data() + offset,
                                    static_cast<size_t>(count) - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) { ok = false; break; }
      offset += static_cast<size_t>(written);
    }
    if (!ok) break;
    done += static_cast<uint64_t>(count);
    progress.value.store(from + static_cast<unsigned>(done * (to - from) / expected));
  }
  if (fsync(output) != 0) ok = false;
  close(input); close(output);
  if (!ok || done != expected || progress.cancel.load()) { unlink(target.c_str()); return false; }
  return true;
}

uint64_t FileSize(const std::string &path) {
  struct stat info{};
  if (stat(path.c_str(), &info) != 0) return 0;
  if (S_ISREG(info.st_mode) && info.st_size > 0)
    return static_cast<uint64_t>(info.st_size);
  if (S_ISBLK(info.st_mode)) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    uint64_t size = 0;
    if (fd >= 0) { ioctl(fd, BLKGETSIZE64, &size); close(fd); }
    return size;
  }
  return 0;
}

std::string RandomCodeToken() {
  std::array<unsigned char, 16> bytes{};
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  size_t done = 0;
  while (fd >= 0 && done < bytes.size()) {
    const ssize_t count = read(fd, bytes.data() + done, bytes.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    done += static_cast<size_t>(count);
  }
  if (fd >= 0) close(fd);
  if (done != bytes.size()) return {};
  constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string result;
  result.reserve(24);
  for (size_t offset = 0; offset < bytes.size(); offset += 3) {
    const size_t left = bytes.size() - offset;
    const uint32_t value = static_cast<uint32_t>(bytes[offset]) << 16 |
        (left > 1 ? static_cast<uint32_t>(bytes[offset + 1]) << 8 : 0) |
        (left > 2 ? bytes[offset + 2] : 0);
    result.push_back(alphabet[(value >> 18) & 63]);
    result.push_back(alphabet[(value >> 12) & 63]);
    result.push_back(left > 1 ? alphabet[(value >> 6) & 63] : '=');
    result.push_back(left > 2 ? alphabet[value & 63] : '=');
  }
  return result;
}

bool CreateApkDirectory(const std::string &path) {
  if (mkdir(path.c_str(), 0755) || chmod(path.c_str(), 0755) ||
      chown(path.c_str(), 1000, 1000)) return false;
  return setfilecon(path.c_str(), "u:object_r:apk_data_file:s0") == 0;
}

bool StageDataApp(const std::string &apk, const std::string &package_name,
                  uint64_t size, Progress &progress, std::string &code_path) {
  struct stat data_app{};
  if (lstat("/data/app", &data_app) || !S_ISDIR(data_app.st_mode) ||
      access("/data/app", W_OK)) return false;
  std::string outer;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    const std::string token = RandomCodeToken();
    if (token.empty()) return false;
    outer = "/data/app/~~" + token;
    if (CreateApkDirectory(outer)) break;
    if (errno != EEXIST) return false;
    outer.clear();
  }
  if (outer.empty()) return false;
  const std::string token = RandomCodeToken();
  const std::string package_dir = outer + "/" + package_name + "-" + token;
  if (token.empty() || !CreateApkDirectory(package_dir)) {
    rmdir(outer.c_str());
    return false;
  }
  code_path = package_dir + "/base.apk";
  if (!CopyExact(apk, code_path, size, progress, 70, 90) ||
      chmod(code_path.c_str(), 0644) || chown(code_path.c_str(), 1000, 1000) ||
      setfilecon(code_path.c_str(), "u:object_r:apk_data_file:s0")) {
    unlink(code_path.c_str());
    rmdir(package_dir.c_str());
    rmdir(outer.c_str());
    code_path.clear();
    return false;
  }
  return true;
}

bool StageManagerFallback(const std::string &apk,
                          const std::string &package_name,
                          const std::string &provider_name,
                          uint64_t apk_size) {
  if (!EnsureDirectory("/data/adb") ||
      !EnsureDirectory("/data/adb/modules") ||
      !EnsureDirectory(kManagerModule)) return false;
  const std::string module = kManagerModule;
  unlink((module + "/remove").c_str());
  const std::string module_prop =
      "id=aera-manager-installer\n"
      "name=AERA Root Manager installer\n"
      "version=1\n"
      "versionCode=1\n"
      "author=AERA Recovery Project\n"
      "description=One-shot installer for " + provider_name + "\n";
  const std::string script =
      "#!/system/bin/sh\n"
      "APK='" + apk + "'\n"
      "APK_SIZE='" + std::to_string(apk_size) + "'\n"
      "PACKAGE='" + package_name + "'\n"
      "MODULE_ID='aera-manager-installer'\n"
      "MODDIR='/data/adb/modules/aera-manager-installer'\n"
      "LOG='/data/adb/aera/root-manager/install.log'\n"
      "boot_attempt=0\n"
      "while [ \"$(getprop sys.boot_completed)\" != \"1\" ] && "
          "[ \"$boot_attempt\" -lt 180 ]; do\n"
      "  sleep 1\n"
      "  boot_attempt=$((boot_attempt + 1))\n"
      "done\n"
      ": >\"$LOG\"\n"
      "result=1\n"
      "install_attempt=0\n"
      "while [ \"$install_attempt\" -lt 60 ]; do\n"
      "  if /system/bin/pm path \"$PACKAGE\" >/dev/null 2>&1; then\n"
      "    result=0\n"
      "    break\n"
      "  fi\n"
      // system_server cannot open files below /data/adb under enforcing
      // SELinux.  Stream the verified APK through stdin so Package Manager
      // never needs direct access to the protected staging path.  Its output
      // must not be redirected to the /data/adb log: the descriptor crosses
      // Binder into system_server, which is forbidden from writing an
      // adb_data_file and aborts the install transaction.
      "  cat \"$APK\" | /system/bin/pm install -S \"$APK_SIZE\" -r "
          "--user 0 >/dev/null 2>&1\n"
      "  result=$?\n"
      "  [ \"$result\" -eq 0 ] && break\n"
      "  echo \"Package Manager attempt $((install_attempt + 1)) failed "
          "($result)\" >>\"$LOG\"\n"
      "  install_attempt=$((install_attempt + 1))\n"
      "  sleep 2\n"
      "done\n"
      "if [ \"$result\" -eq 0 ]; then\n"
      "  rm -f \"$APK\"\n"
      // Ask the provider runtime to retire the one-shot module.  Directly
      // creating its marker can be denied when recovery-created module files
      // retain adb_data_file labels under enforcing Android SELinux.
      "  if [ -x /data/adb/ksud ]; then\n"
      "    /data/adb/ksud module uninstall \"$MODULE_ID\" "
          ">/dev/null 2>&1\n"
      "  else\n"
      "    touch \"$MODDIR/remove\"\n"
      "  fi\n"
      "fi\n";
  return WriteText(module + "/module.prop", module_prop, 0644) &&
      WriteText(module + "/service.sh", script, 0755) &&
      WriteText(module + "/skip_mount", "", 0600);
}

bool StageManagerRemoval(Provider provider, Progress &progress) {
  const std::string package_name = ManagerPackage(provider);
  if (package_name.empty()) return false;
  if (access("/data/system/packages.xml", R_OK)) {
    SetText(progress, "Unlock data first",
            "AERA needs decrypted Android data to remove the manager app.");
    return false;
  }
  if (!PackageListed(package_name)) {
    progress.value.store(100);
    SetText(progress, "Manager already removed",
            i18n::Format("%s is not installed in Android.",
                         ProviderName(provider)));
    return true;
  }
  if (!EnsureDirectory("/data/adb") ||
      !EnsureDirectory("/data/adb/modules") ||
      !EnsureDirectory(kManagerStaging) ||
      !EnsureDirectory(kManagerModule)) {
    SetText(progress, "Uninstall unavailable",
            "Could not create the one-shot Android uninstall service.");
    return false;
  }

  const std::string module = kManagerModule;
  const std::string marker = ManagerRemovalMarker(provider);
  const std::string staged_apk = ManagerStagingPath(provider);
  unlink(staged_apk.c_str());
  unlink((module + "/remove").c_str());
  const std::string module_prop =
      "id=aera-manager-installer\n"
      "name=AERA Root Manager service\n"
      "version=1\n"
      "versionCode=1\n"
      "author=AERA Recovery Project\n"
      "description=One-shot manager app maintenance service\n";
  const std::string script =
      "#!/system/bin/sh\n"
      "ACTION='uninstall'\n"
      "PACKAGE='" + package_name + "'\n"
      "MARKER='" + marker + "'\n"
      "MODULE_ID='aera-manager-installer'\n"
      "MODDIR='/data/adb/modules/aera-manager-installer'\n"
      "LOG='/data/adb/aera/root-manager/uninstall.log'\n"
      "boot_attempt=0\n"
      "while [ \"$(getprop sys.boot_completed)\" != \"1\" ] && "
          "[ \"$boot_attempt\" -lt 180 ]; do\n"
      "  sleep 1\n"
      "  boot_attempt=$((boot_attempt + 1))\n"
      "done\n"
      ": >\"$LOG\"\n"
      "result=1\n"
      "uninstall_attempt=0\n"
      "while [ \"$uninstall_attempt\" -lt 30 ]; do\n"
      "  /system/bin/pm uninstall \"$PACKAGE\" >>\"$LOG\" 2>&1\n"
      "  result=$?\n"
      "  [ \"$result\" -eq 0 ] && break\n"
      "  uninstall_attempt=$((uninstall_attempt + 1))\n"
      "  sleep 2\n"
      "done\n"
      "if [ \"$result\" -eq 0 ]; then\n"
      "  rm -f \"$MARKER\"\n"
      "  if [ -x /data/adb/ksud ]; then\n"
      "    /data/adb/ksud module uninstall \"$MODULE_ID\" "
          ">/dev/null 2>&1\n"
      "  else\n"
      "    touch \"$MODDIR/remove\"\n"
      "  fi\n"
      "fi\n";
  if (!WriteText(marker, package_name + "\n", 0600) ||
      !WriteText(module + "/module.prop", module_prop, 0644) ||
      !WriteText(module + "/service.sh", script, 0755) ||
      !WriteText(module + "/skip_mount", "", 0600)) {
    unlink(marker.c_str());
    SetText(progress, "Uninstall unavailable",
            "Could not stage the one-shot Android uninstall service.");
    return false;
  }
  progress.value.store(100);
  SetText(progress, "Manager removal ready", i18n::Format(
      "%s will be uninstalled when Android boots.", ProviderName(provider)));
  return true;
}

bool InstallManager(Provider provider, Progress &progress) {
  const std::string package_name = ManagerPackage(provider);
  if (package_name.empty()) return false;
  if (PackageListed(package_name)) {
    progress.value.store(100);
    SetText(progress, "Manager already installed",
            i18n::Format("%s is registered in Android.",
                         ProviderName(provider)));
    return true;
  }
  if (access("/data/system/packages.xml", R_OK) || access("/data/app", W_OK)) {
    SetText(progress, "Unlock data first",
            "AERA needs decrypted Android data to install the manager app.");
    return false;
  }
  Release release;
  if (!FetchManagerApk(provider, progress, release)) {
    SetText(progress, "Manager unavailable", release.error);
    return false;
  }
  if (!EnsureDirectory(kCache) || !EnsureDirectory(kManagerStaging)) {
    SetText(progress, "Storage unavailable",
            "Could not create the manager download folders.");
    return false;
  }
  const std::string download = std::string(kCache) + "/" + release.asset_name;
  SetText(progress, "Downloading manager app",
          std::string(ProviderName(provider)) + " " + release.version);
  if (!HashFile(download, release.size, release.sha256) &&
      !Download(release.asset_url, download, kMaxManagerApk, progress, 20, 58,
                release.size, true)) {
    SetText(progress, "Download failed", "Could not download the official manager APK.");
    return false;
  }
  if (!HashFile(download, release.size, release.sha256)) {
    unlink(download.c_str());
    SetText(progress, "Verification failed",
            "The manager APK does not match GitHub's SHA-256 digest.");
    return false;
  }
  std::string parsed_package;
  if (!ReadAndroidPackageName(download, parsed_package) ||
      parsed_package != package_name) {
    unlink(download.c_str());
    SetText(progress, "APK rejected",
            "The downloaded APK does not contain the expected manager package.");
    return false;
  }
  const std::string staged = ManagerStagingPath(provider);
  if (!CopyExact(download, staged, release.size, progress, 58, 68) ||
      chmod(staged.c_str(), 0600) || chown(staged.c_str(), 0, 0)) {
    unlink(staged.c_str());
    SetText(progress, "Staging failed", "Could not preserve the verified manager APK.");
    return false;
  }
  std::string code_path;
  const bool direct = StageDataApp(staged, package_name, release.size,
                                   progress, code_path);
  progress.value.store(92);
  const bool fallback = StageManagerFallback(
      staged, package_name, ProviderName(provider), release.size);
  if (!direct && !fallback) {
    unlink(staged.c_str());
    SetText(progress, "Installation failed",
            "Could not create an Android app directory or boot-time installer.");
    return false;
  }
  progress.value.store(100);
  SetText(progress, "Manager app ready", i18n::Format(
          direct ? "%s %s was placed in Android's app store. It will appear "
                   "after booting Android."
                 : "%s %s was staged. It will appear after booting Android.",
          ProviderName(provider), release.version.c_str()));
  return true;
}

std::string BlockForSlot(const std::string &slot) {
  return std::string("/dev/block/by-name/init_boot_") + (slot == "b" ? "b" : "a");
}

std::string Stamp() {
  time_t now = time(nullptr);
  struct tm local{};
  char text[32] = "unknown";
  if (localtime_r(&now, &local)) strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
  return text;
}

bool ExtractSuki(const std::string &archive, const std::string &kmi,
                 const std::string &output, std::string &error) {
  std::string listing;
  if (!RunCapture({"/system/bin/unzip", "-Z1", archive}, &listing)) {
    error = "Could not inspect the SukiSU module archive."; return false;
  }
  const auto entries = Lines(listing);
  const std::string expected = kmi + "_kernelsu.ko";
  if (entries.size() != 1 || entries.front() != expected ||
      expected.find("..") != std::string::npos || expected.front() == '/') {
    error = "The SukiSU archive did not contain exactly the expected KMI module.";
    return false;
  }
  const int fd = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) { error = "Could not create the extracted module."; return false; }
  if (!RunCapture({"/system/bin/unzip", "-p", archive, expected}, nullptr, fd) ||
      FileSize(output) < 64 * 1024 || FileSize(output) > 4 * 1024 * 1024) {
    unlink(output.c_str()); error = "The extracted SukiSU kernel module is invalid."; return false;
  }
  return true;
}

bool Patch(const Request &request, Progress &progress) {
  const Status device = request.verify_target_kernel ? ProbeSlot(request.slot) : Probe();
  if (!device.ksud_available || !device.kmi_supported || !device.init_boot_available) {
    SetText(progress, "Kernel unsupported", device.error); return false;
  }
  if (!device.storage_ready) {
    SetText(progress, "Unlock storage first",
            "A verified copy of init_boot must be saved before patching."); return false;
  }
  const std::string slot = request.slot == "b" ? "b" : "a";
  Release release = CachedRelease(request.provider);
  if (!release.available || release.kmi != device.kmi)
    release = LoadBundledRelease(request.provider, device.kmi);
  if (!release.available) {
    SetText(progress, "No compatible module", release.error); return false;
  }
  if (!EnsureDirectory(kCache) || !EnsureDirectory(kBackups) ||
      !EnsureDirectory(kReceipts) || !EnsureDirectory(kWork)) {
    SetText(progress, "Storage unavailable", "Could not create AERA/RootManager folders.");
    return false;
  }
  const std::string payload = release.bundled ? release.local_path :
      std::string(kCache) + "/" + release.asset_name;
  SetText(progress, release.bundled ? "Verifying bundled module" :
          "Downloading verified module", release.asset_name);
  if (!release.bundled && !HashFile(payload, release.size, release.sha256) &&
      !Download(release.asset_url, payload, kMaxAsset, progress, 18, 42,
                release.size, true)) {
    SetText(progress, "Download failed", "Could not download the matching official asset.");
    return false;
  }
  if (!HashFile(payload, release.size, release.sha256)) {
    if (!release.bundled) unlink(payload.c_str());
    SetText(progress, "Verification failed", release.bundled
        ? "The bundled release asset failed its SHA-256 integrity check."
        : "The release asset SHA-256 did not match GitHub.");
    return false;
  }
  progress.value.store(46);
  std::string module = payload;
  if (release.archive) {
    module = std::string(kCache) + "/" + device.kmi + "_sukisu.ko";
    std::string error;
    if (!ExtractSuki(payload, device.kmi, module, error)) {
      SetText(progress, "Archive rejected", error); return false;
    }
  }
  const std::string block = BlockForSlot(slot);
  const uint64_t partition_size = FileSize(block);
  if (partition_size < 1024 * 1024 || partition_size > 64 * 1024 * 1024) {
    SetText(progress, "init_boot unavailable", "The selected slot has an invalid partition size.");
    return false;
  }
  const std::string backup = std::string(kBackups) + "/init_boot_" + slot + "-" +
                             Stamp() + ".img";
  SetText(progress, i18n::Format("Backing up init_boot_%s", slot.c_str()),
          "A full rollback image is being saved before any write.");
  if (!CopyExact(block, backup, partition_size, progress, 46, 60)) {
    SetText(progress, "Backup failed", "Nothing was written to init_boot."); return false;
  }
  std::string backup_hash;
  if (!HashFile(backup, partition_size, "", &backup_hash)) {
    unlink(backup.c_str()); SetText(progress, "Backup verification failed"); return false;
  }
  unlink((std::string(kWork) + "/patched.img").c_str());
  SetText(progress, "Patching with ksud", i18n::Format(
          "%s %s • exact %s module", ProviderName(request.provider),
          release.version.c_str(), device.kmi.c_str()));
  progress.value.store(64);
  std::string patch_log;
  std::vector<std::string> patch_command{
      kKsud, "boot-patch", "-b", backup, "-m", module,
      "--partition", "init_boot", "--kmi", device.kmi,
      "-o", kWork, "--out-name", "patched.img"};
  // ksud 3.3 uses its built-in boot image patcher; older helpers need magiskboot.
  if (CaptureKsud({"boot-patch", "--help"}).find("--magiskboot") !=
      std::string::npos) {
    patch_command.insert(patch_command.end(), {"--magiskboot", kMagiskboot});
  }
  const bool patched = RunCapture(patch_command, &patch_log);
  const std::string image = std::string(kWork) + "/patched.img";
  if (!patched || FileSize(image) != partition_size) {
    SetText(progress, "ksud patch failed", Trim(patch_log)); return false;
  }
  std::string patched_hash;
  if (!HashFile(image, partition_size, "", &patched_hash) || patched_hash == backup_hash) {
    SetText(progress, "Patched image rejected",
            "The image was not a valid changed full-size init_boot image."); return false;
  }
  SetText(progress, i18n::Format("Flashing init_boot_%s", slot.c_str()),
          "Do not disconnect power. The verified backup remains on internal storage.");
  if (!CopyExact(image, block, partition_size, progress, 80, 98)) {
    SetText(progress, "Flash failed",
            "The write did not complete. Restore the saved backup before rebooting.");
    return false;
  }
  std::string flashed_hash;
  if (!VerifyBlockWrite(block, partition_size, patched_hash, &flashed_hash)) {
    SetText(progress, "Flash verification failed",
            "The partition readback did not match the patched image. Restore before rebooting.");
    return false;
  }
  Json::Value receipt;
  receipt["schema"] = 1;
  receipt["slot"] = slot;
  receipt["provider"] = ProviderName(request.provider);
  receipt["version"] = release.version;
  receipt["kmi"] = device.kmi;
  receipt["asset"] = release.asset_name;
  receipt["asset_sha256"] = release.sha256;
  receipt["original_sha256"] = backup_hash;
  receipt["patched_sha256"] = patched_hash;
  receipt["backup"] = backup;
  receipt["installed_at"] = Stamp();
  const std::string receipt_path = std::string(kReceipts) + "/init_boot_" + slot + ".json";
  const std::string receipt_temp = receipt_path + ".tmp";
  {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    const std::string text = Json::writeString(builder, receipt) + "\n";
    const int fd = open(receipt_temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
      const bool written = write(fd, text.data(), text.size()) ==
                           static_cast<ssize_t>(text.size()) && fsync(fd) == 0;
      close(fd);
      if (written) rename(receipt_temp.c_str(), receipt_path.c_str());
      else unlink(receipt_temp.c_str());
    }
  }
  progress.value.store(100);
  SetText(progress, "Root installed", i18n::Format(
          "%s %s was installed to init_boot_%s. Backup: %s",
          ProviderName(request.provider), release.version.c_str(),
          slot.c_str(), backup.c_str()));
  return true;
}

std::string LatestBackup(const std::string &slot) {
  DIR *directory = opendir(kBackups);
  if (!directory) return {};
  const std::string prefix = "init_boot_" + slot + "-";
  std::string latest;
  while (dirent *entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.compare(0, prefix.size(), prefix) == 0 &&
        name.size() > prefix.size() + 4 && name.substr(name.size() - 4) == ".img" &&
        name > latest) latest = name;
  }
  closedir(directory);
  return latest.empty() ? std::string() : std::string(kBackups) + "/" + latest;
}

bool Rollback(const Request &request, Progress &progress) {
  const std::string slot = request.slot == "b" ? "b" : "a";
  const std::string backup = LatestBackup(slot);
  const std::string block = BlockForSlot(slot);
  const uint64_t size = FileSize(block);
  if (backup.empty() || !size || FileSize(backup) != size) {
    SetText(progress, "No rollback image", i18n::Format(
        "No valid AERA init_boot_%s backup was found.", slot.c_str()));
    return false;
  }
  SetText(progress, i18n::Format("Restoring init_boot_%s", slot.c_str()),
          backup);
  if (!CopyExact(backup, block, size, progress, 5, 95)) {
    SetText(progress, "Restore failed", "The partition write did not complete."); return false;
  }
  std::string hash;
  if (!HashFile(backup, size, "", &hash) ||
      !VerifyBlockWrite(block, size, hash)) {
    SetText(progress, "Restore verification failed", "Partition readback differs from the backup.");
    return false;
  }
  unlink((std::string(kReceipts) + "/init_boot_" + slot + ".json").c_str());
  progress.value.store(100);
  SetText(progress, "Original init_boot restored", backup);
  return true;
}

std::vector<Module> LoadModules() {
  std::string text;
  if (!RunCapture({kKsud, "module", "list"}, &text)) return {};
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream input(text);
  if (!Json::parseFromStream(builder, input, &root, &errors) || !root.isArray()) return {};
  std::vector<Module> modules;
  for (const auto &item : root) {
    Module module;
    module.id = item["id"].asString();
    if (!SafeId(module.id)) continue;
    module.name = item.get("name", module.id).asString();
    module.version = item.get("version", "Unknown version").asString();
    module.version_code = JsonInteger(item["versionCode"]);
    module.author = item.get("author", "").asString();
    module.description = item.get("description", "").asString();
    module.enabled = JsonBoolean(item["enabled"], true);
    module.remove = JsonBoolean(item["remove"], false);
    module.update_json = item.get("updateJson", "").asString();
    modules.push_back(std::move(module));
  }
  std::lock_guard<std::mutex> lock(gMutex);
  for (auto &module : modules) {
    const auto old = std::find_if(gModules.begin(), gModules.end(), [&](const Module &candidate) {
      return candidate.id == module.id;
    });
    if (old != gModules.end()) {
      module.update_available = old->update_available;
      module.latest_version = old->latest_version;
      module.latest_version_code = old->latest_version_code;
      module.update_zip = old->update_zip;
      module.changelog = old->changelog;
    }
  }
  gModules = modules;
  return modules;
}

bool SafeArchiveListing(const std::string &zip) {
  std::string listing;
  if (!RunCapture({"/system/bin/unzip", "-Z1", zip}, &listing)) return false;
  const auto entries = Lines(listing);
  if (entries.empty() || entries.size() > 4096 ||
      std::find(entries.begin(), entries.end(), "module.prop") == entries.end()) return false;
  return std::all_of(entries.begin(), entries.end(), [](const std::string &entry) {
    return !entry.empty() && entry.size() <= 512 && entry.front() != '/' &&
           entry.find("../") == std::string::npos &&
           entry.find("/..") == std::string::npos;
  });
}

bool RefreshModules(Progress &progress) {
  auto modules = LoadModules();
  if (modules.empty()) {
    SetText(progress, "No modules found", "ksud did not report any installed modules.");
    progress.value.store(100); return true;
  }
  if (!EnsureDirectory(kWork)) return false;
  unsigned checked = 0;
  for (auto &module : modules) {
    if (progress.cancel.load()) return false;
    ++checked;
    progress.value.store(static_cast<unsigned>(checked * 100 / modules.size()));
    SetText(progress, "Checking module updates",
            i18n::Format("%s • %u/%zu", module.name.c_str(), checked,
                         modules.size()));
    if (!SafeHttps(module.update_json)) continue;
    const std::string metadata = std::string(kWork) + "/module-" + module.id + ".json";
    if (!Download(module.update_json, metadata, kMaxMetadata, progress,
                  progress.value.load(), progress.value.load(), 0, false)) continue;
    Json::Value root;
    if (!ReadJson(metadata, root) || !root.isObject()) continue;
    const int64_t code = JsonInteger(root["versionCode"]);
    const std::string zip = root.get("zipUrl", "").asString();
    if (code > module.version_code && SafeHttps(zip)) {
      module.update_available = true;
      module.latest_version_code = code;
      module.latest_version = root.get("version", "New version").asString();
      module.update_zip = zip;
      module.changelog = root.get("changelog", "").asString();
    } else {
      module.update_available = false;
    }
  }
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gModules = modules;
  }
  progress.value.store(100);
  const size_t updates = std::count_if(modules.begin(), modules.end(),
      [](const Module &module) { return module.update_available; });
  SetText(progress, "Module check complete",
          updates ? i18n::Format("%zu updates available", updates)
                  : "Everything is current");
  return true;
}

bool ModuleCommand(const Request &request, Progress &progress) {
  if (!SafeId(request.module_id)) return false;
  const char *command = request.job == Job::kEnableModule ? "enable" :
                        request.job == Job::kDisableModule ? "disable" : "uninstall";
  SetText(progress, std::string(command) + " module", request.module_id);
  std::string output;
  const bool ok = RunCapture({kKsud, "module", command, request.module_id}, &output);
  progress.value.store(100);
  SetText(progress, ok ? "Module updated" : "Module operation failed",
          ok ? request.module_id : Trim(output));
  if (ok) LoadModules();
  return ok;
}

bool UpdateModule(const Request &request, Progress &progress) {
  Module module;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto found = std::find_if(gModules.begin(), gModules.end(), [&](const Module &item) {
      return item.id == request.module_id;
    });
    if (found == gModules.end() || !found->update_available) return false;
    module = *found;
  }
  if (!SafeHttps(module.update_zip) || !EnsureDirectory(kCache)) return false;
  const std::string zip = std::string(kCache) + "/module-" + module.id + ".zip";
  SetText(progress, "Downloading module update", module.name + " " + module.latest_version);
  if (!Download(module.update_zip, zip, kMaxModuleZip, progress, 5, 75, 0, false) ||
      !SafeArchiveListing(zip)) {
    unlink(zip.c_str());
    SetText(progress, "Module update rejected",
            "Download failed or the ZIP is not a safe KernelSU module archive.");
    return false;
  }
  SetText(progress, "Installing with ksud", module.name);
  progress.value.store(82);
  std::string output;
  const bool ok = RunCapture({kKsud, "module", "install", zip}, &output);
  progress.value.store(100);
  SetText(progress, ok ? "Module update installed" : "Module installation failed",
          ok ? i18n::Format("%s will be active after reboot.",
                            module.name.c_str())
             : Trim(output));
  if (ok) LoadModules();
  return ok;
}

}  // namespace

const char *ProviderName(Provider provider) {
  switch (provider) {
    case Provider::kKernelSU: return "KernelSU";
    case Provider::kKernelSUNext: return "KernelSU Next";
    case Provider::kSukiSU: return "SukiSU Ultra";
  }
  return "KernelSU";
}

Status Probe() {
  Status status;
  if (!Enabled()) {
    status.error = "Root Manager is disabled in this build.";
    return status;
  }
  status.ksud_available = access(kKsud, X_OK) == 0 && access(kMagiskboot, X_OK) == 0;
  utsname name{};
  if (uname(&name) == 0) status.kernel_release = name.release;
  if (!status.ksud_available) {
    status.error = "This AERA build does not contain ksud and magiskboot."; return status;
  }
  status.kmi = CaptureKsud({"boot-info", "current-kmi"});
  const auto supported = Lines(CaptureKsud({"boot-info", "supported-kmis"}));
  status.kmi_supported = !status.kmi.empty() &&
      std::find(supported.begin(), supported.end(), status.kmi) != supported.end();
  std::string suffix = CaptureKsud({"boot-info", "slot-suffix"});
  status.slot = suffix.find('b') != std::string::npos ? "b" : "a";
  const auto partitions = Lines(CaptureKsud({"boot-info", "available-partitions"}));
  status.init_boot_available =
      std::find(partitions.begin(), partitions.end(), "init_boot") != partitions.end() &&
      access(BlockForSlot(status.slot).c_str(), R_OK | W_OK) == 0;
  status.storage_ready = access("/sdcard", R_OK | W_OK) == 0 &&
                         EnsureDirectory(kRoot);
  if (!status.kmi_supported)
    status.error = "Exact KMI " + (status.kmi.empty() ? std::string("unknown") : status.kmi) +
                   " is not supported. AERA will not guess another module.";
  else if (!status.init_boot_available)
    status.error = "This device has no writable init_boot for the active slot.";
  else if (!status.storage_ready)
    status.error = "Unlock internal storage so AERA can make a rollback backup.";
  return status;
}

Status ProbeSlot(const std::string &slot) {
  Status device = Probe();
  device.kmi_supported = false;
  device.kmi.clear();
  if (slot != "a" && slot != "b") {
    device.error = "Select slot A or B.";
    return device;
  }
  device.slot = slot;
  device.init_boot_available = access(BlockForSlot(slot).c_str(), R_OK | W_OK) == 0;
  const std::string boot = "/dev/block/by-name/boot_" + slot;
  const std::string work = std::string(kWork) + "/target-" + slot;
  const uint64_t bytes = FileSize(boot);
  if (!device.ksud_available || bytes < 1024 * 1024 || bytes > 256 * 1024 * 1024 ||
      !EnsureDirectory(kWork) || !EnsureDirectory(work)) {
    device.error = "The target slot kernel cannot be inspected on this device.";
    return device;
  }
  // magiskboot extracts and decompresses the target kernel, without writing a partition.
  const std::string kernel = work + "/kernel";
  struct Cleanup {
    std::string directory;
    ~Cleanup() {
      for (const char *file : {"kernel", "ramdisk.cpio", "init_boot.cpio", "dtb", "kernel_dtb",
                               "header", "bootconfig", "second", "extra", "recovery_dtbo"})
        unlink((directory + "/" + file).c_str());
      rmdir(directory.c_str());
    }
  } cleanup{work};
  unlink(kernel.c_str());
  std::string output;
  if (!RunCapture({kMagiskboot, "unpack", boot}, &output, -1, work.c_str())) {
    device.error = "Could not unpack the target boot image. No partition was changed.";
    return device;
  }
  const int fd = open(kernel.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  std::string tail;
  std::array<char, 65536> buffer{};
  uint64_t read_bytes = 0;
  if (fd >= 0) {
    while (read_bytes < 256 * 1024 * 1024) {
      const ssize_t count = read(fd, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      read_bytes += static_cast<uint64_t>(count);
      tail.append(buffer.data(), static_cast<size_t>(count));
      device.kmi = KernelKmiFromBanner(tail);
      if (!device.kmi.empty()) break;
      if (tail.size() > 256) tail.erase(0, tail.size() - 256);
    }
    close(fd);
  }
  const auto supported = Lines(CaptureKsud({"boot-info", "supported-kmis"}));
  device.kmi_supported = !device.kmi.empty() &&
      std::find(supported.begin(), supported.end(), device.kmi) != supported.end();
  device.error = !device.kmi_supported ? "The target kernel KMI could not be verified or is unsupported." :
      !device.init_boot_available ? "The target slot has no writable init_boot." :
      !device.storage_ready ? "Unlock internal storage for the rollback backup." : "";
  return device;
}

PatchInfo InspectSlot(const std::string &requested_slot) {
  PatchInfo info;
  const std::string slot = requested_slot == "b" ? "b" : "a";
  const std::string block = BlockForSlot(slot);
  const uint64_t size = FileSize(block);
  if (size < 1024 * 1024 || size > 64 * 1024 * 1024) {
    info.detail = i18n::Format("init_boot_%s is unavailable.", slot.c_str());
    return info;
  }

  const std::string receipt_path = std::string(kReceipts) + "/init_boot_" + slot + ".json";
  Json::Value receipt;
  if (ReadJson(receipt_path, receipt) && receipt.isObject() &&
      receipt["schema"].asUInt() == 1 && receipt["slot"].asString() == slot) {
    const std::string expected = receipt["patched_sha256"].asString();
    std::string actual;
    if (expected.size() == 64 && HashFile(block, size, expected, &actual)) {
      info.inspected = true;
      info.patched = true;
      info.aera_verified = true;
      info.provider = receipt["provider"].asString();
      info.version = receipt["version"].asString();
      info.kmi = receipt["kmi"].asString();
      info.detail = i18n::Format("Verified AERA patch • %s %s • %s",
          info.provider.c_str(), info.version.c_str(), info.kmi.c_str());
      return info;
    }
  }

  const std::string directory = std::string(kWork) + "/inspect-" + slot;
  if (!EnsureDirectory(kWork) || !EnsureDirectory(directory)) {
    info.detail =
        i18n::Format("Could not inspect init_boot_%s.", slot.c_str());
    return info;
  }
  const std::string image = directory + "/init_boot.img";
  const std::string cpio = directory + "/ramdisk.cpio";
  const std::string module = directory + "/kernelsu.ko";
  unlink(image.c_str()); unlink(cpio.c_str()); unlink(module.c_str());
  Progress copy;
  if (!CopyExact(block, image, size, copy, 0, 1)) {
    info.detail = i18n::Format("Could not read init_boot_%s.", slot.c_str());
    return info;
  }
  std::string unpack;
  if (!RunCapture({kMagiskboot, "unpack", "init_boot.img"}, &unpack, -1,
                  directory.c_str())) {
    info.detail =
        i18n::Format("init_boot_%s could not be unpacked.", slot.c_str());
    return info;
  }
  std::string listing;
  if (!RunCapture({kMagiskboot, "cpio", "ramdisk.cpio", "ls"}, &listing, -1,
                  directory.c_str())) {
    info.detail = i18n::Format("Could not inspect init_boot_%s.", slot.c_str());
    return info;
  }
  info.inspected = true;
  if (listing.find("kernelsu.ko") == std::string::npos) {
    info.detail = "Stock / no KernelSU LKM detected";
    return info;
  }
  info.patched = true;
  info.provider = "KernelSU-compatible";
  info.detail = "Patched LKM detected • provider/version not recorded by AERA";
  std::string extract;
  if (RunCapture({kMagiskboot, "cpio", "ramdisk.cpio", "extract"}, &extract, -1,
                 directory.c_str())) {
    int fd = open(module.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      const uint64_t module_size = FileSize(module);
      std::string bytes(static_cast<size_t>(std::min<uint64_t>(module_size, 4 * 1024 * 1024)), '\0');
      size_t done = 0;
      while (done < bytes.size()) {
        const ssize_t count = read(fd, bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += static_cast<size_t>(count);
      }
      close(fd);
      std::string lower = bytes;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (lower.find("sukisu") != std::string::npos) info.provider = "SukiSU Ultra";
      else if (lower.find("kernelsu next") != std::string::npos ||
               lower.find("kernelsu-next") != std::string::npos ||
               lower.find("ksu_next") != std::string::npos)
        info.provider = "KernelSU Next";
      else if (lower.find("description=android kernelsu") != std::string::npos)
        info.provider = "KernelSU-compatible";
      info.detail = "Patched LKM detected • " + info.provider +
                    " • exact version unavailable (no AERA receipt)";
    }
  }
  return info;
}

Release CachedRelease(Provider provider) {
  std::lock_guard<std::mutex> lock(gMutex);
  const auto found = gReleases.find(provider);
  return found == gReleases.end() ? Release{} : found->second;
}

Release BundledRelease(Provider provider, const std::string &kmi) {
  Release release = LoadBundledRelease(provider, kmi);
  if (release.available) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto found = gReleases.find(provider);
    if (found == gReleases.end() || !found->second.available || found->second.kmi != kmi ||
        found->second.bundled) gReleases[provider] = release;
  }
  return release;
}

std::vector<Module> InstalledModules() {
  return LoadModules();
}

bool PatchUpdateAvailable(const PatchInfo &patch, Provider provider,
                          const Release &release) {
  return patch.patched && patch.aera_verified && release.available &&
      patch.provider == ProviderName(provider) && !patch.version.empty() &&
      !patch.kmi.empty() && patch.kmi == release.kmi &&
      CompareVersions(release.version, patch.version) > 0;
}

ManagerStatus InspectManager(Provider provider) {
  ManagerStatus status;
  status.package_name = ManagerPackage(provider);
  if (status.package_name.empty()) {
    status.detail = "Unknown root provider.";
    return status;
  }
  if (access("/data/system/packages.xml", R_OK)) {
    status.detail = "Unlock data to inspect the Android manager app.";
    return status;
  }
  status.installed = PackageListed(status.package_name);
  const std::string staged_apk = ManagerStagingPath(provider);
  const bool has_staged_apk = access(staged_apk.c_str(), R_OK) == 0;
  const bool installer_pending =
      has_staged_apk && ManagerInstallerPending(provider);
  const bool removal_pending = ManagerRemovalPending(provider);
  if (status.installed) {
    // The boot-time installer normally removes this after pm succeeds.  Also
    // clean it here so an interrupted or older installer cannot leave a stale
    // APK that makes recovery claim another installation is pending.
    if (installer_pending)
      WriteText(std::string(kManagerModule) + "/remove", "", 0600);
    if (has_staged_apk) unlink(staged_apk.c_str());
    status.removal_pending = removal_pending;
    status.detail = removal_pending
        ? i18n::Format("%s will be uninstalled when Android boots.",
                       ProviderName(provider))
        : i18n::Format("%s is installed in Android.", ProviderName(provider));
  } else if (installer_pending) {
    status.staged = true;
    status.detail = i18n::Format(
        "%s is ready and will be installed when Android boots.",
        ProviderName(provider));
  } else {
    // An APK without its matching active one-shot installer cannot be
    // installed on boot.  Remove the orphan instead of presenting it as ready.
    if (has_staged_apk) unlink(staged_apk.c_str());
    if (removal_pending) {
      unlink(ManagerRemovalMarker(provider).c_str());
      WriteText(std::string(kManagerModule) + "/remove", "", 0600);
    }
    status.detail = i18n::Format(
        "%s is not installed. Download the official manager app from GitHub.",
        ProviderName(provider));
  }
  return status;
}

bool Run(const Request &request, Progress &progress) {
  if (!Enabled()) {
    SetText(progress, "Root Manager is disabled in this build.");
    return false;
  }
  std::unique_lock<std::mutex> operation(gOperationMutex, std::try_to_lock);
  if (!operation.owns_lock()) {
    SetText(progress, "Root operation already running");
    return false;
  }
  progress.value.store(0); progress.downloaded.store(0); progress.total.store(0);
  aera::shared_storage::RepairOnExit shared_permissions(kRoot);
  switch (request.job) {
    case Job::kRefreshRelease: {
      Release release;
      const bool ok = FetchRelease(request.provider,
          request.verify_target_kernel ? ProbeSlot(request.slot) : Probe(), progress, release);
      if (!ok) SetText(progress, "No compatible release", release.error);
      else { progress.value.store(100); SetText(progress, "Ready to patch",
          i18n::Format("%s %s • online release • %s",
              ProviderName(request.provider), release.version.c_str(),
              release.asset_name.c_str())); }
      return ok;
    }
    case Job::kInspectTarget: {
      const Status device = ProbeSlot(request.slot);
      if (!device.error.empty()) { SetText(progress, "Target unavailable", device.error); return false; }
      const Release release = BundledRelease(request.provider, device.kmi);
      const PatchInfo patch = InspectSlot(request.slot);
      progress.has_inspection = true;
      progress.inspected_target = device;
      progress.inspected_patch = patch;
      progress.inspected_offline = release;
      SetText(progress, "Target inspected", "init_boot_" + request.slot + " / " + device.kmi +
          "\n" + patch.detail + "\n" + (release.available ?
          std::string(ProviderName(request.provider)) + " " + release.version + " (offline module available)" :
          "No matching offline module. Check the online release before patching."));
      progress.value.store(100);
      return true;
    }
    case Job::kPatch: return Patch(request, progress);
    case Job::kRollback: return Rollback(request, progress);
    case Job::kRefreshModules: return RefreshModules(progress);
    case Job::kEnableModule:
    case Job::kDisableModule:
    case Job::kRemoveModule: return ModuleCommand(request, progress);
    case Job::kUpdateModule: return UpdateModule(request, progress);
    case Job::kInstallManager: return InstallManager(request.provider, progress);
    case Job::kUninstallManager:
      return StageManagerRemoval(request.provider, progress);
  }
  return false;
}

}  // namespace aeraui::root
