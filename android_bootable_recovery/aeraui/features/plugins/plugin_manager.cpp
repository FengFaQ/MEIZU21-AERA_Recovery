/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "plugin_manager.hpp"
#include "../../../aera_shared_storage.hpp"
#include "store_metadata.hpp"

#include <aeraui/i18n.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <json/json.h>
#include <limits>
#include <mutex>
#include <openssl/evp.h>
#include <poll.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ziparchive/zip_archive.h>

namespace aeraui::plugins {
namespace {

constexpr char kCatalogUrl[] =
    "https://raw.githubusercontent.com/AERA-Plugins/registry/main/catalog.json";
constexpr char kCatalogSignatureUrl[] =
    "https://raw.githubusercontent.com/AERA-Plugins/registry/main/catalog.json.sig";
constexpr char kCacheRoot[] = "/tmp/aera-plugin-store";
constexpr char kStorageRoot[] = "/sdcard/AERA/plugins";
constexpr char kPersistentCacheRoot[] = "/sdcard/AERA/plugins/.store";
constexpr char kMemoryRoot[] = "/tmp/aera/plugins";
constexpr uint64_t kMaxCatalog = 1024 * 1024;
constexpr uint64_t kMaxManifest = 64 * 1024;
constexpr uint64_t kMaxSignature = 4096;
constexpr uint64_t kMaxPayload = 512ULL * 1024 * 1024;
constexpr uint64_t kMaxPackage = kMaxPayload + 1024 * 1024;
constexpr std::array<uint8_t, 32> kSigningKey{{
    0x16, 0xee, 0x36, 0x7d, 0xd2, 0xd7, 0xae, 0x19,
    0xa0, 0xd1, 0x62, 0xfb, 0x71, 0x66, 0xe0, 0xf7,
    0x69, 0x2d, 0xe0, 0xed, 0x28, 0x79, 0x4d, 0x44,
    0x5e, 0xed, 0xad, 0x08, 0x6e, 0x48, 0x73, 0xe9,
}};

std::mutex gCatalogMutex;
std::vector<Plugin> gCatalog;
std::string gCatalogSource;
std::string gCatalogLanguage;

bool ParseVersion(const std::string &version, std::vector<uint64_t> &parts,
                  std::string &prerelease) {
  size_t offset = (!version.empty() &&
                   (version.front() == 'v' || version.front() == 'V')) ? 1 : 0;
  if (offset == version.size()) return false;
  while (offset < version.size()) {
    if (!std::isdigit(static_cast<unsigned char>(version[offset]))) return false;
    uint64_t value = 0;
    do {
      const unsigned digit = static_cast<unsigned>(version[offset] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
        return false;
      value = value * 10 + digit;
      ++offset;
    } while (offset < version.size() &&
             std::isdigit(static_cast<unsigned char>(version[offset])));
    parts.push_back(value);
    if (parts.size() > 8) return false;
    if (offset == version.size() || version[offset] == '-' ||
        version[offset] == '+') break;
    if (version[offset++] != '.' || offset == version.size()) return false;
  }
  if (offset < version.size() && version[offset] == '-') {
    const size_t end = version.find('+', offset + 1);
    prerelease = version.substr(offset + 1, end - offset - 1);
    if (prerelease.empty()) return false;
    offset = end == std::string::npos ? version.size() : end;
  }
  if (offset < version.size() && version[offset] == '+') {
    if (++offset == version.size()) return false;
    offset = version.size();
  }
  return offset == version.size() && !parts.empty();
}

int CompareVersions(const std::string &left, const std::string &right) {
  std::vector<uint64_t> left_parts;
  std::vector<uint64_t> right_parts;
  std::string left_prerelease;
  std::string right_prerelease;
  if (!ParseVersion(left, left_parts, left_prerelease) ||
      !ParseVersion(right, right_parts, right_prerelease)) return 0;
  const size_t count = std::max(left_parts.size(), right_parts.size());
  for (size_t index = 0; index < count; ++index) {
    const uint64_t lhs = index < left_parts.size() ? left_parts[index] : 0;
    const uint64_t rhs = index < right_parts.size() ? right_parts[index] : 0;
    if (lhs != rhs) return lhs > rhs ? 1 : -1;
  }
  if (left_prerelease.empty() != right_prerelease.empty())
    return left_prerelease.empty() ? 1 : -1;
  if (left_prerelease == right_prerelease) return 0;
  return left_prerelease > right_prerelease ? 1 : -1;
}

bool SafeId(const std::string &id) {
  if (id.empty() || id.size() > 64 || id.front() == '.' || id.back() == '.')
    return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return std::islower(c) || std::isdigit(c) || c == '-' || c == '.';
  });
}

bool OfficialUrl(const std::string &url) {
  constexpr char raw[] = "https://raw.githubusercontent.com/AERA-Plugins/";
  constexpr char github[] = "https://github.com/AERA-Plugins/";
  return url.compare(0, sizeof(raw) - 1, raw) == 0 ||
         url.compare(0, sizeof(github) - 1, github) == 0;
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
    } else if (errno != ENOENT || mkdir(current.c_str(), 0755) != 0) {
      return false;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

bool ReadBounded(const std::string &path, uint64_t maximum, std::string &data) {
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info{};
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<uint64_t>(info.st_size) > maximum) {
    close(fd); return false;
  }
  data.assign(static_cast<size_t>(info.st_size), '\0');
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t count = read(fd, data.data() + done, data.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { close(fd); return false; }
    done += static_cast<size_t>(count);
  }
  close(fd);
  return true;
}

bool DecodeSignature(std::string text, std::array<uint8_t, 64> &signature) {
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c);
  }), text.end());
  if (text.size() != signature.size() * 2) return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < signature.size(); ++i) {
    const int high = digit(text[i * 2]);
    const int low = digit(text[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    signature[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool Verify(const std::string &content, const std::string &signature_text) {
  std::array<uint8_t, 64> signature{};
  if (!DecodeSignature(signature_text, signature)) return false;
  EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_ED25519, nullptr, kSigningKey.data(), kSigningKey.size());
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  const bool valid = key != nullptr && context != nullptr &&
      EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
      EVP_DigestVerify(context, signature.data(), signature.size(),
                       reinterpret_cast<const uint8_t *>(content.data()),
                       content.size()) == 1;
  EVP_MD_CTX_free(context);
  EVP_PKEY_free(key);
  return valid;
}

bool ParseJson(const std::string &text, Json::Value &value) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::string errors;
  std::istringstream stream(text);
  return Json::parseFromStream(builder, stream, &value, &errors);
}

void ApplyLocalizedMetadata(const Json::Value &root, Plugin &plugin) {
  const Json::Value &localizations = root["localizations"];
  if (!localizations.isObject()) return;
  std::string locale = i18n::CurrentLanguage();
  std::array<std::string, 4> candidates{locale, locale, locale, locale};
  std::replace(candidates[1].begin(), candidates[1].end(), '_', '-');
  std::replace(candidates[2].begin(), candidates[2].end(), '-', '_');
  const auto separator = locale.find_first_of("_-");
  candidates[3] = locale.substr(0, separator);
  for (const auto &candidate : candidates) {
    if (!localizations.isMember(candidate) ||
        !localizations[candidate].isObject()) continue;
    const Json::Value &localized = localizations[candidate];
    if (localized["name"].isString() &&
        !localized["name"].asString().empty())
      plugin.name = localized["name"].asString();
    if (localized["description"].isString())
      plugin.description = localized["description"].asString();
    return;
  }
}

bool ParsePlugin(const std::string &text, Plugin &plugin, std::string &error,
                 bool local_payload = false) {
  Json::Value root;
  if (!ParseJson(text, root) || !root.isObject() || root["schema"].asUInt() != 1) {
    error = "Unsupported or malformed plugin manifest."; return false;
  }
  plugin.id = root["id"].asString();
  plugin.name = root["name"].asString();
  plugin.version = root["version"].asString();
  plugin.description = root["description"].asString();
  ApplyLocalizedMetadata(root, plugin);
  ApplyStoreMetadata(root, plugin, i18n::CurrentLanguage());
  plugin.type = root["type"].asString();
  plugin.entry = root["entry"].asString();
  plugin.payload_url = root["payload_url"].asString();
  plugin.payload_name = root.get("payload", "payload.bin").asString();
  plugin.payload_sha256 = root["payload_sha256"].asString();
  plugin.payload_size = root["payload_size"].asUInt64();
  plugin.expanded_size = root.get("expanded_size", 0).asUInt64();
  plugin.expanded_sha256 = root.get("expanded_sha256", "").asString();
  plugin.member_count = root.get("member_count", 0).asUInt();
  plugin.min_host_api = root["min_host_api"].asUInt();
  plugin.protocol_version = root.get("protocol_version", 1).asUInt();
  plugin.executable = root.get("executable", "").asString();
  plugin.icon = root.get("icon", "plugin").asString();
  plugin.font_family = root.get("font_family", "").asString();
  plugin.permissions.clear();
  const Json::Value permissions =
      root.get("permissions", Json::Value(Json::arrayValue));
  if (!permissions.isArray() || permissions.size() > 16) {
    error = "Plugin permissions are malformed.";
    return false;
  }
  for (const auto &permission : permissions) {
    const std::string name = permission.asString();
    if (!permission.isString() || name.empty() || name.size() > 40 ||
        !std::all_of(name.begin(), name.end(),
                     [](unsigned char c) {
                       return std::islower(c) || std::isdigit(c) || c == '-';
                     })) {
      error = "Plugin permissions are malformed.";
      return false;
    }
    plugin.permissions.push_back(name);
  }
  const bool hash_ok = plugin.payload_sha256.size() == 64 &&
      std::all_of(plugin.payload_sha256.begin(), plugin.payload_sha256.end(),
                  [](unsigned char c) { return std::isxdigit(c); });
  const bool expanded_hash_ok = plugin.expanded_sha256.empty() ||
      (plugin.expanded_sha256.size() == 64 &&
       std::all_of(plugin.expanded_sha256.begin(), plugin.expanded_sha256.end(),
                   [](unsigned char c) { return std::isxdigit(c); }));
  const bool legacy_entry =
      (plugin.id == "browser" && plugin.type == "browser-runtime" &&
       plugin.entry == "browser") ||
      (plugin.id == "retroarch" && plugin.type == "app-runtime" &&
       plugin.entry == "retroarch") ||
      (plugin.id == "doom" && plugin.type == "app-runtime" &&
       plugin.entry == "doom") ||
      (plugin.id == "telegram" && plugin.type == "app-runtime" &&
       plugin.entry == "telegram") ||
      (plugin.id == "gallery" && plugin.type == "app-runtime" &&
       plugin.entry == "gallery") ||
      (plugin.id == "media" && plugin.type == "app-runtime" &&
       plugin.entry == "media") ||
      (plugin.id == "recorder" && plugin.type == "app-runtime" &&
       plugin.entry == "recorder") ||
      (plugin.id == "appvault" && plugin.type == "app-runtime" &&
       plugin.entry == "appvault") ||
      (plugin.id == "streams" && plugin.type == "app-runtime" &&
       plugin.entry == "streams");
  const bool generic_entry = plugin.type == "ui-runtime" &&
      plugin.entry == "main" && plugin.protocol_version == 2 &&
      plugin.min_host_api == 2 &&
      plugin.executable == "usr/bin/aera-plugin" &&
      plugin.icon.size() <= 24;
  const bool theme_extension = plugin.type == "theme-extension" &&
      plugin.entry == "font" && plugin.executable.empty() &&
      plugin.permissions.empty() && plugin.icon.size() <= 24 &&
      (plugin.payload_name == "font.ttf" ||
       plugin.payload_name == "font.otf") &&
      !plugin.font_family.empty() && plugin.font_family.size() <= 64 &&
      std::none_of(plugin.font_family.begin(), plugin.font_family.end(),
                   [](unsigned char c) { return c < 32 || c == 127; });
  const auto has_permission = [&](const char *name) {
    return std::find(plugin.permissions.begin(), plugin.permissions.end(), name) !=
           plugin.permissions.end();
  };
  const bool permissions_ok = !generic_entry ||
      (has_permission("display") && has_permission("touch-input") &&
       std::all_of(plugin.permissions.begin(), plugin.permissions.end(),
                   [](const std::string &permission) {
                     return permission == "display" ||
                            permission == "touch-input" ||
                            permission == "settings-backup" ||
                            permission == "settings-restore" ||
                            permission == "android-settings-backup" ||
                            permission == "android-settings-restore" ||
                            permission == "screen-mirror";
                   }));
  if (!SafeId(plugin.id) || plugin.name.empty() || plugin.name.size() > 80 ||
      plugin.version.empty() || plugin.version.size() > 32 ||
      plugin.description.size() > 320 || plugin.type.empty() ||
      plugin.entry.empty() ||
      ((!theme_extension && plugin.payload_name != "runtime.xz") ||
       (theme_extension && plugin.payload_size > 32ULL * 1024 * 1024)) ||
      (!local_payload && !OfficialUrl(plugin.payload_url)) ||
      !hash_ok || !expanded_hash_ok ||
      plugin.payload_size == 0 || plugin.payload_size > kMaxPayload ||
      plugin.min_host_api == 0 || plugin.min_host_api > kHostApi ||
      (!legacy_entry && !generic_entry && !theme_extension) ||
      (!theme_extension && !permissions_ok)) {
    error = "Plugin manifest violates the AERA host policy."; return false;
  }
  if ((plugin.type == "browser-runtime" || plugin.type == "app-runtime" ||
       generic_entry) &&
      (!plugin.expanded_size || plugin.expanded_size > kMaxPayload ||
       !plugin.member_count || plugin.member_count > 4096 ||
       plugin.expanded_sha256.empty())) {
    error = "Plugin runtime metadata is incomplete."; return false;
  }
  return true;
}

bool ParseCatalog(const std::string &text, std::vector<Plugin> &plugins,
                  std::string &error) {
  Json::Value root;
  if (!ParseJson(text, root) || !root.isObject() || root["schema"].asUInt() != 1 ||
      !root["plugins"].isArray() || root["plugins"].size() > 64) {
    error = "Unsupported or malformed store catalog."; return false;
  }
  std::vector<Plugin> parsed;
  for (const auto &item : root["plugins"]) {
    Plugin plugin;
    plugin.id = item["id"].asString();
    plugin.name = item["name"].asString();
    plugin.version = item["version"].asString();
    plugin.description = item["description"].asString();
    ApplyLocalizedMetadata(item, plugin);
    ApplyStoreMetadata(item, plugin, i18n::CurrentLanguage());
    plugin.manifest_url = item["manifest_url"].asString();
    plugin.signature_url = item["signature_url"].asString();
    plugin.package_url = item.get("package_url", "").asString();
    plugin.package_size = item.get("package_size", 0).asUInt64();
    plugin.package_sha256 = item.get("package_sha256", "").asString();
    const bool package_hash_ok = plugin.package_sha256.size() == 64 &&
        std::all_of(plugin.package_sha256.begin(), plugin.package_sha256.end(),
                    [](unsigned char c) { return std::isxdigit(c); });
    const bool package_complete = OfficialUrl(plugin.package_url) &&
        plugin.package_size > 0 && plugin.package_size <= kMaxPackage &&
        package_hash_ok;
    if (!SafeId(plugin.id) || plugin.name.empty() || plugin.name.size() > 80 ||
        plugin.version.empty() || plugin.description.size() > 320 ||
        !OfficialUrl(plugin.manifest_url) || !OfficialUrl(plugin.signature_url) ||
        !package_complete) {
      error = "The signed catalog contains an invalid entry."; return false;
    }
    parsed.push_back(std::move(plugin));
  }
  plugins = std::move(parsed);
  return true;
}

bool Download(const std::string &url, const std::string &path, uint64_t limit,
              Progress *progress = nullptr, unsigned progress_start = 0,
              unsigned progress_end = 0, uint64_t expected_size = 0) {
  if (!OfficialUrl(url) || limit == 0 || limit > kMaxPayload) return false;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    unlink(path.c_str());

    // Recovery installs a process-wide SIGCHLD handler which may reap wget
    // before this worker reaches waitpid(). Keep a pipe descriptor inherited
    // across exec instead: EOF is delivered only after wget really exits.
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
      // recovery's stderr is a regular log file which is already much larger
      // than the limits used for manifests and signatures. Redirect before
      // RLIMIT_FSIZE so wget diagnostics cannot trip the payload size guard.
      const int null_fd = open("/dev/null", O_RDWR);
      if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
          dup2(null_fd, STDOUT_FILENO) < 0 ||
          dup2(null_fd, STDERR_FILENO) < 0) {
        _exit(126);
      }
      if (null_fd > STDERR_FILENO) close(null_fd);
      rlimit size{limit, limit};
      if (setrlimit(RLIMIT_FSIZE, &size) != 0) _exit(126);
      umask(077);
      const char *busybox = access("/sbin/busybox", X_OK) == 0
                                ? "/sbin/busybox" : "/system/bin/busybox";
      const char *arguments[] = {busybox, "wget", "-q", "-T", "10", "-t", "1",
                                 "-O", path.c_str(), url.c_str(), nullptr};
      execv(busybox, const_cast<char *const *>(arguments));
      _exit(127);
    }

    close(completion[1]);
    if (progress && expected_size) {
      progress->downloaded_bytes.store(0);
      progress->total_bytes.store(expected_size);
      progress->value.store(progress_start);
    }
    pollfd completion_event{completion[0], POLLIN | POLLHUP, 0};
    for (;;) {
      const int result = poll(&completion_event, 1, 100);
      if (result < 0 && errno == EINTR) continue;
      if (result < 0 || (completion_event.revents & (POLLHUP | POLLERR | POLLNVAL)))
        break;
      if (progress && expected_size) {
        struct stat partial{};
        if (lstat(path.c_str(), &partial) == 0 && S_ISREG(partial.st_mode) &&
            partial.st_size >= 0) {
          const uint64_t bytes = std::min<uint64_t>(partial.st_size, expected_size);
          progress->downloaded_bytes.store(bytes);
          const unsigned span = progress_end > progress_start
                                    ? progress_end - progress_start : 0;
          progress->value.store(progress_start +
              static_cast<unsigned>(bytes * span / expected_size));
        }
      }
    }
    close(completion[0]);

    int status = 0;
    pid_t waited;
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const bool exited_ok =
        (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
        (waited < 0 && errno == ECHILD);
    struct stat info{};
    if (exited_ok && lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size > 0 && static_cast<uint64_t>(info.st_size) <= limit) {
      if (progress && expected_size) {
        progress->downloaded_bytes.store(static_cast<uint64_t>(info.st_size));
        progress->value.store(progress_end);
      }
      return true;
    }
  }
  unlink(path.c_str());
  return false;
}

bool HashFile(const std::string &path, uint64_t expected_size,
              const std::string &expected_hash, Progress *progress,
              unsigned progress_start = 78, unsigned progress_end = 96) {
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info{};
  if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<uint64_t>(info.st_size) != expected_size) {
    if (fd >= 0) close(fd);
    return false;
  }
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  std::array<uint8_t, 65536> block{};
  uint64_t done = 0;
  while (ok && done < expected_size) {
    if (progress && progress->cancel.load()) { ok = false; break; }
    const ssize_t count = read(fd, block.data(), block.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ok = false; break; }
    ok = EVP_DigestUpdate(context, block.data(), static_cast<size_t>(count)) == 1;
    done += static_cast<uint64_t>(count);
    if (progress) progress->value.store(progress_start +
        static_cast<unsigned>(done * (progress_end - progress_start) /
                              expected_size));
  }
  close(fd);
  std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  ok = ok && done == expected_size &&
       EVP_DigestFinal_ex(context, digest.data(), &length) == 1 && length == 32;
  EVP_MD_CTX_free(context);
  constexpr char hex[] = "0123456789abcdef";
  std::string actual;
  actual.reserve(64);
  for (unsigned i = 0; ok && i < length; ++i) {
    actual.push_back(hex[digest[i] >> 4]);
    actual.push_back(hex[digest[i] & 15]);
  }
  std::string expected = expected_hash;
  std::transform(expected.begin(), expected.end(), expected.begin(), ::tolower);
  return ok && actual == expected;
}

bool RemoveTree(const std::string &path) {
  if (path.empty() || path == "/" || path == kStorageRoot || path == kMemoryRoot)
    return false;
  struct stat info{};
  if (lstat(path.c_str(), &info)) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return unlink(path.c_str()) == 0;
  DIR *directory = opendir(path.c_str());
  if (!directory) return false;
  bool ok = true;
  while (dirent *entry = readdir(directory)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    ok = RemoveTree(path + "/" + entry->d_name) && ok;
  }
  closedir(directory);
  return rmdir(path.c_str()) == 0 && ok;
}

bool PluginAt(const std::string &directory, Location location,
              Plugin &plugin, std::string &error) {
  std::string manifest, signature;
  if (!ReadBounded(directory + "/plugin.json", kMaxManifest, manifest) ||
      !ParsePlugin(manifest, plugin, error, true)) {
    if (error.empty()) error = "The installed plugin manifest is invalid.";
    return false;
  }
  plugin.trust = ReadBounded(directory + "/plugin.json.sig", kMaxSignature,
                             signature) && Verify(manifest, signature)
                     ? Trust::kOfficial
                     : Trust::kUnofficial;
  plugin.location = location;
  struct stat payload{};
  const std::string path = directory + "/" + plugin.payload_name;
  if (lstat(path.c_str(), &payload) || !S_ISREG(payload.st_mode) ||
      static_cast<uint64_t>(payload.st_size) != plugin.payload_size) {
    error = "The installed plugin payload is missing or incomplete."; return false;
  }
  return true;
}

std::vector<Plugin> ScanRoot(const char *root, Location location) {
  std::vector<Plugin> result;
  DIR *directory = opendir(root);
  if (!directory) return result;
  while (dirent *entry = readdir(directory)) {
    const std::string id = entry->d_name;
    if (!SafeId(id)) continue;
    Plugin plugin; std::string error;
    if (PluginAt(std::string(root) + "/" + id, location, plugin, error) &&
        plugin.id == id) result.push_back(std::move(plugin));
  }
  closedir(directory);
  return result;
}

struct LocalBundle {
  ZipArchiveHandle archive = nullptr;
  ZipEntry64 manifest_entry{};
  ZipEntry64 signature_entry{};
  ZipEntry64 payload_entry{};
  std::string manifest;
  std::string signature;
  Plugin plugin;
  bool has_signature = false;
  std::string payload_entry_name;

  ~LocalBundle() {
    if (archive != nullptr) CloseArchive(archive);
  }
};

bool ExtractText(ZipArchiveHandle archive, const ZipEntry64 &entry,
                 uint64_t maximum, std::string &text) {
  if (entry.uncompressed_length > maximum ||
      entry.uncompressed_length > SIZE_MAX) return false;
  text.assign(static_cast<size_t>(entry.uncompressed_length), '\0');
  return ExtractToMemory(archive, &entry,
                         reinterpret_cast<uint8_t *>(text.data()),
                         text.size()) == 0;
}

bool OpenLocalBundle(const std::string &path, LocalBundle &bundle,
                     std::string &error) {
  if (!IsPackageFile(path)) {
    error = "AERA plugin packages must use the .aerap extension.";
    return false;
  }
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info{};
  if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > kMaxPackage) {
    if (fd >= 0) close(fd);
    error = "The selected plugin package is missing or too large.";
    return false;
  }
  const int result = OpenArchiveFd(fd, path.c_str(), &bundle.archive, true);
  if (result != 0) {
    close(fd);
    error = "The selected .aerap file is not a valid package.";
    return false;
  }

  const auto archive_info = GetArchiveInfo(bundle.archive);
  if (archive_info.entry_count < 2 || archive_info.entry_count > 3) {
    error = "The plugin package contains unsupported files.";
    return false;
  }
  bool manifest_found = false;
  bool payload_found = false;
  void *cookie = nullptr;
  if (StartIteration(bundle.archive, &cookie) != 0) {
    error = "The plugin package directory could not be read.";
    return false;
  }
  bool entries_ok = true;
  ZipEntry64 entry;
  std::string name;
  while (Next(cookie, &entry, &name) == 0) {
    if (name == "plugin.json" && !manifest_found) {
      bundle.manifest_entry = entry;
      manifest_found = true;
    } else if (name == "plugin.json.sig" && !bundle.has_signature) {
      bundle.signature_entry = entry;
      bundle.has_signature = true;
    } else if ((name == "runtime.xz" || name == "font.ttf" ||
                name == "font.otf") && !payload_found) {
      bundle.payload_entry = entry;
      bundle.payload_entry_name = name;
      payload_found = true;
    } else {
      entries_ok = false;
      break;
    }
  }
  EndIteration(cookie);
  if (!entries_ok || !manifest_found || !payload_found ||
      !ExtractText(bundle.archive, bundle.manifest_entry, kMaxManifest,
                   bundle.manifest) ||
      (bundle.has_signature &&
       !ExtractText(bundle.archive, bundle.signature_entry, kMaxSignature,
                    bundle.signature)) ||
      !ParsePlugin(bundle.manifest, bundle.plugin, error, true) ||
      bundle.payload_entry_name != bundle.plugin.payload_name ||
      bundle.payload_entry.uncompressed_length != bundle.plugin.payload_size) {
    if (error.empty()) error = "The plugin package metadata is invalid.";
    return false;
  }
  bundle.plugin.trust = bundle.has_signature &&
                                Verify(bundle.manifest, bundle.signature)
                            ? Trust::kOfficial
                            : Trust::kUnofficial;
  return true;
}

bool WriteFile(const std::string &path, const std::string &content) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                              O_CLOEXEC, 0600);
  if (fd < 0) return false;
  size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t count = write(fd, content.data() + offset,
                                content.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { close(fd); return false; }
    offset += static_cast<size_t>(count);
  }
  const bool ok = fsync(fd) == 0;
  close(fd);
  return ok;
}

bool ExtractPayload(LocalBundle &bundle, const std::string &path) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                              O_CLOEXEC, 0600);
  if (fd < 0) return false;
  const bool ok = ExtractEntryToFile(bundle.archive, &bundle.payload_entry,
                                     fd) == 0 && fsync(fd) == 0;
  close(fd);
  return ok;
}

bool InstallLocal(const Request &request, Location location,
                  Progress &progress, unsigned extract_progress = 18,
                  unsigned verify_start = 55, unsigned verify_end = 94) {
  LocalBundle bundle;
  std::string error;
  if (!OpenLocalBundle(request.path, bundle, error)) {
    progress.error = error;
    return false;
  }
  if (bundle.plugin.trust == Trust::kUnofficial &&
      !request.allow_unofficial) {
    progress.error = "Unofficial plugin installation was not confirmed.";
    return false;
  }
  if (!request.id.empty() && request.id != bundle.plugin.id) {
    progress.error = "The selected plugin identity changed before installation.";
    return false;
  }
  const char *root = location == Location::kMemory ? kMemoryRoot : kStorageRoot;
  if (!EnsureDirectory(root)) {
    progress.error = location == Location::kMemory
        ? "Could not create the RAM plugin store."
        : "Persistent storage is locked or unavailable.";
    return false;
  }
  const std::string staging = std::string(root) + "/.install-" +
      bundle.plugin.id + "-" + std::to_string(static_cast<long long>(getpid()));
  RemoveTree(staging);
  if (mkdir(staging.c_str(), 0700) != 0) {
    progress.error = "Could not create a staging directory.";
    return false;
  }

  progress.status = "Reading " + bundle.plugin.name;
  progress.value.store(extract_progress);
  const std::string manifest_path = staging + "/plugin.json";
  const std::string signature_path = staging + "/plugin.json.sig";
  const std::string payload_path = staging + "/" + bundle.plugin.payload_name;
  if (!WriteFile(manifest_path, bundle.manifest) ||
      (bundle.has_signature && !WriteFile(signature_path, bundle.signature)) ||
      !ExtractPayload(bundle, payload_path)) {
    RemoveTree(staging);
    progress.error = "Could not extract the plugin package.";
    return false;
  }
  progress.status = "Verifying payload";
  progress.value.store(verify_start);
  if (!HashFile(payload_path, bundle.plugin.payload_size,
                bundle.plugin.payload_sha256, &progress, verify_start,
                verify_end)) {
    RemoveTree(staging);
    progress.error = "Plugin payload integrity check failed.";
    return false;
  }
  chmod(manifest_path.c_str(), 0444);
  if (bundle.has_signature) chmod(signature_path.c_str(), 0444);
  chmod(payload_path.c_str(), 0444);

  const std::string final = std::string(root) + "/" + bundle.plugin.id;
  const std::string previous = final + ".previous";
  RemoveTree(previous);
  const bool had_previous = access(final.c_str(), F_OK) == 0;
  if ((had_previous && rename(final.c_str(), previous.c_str()) != 0) ||
      rename(staging.c_str(), final.c_str()) != 0) {
    if (had_previous) rename(previous.c_str(), final.c_str());
    RemoveTree(staging);
    progress.error = "Could not publish the verified plugin.";
    return false;
  }
  RemoveTree(previous);
  aera::shared_storage::Normalize(final, true);
  progress.status = bundle.plugin.name +
      (bundle.plugin.trust == Trust::kOfficial
           ? " installed"
           : " installed as an unofficial app");
  progress.value.store(100);
  return true;
}

bool Refresh(Progress &progress) {
  if (!EnsureDirectory(kCacheRoot)) {
    progress.error = "Could not create the temporary store cache."; return false;
  }
  const std::string manifest = std::string(kCacheRoot) + "/catalog.json.new";
  const std::string signature = manifest + ".sig";
  progress.status = "Downloading signed catalog"; progress.value.store(10);
  if (!Download(kCatalogUrl, manifest, kMaxCatalog)) {
    unlink(manifest.c_str()); unlink(signature.c_str());
    progress.error = "Could not download the AERA plugin catalog."; return false;
  }
  progress.value.store(45);
  if (!Download(kCatalogSignatureUrl, signature, kMaxSignature)) {
    unlink(manifest.c_str()); unlink(signature.c_str());
    progress.error = "Could not download the AERA plugin catalog."; return false;
  }
  progress.value.store(75);
  std::string content, signature_text;
  if (!ReadBounded(manifest, kMaxCatalog, content) ||
      !ReadBounded(signature, kMaxSignature, signature_text) ||
      !Verify(content, signature_text)) {
    unlink(manifest.c_str()); unlink(signature.c_str());
    progress.error = "The plugin catalog signature is invalid."; return false;
  }
  progress.value.store(84);
  std::vector<Plugin> parsed; std::string error;
  if (!ParseCatalog(content, parsed, error)) {
    unlink(manifest.c_str()); unlink(signature.c_str()); progress.error = error; return false;
  }
  progress.value.store(92);
  rename(manifest.c_str(), (std::string(kCacheRoot) + "/catalog.json").c_str());
  rename(signature.c_str(), (std::string(kCacheRoot) + "/catalog.json.sig").c_str());
  // Best effort: a locked or unavailable storage volume must not fail Refresh.
  if (EnsureDirectory(kPersistentCacheRoot) &&
      WriteFile(std::string(kPersistentCacheRoot) + "/catalog.json.new", content) &&
      WriteFile(std::string(kPersistentCacheRoot) + "/catalog.json.sig.new", signature_text)) {
    rename((std::string(kPersistentCacheRoot) + "/catalog.json.new").c_str(),
           (std::string(kPersistentCacheRoot) + "/catalog.json").c_str());
    rename((std::string(kPersistentCacheRoot) + "/catalog.json.sig.new").c_str(),
           (std::string(kPersistentCacheRoot) + "/catalog.json.sig").c_str());
    aera::shared_storage::Normalize(kPersistentCacheRoot, true);
  }
  {
    std::lock_guard<std::mutex> lock(gCatalogMutex);
    gCatalog = std::move(parsed);
    gCatalogSource = content;
    gCatalogLanguage = i18n::CurrentLanguage();
  }
  progress.status = "Store catalog updated"; progress.value.store(100); return true;
}

Plugin CatalogEntry(const std::string &id) {
  for (const auto &plugin : Catalog()) if (plugin.id == id) return plugin;
  return {};
}

bool Install(const std::string &id, Location location, Progress &progress) {
  const Plugin source = CatalogEntry(id);
  if (source.id.empty()) { progress.error = "Plugin is not in the signed store."; return false; }
  if (!source.package_url.empty()) {
    if (!EnsureDirectory(kCacheRoot)) {
      progress.error = "Could not create the temporary store cache.";
      return false;
    }
    const std::string package = std::string(kCacheRoot) + "/" + id + ".aerap";
    progress.status = "Downloading " + source.name;
    progress.value.store(5);
    if (!Download(source.package_url, package, source.package_size, &progress,
                  5, 68, source.package_size)) {
      unlink(package.c_str());
      progress.error = "Plugin package download failed.";
      return false;
    }
    progress.status = "Verifying package";
    if (!HashFile(package, source.package_size, source.package_sha256,
                  &progress, 68, 78)) {
      unlink(package.c_str());
      progress.error = "Plugin package integrity check failed.";
      return false;
    }
    Request request;
    request.job = location == Location::kMemory ? Job::kInstallLocalMemory
                                                 : Job::kInstallLocalStorage;
    request.id = id;
    request.path = package;
    request.allow_unofficial = false;
    const bool installed = InstallLocal(request, location, progress, 80, 84, 98);
    unlink(package.c_str());
    return installed;
  }
  const char *root = location == Location::kMemory ? kMemoryRoot : kStorageRoot;
  if (!EnsureDirectory(root)) {
    progress.error = location == Location::kMemory
        ? "Could not create the RAM plugin store."
        : "Persistent storage is locked or unavailable.";
    return false;
  }
  const std::string staging = std::string(root) + "/.install-" + id + "-" +
      std::to_string(static_cast<long long>(getpid()));
  RemoveTree(staging);
  if (mkdir(staging.c_str(), 0700)) {
    progress.error = "Could not create a staging directory."; return false;
  }
  const std::string manifest_path = staging + "/plugin.json";
  const std::string signature_path = staging + "/plugin.json.sig";
  progress.status = "Downloading signed manifest"; progress.value.store(5);
  if (!Download(source.manifest_url, manifest_path, kMaxManifest)) {
    RemoveTree(staging); progress.error = "Could not download the plugin manifest."; return false;
  }
  progress.value.store(10);
  if (!Download(source.signature_url, signature_path, kMaxSignature)) {
    RemoveTree(staging); progress.error = "Could not download the plugin manifest."; return false;
  }
  progress.value.store(15);
  std::string manifest, signature, error;
  Plugin plugin;
  if (!ReadBounded(manifest_path, kMaxManifest, manifest) ||
      !ReadBounded(signature_path, kMaxSignature, signature) ||
      !Verify(manifest, signature) || !ParsePlugin(manifest, plugin, error) ||
      plugin.id != id) {
    RemoveTree(staging);
    progress.error = error.empty() ? "Plugin manifest signature is invalid." : error;
    return false;
  }
  progress.status = "Downloading " + plugin.name; progress.value.store(20);
  const std::string payload = staging + "/" + plugin.payload_name;
  if (!Download(plugin.payload_url, payload, plugin.payload_size,
                &progress, 20, 78, plugin.payload_size)) {
    RemoveTree(staging); progress.error = "Plugin payload download failed."; return false;
  }
  progress.status = "Verifying payload"; progress.value.store(78);
  if (!HashFile(payload, plugin.payload_size, plugin.payload_sha256, &progress,
                78, 96)) {
    RemoveTree(staging); progress.error = "Plugin payload integrity check failed."; return false;
  }
  progress.value.store(97);
  chmod(manifest_path.c_str(), 0444); chmod(signature_path.c_str(), 0444);
  chmod(payload.c_str(), 0444);
  const std::string final = std::string(root) + "/" + id;
  if (!RemoveTree(final) || rename(staging.c_str(), final.c_str())) {
    RemoveTree(staging); progress.error = "Could not publish the verified plugin."; return false;
  }
  aera::shared_storage::Normalize(final, true);
  progress.status =
      i18n::Format("%s installed", plugin.name.c_str());
  progress.value.store(100);
  return true;
}

bool Remove(const std::string &id, Progress &progress) {
  if (!SafeId(id)) { progress.error = "Invalid plugin identifier."; return false; }
  const bool memory = RemoveTree(std::string(kMemoryRoot) + "/" + id);
  const bool storage = RemoveTree(std::string(kStorageRoot) + "/" + id);
  if (!memory || !storage) {
    progress.error = "One or more plugin files could not be removed."; return false;
  }
  progress.status = "Plugin removed"; progress.value.store(100); return true;
}

}  // namespace

std::vector<Plugin> Catalog() {
  {
    std::lock_guard<std::mutex> lock(gCatalogMutex);
    if (!gCatalogSource.empty() && gCatalogLanguage != i18n::CurrentLanguage()) {
      std::vector<Plugin> translated;
      std::string error;
      if (ParseCatalog(gCatalogSource, translated, error)) {
        gCatalog = std::move(translated);
        gCatalogLanguage = i18n::CurrentLanguage();
      }
    }
    if (!gCatalog.empty()) return gCatalog;
  }
  std::string content, signature, error;
  std::vector<Plugin> parsed;
  for (const char *cache : {kCacheRoot, kPersistentCacheRoot}) {
  if (ReadBounded(std::string(cache) + "/catalog.json", kMaxCatalog, content) &&
      ReadBounded(std::string(cache) + "/catalog.json.sig", kMaxSignature, signature) &&
      Verify(content, signature) && ParseCatalog(content, parsed, error)) {
    std::lock_guard<std::mutex> lock(gCatalogMutex);
    gCatalog = parsed;
    gCatalogSource = content;
    gCatalogLanguage = i18n::CurrentLanguage();
    return parsed;
  }
  }
  return {};
}

std::vector<Plugin> Installed() {
  auto memory = ScanRoot(kMemoryRoot, Location::kMemory);
  auto storage = ScanRoot(kStorageRoot, Location::kStorage);
  for (auto &plugin : storage) {
    const bool shadowed = std::any_of(memory.begin(), memory.end(), [&](const Plugin &item) {
      return item.id == plugin.id;
    });
    if (!shadowed) memory.push_back(std::move(plugin));
  }
  return memory;
}

bool IsUpdateAvailable(const Plugin &installed, const Plugin &available) {
  return installed.id == available.id &&
         installed.trust == Trust::kOfficial &&
         CompareVersions(available.version, installed.version) > 0;
}

std::vector<PluginUpdate> AvailableUpdates() {
  return AvailableUpdates(Installed());
}

std::vector<PluginUpdate> AvailableUpdates(
    const std::vector<Plugin> &installed) {
  const auto catalog = Catalog();
  std::vector<PluginUpdate> updates;
  for (const auto &available : catalog) {
    const auto found = std::find_if(
        installed.begin(), installed.end(), [&](const Plugin &plugin) {
          return plugin.id == available.id;
        });
    if (found != installed.end() && IsUpdateAvailable(*found, available))
      updates.push_back({*found, available});
  }
  return updates;
}

bool FindInstalled(const std::string &id, Plugin &plugin) {
  for (auto &item : Installed()) {
    if (item.id == id) { plugin = std::move(item); return true; }
  }
  return false;
}

bool IsPackageFile(const std::string &name) {
  constexpr char suffix[] = ".aerap";
  if (name.size() < sizeof(suffix) - 1) return false;
  const size_t offset = name.size() - (sizeof(suffix) - 1);
  for (size_t i = 0; i < sizeof(suffix) - 1; ++i) {
    if (std::tolower(static_cast<unsigned char>(name[offset + i])) !=
        suffix[i]) return false;
  }
  return true;
}

bool InspectLocalPackage(const std::string &path, Plugin &plugin,
                         std::string &error) {
  LocalBundle bundle;
  if (!OpenLocalBundle(path, bundle, error)) return false;
  plugin = bundle.plugin;
  return true;
}

bool Run(const Request &request, Progress &progress) {
  progress.value.store(0); progress.error.clear(); progress.status.clear();
  progress.completed_plugins.store(0);
  progress.total_plugins.store(0);
  switch (request.job) {
    case Job::kRefresh: return Refresh(progress);
    case Job::kInstallStorage:
      return Install(request.id, Location::kStorage, progress);
    case Job::kInstallMemory:
      return Install(request.id, Location::kMemory, progress);
    case Job::kInstallLocalStorage:
      return InstallLocal(request, Location::kStorage, progress);
    case Job::kInstallLocalMemory:
      return InstallLocal(request, Location::kMemory, progress);
    case Job::kRemove: return Remove(request.id, progress);
    case Job::kUpdateAll: {
      const auto updates = AvailableUpdates();
      progress.total_plugins.store(updates.size());
      unsigned failed = 0;
      for (size_t i = 0; i < updates.size(); ++i) {
        const auto &update = updates[i];
        progress.value.store(0);
        progress.completed_plugins.store(i);
        progress.downloaded_bytes.store(0);
        progress.total_bytes.store(0);
        if (progress.cancel.load()) return false;
        if (!Install(update.available.id, update.installed.location, progress)) ++failed;
      }
      progress.completed_plugins.store(updates.size());
      progress.value.store(100);
      if (failed) {
        progress.error = i18n::Format("%u plugin updates failed.", failed);
        return false;
      }
      progress.status = "All plugins are up to date.";
      return true;
    }
    case Job::kScreenshot: {
      const Plugin plugin = CatalogEntry(request.id);
      if (request.screenshot_index >= plugin.screenshots.size()) {
        progress.error = "Screenshot unavailable.";
        return false;
      }
      const std::string url = plugin.screenshots[request.screenshot_index];
      // The URL identifies the cache entry, including changed screenshots.
      uint64_t hash = UINT64_C(14695981039346656037);
      for (unsigned char c : url) { hash ^= c; hash *= UINT64_C(1099511628211); }
      const std::string name = plugin.id + "-" + std::to_string(hash) + ScreenshotExtension(url);
      std::string cache = std::string(kPersistentCacheRoot) + "/screenshots";
      if (!EnsureDirectory(cache)) cache = std::string(kCacheRoot) + "/screenshots";
      if (!EnsureDirectory(cache)) {
        progress.error = "Screenshot unavailable.";
        return false;
      }
      const std::string path = cache + "/" + name;
      std::string cached;
      constexpr uint64_t limit = 4 * 1024 * 1024;
      if (!ReadBounded(path, limit, cached) || cached.empty()) {
        if (!Download(url, path + ".new", limit, &progress, 5, 95) ||
            rename((path + ".new").c_str(), path.c_str()) != 0) {
          unlink((path + ".new").c_str());
          progress.error = "Screenshot unavailable.";
          return false;
        }
      }
      progress.value.store(100);
      progress.status = path;
      return true;
    }
  }
  progress.error = "Unsupported plugin operation."; return false;
}

bool ResolvePayload(const std::string &id, Plugin &plugin, std::string &path,
                    std::string &error) {
  if (!FindInstalled(id, plugin)) {
    error = "Install the plugin from Plugin Manager first."; return false;
  }
  const char *root = plugin.location == Location::kMemory ? kMemoryRoot : kStorageRoot;
  path = std::string(root) + "/" + plugin.id + "/" + plugin.payload_name;
  struct stat info{};
  if (lstat(path.c_str(), &info) || !S_ISREG(info.st_mode) ||
      static_cast<uint64_t>(info.st_size) != plugin.payload_size) {
    error = "Installed plugin payload is incomplete."; return false;
  }
  return true;
}

bool IsGeneric(const Plugin &plugin) {
  return plugin.type == "ui-runtime" && plugin.entry == "main" &&
         plugin.protocol_version == 2 &&
         plugin.executable == "usr/bin/aera-plugin";
}

bool IsThemeExtension(const Plugin &plugin) {
  return plugin.type == "theme-extension" && plugin.entry == "font" &&
         plugin.executable.empty() &&
         (plugin.payload_name == "font.ttf" ||
          plugin.payload_name == "font.otf") &&
         !plugin.font_family.empty();
}

bool IsLaunchable(const Plugin &plugin) {
  if (IsThemeExtension(plugin)) return false;
  return IsGeneric(plugin) || plugin.entry == "browser" ||
      plugin.entry == "retroarch" || plugin.entry == "doom" ||
      plugin.entry == "telegram" || plugin.entry == "gallery" ||
      plugin.entry == "media" || plugin.entry == "streams" ||
      plugin.entry == "recorder" || plugin.entry == "appvault";
}

bool HasPermission(const Plugin &plugin, const std::string &permission) {
  return std::find(plugin.permissions.begin(), plugin.permissions.end(),
                   permission) != plugin.permissions.end();
}

const char *LocationLabel(Location location) {
  switch (location) {
    case Location::kStorage: return "Storage";
    case Location::kMemory: return "RAM only";
    default: return "Not installed";
  }
}

const char *TrustLabel(Trust trust) {
  return trust == Trust::kOfficial ? "Official" : "Unofficial";
}

}  // namespace aeraui::plugins
