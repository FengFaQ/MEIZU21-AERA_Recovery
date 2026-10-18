/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace aeraui {
struct HomePluginPreference {
  std::string id;
  int size = 0;
};
struct HomePluginSize { int width, height; const char *label; };
// Keep the first three IDs stable for layouts saved by earlier builds.
inline constexpr std::array<HomePluginSize, 9> kHomePluginSizes{{
    {1, 1, "1 x 1"}, {2, 1, "2 x 1"}, {2, 2, "2 x 2"},
    {1, 2, "1 x 2"}, {3, 1, "3 x 1"}, {3, 2, "3 x 2"},
    {1, 3, "1 x 3"}, {2, 3, "2 x 3"}, {3, 3, "3 x 3"},
}};
struct HomePluginCell {
  int page = 0, column = 0, row = 0, width = 1, height = 1;
};
inline bool ValidHomePluginPreference(const HomePluginPreference &item) {
  return !item.id.empty() && item.id.size() <= 96 && item.size >= 0 &&
      item.size < static_cast<int>(kHomePluginSizes.size()) &&
      std::all_of(item.id.begin(), item.id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
      });
}
inline std::vector<HomePluginPreference> OrderHomePlugins(
    const std::vector<std::string> &installed,
    const std::vector<HomePluginPreference> &saved) {
  std::vector<HomePluginPreference> result;
  auto append = [&](HomePluginPreference item) {
    if (!ValidHomePluginPreference(item) ||
        std::find(installed.begin(), installed.end(), item.id) == installed.end() ||
        std::any_of(result.begin(), result.end(), [&](const auto &other) { return other.id == item.id; })) return;
    result.push_back(std::move(item));
  };
  for (const auto &item : saved) append(item);
  for (const auto &id : installed) append({id, 0});
  return result;
}
inline std::vector<HomePluginCell> PackHomePlugins(
    const std::vector<HomePluginPreference> &items, int columns, int rows, bool icons) {
  columns = std::clamp(columns, 1, 5); rows = std::clamp(rows, 1, 5);
  const int capacity = columns * rows;
  std::vector<bool> occupied(capacity, false);
  std::vector<HomePluginCell> result;
  (void)icons;
  for (const auto &item : items) {
    const auto size = kHomePluginSizes[std::clamp(item.size, 0, static_cast<int>(kHomePluginSizes.size()) - 1)];
    const int width = std::min(size.width, columns);
    const int height = std::min(size.height, rows);
    // Later small tiles may fit gaps skipped by an earlier wide or tall tile.
    for (int cursor = 0;; ++cursor) {
      while (static_cast<int>(occupied.size()) < (cursor / capacity + 1) * capacity)
        occupied.resize(occupied.size() + capacity, false);
      const int page = cursor / capacity, offset = cursor % capacity;
      const int x = offset % columns, y = offset / columns;
      if (x + width > columns || y + height > rows) continue;
      bool free = true;
      for (int dy = 0; dy < height; ++dy)
        for (int dx = 0; dx < width; ++dx)
          free = free && !occupied[page * capacity + (y + dy) * columns + x + dx];
      if (!free) continue;
      for (int dy = 0; dy < height; ++dy)
        for (int dx = 0; dx < width; ++dx)
          occupied[page * capacity + (y + dy) * columns + x + dx] = true;
      result.push_back({page, x, y, width, height});
      break;
    }
  }
  return result;
}
inline int HomePluginPageCount(const std::vector<HomePluginCell> &cells) {
  int count = 1;
  for (const auto &cell : cells) count = std::max(count, cell.page + 1);
  return count;
}
}  // namespace aeraui
