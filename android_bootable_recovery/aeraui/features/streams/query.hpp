/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace aeraui::streams {

bool RunQuery(const std::string& runtime, const std::vector<std::string>& arguments,
              std::atomic<bool>& cancel, std::string& json, std::string& error);

}  // namespace aeraui::streams
