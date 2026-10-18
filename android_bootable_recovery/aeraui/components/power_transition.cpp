/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "power_transition.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "aera_logo.hpp"
#include "design.hpp"
#include "ui_components.hpp"

namespace aeraui::widgets {
namespace {
using namespace design;

struct TransitionState {
  lv_obj_t *overlay = nullptr;
  lv_obj_t *logo = nullptr;
  lv_obj_t *white = nullptr;
  lv_obj_t *accent = nullptr;
  lv_obj_t *title = nullptr;
  std::function<void()> complete;
  bool dispatched = false;
};

struct ModeTransitionState {
  lv_obj_t *overlay = nullptr;
  std::vector<lv_obj_t *> content;
  int width = 0;
  int height = 0;
  int direction = -1;
  int progress = 0;
  int end_progress = 1000;
  std::function<void()> complete;
  bool dispatched = false;
};

void SetOverlayOpacity(void *object, int32_t value) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(object),
                          static_cast<lv_opa_t>(value), 0);
}

void SetImageOpacity(void *object, int32_t value) {
  lv_obj_set_style_image_opa(static_cast<lv_obj_t *>(object),
                             static_cast<lv_opa_t>(value), 0);
}

void SetTextOpacity(void *object, int32_t value) {
  lv_obj_set_style_text_opa(static_cast<lv_obj_t *>(object),
                            static_cast<lv_opa_t>(value), 0);
}

void SetLogoScale(void *object, int32_t value) {
  auto *state = static_cast<TransitionState *>(object);
  if (state == nullptr || state->logo == nullptr) return;
  lv_obj_set_style_transform_scale(state->logo, value, 0);
}

void Dispatch(lv_anim_t *animation) {
  auto *state = static_cast<TransitionState *>(lv_anim_get_user_data(animation));
  if (state == nullptr || state->dispatched || !state->complete) return;
  state->dispatched = true;
  state->complete();
}

void SetModeProgress(void *object, int32_t value) {
  auto *state = static_cast<ModeTransitionState *>(object);
  if (state == nullptr) return;
  state->progress = std::clamp<int32_t>(value, 0, state->end_progress);

  // The current interface accelerates toward the viewer and vanishes before
  // the light trails take over.  It is deliberately brief: the transition
  // should communicate motion without becoming a loading screen.
  const int scene_progress = std::min(state->progress, 360);
  const int offset = state->direction * state->width * scene_progress / 11500;
  const int scale = LV_SCALE_NONE + scene_progress * scene_progress / 1250;
  const int scene_opacity = std::clamp(255 - scene_progress * 255 / 285, 0, 255);
  for (auto *child : state->content) {
    if (child == nullptr || !lv_obj_is_valid(child)) continue;
    lv_obj_set_style_translate_x(child, offset, 0);
    lv_obj_set_style_transform_scale(child, scale, 0);
    lv_obj_set_style_opa(child, static_cast<lv_opa_t>(scene_opacity), 0);
  }
  lv_obj_set_style_bg_opa(state->overlay, static_cast<lv_opa_t>(
      std::min(255, state->progress * 255 / 240)), 0);
  lv_obj_invalidate(state->overlay);
}

lv_point_precise_t TransitionPoint(int32_t x, int32_t y) {
  lv_point_precise_t point{};
  point.x = x;
  point.y = y;
  return point;
}

void DrawModeTransition(lv_event_t *event) {
  auto *state = static_cast<ModeTransitionState *>(
      lv_event_get_user_data(event));
  lv_layer_t *layer = lv_event_get_layer(event);
  if (state == nullptr || layer == nullptr || state->overlay == nullptr) return;

  lv_area_t bounds{};
  lv_obj_get_coords(state->overlay, &bounds);
  const int width = lv_area_get_width(&bounds);
  const int height = lv_area_get_height(&bounds);
  const int centre_x = bounds.x1 + width / 2 + state->direction * width / 18;
  const int centre_y = bounds.y1 + height / 2;
  const int progress = state->progress;

  // Fine radial trails are drawn twice: a broad, quiet accent underlay and a
  // narrow white core.  This gives the impression of glow without invoking a
  // costly full-screen blur or creating chunky geometric tunnel walls.
  lv_draw_line_dsc_t ray;
  lv_draw_line_dsc_init(&ray);
  ray.round_start = true;
  ray.round_end = true;
  for (int index = 0; index < 34; ++index) {
    int dx = (index * 97 + 37) % 237 - 118;
    int dy = (index * 151 + 23) % 287 - 143;
    if (std::abs(dx) < 18) dx += dx < 0 ? -29 : 29;
    if (std::abs(dy) < 18) dy += dy < 0 ? -31 : 31;
    const int phase = (progress + index * 89) % 1100;
    const int inner = 8 + phase * phase / 3900;
    const int outer = inner + 12 + phase * phase / 1350;
    int opacity = std::min(
        190, std::min(phase, std::max(0, (1100 - phase) * 2)));
    const int fade_start = state->end_progress - 160;
    if (progress > fade_start)
      opacity = opacity * std::max(0, state->end_progress - progress) / 160;
    ray.p1 = TransitionPoint(centre_x + dx * inner / 100,
                             centre_y + dy * inner / 100);
    ray.p2 = TransitionPoint(centre_x + dx * outer / 100,
                             centre_y + dy * outer / 100);
    ray.color = design::kAccent;
    ray.width = phase > 660 ? 6 : 4;
    ray.opa = static_cast<lv_opa_t>(opacity * 2 / 5);
    lv_draw_line(layer, &ray);
    ray.color = index % 6 == 0 ? design::kAccent : lv_color_white();
    ray.width = phase > 760 ? 3 : 2;
    ray.opa = static_cast<lv_opa_t>(opacity);
    lv_draw_line(layer, &ray);
  }

  // Trails fade into the dark handoff.  Avoid a full-screen flash: the next
  // mode should resolve naturally rather than appearing after a white frame.
}

void DispatchMode(lv_anim_t *animation) {
  auto *state = static_cast<ModeTransitionState *>(
      lv_anim_get_user_data(animation));
  if (state == nullptr || state->dispatched || !state->complete) return;
  state->dispatched = true;
  state->complete();
}

void Animate(void *object, lv_anim_exec_xcb_t callback, int32_t from,
             int32_t to, uint32_t delay, uint32_t duration,
             lv_anim_path_cb_t path = lv_anim_path_ease_out) {
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

lv_obj_t *LogoLayer(lv_obj_t *parent, const lv_image_dsc_t *source,
                    lv_color_t color, int scale) {
  auto *image = lv_image_create(parent);
  lv_image_set_src(image, source);
  lv_image_set_antialias(image, true);
  lv_image_set_scale(image, scale);
  lv_image_set_pivot(image, 0, 0);
  lv_obj_set_style_image_recolor(image, color, 0);
  lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
  lv_obj_set_style_image_opa(image, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
  return image;
}

}  // namespace

void PowerTransition(lv_obj_t *screen, const std::string &title,
                     std::function<void()> complete) {
  if (screen == nullptr || !complete) return;
  const bool landscape = Landscape(screen);
  const int screen_width = lv_obj_get_width(screen);
  const int base_scale = landscape ? 138 :
      std::clamp(screen_width * 164 / 1440, 112, 178);

  auto *state = new TransitionState;
  state->complete = std::move(complete);

  auto *overlay = lv_obj_create(screen);
  state->overlay = overlay;
  lv_obj_set_user_data(overlay, &kPersistentModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, Color(0x080b10), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

  const int logo_width =
      assets::kAeraBrandWidth * base_scale / LV_SCALE_NONE;
  const int logo_height = 500 * base_scale / LV_SCALE_NONE;
  state->logo = lv_obj_create(overlay);
  Clear(state->logo);
  lv_obj_set_size(state->logo, logo_width, logo_height);
  lv_obj_align(state->logo, LV_ALIGN_CENTER, 0, -100);
  lv_obj_set_style_clip_corner(state->logo, true, 0);

  state->white = LogoLayer(state->logo, &assets::kAeraBrandWhite,
                           lv_color_white(), base_scale);
  lv_obj_set_pos(state->white, 0, 0);

  // The cyan boot layer also contains the orbit/spinner above the A.  The
  // terminal transition uses the approved static mark, so expose only the
  // single colored segment inside the letter.
  constexpr int kSegmentX = 413;
  constexpr int kSegmentY = 230;
  constexpr int kSegmentWidth = 183;
  constexpr int kSegmentHeight = 121;
  auto *segment_crop = lv_obj_create(state->logo);
  Clear(segment_crop);
  lv_obj_set_pos(segment_crop, kSegmentX * base_scale / LV_SCALE_NONE,
                 kSegmentY * base_scale / LV_SCALE_NONE);
  lv_obj_set_size(segment_crop,
                  kSegmentWidth * base_scale / LV_SCALE_NONE,
                  kSegmentHeight * base_scale / LV_SCALE_NONE);
  lv_obj_set_style_clip_corner(segment_crop, true, 0);
  state->accent = LogoLayer(segment_crop, &assets::kAeraBrandAccent,
                            kCyan, base_scale);
  lv_obj_set_pos(state->accent,
                 -kSegmentX * base_scale / LV_SCALE_NONE,
                 -kSegmentY * base_scale / LV_SCALE_NONE);

  state->title = Label(overlay, title.c_str(), &lv_font_montserrat_48,
                       lv_color_white());
  lv_obj_align(state->title, LV_ALIGN_CENTER, 0, landscape ? 190 : 220);
  lv_obj_set_style_text_letter_space(state->title, 1, 0);
  lv_obj_set_style_text_opa(state->title, LV_OPA_TRANSP, 0);

  lv_obj_move_foreground(overlay);
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    auto *state = static_cast<TransitionState *>(lv_event_get_user_data(event));
    lv_anim_delete(state, SetLogoScale);
    delete state;
  }, LV_EVENT_DELETE, state);

  Animate(overlay, SetOverlayOpacity, LV_OPA_TRANSP, LV_OPA_COVER,
          0, 180, lv_anim_path_ease_in);
  Animate(state->white, SetImageOpacity, LV_OPA_TRANSP, LV_OPA_COVER,
          70, 220);
  Animate(state->accent, SetImageOpacity, LV_OPA_TRANSP, LV_OPA_COVER,
          110, 220);
  Animate(state->title, SetTextOpacity, LV_OPA_TRANSP, LV_OPA_COVER,
          190, 210);

  lv_anim_t arrival;
  lv_anim_init(&arrival);
  lv_anim_set_var(&arrival, state);
  lv_anim_set_values(&arrival, 226, LV_SCALE_NONE);
  lv_anim_set_duration(&arrival, 700);
  lv_anim_set_path_cb(&arrival, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&arrival, SetLogoScale);
  lv_anim_set_user_data(&arrival, state);
  lv_anim_set_completed_cb(&arrival, Dispatch);
  lv_anim_start(&arrival);
}

void ModeTransition(lv_obj_t *screen, bool toward_fastbootd,
                    std::function<void()> complete, uint32_t duration_ms) {
  if (screen == nullptr || !complete) return;
  auto *state = new ModeTransitionState;
  state->width = lv_obj_get_width(screen);
  state->height = lv_obj_get_height(screen);
  state->direction = toward_fastbootd ? -1 : 1;
  // Preserve the original 1000 units / 720 ms travel speed. A longer
  // transition adds more star-field travel instead of slowing the same
  // animation down.
  state->end_progress = std::max<int>(
      1000, static_cast<int>((static_cast<uint64_t>(duration_ms) * 1000) / 720));
  state->complete = std::move(complete);

  const uint32_t count = lv_obj_get_child_count(screen);
  state->content.reserve(count);
  for (uint32_t index = 0; index < count; ++index)
    state->content.push_back(lv_obj_get_child(screen, index));

  state->overlay = lv_obj_create(screen);
  lv_obj_set_user_data(state->overlay, &kPersistentModalMarker);
  Clear(state->overlay);
  lv_obj_set_size(state->overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(state->overlay, Color(0x080b10), 0);
  lv_obj_set_style_bg_opa(state->overlay, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(state->overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(state->overlay, DrawModeTransition,
                      LV_EVENT_DRAW_MAIN, state);

  lv_obj_add_event_cb(state->overlay, [](lv_event_t *event) {
    auto *state = static_cast<ModeTransitionState *>(
        lv_event_get_user_data(event));
    lv_anim_delete(state, SetModeProgress);
    delete state;
  }, LV_EVENT_DELETE, state);

  lv_anim_t throw_animation;
  lv_anim_init(&throw_animation);
  lv_anim_set_var(&throw_animation, state);
  lv_anim_set_values(&throw_animation, 0, state->end_progress);
  lv_anim_set_duration(&throw_animation, duration_ms);
  lv_anim_set_path_cb(&throw_animation, lv_anim_path_linear);
  lv_anim_set_exec_cb(&throw_animation, SetModeProgress);
  lv_anim_set_user_data(&throw_animation, state);
  lv_anim_set_completed_cb(&throw_animation, DispatchMode);
  lv_anim_start(&throw_animation);
}

}  // namespace aeraui::widgets
