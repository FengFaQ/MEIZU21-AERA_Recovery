/* SPDX-License-Identifier: Apache-2.0 */
#include "aeraui/home_layout.hpp"
#include <cassert>
#include <cstdio>
#include <set>
#include <tuple>
using namespace aeraui;
int main() {
  const auto order = OrderHomePlugins({"one", "two", "new", "one"},
      {{"two", 2}, {"gone", 1}, {"two", 0}, {"one", 1}, {"../bad", 0}});
  assert(order.size() == 3 && order[0].id == "two" && order[0].size == 2);
  assert(order[1].id == "one" && order[2].id == "new" && order[2].size == 0);
  assert(!ValidHomePluginPreference({"bad/id", 0}));
  assert(!ValidHomePluginPreference({"ok", 9}));
  assert(ValidHomePluginPreference({"ok", 8}));
  const auto full_width = PackHomePlugins({{"full", 4}}, 3, 3, false);
  assert(full_width[0].width == 3 && full_width[0].height == 1);
  const auto gap = PackHomePlugins({{"A", 0}, {"B", 0}, {"C", 1}, {"D", 0}}, 3, 3, false);
  assert(gap[2].row == 1 && gap[2].column == 0 && gap[2].width == 2);
  assert(gap[3].page == 0 && gap[3].row == 0 && gap[3].column == 2);
  const auto earlier_page = PackHomePlugins({{"large-A", 2}, {"large-B", 2}, {"small", 0}}, 3, 3, false);
  assert(earlier_page[1].page == 1 && earlier_page[2].page == 0);
  assert(HomePluginPageCount(earlier_page) == 2);
  assert(HomePluginPageCount({}) == 1);
  for (int columns = 2; columns <= 5; ++columns) {
    for (int rows : {1, 3, 5}) {
      for (bool icons : {false, true}) {
        std::vector<HomePluginPreference> items;
        for (int i = 0; i < 80; ++i) items.push_back({std::to_string(i), i % 9});
        const auto cells = PackHomePlugins(items, columns, rows, icons);
        assert(cells.size() == items.size());
        std::set<std::tuple<int, int, int>> occupied;
        for (const auto &cell : cells) {
          assert(cell.page >= 0 && cell.page < HomePluginPageCount(cells));
          assert(cell.column >= 0 && cell.column + cell.width <= columns);
          assert(cell.row >= 0 && cell.row + cell.height <= rows);
          assert(cell.width == std::min(columns, kHomePluginSizes[items[&cell - cells.data()].size].width));
          for (int y = 0; y < cell.height; ++y)
            for (int x = 0; x < cell.width; ++x)
              assert(occupied.emplace(cell.page, cell.column + x, cell.row + y).second);
        }
      }
    }
  }
  puts("Home plugin order, stale entries, sizes, page packing and icon grids passed.");
}
