/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <lvgl.h>

namespace aeraui::widgets {

// Creates a transparent icon plate containing RetroArch's official invader
// mark. The source artwork remains licensed and attributed with RetroArch.
lv_obj_t *RetroArchIconPlate(lv_obj_t *parent, lv_color_t color,
                             int32_t size = 88);

}  // namespace aeraui::widgets
