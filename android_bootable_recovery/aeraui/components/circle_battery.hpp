/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <cstdio>
#include "design.hpp"
#include "aeraui/backend.hpp"

namespace aeraui::widgets {

struct CircleBattery {
  lv_obj_t *root = nullptr;
  lv_obj_t *ring = nullptr;
  lv_obj_t *number = nullptr;
  lv_obj_t *charge = nullptr;
};

inline void ResizeCircleBattery(const CircleBattery &battery, int size) {
  using namespace design;
  size = std::clamp(size, kBatteryCircleSizeMin, kBatteryCircleSizeMax);
  if (lv_obj_get_width(battery.root) == size) return;
  lv_obj_set_size(battery.root, size, size);
  const int inset = std::max(2, size / 20);
  const int stroke = std::max(3, size / 16);
  lv_obj_set_pos(battery.ring, inset, inset);
  lv_obj_set_size(battery.ring, size - inset * 2, size - inset * 2);
  lv_obj_set_style_arc_width(battery.ring, stroke, LV_PART_MAIN);
  lv_obj_set_style_arc_width(battery.ring, stroke, LV_PART_INDICATOR);
  const lv_font_t *font = size < 56 ? &lv_font_montserrat_18 :
      size < 68 ? &lv_font_montserrat_24 : size < 84 ? &lv_font_montserrat_28 : &lv_font_montserrat_32;
  lv_obj_set_style_text_font(battery.number, UiFont(font), 0);
  lv_obj_center(battery.number);
  lv_obj_align(battery.charge, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
}

inline CircleBattery CreateCircleBattery(lv_obj_t *parent) {
  using namespace design;
  CircleBattery battery;
  battery.root = lv_obj_create(parent);
  Clear(battery.root);
  lv_obj_remove_flag(battery.root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(battery.root, 80, 80);
  battery.ring = lv_arc_create(battery.root);
  lv_obj_set_pos(battery.ring, 4, 4);
  lv_obj_set_size(battery.ring, 72, 72);
  lv_obj_remove_flag(battery.ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(battery.ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(battery.ring, 0, 0);
  lv_obj_set_style_pad_all(battery.ring, 0, 0);
  lv_obj_set_style_arc_width(battery.ring, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_color(battery.ring, kMuted, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(battery.ring, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_arc_width(battery.ring, 5, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(battery.ring, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(battery.ring, 0, LV_PART_KNOB);
  lv_arc_set_rotation(battery.ring, 270);
  lv_arc_set_bg_angles(battery.ring, 0, 360);
  lv_arc_set_range(battery.ring, 0, 100);
  battery.number = Label(battery.root, "", &lv_font_montserrat_28, kMutedStrong);
  lv_obj_center(battery.number);
  battery.charge = Label(battery.root, LV_SYMBOL_CHARGE, &lv_font_montserrat_16, kGreen);
  lv_obj_set_pos(battery.charge, 64, 58);
  ResizeCircleBattery(battery, kBatteryCircleSizeDefault);
  return battery;
}

inline void UpdateCircleBattery(const CircleBattery &battery, int percent, bool charging) {
  using namespace design;
  const int level = std::clamp(percent, 0, 100);
  lv_arc_set_value(battery.ring, level);
  lv_obj_set_style_arc_color(battery.ring,
      level <= 15 ? kRed : charging ? kGreen : kText, LV_PART_INDICATOR);
  char text[8] = "";
  if (percent < 0) snprintf(text, sizeof(text), "--");
  else if (level < 100) snprintf(text, sizeof(text), "%d", level);
  lv_label_set_text(battery.number, text);
  lv_obj_center(battery.number);
  if (charging) lv_obj_remove_flag(battery.charge, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(battery.charge, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace aeraui::widgets
