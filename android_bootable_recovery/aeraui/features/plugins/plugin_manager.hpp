/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aeraui::plugins {

constexpr uint32_t kHostApi = 2;

enum class Location {
  kNone = 0,
  kStorage,
  kMemory,
};

enum class Trust {
  kOfficial = 0,
  kUnofficial,
};

enum class Job {
  kRefresh = 0,
  kInstallStorage,
  kInstallMemory,
  kInstallLocalStorage,
  kInstallLocalMemory,
  kRemove,
  kUpdateAll,
  kScreenshot,
};

struct Plugin {
  std::string id;
  std::string name;
  std::string version;
  std::string description;
  std::string category = "tools";
  std::string details;
  std::string author;
  std::vector<std::string> screenshots;
  std::string type;
  std::string entry;
  std::string manifest_url;
  std::string signature_url;
  std::string package_url;
  std::string package_sha256;
  uint64_t package_size = 0;
  std::string payload_url;
  std::string payload_name;
  std::string payload_sha256;
  uint64_t payload_size = 0;
  uint64_t expanded_size = 0;
  std::string expanded_sha256;
  uint32_t member_count = 0;
  uint32_t min_host_api = 0;
  uint32_t protocol_version = 1;
  std::string executable;
  std::string icon;
  std::string font_family;
  std::vector<std::string> permissions;
  Location location = Location::kNone;
  Trust trust = Trust::kOfficial;
};

struct PluginUpdate {
  Plugin installed;
  Plugin available;
};

struct Request {
  Job job = Job::kRefresh;
  std::string id;
  std::string path;
  bool allow_unofficial = false;
  size_t screenshot_index = 0;
};

struct Progress {
  std::atomic<unsigned> value{0};
  std::atomic<uint64_t> downloaded_bytes{0};
  std::atomic<uint64_t> total_bytes{0};
  std::atomic<bool> cancel{false};
  std::atomic<unsigned> completed_plugins{0};
  std::atomic<unsigned> total_plugins{0};
  std::string status;
  std::string error;
};

std::vector<Plugin> Catalog();
std::vector<Plugin> Installed();
std::vector<PluginUpdate> AvailableUpdates();
std::vector<PluginUpdate> AvailableUpdates(
    const std::vector<Plugin> &installed);
bool IsUpdateAvailable(const Plugin &installed, const Plugin &available);
bool FindInstalled(const std::string &id, Plugin &plugin);
bool Run(const Request &request, Progress &progress);
bool InspectLocalPackage(const std::string &path, Plugin &plugin,
                         std::string &error);
bool IsPackageFile(const std::string &name);

// Revalidates the signed manifest and returns the payload path. The large
// payload hash is checked by its consumer immediately before extraction/use.
bool ResolvePayload(const std::string &id, Plugin &plugin, std::string &path,
                    std::string &error);
bool IsGeneric(const Plugin &plugin);
bool IsThemeExtension(const Plugin &plugin);
bool IsLaunchable(const Plugin &plugin);
bool HasPermission(const Plugin &plugin, const std::string &permission);

const char *LocationLabel(Location location);
const char *TrustLabel(Trust trust);

}  // namespace aeraui::plugins
