/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>
#include "design.hpp"

namespace aeraui::widgets {
constexpr int kWifiIndicatorWidth = 64;

// AOSP's default config_wifiRssiLevelThresholds, from weakest to strongest.
inline int WifiSignalLevel(int dbm) {
  if (dbm < -127 || dbm >= 0) return -1;
  int level = 0;
  for (const int threshold : {-88, -77, -66, -55})
    if (dbm >= threshold) ++level;
  return level;
}

struct WifiIndicator {
  lv_obj_t *root = nullptr;
  std::array<lv_obj_t *, 3> arcs{};
  lv_obj_t *dot = nullptr;
};

inline WifiIndicator CreateWifiIndicator(lv_obj_t *parent) {
  using namespace design;
  WifiIndicator indicator;
  indicator.root = lv_obj_create(parent);
  Clear(indicator.root);
  lv_obj_remove_flag(indicator.root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(indicator.root, kWifiIndicatorWidth, 44);
  for (int i = 0; i < 3; ++i) {
    auto *arc = lv_arc_create(indicator.root);
    indicator.arcs[i] = arc;
    const int size = 32 + i * 16;
    lv_obj_set_pos(arc, (kWifiIndicatorWidth - size) / 2, 34 - size / 2);
    lv_obj_set_size(arc, size, size);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc, 0, 0);
    lv_obj_set_style_pad_all(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    lv_arc_set_bg_angles(arc, 225, 315);
  }
  indicator.dot = lv_obj_create(indicator.root);
  Clear(indicator.dot);
  lv_obj_remove_flag(indicator.dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(indicator.dot, 27, 29);
  lv_obj_set_size(indicator.dot, 10, 10);
  lv_obj_set_style_radius(indicator.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(indicator.dot, kMutedStrong, 0);
  lv_obj_set_style_bg_opa(indicator.dot, LV_OPA_COVER, 0);
  return indicator;
}

inline void UpdateWifiIndicator(const WifiIndicator &indicator, int dbm) {
  using namespace design;
  const int level = WifiSignalLevel(dbm);
  for (int i = 0; i < 3; ++i) {
    lv_obj_set_style_arc_color(indicator.arcs[i], kMutedStrong, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(indicator.arcs[i], level >= i + 2 ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
  }
  lv_obj_set_style_bg_opa(indicator.dot, level >= 1 ? LV_OPA_COVER : LV_OPA_30, 0);
}

}  // namespace aeraui::widgets
