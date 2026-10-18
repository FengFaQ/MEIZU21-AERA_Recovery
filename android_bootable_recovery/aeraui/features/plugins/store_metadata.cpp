/* SPDX-License-Identifier: Apache-2.0 */
#include "store_metadata.hpp"
#include <algorithm>
#include <array>
#include <cctype>

namespace aeraui::plugins {
const char *CategoryLabel(const std::string &category) {
  if (category == "backup") return "Backup";
  if (category == "multimedia") return "Multimedia";
  if (category == "network") return "Network";
  if (category == "games") return "Games";
  if (category == "themes") return "Themes";
  return "Tools";
}

std::string ScreenshotExtension(const std::string &url) {
  if (url.size() > 2048 ||
      (url.rfind("https://raw.githubusercontent.com/AERA-Plugins/", 0) != 0 &&
       url.rfind("https://github.com/AERA-Plugins/", 0) != 0)) return {};
  const auto dot = url.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string extension = url.substr(dot);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".png" || extension == ".jpg" || extension == ".jpeg"
             ? extension : std::string{};
}

void ApplyStoreMetadata(const Json::Value &root, Plugin &plugin,
                        const std::string &language) {
  if (root["type"].isString() && root["type"].asString() == "theme-extension")
    plugin.category = "themes";
  const auto &store = root["store"];
  if (!store.isObject()) return;
  if (store["category"].isString()) {
    const std::string category = store["category"].asString();
    for (const char *known : {"tools", "backup", "multimedia", "network", "games", "themes"})
      if (category == known) plugin.category = category;
  }
  if (store["description"].isString() && store["description"].asString().size() <= 8192)
    plugin.details = store["description"].asString();
  if (store["summary"].isString() && !store["summary"].asString().empty() &&
      store["summary"].asString().size() <= 320)
    plugin.description = store["summary"].asString();
  if (store["author"].isString() && store["author"].asString().size() <= 160)
    plugin.author = store["author"].asString();
  const auto &localizations = store["localizations"];
  if (localizations.isObject()) {
    std::array<std::string, 4> candidates{language, language, language, language};
    std::replace(candidates[1].begin(), candidates[1].end(), '_', '-');
    std::replace(candidates[2].begin(), candidates[2].end(), '-', '_');
    candidates[3] = language.substr(0, language.find_first_of("_-"));
    for (const auto &candidate : candidates) {
      if (!localizations[candidate].isObject()) continue;
      const auto &value = localizations[candidate]["description"];
      const auto &summary = localizations[candidate]["summary"];
      if (summary.isString() && !summary.asString().empty() && summary.asString().size() <= 320)
        plugin.description = summary.asString();
      if (value.isString() && !value.asString().empty() && value.asString().size() <= 8192) {
        plugin.details = value.asString();
        break;
      }
    }
  }
  if (store["screenshots"].isArray()) {
    for (const auto &value : store["screenshots"]) {
      if (plugin.screenshots.size() == 8) break;
      if (value.isString() && !ScreenshotExtension(value.asString()).empty())
        plugin.screenshots.push_back(value.asString());
    }
  }
}
}  // namespace aeraui::plugins
