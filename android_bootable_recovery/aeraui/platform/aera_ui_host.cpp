// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#include "aeraui/platform/aera_ui_host.h"
#include "aeraui/platform/aera_ui_host.hpp"

#include <android-base/properties.h>
#include <aeraui/backend.hpp>
#include <aeraui/runner.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "aera_rpc/aera_channel.hpp"
#include "aeraui/platform/aera_screen_timer.hpp"
#include "data.hpp"
#include "minuitwrp/minui.h"
#include "twrp-functions.hpp"
#include "variables.h"

namespace {

bool gInitialized = false;
float gScaleWidth = 1.0f;
float gScaleHeight = 1.0f;

#ifndef AERA_STATUS_INDENT_LEFT
#define AERA_STATUS_INDENT_LEFT 54
#endif
#ifndef AERA_STATUS_INDENT_RIGHT
#define AERA_STATUS_INDENT_RIGHT 54
#endif
#ifdef AERA_UI_ADAPTIVE_RESOLUTION
#ifndef AERA_SCREEN_H
#define AERA_SCREEN_H 2376
#endif
#ifndef AERA_STATUS_H
#define AERA_STATUS_H 124
#endif
constexpr auto kDisplayMetrics = aeraui::DisplayMetrics::FromThemeMetrics(
    true, AERA_SCREEN_H, AERA_STATUS_H, AERA_STATUS_INDENT_LEFT,
    AERA_STATUS_INDENT_RIGHT);
#else
constexpr aeraui::DisplayMetrics kDisplayMetrics{
    false, 3168, 165, AERA_STATUS_INDENT_LEFT, AERA_STATUS_INDENT_RIGHT};
#endif

}  // namespace

extern "C" int aeraui_is_active() { return 1; }

extern "C" int gui_init() {
  gr_init();
  TWFunc::Set_Brightness(DataManager::GetStrValue("tw_brightness"));
#ifdef TW_SCREEN_BLANK_ON_BOOT
  aeraScreenTimer.Blank();
  aeraScreenTimer.Wake();
#endif
#ifdef TW_DELAY_TOUCH_INIT_MS
  usleep(TW_DELAY_TOUCH_INIT_MS);
#endif
  ev_init();

  const bool fastboot =
      android::base::GetProperty(TW_FASTBOOT_MODE_PROP, "0") == "1";
  const bool soft_switch =
      android::base::GetProperty(AERA_SOFT_SWITCH_PROP, "0") == "1";
  if (!fastboot && !soft_switch) {
    aeraui::StartAeraUiEarly(kDisplayMetrics);
  } else if (soft_switch && !fastboot) {
    gui_print("I:AERA userspace handoff: restoring the existing UI session.\n");
  } else {
    gui_print("I:Fastbootd startup: opening the dedicated AERA surface.\n");
  }
  return 0;
}

extern "C" int gui_loadResources() {
  gInitialized = true;
  return 0;
}

extern "C" int gui_loadCustomResources() { return 0; }

extern "C" int gui_start() { return gui_startPage("main", 1, 0); }

extern "C" int gui_startPage(const char* page_name, int allow_commands,
                              int stop_on_page_done) {
  (void)stop_on_page_done;
  if (!gInitialized) return -1;

#ifndef TW_OEM_BUILD
  if (allow_commands)
    aera::rpc::Channel::Setup();
  else
    aera::rpc::Channel::Shutdown();
#endif

  const bool fastboot =
      (page_name != nullptr && std::strcmp(page_name, "fastboot") == 0) ||
      android::base::GetProperty(TW_FASTBOOT_MODE_PROP, "0") == "1";
  const bool resume = !fastboot &&
      android::base::GetProperty(AERA_SOFT_SWITCH_PROP, "0") == "1";
  if (!fastboot) aeraui::RecoveryWifiInitialize();
  if (resume) android::base::SetProperty(AERA_SOFT_SWITCH_PROP, "0");

  const aeraui::RunResult result = fastboot
      ? aeraui::RunAeraUiFastboot(kDisplayMetrics)
      : resume ? aeraui::RunAeraUiResume(kDisplayMetrics)
               : aeraui::RunAeraUi(kDisplayMetrics);
  gui_print("I:AERA UI exited with result %d.\n", static_cast<int>(result));
  switch (result) {
    case aeraui::RunResult::kRebootSystem:
      DataManager::SetValue("tw_reboot_arg", "system");
      return 0;
    case aeraui::RunResult::kRebootRecovery:
      DataManager::SetValue("tw_reboot_arg", "recovery");
      return 0;
    case aeraui::RunResult::kRebootBootloader:
      DataManager::SetValue("tw_reboot_arg", "bootloader");
      return 0;
    case aeraui::RunResult::kRebootFastbootd:
      DataManager::SetValue("tw_reboot_arg", "fastboot");
      return 0;
    case aeraui::RunResult::kPowerOff:
      DataManager::SetValue("tw_reboot_arg", "poweroff");
      return 0;
    default:
      gui_print_color("error", "E:AERA UI stopped unexpectedly; no fallback UI is enabled.\n");
      return -1;
  }
}

extern "C" void set_scale_values(float width, float height) {
  gScaleWidth = width;
  gScaleHeight = height;
}
extern "C" int scale_theme_x(int value) {
  return static_cast<int>(value * gScaleWidth);
}
extern "C" int scale_theme_y(int value) {
  return static_cast<int>(value * gScaleHeight);
}
extern "C" int scale_theme_min(int value) {
  return static_cast<int>(value * std::min(gScaleWidth, gScaleHeight));
}
extern "C" float get_scale_w() { return gScaleWidth; }
extern "C" float get_scale_h() { return gScaleHeight; }

void set_select_fd() {}
int gui_forceRender() { return 0; }
int gui_changePage(std::string page) {
  gui_print("I:Ignoring legacy page request '%s'; AERA UI owns navigation.\n",
            page.c_str());
  return 0;
}
int gui_changeOverlay(std::string overlay) {
  gui_print("I:Ignoring legacy overlay request '%s'.\n", overlay.c_str());
  return 0;
}
void gui_switchControlMode() {
  const bool enabled = DataManager::GetIntValue("of_hw_control_mode") == 1;
  DataManager::SetValue("of_hw_control_mode", enabled ? 0 : 1);
  DataManager::Vibrate("tw_button_vibrate");
}
void gui_notifyVarChange(const char* name, const char* value) {
  (void)name;
  (void)value;
}

std::string gui_lookup(const std::string& resource_name,
                       const std::string& default_value) {
  return default_value.empty() ? resource_name : default_value;
}

std::string gui_parse_text(std::string text) {
  size_t position = 0;
  while ((position = text.find("{@", position)) != std::string::npos) {
    const size_t end = text.find('}', position + 2);
    if (end == std::string::npos) break;
    std::string tag = text.substr(position + 2, end - position - 2);
    const size_t separator = tag.find('=');
    const std::string key = tag.substr(0, separator);
    const std::string fallback = separator == std::string::npos
        ? (key == "mbyte" ? " MB" : key)
        : tag.substr(separator + 1);
    text.replace(position, end - position + 1, gui_lookup(key, fallback));
  }
  position = 0;
  while ((position = text.find('%', position)) != std::string::npos) {
    const size_t end = text.find('%', position + 1);
    if (end == std::string::npos) break;
    const std::string key = text.substr(position + 1, end - position - 1);
    std::string replacement;
    if (key.empty()) {
      replacement = "%";
    } else if (key.front() == '@') {
      replacement = gui_lookup(key.substr(1), key.substr(1));
    } else {
      DataManager::GetValue(key, replacement);
    }
    text.replace(position, end - position + 1, replacement);
    position += replacement.size();
  }
  return text;
}

char* aera_read_file_buffer(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return nullptr;
  const std::string data((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  char* buffer = static_cast<char*>(std::malloc(data.size() + 1));
  if (buffer == nullptr) return nullptr;
  std::memcpy(buffer, data.data(), data.size());
  buffer[data.size()] = '\0';
  return buffer;
}
