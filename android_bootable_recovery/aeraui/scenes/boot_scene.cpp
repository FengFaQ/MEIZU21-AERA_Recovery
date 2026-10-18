/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

#include <lvgl.h>
#include "src/misc/cache/instance/lv_image_cache.h"

#include "aera_logo.hpp"
#include "design.hpp"
#include "aeraui/engine.hpp"

namespace aeraui {
namespace {
using namespace design;

// Assets are prefiltered to the Dodge panel's 94%-width boot canvas.  This
// keeps the intro at a 1:1 pixel scale instead of asking LVGL to upscale the
// 1254 px reference on every frame (which made diagonal edges look jagged).
constexpr int32_t kReferenceSize = 1353;
constexpr int32_t kAssetCount = 9;
constexpr int32_t kArrowWidth = 543;
constexpr int32_t kArrowHeight = 363;
constexpr int32_t kArrowBytes = kArrowWidth * kArrowHeight;
constexpr int32_t kFinalX = 254;
constexpr int32_t kFinalY = 256;
constexpr int32_t kFinalWidth = 846;
constexpr int32_t kFinalHeight = 791;

enum AssetIndex {
  kMidLeft,
  kTop,
  kMidRight,
  kBottomLeft,
  kCenter,
  kBottomRight,
  kArrow,
  kWord,
  kSubtitle,
};

#include "aera_boot_top.inc"
#include "aera_boot_mid_left.inc"
#include "aera_boot_mid_right.inc"
#include "aera_boot_bottom_left.inc"
#include "aera_boot_center.inc"
#include "aera_boot_bottom_right.inc"
#include "aera_boot_arrow.inc"
#include "aera_boot_word.inc"
#include "aera_boot_subtitle.inc"
#define AERA_BOOT_IMAGE(name, pixels, width, height)                       \
  static const lv_image_dsc_t name = {                                    \
      .header = {.magic = LV_IMAGE_HEADER_MAGIC,                           \
                 .cf = LV_COLOR_FORMAT_A8,                                \
                 .flags = 0,                                              \
                 .w = width,                                              \
                 .h = height,                                             \
                 .stride = width},                                        \
      .data_size = sizeof(pixels),                                        \
      .data = pixels,                                                      \
  }

AERA_BOOT_IMAGE(kTopImage, _tmp_aera_boot_top_raw, 213, 123);
AERA_BOOT_IMAGE(kMidLeftImage, _tmp_aera_boot_mid_left_raw, 179, 121);
AERA_BOOT_IMAGE(kMidRightImage, _tmp_aera_boot_mid_right_raw, 183, 121);
AERA_BOOT_IMAGE(kBottomLeftImage, _tmp_aera_boot_bottom_left_raw, 220, 138);
AERA_BOOT_IMAGE(kCenterImage, _tmp_aera_boot_center_raw, 142, 126);
AERA_BOOT_IMAGE(kBottomRightImage, _tmp_aera_boot_bottom_right_raw, 208, 138);
AERA_BOOT_IMAGE(kWordImage, _tmp_aera_boot_word_raw, 846, 168);
AERA_BOOT_IMAGE(kSubtitleImage, _tmp_aera_boot_subtitle_raw, 846, 50);
#undef AERA_BOOT_IMAGE

struct AssetGeometry {
  const lv_image_dsc_t *source;
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

const AssetGeometry kGeometry[kAssetCount] = {
    {&kMidLeftImage, 495, 486, 179, 121},
    {&kTopImage, 569, 356, 213, 123},
    {&kMidRightImage, 667, 486, 183, 121},
    {&kBottomLeftImage, 410, 613, 220, 138},
    {&kCenterImage, 601, 625, 142, 126},
    {&kBottomRightImage, 722, 613, 208, 138},
    {nullptr, 421, 256, kArrowWidth, kArrowHeight},
    {&kWordImage, 254, 800, 846, 168},
    {&kSubtitleImage, 254, 997, 846, 50},
};

struct BootContext {
  ActionCallback callback;
  void *context;
  lv_obj_t *objects[kAssetCount];
  lv_obj_t *final_white;
  lv_obj_t *final_cyan;
  lv_image_dsc_t arrow_descriptor;
  uint8_t *arrow_pixels;
  uint8_t *arrow_order;
  int32_t final_x[kAssetCount];
  int32_t final_y[kAssetCount];
  int32_t width;
  int32_t height;
  int32_t base_scale;
  uint32_t pulse_scale_q16;
  bool ready;
  bool intro_complete;
  bool exiting;
};

void SetImageOpacity(void *object, int32_t value) {
  lv_obj_set_style_image_opa(static_cast<lv_obj_t *>(object), value, 0);
}

void SetObjectX(void *object, int32_t value) {
  lv_obj_set_x(static_cast<lv_obj_t *>(object), value);
}

void SetObjectY(void *object, int32_t value) {
  lv_obj_set_y(static_cast<lv_obj_t *>(object), value);
}

int32_t ScaleOffset(int32_t origin, int32_t extent, int32_t scale) {
  return (origin * 2 + extent - kReferenceSize) * scale /
         (2 * static_cast<int32_t>(LV_SCALE_NONE));
}

int32_t ScaleDistance(int32_t distance, int32_t scale) {
  return distance * scale / static_cast<int32_t>(LV_SCALE_NONE);
}

void PositionAsset(BootContext *boot, int32_t index, int32_t scale) {
  auto *object = boot->objects[index];
  if (object == nullptr) return;
  const auto &geometry = kGeometry[index];
  lv_image_set_scale(object, static_cast<uint32_t>(scale));
  lv_obj_align(object, LV_ALIGN_CENTER,
               ScaleOffset(geometry.x, geometry.width, scale),
               ScaleOffset(geometry.y, geometry.height, scale));
}

void PositionFinalObject(lv_obj_t *object, int32_t scale) {
  if (object == nullptr) return;
  lv_image_set_scale(object, static_cast<uint32_t>(scale));
  lv_obj_align(object, LV_ALIGN_CENTER,
               ScaleOffset(kFinalX, kFinalWidth, scale),
               ScaleOffset(kFinalY, kFinalHeight, scale));
}

void SetLogoScale(void *object, int32_t value) {
  auto *boot = static_cast<BootContext *>(object);
  if (boot == nullptr) return;
  PositionFinalObject(boot->final_white, value);
  PositionFinalObject(boot->final_cyan, value);
}

void ApplyFineLogoScale(lv_event_t *event) {
  auto *boot = static_cast<BootContext *>(lv_event_get_user_data(event));
  if (boot == nullptr || boot->pulse_scale_q16 == 0) return;
  auto *draw = lv_draw_task_get_image_dsc(lv_event_get_draw_task(event));
  if (draw == nullptr) return;
  draw->gpu_scale_x_q16 = boot->pulse_scale_q16;
  draw->gpu_scale_y_q16 = boot->pulse_scale_q16;
}

void SetFineLogoScale(void *object, int32_t value) {
  auto *boot = static_cast<BootContext *>(object);
  if (boot == nullptr) return;
  boot->pulse_scale_q16 = static_cast<uint32_t>(value);
  lv_obj_invalidate(boot->final_white);
  lv_obj_invalidate(boot->final_cyan);
}

void ShowFinalLogo(BootContext *boot) {
  if (boot == nullptr) return;
  for (auto *image : boot->objects)
    lv_obj_set_style_image_opa(image, LV_OPA_TRANSP, 0);
  PositionFinalObject(boot->final_white, boot->base_scale);
  PositionFinalObject(boot->final_cyan, boot->base_scale);
  lv_obj_set_style_image_opa(boot->final_white, LV_OPA_COVER, 0);
  lv_obj_set_style_image_opa(boot->final_cyan, LV_OPA_COVER, 0);
}

lv_obj_t *CreateImage(const lv_image_dsc_t *source, lv_color_t color) {
  auto *image = lv_image_create(lv_layer_top());
  lv_image_set_src(image, source);
  lv_image_set_antialias(image, true);
  lv_obj_set_style_image_recolor(image, color, 0);
  lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
  lv_obj_set_style_image_opa(image, LV_OPA_TRANSP, 0);
  return image;
}

void AnimateValue(void *object, lv_anim_exec_xcb_t callback, int32_t from,
                  int32_t to, uint32_t delay, uint32_t duration,
                  lv_anim_path_cb_t path) {
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, object);
  lv_anim_set_values(&animation, from, to);
  lv_anim_set_delay(&animation, delay);
  lv_anim_set_duration(&animation, duration);
  lv_anim_set_path_cb(&animation, path);
  lv_anim_set_exec_cb(&animation, callback);
  lv_anim_start(&animation);
}

void AnimatePiece(BootContext *boot, int32_t index, int32_t delta_x,
                  int32_t delta_y, uint32_t delay, uint32_t duration) {
  auto *object = boot->objects[index];
  // lv_obj_get_x/y() above return the resolved absolute position, while an
  // aligned object's lv_obj_set_x/y() values are interpreted as offsets. Move
  // only animated A pieces to top-left positioning before changing x/y.
  lv_obj_set_align(object, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(object, boot->final_x[index], boot->final_y[index]);
  const int32_t start_x =
      boot->final_x[index] + ScaleDistance(delta_x, boot->base_scale);
  const int32_t start_y =
      boot->final_y[index] + ScaleDistance(delta_y, boot->base_scale);
  lv_obj_set_x(object, start_x);
  lv_obj_set_y(object, start_y);
  lv_obj_set_style_image_opa(object, LV_OPA_TRANSP, 0);
  AnimateValue(object, SetObjectX, start_x, boot->final_x[index], delay,
               duration, lv_anim_path_ease_out);
  AnimateValue(object, SetObjectY, start_y, boot->final_y[index], delay,
               duration, lv_anim_path_ease_out);
  AnimateValue(object, SetImageOpacity, LV_OPA_TRANSP, LV_OPA_COVER, delay,
               duration * 2 / 5, lv_anim_path_ease_in_out);
}

void SetArrowProgress(void *object, int32_t value) {
  auto *boot = static_cast<BootContext *>(object);
  if (boot == nullptr || boot->arrow_pixels == nullptr) return;
  for (int32_t i = 0; i < kArrowBytes; ++i) {
    const int32_t lead = value - boot->arrow_order[i];
    if (lead <= 0) {
      boot->arrow_pixels[i] = 0;
    } else if (lead >= 10) {
      boot->arrow_pixels[i] = _tmp_aera_boot_arrow_raw[i];
    } else {
      boot->arrow_pixels[i] = static_cast<uint8_t>(
          static_cast<int32_t>(_tmp_aera_boot_arrow_raw[i]) * lead / 10);
    }
  }
  lv_image_cache_drop(&boot->arrow_descriptor);
  lv_obj_invalidate(boot->objects[kArrow]);
}

void SetWordProgress(void *object, int32_t value) {
  auto *boot = static_cast<BootContext *>(object);
  lv_obj_set_style_image_opa(boot->objects[kWord],
                             static_cast<lv_opa_t>(value), 0);
  lv_obj_set_y(boot->objects[kWord], boot->final_y[kWord] +
      ScaleDistance((LV_OPA_COVER - value) * 112 / LV_OPA_COVER,
                    boot->base_scale));
}

void SetSubtitleProgress(void *object, int32_t value) {
  auto *boot = static_cast<BootContext *>(object);
  lv_obj_set_style_image_opa(boot->objects[kSubtitle],
                             static_cast<lv_opa_t>(value), 0);
}

void SetCyanProgress(void *object, int32_t value) {
  auto *boot = static_cast<BootContext *>(object);
  const auto color = lv_color_mix(Color(0x16c8ff), lv_color_white(),
                                  static_cast<uint8_t>(value));
  lv_obj_set_style_image_recolor(boot->objects[kMidRight], color, 0);
  lv_obj_set_style_image_recolor(boot->objects[kArrow], color, 0);
  lv_obj_set_style_image_recolor(boot->objects[kSubtitle], color, 0);
}

void StartOutro(BootContext *boot);

void BeginIdlePulse(BootContext *boot) {
  lv_anim_delete(boot, SetLogoScale);
  lv_anim_delete(boot, SetFineLogoScale);
  // Keep the LVGL draw bounds at the largest pulse size.  The OpenGL quad
  // receives Q16 scale below, so the visible mark still moves every frame.
  PositionFinalObject(boot->final_white, boot->base_scale + 10);
  PositionFinalObject(boot->final_cyan, boot->base_scale + 10);
  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, boot);
  lv_anim_set_values(&pulse, boot->base_scale * 256,
                     (boot->base_scale + 10) * 256);
  lv_anim_set_duration(&pulse, 1050);
  lv_anim_set_playback_duration(&pulse, 1050);
  lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&pulse, SetFineLogoScale);
  lv_anim_start(&pulse);
}

void IntroComplete(lv_anim_t *animation) {
  auto *boot = static_cast<BootContext *>(lv_anim_get_user_data(animation));
  if (boot == nullptr || boot->exiting) return;
  boot->intro_complete = true;
  ShowFinalLogo(boot);
  BeginIdlePulse(boot);
  if (boot->ready) StartOutro(boot);
}

void CompleteZoom(lv_anim_t *animation) {
  auto *boot = static_cast<BootContext *>(lv_anim_get_user_data(animation));
  if (boot == nullptr || boot->callback == nullptr) return;
  boot->callback(Action::kBootComplete, boot->context);
}

void StartOutro(BootContext *boot) {
  if (boot == nullptr || boot->exiting) return;
  boot->exiting = true;
  lv_anim_delete(boot, SetArrowProgress);
  lv_anim_delete(boot, SetWordProgress);
  lv_anim_delete(boot, SetSubtitleProgress);
  lv_anim_delete(boot, SetCyanProgress);
  lv_anim_delete(boot, SetLogoScale);
  lv_anim_delete(boot, SetFineLogoScale);
  boot->pulse_scale_q16 = 0;
  for (auto *image : boot->objects) {
    lv_anim_delete(image, SetObjectX);
    lv_anim_delete(image, SetObjectY);
    lv_anim_delete(image, SetImageOpacity);
  }

  SetArrowProgress(boot, 255);
  SetWordProgress(boot, LV_OPA_COVER);
  SetSubtitleProgress(boot, LV_OPA_COVER);
  SetCyanProgress(boot, LV_OPA_COVER);
  ShowFinalLogo(boot);
  SetLogoScale(boot, boot->base_scale);

  lv_anim_t zoom;
  lv_anim_init(&zoom);
  lv_anim_set_var(&zoom, boot);
  lv_anim_set_values(&zoom, boot->base_scale, 4300);
  lv_anim_set_duration(&zoom, 760);
  lv_anim_set_path_cb(&zoom, lv_anim_path_ease_in);
  lv_anim_set_exec_cb(&zoom, SetLogoScale);
  lv_anim_set_user_data(&zoom, boot);
  lv_anim_set_completed_cb(&zoom, CompleteZoom);
  lv_anim_start(&zoom);

  for (auto *image : {boot->final_white, boot->final_cyan}) {
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, image);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&fade, 610);
    lv_anim_set_duration(&fade, 150);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade, SetImageOpacity);
    lv_anim_start(&fade);
  }
}

void DeleteBoot(lv_event_t *event) {
  auto *boot = static_cast<BootContext *>(lv_event_get_user_data(event));
  if (boot == nullptr) return;
  lv_anim_delete(boot, SetArrowProgress);
  lv_anim_delete(boot, SetWordProgress);
  lv_anim_delete(boot, SetSubtitleProgress);
  lv_anim_delete(boot, SetCyanProgress);
  lv_anim_delete(boot, SetLogoScale);
  for (auto *image : boot->objects) {
    if (image == nullptr) continue;
    lv_anim_delete(image, SetObjectX);
    lv_anim_delete(image, SetObjectY);
    lv_anim_delete(image, SetImageOpacity);
    if (lv_obj_is_valid(image)) lv_obj_delete(image);
  }
  for (auto *image : {boot->final_white, boot->final_cyan}) {
    if (image == nullptr) continue;
    lv_anim_delete(image, SetImageOpacity);
    if (lv_obj_is_valid(image)) lv_obj_delete(image);
  }
  lv_image_cache_drop(&boot->arrow_descriptor);
  if (boot->arrow_pixels != nullptr) lv_free(boot->arrow_pixels);
  if (boot->arrow_order != nullptr) lv_free(boot->arrow_order);
  lv_free(boot);
}

void PrepareArrow(BootContext *boot) {
  boot->arrow_pixels = static_cast<uint8_t *>(lv_malloc(kArrowBytes));
  boot->arrow_order = static_cast<uint8_t *>(lv_malloc(kArrowBytes));
  std::memset(boot->arrow_pixels, 0, kArrowBytes);

  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kCenterX = 255.0f;
  constexpr float kCenterY = 244.0f;
  for (int32_t y = 0; y < kArrowHeight; ++y) {
    for (int32_t x = 0; x < kArrowWidth; ++x) {
      const int32_t index = y * kArrowWidth + x;
      float angle = std::atan2(static_cast<float>(y) - kCenterY,
                               static_cast<float>(x) - kCenterX) *
                    180.0f / kPi;
      if (angle < 0.0f) angle += 360.0f;
      float travel = angle >= 135.0f ? angle - 135.0f : angle + 225.0f;
      if (travel > 250.0f) travel = 250.0f;
      boot->arrow_order[index] =
          static_cast<uint8_t>(travel * 255.0f / 250.0f);
    }
  }

  std::memset(&boot->arrow_descriptor, 0, sizeof(boot->arrow_descriptor));
  boot->arrow_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  boot->arrow_descriptor.header.cf = LV_COLOR_FORMAT_A8;
  boot->arrow_descriptor.header.flags =
      LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;
  boot->arrow_descriptor.header.w = kArrowWidth;
  boot->arrow_descriptor.header.h = kArrowHeight;
  boot->arrow_descriptor.header.stride = kArrowWidth;
  boot->arrow_descriptor.data_size = kArrowBytes;
  boot->arrow_descriptor.data = boot->arrow_pixels;
}

}  // namespace

void BuildBootScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *boot = static_cast<BootContext *>(lv_malloc(sizeof(BootContext)));
  std::memset(boot, 0, sizeof(BootContext));
  boot->callback = callback;
  boot->context = context;

  auto *display = lv_display_get_default();
  boot->width = lv_display_get_horizontal_resolution(display);
  boot->height = lv_display_get_vertical_resolution(display);
  const int32_t target_size = boot->width * 94 / 100;
  boot->base_scale = target_size * LV_SCALE_NONE / kReferenceSize;

  lv_obj_add_event_cb(screen, DeleteBoot, LV_EVENT_DELETE, boot);
  lv_obj_set_user_data(screen, boot);
  Screen(screen);
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  PrepareArrow(boot);
  for (int32_t i = 0; i < kAssetCount; ++i) {
    const lv_image_dsc_t *source =
        i == kArrow ? &boot->arrow_descriptor : kGeometry[i].source;
    boot->objects[i] = CreateImage(source, lv_color_white());
    const auto &geometry = kGeometry[i];
    lv_image_set_pivot(boot->objects[i], geometry.width / 2,
                       geometry.height / 2);
    PositionAsset(boot, i, boot->base_scale);
    lv_obj_update_layout(boot->objects[i]);
    boot->final_x[i] = lv_obj_get_x(boot->objects[i]);
    boot->final_y[i] = lv_obj_get_y(boot->objects[i]);
  }

  // The assembly needs individual pieces, but the idle pulse and outro do
  // not.  Precomposed two-tone layers cut per-frame transforms from nine to
  // two and avoid repeatedly resampling every seam in the finished mark.
  boot->final_white = CreateImage(&assets::kAeraBrandWhite, lv_color_white());
  boot->final_cyan = CreateImage(&assets::kAeraBrandAccent, Color(0x16c8ff));
  for (auto *image : {boot->final_white, boot->final_cyan}) {
    lv_image_set_pivot(image, kFinalWidth / 2, kFinalHeight / 2);
    PositionFinalObject(image, boot->base_scale);
    lv_obj_add_event_cb(image, ApplyFineLogoScale, LV_EVENT_DRAW_TASK_ADDED,
                        boot);
    lv_obj_add_flag(image, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
  }

  // Approved route: lower-left, up to the crown, all the way down the right,
  // and only then the isolated internal triangle.
  AnimatePiece(boot, kBottomLeft, -200, 212, 0, 620);
  AnimatePiece(boot, kMidLeft, -120, 150, 350, 600);
  AnimatePiece(boot, kTop, -88, 140, 700, 600);
  AnimatePiece(boot, kMidRight, -58, -140, 1050, 600);
  AnimatePiece(boot, kBottomRight, -70, -138, 1400, 600);
  AnimatePiece(boot, kCenter, 142, 60, 2050, 500);

  // The wordmark slides vertically, so switch it from center-aligned offsets
  // to resolved top-left coordinates before SetWordProgress changes its Y.
  lv_obj_set_align(boot->objects[kWord], LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(boot->objects[kWord], boot->final_x[kWord],
                 boot->final_y[kWord]);
  AnimateValue(boot, SetWordProgress, 0, LV_OPA_COVER, 2580, 320,
               lv_anim_path_ease_out);
  AnimateValue(boot, SetSubtitleProgress, 0, LV_OPA_COVER, 2940, 220,
               lv_anim_path_ease_out);

  lv_obj_set_style_image_opa(boot->objects[kArrow], LV_OPA_COVER, 0);
  AnimateValue(boot, SetArrowProgress, 0, 255, 3250, 1100,
               lv_anim_path_ease_in_out);

  // The circle, right body and subtitle remain white until the moving stroke
  // reaches the right body of the A, then transition together to cyan.
  lv_anim_t cyan;
  lv_anim_init(&cyan);
  lv_anim_set_var(&cyan, boot);
  lv_anim_set_values(&cyan, 0, LV_OPA_COVER);
  lv_anim_set_delay(&cyan, 4140);
  lv_anim_set_duration(&cyan, 210);
  lv_anim_set_path_cb(&cyan, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&cyan, SetCyanProgress);
  lv_anim_set_user_data(&cyan, boot);
  lv_anim_set_completed_cb(&cyan, IntroComplete);
  lv_anim_start(&cyan);
}

void CompleteBootScene(lv_obj_t *screen) {
  if (screen == nullptr) return;
  auto *boot = static_cast<BootContext *>(lv_obj_get_user_data(screen));
  if (boot == nullptr || boot->exiting) return;
  boot->ready = true;
  if (boot->intro_complete) StartOutro(boot);
}

}  // namespace aeraui
