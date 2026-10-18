/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <lvgl.h>

namespace aeraui::widgets {

// Creates a complete circular WebKit-inspired compass mask. The A8 artwork
// stays neutral and is recolored with AERA's active accent at runtime.
lv_obj_t *WebKitIconPlate(lv_obj_t *parent, lv_color_t color,
                          int32_t size = 132);

}  // namespace aeraui::widgets
