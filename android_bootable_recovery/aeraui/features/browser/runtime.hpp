// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace aeraui::web {
struct Preparation {
  std::atomic<bool> cancel{false};
  std::atomic<unsigned> progress{0};
  std::atomic<bool> done{false};
  bool verified = false;
  std::string directory;
  std::string version;
  std::string error;
};
bool RuntimeInstalled();
bool PluginRuntimeInstalled(const char *id, const char *type,
                            const char *entry);
void PrepareRuntime(Preparation &state,
                    const char *payload = nullptr,
                    const char *parent = "/tmp");
void PreparePluginRuntime(Preparation &state, const char *id,
                          const char *type, const char *entry,
                          const char *parent = "/tmp");
void RemoveRuntime(const std::string &directory);
// The browser payload is supplied by a signed plugin, then executed only by
// the fixed unprivileged jail bundled with the AERA host.
std::string LaunchBlockReason();
}  // namespace aeraui::web
