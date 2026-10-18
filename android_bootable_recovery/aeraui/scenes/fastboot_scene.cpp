/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <android-base/properties.h>
#include <cutils/sockets.h>

#include "aera_logo.hpp"
#include "design.hpp"
#include "power_transition.hpp"
#include "ui_components.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

struct TelemetryState {
  std::string phase = "idle";
  std::string target;
  uint64_t current = 0;
  uint64_t total = 0;
  int result = -1;
  uint32_t updated_ms = 0;
  bool dirty = true;
};

struct TelemetryView {
  lv_obj_t *screen = nullptr;
  lv_obj_t *title = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *percent = nullptr;
};

TelemetryState g_telemetry;
TelemetryView g_view;
int g_server_fd = -1;
int g_client_fd = -1;
std::string g_input;

bool WirelessFastboot() {
  if (android::base::GetProperty("fastbootd.protocol", "usb") == "tcp")
    return true;
  // The destination scene is painted just before the backend enters live
  // fastbootd. In that brief handoff, reflect the enabled session preference;
  // once fastbootd is active the explicit protocol is authoritative.
  return android::base::GetProperty("sys.usb.config.aera_fastbootd", "0") != "1" &&
      RecoveryFastbootWifiStatus().enabled;
}

bool ActiveWirelessFastboot() {
  return WirelessFastboot();
}

std::string ReadyDetail() {
  if (WirelessFastboot()) {
    return "Fastboot is using the authenticated paired-ADB tunnel. The USB cable may remain\n"
           "connected, but partition commands use Wi-Fi until you select USB.";
  }
  return "Use the fastboot client on your computer to flash, erase, resize or\n"
         "inspect dynamic partitions. Keep the USB cable connected during writes.";
}

constexpr int kTransportPillWidth = 390;
constexpr int kTransportPillHeight = 96;
constexpr int kTransportInset = 7;
constexpr int kTransportGap = 6;
constexpr int kTransportSegmentWidth =
    (kTransportPillWidth - 2 * kTransportInset - kTransportGap) / 2;

struct TransportPillState {
  lv_obj_t *screen = nullptr;
  lv_obj_t *highlight = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *guide = nullptr;
  lv_obj_t *warning = nullptr;
  std::array<lv_obj_t *, 2> icons{};
  std::array<lv_obj_t *, 2> labels{};
  bool wireless = false;
  bool pending_wireless = false;
  bool switching = false;
};

TransportPillState g_transport_pill;

void SetTransportPillVisual(bool wireless, bool animate);

void ApplyTransportSwitch(lv_timer_t *timer) {
  (void)timer;
  if (!g_transport_pill.switching) return;
  const bool wireless = g_transport_pill.pending_wireless;
  const bool success = RecoverySelectFastbootTransport(wireless);
  g_transport_pill.switching = false;
  if (!success) {
    SetTransportPillVisual(!wireless, true);
    if (g_transport_pill.screen &&
        lv_obj_is_valid(g_transport_pill.screen)) {
      Sheet(g_transport_pill.screen,
            "Could not change Fastboot transport",
            wireless
                ? "The paired ADB tunnel is unavailable. Check Wi-Fi and the paired computer."
                : "USB Fastboot could not be restored. Reconnect the cable and try again.",
            Handler{}, 760, true, SheetPresentation::kCompactGlass);
    }
    return;
  }

  g_transport_pill.wireless = wireless;
  if (g_transport_pill.detail &&
      lv_obj_is_valid(g_transport_pill.detail)) {
    i18n::BindLabel(g_transport_pill.detail, ReadyDetail().c_str());
  }
  if (g_transport_pill.guide && lv_obj_is_valid(g_transport_pill.guide)) {
    if (wireless)
      lv_obj_remove_flag(g_transport_pill.guide, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(g_transport_pill.guide, LV_OBJ_FLAG_HIDDEN);
  }
  if (g_transport_pill.warning &&
      lv_obj_is_valid(g_transport_pill.warning)) {
    i18n::BindLabel(
        g_transport_pill.warning,
        wireless
            ? "Keep Wi-Fi connected while a command is writing data."
            : "Do not disconnect USB while a command is writing data.");
  }
}

void SetTransportPillVisual(bool wireless, bool animate) {
  auto &state = g_transport_pill;
  if (state.highlight == nullptr) return;
  const int target = kTransportInset +
      (wireless ? kTransportSegmentWidth + kTransportGap : 0);
  if (animate) {
    lv_anim_delete(state.highlight, nullptr);
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, state.highlight);
    lv_anim_set_values(&slide, lv_obj_get_x(state.highlight), target);
    lv_anim_set_duration(&slide, 210);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&slide, [](void *object, int32_t x) {
      lv_obj_set_x(static_cast<lv_obj_t *>(object), x);
    });
    lv_anim_start(&slide);
  } else {
    lv_obj_set_x(state.highlight, target);
  }

  state.wireless = wireless;
  for (size_t index = 0; index < state.labels.size(); ++index) {
    const bool selected = (index == 1) == wireless;
    const lv_color_t color = selected ? kOnAccent : kMutedStrong;
    lv_obj_set_style_text_color(state.icons[index], color, 0);
    lv_obj_set_style_text_color(state.labels[index], color, 0);
  }
}

void BeginTransportSwitch(bool wireless) {
  auto &state = g_transport_pill;
  if (state.switching || state.wireless == wireless)
    return;

  // Move the pill immediately, then apply the already-prepared transport once
  // the 210 ms slide is visible. No service restart is performed on this path.
  state.switching = true;
  state.pending_wireless = wireless;
  SetTransportPillVisual(wireless, true);
  auto *timer = lv_timer_create(ApplyTransportSwitch, 230, nullptr);
  lv_timer_set_repeat_count(timer, 1);
}

void AddTransportPill(lv_obj_t *screen, bool wireless) {
  auto *pill = lv_obj_create(screen);
  Panel(pill, LV_RADIUS_CIRCLE, kMainPanel);
  lv_obj_set_size(pill, kTransportPillWidth, kTransportPillHeight);
  lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -64,
               Landscape(screen) ? 242 : 292);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_set_style_border_color(pill, kMainLine, 0);
  lv_obj_set_style_border_opa(pill, LV_OPA_50, 0);
  lv_obj_set_style_pad_all(pill, 0, 0);
  lv_obj_set_style_clip_corner(pill, true, 0);

  auto &state = g_transport_pill;
  state = {};
  state.screen = screen;
  state.wireless = wireless;
  state.pending_wireless = wireless;

  state.highlight = lv_obj_create(pill);
  Clear(state.highlight);
  lv_obj_set_pos(state.highlight,
                 kTransportInset +
                     (wireless ? kTransportSegmentWidth + kTransportGap : 0),
                 kTransportInset);
  lv_obj_set_size(state.highlight, kTransportSegmentWidth,
                  kTransportPillHeight - 2 * kTransportInset);
  lv_obj_set_style_radius(state.highlight, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(state.highlight, kAccent, 0);
  lv_obj_set_style_bg_opa(state.highlight, LV_OPA_COVER, 0);
  lv_obj_remove_flag(state.highlight, LV_OBJ_FLAG_CLICKABLE);

  struct Segment {
    const char *icon;
    const char *label;
    bool wifi;
  };
  constexpr std::array<Segment, 2> segments{{
      {LV_SYMBOL_USB, "USB", false},
      {LV_SYMBOL_WIFI, "Wi-Fi", true},
  }};

  for (size_t index = 0; index < segments.size(); ++index) {
    const Segment segment = segments[index];
    const bool selected = wireless == segment.wifi;
    auto *button = lv_button_create(pill);
    Clear(button);
    lv_obj_set_pos(button,
                   kTransportInset + static_cast<int>(index) *
                       (kTransportSegmentWidth + kTransportGap),
                   kTransportInset);
    lv_obj_set_size(button, kTransportSegmentWidth,
                    kTransportPillHeight - 2 * kTransportInset);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_transform_scale(button, 252, LV_STATE_PRESSED);

    state.icons[index] = Label(button, segment.icon,
                               &lv_font_montserrat_32,
                               selected ? kOnAccent : kMutedStrong);
    lv_obj_set_pos(state.icons[index], 27, 25);
    state.labels[index] = Label(button, segment.label,
                                &lv_font_montserrat_24,
                                selected ? kOnAccent : kMutedStrong);
    lv_obj_set_pos(state.labels[index], 78, 27);
    OnClick(button, [segment] {
      BeginTransportSwitch(segment.wifi);
    });
  }
}

std::string Bytes(uint64_t value) {
  char result[48];
  if (value >= 1024ULL * 1024ULL * 1024ULL) {
    snprintf(result, sizeof(result), "%.2f GiB",
             static_cast<double>(value) / (1024.0 * 1024.0 * 1024.0));
  } else if (value >= 1024ULL * 1024ULL) {
    snprintf(result, sizeof(result), "%.1f MiB",
             static_cast<double>(value) / (1024.0 * 1024.0));
  } else {
    snprintf(result, sizeof(result), "%.1f KiB",
             static_cast<double>(value) / 1024.0);
  }
  return result;
}

lv_obj_t *BrandLayer(lv_obj_t *parent, const lv_image_dsc_t *source,
                     int scale, lv_color_t color) {
  auto *layer = lv_image_create(parent);
  lv_image_set_src(layer, source);
  lv_image_set_antialias(layer, true);
  lv_image_set_scale(layer, scale);
  lv_image_set_pivot(layer, 0, 0);
  lv_obj_set_style_image_recolor(layer, color, 0);
  lv_obj_set_style_image_recolor_opa(layer, LV_OPA_COVER, 0);
  lv_obj_remove_flag(layer, LV_OBJ_FLAG_CLICKABLE);
  return layer;
}

void FastbootMark(lv_obj_t *parent, bool landscape) {
  const int scale = landscape ? 170 : 230;
  const int width = assets::kAeraBrandWidth * scale / LV_SCALE_NONE;
  const int height = 500 * scale / LV_SCALE_NONE;
  const int text_top = landscape ? 430 : 760;
  const int y = (text_top - height) / 2;

  auto *crop = lv_obj_create(parent);
  Clear(crop);
  lv_obj_set_size(crop, width, height);
  lv_obj_align(crop, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_clip_corner(crop, true, 0);

  auto *white = BrandLayer(crop, &assets::kAeraBrandWhite, scale, kText);
  lv_obj_set_pos(white, 0, 0);

  constexpr int kSegmentX = 413;
  constexpr int kSegmentY = 230;
  constexpr int kSegmentWidth = 183;
  constexpr int kSegmentHeight = 121;
  auto *segment_crop = lv_obj_create(crop);
  Clear(segment_crop);
  lv_obj_set_pos(segment_crop, kSegmentX * scale / LV_SCALE_NONE,
                 kSegmentY * scale / LV_SCALE_NONE);
  lv_obj_set_size(segment_crop,
                  kSegmentWidth * scale / LV_SCALE_NONE,
                  kSegmentHeight * scale / LV_SCALE_NONE);
  lv_obj_set_style_clip_corner(segment_crop, true, 0);
  auto *segment = BrandLayer(segment_crop, &assets::kAeraBrandAccent,
                             scale, kCyan);
  lv_obj_set_pos(segment, -kSegmentX * scale / LV_SCALE_NONE,
                 -kSegmentY * scale / LV_SCALE_NONE);
}

void ParseTelemetry(const std::string &line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (start <= line.size()) {
    const size_t end = line.find('|', start);
    fields.emplace_back(line.substr(start, end == std::string::npos
                                               ? std::string::npos
                                               : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (fields.size() != 6 || fields[0] != "AERA1") return;
  char *end = nullptr;
  const uint64_t current = strtoull(fields[2].c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return;
  const uint64_t total = strtoull(fields[3].c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return;
  const long result = strtol(fields[4].c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || result < -1 || result > 1) return;
  g_telemetry.phase = fields[1];
  g_telemetry.target = fields[5];
  g_telemetry.current = current;
  g_telemetry.total = total;
  g_telemetry.result = static_cast<int>(result);
  g_telemetry.updated_ms = lv_tick_get();
  g_telemetry.dirty = true;
}

void PollSocket() {
  if (g_server_fd < 0) {
    g_server_fd = android_get_control_socket("recovery");
    if (g_server_fd < 0) return;
    const int flags = fcntl(g_server_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(g_server_fd, F_SETFL, flags | O_NONBLOCK);
    if (listen(g_server_fd, 4) != 0) {
      g_server_fd = -1;
      return;
    }
  }

  for (;;) {
    const int connection = accept(g_server_fd, nullptr, nullptr);
    if (connection < 0) break;
    if (g_client_fd >= 0) close(g_client_fd);
    g_client_fd = connection;
    const int flags = fcntl(g_client_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(g_client_fd, F_SETFL, flags | O_NONBLOCK);
    g_input.clear();
  }
  if (g_client_fd < 0) return;

  char buffer[512];
  for (;;) {
    const ssize_t count = recv(g_client_fd, buffer, sizeof(buffer), 0);
    if (count > 0) {
      g_input.append(buffer, static_cast<size_t>(count));
      while (true) {
        const size_t newline = g_input.find('\n');
        if (newline == std::string::npos) break;
        ParseTelemetry(g_input.substr(0, newline));
        g_input.erase(0, newline + 1);
      }
      if (g_input.size() > 4096) g_input.clear();
      continue;
    }
    if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      close(g_client_fd);
      g_client_fd = -1;
      g_input.clear();
    }
    break;
  }
}

void ApplyTelemetry() {
  if (!g_telemetry.dirty || g_view.screen == nullptr ||
      !lv_obj_is_valid(g_view.screen)) return;
  g_telemetry.dirty = false;

  std::string title = "Ready for fastboot commands";
  std::string detail = ReadyDetail();
  bool show_progress = false;
  const bool wireless = ActiveWirelessFastboot();
  const char *transport = wireless ? "Wi-Fi" : "USB";

  const bool failed = g_telemetry.result == 1;
  const bool finished = g_telemetry.result == 0;
  const std::string target = g_telemetry.target.empty()
      ? std::string("partition") : g_telemetry.target;
  if (failed) {
    title = g_telemetry.target.empty()
        ? i18n::Translate("Transfer failed")
        : i18n::Format("Operation failed · %s", target.c_str());
    detail = "Fastbootd reported an error. Check the host output before retrying.";
  } else if (g_telemetry.phase == "receiving") {
    title = "Receiving image";
    detail = g_telemetry.total == 0
        ? i18n::Format("Receiving the image over %s. The target partition follows next.",
                       transport)
        : i18n::Format(
              "%s of %s received over %s. The target partition follows next.",
              Bytes(g_telemetry.current).c_str(),
              Bytes(g_telemetry.total).c_str(), transport);
    show_progress = g_telemetry.total != 0;
  } else if (g_telemetry.phase == "received") {
    title = "Image received";
    detail = i18n::Format(
        "%s received. Waiting for the host to name the target partition.",
        Bytes(g_telemetry.total).c_str());
  } else if (g_telemetry.phase == "flashing") {
    title = i18n::Format(finished ? "Flashed %s" : "Flashing %s",
                         target.c_str());
    detail = finished
        ? "The partition was written successfully."
        : wireless
            ? i18n::Format("Writing %s over Wi-Fi. Keep the connection active.",
                           target.c_str())
            : i18n::Format("Writing %s. Keep the USB cable connected.",
                           target.c_str());
    show_progress = g_telemetry.total != 0;
  } else if (g_telemetry.phase == "erasing") {
    title = i18n::Format(finished ? "Erased %s" : "Erasing %s",
                         target.c_str());
    detail = finished ? "The partition was erased successfully."
                      : i18n::Format(
                            "Erasing %s. Keep the USB cable connected.",
                            target.c_str());
  } else if (g_telemetry.phase == "updating") {
    title = i18n::Format(finished ? "Updated %s" : "Updating %s",
                         target.c_str());
    detail = finished ? "Dynamic partition metadata was updated successfully."
                      : wireless
                          ? "Applying dynamic partition metadata over Wi-Fi. Keep the connection active."
                          : "Applying dynamic partition metadata. Do not disconnect USB.";
  }

  i18n::BindLabel(g_view.title, title.c_str());
  i18n::BindLabel(g_view.detail, detail.c_str());
  if (show_progress) {
    const int percent = static_cast<int>(std::min<uint64_t>(
        100, g_telemetry.current * 100 / std::max<uint64_t>(1, g_telemetry.total)));
    // Telemetry arrives about every 50 ms. Applying each sample directly
    // keeps the indicator in lockstep with the percentage instead of
    // repeatedly restarting LVGL's value animation.
    lv_bar_set_value(g_view.progress, percent, LV_ANIM_OFF);
    char value[8];
    snprintf(value, sizeof(value), "%d%%", percent);
    i18n::BindLabel(g_view.percent, value);
    lv_obj_remove_flag(g_view.progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_view.percent, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_view.progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_view.percent, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace

void PollFastbootTelemetry() {
  PollSocket();
  if (g_telemetry.result >= 0 &&
      lv_tick_elaps(g_telemetry.updated_ms) > 2400) {
    g_telemetry = TelemetryState{};
  }
  ApplyTelemetry();
}

void BuildFastbootScene(lv_obj_t *screen, ActionCallback callback,
                        void *context) {
  g_transport_pill = {};
  const bool landscape = Landscape(screen);
  const FastbootWifiStatus fastboot_wifi = RecoveryFastbootWifiStatus();
  const bool wireless_transport = WirelessFastboot();
  MainBackground(screen);
  // Fastbootd owns USB. Keep the normal pull-down controls disabled so Wi-Fi,
  // rotation and recovery jobs cannot be started in this restricted mode.
  AttachStatusBar(screen, nullptr, nullptr, StatusBarAction::kNone, true);

  auto *mode = Kicker(screen, "USERSPACE FASTBOOT", kAccent);
  lv_obj_set_pos(mode, 80, landscape ? 210 : 252);
  auto *title = Label(screen, "Fastbootd", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 80, landscape ? 264 : 314);
  if (fastboot_wifi.enabled) {
    AddTransportPill(screen, wireless_transport);
  }

  auto *hero = lv_obj_create(screen);
  Panel(hero, 52, kMainSheet);
  lv_obj_set_pos(hero, 64, landscape ? 330 : 430);
  lv_obj_set_size(hero, landscape ? 1400 : 1312,
                  landscape ? 760 : 1160);
  lv_obj_set_style_border_width(hero, 1, 0);
  lv_obj_set_style_border_color(hero, kMainLine, 0);
  lv_obj_set_style_border_opa(hero, LV_OPA_50, 0);

  FastbootMark(hero, landscape);

  auto *ready = Label(hero, "Ready for fastboot commands",
                      &lv_font_montserrat_48, kText);
  lv_obj_align(ready, LV_ALIGN_TOP_MID, 0, landscape ? 430 : 760);
  auto *detail = Label(
      hero, ReadyDetail().c_str(),
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_width(detail, landscape ? 1120 : 1160);
  lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, landscape ? 514 : 862);
  g_transport_pill.detail = detail;

  auto *progress = lv_bar_create(hero);
  lv_obj_set_size(progress, landscape ? 1080 : 1120, 24);
  lv_obj_align(progress, LV_ALIGN_TOP_MID, 0, landscape ? 640 : 1010);
  lv_bar_set_range(progress, 0, 100);
  lv_obj_set_style_radius(progress, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(progress, kInset, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress, kAccent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_bar_set_value(progress, 0, LV_ANIM_OFF);
  lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
  auto *percent = Label(hero, "0%", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_align(percent, LV_ALIGN_TOP_MID, 0, landscape ? 680 : 1052);
  lv_obj_add_flag(percent, LV_OBJ_FLAG_HIDDEN);

  g_view = {screen, ready, detail, progress, percent};
  g_telemetry.dirty = true;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    if (g_view.screen == lv_event_get_target_obj(event)) g_view = {};
  }, LV_EVENT_DELETE, nullptr);
  PollFastbootTelemetry();

  const std::string slot = RecoverySlot();
  auto *slot_label = Kicker(
      hero, ("ACTIVE SLOT  " + (slot.empty() ? std::string("—") : slot)).c_str(),
      kMutedStrong);
  lv_obj_align(slot_label, LV_ALIGN_BOTTOM_RIGHT, -58, -54);

  auto *actions = lv_obj_create(screen);
  Clear(actions);
  lv_obj_set_pos(actions, landscape ? 1510 : 64,
                 landscape ? 330 : 1640);
  lv_obj_set_size(actions, landscape ? 1594 : 1312,
                  landscape ? 1050 : 1384);

  {
    const std::string connect = fastboot_wifi.connect_command.empty()
        ? "adb connect <phone-ip>:5555" : fastboot_wifi.connect_command;
    const std::string forward = fastboot_wifi.forward_command.empty()
        ? "adb -s <phone-ip>:5555 forward tcp:5554 tcp:5554"
        : fastboot_wifi.forward_command;
    const std::string verify = fastboot_wifi.fastboot_command.empty()
        ? "fastboot -s tcp:127.0.0.1:5554 getvar product"
        : fastboot_wifi.fastboot_command;
    auto *guide = lv_obj_create(actions);
    Panel(guide, 34, kMainPanel);
    lv_obj_set_pos(guide, 0, landscape ? 518 : 610);
    lv_obj_set_size(guide, landscape ? 1594 : 1312,
                    landscape ? 252 : 450);
    lv_obj_set_style_border_width(guide, 1, 0);
    lv_obj_set_style_border_color(guide, kMainLine, 0);
    lv_obj_set_style_border_opa(guide, LV_OPA_40, 0);
    if (!wireless_transport)
      lv_obj_add_flag(guide, LV_OBJ_FLAG_HIDDEN);
    g_transport_pill.guide = guide;
    auto *guide_title = Kicker(guide, "WI-FI FASTBOOT", kAccent);
    lv_obj_set_pos(guide_title, 38, landscape ? 24 : 30);
    auto *guide_detail = Label(guide,
        "Run these commands on the paired computer",
        landscape ? &lv_font_montserrat_24 : &lv_font_montserrat_28,
        kMuted);
    lv_obj_set_pos(guide_detail, 38, landscape ? 58 : 72);
    const std::array<std::string, 3> commands{{connect, forward, verify}};
    for (size_t index = 0; index < commands.size(); ++index) {
      auto *step = Label(guide, std::to_string(index + 1).c_str(),
                         landscape ? &lv_font_montserrat_28
                                   : &lv_font_montserrat_32,
                         kAccent);
      lv_obj_set_pos(step, 38, (landscape ? 102 : 132) +
                              static_cast<int>(index) * (landscape ? 48 : 82));
      auto *command = Label(guide, commands[index].c_str(),
                            landscape ? &lv_font_montserrat_28
                                      : &lv_font_montserrat_32,
                            kText);
      lv_obj_set_pos(command, 86, (landscape ? 102 : 132) +
                                  static_cast<int>(index) * (landscape ? 48 : 82));
      lv_obj_set_width(command, (landscape ? 1594 : 1312) - 130);
      lv_label_set_long_mode(command, LV_LABEL_LONG_DOT);
    }
  }

  struct Destination {
    const char *icon;
    const char *name;
    const char *detail;
    Action action;
  };
  constexpr std::array<Destination, 5> destinations{{
      {LV_SYMBOL_HOME, "Android", "Boot the operating system",
       Action::kRebootSystem},
      {LV_SYMBOL_REFRESH, "Recovery", "Return to full AERA recovery",
       Action::kRebootRecovery},
      {LV_SYMBOL_SETTINGS, "Bootloader", "Open hardware fastboot",
       Action::kRebootBootloader},
      {LV_SYMBOL_POWER, "Power off", "Shut down the device",
       Action::kPowerOff},
      {LV_SYMBOL_TRASH, "Format Data", "Erase internal storage and encryption",
       Action::kFormatData},
  }};
  for (size_t index = 0; index < destinations.size(); ++index) {
    const auto destination = destinations[index];
    const bool format = destination.action == Action::kFormatData;
    const int column = static_cast<int>(index % 2);
    const int row = static_cast<int>(index / 2);
    const int gap = landscape ? 26 : 24;
    const int card_width = format ? (landscape ? 1594 : 1312)
                                  : (landscape ? 784 : 644);
    const int card_height = format ? (landscape ? 220 : 250)
                                   : (landscape ? 232 : 270);
    auto *card = lv_button_create(actions);
    Panel(card, 38, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_size(card, card_width, card_height);
    if (format) {
      lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, landscape ? -30 : -24);
    } else {
      lv_obj_set_pos(card, column * (card_width + gap),
                     row * (card_height + (landscape ? 26 : 24)));
    }
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, kMainLine, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
    OnClick(card, [screen, callback, context, destination, slot] {
      if (destination.action == Action::kFormatData) {
        callback(destination.action, context);
        return;
      }
      const std::string title = destination.action == Action::kPowerOff
          ? "Power off device?"
          : std::string("Reboot to ") + destination.name + "?";
      std::string detail = std::string(destination.detail) + ".\n\n";
      if (!slot.empty() && destination.action != Action::kPowerOff)
        detail += "Active slot: " + slot + "  •  ";
      detail += "The current Fastbootd session will end.";
      Sheet(screen, title, detail, [screen, callback, context, destination] {
        if (destination.action == Action::kRebootRecovery) {
          ModeTransition(screen, false, [callback, context, destination] {
            callback(destination.action, context);
          });
          return;
        }
        const bool power_off = destination.action == Action::kPowerOff;
        const std::string transition_title = power_off
            ? "Powering off"
            : std::string("Rebooting to ") + destination.name;
        PowerTransition(screen, transition_title,
                        [callback, context, destination] {
                          callback(destination.action, context);
                        });
      }, 0, false, SheetPresentation::kCompactGlass,
            destination.action == Action::kPowerOff
                ? "Slide to power off" : "Slide to reboot");
    });
    auto *icon = Label(card, destination.icon, &lv_font_montserrat_48,
        kAccent);
    lv_obj_set_pos(icon, 38, format ? 70 : (landscape ? 32 : 32));
    auto *name = Label(card, destination.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 122, format ? 52 : (landscape ? 40 : 40));
    auto *copy = Label(card, destination.detail, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(copy, format ? 122 : 38,
                   format ? 118 : (landscape ? 126 : 126));
    lv_obj_set_width(copy, card_width - (format ? 160 : 76));
    AnimateEnter(card, 50 + static_cast<uint32_t>(index) * 35, 12);
  }

  auto *warning = Label(screen,
                        wireless_transport
                            ? "Keep Wi-Fi connected while a command is writing data."
                            : "Do not disconnect USB while a command is writing data.",
                        &lv_font_montserrat_24, kAmber);
  lv_obj_align(warning, LV_ALIGN_BOTTOM_MID, 0, landscape ? -38 : -72);
  g_transport_pill.warning = warning;
}

}  // namespace aeraui
