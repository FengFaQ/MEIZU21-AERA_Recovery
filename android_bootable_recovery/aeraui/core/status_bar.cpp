/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aeraui/status_bar.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <lvgl.h>

#include "design.hpp"
#include "ui_components.hpp"
#include "circle_battery.hpp"
#include "wifi_indicator.hpp"
#include "browser/session.hpp"
#include "plugin_api/operations.hpp"
#include "recorder/service.hpp"
#include "aeraui/backend.hpp"
#include "update/update_manager.hpp"

namespace aeraui {
namespace {
using namespace design;
constexpr int kWifiSpeedWidth = 128;
constexpr int kWifiSpeedGap = 12;
constexpr int kWifiSpeedReserved = kWifiSpeedWidth + kWifiSpeedGap;
constexpr int kSystemIndicatorGap = 12;

struct TrafficRate {
  WifiTraffic previous;
  uint32_t sampled_at = 0;
  double bytes_per_second = 0;

  void Reset() { previous = {}; sampled_at = 0; bytes_per_second = 0; }

  double Sample(const WifiTraffic &traffic, uint32_t now) {
    if (!traffic.valid) { Reset(); return 0; }
    const uint32_t elapsed = now - sampled_at;
    if (!previous.valid || previous.interface != traffic.interface ||
        previous.interface_index != traffic.interface_index ||
        traffic.received_bytes < previous.received_bytes ||
        traffic.sent_bytes < previous.sent_bytes) {
      bytes_per_second = 0;
    } else if (elapsed != 0) {
      // Use the actual interval, including UI stalls, and sum RX + TX in bytes.
      bytes_per_second = (double(traffic.received_bytes - previous.received_bytes) +
          double(traffic.sent_bytes - previous.sent_bytes)) * 1000.0 / elapsed;
    } else {
      return bytes_per_second;
    }
    previous = traffic;
    sampled_at = now;
    return bytes_per_second;
  }
};

std::string TrafficText(double bytes_per_second) {
  const char *units[] = {"KB/s", "MB/s", "GB/s", "TB/s", "PB/s", "EB/s"};
  double value = bytes_per_second > 0 ? bytes_per_second / 1000.0 : 0;
  unsigned unit = 0;
  while (value >= 999.5 && unit + 1 < sizeof(units) / sizeof(units[0])) {
    value /= 1000.0;
    ++unit;
  }
  char text[32];
  snprintf(text, sizeof(text), unit > 0 && value < 99.95 ? "%.1f %s" : "%.0f %s",
           value, units[unit]);
  return text;
}

const lv_font_t *TemperatureFont(int size) {
  return UiFont(size <= 20 ? &lv_font_montserrat_20 : size <= 24 ? &lv_font_montserrat_24 :
                size <= 28 ? &lv_font_montserrat_28 : &lv_font_montserrat_32);
}

std::string TemperatureText(const TemperatureDisplay &display, const Temperatures &values) {
  std::string text;
  auto append = [&](bool enabled, int millidegrees, const char *name) {
    if (!enabled || millidegrees == kTemperatureUnavailable) return;
    if (!text.empty()) text += "  ";
    if (display.labels) text += std::string(name) + " ";
    const double celsius = millidegrees / 1000.0;
    const double value = display.fahrenheit ? celsius * 1.8 + 32 : celsius;
    char number[32];
    snprintf(number, sizeof(number), display.decimals ? "%.1f" : "%.0f", value);
    text += number;
    text += display.fahrenheit ? "\xc2\xb0" "F" : "\xc2\xb0" "C";
  };
  append(display.cpu, values.cpu, "CPU");
  append(display.battery, values.battery, "BAT");
  return text;
}

lv_color_t TemperatureColor(int millidegrees, bool colored, int warm, int hot) {
  if (!colored || millidegrees == kTemperatureUnavailable) return kMutedStrong;
  return millidegrees >= hot * 1000 ? kRed : millidegrees >= warm * 1000 ? kAmber : kGreen;
}

struct StatusState {
  lv_timer_t *timer = nullptr;
  lv_obj_t *screen = nullptr;
  lv_obj_t *bar = nullptr;
  lv_obj_t *clock = nullptr;
  lv_obj_t *mirror = nullptr;
  lv_obj_t *download = nullptr;
  lv_obj_t *wifi = nullptr;
  widgets::WifiIndicator wifi_icon;
  lv_obj_t *wifi_speed = nullptr;
  lv_obj_t *temperature = nullptr;
  lv_obj_t *temperature_cpu = nullptr, *temperature_battery = nullptr;
  lv_obj_t *cpu_load = nullptr, *ram = nullptr;
  TrafficRate traffic_rate;
  lv_obj_t *recording_dot = nullptr;
  lv_obj_t *recording = nullptr;
  lv_obj_t *battery_text = nullptr;
  lv_obj_t *battery_fill = nullptr;
  lv_obj_t *battery_body = nullptr;
  lv_obj_t *battery_tip = nullptr;
  lv_obj_t *battery_charge = nullptr;
  widgets::CircleBattery battery_circle;
  lv_obj_t *shade = nullptr;
  lv_obj_t *sheet = nullptr;
  lv_obj_t *shade_wifi = nullptr;
  lv_obj_t *shade_wifi_detail = nullptr;
  lv_obj_t *shade_rotation = nullptr;
  lv_obj_t *shade_rotation_detail = nullptr;
  lv_obj_t *shade_flashlight = nullptr;
  lv_obj_t *shade_flashlight_detail = nullptr;
  lv_obj_t *shade_reboot = nullptr;
  lv_obj_t *shade_reboot_detail = nullptr;
  lv_obj_t *shade_recorder = nullptr;
  lv_obj_t *shade_recorder_detail = nullptr;
  lv_obj_t *shade_update = nullptr;
  lv_obj_t *shade_update_title = nullptr;
  lv_obj_t *shade_update_detail = nullptr;
  lv_obj_t *shade_download = nullptr;
  lv_obj_t *shade_download_title = nullptr;
  lv_obj_t *shade_download_detail = nullptr;
  lv_obj_t *shade_download_progress = nullptr;
  lv_obj_t *brightness_value = nullptr;
  lv_obj_t *brightness_slider = nullptr;
  int battery = -1;
  int shade_height = 1390;
  int shade_visible = 0;
  int drag_start_x = 0;
  int drag_start_y = 0;
  int drag_start_visible = 0;
  int applied_brightness = -1;
  bool charging = false;
  bool dragging = false;
  uint32_t ticks = 0;
  void (*callback)(Action, void *) = nullptr;
  void *context = nullptr;
  StatusBarAction action = StatusBarAction::kNone;
};

constexpr int kShadeHeight = 1390;
int32_t gStatusBarHeight = 165;
int32_t gStatusIndentLeft = 54;
int32_t gStatusIndentRight = 54;
lv_obj_t *gOpenShade = nullptr;
std::vector<StatusState *> gStatusBars;

void ShadeDeleted(lv_event_t *event) {
  auto *shade = lv_event_get_target_obj(event);
  if (gOpenShade == shade) gOpenShade = nullptr;
}

void PulseRecordingDot(void *target, int32_t opacity) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(target),
                          static_cast<lv_opa_t>(opacity), 0);
}

void StyleTile(lv_obj_t *tile, bool selected, bool available = true) {
  if (tile == nullptr) return;
  lv_obj_set_style_bg_color(tile, selected ? kAccentSoft : kMainPanel, 0);
  lv_obj_set_style_border_color(tile, selected ? kAccent : kMainLine, 0);
  lv_obj_set_style_border_opa(tile, selected ? LV_OPA_70 : LV_OPA_40, 0);
  lv_obj_set_style_opa(tile, available ? LV_OPA_COVER : LV_OPA_40, 0);
}

void SetShadeVisible(StatusState *state, int visible) {
  if (state == nullptr || state->shade == nullptr || state->sheet == nullptr)
    return;
  state->shade_visible = std::clamp(visible, 0, state->shade_height);
  lv_obj_set_y(state->sheet, state->shade_visible - state->shade_height);
}

void AnimateShade(StatusState *state, bool open) {
  if (state == nullptr || state->shade == nullptr) return;
  if (!open) {
    lv_obj_t *closing = state->shade;
    lv_obj_t *sheet = state->sheet;
    lv_anim_delete(state, nullptr);
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, sheet);
    lv_anim_set_values(&slide, lv_obj_get_y(sheet), -state->shade_height);
    lv_anim_set_duration(&slide, 240);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&slide, [](void *target, int32_t value) {
      lv_obj_set_y(static_cast<lv_obj_t *>(target), value);
    });
    lv_anim_start(&slide);
    state->shade = nullptr;
    state->sheet = nullptr;
    state->shade_wifi = nullptr;
    state->shade_wifi_detail = nullptr;
    state->shade_rotation = nullptr;
    state->shade_rotation_detail = nullptr;
    state->shade_flashlight = nullptr;
    state->shade_flashlight_detail = nullptr;
    state->shade_reboot = nullptr;
    state->shade_reboot_detail = nullptr;
    state->shade_recorder = nullptr;
    state->shade_recorder_detail = nullptr;
    state->shade_update = nullptr;
    state->shade_update_title = nullptr;
    state->shade_update_detail = nullptr;
    state->shade_download = nullptr;
    state->shade_download_title = nullptr;
    state->shade_download_detail = nullptr;
    state->shade_download_progress = nullptr;
    state->brightness_value = nullptr;
    state->brightness_slider = nullptr;
    lv_obj_delete_delayed(closing, 250);
    return;
  }
  const int from = state->shade_visible;
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, state);
  lv_anim_set_values(&animation, from, state->shade_height);
  lv_anim_set_duration(&animation, 310);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&animation, [](void *target, int32_t value) {
    SetShadeVisible(static_cast<StatusState *>(target), value);
  });
  lv_anim_start(&animation);
}

lv_obj_t *QuickTile(StatusState *state, int x, int y, int width, int height,
                    const char *symbol, const char *title, lv_obj_t **detail) {
  auto *tile = lv_button_create(state->sheet);
  NoScroll(tile);
  lv_obj_set_pos(tile, x, y);
  lv_obj_set_size(tile, width, height);
  lv_obj_set_style_radius(tile, 44, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(tile, 2, 0);
  lv_obj_set_style_transform_scale(tile, 252, LV_STATE_PRESSED);
  auto *icon = Label(tile, symbol, &lv_font_montserrat_48, kAccent);
  lv_obj_set_pos(icon, 30, 30);
  auto *name = Label(tile, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 30, height >= 270 ? 128 : 112);
  *detail = Label(tile, "", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(*detail, 30, height >= 270 ? 198 : 168);
  lv_obj_set_width(*detail, width - 60);
  lv_label_set_long_mode(*detail, LV_LABEL_LONG_DOT);
  return tile;
}

void RefreshShade(StatusState *state);

void TileClicked(lv_event_t *event) {
  auto *state = static_cast<StatusState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->callback == nullptr) return;
  auto *target = lv_event_get_target_obj(event);
  if (target == state->shade_wifi) {
    if (RecoveryWifiStatus().supported)
      state->callback(Action::kQuickWifiToggle, state->context);
  } else if (target == state->shade_rotation) {
    state->callback(Action::kToggleRotation, state->context);
  } else if (target == state->shade_flashlight) {
    RecoverySetFlashlight(!RecoveryFlashlightEnabled());
  } else if (target == state->shade_reboot) {
    state->callback(Action::kOpenReboot, state->context);
    return;
  } else if (target == state->shade_recorder) {
    if (recorder::Installed() || recorder::Active())
      state->callback(Action::kToggleRecording, state->context);
  }
  RefreshShade(state);
}

void BrightnessChanged(lv_event_t *event) {
  auto *state = static_cast<StatusState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->brightness_slider == nullptr) return;
  const int percent = lv_slider_get_value(state->brightness_slider);
  char text[16];
  snprintf(text, sizeof(text), "%d%%", percent);
  lv_label_set_text(state->brightness_value, text);
  // Update while dragging, but avoid hammering sysfs for every input sample.
  if (lv_event_get_code(event) == LV_EVENT_RELEASED ||
      state->applied_brightness < 0 ||
      std::abs(percent - state->applied_brightness) >= 2) {
    RecoverySetBrightness(percent);
    state->applied_brightness = percent;
  }
  if (lv_event_get_code(event) == LV_EVENT_RELEASED &&
      !RecoveryAutoSavePreferences())
    widgets::Sheet(state->screen, "Could not save preferences",
        "Changes still apply to this session. Unlock and mount settings storage, then try again.");
}

void ShadeGesture(lv_event_t *event) {
  auto *state = static_cast<StatusState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->shade == nullptr) return;
  const auto code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESS_LOST) {
    if (state->dragging) {
      state->dragging = false;
      AnimateShade(state, state->drag_start_visible >= state->shade_height - 170);
    }
    return;
  }
  lv_indev_t *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);
  if (code == LV_EVENT_PRESSED) {
    state->dragging = true;
    state->drag_start_y = point.y;
    state->drag_start_visible = state->shade_visible;
  } else if (code == LV_EVENT_PRESSING && state->dragging) {
    SetShadeVisible(state, state->drag_start_visible + point.y - state->drag_start_y);
  } else if (code == LV_EVENT_RELEASED && state->dragging) {
    state->dragging = false;
    AnimateShade(state, state->shade_visible >= state->shade_height - 170);
  }
}

void CloseShade(lv_event_t *event) {
  auto *state = static_cast<StatusState *>(lv_event_get_user_data(event));
  if (state != nullptr) AnimateShade(state, false);
}

void ShortcutClicked(lv_event_t *event) {
  auto *state = static_cast<StatusState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->callback == nullptr) return;
  const auto action = static_cast<Action>(
      reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
  state->callback(action, state->context);
}

void AddShortcut(StatusState *state, int x, int y, int width,
                 const char *text, Action action) {
  auto *button = lv_button_create(state->sheet);
  NoScroll(button);
  lv_obj_set_user_data(button,
      reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, 118);
  lv_obj_set_style_radius(button, 59, 0);
  lv_obj_set_style_bg_color(button, kMainPanel, 0);
  lv_obj_set_style_bg_color(button, kMainSelected, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, kMainLine, 0);
  auto *label = Label(button, text, &lv_font_montserrat_32, kText);
  lv_obj_center(label);
  lv_obj_add_event_cb(button, ShortcutClicked, LV_EVENT_CLICKED, state);
}

void BuildShade(StatusState *state) {
  if (state == nullptr || state->shade != nullptr || state->callback == nullptr)
    return;
  const int screen_width = lv_obj_get_width(state->screen);
  const int screen_height = lv_obj_get_height(state->screen);
  const bool landscape = screen_width > screen_height;
  const int content_width = landscape ? std::min(2200, screen_width - 128)
                                      : std::min(1296, screen_width - 144);
  const int offset = (screen_width - content_width) / 2;
  state->shade_height = std::min(kShadeHeight, screen_height - 24);
  state->shade_visible = 0;
  state->shade = lv_obj_create(state->screen);
  gOpenShade = state->shade;
  lv_obj_add_event_cb(state->shade, ShadeDeleted, LV_EVENT_DELETE, nullptr);
  NoScroll(state->shade);
  lv_obj_set_pos(state->shade, 0, 0);
  lv_obj_set_size(state->shade, screen_width, screen_height);
  lv_obj_set_style_radius(state->shade, 0, 0);
  lv_obj_set_style_bg_color(state->shade, lv_color_black(), 0);
  // Keep the backdrop at its final opacity while the opaque sheet moves.
  // Animating this full-screen alpha forced every Home object underneath it
  // to be recomposited for every drag sample, which made the pull-down lag on
  // object-rich scenes such as the Terminal Matrix card.
  lv_obj_set_style_bg_opa(state->shade, LV_OPA_50, 0);
  lv_obj_set_style_border_width(state->shade, 0, 0);
  lv_obj_set_style_pad_all(state->shade, 0, 0);
  lv_obj_add_event_cb(state->shade, CloseShade, LV_EVENT_CLICKED, state);

  state->sheet = lv_obj_create(state->shade);
  NoScroll(state->sheet);
  lv_obj_set_pos(state->sheet, 0, -state->shade_height);
  lv_obj_set_size(state->sheet, screen_width, state->shade_height);
  lv_obj_set_style_radius(state->sheet, 0, 0);
  lv_obj_set_style_bg_color(state->sheet, kMainSheet, 0);
  lv_obj_set_style_bg_grad_color(state->sheet, kMainCanvas, 0);
  lv_obj_set_style_bg_grad_dir(state->sheet, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(state->sheet, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(state->sheet, 1, 0);
  lv_obj_set_style_border_side(state->sheet, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(state->sheet, kMainLine, 0);
  lv_obj_set_style_pad_all(state->sheet, 0, 0);
  lv_obj_add_event_cb(state->sheet, ShadeGesture, LV_EVENT_PRESSED, state);
  lv_obj_add_event_cb(state->sheet, ShadeGesture, LV_EVENT_PRESSING, state);
  lv_obj_add_event_cb(state->sheet, ShadeGesture, LV_EVENT_RELEASED, state);
  lv_obj_add_event_cb(state->sheet, ShadeGesture, LV_EVENT_PRESS_LOST, state);

  auto *title = Label(state->sheet, "Quick Settings", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, offset, 62);
  auto *hint = Label(state->sheet, "AERA controls", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, offset + 2, 140);
  auto *close = lv_button_create(state->sheet);
  NoScroll(close);
  lv_obj_set_pos(close, screen_width - offset - 116, 52);
  lv_obj_set_size(close, 116, 116);
  lv_obj_set_style_radius(close, 58, 0);
  lv_obj_set_style_bg_color(close, kMainPanel, 0);
  lv_obj_set_style_border_width(close, 0, 0);
  auto *close_icon = Label(close, LV_SYMBOL_UP, &lv_font_montserrat_32, kText);
  lv_obj_center(close_icon);
  lv_obj_add_event_cb(close, CloseShade, LV_EVENT_CLICKED, state);

  const int tile_gap = landscape ? 24 : 20;
  const int tile_width = (content_width - tile_gap * 4) / 5;
  const int tile_y = landscape ? 228 : 262;
  const int tile_height = landscape ? 250 : 286;
  state->shade_wifi = QuickTile(state, offset, tile_y, tile_width, tile_height,
                                LV_SYMBOL_WIFI, "Wi-Fi",
                                &state->shade_wifi_detail);
  state->shade_rotation = QuickTile(
      state, offset + tile_width + tile_gap, tile_y, tile_width, tile_height,
      LV_SYMBOL_REFRESH, "Rotation", &state->shade_rotation_detail);
  state->shade_flashlight = QuickTile(
      state, offset + (tile_width + tile_gap) * 2, tile_y, tile_width,
      tile_height, LV_SYMBOL_EYE_OPEN, "Flashlight",
      &state->shade_flashlight_detail);
  state->shade_reboot = QuickTile(
      state, offset + (tile_width + tile_gap) * 3, tile_y, tile_width,
      tile_height, LV_SYMBOL_POWER, "Reboot", &state->shade_reboot_detail);
  state->shade_recorder = QuickTile(
      state, offset + (tile_width + tile_gap) * 4, tile_y, tile_width,
      tile_height, LV_SYMBOL_VIDEO, "Recorder", &state->shade_recorder_detail);
  i18n::BindLabel(state->shade_reboot_detail, "Power menu");
  for (auto *tile : {state->shade_wifi, state->shade_rotation,
                     state->shade_flashlight, state->shade_reboot,
                     state->shade_recorder})
    lv_obj_add_event_cb(tile, TileClicked, LV_EVENT_CLICKED, state);

  auto *brightness_title = Label(state->sheet, "Brightness",
                                  &lv_font_montserrat_32, kText);
  const int brightness_y = landscape ? 530 : 620;
  lv_obj_set_pos(brightness_title, offset + 2, brightness_y);
  state->brightness_value = Label(state->sheet, "", &lv_font_montserrat_32,
                                  kAccent);
  lv_obj_align(state->brightness_value, LV_ALIGN_TOP_RIGHT,
               -(screen_width - offset - content_width), brightness_y);
  state->brightness_slider = lv_slider_create(state->sheet);
  lv_obj_set_pos(state->brightness_slider, offset + 2,
                 landscape ? 635 : 730);
  lv_obj_set_size(state->brightness_slider, content_width - 4, 22);
  lv_slider_set_range(state->brightness_slider, 10, 100);
  const int brightness = std::max(10, RecoveryBrightness());
  state->applied_brightness = brightness;
  lv_slider_set_value(state->brightness_slider, brightness, LV_ANIM_OFF);
  RangeSlider(state->brightness_slider);
  lv_obj_add_event_cb(state->brightness_slider, BrightnessChanged,
                      LV_EVENT_VALUE_CHANGED, state);
  lv_obj_add_event_cb(state->brightness_slider, BrightnessChanged,
                      LV_EVENT_RELEASED, state);

  const int update_y = landscape ? 735 : 855;
  state->shade_download = lv_obj_create(state->sheet);
  NoScroll(state->shade_download);
  lv_obj_set_pos(state->shade_download, offset, update_y);
  lv_obj_set_size(state->shade_download, content_width, 126);
  lv_obj_set_style_radius(state->shade_download, 30, 0);
  lv_obj_set_style_bg_color(state->shade_download, kMainPanel, 0);
  lv_obj_set_style_bg_opa(state->shade_download, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(state->shade_download, 1, 0);
  lv_obj_set_style_border_color(state->shade_download, kMainLine, 0);
  auto *download_icon = Label(state->shade_download, LV_SYMBOL_DOWNLOAD,
                              &lv_font_montserrat_36, kAccent);
  lv_obj_align(download_icon, LV_ALIGN_LEFT_MID, 34, -9);
  state->shade_download_title = Label(state->shade_download, "Downloading",
                                      &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->shade_download_title, 104, 16);
  lv_obj_set_width(state->shade_download_title, content_width - 190);
  lv_label_set_long_mode(state->shade_download_title, LV_LABEL_LONG_DOT);
  state->shade_download_detail = Label(state->shade_download, "",
                                       &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(state->shade_download_detail, 104, 62);
  lv_obj_set_width(state->shade_download_detail, content_width - 190);
  lv_label_set_long_mode(state->shade_download_detail, LV_LABEL_LONG_DOT);
  state->shade_download_progress = lv_bar_create(state->shade_download);
  lv_obj_set_pos(state->shade_download_progress, 104, 102);
  lv_obj_set_size(state->shade_download_progress, content_width - 142, 8);
  lv_obj_set_style_bg_color(state->shade_download_progress, kMainLine, 0);
  lv_obj_set_style_bg_color(state->shade_download_progress, kAccent,
                            LV_PART_INDICATOR);
  lv_obj_add_flag(state->shade_download, LV_OBJ_FLAG_HIDDEN);

  state->shade_update = lv_button_create(state->sheet);
  NoScroll(state->shade_update);
  lv_obj_set_user_data(state->shade_update,
      reinterpret_cast<void *>(static_cast<uintptr_t>(Action::kUpdates)));
  lv_obj_set_pos(state->shade_update, offset, update_y);
  lv_obj_set_size(state->shade_update, content_width, 126);
  lv_obj_set_style_radius(state->shade_update, 30, 0);
  lv_obj_set_style_bg_color(state->shade_update, kAccentSoft, 0);
  lv_obj_set_style_bg_color(state->shade_update, kMainSelected,
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_width(state->shade_update, 1, 0);
  lv_obj_set_style_border_color(state->shade_update, kAccent, 0);
  lv_obj_set_style_border_opa(state->shade_update, LV_OPA_60, 0);
  auto *update_icon =
      Label(state->shade_update, LV_SYMBOL_DOWNLOAD,
            &lv_font_montserrat_36, kAccent);
  lv_obj_align(update_icon, LV_ALIGN_LEFT_MID, 34, 0);
  state->shade_update_title =
      Label(state->shade_update, "AERA update available",
            &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->shade_update_title, 104, 22);
  lv_obj_set_width(state->shade_update_title, content_width - 190);
  lv_label_set_long_mode(state->shade_update_title, LV_LABEL_LONG_DOT);
  state->shade_update_detail =
      Label(state->shade_update, "Tap to review",
            &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(state->shade_update_detail, 104, 70);
  lv_obj_add_event_cb(state->shade_update, ShortcutClicked,
                      LV_EVENT_CLICKED, state);
  lv_obj_add_flag(state->shade_update, LV_OBJ_FLAG_HIDDEN);

  // Keep the shortcut row attached to the bottom of the shade instead of
  // leaving a large, visually empty footer below it.  Deriving the position
  // from the actual shade height also keeps it consistent across devices.
  const int shortcut_y = state->shade_height - 200;
  const int shortcut_gap = 24;
  const int shortcut_width = (content_width - shortcut_gap * 2) / 3;
  AddShortcut(state, offset, shortcut_y, shortcut_width,
              LV_SYMBOL_WIFI "  Network", Action::kWifi);
  AddShortcut(state, offset + shortcut_width + shortcut_gap, shortcut_y,
              shortcut_width, LV_SYMBOL_SETTINGS "  Preferences",
              Action::kPreferences);
  AddShortcut(state, offset + (shortcut_width + shortcut_gap) * 2, shortcut_y,
              shortcut_width, LV_SYMBOL_REFRESH "  Check updates",
              Action::kCheckUpdates);
  auto *handle = lv_obj_create(state->sheet);
  NoScroll(handle);
  lv_obj_set_size(handle, 190, 12);
  lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_set_style_radius(handle, 6, 0);
  lv_obj_set_style_bg_color(handle, kMuted, 0);
  lv_obj_set_style_bg_opa(handle, LV_OPA_60, 0);
  lv_obj_set_style_border_width(handle, 0, 0);
  RefreshShade(state);
  lv_obj_move_foreground(state->shade);
}

void StatusGesture(lv_event_t *event) {
  auto *state = static_cast<StatusState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->callback == nullptr) return;
  const auto code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESS_LOST) {
    if (state->dragging) {
      state->dragging = false;
      AnimateShade(state, false);
    }
    return;
  }
  lv_indev_t *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);
  if (code == LV_EVENT_PRESSED) {
    lv_area_t bounds{};
    lv_obj_get_coords(state->bar, &bounds);
    if (point.x < bounds.x1 || point.x > bounds.x2 ||
        point.y < bounds.y1 || point.y > bounds.y2) return;
    BuildShade(state);
    state->dragging = true;
    state->drag_start_x = point.x;
    state->drag_start_y = point.y;
    state->drag_start_visible = 0;
  } else if (code == LV_EVENT_PRESSING && state->dragging) {
    SetShadeVisible(state, point.y - state->drag_start_y);
  } else if (code == LV_EVENT_RELEASED && state->dragging) {
    const bool tap = std::abs(point.x - state->drag_start_x) < 20 &&
                     std::abs(point.y - state->drag_start_y) < 20;
    state->dragging = false;
    AnimateShade(state, tap || state->shade_visible >= 150);
  }
}

int ReadCapacity() {
  constexpr const char *kPaths[] = {
      "/sys/class/power_supply/battery/capacity",
      "/sys/class/power_supply/bms/capacity",
  };
  for (const char *path : kPaths) {
    FILE *file = fopen(path, "re");
    if (file == nullptr) continue;
    int capacity = -1;
    const int matched = fscanf(file, "%d", &capacity);
    fclose(file);
    if (matched == 1) return std::clamp(capacity, 0, 100);
  }
  return -1;
}

bool ReadCharging() {
  FILE *file = fopen("/sys/class/power_supply/battery/status", "re");
  if (file == nullptr) return false;
  char status[32] = {};
  const bool read = fgets(status, sizeof(status), file) != nullptr;
  fclose(file);
  return read && (strncmp(status, "Charging", 8) == 0 ||
                  strncmp(status, "Full", 4) == 0);
}

std::string FormatDownloadSpeed(uint64_t bytes_per_second) {
  char text[32];
  if (bytes_per_second >= 1024ULL * 1024)
    snprintf(text, sizeof(text), "%.1f MB/s",
             bytes_per_second / (1024.0 * 1024));
  else if (bytes_per_second >= 1024)
    snprintf(text, sizeof(text), "%.0f KB/s", bytes_per_second / 1024.0);
  else
    snprintf(text, sizeof(text), "%llu B/s",
             static_cast<unsigned long long>(bytes_per_second));
  return text;
}

void RefreshShade(StatusState *state) {
  if (state == nullptr || state->shade == nullptr) return;
  const auto wifi = RecoveryWifiStatus();
  std::string wifi_detail;
  if (!wifi.supported)
    wifi_detail = "Unavailable";
  else if (wifi.connected)
    wifi_detail = wifi.ssid.empty() ? "Connected" : wifi.ssid;
  else if (wifi.busy)
    wifi_detail = "Working…";
  else
    wifi_detail = wifi.enabled ? "On" : "Off";
  i18n::BindLabel(state->shade_wifi_detail, wifi_detail.c_str());
  StyleTile(state->shade_wifi, wifi.enabled, wifi.supported && !wifi.busy);

  const bool landscape =
      lv_display_get_rotation(lv_display_get_default()) !=
      LV_DISPLAY_ROTATION_0;
  i18n::BindLabel(state->shade_rotation_detail,
                    landscape ? "Landscape" : "Portrait");
  StyleTile(state->shade_rotation, landscape);

  const bool torch_supported = RecoveryFlashlightSupported();
  const bool torch_enabled = RecoveryFlashlightEnabled();
  i18n::BindLabel(state->shade_flashlight_detail,
                    !torch_supported ? "Unavailable"
                                     : torch_enabled ? "On" : "Off");
  StyleTile(state->shade_flashlight, torch_enabled, torch_supported);
  StyleTile(state->shade_reboot, false);

  const auto recording = recorder::GetSnapshot();
  const bool recorder_installed = recorder::Installed();
  const bool recorder_active = recorder::Active();
  i18n::BindLabel(state->shade_recorder_detail,
      !recorder_installed && !recorder_active ? "Not installed" :
      recording.state == recorder::State::kRecording ? "Recording" :
      recording.state == recorder::State::kFinalizing ? "Saving…" :
      (recording.state == recorder::State::kPreparing ||
       recording.state == recorder::State::kStarting) ? "Starting…" : "Ready");
  StyleTile(state->shade_recorder, recorder_active,
            recorder_installed || recorder_active);

  const auto update = update::GetSnapshot();
  const auto download = web::CurrentDownloadSummary();
  if (state->shade_download != nullptr) {
    if (download.available) {
      const bool active = download.active_count > 0;
      const std::string title = active
          ? (download.active_count > 1
              ? i18n::Format("%u downloads", download.active_count)
              : i18n::Format("Downloading %s", download.name.c_str()))
          : download.status == web::DownloadStatus::kFinished
              ? "Download finished"
              : download.status == web::DownloadStatus::kCancelled
                  ? "Download cancelled" : "Download failed";
      std::string detail = active
          ? i18n::Format("%u%%", download.progress)
          : download.name;
      if (active && download.total_bytes)
        detail = i18n::Format(
            "%s  •  %llu of %llu MB", detail.c_str(),
            static_cast<unsigned long long>(download.received_bytes / (1024 * 1024)),
            static_cast<unsigned long long>(download.total_bytes / (1024 * 1024)));
      if (active && download.speed_bytes_per_second)
        detail += "  •  " + FormatDownloadSpeed(download.speed_bytes_per_second);
      i18n::BindLabel(state->shade_download_title, title.c_str());
      i18n::BindLabel(state->shade_download_detail, detail.c_str());
      lv_bar_set_value(state->shade_download_progress,
                       active ? download.progress :
                       download.status == web::DownloadStatus::kFinished ? 100 : 0,
                       LV_ANIM_ON);
      lv_obj_remove_flag(state->shade_download, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(state->shade_download, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (state->shade_update != nullptr) {
    lv_obj_set_y(state->shade_update,
                 (landscape ? 735 : 855) + (download.available ? 148 : 0));
    if (update.available) {
      const std::string title = i18n::Format(
          "AERA %s available", update.release.version.c_str());
      i18n::BindLabel(state->shade_update_title, title.c_str());
      i18n::BindLabel(state->shade_update_detail, "Tap to review the update");
      lv_obj_remove_flag(state->shade_update, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(state->shade_update, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (state->brightness_value != nullptr && !state->dragging) {
    const int brightness = std::max(10, RecoveryBrightness());
    char text[16];
    snprintf(text, sizeof(text), "%d%%", brightness);
    i18n::BindLabel(state->brightness_value, text);
    if (state->brightness_slider != nullptr)
      lv_slider_set_value(state->brightness_slider, brightness, LV_ANIM_OFF);
  }
}

void Refresh(StatusState *state, bool refresh_battery) {
  if (state == nullptr) return;

  const time_t now = time(nullptr);
  struct tm local {};
  char clock_text[16] = "--:--";
  if (localtime_r(&now, &local) != nullptr)
    strftime(clock_text, sizeof(clock_text),
             RecoveryPreference(Preference::kClock24) ? "%H:%M" : "%I:%M %p", &local);
  i18n::BindLabel(state->clock, clock_text);
  if (refresh_battery) {
    state->battery = ReadCapacity();
    state->charging = ReadCharging();
  }
  char battery_text[24];
  if (state->battery < 0) {
    snprintf(battery_text, sizeof(battery_text), "--%%");
  } else {
    snprintf(battery_text, sizeof(battery_text), "%d%%%s", state->battery,
             state->charging ? " +" : "");
  }
  i18n::BindLabel(state->battery_text, battery_text);
  const bool circle_battery = RecoveryBatteryStyle() == BatteryStyle::kCircle;
  const bool show_percentage = RecoveryBatteryPercentage();
  for (auto *object : {state->battery_body, state->battery_tip}) {
    if (circle_battery) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
  }
  if (circle_battery || !show_percentage) lv_obj_add_flag(state->battery_text, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(state->battery_text, LV_OBJ_FLAG_HIDDEN);
  if (!circle_battery && !show_percentage && state->charging)
    lv_obj_remove_flag(state->battery_charge, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(state->battery_charge, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_color(state->battery_charge, state->battery >= 60 ? kCanvas : kText, 0);
  if (circle_battery) lv_obj_remove_flag(state->battery_circle.root, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(state->battery_circle.root, LV_OBJ_FLAG_HIDDEN);
  widgets::ResizeCircleBattery(state->battery_circle, RecoveryBatteryCircleSize());
  widgets::UpdateCircleBattery(state->battery_circle, state->battery, state->charging);
  lv_obj_update_layout(state->bar);

  const auto connection = RecoveryWifiConnection();
  const auto display = RecoveryWifiStatusDisplay();
  const auto traffic = connection.connected && display.show_speed ? RecoveryWifiTraffic() : WifiTraffic{};
  const auto speed = TrafficText(state->traffic_rate.Sample(traffic, lv_tick_get()));
  lv_label_set_text(state->wifi_speed, speed.c_str());
  widgets::FitLabelToLines(state->wifi_speed, kWifiSpeedWidth, 1,
      {&lv_font_montserrat_24, &lv_font_montserrat_20, &lv_font_montserrat_18, &lv_font_montserrat_16});
  const bool show_speed = connection.connected && display.show_speed && traffic.valid;
  auto temperature_display = RecoveryTemperatureDisplay();
  const auto temperatures = RecoveryTemperatures();
  const auto *temperature_font = TemperatureFont(temperature_display.size);
  // Reserve a stable width so digit changes cannot move neighboring indicators.
  temperature_display.cpu &= temperatures.cpu != kTemperatureUnavailable;
  temperature_display.battery &= temperatures.battery != kTemperatureUnavailable;
  int temperature_width = 0;
  auto temperature_label = [&](lv_obj_t *label, bool cpu) {
    auto options = temperature_display;
    options.cpu &= cpu; options.battery &= !cpu;
    const auto text = TemperatureText(options, temperatures);
    if (text.empty()) { lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, temperature_font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(label, TemperatureColor(cpu ? temperatures.cpu : temperatures.battery,
        options.colors, cpu ? options.cpu_warm : options.battery_warm,
        cpu ? options.cpu_hot : options.battery_hot), 0);
    lv_point_t positive{}, negative{};
    lv_text_get_size(&positive, TemperatureText(options, {150000, 90000}).c_str(), temperature_font,
                    0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&negative, TemperatureText(options, {150000, -20000}).c_str(), temperature_font,
                    0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int width = std::max(positive.x, negative.x) + 8;
    if (temperature_width) temperature_width += 12;
    lv_obj_set_pos(label, temperature_width, 0);
    lv_obj_set_width(label, width);
    temperature_width += width;
  };
  temperature_label(state->temperature_cpu, true);
  temperature_label(state->temperature_battery, false);
  const auto system = RecoverySystemMetrics();
  auto metric_label = [&](lv_obj_t *label, bool enabled, int percent, const char *name) {
    if (!enabled || percent < 0) { lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN); return; }
    const auto prefix = temperature_display.labels ? std::string(name) + " " : "";
    lv_label_set_text(label, (prefix + std::to_string(percent) + "%").c_str());
    lv_obj_set_style_text_font(label, temperature_font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(label, kMutedStrong, 0);
    lv_point_t bounds{};
    lv_text_get_size(&bounds, (prefix + "100%").c_str(), temperature_font, 0, 0,
                    LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (temperature_width) temperature_width += 20;
    lv_obj_set_pos(label, temperature_width, 0);
    lv_obj_set_width(label, bounds.x + 8);
    temperature_width += bounds.x + 8;
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
  };
  metric_label(state->cpu_load, temperature_display.cpu_load, system.cpu_percent, "CPU");
  metric_label(state->ram, temperature_display.ram, system.memory_total ?
      static_cast<int>(100.0 * system.memory_used / system.memory_total) : -1, "RAM");
  const int temperature_reserved = temperature_width ? temperature_width + kSystemIndicatorGap : 0;
  lv_obj_set_size(state->temperature, std::max(1, temperature_width), lv_font_get_line_height(temperature_font));
  // Reserve only the width currently used by the battery percentage/charge text.
  const int wifi_right = lv_obj_get_x(circle_battery ? state->battery_circle.root :
      show_percentage ? state->battery_text : state->battery_body) - 24;
  const int network_reserved = (connection.connected ? widgets::kWifiIndicatorWidth + 26 +
      (show_speed ? kWifiSpeedReserved : 0) : 0) + temperature_reserved;
  const auto recording = recorder::GetSnapshot();
  const bool recording_active = recorder::Active();
  if (recording_active) {
    const uint64_t seconds = recording.elapsed_ms / 1000;
    char text[48];
    snprintf(text, sizeof(text), "%s%02llu:%02llu",
             recording.state == recorder::State::kRecording ? "REC  " : "",
             static_cast<unsigned long long>(seconds / 60),
             static_cast<unsigned long long>(seconds % 60));
    const lv_color_t color =
        recording.state == recorder::State::kRecording ? kRed : kAccent;
    i18n::BindLabel(state->recording, text);
    lv_obj_set_style_text_color(state->recording, color, 0);
    lv_obj_set_style_bg_color(state->recording_dot, color, 0);
    lv_obj_set_width(state->recording, LV_SIZE_CONTENT);
    lv_obj_remove_flag(state->recording_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(state->recording, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(state->recording_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(state->recording, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_update_layout(state->bar);
  const auto download = web::CurrentDownloadSummary();
  const int recording_reserved = recording_active ? lv_obj_get_width(state->recording) + 48 : 0;
  const int download_reserved = download.active_count ? 90 : 0;
  lv_obj_t *activity_anchor = state->clock;
  int activity_gap = 26;
  const auto mirror = plugin_api::ActiveMirrorMode();
  if (mirror != plugin_api::MirrorMode::kOff) {
    const std::string text =
        mirror == plugin_api::MirrorMode::kWifi
            ? std::string(LV_SYMBOL_VIDEO) + "  Wi-Fi Mirror"
            : std::string(LV_SYMBOL_USB) + "  USB Mirror";
    i18n::BindLabel(state->mirror, text.c_str());
    const int available = wifi_right - network_reserved - recording_reserved -
        download_reserved - lv_obj_get_x(state->clock) - lv_obj_get_width(state->clock) - 26;
    lv_obj_set_width(state->mirror, std::clamp(available, 0, 330));
    lv_obj_align_to(state->mirror, activity_anchor,
                    LV_ALIGN_OUT_RIGHT_MID, activity_gap, 0);
    lv_obj_remove_flag(state->mirror, LV_OBJ_FLAG_HIDDEN);
    activity_anchor = state->mirror;
    activity_gap = 18;
  } else {
    lv_obj_add_flag(state->mirror, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_update_layout(state->bar);
  if (download.active_count) {
    lv_obj_align_to(state->download, activity_anchor,
                    LV_ALIGN_OUT_RIGHT_MID, activity_gap, 0);
    lv_obj_remove_flag(state->download, LV_OBJ_FLAG_HIDDEN);
    activity_anchor = state->download;
    activity_gap = 18;
  } else {
    lv_obj_add_flag(state->download, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_update_layout(state->bar);
  lv_obj_align_to(state->recording_dot, activity_anchor,
                  LV_ALIGN_OUT_RIGHT_MID, activity_gap, 0);
  lv_obj_update_layout(state->bar);
  lv_obj_align_to(state->recording, state->recording_dot,
                  LV_ALIGN_OUT_RIGHT_MID, 14, 0);
  if (recording_active) activity_anchor = state->recording;
  lv_obj_update_layout(state->bar);

  if (connection.connected) {
    const std::string text = display.show_name ?
        (connection.ssid.empty() ? std::string("Connected") : connection.ssid) : "";
    i18n::BindLabel(state->wifi, text.c_str());
    lv_obj_remove_flag(state->wifi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(state->wifi, LV_SIZE_CONTENT);
    lv_obj_set_height(state->wifi,
        lv_font_get_line_height(lv_obj_get_style_text_font(state->wifi, LV_PART_MAIN)));
    lv_obj_update_layout(state->bar);
    const int available = wifi_right - lv_obj_get_x(activity_anchor) -
        lv_obj_get_width(activity_anchor) - 26 - (show_speed ? kWifiSpeedReserved : 0) - temperature_reserved;
    const int name_width = display.show_name ? std::clamp(lv_obj_get_width(state->wifi), 0,
        std::max(0, std::min(available, 660 - (show_speed ? kWifiSpeedReserved : 0)) - widgets::kWifiIndicatorWidth - 12)) : 0;
    lv_obj_set_width(state->wifi, name_width);
    const int icon_x = wifi_right - widgets::kWifiIndicatorWidth - (name_width > 0 ? name_width + 12 : 0);
    lv_obj_align(state->wifi_icon.root, LV_ALIGN_LEFT_MID, icon_x, 0);
    widgets::UpdateWifiIndicator(state->wifi_icon, connection.signal_dbm);
    lv_obj_remove_flag(state->wifi_icon.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(state->wifi, LV_ALIGN_LEFT_MID, icon_x + widgets::kWifiIndicatorWidth + 12, 0);
    lv_obj_set_style_text_color(state->wifi, kMutedStrong, 0);
    if (name_width > 0) lv_obj_remove_flag(state->wifi, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(state->wifi, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(state->wifi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(state->wifi_icon.root, LV_OBJ_FLAG_HIDDEN);
  }
  if (show_speed) {
    lv_obj_update_layout(state->bar);
    const int left = lv_obj_get_x(activity_anchor) + lv_obj_get_width(activity_anchor) + 26;
    const int x = std::max(left, int(lv_obj_get_x(state->wifi_icon.root)) - kWifiSpeedReserved);
    lv_obj_align(state->wifi_speed, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_remove_flag(state->wifi_speed, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(state->wifi_speed, LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_update_layout(state->bar);
  const int temperature_left = lv_obj_get_x(activity_anchor) + lv_obj_get_width(activity_anchor) + 26;
  const int temperature_right = (connection.connected ? lv_obj_get_x(show_speed ? state->wifi_speed :
      state->wifi_icon.root) : wifi_right + 24) - kSystemIndicatorGap;
  // Keep all selected values readable when a narrow bar cannot fit their preferred size.
  for (const auto *font : {TemperatureFont(20), UiFont(&lv_font_montserrat_16)}) {
    if (temperature_width <= temperature_right - temperature_left) break;
    if (lv_font_get_line_height(font) >= lv_font_get_line_height(temperature_font)) continue;
    temperature_font = font;
    temperature_width = 0;
    temperature_label(state->temperature_cpu, true);
    temperature_label(state->temperature_battery, false);
    metric_label(state->cpu_load, temperature_display.cpu_load, system.cpu_percent, "CPU");
    metric_label(state->ram, temperature_display.ram, system.memory_total ?
        static_cast<int>(100.0 * system.memory_used / system.memory_total) : -1, "RAM");
    lv_obj_set_size(state->temperature, std::max(1, temperature_width), lv_font_get_line_height(font));
    lv_obj_update_layout(state->bar);
  }
  if (temperature_width > 0 && temperature_right - temperature_left >= temperature_width) {
    const int spacing = std::min(temperature_right - temperature_left - temperature_width,
        std::clamp(temperature_display.position, 0, 100) * 2);
    const int x = temperature_right - temperature_width - spacing;
    lv_obj_align(state->temperature, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_remove_flag(state->temperature, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(state->temperature, LV_OBJ_FLAG_HIDDEN);
  }

  const int level = state->battery < 0 ? 0 : state->battery;
  lv_obj_set_width(state->battery_fill, std::max(3, level * 58 / 100));
  lv_obj_set_style_bg_color(
      state->battery_fill,
      level <= 15 ? kRed : (state->charging ? kGreen : kText), 0);
  RefreshShade(state);
}

void TimerTick(lv_timer_t *timer) {
  auto *state = static_cast<StatusState *>(lv_timer_get_user_data(timer));
  if (state == nullptr) return;
  ++state->ticks;
  Refresh(state, state->ticks % 10 == 0);
}

void DeleteState(lv_event_t *event) {
  auto *state = static_cast<StatusState *>(lv_event_get_user_data(event));
  if (state == nullptr) return;
  gStatusBars.erase(std::remove(gStatusBars.begin(), gStatusBars.end(), state), gStatusBars.end());
  lv_anim_delete(state, nullptr);
  if (state->timer != nullptr) lv_timer_delete(state->timer);
  delete state;
}

void Activate(lv_event_t *event) {
  auto *state = static_cast<StatusState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->callback == nullptr) return;
  if (state->action == StatusBarAction::kPower) {
    RecoveryVibrate(Haptic::kTouch);
    state->callback(Action::kOpenReboot, state->context);
  } else if (state->action == StatusBarAction::kBack) {
    RecoveryVibrate(Haptic::kTouch);
    state->callback(Action::kBack, state->context);
  }
}

}  // namespace

void ConfigureStatusBar(int32_t height, int32_t left_indent,
                        int32_t right_indent) {
  // Keep malformed device configuration from making the status bar consume
  // the whole display or disappear completely.
  gStatusBarHeight = std::clamp(height, 112, 260);
  gStatusIndentLeft = std::clamp(left_indent, 0, 240);
  gStatusIndentRight = std::clamp(right_indent, 0, 240);
}

int32_t StatusBarHeight() {
  return gStatusBarHeight;
}

bool StatusBarShadeOpen() {
  return gOpenShade != nullptr;
}

void RefreshStatusBars() {
  for (auto *state : gStatusBars) Refresh(state, false);
}

void AttachStatusBar(lv_obj_t *screen, void (*callback)(Action, void *),
                     void *context, StatusBarAction action, bool soft_surface) {
  auto *state = new StatusState;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  state->action = action;
  gStatusBars.push_back(state);
  lv_obj_add_event_cb(screen, DeleteState, LV_EVENT_DELETE, state);

  lv_obj_t *bar = lv_obj_create(screen);
  state->bar = bar;
  NoScroll(bar);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_size(bar, LV_PCT(100), gStatusBarHeight);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_bg_color(bar, kCanvas, 0);
  lv_obj_set_style_bg_opa(bar, soft_surface ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(bar, soft_surface ? kMainLine : kLine, 0);
  lv_obj_set_style_border_opa(bar, soft_surface ? LV_OPA_30 : LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  if (callback != nullptr) {
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, StatusGesture, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(bar, StatusGesture, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(bar, StatusGesture, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(bar, StatusGesture, LV_EVENT_PRESS_LOST, state);
  }

  state->clock = Label(bar, "--:--", &lv_font_montserrat_36, kText);
  lv_obj_set_style_text_letter_space(state->clock, 2, 0);
  lv_obj_align(state->clock, LV_ALIGN_LEFT_MID, gStatusIndentLeft, 0);

  state->mirror = Label(bar, "", &lv_font_montserrat_24, kAccent);
  lv_obj_set_width(state->mirror, 330);
  lv_label_set_long_mode(state->mirror, LV_LABEL_LONG_DOT);
  lv_obj_align_to(state->mirror, state->clock, LV_ALIGN_OUT_RIGHT_MID, 26, 0);
  lv_obj_add_flag(state->mirror, LV_OBJ_FLAG_HIDDEN);

  state->download = Label(bar, LV_SYMBOL_DOWNLOAD,
                          &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(state->download, 64);
  lv_obj_add_flag(state->download, LV_OBJ_FLAG_HIDDEN);

  state->recording_dot = lv_obj_create(bar);
  NoScroll(state->recording_dot);
  lv_obj_set_size(state->recording_dot, 16, 16);
  lv_obj_align(state->recording_dot, LV_ALIGN_LEFT_MID, 250, 0);
  lv_obj_set_style_radius(state->recording_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(state->recording_dot, kRed, 0);
  lv_obj_set_style_bg_opa(state->recording_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(state->recording_dot, 0, 0);
  lv_obj_set_style_pad_all(state->recording_dot, 0, 0);
  lv_obj_add_flag(state->recording_dot, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, state->recording_dot);
  lv_anim_set_values(&pulse, LV_OPA_40, LV_OPA_COVER);
  lv_anim_set_duration(&pulse, 720);
  lv_anim_set_playback_duration(&pulse, 720);
  lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&pulse, PulseRecordingDot);
  lv_anim_start(&pulse);

  state->recording = Label(bar, "", &lv_font_montserrat_24, kRed);
  lv_obj_set_width(state->recording, 260);
  lv_obj_align(state->recording, LV_ALIGN_LEFT_MID, 280, 0);
  lv_obj_add_flag(state->recording, LV_OBJ_FLAG_HIDDEN);

  state->wifi = Label(bar, "", &lv_font_montserrat_32, kMutedStrong);
  state->wifi_icon = widgets::CreateWifiIndicator(bar);
  lv_obj_add_flag(state->wifi_icon.root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_width(state->wifi, 660);
  lv_obj_set_style_text_align(state->wifi, LV_TEXT_ALIGN_LEFT, 0);
  lv_label_set_long_mode(state->wifi, LV_LABEL_LONG_DOT);
  lv_obj_align(state->wifi, LV_ALIGN_RIGHT_MID,
               -(gStatusIndentRight +
                 (action == StatusBarAction::kNone ? 216 : 336)), 0);
  lv_obj_add_flag(state->wifi, LV_OBJ_FLAG_HIDDEN);

  state->wifi_speed = Label(bar, "0 KB/s", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_width(state->wifi_speed, kWifiSpeedWidth);
  lv_obj_set_style_text_align(state->wifi_speed, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_add_flag(state->wifi_speed, LV_OBJ_FLAG_HIDDEN);

  state->temperature = lv_obj_create(bar);
  Clear(state->temperature);
  lv_obj_remove_flag(state->temperature, LV_OBJ_FLAG_CLICKABLE);
  state->temperature_cpu = Label(state->temperature, "", &lv_font_montserrat_24, kMutedStrong);
  state->temperature_battery = Label(state->temperature, "", &lv_font_montserrat_24, kMutedStrong);
  state->cpu_load = Label(state->temperature, "", &lv_font_montserrat_24, kMutedStrong);
  state->ram = Label(state->temperature, "", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_add_flag(state->temperature, LV_OBJ_FLAG_HIDDEN);

  state->battery_text =
      Label(bar, "--%", &lv_font_montserrat_32, kMutedStrong);
  lv_obj_align(state->battery_text, LV_ALIGN_RIGHT_MID,
               -(gStatusIndentRight +
                 (action == StatusBarAction::kNone ? 96 : 216)), 0);

  lv_obj_t *battery = lv_obj_create(bar);
  state->battery_body = battery;
  NoScroll(battery);
  lv_obj_set_size(battery, 68, 36);
  lv_obj_align(battery, LV_ALIGN_RIGHT_MID,
               -(gStatusIndentRight +
                 (action == StatusBarAction::kNone ? 0 : 110)), 0);
  lv_obj_set_style_radius(battery, 6, 0);
  lv_obj_set_style_bg_opa(battery, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(battery, 2, 0);
  lv_obj_set_style_border_color(battery, kMutedStrong, 0);
  lv_obj_set_style_pad_all(battery, 0, 0);

  lv_obj_t *terminal = lv_obj_create(bar);
  state->battery_tip = terminal;
  NoScroll(terminal);
  lv_obj_set_size(terminal, 6, 16);
  lv_obj_align_to(terminal, battery, LV_ALIGN_OUT_RIGHT_MID, 3, 0);
  lv_obj_set_style_radius(terminal, 2, 0);
  lv_obj_set_style_bg_color(terminal, kMutedStrong, 0);
  lv_obj_set_style_bg_opa(terminal, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(terminal, 0, 0);

  state->battery_fill = lv_obj_create(battery);
  NoScroll(state->battery_fill);
  lv_obj_set_pos(state->battery_fill, 3, 3);
  lv_obj_set_size(state->battery_fill, 3, 26);
  lv_obj_set_style_radius(state->battery_fill, 3, 0);
  lv_obj_set_style_bg_opa(state->battery_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(state->battery_fill, 0, 0);
  state->battery_charge = Label(bar, LV_SYMBOL_CHARGE, &lv_font_montserrat_16, kText);
  lv_obj_align_to(state->battery_charge, state->battery_body, LV_ALIGN_CENTER, 0, 0);

  state->battery_circle = widgets::CreateCircleBattery(bar);
  lv_obj_align(state->battery_circle.root, LV_ALIGN_RIGHT_MID,
      -(gStatusIndentRight + (action == StatusBarAction::kNone ? 0 : 110)), 0);

  Refresh(state, true);
  state->timer = lv_timer_create(TimerTick, 1000, state);

  if (action != StatusBarAction::kNone) {
    lv_obj_t *button = lv_button_create(bar);
    lv_obj_set_size(button, 88, 88);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -gStatusIndentRight, 0);
    Panel(button, 24, kPanelStrong);
    Interactive(button);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, Activate, LV_EVENT_CLICKED, state);
    const char *symbol = action == StatusBarAction::kPower
                             ? LV_SYMBOL_POWER
                             : LV_SYMBOL_LEFT;
    lv_obj_t *icon = Label(button, symbol, &lv_font_montserrat_32,
                           action == StatusBarAction::kPower ? kAccent : kText);
    lv_obj_center(icon);
  }
}

}  // namespace aeraui
