/* SPDX-License-Identifier: Apache-2.0 */
#include "picture_viewer.hpp"
#include "picture_decode.hpp"
#include "ui_components.hpp"
#include "aeraui/backend.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

namespace aeraui {
namespace {
using namespace widgets;

struct Contact {
  int x = 0;
  int y = 0;
  bool pressed = false;
};

struct Viewer {
  std::shared_ptr<PictureData> data = std::make_shared<PictureData>();
  lv_obj_t *overlay = nullptr;
  lv_obj_t *viewport = nullptr;
  lv_obj_t *content = nullptr;
  lv_obj_t *image = nullptr;
  lv_obj_t *message = nullptr;
  lv_obj_t *detail = nullptr;
  lv_timer_t *timer = nullptr;
  lv_image_dsc_t descriptor{};
  uint32_t scale = 256;
  uint32_t fit = 256;
  int viewport_width = 0;
  int viewport_height = 0;
  Contact contacts[2];
  bool pinch = false;
  bool consume_until_clear = false;
  bool original_snap = false;
  int pinch_start_distance = 1;
  uint32_t pinch_start_scale = 256;
};

Viewer *gViewer = nullptr;

int Distance(const Contact &first, const Contact &second) {
  const int64_t dx = first.x - second.x;
  const int64_t dy = first.y - second.y;
  return std::max(1, static_cast<int>(std::sqrt(
      static_cast<double>(dx * dx + dy * dy))));
}

void UpdateDetail(Viewer *state) {
  if (!state->detail || !state->data->pixels) return;
  const bool original = state->scale == 256;
  const std::string text = std::to_string(state->data->width) + " × " +
      std::to_string(state->data->height) + "   •   " +
      std::to_string(state->scale * 100 / 256) + "%" +
      (original ? "   •   Original size" : "");
  i18n::BindLabel(state->detail, text.c_str());
}

void Zoom(Viewer *state, uint32_t requested, int focus_x = -1,
          int focus_y = -1) {
  if (!state->image) return;
  const uint32_t old_scale = state->scale;
  state->scale = std::clamp(requested, state->fit, 1536u);

  const int old_image_width = state->data->width * old_scale / 256;
  const int old_image_height = state->data->height * old_scale / 256;
  const int old_content_width = std::max(state->viewport_width, old_image_width);
  const int old_content_height = std::max(state->viewport_height, old_image_height);
  const int old_scroll_x = lv_obj_get_scroll_x(state->viewport);
  const int old_scroll_y = lv_obj_get_scroll_y(state->viewport);

  lv_area_t viewport_area{};
  lv_obj_get_coords(state->viewport, &viewport_area);
  const int local_x = focus_x < 0 ? state->viewport_width / 2 :
      std::clamp(focus_x - viewport_area.x1, 0, state->viewport_width);
  const int local_y = focus_y < 0 ? state->viewport_height / 2 :
      std::clamp(focus_y - viewport_area.y1, 0, state->viewport_height);
  const int old_offset_x = (old_content_width - old_image_width) / 2;
  const int old_offset_y = (old_content_height - old_image_height) / 2;
  const int64_t image_x = static_cast<int64_t>(
      old_scroll_x + local_x - old_offset_x) * 256 / old_scale;
  const int64_t image_y = static_cast<int64_t>(
      old_scroll_y + local_y - old_offset_y) * 256 / old_scale;

  const int image_width = state->data->width * state->scale / 256;
  const int image_height = state->data->height * state->scale / 256;
  const int content_width = std::max(state->viewport_width, image_width);
  const int content_height = std::max(state->viewport_height, image_height);
  lv_obj_set_size(state->content, content_width, content_height);
  lv_image_set_scale(state->image, state->scale);
  lv_obj_center(state->image);
  lv_obj_update_layout(state->viewport);

  const int offset_x = (content_width - image_width) / 2;
  const int offset_y = (content_height - image_height) / 2;
  const int target_x = static_cast<int>(
      offset_x + image_x * state->scale / 256 - local_x);
  const int target_y = static_cast<int>(
      offset_y + image_y * state->scale / 256 - local_y);
  lv_obj_scroll_to(state->viewport,
      std::clamp(target_x, 0, std::max(0, content_width - state->viewport_width)),
      std::clamp(target_y, 0, std::max(0, content_height - state->viewport_height)),
      LV_ANIM_OFF);
  UpdateDetail(state);
}

void Loaded(lv_timer_t *timer) {
  auto *state = static_cast<Viewer *>(lv_timer_get_user_data(timer));
  if (!state->data->ready.load(std::memory_order_acquire)) return;
  lv_timer_pause(timer);
  if (!state->data->pixels) {
    i18n::BindLabel(state->message, state->data->error.c_str());
    i18n::BindLabel(state->detail, "PNG and baseline JPEG supported");
    return;
  }
  lv_obj_add_flag(state->message, LV_OBJ_FLAG_HIDDEN);
  auto &descriptor = state->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = state->data->width;
  descriptor.header.h = state->data->height;
  descriptor.header.stride = state->data->width * 4;
  descriptor.data_size = state->data->width * state->data->height * 4;
  descriptor.data = state->data->pixels;
  state->image = lv_image_create(state->content);
  lv_image_set_src(state->image, &descriptor);
  lv_image_set_antialias(state->image, true);
  lv_obj_remove_flag(state->image, LV_OBJ_FLAG_CLICKABLE);
  state->fit = std::max(1u, std::min({
      static_cast<uint32_t>(state->viewport_width) * 256 / state->data->width,
      static_cast<uint32_t>(state->viewport_height) * 256 / state->data->height,
      256u}));
  Zoom(state, state->fit);
}

void DeleteViewer(lv_event_t *event) {
  auto *state = static_cast<Viewer *>(lv_event_get_user_data(event));
  if (gViewer == state) gViewer = nullptr;
  state->data->cancelled = true;
  if (state->timer) lv_timer_delete(state->timer);
  lv_obj_delete(state->content);
  lv_image_cache_drop(&state->descriptor);
  delete state;
}
}  // namespace

bool PictureViewerHandlePointer(int slot, int x, int y, bool pressed) {
  auto *state = gViewer;
  if (state == nullptr || slot < 0 || slot > 1) return false;
  state->contacts[slot] = {x, y, pressed};

  if (!state->image) return slot == 1;
  const bool both = state->contacts[0].pressed && state->contacts[1].pressed;
  if (both && !state->pinch) {
    state->pinch = true;
    state->consume_until_clear = true;
    state->pinch_start_distance = Distance(state->contacts[0], state->contacts[1]);
    state->pinch_start_scale = state->scale;
    state->original_snap = false;
  }
  if (state->pinch && both) {
    const int distance = Distance(state->contacts[0], state->contacts[1]);
    uint32_t scale = std::clamp<uint32_t>(
        static_cast<uint32_t>(static_cast<uint64_t>(state->pinch_start_scale) *
                              distance / state->pinch_start_distance),
        state->fit, 1536u);
    if (state->fit <= 256 && scale >= 230 && scale <= 282) {
      scale = 256;
      if (!state->original_snap) {
        state->original_snap = true;
        RecoveryVibrate(Haptic::kTouch);
      }
    } else if (scale < 210 || scale > 302) {
      state->original_snap = false;
    }
    Zoom(state, scale,
         (state->contacts[0].x + state->contacts[1].x) / 2,
         (state->contacts[0].y + state->contacts[1].y) / 2);
  } else if (state->pinch) {
    state->pinch = false;
  }

  if (!state->contacts[0].pressed && !state->contacts[1].pressed)
    state->consume_until_clear = false;
  return state->pinch || state->consume_until_clear || slot == 1;
}

void OpenPicture(lv_obj_t *screen, const std::string &path) {
  auto *state = new Viewer;
  auto *overlay = lv_obj_create(screen);
  state->overlay = overlay;
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_user_data(overlay, &kModalMarker);
  MainBackground(overlay);
  AttachStatusBar(overlay, nullptr, nullptr, StatusBarAction::kNone, true);

  const bool landscape = Landscape(overlay);
  const int toolbar_y = StatusBarHeight() + 20;
  auto *toolbar = lv_obj_create(overlay);
  Panel(toolbar, 34, kMainPanel);
  lv_obj_set_pos(toolbar, 38, toolbar_y);
  lv_obj_set_size(toolbar, lv_obj_get_width(overlay) - 76,
                  landscape ? 122 : 146);
  lv_obj_set_style_border_width(toolbar, 1, 0);
  lv_obj_set_style_border_color(toolbar, kMainLine, 0);
  lv_obj_set_style_border_opa(toolbar, LV_OPA_30, 0);

  auto *back = Button(toolbar, LV_SYMBOL_LEFT,
                      [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_set_pos(back, 14, 14);
  lv_obj_set_size(back, landscape ? 94 : 112, landscape ? 94 : 118);

  auto *name = Label(toolbar, path.substr(path.find_last_of('/') + 1).c_str(),
                     &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, landscape ? 132 : 148, 18);
  lv_obj_set_width(name, lv_obj_get_width(toolbar) -
      (landscape ? 165 : 185));
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  state->detail = Label(toolbar, "Loading image…", &lv_font_montserrat_24,
                        kMutedStrong);
  lv_obj_set_pos(state->detail, landscape ? 132 : 148,
                 landscape ? 67 : 79);

  const int viewport_y = toolbar_y + (landscape ? 142 : 166);
  state->viewport_width = lv_obj_get_width(overlay) - 76;
  state->viewport_height = lv_obj_get_height(overlay) - viewport_y - 38;
  state->viewport = lv_obj_create(overlay);
  Clear(state->viewport);
  lv_obj_set_pos(state->viewport, 38, viewport_y);
  lv_obj_set_size(state->viewport, state->viewport_width,
                  state->viewport_height);
  lv_obj_set_style_radius(state->viewport, 34, 0);
  lv_obj_set_style_bg_color(state->viewport, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(state->viewport, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(state->viewport, 1, 0);
  lv_obj_set_style_border_color(state->viewport, kMainLine, 0);
  lv_obj_set_style_border_opa(state->viewport, LV_OPA_30, 0);
  lv_obj_add_flag(state->viewport, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(state->viewport, LV_DIR_ALL);
  lv_obj_set_scrollbar_mode(state->viewport, LV_SCROLLBAR_MODE_AUTO);

  state->content = lv_obj_create(state->viewport);
  Clear(state->content);
  lv_obj_set_size(state->content, state->viewport_width,
                  state->viewport_height);
  lv_obj_remove_flag(state->content, LV_OBJ_FLAG_CLICKABLE);
  state->message = Label(state->content, "Opening photo…",
                         &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(state->message, state->viewport_width - 180);
  lv_obj_set_style_text_align(state->message, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(state->message);

  auto *hint = Label(state->viewport,
      "Pinch to zoom   •   Drag to move   •   100% snaps",
      &lv_font_montserrat_24, kText);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -26);
  lv_obj_set_style_bg_color(hint, kCanvas, 0);
  lv_obj_set_style_bg_opa(hint, LV_OPA_70, 0);
  lv_obj_set_style_radius(hint, 24, 0);
  lv_obj_set_style_pad_hor(hint, 24, 0);
  lv_obj_set_style_pad_ver(hint, 14, 0);

  lv_obj_add_event_cb(overlay, DeleteViewer, LV_EVENT_DELETE, state);
  state->timer = lv_timer_create(Loaded, 40, state);
  gViewer = state;
  std::thread([data = state->data, path] { DecodePicture(path, *data); }).detach();
}
}  // namespace aeraui
