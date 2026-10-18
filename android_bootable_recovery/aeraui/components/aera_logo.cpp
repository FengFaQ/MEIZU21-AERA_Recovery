/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "aera_logo.hpp"

namespace aeraui::assets {
namespace {

#include "aera_lock_logo.inc"
#include "aera_lock_inner_dark.inc"
#include "aera_lock_inner_light.inc"
#include "aera_boot_final_white.inc"
#include "aera_boot_final_cyan.inc"

}  // namespace

const lv_image_dsc_t kAeraLogo = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = 900,
               .h = 684,
               .stride = 900},
    .data_size = sizeof(_tmp_aera_lock_logo_raw),
    .data = _tmp_aera_lock_logo_raw,
};

const lv_image_dsc_t kAeraLogoInnerDark = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = 900,
               .h = 684,
               .stride = 900},
    .data_size = sizeof(_tmp_aera_lock_inner_dark_raw),
    .data = _tmp_aera_lock_inner_dark_raw,
};

const lv_image_dsc_t kAeraLogoInnerLight = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = 900,
               .h = 684,
               .stride = 900},
    .data_size = sizeof(_tmp_aera_lock_inner_light_raw),
    .data = _tmp_aera_lock_inner_light_raw,
};

const lv_image_dsc_t kAeraBrandWhite = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = kAeraBrandWidth,
               .h = kAeraBrandHeight,
               .stride = kAeraBrandWidth},
    .data_size = sizeof(_tmp_aera_boot_final_white_raw),
    .data = _tmp_aera_boot_final_white_raw,
};

const lv_image_dsc_t kAeraBrandAccent = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = kAeraBrandWidth,
               .h = kAeraBrandHeight,
               .stride = kAeraBrandWidth},
    .data_size = sizeof(_tmp_aera_boot_final_cyan_raw),
    .data = _tmp_aera_boot_final_cyan_raw,
};

}  // namespace aeraui::assets
