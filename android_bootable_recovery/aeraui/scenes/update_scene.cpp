/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "ui_components.hpp"
#include "update/update_manager.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

std::string UpdateSize(uint64_t bytes) {
  char text[64];
  if (bytes >= 1024ULL * 1024 * 1024)
    snprintf(text, sizeof(text), "%.2f GB",
             static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
  else if (bytes >= 1024ULL * 1024)
    snprintf(text, sizeof(text), "%.1f MB",
             static_cast<double>(bytes) / (1024.0 * 1024));
  else
    snprintf(text, sizeof(text), "%.1f KB",
             static_cast<double>(bytes) / 1024.0);
  return text;
}

std::string BuildDate(uint64_t value) {
  if (value == 0) return "Unknown";
  const time_t timestamp = static_cast<time_t>(value);
  struct tm date {};
  char text[64] = {};
  if (gmtime_r(&timestamp, &date) == nullptr ||
      strftime(text, sizeof(text), "%Y-%m-%d %H:%M UTC", &date) == 0)
    return std::to_string(value);
  return text;
}

std::string Notes(const update::Release &release) {
  if (release.changelog.empty()) return "No release notes were provided.";
  std::string text;
  for (const auto &line : release.changelog) {
    if (!text.empty()) text += "\n";
    text += LV_SYMBOL_RIGHT "  " + line;
  }
  return text;
}

void SetDisabled(lv_obj_t *object, bool disabled) {
  if (disabled) lv_obj_add_state(object, LV_STATE_DISABLED);
  else lv_obj_remove_state(object, LV_STATE_DISABLED);
}

void SetChannelSelected(lv_obj_t *button, bool selected) {
  if (button == nullptr) return;
  lv_obj_set_style_bg_color(button, selected ? kAccent : kMainPanel, 0);
  if (lv_obj_get_child_count(button) != 0) {
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                selected ? kOnAccent : kText, 0);
  }
}

}  // namespace

UpdateScene BuildUpdateScene(lv_obj_t *screen, ActionCallback callback,
                             void *context) {
  UpdateScene result;
  result.screen = screen;
  Header(screen, "Recovery Update", "AERA recovery releases", callback, context);
  const bool landscape = Landscape(screen);

  auto *summary = lv_obj_create(screen);
  Panel(summary, 38, kMainSheet);
  lv_obj_set_pos(summary, 64, landscape ? 330 : 460);
  lv_obj_set_size(summary, landscape ? 1450 : 1312,
                  landscape ? 850 : 780);
  lv_obj_set_style_border_width(summary, 1, 0);
  lv_obj_set_style_border_color(summary, kMainLine, 0);
  lv_obj_set_style_border_opa(summary, LV_OPA_30, 0);

  auto *icon_plate = lv_obj_create(summary);
  Panel(icon_plate, 28, kAccentSoft);
  lv_obj_set_pos(icon_plate, 46, 44);
  lv_obj_set_size(icon_plate, 122, 122);
  auto *icon = Label(icon_plate, LV_SYMBOL_DOWNLOAD,
                     &lv_font_montserrat_48, kAccent);
  lv_obj_center(icon);

  result.status = Label(summary, "Ready to check", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(result.status, 204, 42);
  const int channel_width = landscape ? 440 : 400;
  const int channel_x = (landscape ? 1450 : 1312) - 46 - channel_width;
  lv_obj_set_width(result.status, channel_x - 224);
  lv_label_set_long_mode(result.status, LV_LABEL_LONG_DOT);
  result.detail = Label(summary, "", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(result.detail, 204, 132);
  lv_obj_set_width(result.detail, landscape ? 1160 : 1040);
  lv_label_set_long_mode(result.detail, LV_LABEL_LONG_WRAP);

  constexpr int channel_gap = 12;
  const int channel_button_width = (channel_width - channel_gap) / 2;
  result.stable = Button(summary, "Stable", [screen, callback, context] {
    if (update::GetChannel() == update::Channel::kStable) return;
    if (!update::SetChannel(update::Channel::kStable)) {
      Sheet(screen, "Setting unavailable",
            "This setting could not be changed.");
      return;
    }
    callback(Action::kCheckUpdates, context);
  });
  lv_obj_set_pos(result.stable, channel_x, 42);
  lv_obj_set_size(result.stable, channel_button_width, 80);
  lv_obj_set_style_radius(result.stable, 24, 0);
  result.nightly = Button(summary, "Nightly", [screen, callback, context] {
    if (update::GetChannel() == update::Channel::kNightly) return;
    if (!update::SetChannel(update::Channel::kNightly)) {
      Sheet(screen, "Setting unavailable",
            "This setting could not be changed.");
      return;
    }
    callback(Action::kCheckUpdates, context);
  });
  lv_obj_set_pos(result.nightly, channel_x + channel_button_width + channel_gap,
                 42);
  lv_obj_set_size(result.nightly, channel_button_width, 80);
  lv_obj_set_style_radius(result.nightly, 24, 0);

  auto *line = lv_obj_create(summary);
  Clear(line);
  lv_obj_set_pos(line, 46, 206);
  lv_obj_set_size(line, landscape ? 1358 : 1220, 1);
  lv_obj_set_style_bg_color(line, kMainLine, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);

  auto *installed_title =
      Label(summary, "INSTALLED BUILD", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(installed_title, 46, 250);
  result.installed = Label(summary, "", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(result.installed, 46, 306);
  lv_obj_set_width(result.installed, landscape ? 1358 : 1220);

  auto *release_title =
      Label(summary, "AVAILABLE RELEASE", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(release_title, 46, 388);
  result.release = Label(summary, "", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(result.release, 46, 444);
  lv_obj_set_width(result.release, landscape ? 1358 : 1220);
  lv_label_set_long_mode(result.release, LV_LABEL_LONG_WRAP);

  result.progress = lv_bar_create(summary);
  lv_obj_set_pos(result.progress, 46, 548);
  lv_obj_set_size(result.progress, landscape ? 1358 : 1220, 30);
  lv_bar_set_range(result.progress, 0, 100);
  lv_obj_set_style_radius(result.progress, 12, LV_PART_MAIN);
  lv_obj_set_style_radius(result.progress, 12, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(result.progress, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_bg_color(result.progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(result.progress, LV_OBJ_FLAG_HIDDEN);
  result.progress_value =
      Label(summary, "0%", &lv_font_montserrat_28, kAccent);
  lv_obj_set_pos(result.progress_value, 46, 590);
  result.progress_amount =
      Label(summary, "", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(result.progress_amount, landscape ? 510 : 350, 594);
  lv_obj_set_width(result.progress_amount, landscape ? 894 : 916);
  lv_obj_set_style_text_align(result.progress_amount, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_add_flag(result.progress_value, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(result.progress_amount, LV_OBJ_FLAG_HIDDEN);

  result.check = Button(summary, LV_SYMBOL_REFRESH "  Check for updates",
                        [callback, context] {
                          callback(Action::kCheckUpdates, context);
                        });
  const int actions_width = landscape ? 1358 : 1220;
  const int action_gap = 24;
  const int action_width = (actions_width - action_gap) / 2;
  lv_obj_set_pos(result.check, 46, landscape ? 650 : 640);
  lv_obj_set_size(result.check, action_width, 112);

  result.install = Button(summary, LV_SYMBOL_DOWNLOAD "  Download and install",
      [screen, callback, context] {
        const auto snapshot = update::GetSnapshot();
        if (!snapshot.available) return;
        std::string copy = i18n::Format(
            "%s  /  %s\n%s\n\nThe package's included installer will "
            "control the target partitions and slots.",
            snapshot.release.version.c_str(),
            UpdateSize(snapshot.release.size).c_str(),
            BuildDate(snapshot.release.build_time).c_str());
        Sheet(screen, "Install this recovery update?", copy,
              [callback, context] {
                callback(Action::kDownloadUpdate, context);
              });
      }, true);
  lv_obj_set_pos(result.install, 46 + action_width + action_gap,
                 landscape ? 650 : 640);
  lv_obj_set_size(result.install, action_width, 112);

  const int notes_y = landscape ? 330 : 1290;
  const int notes_min_height = landscape ? 850 : 920;
  const int notes_height = std::max(
      notes_min_height,
      static_cast<int>(lv_obj_get_height(screen)) - notes_y - 80);
  auto *notes = lv_obj_create(screen);
  Panel(notes, 38, kMainSheet);
  lv_obj_set_pos(notes, landscape ? 1560 : 64, notes_y);
  lv_obj_set_size(notes, landscape ? 1544 : 1312,
                  notes_height);
  lv_obj_set_style_border_width(notes, 1, 0);
  lv_obj_set_style_border_color(notes, kMainLine, 0);
  lv_obj_set_style_border_opa(notes, LV_OPA_30, 0);
  auto *notes_title =
      Label(notes, "What's new", &lv_font_montserrat_40, kText);
  lv_obj_set_pos(notes_title, 46, 42);
  auto *notes_hint =
      Label(notes, "Release notes", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(notes_hint, 46, 108);

  const int notes_body_width = landscape ? 1452 : 1220;
  auto *notes_body = lv_obj_create(notes);
  Clear(notes_body);
  lv_obj_set_pos(notes_body, 46, 174);
  lv_obj_set_size(notes_body, notes_body_width, notes_height - 214);
  lv_obj_add_flag(notes_body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(notes_body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_set_scroll_dir(notes_body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(notes_body, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(notes_body, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(notes_body, 6, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(notes_body, 3, LV_PART_SCROLLBAR);

  result.changelog =
      Label(notes_body, "", &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_pos(result.changelog, 0, 10);
  lv_obj_set_width(result.changelog, notes_body_width - 22);
  lv_obj_set_style_text_line_space(result.changelog, 18, 0);
  lv_label_set_long_mode(result.changelog, LV_LABEL_LONG_WRAP);

  AnimateEnter(summary, 30, 24);
  AnimateEnter(notes, 65, 24);
  RefreshUpdateScene(result);
  return result;
}

void RefreshUpdateScene(const UpdateScene &scene) {
  if (scene.screen == nullptr) return;
  const auto snapshot = update::GetSnapshot();
  std::string status;
  switch (snapshot.phase) {
    case update::Phase::kChecking:
      status = "Checking for updates";
      break;
    case update::Phase::kAvailable:
      status = i18n::Format("%s is available",
                            snapshot.release.version.c_str());
      break;
    case update::Phase::kUpToDate:
      status = "AERA is up to date";
      break;
    case update::Phase::kDownloading:
      status = "Downloading update";
      break;
    case update::Phase::kVerifying:
      status = "Verifying package";
      break;
    case update::Phase::kReady:
      status = "Update package ready";
      break;
    case update::Phase::kError:
      status = "Update check failed";
      break;
    case update::Phase::kIdle:
    default:
      status = "Ready to check";
      break;
  }
  i18n::BindLabel(scene.status, status.c_str());
  i18n::BindLabel(scene.detail, snapshot.message.empty()
      ? "Connect to Wi-Fi to check for a newer build."
      : snapshot.message.c_str());

  const std::string installed = snapshot.device.empty()
      ? "Current recovery"
      : snapshot.device + "  /  " + BuildDate(snapshot.local_build_time);
  i18n::BindLabel(scene.installed, installed.c_str());

  std::string release = "No newer build found";
  if (snapshot.release.build_time != 0) {
    release = snapshot.release.version;
    if (!snapshot.release.build_type.empty())
      release += "  /  " + snapshot.release.build_type;
    release += "  /  " + BuildDate(snapshot.release.build_time);
    release += "\n" + snapshot.release.filename + "  /  " +
        UpdateSize(snapshot.release.size);
  }
  i18n::BindLabel(scene.release, release.c_str());
  i18n::BindLabel(scene.changelog, Notes(snapshot.release).c_str());

  const bool busy = snapshot.phase == update::Phase::kChecking ||
                    snapshot.phase == update::Phase::kDownloading ||
                    snapshot.phase == update::Phase::kVerifying;
  const bool nightly = update::GetChannel() == update::Channel::kNightly;
  SetChannelSelected(scene.stable, !nightly);
  SetChannelSelected(scene.nightly, nightly);
  SetDisabled(scene.stable, busy);
  SetDisabled(scene.nightly, busy);
  const int actions_width = Landscape(scene.screen) ? 1358 : 1220;
  constexpr int action_gap = 24;
  if (snapshot.available) {
    const int action_width = (actions_width - action_gap) / 2;
    lv_obj_set_width(scene.check, action_width);
    lv_obj_set_x(scene.install, 46 + action_width + action_gap);
    lv_obj_set_width(scene.install, action_width);
    lv_obj_remove_flag(scene.install, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_set_width(scene.check, actions_width);
    lv_obj_add_flag(scene.install, LV_OBJ_FLAG_HIDDEN);
  }
  SetDisabled(scene.check, busy);
  SetDisabled(scene.install, busy || !snapshot.available);
  const bool downloading = snapshot.phase == update::Phase::kDownloading;
  const bool verifying = snapshot.phase == update::Phase::kVerifying;
  if (downloading || verifying) {
    unsigned stage_progress = 0;
    std::string value;
    std::string amount;
    if (downloading) {
      if (snapshot.total != 0) {
        stage_progress = static_cast<unsigned>(
            std::min(snapshot.downloaded, snapshot.total) * 100 /
            snapshot.total);
      }
      value = std::to_string(stage_progress) + "%";
      amount = UpdateSize(snapshot.downloaded) + " / " +
          UpdateSize(snapshot.total);
    } else {
      stage_progress = snapshot.progress <= 90
          ? 0 : std::min((snapshot.progress - 90) * 10, 100U);
      value = "Verifying";
      amount = "SHA-256 integrity check";
    }
    lv_bar_set_value(scene.progress, static_cast<int32_t>(stage_progress),
                     LV_ANIM_ON);
    i18n::BindLabel(scene.progress_value, value.c_str());
    i18n::BindLabel(scene.progress_amount, amount.c_str());
    lv_obj_remove_flag(scene.progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(scene.progress_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(scene.progress_amount, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(scene.progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scene.progress_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scene.progress_amount, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace aeraui
