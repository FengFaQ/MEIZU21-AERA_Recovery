/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "ui_components.hpp"
#include "phone_keyboard.hpp"

namespace aeraui {
namespace {
using namespace widgets;
using namespace design;

constexpr int32_t kWifiIconSize = 72;
#include "wifi_open_icon.inc"
#include "wifi_locked_icon.inc"
#include "wifi_saved_icon.inc"

#define AERA_WIFI_IMAGE(name, pixels)                                    \
  static const lv_image_dsc_t name = {                                  \
      .header = {.magic = LV_IMAGE_HEADER_MAGIC,                         \
                 .cf = LV_COLOR_FORMAT_A8,                              \
                 .flags = 0,                                            \
                 .w = kWifiIconSize,                                    \
                 .h = kWifiIconSize,                                    \
                 .stride = kWifiIconSize},                              \
      .data_size = sizeof(pixels),                                      \
      .data = pixels,                                                    \
  }

AERA_WIFI_IMAGE(kWifiOpenImage, _tmp_aera_wlan_raw);
AERA_WIFI_IMAGE(kWifiLockedImage, _tmp_aera_wlan_locked_raw);
AERA_WIFI_IMAGE(kWifiSavedImage, _tmp_aera_wlan_saved_raw);
#undef AERA_WIFI_IMAGE

WifiRequest gRequest;

struct WifiUi {
  WifiScene scene;
  lv_obj_t *screen = nullptr;
  lv_obj_t *radio_switch = nullptr;
  lv_obj_t *hero = nullptr;
  lv_obj_t *status_plate = nullptr;
  lv_obj_t *status_icon = nullptr;
  lv_timer_t *timer = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  WifiStatus snapshot;
  std::string signature;
  WifiOperation running = WifiOperation::kScan;
  bool busy = false;
  bool activity_animating = false;
  uint32_t busy_phase = 0;
  int list_width = 1312;
  lv_obj_t *settings_page = nullptr;
  lv_obj_t *settings_message = nullptr;
  lv_obj_t *adb_switch = nullptr;
  lv_obj_t *adb_toggle = nullptr;
  lv_obj_t *adb_status = nullptr;
  lv_obj_t *adb_detail = nullptr;
  lv_obj_t *adb_endpoint = nullptr;
  lv_obj_t *adb_command = nullptr;
  lv_obj_t *pair_panel = nullptr;
  lv_obj_t *pair_command = nullptr;
  lv_obj_t *pair_code = nullptr;
  lv_obj_t *pair_button = nullptr;
  lv_obj_t *adb_card = nullptr;
  lv_obj_t *paired_caption = nullptr;
  lv_obj_t *paired_list = nullptr;
  lv_obj_t *fastboot_caption = nullptr;
  lv_obj_t *fastboot_card = nullptr;
  lv_obj_t *fastboot_switch = nullptr;
  lv_obj_t *fastboot_toggle = nullptr;
  lv_obj_t *fastboot_status = nullptr;
  lv_obj_t *fastboot_detail = nullptr;
  lv_obj_t *fastboot_guide = nullptr;
  lv_obj_t *fastboot_connect = nullptr;
  lv_obj_t *fastboot_forward = nullptr;
  lv_obj_t *fastboot_client = nullptr;
  lv_obj_t *security_note = nullptr;
  std::string paired_signature;
  bool last_pairing = false;
  bool adb_busy = false;
  bool adb_desired = false;
  bool fastboot_busy = false;
  bool fastboot_desired = false;
};

std::string Signature(const WifiStatus &status) {
  std::string value = status.state + "|" + status.ssid + "|" +
      (status.enabled ? "1" : "0") + (status.connected ? "1" : "0");
  for (const auto &network : status.networks)
    value += "|" + network.ssid + ":" + network.security +
        (network.saved ? ":s" : "") + (network.connected ? ":c" : "");
  return value;
}

void Dispatch(WifiUi *state, WifiRequest request) {
  if (!state || state->busy) return;
  gRequest = std::move(request);
  state->callback(Action::kRunWifiOperation, state->context);
}

void CloseOverlay(lv_obj_t *overlay) { lv_obj_delete_async(overlay); }

void SelectNetwork(WifiUi *state, WifiNetwork network);
void RefreshWifiSettings(WifiUi *state);
void RefreshPairedComputers(WifiUi *state, bool force = false);


lv_obj_t *SectionCaption(lv_obj_t *parent, int y, const char *text) {
  auto *caption = Label(parent, text, &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(caption, 3, 0);
  lv_obj_set_pos(caption, 16, y);
  return caption;
}

void RefreshPairedComputers(WifiUi *state, bool force) {
  if (!state || !state->paired_list ||
      !lv_obj_is_valid(state->paired_list)) return;
  const auto devices = RecoveryAdbPairedDevices();
  std::string signature;
  for (const auto &device : devices)
    signature += device.fingerprint + "|" + device.name + ";";
  if (!force && signature == state->paired_signature) return;
  state->paired_signature = std::move(signature);
  lv_obj_clean(state->paired_list);
  lv_obj_update_layout(state->paired_list);
  const int width = std::max(320,
      static_cast<int>(lv_obj_get_width(state->paired_list)));

  if (devices.empty()) {
    auto *empty = lv_obj_create(state->paired_list);
    Panel(empty, 28, kMainPanel);
    lv_obj_set_pos(empty, 0, 0);
    lv_obj_set_size(empty, width, 190);
    auto *icon = Label(empty, LV_SYMBOL_USB, &lv_font_montserrat_40, kAccent);
    lv_obj_set_pos(icon, 32, 64);
    auto *title = Label(empty, "No paired computers",
                        &lv_font_montserrat_32, kText);
    lv_obj_set_pos(title, 108, 38);
    auto *detail = Label(empty,
        "Use Pair new computer above to authorize one.",
        &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(detail, 108, 100);
    lv_obj_set_width(detail, width - 150);
  } else {
    int y = 0;
    for (const auto &device : devices) {
      auto *card = lv_obj_create(state->paired_list);
      Panel(card, 28, kMainPanel);
      lv_obj_set_pos(card, 0, y);
      lv_obj_set_size(card, width, 170);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_color(card, kMainLine, 0);
      lv_obj_set_style_border_opa(card, LV_OPA_30, 0);

      auto *icon = Label(card, LV_SYMBOL_USB, &lv_font_montserrat_40, kAccent);
      lv_obj_set_pos(icon, 30, 62);
      const std::string name = device.name.empty() ? "Paired computer" : device.name;
      auto *title = Label(card, name.c_str(), &lv_font_montserrat_32, kText);
      lv_obj_set_pos(title, 102, 30);
      lv_obj_set_width(title, width - 430);
      lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
      std::string fingerprint = "Fingerprint  " + device.fingerprint;
      auto *detail = Label(card, fingerprint.c_str(),
                           &lv_font_montserrat_20, kMuted);
      lv_obj_set_pos(detail, 102, 94);
      lv_obj_set_width(detail, width - 430);
      lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);

      auto *forget = Button(card, "Forget",
          [state, device] {
            if (RecoveryForgetAdbDevice(device.fingerprint)) {
              i18n::BindLabel(state->settings_message,
                  "Computer forgotten. Pair it again to restore access.");
              lv_obj_set_style_text_color(state->settings_message, kGreen, 0);
              // Rebuild after this button's event has returned; the rebuild
              // deletes the row which currently owns the callback.
              lv_async_call([](void *context) {
                RefreshPairedComputers(static_cast<WifiUi *>(context), true);
              }, state);
            } else {
              i18n::BindLabel(state->settings_message,
                              "Could not forget this computer.");
              lv_obj_set_style_text_color(state->settings_message, kRed, 0);
            }
          });
      lv_obj_set_size(forget, 230, 88);
      lv_obj_align(forget, LV_ALIGN_RIGHT_MID, -28, 0);
      lv_obj_set_style_bg_color(forget, kMainSheet, 0);
      lv_obj_set_style_bg_color(forget, kMainSelected, LV_STATE_PRESSED);
      auto *forget_label = lv_obj_get_child(forget, 0);
      lv_obj_set_style_text_color(forget_label, kRed, 0);
      lv_obj_set_style_text_align(forget_label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(forget_label);
      y += 186;
    }
  }
}

void RefreshWifiSettings(WifiUi *state) {
  if (!state || !state->settings_page || !lv_obj_is_valid(state->settings_page))
    return;
  const AdbWifiStatus adb = RecoveryAdbWifiStatus();
  const bool visual_enabled = state->adb_busy ? state->adb_desired : adb.enabled;
  if (visual_enabled) lv_obj_add_state(state->adb_switch, LV_STATE_CHECKED);
  else lv_obj_remove_state(state->adb_switch, LV_STATE_CHECKED);

  const char *status = state->adb_busy ?
      (state->adb_desired ? "Starting wireless ADB..." :
                            "Stopping wireless ADB...") :
      !adb.wifi_connected ? "Wi-Fi connection required" :
      adb.pairing ? "Waiting for your computer" :
      adb.enabled ? "Wireless ADB is ready" : "Wireless ADB is off";
  const char *detail = state->adb_busy ?
      (state->adb_desired ?
          "Preparing a secure connection for authorized computers." :
          "Closing wireless debugging for this recovery session.") :
      !adb.wifi_connected ?
      "Connect to a network before enabling wireless debugging." :
      adb.pairing ? "Complete pairing from the computer within two minutes." :
      adb.enabled ? "This recovery session accepts authorized ADB clients." :
      "Disabled by default for security. Enable it only when needed.";
  i18n::BindLabel(state->adb_status, status);
  i18n::BindLabel(state->adb_detail, detail);
  lv_obj_set_style_text_color(state->adb_status,
      state->adb_busy ? kAccent :
      adb.enabled && adb.wifi_connected ? kGreen : kText, 0);
  if (state->adb_toggle) {
    if (state->adb_busy) lv_obj_add_state(state->adb_toggle, LV_STATE_DISABLED);
    else lv_obj_remove_state(state->adb_toggle, LV_STATE_DISABLED);
  }

  const std::string endpoint = !state->adb_busy && adb.enabled &&
      !adb.ip_address.empty() ?
      adb.ip_address + ":" + adb.connect_port : "Not available";
  lv_label_set_text(state->adb_endpoint, endpoint.c_str());
  const std::string command = adb.connect_command.empty() ?
      "Enable ADB over Wi-Fi to reveal the PC command." : adb.connect_command;
  lv_label_set_text(state->adb_command, command.c_str());

  if (!state->adb_busy && adb.enabled && adb.wifi_connected) {
    lv_obj_clear_flag(state->pair_button, LV_OBJ_FLAG_HIDDEN);
    auto *label = lv_obj_get_child(state->pair_button, 0);
    i18n::BindLabel(label, adb.pairing ? "Cancel pairing" : "Pair new computer");
    FitButtonLabel(state->pair_button);
  } else {
    lv_obj_add_flag(state->pair_button, LV_OBJ_FLAG_HIDDEN);
  }

  if (adb.pairing) {
    lv_obj_clear_flag(state->pair_panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(state->pair_command, adb.pairing_command.c_str());
    lv_label_set_text(state->pair_code, adb.pairing_code.c_str());
  } else {
    lv_obj_add_flag(state->pair_panel, LV_OBJ_FLAG_HIDDEN);
  }

  const int paired_top = adb.pairing ? 950 : 760;
  lv_obj_set_y(state->paired_caption, paired_top);
  lv_obj_set_y(state->paired_list, paired_top + 44);
  const int adb_card_height = paired_top + 348;
  lv_obj_set_height(state->adb_card, adb_card_height);

  const FastbootWifiStatus fastboot = RecoveryFastbootWifiStatus();
  const bool fastboot_enabled = state->fastboot_busy ?
      state->fastboot_desired : fastboot.enabled;
  if (fastboot_enabled)
    lv_obj_add_state(state->fastboot_switch, LV_STATE_CHECKED);
  else
    lv_obj_remove_state(state->fastboot_switch, LV_STATE_CHECKED);

  const int fastboot_caption_top = 538 + adb_card_height + 52;
  const int fastboot_card_top = fastboot_caption_top + 52;
  lv_obj_set_y(state->fastboot_caption, fastboot_caption_top);
  lv_obj_set_y(state->fastboot_card, fastboot_card_top);
  const char *fastboot_title = state->fastboot_busy ?
      (state->fastboot_desired ? "Enabling secure fastboot..." :
                                 "Disabling fastboot over Wi-Fi...") :
      fastboot_enabled ? "Fastboot over Wi-Fi is ready" :
                         "Fastboot over Wi-Fi is off";
  i18n::BindLabel(state->fastboot_status, fastboot_title);
  i18n::BindLabel(state->fastboot_detail,
      state->fastboot_busy ? "Updating the paired-computer tunnel." :
      fastboot.reason.c_str());
  lv_obj_set_style_text_color(state->fastboot_status,
      fastboot_enabled ? kGreen : kText, 0);
  if (state->fastboot_busy || (!fastboot.available && !fastboot_enabled))
    lv_obj_add_state(state->fastboot_toggle, LV_STATE_DISABLED);
  else
    lv_obj_remove_state(state->fastboot_toggle, LV_STATE_DISABLED);

  if (fastboot_enabled) {
    lv_obj_clear_flag(state->fastboot_guide, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(state->fastboot_connect,
        fastboot.connect_command.empty() ? "adb connect <shown address>" :
                                           fastboot.connect_command.c_str());
    lv_label_set_text(state->fastboot_forward,
        fastboot.forward_command.empty() ?
            "adb -s <address> forward tcp:5554 tcp:5554" :
            fastboot.forward_command.c_str());
    lv_label_set_text(state->fastboot_client,
        fastboot.fastboot_command.empty() ?
            "fastboot -s tcp:127.0.0.1:5554 devices" :
            fastboot.fastboot_command.c_str());
  } else {
    lv_obj_add_flag(state->fastboot_guide, LV_OBJ_FLAG_HIDDEN);
  }

  const int fastboot_card_height = fastboot_enabled ? 684 : 234;
  lv_obj_set_height(state->fastboot_card, fastboot_card_height);
  lv_obj_set_y(state->security_note,
               fastboot_card_top + fastboot_card_height + 28);
  if (state->last_pairing && !adb.pairing)
    RefreshPairedComputers(state, true);
  state->last_pairing = adb.pairing;
}

void ShowWifiSettings(WifiUi *state) {
  if (!state || state->settings_page) return;
  auto *page = lv_obj_create(state->screen);
  state->settings_page = page;
  lv_obj_set_user_data(page, &kModalMarker);
  Clear(page);
  lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
  Header(page, "Wi-Fi settings", "Automation & wireless debugging",
         state->callback, state->context);
  lv_obj_add_event_cb(page, [](lv_event_t *event) {
    auto *s = static_cast<WifiUi *>(lv_event_get_user_data(event));
    s->settings_page = nullptr;
    s->settings_message = nullptr;
    s->adb_switch = nullptr;
    s->adb_toggle = nullptr;
    s->adb_status = nullptr;
    s->adb_detail = nullptr;
    s->adb_endpoint = nullptr;
    s->adb_command = nullptr;
    s->pair_panel = nullptr;
    s->pair_command = nullptr;
    s->pair_code = nullptr;
    s->pair_button = nullptr;
    s->adb_card = nullptr;
    s->paired_caption = nullptr;
    s->paired_list = nullptr;
    s->fastboot_caption = nullptr;
    s->fastboot_card = nullptr;
    s->fastboot_switch = nullptr;
    s->fastboot_toggle = nullptr;
    s->fastboot_status = nullptr;
    s->fastboot_detail = nullptr;
    s->fastboot_guide = nullptr;
    s->fastboot_connect = nullptr;
    s->fastboot_forward = nullptr;
    s->fastboot_client = nullptr;
    s->security_note = nullptr;
    s->paired_signature.clear();
  }, LV_EVENT_DELETE, state);

  const bool landscape = Landscape(page);
  auto *back = Button(page, LV_SYMBOL_LEFT "  Networks",
                      [page] { CloseOverlay(page); });
  lv_obj_set_size(back, 330, 104);
  lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -64, landscape ? 184 : 232);
  lv_obj_set_style_radius(back, 32, 0);

  const int top = landscape ? 350 : 440;
  const int available = lv_obj_get_height(lv_obj_get_screen(page)) - top -
      NavigationHeight(page) - 24;
  auto *scroll = Scroll(page, top, std::max(640, available));
  lv_obj_set_style_pad_bottom(scroll, 72, 0);
  lv_obj_update_layout(scroll);
  const int width = std::max(320, static_cast<int>(lv_obj_get_width(scroll)));
  auto *content = lv_obj_create(scroll);
  Clear(content); lv_obj_set_pos(content, 0, 0);
  lv_obj_set_width(content, width); lv_obj_set_height(content, LV_SIZE_CONTENT);

  SectionCaption(content, 0, "STARTUP");
  auto *message = Label(content, "Changes apply immediately",
                        &lv_font_montserrat_20, kMuted);
  state->settings_message = message;
  lv_obj_set_pos(message, 16, 416);
  lv_obj_set_width(message, width - 32);
  SettingsRow(content, 52, "Auto-enable",
      "Turn Wi-Fi on when recovery starts", RecoveryWifiAutoEnable(),
      [](bool enabled) { return RecoverySetWifiAutoEnable(enabled); }, message);
  SettingsRow(content, 232, "Auto-connect",
      "Reconnect to a saved network automatically", RecoveryWifiAutoConnect(),
      [](bool enabled) { return RecoverySetWifiAutoConnect(enabled); }, message);

  SectionCaption(content, 486, "DEVELOPER ACCESS");
  auto *adb_card = lv_obj_create(content);
  state->adb_card = adb_card;
  Panel(adb_card, 36, kMainSheet);
  lv_obj_set_pos(adb_card, 0, 538);
  lv_obj_set_size(adb_card, width, 1108);
  lv_obj_set_style_border_width(adb_card, 1, 0);
  lv_obj_set_style_border_color(adb_card, kMainLine, 0);
  lv_obj_set_style_border_opa(adb_card, LV_OPA_30, 0);

  auto *adb_icon = IconPlate(adb_card, LV_SYMBOL_USB, kAccent, kAccentSoft, 104);
  lv_obj_set_pos(adb_icon, 32, 34);
  state->adb_status = Label(adb_card, "Wireless ADB is off",
                            &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->adb_status, 168, 30);
  lv_obj_set_width(state->adb_status, width - 430);
  state->adb_detail = Label(adb_card,
      "Disabled by default for security. Enable it only when needed.",
      &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(state->adb_detail, 168, 88);
  lv_obj_set_width(state->adb_detail, width - 430);
  state->adb_switch = lv_switch_create(adb_card);
  StyleSwitch(state->adb_switch, RecoveryAdbOverWifi());
  lv_obj_align(state->adb_switch, LV_ALIGN_TOP_RIGHT, -34, 54);
  auto *adb_toggle = lv_button_create(adb_card);
  state->adb_toggle = adb_toggle;
  Clear(adb_toggle);
  lv_obj_set_pos(adb_toggle, 0, 0);
  lv_obj_set_size(adb_toggle, width, 172);
  lv_obj_set_style_bg_opa(adb_toggle, LV_OPA_TRANSP, 0);
  lv_obj_move_to_index(adb_toggle, 0);
  OnClick(adb_toggle, [state] {
    if (state->adb_busy) return;
    WifiRequest request;
    request.operation = RecoveryAdbOverWifi() ? WifiOperation::kDisableAdb
                                              : WifiOperation::kEnableAdb;
    Dispatch(state, std::move(request));
  });

  auto *line = lv_obj_create(adb_card);
  Clear(line);
  lv_obj_set_pos(line, 32, 172);
  lv_obj_set_size(line, width - 64, 1);
  lv_obj_set_style_bg_color(line, kMainLine, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);

  auto *address_title = Label(adb_card, "CONNECTION ADDRESS",
                              &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(address_title, 3, 0);
  lv_obj_set_pos(address_title, 36, 214);
  state->adb_endpoint = Label(adb_card, "Not available",
                              &lv_font_montserrat_40, kText);
  lv_obj_set_pos(state->adb_endpoint, 36, 258);
  lv_obj_set_width(state->adb_endpoint, width - 72);

  auto *command_card = lv_obj_create(adb_card);
  Panel(command_card, 26, kMainPanel);
  lv_obj_set_pos(command_card, 32, 334);
  lv_obj_set_size(command_card, width - 64, 128);
  auto *terminal = Label(command_card, LV_SYMBOL_RIGHT, &lv_font_montserrat_28,
                         kAccent);
  lv_obj_set_pos(terminal, 28, 46);
  state->adb_command = Label(command_card,
      "Enable ADB over Wi-Fi to reveal the PC command.",
      &lv_font_montserrat_36, kMutedStrong);
  lv_obj_set_pos(state->adb_command, 82, 34);
  lv_obj_set_width(state->adb_command, width - 190);
  lv_label_set_long_mode(state->adb_command, LV_LABEL_LONG_DOT);

  auto *steps = Label(adb_card,
      "On the computer, open a terminal and run the command above.\n"
      "Then use  adb devices  to confirm the connection.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(steps, 36, 494);
  lv_obj_set_width(steps, width - 72);
  lv_obj_set_style_text_line_space(steps, 16, 0);

  state->pair_button = Button(adb_card, "Pair new computer", [state] {
    const AdbWifiStatus adb = RecoveryAdbWifiStatus();
    const bool ok = adb.pairing ? RecoveryStopAdbPairing()
                                : RecoveryStartAdbPairing();
    i18n::BindLabel(state->settings_message,
        ok ? (adb.pairing ? "ADB pairing cancelled"
                          : "Pairing is available for two minutes")
           : "Could not start ADB pairing");
    lv_obj_set_style_text_color(state->settings_message, ok ? kGreen : kRed, 0);
    RefreshWifiSettings(state);
  }, true);
  lv_obj_set_pos(state->pair_button, 32, 614);
  lv_obj_set_size(state->pair_button, width - 64, 112);

  state->pair_panel = lv_obj_create(adb_card);
  Panel(state->pair_panel, 28, kMainPanel);
  lv_obj_set_pos(state->pair_panel, 32, 750);
  lv_obj_set_size(state->pair_panel, width - 64, 154);
  auto *pair_label = Label(state->pair_panel,
                           "PAIRING COMMAND (TEMPORARY PORT)",
                           &lv_font_montserrat_16, kAccent);
  lv_obj_set_style_text_letter_space(pair_label, 2, 0);
  lv_obj_set_pos(pair_label, 24, 18);
  state->pair_command = Label(state->pair_panel, "adb pair",
                              &lv_font_montserrat_36, kText);
  lv_obj_set_pos(state->pair_command, 24, 50);
  lv_obj_set_width(state->pair_command, width - 330);
  auto *code_title = Label(state->pair_panel, "CODE", &lv_font_montserrat_16,
                           kMuted);
  lv_obj_align(code_title, LV_ALIGN_TOP_RIGHT, -28, 18);
  state->pair_code = Label(state->pair_panel, "000000",
                           &lv_font_montserrat_36, kAccent);
  lv_obj_align(state->pair_code, LV_ALIGN_TOP_RIGHT, -28, 54);

  state->paired_caption = Label(adb_card, "PAIRED COMPUTERS",
                                &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(state->paired_caption, 3, 0);
  lv_obj_set_pos(state->paired_caption, 36, 760);
  state->paired_list = lv_obj_create(adb_card);
  Clear(state->paired_list);
  lv_obj_set_pos(state->paired_list, 32, 804);
  lv_obj_set_size(state->paired_list, width - 64, 260);
  lv_obj_set_scroll_dir(state->paired_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->paired_list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_bottom(state->paired_list, 16, 0);

  state->fastboot_caption = Label(content, "FASTBOOT TUNNEL",
                                  &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(state->fastboot_caption, 3, 0);

  auto *fastboot_card = lv_obj_create(content);
  state->fastboot_card = fastboot_card;
  Panel(fastboot_card, 36, kMainSheet);
  lv_obj_set_x(fastboot_card, 0);
  lv_obj_set_width(fastboot_card, width);
  lv_obj_set_style_border_width(fastboot_card, 1, 0);
  lv_obj_set_style_border_color(fastboot_card, kMainLine, 0);
  lv_obj_set_style_border_opa(fastboot_card, LV_OPA_30, 0);

  auto *fastboot_row = lv_button_create(fastboot_card);
  state->fastboot_toggle = fastboot_row;
  Panel(fastboot_row, 28, kMainPanel);
  Interactive(fastboot_row, kMainSelected);
  lv_obj_set_pos(fastboot_row, 32, 32);
  lv_obj_set_size(fastboot_row, width - 64, 170);
  lv_obj_set_style_border_width(fastboot_row, 1, 0);
  lv_obj_set_style_border_color(fastboot_row, kMainLine, 0);
  lv_obj_set_style_border_opa(fastboot_row, LV_OPA_30, 0);
  auto *fastboot_icon = Label(fastboot_row, LV_SYMBOL_SHUFFLE,
                              &lv_font_montserrat_40, kAccent);
  lv_obj_set_pos(fastboot_icon, 30, 62);
  state->fastboot_status = Label(fastboot_row, "Fastboot over Wi-Fi is off",
                                 &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->fastboot_status, 102, 28);
  lv_obj_set_width(state->fastboot_status, width - 470);
  state->fastboot_detail = Label(fastboot_row,
      "Available only through a paired computer",
      &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(state->fastboot_detail, 102, 92);
  lv_obj_set_width(state->fastboot_detail, width - 470);
  state->fastboot_switch = lv_switch_create(fastboot_row);
  StyleSwitch(state->fastboot_switch, false);
  lv_obj_align(state->fastboot_switch, LV_ALIGN_RIGHT_MID, -28, 0);
  OnClick(fastboot_row, [state] {
    if (state->fastboot_busy) return;
    const FastbootWifiStatus status = RecoveryFastbootWifiStatus();
    WifiRequest request;
    request.operation = status.enabled ? WifiOperation::kDisableFastbootWifi
                                       : WifiOperation::kEnableFastbootWifi;
    Dispatch(state, std::move(request));
  });

  state->fastboot_guide = lv_obj_create(fastboot_card);
  Panel(state->fastboot_guide, 28, kMainPanel);
  lv_obj_set_pos(state->fastboot_guide, 32, 222);
  lv_obj_set_size(state->fastboot_guide, width - 64, 430);
  auto *guide_title = Label(state->fastboot_guide,
      "CONNECT FROM THE PAIRED COMPUTER",
      &lv_font_montserrat_16, kAccent);
  lv_obj_set_style_text_letter_space(guide_title, 2, 0);
  lv_obj_set_pos(guide_title, 28, 20);
  auto command_row = [state, width](int y, const char *number,
                                     lv_obj_t **target) {
    auto *badge = Label(state->fastboot_guide, number,
                        &lv_font_montserrat_28, kAccent);
    lv_obj_set_pos(badge, 28, y + 6);
    *target = Label(state->fastboot_guide, "Preparing command...",
                    &lv_font_montserrat_32, kText);
    lv_obj_set_pos(*target, 82, y);
    lv_obj_set_width(*target, width - 196);
    lv_label_set_long_mode(*target, LV_LABEL_LONG_DOT);
  };
  command_row(78, "1", &state->fastboot_connect);
  command_row(174, "2", &state->fastboot_forward);
  command_row(270, "3", &state->fastboot_client);
  auto *guide_note = Label(state->fastboot_guide,
      "The fastboot port exists only inside this authenticated ADB tunnel.",
      &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(guide_note, 28, 382);
  lv_obj_set_width(guide_note, width - 120);

  auto *security = Label(content,
      "ADB is encrypted. Fastboot is reachable only through an authenticated "
      "ADB tunnel from a paired computer. Wireless access stops with Wi-Fi.",
      &lv_font_montserrat_20, kMuted);
  state->security_note = security;
  lv_obj_set_pos(security, 16, 1674);
  lv_obj_set_width(security, width - 32);


  Navigation(page, Action::kSettings, state->callback, state->context);
  AnimateEnter(content, 12, 16);
  AnimateEnter(back, 28, 12);
  RefreshPairedComputers(state, true);
  RefreshWifiSettings(state);
}

void SetActivity(WifiUi *state, bool active) {
  if (state == nullptr || state->scene.activity == nullptr) return;
  if (active && !state->activity_animating) {
    state->activity_animating = true;
    lv_obj_remove_flag(state->scene.activity, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t spin;
    lv_anim_init(&spin);
    lv_anim_set_var(&spin, state->scene.activity);
    lv_anim_set_values(&spin, 0, 3600);
    lv_anim_set_duration(&spin, 900);
    lv_anim_set_repeat_count(&spin, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&spin, [](void *target, int32_t angle) {
      lv_obj_set_style_transform_rotation(static_cast<lv_obj_t *>(target),
                                          angle, 0);
    });
    lv_anim_start(&spin);
  } else if (!active && state->activity_animating) {
    state->activity_animating = false;
    lv_anim_delete(state->scene.activity, nullptr);
    lv_obj_set_style_transform_rotation(state->scene.activity, 0, 0);
    lv_obj_add_flag(state->scene.activity, LV_OBJ_FLAG_HIDDEN);
  }
}

std::string BusyTitle(const WifiUi *state) {
  const char *base = "Updating Wi-Fi";
  switch (state->running) {
    case WifiOperation::kEnable: base = "Turning Wi-Fi on"; break;
    case WifiOperation::kDisable: base = "Turning Wi-Fi off"; break;
    case WifiOperation::kDisconnect: base = "Disconnecting"; break;
    case WifiOperation::kScan: base = "Scanning nearby"; break;
    case WifiOperation::kConnect: base = "Connecting"; break;
    case WifiOperation::kForget: base = "Forgetting network"; break;
    case WifiOperation::kTest: base = "Testing connection"; break;
    case WifiOperation::kEnableAdb: base = "Starting wireless ADB"; break;
    case WifiOperation::kDisableAdb: base = "Stopping wireless ADB"; break;
    case WifiOperation::kEnableFastbootWifi: base = "Securing fastboot tunnel"; break;
    case WifiOperation::kDisableFastbootWifi: base = "Closing fastboot tunnel"; break;
  }
  std::string title(base);
  title.append(1 + (state->busy_phase % 3), '.');
  return title;
}

void EmptyNetworkCard(WifiUi *state, const char *icon, const char *title,
                      const char *detail) {
  auto *card = lv_obj_create(state->scene.list);
  Panel(card, 34, kMainSheet);
  lv_obj_set_pos(card, 0, 0);
  const int width = state->list_width;
  lv_obj_set_size(card, width, 230);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
  auto *plate = IconPlate(card, icon, kAccent, kAccentSoft, 104);
  lv_obj_set_pos(plate, 34, 63);
  auto *name = Label(card, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 174, 56);
  auto *copy = Label(card, detail, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 174, 118);
  lv_obj_set_width(copy, width - 260);
  AnimateEnter(card, 20, 10);
}

void NetworkCard(WifiUi *state, int y, const WifiNetwork &network) {
  auto *card = lv_button_create(state->scene.list);
  // Connected rows stay on the normal page surface. Connectivity is shown
  // with green details only, without a competing cyan selection wash.
  Panel(card, 32, kMainSheet);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, 0, y);
  const int width = state->list_width;
  lv_obj_set_size(card, width, 174);
  lv_obj_set_style_transform_scale(card, 256, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
  OnClick(card, [state, network] { SelectNetwork(state, network); });

  auto *plate = lv_obj_create(card);
  Panel(plate, 28, kMainPanel);
  lv_obj_set_pos(plate, 28, 41);
  lv_obj_set_size(plate, 92, 92);
  const lv_image_dsc_t *source = network.saved ? &kWifiSavedImage :
      network.security == "OPEN" ? &kWifiOpenImage : &kWifiLockedImage;
  auto *network_icon = lv_image_create(plate);
  lv_image_set_src(network_icon, source);
  lv_obj_set_style_image_recolor(network_icon,
      network.connected ? kGreen : kAccent, 0);
  lv_obj_set_style_image_recolor_opa(network_icon, LV_OPA_COVER, 0);
  lv_obj_center(network_icon);
  auto *name = Label(card, network.ssid.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 152, 34);
  lv_obj_set_width(name, width - 300);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  std::string detail = network.security == "OPEN" ? "Open network" : network.security;
  if (network.saved) detail += "  /  Saved";
  if (network.connected) detail = "Connected  /  " + detail;
  auto *copy = Label(card, detail.c_str(), &lv_font_montserrat_24,
                     network.connected ? kGreen : kMuted);
  lv_obj_set_pos(copy, 152, 98);
  lv_obj_set_width(copy, width - 300);
  lv_label_set_long_mode(copy, LV_LABEL_LONG_DOT);
  const char *trailing = network.connected ? LV_SYMBOL_OK : LV_SYMBOL_RIGHT;
  auto *end = Label(card, trailing, &lv_font_montserrat_32,
                    network.connected ? kGreen : kMutedStrong);
  lv_obj_align(end, LV_ALIGN_RIGHT_MID, -38, 0);
  AnimateEnter(card, 18 + static_cast<uint32_t>(y / 8), 10);
}

void PasswordDialog(WifiUi *state, const WifiNetwork &network) {
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);

  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, 1312, 1260);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_set_style_pad_all(sheet, 48, 0);
  auto *title = Label(sheet, network.ssid.c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_width(title, 1180);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  const std::string password_hint =
      i18n::Format("%s network password", network.security.c_str());
  auto *hint = Label(sheet, password_hint.c_str(),
                     &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 0, 76);

  auto *input = TextArea(sheet);
  lv_obj_set_pos(input, 0, 130);
  lv_obj_set_size(input, 1216, 126);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_password_mode(input, true);
  lv_textarea_set_max_length(input, 63);
  lv_textarea_set_placeholder_text(input, "Password");
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_bg_color(input, kMainPanel, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(input, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(input, 24, 0);
  lv_obj_set_style_pad_all(input, 28, 0);

  auto *keyboard = lv_keyboard_create(sheet);
  phone_keyboard::Apply(keyboard);
  // LVGL keyboards default to BOTTOM_MID; use sheet-local coordinates so the
  // complete keyboard remains between the password field and action buttons.
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard, 0, 286);
  lv_obj_set_size(keyboard, 1216, 650);
  lv_keyboard_set_textarea(keyboard, input);

  auto *cancel = Button(sheet, "Cancel", [overlay] { CloseOverlay(overlay); });
  lv_obj_set_pos(cancel, 0, 996);
  lv_obj_set_size(cancel, 580, 116);
  auto *connect = Button(sheet, "Connect", [state, network, input, overlay] {
    WifiRequest request;
    request.operation = WifiOperation::kConnect;
    request.ssid = network.ssid;
    request.password = lv_textarea_get_text(input);
    CloseOverlay(overlay);
    Dispatch(state, std::move(request));
  }, true);
  lv_obj_set_pos(connect, 636, 996);
  lv_obj_set_size(connect, 580, 116);
  lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
  AnimateEnter(sheet, 0, 42);
}

void SavedDialog(WifiUi *state, const WifiNetwork &network) {
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, 1312, 700);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_set_style_pad_all(sheet, 48, 0);
  auto *title = Label(sheet, network.ssid.c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_width(title, 1180);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  const std::string saved_detail =
      (network.connected ? "Connected / " : "Saved / ") + network.security;
  auto *detail = Label(sheet, saved_detail.c_str(),
      &lv_font_montserrat_32, network.connected ? kGreen : kMutedStrong);
  lv_obj_set_pos(detail, 0, 100);
  auto *connect = Button(sheet, network.connected ? "Disconnect" : "Connect",
      [state, network, overlay] {
        WifiRequest request;
        request.operation = network.connected ? WifiOperation::kDisconnect
                                              : WifiOperation::kConnect;
        request.ssid = network.ssid;
        request.use_saved_credentials = !network.connected;
        CloseOverlay(overlay);
        Dispatch(state, std::move(request));
      }, !network.connected);
  lv_obj_set_pos(connect, 0, 240);
  lv_obj_set_size(connect, 1216, 116);
  if (network.connected) {
    lv_obj_set_style_bg_color(connect, kRedSoft, 0);
    lv_obj_set_style_bg_color(connect, kMainSelected, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(connect, 0), kRed, 0);
  }
  auto *forget = Button(sheet, "Forget saved network", [state, network, overlay] {
    WifiRequest request;
    request.operation = WifiOperation::kForget;
    request.ssid = network.ssid;
    CloseOverlay(overlay);
    Dispatch(state, std::move(request));
  });
  lv_obj_set_pos(forget, 0, 380);
  lv_obj_set_size(forget, 1216, 116);
  auto *cancel = Button(sheet, "Cancel", [overlay] { CloseOverlay(overlay); });
  lv_obj_set_pos(cancel, 0, 520);
  lv_obj_set_size(cancel, 1216, 116);
  AnimateEnter(sheet, 0, 42);
}

void SelectNetwork(WifiUi *state, WifiNetwork network) {
  if (state->busy) return;
  if (network.saved) {
    SavedDialog(state, network);
  } else if (network.security == "OPEN") {
    WifiRequest request;
    request.operation = WifiOperation::kConnect;
    request.ssid = network.ssid;
    Dispatch(state, std::move(request));
  } else {
    PasswordDialog(state, network);
  }
}

void Populate(WifiUi *state) {
  lv_obj_clean(state->scene.list);
  if (!state->snapshot.supported) {
    EmptyNetworkCard(state, LV_SYMBOL_WARNING, "Wi-Fi unavailable",
                     "Wireless support is not available on this device.");
    return;
  }
  if (!state->snapshot.enabled) {
    EmptyNetworkCard(state, LV_SYMBOL_WIFI, "Wi-Fi is off",
                     "Turn it on to discover nearby networks.");
    return;
  }
  if (state->snapshot.networks.empty()) {
    EmptyNetworkCard(state, LV_SYMBOL_REFRESH, "Looking for networks",
                     state->busy ? "Scanning nearby access points."
                                 : "No networks found. Tap Scan again.");
    return;
  }
  int y = 0;
  for (const auto &network : state->snapshot.networks) {
    NetworkCard(state, y, network);
    y += 192;
  }
}

void Refresh(WifiUi *state, bool force) {
  if (!state) return;
  state->snapshot = RecoveryWifiStatus();
  const auto &status = state->snapshot;
  const bool busy = state->busy || status.busy;
  const std::string busy_title = busy ? BusyTitle(state) : std::string();
  const char *title = !status.supported ? "Unavailable" :
      busy ? busy_title.c_str() : status.connected ? "Connected" :
      status.enabled ? "Ready to connect" : "Wi-Fi is off";
  i18n::BindLabel(state->scene.status, title);
  std::string detail;
  if (status.connected) {
    detail = status.ssid;
    if (!status.ip_address.empty()) detail += "  /  " + status.ip_address;
  } else if (busy) {
    if (!status.state.empty()) detail = status.state;
    else if (state->running == WifiOperation::kConnect && !gRequest.ssid.empty())
      detail = "Joining " + gRequest.ssid;
    else detail = "Updating wireless state";
  } else {
    detail = status.enabled ? "Choose a network below" : "Wireless radio is disabled";
  }
  i18n::BindLabel(state->scene.detail, detail.c_str());
  if (status.enabled) lv_obj_add_state(state->radio_switch, LV_STATE_CHECKED);
  else lv_obj_remove_state(state->radio_switch, LV_STATE_CHECKED);
  SetActivity(state, busy);
  // A successful connection should not ring the whole hero card in green;
  // the green Wi-Fi icon and status text already communicate that state.
  lv_obj_set_style_border_opa(state->hero, LV_OPA_TRANSP, 0);
  lv_obj_set_style_text_color(state->status_icon,
      status.connected ? kGreen : kAccent, 0);
  lv_obj_set_style_bg_color(state->status_plate, kMainPanel, 0);
  lv_obj_set_style_border_color(state->status_plate, kMainLine, 0);
  lv_obj_set_style_border_opa(state->status_plate, LV_OPA_TRANSP, 0);
  for (auto *control : {state->scene.toggle, state->scene.refresh, state->scene.test}) {
    if (busy) lv_obj_add_state(control, LV_STATE_DISABLED);
    else lv_obj_remove_state(control, LV_STATE_DISABLED);
  }
  const std::string signature = Signature(status);
  if (force || signature != state->signature) {
    state->signature = signature;
    Populate(state);
  }
}

void Timer(lv_timer_t *timer) {
  auto *state = static_cast<WifiUi *>(lv_timer_get_user_data(timer));
  if (state->busy) ++state->busy_phase;
  Refresh(state, false);
  RefreshWifiSettings(state);
}
}  // namespace

WifiScene BuildWifiScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *state = new WifiUi;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  state->scene.state = state;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    auto *s = static_cast<WifiUi *>(lv_event_get_user_data(event));
    if (s->timer) lv_timer_delete(s->timer);
    // Child delete callbacks clear fields in WifiUi, so close the sheet first.
    if (s->settings_page) lv_obj_delete(s->settings_page);
    delete s;
  }, LV_EVENT_DELETE, state);

  Header(screen, "Wi-Fi", "Networks & connectivity", callback, context);
  const bool landscape = Landscape(screen);
  auto *settings = Button(screen, LV_SYMBOL_SETTINGS "  Wi-Fi settings",
                          [state] { ShowWifiSettings(state); });
  lv_obj_set_size(settings, 360, 104);
  lv_obj_align(settings, LV_ALIGN_TOP_RIGHT, -64, landscape ? 184 : 232);
  lv_obj_set_style_radius(settings, 32, 0);
  auto *panel = lv_obj_create(screen);
  Panel(panel, 36, kMainSheet);
  state->hero = panel;
  lv_obj_set_pos(panel, 64, landscape ? 350 : 420);
  lv_obj_set_size(panel, landscape ? 1450 : 1312,
                  landscape ? 520 : 352);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_border_color(panel, kMainLine, 0);
  lv_obj_set_style_border_opa(panel, LV_OPA_TRANSP, 0);

  auto *status_plate = lv_obj_create(panel);
  state->status_plate = status_plate;
  Panel(status_plate, 34, kMainPanel);
  lv_obj_set_pos(status_plate, 40, 38);
  lv_obj_set_size(status_plate, 124, 124);
  lv_obj_set_style_border_width(status_plate, 1, 0);
  lv_obj_set_style_border_color(status_plate, kMainLine, 0);
  lv_obj_set_style_border_opa(status_plate, LV_OPA_TRANSP, 0);
  state->status_icon = Label(status_plate, LV_SYMBOL_WIFI,
                             &lv_font_montserrat_48, kAccent);
  lv_obj_center(state->status_icon);

  state->scene.status = Label(panel, "Checking...", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(state->scene.status, 198, 34);
  lv_obj_set_width(state->scene.status, 690);
  lv_label_set_long_mode(state->scene.status, LV_LABEL_LONG_DOT);
  state->scene.detail = Label(panel, "Reading wireless state", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->scene.detail, 198, 108);
  lv_obj_set_width(state->scene.detail, 690);
  lv_label_set_long_mode(state->scene.detail, LV_LABEL_LONG_DOT);
  state->scene.toggle = lv_button_create(panel);
  Panel(state->scene.toggle, 38, kMainPanel);
  Interactive(state->scene.toggle, kMainSelected);
  lv_obj_set_pos(state->scene.toggle, 930, 50);
  lv_obj_set_size(state->scene.toggle, 294, 96);
  lv_obj_set_style_border_width(state->scene.toggle, 1, 0);
  lv_obj_set_style_border_color(state->scene.toggle, kMainLine, 0);
  lv_obj_set_style_border_opa(state->scene.toggle, LV_OPA_30, 0);
  auto *radio_label = Label(state->scene.toggle, "Wi-Fi",
                            &lv_font_montserrat_24, kText);
  lv_obj_align(radio_label, LV_ALIGN_LEFT_MID, 28, 0);
  state->radio_switch = lv_switch_create(state->scene.toggle);
  StyleSwitch(state->radio_switch, false);
  lv_obj_align(state->radio_switch, LV_ALIGN_RIGHT_MID, -22, 0);
  OnClick(state->scene.toggle, [state] {
    WifiRequest request;
    request.operation = state->snapshot.enabled ? WifiOperation::kDisable : WifiOperation::kEnable;
    Dispatch(state, std::move(request));
  });

  state->scene.refresh = Button(panel, LV_SYMBOL_REFRESH "  Scan again", [state] {
    WifiRequest request;
    request.operation = WifiOperation::kScan;
    Dispatch(state, std::move(request));
  });
  lv_obj_set_pos(state->scene.refresh, 40, 210);
  lv_obj_set_size(state->scene.refresh, 587, 104);
  lv_obj_set_style_radius(state->scene.refresh, 30, 0);
  state->scene.test = Button(panel, LV_SYMBOL_GPS "  Test connection", [state] {
    WifiRequest request;
    request.operation = WifiOperation::kTest;
    Dispatch(state, std::move(request));
  });
  lv_obj_set_pos(state->scene.test, 651, 210);
  lv_obj_set_size(state->scene.test, 587, 104);
  lv_obj_set_style_radius(state->scene.test, 30, 0);

  auto *caption = Label(screen, "AVAILABLE NETWORKS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(caption, 3, 0);
  lv_obj_set_pos(caption, landscape ? 1580 : 80,
                 landscape ? 306 : 830);
  state->scene.list = Scroll(screen, landscape ? 350 : 884,
                             landscape ? 900 : 1960);
  if (landscape) {
    lv_obj_set_x(state->scene.list, 1560);
    lv_obj_set_width(state->scene.list, 1544);
    state->list_width = 1544;
  } else {
    lv_obj_set_x(state->scene.list, 64);
    lv_obj_set_width(state->scene.list, 1312);
    state->list_width = 1312;
  }
  Navigation(screen, Action::kSettings, callback, context);
  AnimateEnter(panel, 10, 14);
  AnimateEnter(settings, 30, 12);
  Refresh(state, true);
  state->timer = lv_timer_create(Timer, 260, state);
  return state->scene;
}

void SetWifiRequest(const WifiRequest &request) { gRequest = request; }
WifiRequest GetWifiRequest() { return gRequest; }

void SetWifiBusy(const WifiScene &scene, const WifiRequest &request) {
  auto *state = static_cast<WifiUi *>(scene.state);
  if (!state) return;
  state->busy = true;
  state->running = request.operation;
  state->busy_phase = 0;
  state->signature.clear();
  if (request.operation == WifiOperation::kEnableAdb ||
      request.operation == WifiOperation::kDisableAdb) {
    state->adb_busy = true;
    state->adb_desired = request.operation == WifiOperation::kEnableAdb;
    RefreshWifiSettings(state);
  }
  if (request.operation == WifiOperation::kEnableFastbootWifi ||
      request.operation == WifiOperation::kDisableFastbootWifi) {
    state->fastboot_busy = true;
    state->fastboot_desired =
        request.operation == WifiOperation::kEnableFastbootWifi;
    RefreshWifiSettings(state);
  }
  Refresh(state, true);
}

void CompleteWifiOperation(const WifiScene &scene, bool success) {
  auto *state = static_cast<WifiUi *>(scene.state);
  if (!state) return;
  const WifiOperation completed = state->running;
  const bool adb_operation = completed == WifiOperation::kEnableAdb ||
      completed == WifiOperation::kDisableAdb;
  const bool fastboot_operation =
      completed == WifiOperation::kEnableFastbootWifi ||
      completed == WifiOperation::kDisableFastbootWifi;
  state->busy = false;
  if (adb_operation) state->adb_busy = false;
  if (fastboot_operation) state->fastboot_busy = false;
  Refresh(state, true);
  RefreshWifiSettings(state);
  if (completed == WifiOperation::kTest) {
    const auto status = RecoveryWifiStatus();
    Sheet(state->screen, success ? "Network test passed" : "Network test failed",
          status.test_result.empty() ? "No test details were returned." : status.test_result);
  } else if (adb_operation) {
    if (state->settings_message && lv_obj_is_valid(state->settings_message)) {
      i18n::BindLabel(state->settings_message,
          success ? (completed == WifiOperation::kEnableAdb
                         ? "ADB over Wi-Fi enabled for this session"
                         : "ADB over Wi-Fi disabled")
                  : "Could not change ADB over Wi-Fi");
      lv_obj_set_style_text_color(state->settings_message,
                                  success ? kGreen : kRed, 0);
    }
  } else if (fastboot_operation) {
    if (state->settings_message && lv_obj_is_valid(state->settings_message)) {
      i18n::BindLabel(state->settings_message,
          success ? (completed == WifiOperation::kEnableFastbootWifi
                         ? "Fastboot tunnel enabled for paired computers"
                         : "Fastboot over Wi-Fi disabled")
                  : "Could not change fastboot over Wi-Fi");
      lv_obj_set_style_text_color(state->settings_message,
                                  success ? kGreen : kRed, 0);
    }
  } else if (!success) {
    Sheet(state->screen, "Wi-Fi action failed",
          "AERA could not complete the request. Check the password, signal and recovery log, then try again.");
  }
}
}  // namespace aeraui
