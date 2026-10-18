// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <aeraui/backend.hpp>

namespace aeraui::smb {
struct Result {
  std::vector<std::string> directories;
  std::string error;
};

bool ParseListing(const std::string &json, Result *result);
Result List(const NasConfig &config, const std::string &share,
            const std::string &path, const std::atomic<bool> &cancel);
}  // namespace aeraui::smb
