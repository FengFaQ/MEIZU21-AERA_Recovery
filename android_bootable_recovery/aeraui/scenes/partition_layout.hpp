/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "ui_components.hpp"

namespace aeraui {

inline void LayoutPartitionReview(lv_obj_t *screen, lv_obj_t *list,
                                  lv_obj_t *review, bool selected) {
  const bool landscape = widgets::Landscape(screen);
  const int dock_gap = landscape ? 20 : 24;
  const int bottom = lv_obj_get_height(screen) -
      widgets::NavigationHeight(screen) - dock_gap;
  constexpr int review_height = 150;
  lv_obj_set_pos(review, 80, bottom - review_height);
  lv_obj_set_size(review, landscape ? 940 : 1280, review_height);
  if (selected) lv_obj_remove_flag(review, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(review, LV_OBJ_FLAG_HIDDEN);

  // In portrait the review action shares the list's column. Reserve its
  // space only while visible; in landscape it sits beside the list.
  const int list_bottom = bottom -
      (selected && !landscape ? review_height + 28 : 0);
  lv_obj_update_layout(list);
  lv_obj_set_height(list, std::max(1, list_bottom - lv_obj_get_y(list)));
}

}  // namespace aeraui
