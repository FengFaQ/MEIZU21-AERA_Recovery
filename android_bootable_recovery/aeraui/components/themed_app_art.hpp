/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <zlib.h>
#include <lvgl.h>
#include "design.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace aeraui::themed_art {

enum class Kind { Files, Packages, Browser, Chat, Gallery, Media, Recorder,
                  Games, Doom, Streams, Mirror, Backup, Settings, System, Tools };

inline Kind ForPlugin(const std::string &id, const std::string &entry,
                      const std::string &category) {
  if (entry == "browser") return Kind::Browser;
  if (entry == "telegram") return Kind::Chat;
  if (entry == "gallery") return Kind::Gallery;
  if (entry == "media") return Kind::Media;
  if (entry == "recorder") return Kind::Recorder;
  if (entry == "retroarch") return Kind::Games;
  if (entry == "doom") return Kind::Doom;
  if (entry == "streams") return Kind::Streams;
  if (id == "mirror") return Kind::Mirror;
  if (entry == "appvault" || category == "backup") return Kind::Backup;
  if (id.find("settings") != std::string::npos || category == "themes") return Kind::Settings;
  if (id.find("sysinfo") != std::string::npos) return Kind::System;
  if (category == "network") return Kind::Browser;
  if (category == "games") return Kind::Games;
  if (category == "multimedia") return Kind::Media;
  return Kind::Tools;
}

namespace detail {
#include "home_art_data.inc"

inline const std::vector<uint8_t> &Atlas() {
  static const auto pixels = [] {
    std::vector<uint8_t> result(kArtWidth * kArtHeight * kArtCount);
    uLongf length = result.size();
    if (uncompress(result.data(), &length, kHomeArtCompressed,
                   sizeof(kHomeArtCompressed)) != Z_OK || length != result.size()) result.clear();
    return result;
  }();
  return pixels;
}

struct Artwork {
  Kind kind;
  int width = 0, height = 0, radius = -1;
  bool light = false;
  unsigned renders = 0;
  uint32_t generation = 0;
  lv_image_dsc_t image{};
  std::vector<uint32_t> pixels;

  explicit Artwork(Kind value) : kind(value) {}
  ~Artwork() { lv_image_cache_drop(&image); }

  // Bake the rounded mask and readability fade once, not once per swipe frame.
  // The GPU sees a single immutable image and can retain its texture in cache.
  void Prepare(int w, int h, int r, bool light_theme) {
    if (w == width && h == height && r == radius && light_theme == light) return;
    lv_image_cache_drop(&image);
    width = w; height = h; radius = r; light = light_theme;
    const auto &atlas = Atlas();
    if (atlas.empty() || w < 1 || h < 1) return;
    ++renders;
    static uint32_t next_generation = 0;
    generation = ++next_generation;
    const float reduction = std::min(1.0f, 512.0f / std::max(w, h));
    const int tw = std::max(1, static_cast<int>(w * reduction));
    const int th = std::max(1, static_cast<int>(h * reduction));
    const float rr = std::min({r * reduction, tw / 2.0f, th / 2.0f});
    pixels.resize(tw * th);
    const uint8_t *source = atlas.data() + static_cast<int>(kind) * kArtWidth * kArtHeight;
    // Aspect-fill: crop, never squash a globe or a game controller.
    const float scale = std::max(tw / float(kArtWidth), th / float(kArtHeight));
    for (int y = 0; y < th; ++y) {
      const float sy = std::clamp((y + 0.5f - th / 2.0f) / scale + kArtHeight / 2.0f - 0.5f,
                                 0.0f, float(kArtHeight - 1));
      const int y0 = int(sy), y1 = std::min(y0 + 1, kArtHeight - 1);
      const float fy = sy - y0;
      const float fade = 1.0f - 0.65f * std::clamp((y / float(th) - 0.5f) / 0.5f, 0.0f, 1.0f);
      for (int x = 0; x < tw; ++x) {
        const float sx = std::clamp((x + 0.5f - tw / 2.0f) / scale + kArtWidth / 2.0f - 0.5f,
                                   0.0f, float(kArtWidth - 1));
        const int x0 = int(sx), x1 = std::min(x0 + 1, kArtWidth - 1);
        const float fx = sx - x0;
        const float top = source[y0 * kArtWidth + x0] * (1-fx) + source[y0 * kArtWidth + x1] * fx;
        const float bottom = source[y1 * kArtWidth + x0] * (1-fx) + source[y1 * kArtWidth + x1] * fx;
        const float gray = top * (1-fy) + bottom * fy;
        float coverage = 1;
        const float dx = std::max(0.0f, rr - std::min(x + 0.5f, tw - x - 0.5f));
        const float dy = std::max(0.0f, rr - std::min(y + 0.5f, th - y - 0.5f));
        if (dx > 0 && dy > 0) coverage = std::clamp(rr - std::sqrt(dx*dx + dy*dy) - 0.25f, 0.0f, 1.0f);
        // Neutral translucent shading retains glass/outline/surface styles.
        // The illustration has both relief highlights and dark silhouettes.
        float delta = (gray - 52.0f) * fade;
        if (light) delta = -delta;
        const uint32_t rgb = delta >= 0 ? 0x00ffffff : 0;
        const float multiplier = light ? (delta >= 0 ? 3.0f : 1.5f) : (delta >= 0 ? 1.1f : 4.0f);
        const uint32_t alpha = std::clamp(int(std::abs(delta) * multiplier * coverage), 0, 180);
        pixels[y * tw + x] = (alpha << 24) | rgb;
      }
    }
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_ARGB8888;
    image.header.w = tw; image.header.h = th; image.header.stride = tw * 4;
    image.data_size = pixels.size() * sizeof(uint32_t);
    image.data = reinterpret_cast<const uint8_t *>(pixels.data());
  }
};
} // namespace detail

inline lv_obj_t *Attach(lv_obj_t *parent, Kind kind) {
  // No clip_corner layer: rounding lives in the texture alpha itself.
  auto *art = lv_obj_create(parent);
  design::Clear(art);
  lv_obj_set_pos(art, 0, 0);
  lv_obj_set_size(art, LV_PCT(100), LV_PCT(100));
  lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(art, LV_OBJ_FLAG_SCROLLABLE);
  auto *stored = new detail::Artwork(kind);
  lv_obj_add_event_cb(art, [](lv_event_t *event) {
    auto *state = static_cast<detail::Artwork *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) { delete state; return; }
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    auto *object = static_cast<lv_obj_t *>(lv_event_get_target(event));
    auto *parent = lv_obj_get_parent(object);
    const int w = lv_obj_get_width(object), h = lv_obj_get_height(object);
    state->Prepare(w, h, lv_obj_get_style_radius(parent, LV_PART_MAIN),
                   lv_color_luminance(design::kText) < 128);
    if (state->pixels.empty()) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &state->image;
    // Zero-opacity recolor is visually inert, but identifies this immutable
    // texture generation to the GLES descriptor cache. A recreated Home can
    // reuse a freed descriptor address; that must never reuse its old pixels.
    d.recolor = lv_color_hex(state->generation);
    d.recolor_opa = LV_OPA_TRANSP;
    d.pivot = {0, 0};
    d.scale_x = w * 256 / state->image.header.w;
    d.scale_y = h * 256 / state->image.header.h;
    d.gpu_scale_x_q16 = uint32_t((uint64_t(w) << 16) / state->image.header.w);
    d.gpu_scale_y_q16 = uint32_t((uint64_t(h) << 16) / state->image.header.h);
    d.antialias = true;
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    area.x2 = area.x1 + state->image.header.w - 1;
    area.y2 = area.y1 + state->image.header.h - 1;
    lv_draw_image(lv_event_get_layer(event), &d, &area);
  }, LV_EVENT_ALL, stored);
  return art;
}
} // namespace aeraui::themed_art
