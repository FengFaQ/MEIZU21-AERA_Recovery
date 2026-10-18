/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <json/json.h>
#include <aeraui/backend.hpp>
namespace aeraui {
struct AppVaultProgress {
  unsigned percent = 0;
  std::string stage;
  std::string amount;
  uint64_t files_done = 0;
  uint64_t files_total = 0;
  std::string current_file;
};
bool AppVaultInstalled();
bool RunAppVaultRemote(const JobRequest &request, Json::Value &result,
                      const std::function<void(const AppVaultProgress &)> &progress);
}
