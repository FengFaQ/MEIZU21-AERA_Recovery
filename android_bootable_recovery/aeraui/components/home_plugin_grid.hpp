/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "scene.hpp"
#include <functional>

namespace aeraui {
struct HomePluginGeometry {
  int x, y, width, height, columns, rows, tile_height;
  bool icons;
};
using HomePluginTileFactory = std::function<lv_obj_t *(
    lv_obj_t *, const plugins::Plugin &, int, int, int, int, bool, std::function<void()>)>;
void BuildHomePluginGrid(lv_obj_t *screen, const std::vector<plugins::Plugin> &installed,
                        HomePluginGeometry geometry, HomePluginTileFactory factory,
                        std::function<void(const plugins::Plugin &)> launch,
                        ActionCallback callback, void *context);
}  // namespace aeraui
