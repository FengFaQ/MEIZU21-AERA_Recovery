/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <lvgl.h>

namespace aeraui::assets {

// Shared monochrome masks used by the lock screen and branded system pages.
// Keeping the large alpha assets in one translation unit avoids embedding a
// second copy of the logo in recovery when another page needs it.
extern const lv_image_dsc_t kAeraLogo;
extern const lv_image_dsc_t kAeraLogoInnerDark;
extern const lv_image_dsc_t kAeraLogoInnerLight;

inline constexpr int32_t kAeraBrandWidth = 846;
inline constexpr int32_t kAeraBrandHeight = 791;
extern const lv_image_dsc_t kAeraBrandWhite;
extern const lv_image_dsc_t kAeraBrandAccent;

}  // namespace aeraui::assets
