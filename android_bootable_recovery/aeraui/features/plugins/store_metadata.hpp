/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "plugin_manager.hpp"
#include <json/json.h>

namespace aeraui::plugins {
// Optional presentation metadata; release and manifest validation is unchanged.
void ApplyStoreMetadata(const Json::Value &root, Plugin &plugin,
                        const std::string &language);
std::string ScreenshotExtension(const std::string &url);
const char *CategoryLabel(const std::string &category);
}  // namespace aeraui::plugins
