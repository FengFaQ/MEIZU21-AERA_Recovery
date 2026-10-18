/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "design.hpp"
#include <algorithm>

namespace aeraui {
// Four translucent gradients rather than a blurred screen copy: no readback
// or intermediate full-screen layer, and the middle remains unobscured.
inline lv_obj_t *ScreenshotFlash(lv_obj_t *parent) {
  auto *overlay = lv_obj_create(parent);
  design::Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_remove_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_update_layout(overlay);
  const int width = lv_obj_get_width(overlay), height = lv_obj_get_height(overlay);
  const int depth = std::min(width, height) / 5;
  for (int side = 0; side < 4; ++side) {
    auto *edge = lv_obj_create(overlay);
    design::Clear(edge);
    lv_obj_remove_flag(edge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(edge, LV_OBJ_FLAG_SCROLLABLE);
    const bool vertical = side < 2, reverse = side == 1 || side == 3;
    lv_obj_set_size(edge, vertical ? width : depth, vertical ? depth : height);
    lv_obj_align(edge, side == 0 ? LV_ALIGN_TOP_MID : side == 1 ? LV_ALIGN_BOTTOM_MID :
                       side == 2 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(edge, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_color(edge, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_dir(edge, vertical ? LV_GRAD_DIR_VER : LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_main_opa(edge, reverse ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(edge, reverse ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(edge, LV_OPA_TRANSP, 0);
  }
  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, overlay);
  lv_anim_set_values(&pulse, 0, 190);
  lv_anim_set_duration(&pulse, 100);
  lv_anim_set_playback_duration(&pulse, 380);
  lv_anim_set_path_cb(&pulse, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&pulse, [](void *target, int32_t opacity) {
    auto *root = static_cast<lv_obj_t *>(target);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
      lv_obj_set_style_bg_opa(lv_obj_get_child(root, i), opacity, 0);
  });
  lv_anim_start(&pulse);
  lv_obj_delete_delayed(overlay, 520);
  return overlay;
}
}  // namespace aeraui
