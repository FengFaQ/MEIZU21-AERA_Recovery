/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "update_manager.hpp"
#include "../../../aera_shared_storage.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <json/json.h>
#include <mutex>
#include <openssl/evp.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <cutils/properties.h>

#include <aeraui/backend.hpp>

namespace aeraui::update {
namespace {

constexpr char kLogTag[] = "AERAUpdate";
constexpr char kStableCatalogUrl[] =
    "https://aera-recovery.com/catalog.json";
constexpr char kNightlyCatalogUrl[] =
    "https://aera-recovery.com/catalog-nightly.json";
constexpr char kCatalogPath[] = "/tmp/aera-update-catalog.json";
constexpr char kUpdateRoot[] = "/sdcard/AERA/Updates";
constexpr uint64_t kMaxCatalog = 1024 * 1024;
constexpr uint64_t kMaxPackage = 512ULL * 1024 * 1024;

std::mutex gMutex;
Snapshot gSnapshot;
std::atomic<bool> gCancel{false};
std::atomic<uint64_t> gDownloaded{0};
std::atomic<uint64_t> gTotal{0};
std::atomic<unsigned> gProgress{0};

std::string Property(const char *key) {
  char value[PROPERTY_VALUE_MAX] = {};
  property_get(key, value, "");
  return value;
}

uint64_t ParseUnsigned(const std::string &text) {
  if (text.empty()) return 0;
  char *end = nullptr;
  errno = 0;
  const unsigned long long value = strtoull(text.c_str(), &end, 10);
  return errno == 0 && end != text.c_str() && *end == '\0'
             ? static_cast<uint64_t>(value) : 0;
}

uint64_t JsonUnsigned(const Json::Value &value) {
  if (value.isUInt64()) return value.asUInt64();
  if (value.isInt64() && value.asInt64() > 0)
    return static_cast<uint64_t>(value.asInt64());
  if (value.isString()) return ParseUnsigned(value.asString());
  return 0;
}

bool SafeDevice(const std::string &value) {
  return !value.empty() && value.size() <= 96 &&
      std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
      });
}

bool SafeFilename(const std::string &value) {
  return !value.empty() && value.size() <= 160 &&
      value.find('/') == std::string::npos &&
      value.find('\\') == std::string::npos &&
      value.find("..") == std::string::npos && value.size() > 4 &&
      value.compare(value.size() - 4, 4, ".zip") == 0;
}

bool SafeHttps(const std::string &value) {
  return value.size() >= 12 && value.size() <= 2048 &&
      value.compare(0, 8, "https://") == 0 &&
      std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7f;
      });
}

bool SafeSha256(const std::string &value) {
  return value.size() == 64 &&
      std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c);
      });
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
    struct stat info {};
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

void SetPhase(Phase phase, const std::string &message, bool available) {
  std::lock_guard<std::mutex> lock(gMutex);
  gSnapshot.phase = phase;
  gSnapshot.message = message;
  gSnapshot.available = available;
}

bool DownloadFile(const std::string &url, const std::string &path,
                  uint64_t maximum, uint64_t expected_size,
                  unsigned progress_start, unsigned progress_end) {
  if (!SafeHttps(url) || maximum == 0) return false;
  const char *busybox = access("/sbin/busybox", X_OK) == 0
                            ? "/sbin/busybox" : "/system/bin/busybox";
  if (access(busybox, X_OK) != 0) return false;

  for (unsigned attempt = 0; attempt < 2 && !gCancel.load(); ++attempt) {
    unlink(path.c_str());
    int completion[2];
    if (pipe(completion) != 0) return false;
    const pid_t child = fork();
    if (child < 0) {
      close(completion[0]);
      close(completion[1]);
      return false;
    }
    if (child == 0) {
      close(completion[0]);
      const int null_fd = open("/dev/null", O_RDWR);
      if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
          dup2(null_fd, STDOUT_FILENO) < 0 ||
          dup2(null_fd, STDERR_FILENO) < 0)
        _exit(126);
      if (null_fd > STDERR_FILENO) close(null_fd);
      rlimit limit{maximum, maximum};
      if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(126);
      umask(077);
      const char *argv[] = {busybox, "wget", "-q", "-T", "20", "-t", "2",
                            "-O", path.c_str(), url.c_str(), nullptr};
      execv(busybox, const_cast<char *const *>(argv));
      _exit(127);
    }

    close(completion[1]);
    pollfd event{completion[0], POLLIN | POLLHUP, 0};
    timespec started {};
    clock_gettime(CLOCK_MONOTONIC, &started);
    const uint64_t timeout_ms = maximum <= kMaxCatalog ? 30000 : 900000;
    bool timed_out = false;
    while (!gCancel.load()) {
      const int result = poll(&event, 1, 100);
      if (result < 0 && errno == EINTR) continue;
      if (result < 0 ||
          (event.revents & (POLLHUP | POLLERR | POLLNVAL)))
        break;
      timespec now {};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const int64_t elapsed =
          static_cast<int64_t>(now.tv_sec - started.tv_sec) * 1000 +
          static_cast<int64_t>(now.tv_nsec - started.tv_nsec) / 1000000;
      if (elapsed >= static_cast<int64_t>(timeout_ms)) {
        timed_out = true;
        kill(child, SIGKILL);
        break;
      }
      if (expected_size != 0) {
        struct stat info {};
        if (lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_size >= 0) {
          const uint64_t amount = static_cast<uint64_t>(info.st_size);
          gDownloaded.store(amount);
          gProgress.store(progress_start + static_cast<unsigned>(
              std::min(amount, expected_size) *
              (progress_end - progress_start) / expected_size));
        }
      }
    }
    if (gCancel.load()) kill(child, SIGKILL);
    close(completion[0]);
    int status = 0;
    pid_t waited;
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const bool exited =
        (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
        (waited < 0 && errno == ECHILD);
    struct stat info {};
    const bool valid = !timed_out && exited &&
        lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size > 0 && static_cast<uint64_t>(info.st_size) <= maximum &&
        (expected_size == 0 ||
         static_cast<uint64_t>(info.st_size) == expected_size);
    if (valid) {
      if (expected_size != 0) {
        gDownloaded.store(expected_size);
        gProgress.store(progress_end);
      }
      return true;
    }
  }
  unlink(path.c_str());
  return false;
}

bool ReadCatalog(Json::Value *root) {
  int fd = open(kCatalogPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info {};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > kMaxCatalog) {
    close(fd);
    return false;
  }
  std::string text(static_cast<size_t>(info.st_size), '\0');
  size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t count = read(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      close(fd);
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  close(fd);
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::string errors;
  std::istringstream input(text);
  return Json::parseFromStream(builder, input, root, &errors);
}

bool HashFile(const std::string &path, uint64_t expected_size,
              const std::string &expected_hash) {
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info {};
  if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size <= 0 ||
      static_cast<uint64_t>(info.st_size) != expected_size) {
    if (fd >= 0) close(fd);
    return false;
  }
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  bool ok = context != nullptr &&
      EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  std::array<unsigned char, 65536> buffer {};
  uint64_t done = 0;
  while (ok) {
    if (gCancel.load()) {
      ok = false;
      break;
    }
    const ssize_t count = read(fd, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      ok = false;
      break;
    }
    if (count == 0) break;
    ok = EVP_DigestUpdate(context, buffer.data(),
                          static_cast<size_t>(count)) == 1;
    done += static_cast<uint64_t>(count);
    gProgress.store(90 + static_cast<unsigned>(
        std::min(done, expected_size) * 10 / expected_size));
  }
  close(fd);
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
  unsigned length = 0;
  ok = ok && done == expected_size &&
      EVP_DigestFinal_ex(context, digest.data(), &length) == 1 &&
      length == 32;
  EVP_MD_CTX_free(context);
  constexpr char hex[] = "0123456789abcdef";
  std::string actual;
  actual.reserve(64);
  for (unsigned i = 0; ok && i < length; ++i) {
    actual.push_back(hex[digest[i] >> 4]);
    actual.push_back(hex[digest[i] & 15]);
  }
  std::string expected = expected_hash;
  std::transform(expected.begin(), expected.end(), expected.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return ok && actual == expected;
}

std::string DeviceName() {
  for (const char *key : {"ro.product.device", "ro.build.product",
                          "ro.product.vendor.device"}) {
    const std::string value = Property(key);
    if (SafeDevice(value)) return value;
  }
  return {};
}

uint64_t LocalBuildTime() {
  for (const char *key : {"ro.build.date.utc_aera",
                          "ro.bootimage.build.date.utc_aera",
                          "ro.build.date.utc"}) {
    const uint64_t value = ParseUnsigned(Property(key));
    if (value != 0) return value;
  }
  return 0;
}

bool ParseRelease(const Json::Value &value, Release *release) {
  if (!value.isObject()) return false;
  release->version = value["version"].asString();
  release->build_type = value.get("build_type", "").asString();
  release->build_time = JsonUnsigned(value["build_time"]);
  release->filename = value["filename"].asString();
  release->size = JsonUnsigned(value["size"]);
  release->sha256 = value["sha256"].asString();
  release->url = value.get("ota_url", "").asString();
  if (release->url.empty()) release->url = value["url"].asString();
  release->changelog.clear();
  const Json::Value &notes = value["changelog"];
  if (notes.isArray()) {
    for (Json::ArrayIndex i = 0; i < notes.size() && i < 32; ++i) {
      if (notes[i].isString() && notes[i].asString().size() <= 512)
        release->changelog.push_back(notes[i].asString());
    }
  }
  return !release->version.empty() && release->version.size() <= 64 &&
      release->build_time != 0 && SafeFilename(release->filename) &&
      release->size != 0 && release->size <= kMaxPackage &&
      SafeSha256(release->sha256) && SafeHttps(release->url);
}

}  // namespace

const char *CatalogUrl() {
  return GetChannel() == Channel::kNightly
      ? kNightlyCatalogUrl : kStableCatalogUrl;
}

Channel GetChannel() {
  return RecoveryPreference(Preference::kUpdateNightly)
      ? Channel::kNightly : Channel::kStable;
}

bool SetChannel(Channel channel) {
  const bool nightly = channel == Channel::kNightly;
  if (GetChannel() == channel) return true;
  if (!RecoverySetPreference(Preference::kUpdateNightly, nightly) ||
      !RecoverySavePreferences()) {
    RecoverySetPreference(Preference::kUpdateNightly, !nightly);
    return false;
  }
  std::lock_guard<std::mutex> lock(gMutex);
  gSnapshot.phase = Phase::kIdle;
  gSnapshot.release = {};
  gSnapshot.package_path.clear();
  gSnapshot.message.clear();
  gSnapshot.checked = false;
  gSnapshot.available = false;
  gDownloaded.store(0);
  gTotal.store(0);
  gProgress.store(0);
  return true;
}

Snapshot GetSnapshot() {
  std::lock_guard<std::mutex> lock(gMutex);
  if (gSnapshot.device.empty()) gSnapshot.device = DeviceName();
  if (gSnapshot.local_build_time == 0)
    gSnapshot.local_build_time = LocalBuildTime();
  Snapshot result = gSnapshot;
  result.downloaded = gDownloaded.load();
  result.total = gTotal.load();
  result.progress = gProgress.load();
  return result;
}

bool Check() {
  gCancel.store(false);
  gDownloaded.store(0);
  gTotal.store(0);
  gProgress.store(2);
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gSnapshot.phase = Phase::kChecking;
    gSnapshot.device = DeviceName();
    gSnapshot.local_build_time = LocalBuildTime();
    gSnapshot.message = "Contacting the AERA update service";
    gSnapshot.checked = false;
    if (!gSnapshot.available) gSnapshot.release = {};
    gSnapshot.package_path.clear();
  }

  unlink(kCatalogPath);
  const char *catalog_url = CatalogUrl();
  if (!DownloadFile(catalog_url, kCatalogPath, kMaxCatalog, 0, 2, 25)) {
    SetPhase(Phase::kError, "Could not reach the update service", false);
    return false;
  }
  gProgress.store(40);

  Json::Value root;
  if (!ReadCatalog(&root) || root.get("schema", 0).asInt() != 1 ||
      !root["devices"].isObject()) {
    unlink(kCatalogPath);
    SetPhase(Phase::kError, "The update catalog is invalid", false);
    return false;
  }

  const std::string device = DeviceName();
  const uint64_t local = LocalBuildTime();
  if (!SafeDevice(device) || local == 0) {
    unlink(kCatalogPath);
    SetPhase(Phase::kError, "AERA could not identify this recovery build", false);
    return false;
  }
  const Json::Value &entry = root["devices"][device];
  Release release;
  if (entry.isNull()) {
    unlink(kCatalogPath);
    std::lock_guard<std::mutex> lock(gMutex);
    gSnapshot.phase = Phase::kUpToDate;
    gSnapshot.device = device;
    gSnapshot.local_build_time = local;
    gSnapshot.message = "No update channel is published for this device";
    gSnapshot.checked = true;
    gSnapshot.available = false;
    gProgress.store(100);
    return true;
  }
  if (!ParseRelease(entry, &release)) {
    unlink(kCatalogPath);
    SetPhase(Phase::kError, "The device entry in the catalog is invalid", false);
    return false;
  }
  unlink(kCatalogPath);

  const bool available = release.build_time > local;
  const uint64_t remote = release.build_time;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gSnapshot.phase = available ? Phase::kAvailable : Phase::kUpToDate;
    gSnapshot.device = device;
    gSnapshot.local_build_time = local;
    gSnapshot.release = std::move(release);
    gSnapshot.message =
        available ? "A newer recovery build is available"
                  : "This recovery is up to date";
    gSnapshot.checked = true;
    gSnapshot.available = available;
  }
  gProgress.store(100);
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "%s catalog checked for %s: local=%llu remote=%llu available=%d",
                      GetChannel() == Channel::kNightly ? "nightly" : "stable",
                      device.c_str(), static_cast<unsigned long long>(local),
                      static_cast<unsigned long long>(remote),
                      available ? 1 : 0);
  return true;
}

bool PrepareDownload() {
  std::lock_guard<std::mutex> lock(gMutex);
  if (!gSnapshot.available || gSnapshot.release.size == 0) return false;
  gCancel.store(false);
  gDownloaded.store(0);
  gTotal.store(gSnapshot.release.size);
  gProgress.store(0);
  gSnapshot.phase = Phase::kDownloading;
  gSnapshot.message = "Starting recovery download";
  gSnapshot.package_path.clear();
  return true;
}

bool Download() {
  aera::shared_storage::RepairOnExit shared_permissions(kUpdateRoot);
  Snapshot snapshot = GetSnapshot();
  if (!snapshot.available || snapshot.release.size == 0) {
    SetPhase(Phase::kError, "No recovery update is ready to download", false);
    return false;
  }
  if (!EnsureDirectory(kUpdateRoot)) {
    SetPhase(Phase::kError,
             "Internal storage is unavailable. Unlock data and try again.",
             true);
    return false;
  }

  if (snapshot.phase != Phase::kDownloading && !PrepareDownload()) {
    SetPhase(Phase::kError, "The recovery download could not be started", false);
    return false;
  }
  const std::string final_path =
      std::string(kUpdateRoot) + "/" + snapshot.release.filename;
  const std::string partial_path = final_path + ".part";
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gSnapshot.phase = Phase::kDownloading;
    gSnapshot.message = "Downloading " + snapshot.release.filename;
    gSnapshot.package_path.clear();
  }

  if (HashFile(final_path, snapshot.release.size, snapshot.release.sha256)) {
    std::lock_guard<std::mutex> lock(gMutex);
    gSnapshot.phase = Phase::kReady;
    gSnapshot.message = "The verified update package is ready";
    gSnapshot.package_path = final_path;
    gProgress.store(100);
    gDownloaded.store(snapshot.release.size);
    return true;
  }
  gProgress.store(0);
  gDownloaded.store(0);

  const uint64_t limit = snapshot.release.size < kMaxPackage
      ? snapshot.release.size + 1 : kMaxPackage;
  if (!DownloadFile(snapshot.release.url, partial_path, limit,
                    snapshot.release.size, 0, 90)) {
    SetPhase(Phase::kError, "The recovery package could not be downloaded", true);
    return false;
  }
  SetPhase(Phase::kVerifying, "Verifying the downloaded package", true);
  if (!HashFile(partial_path, snapshot.release.size, snapshot.release.sha256)) {
    unlink(partial_path.c_str());
    SetPhase(Phase::kError,
             "The downloaded package does not match its SHA-256", true);
    return false;
  }
  unlink(final_path.c_str());
  if (rename(partial_path.c_str(), final_path.c_str()) != 0) {
    unlink(partial_path.c_str());
    SetPhase(Phase::kError, "The verified package could not be saved", true);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gSnapshot.phase = Phase::kReady;
    gSnapshot.message = "The verified update package is ready";
    gSnapshot.package_path = final_path;
  }
  gProgress.store(100);
  return true;
}

void SetOffline() {
  std::lock_guard<std::mutex> lock(gMutex);
  gSnapshot.phase = Phase::kError;
  gSnapshot.message = "Connect to Wi-Fi and try again";
}

void Cancel() {
  gCancel.store(true);
}

void MarkInstalled() {
  std::lock_guard<std::mutex> lock(gMutex);
  gSnapshot.phase = Phase::kUpToDate;
  gSnapshot.message = "The recovery update was installed";
  gSnapshot.available = false;
  gSnapshot.checked = true;
}

}  // namespace aeraui::update
