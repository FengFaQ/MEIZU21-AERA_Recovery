/* SPDX-License-Identifier: Apache-2.0 */
#include "webkit_icon.hpp"

#include "design.hpp"

namespace aeraui::widgets {
namespace {

constexpr int32_t kIconWidth = 96;
constexpr int32_t kIconHeight = 96;

#include "webkit_icon.inc"

const lv_image_dsc_t kWebKitIcon = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_A8,
        .flags = 0,
        .w = kIconWidth,
        .h = kIconHeight,
        .stride = kIconWidth,
    },
    .data_size = sizeof(kWebKitIconPixels),
    .data = kWebKitIconPixels,
};

}  // namespace

lv_obj_t *WebKitIconPlate(lv_obj_t *parent, lv_color_t color, int32_t size) {
  auto *plate = lv_obj_create(parent);
  design::Clear(plate);
  lv_obj_set_size(plate, size, size);

  auto *icon = lv_image_create(plate);
  lv_image_set_src(icon, &kWebKitIcon);
  lv_image_set_scale(icon, static_cast<uint32_t>(size * 256 / kIconWidth));
  lv_obj_set_style_image_recolor(icon, color, 0);
  lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
  lv_obj_center(icon);
  return plate;
}

}  // namespace aeraui::widgets
