/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <cstdint>

#include <lvgl.h>

#include "fonts/aera_fallback.hpp"
#include "wallpaper.hpp"
#include <aeraui/i18n.hpp>

namespace aeraui::design {

constexpr lv_color_t Color(uint32_t rgb) {
  return LV_COLOR_MAKE(static_cast<uint8_t>((rgb >> 16) & 0xff),
                       static_cast<uint8_t>((rgb >> 8) & 0xff),
                       static_cast<uint8_t>(rgb & 0xff));
}

// Runtime surface tokens. ApplySurfaceMode() swaps these together so native
// AERA pages never become a mixture of dark and light controls.
inline bool kLightMode = false;
inline lv_color_t kCanvas = Color(0x0d0c0b);
inline lv_color_t kTopbar = Color(0x100e0d);
inline lv_color_t kPanel = Color(0x171411);
inline lv_color_t kPanelStrong = Color(0x1f1b17);
inline lv_color_t kPanelPressed = Color(0x30271f);
inline lv_color_t kInset = Color(0x12100e);
inline lv_color_t kLine = Color(0x3f352c);
inline lv_color_t kLineBright = Color(0x675544);
inline lv_color_t kText = Color(0xf5f7fa);
inline lv_color_t kMuted = Color(0xaeb6c0);
inline lv_color_t kMutedStrong = Color(0xdce2e8);
inline lv_color_t kDim = Color(0x858e99);
constexpr auto kRed = Color(0xff4d55);
inline lv_color_t kRedSoft = Color(0x35191d);
constexpr auto kCyan = Color(0x16c8ff);
inline lv_color_t kCyanSoft = Color(0x122a31);
constexpr auto kGreen = Color(0x42d392);
inline lv_color_t kGreenSoft = Color(0x142b22);
constexpr auto kAmber = Color(0xf4b942);
inline lv_color_t kAmberSoft = Color(0x302715);
constexpr auto kViolet = Color(0xa991ff);

// Shared matte graphite for navigation and unlock; boot keeps its own palette.
inline lv_color_t kMainTop = Color(0x25272b);
inline lv_color_t kMainBottom = Color(0x191b1f);
inline lv_color_t kMainCanvas = Color(0x202226);
inline lv_color_t kMainPanel = Color(0x34373d);
inline lv_color_t kMainSheet = Color(0x2d3036);
inline lv_color_t kMainLine = Color(0x535961);
inline lv_color_t kOnAccent = Color(0x071116);

// Runtime theme tokens. All interactive AERA surfaces consume these shared
// values, so rebuilding a scene after ApplyAccent() recolors the entire UI.
constexpr uint32_t kDefaultAccentRgb = 0x16c8ff;
inline uint32_t kAccentRgb = kDefaultAccentRgb;
inline lv_color_t kAccent = Color(kDefaultAccentRgb);
inline lv_color_t kAccentPressed = Color(0x0aa8db);
inline lv_color_t kAccentSoft = Color(0x17343d);
inline lv_color_t kMainSelected = Color(0x30434a);
inline int kInterfaceSize = 1;
inline int kSurfaceStyle = 0;
inline int kAppearanceMode = 0;

inline void ApplyInterfaceSize(int size) {
  kInterfaceSize = size < 0 ? 0 : size > 3 ? 3 : size;
}

inline const lv_font_t *UiFont(const lv_font_t *font) {
  if (font == nullptr) return nullptr;
  const lv_font_t *selected = font;
  if (kInterfaceSize == 0) {
    if (font == &lv_font_montserrat_48) selected = &lv_font_montserrat_40;
    else if (font == &lv_font_montserrat_40) selected = &lv_font_montserrat_36;
    else if (font == &lv_font_montserrat_36) selected = &lv_font_montserrat_32;
    else if (font == &lv_font_montserrat_32) selected = &lv_font_montserrat_28;
    else if (font == &lv_font_montserrat_24) selected = &lv_font_montserrat_20;
    else if (font == &lv_font_montserrat_20) selected = &lv_font_montserrat_18;
    else if (font == &lv_font_montserrat_18) selected = &lv_font_montserrat_16;
  } else if (kInterfaceSize == 2) {
    if (font == &lv_font_montserrat_40) selected = &lv_font_montserrat_48;
    else if (font == &lv_font_montserrat_36) selected = &lv_font_montserrat_40;
    else if (font == &lv_font_montserrat_32) selected = &lv_font_montserrat_36;
    else if (font == &lv_font_montserrat_24) selected = &lv_font_montserrat_28;
    else if (font == &lv_font_montserrat_20) selected = &lv_font_montserrat_24;
    else if (font == &lv_font_montserrat_18) selected = &lv_font_montserrat_20;
  } else if (kInterfaceSize == 3) {
    if (font == &lv_font_montserrat_40) selected = &lv_font_montserrat_48;
    else if (font == &lv_font_montserrat_36) selected = &lv_font_montserrat_48;
    else if (font == &lv_font_montserrat_32) selected = &lv_font_montserrat_40;
    else if (font == &lv_font_montserrat_28) selected = &lv_font_montserrat_36;
    else if (font == &lv_font_montserrat_24) selected = &lv_font_montserrat_32;
    else if (font == &lv_font_montserrat_20) selected = &lv_font_montserrat_28;
    else if (font == &lv_font_montserrat_18) selected = &lv_font_montserrat_24;
    else if (font == &lv_font_montserrat_16) selected = &lv_font_montserrat_20;
  }
  return fonts::WithLanguageFallback(selected);
}

inline bool IsLightMode() { return kLightMode; }

inline int SurfaceStyleValue() { return kSurfaceStyle; }
inline int AppearanceModeValue() { return kAppearanceMode; }

inline void ApplySurfaceMode(bool light) {
  kAppearanceMode = light ? 1 : 0;
  kLightMode = light;
  if (light) {
    // Warm porcelain rather than pure white: still bright, but comfortable on
    // a phone panel and compatible with AERA's subtle grain.
    kCanvas = Color(0xf5f4f1);
    kTopbar = Color(0xfaf9f6);
    kPanel = Color(0xfdfcf9);
    kPanelStrong = Color(0xf2f0eb);
    kPanelPressed = Color(0xe1dfda);
    kInset = Color(0xe9e7e2);
    kLine = Color(0xc9c7c2);
    kLineBright = Color(0x9c9993);
    kText = Color(0x16191d);
    kMuted = Color(0x595f66);
    kMutedStrong = Color(0x363b41);
    kDim = Color(0x737980);
    kMainTop = Color(0xfcfbf8);
    kMainBottom = Color(0xefede8);
    kMainCanvas = Color(0xf3f2ef);
    kMainPanel = Color(0xfdfcf9);
    kMainSheet = Color(0xeceae5);
    kMainLine = Color(0xb9bdc2);
    kRedSoft = Color(0xf8dfe1);
    kCyanSoft = Color(0xdaf3fa);
    kGreenSoft = Color(0xdff3e9);
    kAmberSoft = Color(0xf6edda);
  } else {
    kCanvas = Color(0x0d0c0b);
    kTopbar = Color(0x100e0d);
    kPanel = Color(0x171411);
    kPanelStrong = Color(0x1f1b17);
    kPanelPressed = Color(0x30271f);
    kInset = Color(0x12100e);
    kLine = Color(0x3f352c);
    kLineBright = Color(0x675544);
    kText = Color(0xf5f7fa);
    kMuted = Color(0xaeb6c0);
    kMutedStrong = Color(0xdce2e8);
    kDim = Color(0x858e99);
    kMainTop = Color(0x25272b);
    kMainBottom = Color(0x191b1f);
    kMainCanvas = Color(0x202226);
    kMainPanel = Color(0x34373d);
    kMainSheet = Color(0x2d3036);
    kMainLine = Color(0x535961);
    kRedSoft = Color(0x35191d);
    kCyanSoft = Color(0x122a31);
    kGreenSoft = Color(0x142b22);
    kAmberSoft = Color(0x302715);
  }
}

inline void ApplyAppearanceMode(int mode) {
  const int appearance = mode < 0 ? 0 : mode > 2 ? 2 : mode;
  ApplySurfaceMode(appearance == 1);
  kAppearanceMode = appearance;
  if (appearance != 2) return;
  kLightMode = false;
  kCanvas = Color(0x000000);
  kTopbar = Color(0x000000);
  kPanel = Color(0x070707);
  kPanelStrong = Color(0x0d0d0d);
  kPanelPressed = Color(0x191919);
  kInset = Color(0x030303);
  kLine = Color(0x25282d);
  kLineBright = Color(0x444950);
  kMainTop = Color(0x000000);
  kMainBottom = Color(0x000000);
  kMainCanvas = Color(0x000000);
  kMainPanel = Color(0x080808);
  kMainSheet = Color(0x0c0c0c);
  kMainLine = Color(0x30343a);
  kRedSoft = Color(0x2c1014);
  kCyanSoft = Color(0x08242c);
  kGreenSoft = Color(0x0a251a);
  kAmberSoft = Color(0x281e08);
}

// Surface style is independent from appearance and accent. It changes the
// material shared by AERA panels without changing the underlying palette.
inline void ApplySurfaceStyle(int style) {
  kSurfaceStyle = style < 0 ? 0 : style > 3 ? 3 : style;
}

inline uint8_t BlendChannel(uint8_t foreground, uint8_t background,
                            uint8_t foreground_percent) {
  return static_cast<uint8_t>(
      (static_cast<uint32_t>(foreground) * foreground_percent +
       static_cast<uint32_t>(background) * (100 - foreground_percent)) /
      100);
}

inline void ApplyAccent(uint32_t rgb) {
  rgb &= 0x00ffffffu;
  if (rgb == 0) rgb = kDefaultAccentRgb;
  kAccentRgb = rgb;
  kAccent = Color(rgb);
  const uint8_t red = static_cast<uint8_t>((rgb >> 16) & 0xff);
  const uint8_t green = static_cast<uint8_t>((rgb >> 8) & 0xff);
  const uint8_t blue = static_cast<uint8_t>(rgb & 0xff);
  kAccentPressed = LV_COLOR_MAKE(
      static_cast<uint8_t>(red * 82 / 100),
      static_cast<uint8_t>(green * 82 / 100),
      static_cast<uint8_t>(blue * 82 / 100));
  kAccentSoft = LV_COLOR_MAKE(
      BlendChannel(red, kMainCanvas.red, 20),
      BlendChannel(green, kMainCanvas.green, 20),
      BlendChannel(blue, kMainCanvas.blue, 20));
  kMainSelected = LV_COLOR_MAKE(
      BlendChannel(red, kMainPanel.red, 16),
      BlendChannel(green, kMainPanel.green, 16),
      BlendChannel(blue, kMainPanel.blue, 16));
}

inline uint32_t AccentRgb() { return kAccentRgb; }

inline void NoScroll(lv_obj_t *object) {
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

inline void Clear(lv_obj_t *object) {
  NoScroll(object);
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_pad_all(object, 0, 0);
}

inline lv_obj_t *Label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  i18n::BindLabel(label, text);
  lv_obj_set_style_text_font(label, UiFont(font), 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

// Decorative artwork has fixed geometry and must not be reflowed by the
// user's Density setting. Use this only for brand marks and generated art;
// readable interface copy should continue through Label()/UiFont().
inline lv_obj_t *FixedLabel(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  i18n::BindLabel(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

inline void SingleLineLabel(lv_obj_t *label, int width,
                            const lv_font_t *font) {
  lv_obj_set_size(label, width, lv_font_get_line_height(UiFont(font)));
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

inline void Screen(lv_obj_t *screen) {
  NoScroll(screen);
  i18n::ApplyDirection(screen);
  lv_obj_set_style_bg_color(screen, kCanvas, 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

struct MatteTexture {
  // Matches the native UI's 1440 x 3168 design surface. Storage is static BSS,
  // not a bitmap embedded in the image or a large temporary on the UI stack.
  static constexpr int kWidth = 1440, kHeight = 3168;
  alignas(64) std::array<uint32_t, kWidth * kHeight> pixels;
  lv_image_dsc_t image{};
  lv_image_dsc_t landscape_image{};

  explicit MatteTexture(lv_color_t base) {
    uint32_t seed = 0xa3e14b79;
    for (int y = 0; y < kHeight; y += 2) {
      for (int x = 0; x < kWidth; x += 2) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const uint8_t alpha = static_cast<uint8_t>(seed % 10);
        // Composite the grain once. XRGB matches scanout, allowing an opaque
        // copy during scroll instead of reading/blending the DRM framebuffer.
        const auto grain = [alpha](uint8_t base) -> uint32_t {
          return (uint32_t(base) * (255 - alpha) + 255u * alpha) / 255;
        };
        const uint32_t pixel = 0xff000000u | (grain(base.red) << 16) |
            (grain(base.green) << 8) | grain(base.blue);
        pixels[y * kWidth + x] = pixels[y * kWidth + x + 1] = pixel;
        pixels[(y + 1) * kWidth + x] = pixels[(y + 1) * kWidth + x + 1] = pixel;
      }
    }
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_XRGB8888;
    image.header.w = kWidth;
    image.header.stride = kWidth * sizeof(uint32_t);
    image.header.h = kHeight;
    image.data_size = sizeof(pixels);
    image.data = reinterpret_cast<const uint8_t *>(pixels.data());
    // Portrait and landscape contain the same number of pixels. Reusing the
    // same continuous noise allocation with a swapped stride gives landscape
    // one full 3168x1440 surface without another ~18 MiB texture or a visible
    // tiled seam at x=1440.
    landscape_image = image;
    landscape_image.header.w = kHeight;
    landscape_image.header.h = kWidth;
    landscape_image.header.stride = kHeight * sizeof(uint32_t);
  }
};

inline void MainBackground(lv_obj_t *screen) {
  Screen(screen);
  // A user wallpaper replaces only the canvas. Appearance still supplies the
  // text, panel, dialog and navigation colours layered above it.
  if (wallpaper::Attach(screen)) return;
  if (kAppearanceMode == 2) {
    lv_obj_set_style_bg_color(screen, kMainCanvas, 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_image_src(screen, nullptr, 0);
    return;
  }
  // One continuous opaque surface shared by every page: no tile boundaries,
  // per-frame generation or extra draw objects. Separate descriptors prevent
  // LVGL's image/GPU cache from retaining graphite after a live mode change.
  // Each texture is created only if that appearance is actually used.
  const auto texture_for_mode = []() -> const MatteTexture & {
    if (IsLightMode()) {
      static const MatteTexture light(Color(0xf3f2ef));
      return light;
    }
    static const MatteTexture graphite(Color(0x202226));
    return graphite;
  };
  const auto &texture = texture_for_mode();
  // An 8-bit near-black gradient had only ~12 levels across the display,
  // producing visible horizontal bands even with a continuous grain mask.
  lv_obj_set_style_bg_color(screen, kMainCanvas, 0);
  lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
  const bool landscape = lv_obj_get_width(screen) > lv_obj_get_height(screen);
  lv_obj_set_style_bg_image_src(
      screen, landscape ? &texture.landscape_image : &texture.image, 0);
  lv_obj_set_style_bg_image_tiled(screen, false, 0);
  lv_obj_set_style_bg_image_recolor(screen, lv_color_white(), 0);
  lv_obj_set_style_bg_image_recolor_opa(screen, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_image_opa(screen, LV_OPA_COVER, 0);
}

inline void Panel(lv_obj_t *object, int32_t radius = 28,
                  lv_color_t fill = kPanel) {
  NoScroll(object);
  lv_obj_set_style_radius(object, radius, 0);
  lv_obj_set_style_bg_color(object, fill, 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_border_color(object, kMainLine, 0);
  // Keep opacity ready for callers which enable a selected-state border after
  // Panel(); width zero remains the neutral default.
  lv_obj_set_style_border_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(object, 0, 0);
  lv_obj_set_style_shadow_opa(object, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_offset_x(object, 0, 0);
  lv_obj_set_style_shadow_offset_y(object, 0, 0);
  lv_obj_set_style_blur_backdrop(object, false, 0);
  lv_obj_set_style_blur_radius(object, 0, 0);

  switch (kSurfaceStyle) {
    case 1:  // Frosted glass.
      lv_obj_set_style_bg_opa(object, kLightMode ? LV_OPA_80 : LV_OPA_70, 0);
      lv_obj_set_style_border_width(object, 1, 0);
      lv_obj_set_style_border_opa(object, LV_OPA_50, 0);
      lv_obj_set_style_blur_backdrop(object, true, 0);
      lv_obj_set_style_blur_radius(object, 16, 0);
      lv_obj_set_style_blur_quality(object, LV_BLUR_QUALITY_SPEED, 0);
      break;
    case 2:  // Outline.
      lv_obj_set_style_bg_opa(object, LV_OPA_10, 0);
      lv_obj_set_style_border_width(object, 2, 0);
      lv_obj_set_style_border_opa(object, LV_OPA_60, 0);
      break;
    case 3:  // Elevated.
      lv_obj_set_style_border_width(object, 1, 0);
      lv_obj_set_style_border_opa(object, LV_OPA_30, 0);
      lv_obj_set_style_shadow_color(object, Color(0x000000), 0);
      lv_obj_set_style_shadow_width(object, 22, 0);
      lv_obj_set_style_shadow_offset_y(object, 6, 0);
      lv_obj_set_style_shadow_opa(object,
                                  kLightMode ? LV_OPA_20 : LV_OPA_40, 0);
      break;
    default:  // Solid uses opaque, shadow-free surfaces.
      break;
  }
}

inline void Interactive(lv_obj_t *object, lv_color_t pressed = kPanelPressed) {
  lv_obj_set_style_bg_color(object, pressed, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(object, kLineBright, LV_STATE_PRESSED);
  lv_obj_set_style_transform_scale(object, 253, LV_STATE_PRESSED);
}

inline void AccentButton(lv_obj_t *object) {
  NoScroll(object);
  lv_obj_set_style_radius(object, 24, 0);
  lv_obj_set_style_bg_color(object, kAccent, 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(object, kAccentPressed, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_transform_scale(object, 253, LV_STATE_PRESSED);
}

// AERA's shared range-control treatment. Keep the actual rail deliberately
// slim, then expand the knob and click area so it remains comfortable on a
// phone. Explicit opacity on every slider part is important: LVGL's inherited
// slider theme can otherwise leave the rail transparent in the pull-down shade.
inline void RangeSlider(lv_obj_t *slider) {
  NoScroll(slider);
  lv_obj_set_ext_click_area(slider, 32);

  lv_obj_set_style_bg_color(slider, kInset, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(slider, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(slider, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_border_opa(slider, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_set_style_bg_color(slider, kAccent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_border_width(slider, 0, LV_PART_INDICATOR);

  lv_obj_set_style_bg_color(slider, kText, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_border_width(slider, 6, LV_PART_KNOB);
  lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
  lv_obj_set_style_border_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 18, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
}

inline void AnimateEnter(lv_obj_t *object, uint32_t delay,
                         int32_t distance = 18) {
  lv_obj_set_style_translate_y(object, distance, 0);
  lv_obj_set_style_opa(object, LV_OPA_40, 0);

  lv_anim_t move;
  lv_anim_init(&move);
  lv_anim_set_var(&move, object);
  lv_anim_set_values(&move, distance, 0);
  lv_anim_set_duration(&move, 280);
  lv_anim_set_delay(&move, delay);
  lv_anim_set_path_cb(&move, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&move, [](void *target, int32_t value) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(target), value, 0);
  });
  lv_anim_start(&move);

  lv_anim_t fade;
  lv_anim_init(&fade);
  lv_anim_set_var(&fade, object);
  lv_anim_set_values(&fade, LV_OPA_40, LV_OPA_COVER);
  lv_anim_set_duration(&fade, 220);
  lv_anim_set_delay(&fade, delay);
  lv_anim_set_exec_cb(&fade, [](void *target, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(target), value, 0);
  });
  lv_anim_start(&fade);
}

inline lv_obj_t *Kicker(lv_obj_t *parent, const char *text,
                        lv_color_t color = kDim) {
  lv_obj_t *label = Label(parent, text, &lv_font_montserrat_18, color);
  lv_obj_set_style_text_letter_space(label, 3, 0);
  return label;
}

inline lv_obj_t *IconPlate(lv_obj_t *parent, const char *symbol,
                           lv_color_t accent, lv_color_t fill,
                           int32_t size = 88) {
  lv_obj_t *plate = lv_obj_create(parent);
  NoScroll(plate);
  lv_obj_set_size(plate, size, size);
  lv_obj_set_style_radius(plate, 0, 0);
  lv_obj_set_style_bg_color(plate, fill, 0);
  lv_obj_set_style_bg_opa(plate, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(plate, 0, 0);
  lv_obj_set_style_pad_all(plate, 0, 0);
  lv_obj_t *icon = Label(plate, symbol, &lv_font_montserrat_32, accent);
  lv_obj_center(icon);
  return plate;
}

inline lv_obj_t *Divider(lv_obj_t *parent, int32_t width) {
  lv_obj_t *line = lv_obj_create(parent);
  NoScroll(line);
  lv_obj_set_size(line, width, 2);
  lv_obj_set_style_radius(line, 0, 0);
  lv_obj_set_style_bg_color(line, kLine, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(line, 0, 0);
  return line;
}

} // namespace aeraui::design
