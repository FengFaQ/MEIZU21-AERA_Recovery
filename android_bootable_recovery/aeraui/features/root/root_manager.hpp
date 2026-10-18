/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aeraui::root {

inline constexpr bool Enabled() {
#if defined(AERA_ENABLE_ROOT_MANAGER) && AERA_ENABLE_ROOT_MANAGER == 1
  return true;
#else
  return false;
#endif
}

enum class Provider { kKernelSU, kKernelSUNext, kSukiSU };
enum class Job {
  kRefreshRelease,
  kPatch,
  kRollback,
  kRefreshModules,
  kEnableModule,
  kDisableModule,
  kRemoveModule,
  kUpdateModule,
  kInstallManager,
  kUninstallManager,
  kInspectTarget,
};

struct Status {
  bool ksud_available = false;
  bool kmi_supported = false;
  bool init_boot_available = false;
  bool storage_ready = false;
  std::string kernel_release;
  std::string kmi;
  std::string slot;
  std::string error;
};

struct PatchInfo {
  bool inspected = false;
  bool patched = false;
  bool aera_verified = false;
  std::string provider;
  std::string version;
  std::string kmi;
  std::string detail;
};

struct Release {
  Provider provider = Provider::kKernelSU;
  bool available = false;
  bool archive = false;
  bool bundled = false;
  std::string version;
  std::string kmi;
  std::string asset_name;
  std::string asset_url;
  std::string local_path;
  std::string sha256;
  uint64_t size = 0;
  std::string error;
};

struct Module {
  std::string id;
  std::string name;
  std::string version;
  int64_t version_code = 0;
  std::string author;
  std::string description;
  bool enabled = true;
  bool remove = false;
  std::string update_json;
  bool update_available = false;
  std::string latest_version;
  int64_t latest_version_code = 0;
  std::string update_zip;
  std::string changelog;
};

struct ManagerStatus {
  bool installed = false;
  bool staged = false;
  bool removal_pending = false;
  std::string package_name;
  std::string detail;
};

struct Request {
  Job job = Job::kRefreshRelease;
  Provider provider = Provider::kKernelSU;
  std::string slot;
  std::string module_id;
  bool verify_target_kernel = false;
};

struct Progress {
  std::atomic<unsigned> value{0};
  std::atomic<uint64_t> downloaded{0};
  std::atomic<uint64_t> total{0};
  std::atomic<bool> cancel{false};
  std::mutex text_mutex;
  std::string status;
  std::string detail;
  bool has_inspection = false;
  Status inspected_target;
  PatchInfo inspected_patch;
  Release inspected_offline;
};

Status Probe();
Status ProbeSlot(const std::string &slot);
PatchInfo InspectSlot(const std::string &slot);
Release BundledRelease(Provider provider, const std::string &kmi);
Release CachedRelease(Provider provider);
std::vector<Module> InstalledModules();
ManagerStatus InspectManager(Provider provider);
bool PatchUpdateAvailable(const PatchInfo &patch, Provider provider,
                          const Release &release);
bool Run(const Request &request, Progress &progress);
const char *ProviderName(Provider provider);

}  // namespace aeraui::root
