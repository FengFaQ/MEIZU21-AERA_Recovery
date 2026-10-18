/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"
#include "../../aera_remote/pc_connection.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "ui_components.hpp"

namespace aeraui {
namespace {
using namespace widgets;

JobRequest gRequest;

struct FriendlyProgress {
  std::string title;
  std::string explanation;
  std::string amount;
  std::string files;
  std::string activity;
  int step = 0;
};

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::vector<std::string> Lines(const std::string &text) {
  std::vector<std::string> result;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t end = text.find('\n', begin);
    auto line = text.substr(begin, end == std::string::npos ? end : end - begin);
    if (!line.empty()) result.push_back(std::move(line));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
}

std::string CleanMetric(std::string value) {
  if (!value.empty() && value.back() == ',') value.pop_back();
  for (const char *unit : {"MB", "GB", "KB"}) {
    size_t pos = 0;
    while ((pos = value.find(unit, pos)) != std::string::npos) {
      if (pos > 0 && std::isdigit(static_cast<unsigned char>(value[pos - 1]))) {
        value.insert(pos, " ");
        ++pos;
      }
      pos += 2;
    }
  }
  return value;
}

std::string DataSize(uint64_t bytes) {
  char text[64];
  if (bytes >= 1024ULL * 1024 * 1024) {
    snprintf(text, sizeof(text), "%.2f GB",
             static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
  } else if (bytes >= 1024ULL * 1024) {
    snprintf(text, sizeof(text), "%.1f MB",
             static_cast<double>(bytes) / (1024.0 * 1024));
  } else if (bytes >= 1024ULL) {
    snprintf(text, sizeof(text), "%.1f KB",
             static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(text, sizeof(text), "%llu bytes",
             static_cast<unsigned long long>(bytes));
  }
  return text;
}

std::string CleanInstallerStatus(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  if (!value.empty() && value.front() == '-') {
    value.erase(value.begin());
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())))
      value.erase(value.begin());
  }
  return value;
}

std::string CleanInstallerHistory(const std::string &value,
                                  size_t visible_lines) {
  const auto lines = Lines(value);
  std::string history;
  const size_t first = lines.size() > visible_lines
      ? lines.size() - visible_lines : 0;
  for (size_t i = first; i < lines.size(); ++i) {
    auto line = lines[i];
    line = CleanInstallerStatus(std::move(line));
    if (line.empty()) continue;
    if (!history.empty()) history += '\n';
    history += line;
  }
  return history;
}

const char *InitialTitle(Job job) {
  switch (job) {
    case Job::kSideload: return "Waiting for package";
    case Job::kFlashImage: return "Preparing image flash";
    case Job::kExtractPayload: return "Extracting selected images";
    case Job::kFlashPayload: return "Preparing selected partitions";
    case Job::kBackup: return "Preparing your backup";
    case Job::kPcBackupOperation: return "Preparing your backup";
    case Job::kUploadBackup: return "Preparing network upload";
    case Job::kRestore: return "Preparing to restore";
    case Job::kWipe: return "Preparing to wipe";
    case Job::kFormatData: return "Preparing data format";
    case Job::kFormatUsb: return "Preparing USB format";
    case Job::kClearSnapshotCow: return "Preparing snapshot cleanup";
    case Job::kMount: return "Mounting storage";
    case Job::kUnmount: return "Unmounting storage";
    case Job::kRootOperation: return "Root Manager";
    case Job::kNetworkStorage: return "Network storage";
    default: return "Preparing installation";
  }
}

const char *OperationSymbol(Job job) {
  switch (job) {
    case Job::kSideload: return LV_SYMBOL_USB;
    case Job::kFlashImage: return LV_SYMBOL_UPLOAD;
    case Job::kFlashPayload: return LV_SYMBOL_UPLOAD;
    case Job::kExtractPayload: return LV_SYMBOL_SAVE;
    case Job::kBackup: return LV_SYMBOL_SAVE;
    case Job::kPcBackupOperation: return LV_SYMBOL_SAVE;
    case Job::kUploadBackup: return LV_SYMBOL_UPLOAD;
    case Job::kRestore: return LV_SYMBOL_REFRESH;
    case Job::kWipe:
    case Job::kFormatData:
    case Job::kFormatUsb:
    case Job::kClearSnapshotCow: return LV_SYMBOL_TRASH;
    case Job::kMount:
    case Job::kUnmount: return LV_SYMBOL_DRIVE;
    case Job::kRootOperation: return LV_SYMBOL_SETTINGS;
    case Job::kNetworkStorage: return LV_SYMBOL_DRIVE;
    default: return LV_SYMBOL_DOWNLOAD;
  }
}

void SetActivityBorderOpacity(void *target, int32_t opacity) {
  lv_obj_set_style_border_opa(static_cast<lv_obj_t *>(target), opacity, 0);
}

void SetProgressPulseOpacity(void *target, int32_t opacity) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(target),
                       static_cast<lv_opa_t>(opacity), 0);
}

void SetIndeterminateProgressX(void *target, int32_t x) {
  lv_obj_set_x(static_cast<lv_obj_t *>(target), x);
}
void StartIndeterminateProgress(lv_obj_t *target) {
  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, target);
  lv_anim_set_values(&pulse, 48, 1024);
  lv_anim_set_duration(&pulse, 1100);
  lv_anim_set_playback_duration(&pulse, 1100);
  lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&pulse, SetIndeterminateProgressX);
  lv_anim_start(&pulse);
}

constexpr const char *kRomInstallerStepTitles[4] = {
    "Boot partitions", "Device firmware", "Super image", "Finishing up"};

constexpr const char *kRomInstallerStepDetails[4] = {
    "Prepare the boot chain on both slots",
    "Update the device firmware safely",
    "Stream and write the super partition",
    "Verify the installation and clean up"};

constexpr const char *kRecoveryUpdaterStepTitles[4] = {
    "Verify update", "Recovery image", "Support files", "Finish update"};

constexpr const char *kRecoveryUpdaterStepDetails[4] = {
    "Check device compatibility and package integrity",
    "Install AERA Recovery to the required slots",
    "Update AERA tools and files on device storage",
    "Apply settings and prepare the updated recovery"};

bool IsRecoveryUpdater(const InstallerPresentation &presentation) {
  return presentation.package_name.rfind("AERA Recovery", 0) == 0;
}

void SetInstallerTimelineVisible(const OperationScene &scene, bool visible) {
  for (auto *step : scene.steps) {
    if (!step) continue;
    if (visible)
      lv_obj_remove_flag(step, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(step, LV_OBJ_FLAG_HIDDEN);
  }
  if (scene.activity_summary) {
    if (visible)
      lv_obj_add_flag(scene.activity_summary, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_remove_flag(scene.activity_summary, LV_OBJ_FLAG_HIDDEN);
  }
}

void RefreshInstallerTimeline(const OperationScene &scene, int current_stage,
                              const std::string &current_detail,
                              bool recovery_updater = false,
                              bool finished = false, bool failed = false) {
  const auto &titles = recovery_updater ? kRecoveryUpdaterStepTitles
                                        : kRomInstallerStepTitles;
  const auto &details = recovery_updater ? kRecoveryUpdaterStepDetails
                                         : kRomInstallerStepDetails;
  for (int i = 0; i < 4; ++i) {
    if (!scene.steps[i]) continue;
    const int number = i + 1;
    const bool complete = finished || current_stage > number;
    const bool current = !finished && current_stage == number;
    const auto color = failed && current ? design::kRed
        : complete ? design::kGreen
        : current ? kAccent : kMuted;

    // Keep every stage on the same calm surface. Status belongs to the icon
    // and label; tinting an entire completed row makes the timeline noisy.
    lv_obj_set_style_bg_color(scene.steps[i], kMainPanel, 0);
    lv_obj_set_style_bg_opa(scene.steps[i], LV_OPA_70, 0);
    lv_obj_set_style_border_color(scene.steps[i],
                                  current ? color : kMainLine, 0);
    lv_obj_set_style_border_opa(scene.steps[i],
        current ? LV_OPA_30 : LV_OPA_10, 0);

    if (scene.step_icons[i]) {
      i18n::BindLabel(scene.step_icons[i], complete ? LV_SYMBOL_OK
          : current ? LV_SYMBOL_PLAY : i18n::Format("%02d", number).c_str());
      lv_obj_set_style_text_color(scene.step_icons[i], color, 0);
    }
    if (scene.step_titles[i]) {
      i18n::BindLabel(scene.step_titles[i], titles[i]);
      lv_obj_set_style_text_color(scene.step_titles[i],
                                  complete || current ? kText : kMutedStrong, 0);
    }
    if (scene.step_details[i]) {
      std::string detail = details[i];
      if (current && !current_detail.empty()) detail = current_detail;
      if (complete) detail = "Completed safely";
      if (failed && current) detail = "Stopped before this stage could finish";
      i18n::BindLabel(scene.step_details[i], detail.c_str());
      lv_obj_set_style_text_color(scene.step_details[i],
                                  complete || current ? kMutedStrong : kMuted, 0);
    }
    if (scene.step_states[i]) {
      i18n::BindLabel(scene.step_states[i], complete ? "DONE"
          : failed && current ? "STOPPED" : current ? "IN PROGRESS" : "UP NEXT");
      lv_obj_set_style_text_color(scene.step_states[i], color, 0);
    }
  }
}

FriendlyProgress Explain(const std::string &raw, Job job, int progress) {
  FriendlyProgress value;
  const auto lines = Lines(raw);
  const std::string headline = lines.empty() ? "" : lines[0];
  const std::string all = Lower(raw);
  std::string subject;
  const size_t slash = headline.find(" / ");
  if (slash != std::string::npos) subject = headline.substr(slash + 3);
  if (subject.empty()) subject = "selected partitions";
  if (lines.size() > 1) value.amount = CleanMetric(lines[1]);
  if (lines.size() > 2) value.files = CleanMetric(lines[2]);

  if (job == Job::kUploadBackup ||
      all.find("nas upload") != std::string::npos ||
      all.find("uploading") != std::string::npos) {
    value.title = "Uploading to Network Storage";
    value.explanation = "Your local backup is ready. AERA is securely transferring it to your network storage.";
    value.activity = "Keep Wi-Fi connected and leave the device powered on until the upload completes.";
    value.step = 3;
  } else if (all.find("digest") != std::string::npos ||
             all.find("checksum") != std::string::npos ||
             all.find("verify") != std::string::npos) {
    value.title = "Verifying the backup";
    value.explanation = "AERA is checking the backup so it can be restored reliably later.";
    value.activity = "Reading the completed archive and validating its integrity.";
    value.step = 2;
  } else if (job == Job::kBackup || (job != Job::kRestore && all.find("backing") != std::string::npos)) {
    value.title = i18n::Format("Backing up %s", subject.c_str());
    value.explanation = "AERA is safely copying the selected data into your new backup.";
    value.activity = i18n::Format(
        "Copying files from %s. Large partitions can take a few minutes.",
        subject.c_str());
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kRestore || all.find("restor") != std::string::npos) {
    value.title = i18n::Format("Restoring %s", subject.c_str());
    value.explanation = "AERA is writing the selected backup data back to the device.";
    value.activity = i18n::Format(
        "Restoring files for %s. Do not reboot during this operation.",
        subject.c_str());
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kFormatData || all.find("format") != std::string::npos) {
    value.title = "Formatting data";
    value.explanation = "AERA is recreating the data volume and internal storage.";
    value.activity = "Removing encryption metadata and preparing a clean data volume.";
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kClearSnapshotCow) {
    value.title = "Removing snapshot COW partitions";
    value.explanation = "AERA is releasing stale Android snapshot storage from Super.";
    value.activity = "Only verified partitions in the COW metadata group are removed.";
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kWipe || all.find("wip") != std::string::npos) {
    value.title = i18n::Format("Wiping %s", subject.c_str());
    value.explanation = "AERA is clearing the selected partition safely.";
    value.activity = i18n::Format(
        "Cleaning %s and preparing it for use.", subject.c_str());
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kExtractPayload || job == Job::kFlashPayload) {
    value.title = job == Job::kFlashPayload ? "Preparing and flashing selected images" : "Extracting selected images";
    value.explanation = "Selected images are reconstructed and verified against the payload hashes.";
    value.activity = job == Job::kFlashPayload ? "Only selected partitions will be written. The active slot will not change."
                                             : "Images are saved under AERA/Extracted on the selected storage.";
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kFlashImage) {
    value.title = progress > 1 ? "Flashing partition image" :
                                 "Preparing image flash";
    value.explanation = i18n::Format(
        "AERA is writing the selected image directly to %s.",
        subject.c_str());
    value.activity = "Do not reboot or disconnect the device while the image is being written.";
    value.step = progress > 1 ? 1 : 0;
  } else if (job == Job::kSideload) {
    value.title = "Waiting for package";
    value.explanation =
        "Run adb sideload package.zip on the connected computer.";
    value.activity =
        "Normal ADB is paused while the dedicated sideload service is active.";
    value.step = 0;
  } else if (job == Job::kInstall) {
    value.title = progress > 1 ? "Installing package" : "Preparing installation";
    value.explanation = "AERA is applying the selected package to your device.";
    value.activity = "Processing the package and updating the required partitions.";
    value.step = progress > 1 ? 1 : 0;
  } else {
    value.title = InitialTitle(job);
    value.explanation = "AERA is completing the requested recovery operation.";
    value.activity = headline.empty() ? "Waiting for the recovery backend to begin." : headline;
    value.step = progress > 1 ? 1 : 0;
  }
  return value;
}

void ShowTechnicalLog(lv_obj_t *screen) {
  auto *overlay = lv_obj_create(screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);

  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 44, kMainSheet);
  const bool landscape = Landscape(screen);
  lv_obj_set_size(sheet, landscape ? 2200 : 1312,
                  landscape ? 1250 : 2550);
  lv_obj_align(sheet, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_30, 0);

  auto *title = Label(sheet, "Technical details", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 48, 48);
  auto *copy = Label(sheet, "Raw recovery output for troubleshooting", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 48, 118);

  auto *area = lv_obj_create(sheet);
  Clear(area);
  lv_obj_set_pos(area, 48, 190);
  lv_obj_set_size(area, landscape ? 2104 : 1216,
                  landscape ? 820 : 2110);
  lv_obj_add_flag(area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(area, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(area, kInset, 0);
  lv_obj_set_style_bg_opa(area, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(area, 24, 0);
  lv_obj_set_style_pad_all(area, 32, 0);
  lv_obj_set_style_bg_color(area, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(area, 5, LV_PART_SCROLLBAR);
  auto *log = Label(area, ReadLog().c_str(), &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_width(log, landscape ? 2010 : 1120);
  lv_obj_set_style_text_line_space(log, 8, 0);

  auto *close = Button(sheet, "Back to operation",
                       [overlay] { lv_obj_delete_async(overlay); }, true);
  lv_obj_set_pos(close, 48, landscape ? 1060 : 2342);
  lv_obj_set_size(close, landscape ? 2104 : 1216, 136);
  AnimateEnter(sheet, 0, 36);
}

void BuildInstallerPrompt(OperationScene *scene, lv_obj_t *screen,
                          bool landscape) {
  if (scene == nullptr) return;
  scene->installer_prompt = lv_obj_create(screen);
  lv_obj_set_user_data(scene->installer_prompt, &kModalMarker);
  Clear(scene->installer_prompt);
  lv_obj_set_size(scene->installer_prompt, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(scene->installer_prompt, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scene->installer_prompt, LV_OPA_70, 0);
  lv_obj_add_flag(scene->installer_prompt, LV_OBJ_FLAG_HIDDEN);

  auto *sheet = lv_obj_create(scene->installer_prompt);
  Panel(sheet, 44, kMainSheet);
  lv_obj_set_size(sheet, landscape ? 1800 : 1248,
                  landscape ? 850 : 1040);
  lv_obj_align(sheet, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_border_width(sheet, 2, 0);
  lv_obj_set_style_border_color(sheet, kAccent, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_30, 0);
  lv_obj_set_style_shadow_color(sheet, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(sheet, 70, 0);
  lv_obj_set_style_shadow_opa(sheet, LV_OPA_50, 0);

  auto *badge = lv_obj_create(sheet);
  Panel(badge, LV_RADIUS_CIRCLE, kAccentSoft);
  lv_obj_set_pos(badge, 48, 48);
  lv_obj_set_size(badge, 116, 116);
  lv_obj_set_style_border_width(badge, 2, 0);
  lv_obj_set_style_border_color(badge, kAccent, 0);
  lv_obj_set_style_border_opa(badge, LV_OPA_40, 0);
  auto *icon = Label(badge, LV_SYMBOL_UPLOAD, &lv_font_montserrat_48, kAccent);
  lv_obj_center(icon);

  auto *eyebrow = Label(sheet, "AERA INTERACTIVE INSTALLER",
                        &lv_font_montserrat_20, kAccent);
  lv_obj_set_pos(eyebrow, 198, 50);
  lv_obj_set_style_text_letter_space(eyebrow, 3, 0);
  scene->installer_prompt_title =
      Label(sheet, "Ready to install?", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(scene->installer_prompt_title, 198, 88);
  lv_obj_set_width(scene->installer_prompt_title,
                   landscape ? 1510 : 980);
  lv_label_set_long_mode(scene->installer_prompt_title, LV_LABEL_LONG_DOT);

  auto *line = lv_obj_create(sheet);
  Clear(line);
  lv_obj_set_pos(line, 48, 202);
  lv_obj_set_size(line, landscape ? 1704 : 1152, 2);
  lv_obj_set_style_bg_color(line, kMainLine, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_30, 0);

  scene->installer_prompt_message = Label(
      sheet,
      "Review the package information before allowing it to write to your device.",
      &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_pos(scene->installer_prompt_message, 48, 260);
  lv_obj_set_size(scene->installer_prompt_message,
                  landscape ? 1704 : 1152,
                  landscape ? 270 : 430);
  lv_label_set_long_mode(scene->installer_prompt_message,
                         LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(scene->installer_prompt_message, 12, 0);

  auto *warning = Label(
      sheet,
      LV_SYMBOL_WARNING "  The installer can modify boot and system partitions.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(warning, 48, landscape ? 555 : 720);
  lv_obj_set_width(warning, landscape ? 1704 : 1152);

  scene->installer_prompt_decline = Button(
      sheet, "Cancel",
      [overlay = scene->installer_prompt] {
        if (RecoveryAnswerInstallerPrompt(false))
          lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
      });
  lv_obj_set_pos(scene->installer_prompt_decline, 48,
                 landscape ? 650 : 830);
  lv_obj_set_size(scene->installer_prompt_decline,
                  landscape ? 820 : 552, 142);

  scene->installer_prompt_accept = Button(
      sheet, "Install now",
      [overlay = scene->installer_prompt] {
        if (RecoveryAnswerInstallerPrompt(true))
          lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
      }, true);
  lv_obj_set_pos(scene->installer_prompt_accept,
                 landscape ? 932 : 648,
                 landscape ? 650 : 830);
  lv_obj_set_size(scene->installer_prompt_accept,
                  landscape ? 820 : 552, 142);
}

void ReflowOperationHeader(const OperationScene &scene) {
  auto *card = lv_obj_get_parent(scene.status);
  const bool landscape = Landscape(lv_obj_get_parent(card));
  lv_obj_update_layout(card);
  lv_obj_set_y(scene.detail, std::max(112,
      static_cast<int>(lv_obj_get_y(scene.status) + lv_obj_get_height(scene.status)) + 12));
  lv_obj_update_layout(card);
  const int extra = std::max(0,
      static_cast<int>(lv_obj_get_y(scene.detail) + lv_obj_get_height(scene.detail)) + 16 - 222);
  lv_obj_set_y(scene.percent, 222 + extra);
  lv_obj_align(scene.elapsed, LV_ALIGN_TOP_RIGHT, -48, 246 + extra);
  lv_obj_set_y(scene.progress, 310 + extra);
  if (scene.progress_pulse) lv_obj_set_y(scene.progress_pulse, 314 + extra);
  lv_obj_set_y(scene.metrics_panel, 390 + extra);
  if (scene.transfer_speed) lv_obj_set_y(scene.transfer_speed, 600 + extra);
  lv_obj_set_y(scene.destination, 650 + extra);
  lv_obj_set_height(card, (landscape ? 850 : 756) + extra);
  if (!landscape) lv_obj_set_y(scene.activity_panel, 1230 + extra);
  if (scene.payload_parallel && scene.payload_list && scene.details) {
    auto *screen = lv_obj_get_parent(card);
    lv_obj_update_layout(screen);
    // Use the otherwise empty lower viewport while flashing. Once Done is
    // visible, shrink the scrollable list to leave the footer accessible.
    const int bottom = scene.done &&
        !lv_obj_has_flag(scene.done, LV_OBJ_FLAG_HIDDEN)
        ? static_cast<int>(lv_obj_get_y(scene.done)) - 36
        : static_cast<int>(lv_obj_get_height(screen)) - (landscape ? 70 : 130);
    // Use the resting position, not the entrance animation's temporary offset.
    const int height = std::max(1, bottom - (landscape ? 340 : 1230 + extra));
    lv_obj_set_height(scene.activity_panel, height);
    lv_obj_set_y(scene.details, height - 200);
    lv_obj_set_height(scene.payload_list, std::max(1,
        height - 240 - static_cast<int>(lv_obj_get_y(scene.payload_list))));
  }
}

}  // namespace

void SetJobRequest(const JobRequest &request) { gRequest = request; }
JobRequest GetJobRequest() { return gRequest; }

OperationScene BuildJobScene(lv_obj_t *screen, const JobRequest &request,
                             ActionCallback callback, void *context) {
  Header(screen, request.title.c_str(), "Progress & activity", nullptr, nullptr);
  OperationScene result;
  result.job = request.job;
  result.pc_restore = request.backup_action == "restore";
  result.pc_app_backup = request.job == Job::kPcBackupOperation && request.backup_kind == "apps";
  result.payload_parallel = request.job == Job::kFlashPayload && request.payload_direct;
  result.format_data = request.job == Job::kFormatData;
  const bool installer =
      request.job == Job::kInstall || request.job == Job::kSideload;
  result.indeterminate_progress = request.job == Job::kSideload;
  result.started = lv_tick_get();
  const bool landscape = Landscape(screen);

  auto *card = lv_obj_create(screen);
  Panel(card, 40, kMainSheet);
  lv_obj_set_pos(card, 64, landscape ? 340 : 430);
  lv_obj_set_size(card, landscape ? 1450 : 1312,
                  landscape ? 850 : 756);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);

  // Interactive installers use a clean standalone glyph. Other operations
  // retain the calm status beacon used throughout AERA.
  result.activity = lv_obj_create(card);
  if (installer) {
    Clear(result.activity);
    lv_obj_set_pos(result.activity, 48, 48);
    lv_obj_set_size(result.activity, 96, 96);
  } else {
    Panel(result.activity, LV_RADIUS_CIRCLE, kAccentSoft);
    lv_obj_set_pos(result.activity, 48, 46);
    lv_obj_set_size(result.activity, 124, 124);
    lv_obj_set_style_border_width(result.activity, 3, 0);
    lv_obj_set_style_border_color(result.activity, kAccent, 0);
    lv_obj_set_style_border_opa(result.activity, LV_OPA_30, 0);
  }
  auto *activity_icon = Label(result.activity, request.job == Job::kPcBackupOperation && result.pc_restore
      ? LV_SYMBOL_REFRESH : OperationSymbol(request.job),
                              &lv_font_montserrat_48, kAccent);
  lv_obj_center(activity_icon);
  if (!installer) {
    lv_anim_t breathe;
    lv_anim_init(&breathe);
    lv_anim_set_var(&breathe, result.activity);
    lv_anim_set_values(&breathe, LV_OPA_20, LV_OPA_60);
    lv_anim_set_duration(&breathe, 1450);
    lv_anim_set_playback_duration(&breathe, 1450);
    lv_anim_set_repeat_count(&breathe, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&breathe, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&breathe, SetActivityBorderOpacity);
    lv_anim_start(&breathe);
  }

  result.status = Label(card, request.job == Job::kPcBackupOperation && result.pc_restore
      ? "Preparing to restore" : InitialTitle(request.job), &lv_font_montserrat_48, kText);
  lv_obj_set_pos(result.status, installer ? 176 : 208, 42);
  lv_obj_set_width(result.status, installer ? 1042 : 1010);
  lv_label_set_long_mode(result.status, LV_LABEL_LONG_DOT);
  lv_obj_set_style_max_height(result.status,
      2 * lv_font_get_line_height(lv_obj_get_style_text_font(result.status, LV_PART_MAIN)), 0);
  result.detail = Label(card, "Waiting for the recovery backend to begin.", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(result.detail, installer ? 176 : 208, 112);
  lv_obj_set_width(result.detail, installer ? 1042 : 1010);
  lv_label_set_long_mode(result.detail, LV_LABEL_LONG_DOT);
  lv_obj_set_style_max_height(result.detail,
      2 * lv_font_get_line_height(lv_obj_get_style_text_font(result.detail, LV_PART_MAIN)), 0);

  result.percent = Label(card, "0%", &lv_font_montserrat_48, kAccent);
  lv_obj_set_pos(result.percent, 48, 222);
  result.elapsed = Label(card, "0:00 elapsed", &lv_font_montserrat_24, kMuted);
  lv_obj_align(result.elapsed, LV_ALIGN_TOP_RIGHT, -48, 246);

  result.progress = lv_bar_create(card);
  lv_obj_set_pos(result.progress, 48, 310);
  lv_obj_set_size(result.progress, 1216, 32);
  lv_bar_set_range(result.progress, 0, 100);
  lv_obj_set_style_anim_duration(result.progress, 450, 0);
  lv_bar_set_value(result.progress, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(result.progress, 16, LV_PART_MAIN);
  lv_obj_set_style_radius(result.progress, 16, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(result.progress, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(result.progress, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_bg_color(result.progress, kAccent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(result.progress, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_border_width(result.progress, 0, LV_PART_MAIN);

  if (result.indeterminate_progress || (request.job == Job::kPcBackupOperation &&
      request.backup_source == "computer" &&
      (request.backup_action == "backup" || request.backup_action == "restore"))) {
    result.progress_pulse = lv_obj_create(card);
    Clear(result.progress_pulse);
    lv_obj_set_pos(result.progress_pulse, 48, 314);
    lv_obj_set_size(result.progress_pulse, 240, 24);
    lv_obj_set_style_radius(result.progress_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(result.progress_pulse, kAccent, 0);
    lv_obj_set_style_bg_opa(result.progress_pulse, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(result.progress_pulse, kAccent, 0);
    lv_obj_set_style_shadow_width(result.progress_pulse, 18, 0);
    lv_obj_set_style_shadow_opa(result.progress_pulse, LV_OPA_40, 0);
    lv_obj_remove_flag(result.progress_pulse, LV_OBJ_FLAG_CLICKABLE);
    if (result.indeterminate_progress) StartIndeterminateProgress(result.progress_pulse);
    else lv_obj_add_flag(result.progress_pulse, LV_OBJ_FLAG_HIDDEN);
  }

  auto *stats = lv_obj_create(card);
  result.metrics_panel = stats;
  Panel(stats, 26, kMainPanel);
  lv_obj_set_pos(stats, 48, 390);
  lv_obj_set_size(stats, 1216, 180);
  auto *stats_title = Label(stats, "PROGRESS", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(stats_title, 30, 24);
  lv_obj_set_style_text_letter_space(stats_title, 3, 0);
  result.metrics = Label(stats, "Waiting for progress information", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(result.metrics, 30, 78);
  lv_obj_set_width(result.metrics, 740);
  lv_label_set_long_mode(result.metrics, LV_LABEL_LONG_DOT);
  result.files = Label(stats, "", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_align(result.files, LV_ALIGN_RIGHT_MID, -30, 28);
  lv_obj_set_width(result.files, 380);
  lv_obj_set_style_text_align(result.files, LV_TEXT_ALIGN_RIGHT, 0);

  const bool network = request.job == Job::kUploadBackup ||
      request.path.find("/mnt/nas") != std::string::npos;
  if (request.job == Job::kPcBackupOperation && request.backup_source == "computer" &&
      (request.backup_action == "backup" || request.backup_action == "restore")) {
    result.transfer_speed = Label(card, "0.0 MB/s", &lv_font_montserrat_32, kAccent);
    lv_obj_set_pos(result.transfer_speed, 48, 600);
    lv_obj_set_width(result.transfer_speed, 1216);
  }
  const std::string destination = request.job == Job::kClearSnapshotCow
      ? "Target  /  Super metadata"
      : request.job == Job::kSideload
      ? "Source  /  ADB over USB"
      : network ? "Destination  /  Network Storage" :
      request.path.empty() ? "Destination  /  Recovery storage" : "Destination  /  " + request.path;
  result.destination = Label(card, destination.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(result.destination, 48, 650);
  lv_obj_set_width(result.destination, 1216);
  lv_label_set_long_mode(result.destination, LV_LABEL_LONG_DOT);

  const int activity_y = landscape ? 340 : 1230;
  const int activity_height = installer
      ? (landscape ? 850 : 1230)
      : (landscape ? 850 : result.payload_parallel ? 1120 : 600);
  auto *activity_card = lv_obj_create(screen);
  result.activity_panel = activity_card;
  Panel(activity_card, 40, kMainSheet);
  lv_obj_set_pos(activity_card, landscape ? 1560 : 64, activity_y);
  lv_obj_set_size(activity_card, landscape ? 1544 : 1312,
                  activity_height);
  lv_obj_set_style_border_width(activity_card, 1, 0);
  lv_obj_set_style_border_color(activity_card, kMainLine, 0);
  lv_obj_set_style_border_opa(activity_card, LV_OPA_20, 0);
  result.activity_title = Label(activity_card,
      request.job == Job::kSideload ? "SIDELOAD & INSTALLER OUTPUT" :
          installer ? "INSTALLER OUTPUT" : "WHAT'S HAPPENING",
      &lv_font_montserrat_24, kAccent);
  lv_obj_set_pos(result.activity_title, 48, 42);
  lv_obj_set_style_text_letter_space(result.activity_title, 3, 0);
  const lv_font_t *activity_font = installer
      ? (landscape ? &lv_font_montserrat_20 : &lv_font_montserrat_24)
      : &lv_font_montserrat_32;
  const int activity_text_height = installer
      ? activity_height - 150 : LV_SIZE_CONTENT;
  result.activity_summary = Label(activity_card,
      installer ? "Waiting for installer output..." :
          "AERA is preparing the operation. Progress will appear here in plain language.",
      activity_font, kText);
  lv_obj_set_pos(result.activity_summary, 48, installer ? 100 : 112);
  lv_obj_set_size(result.activity_summary, landscape ? 1448 : 1216,
                  activity_text_height);
  if (result.pc_app_backup) {
    lv_obj_set_height(result.activity_summary, activity_height - 160);
    lv_label_set_long_mode(result.activity_summary, LV_LABEL_LONG_DOT);
  }
  if (installer) {
    const int line_space = landscape ? 0 : 2;
    const int line_height =
        static_cast<int>(lv_font_get_line_height(activity_font)) + line_space;
    result.installer_lines = static_cast<unsigned>(std::max(
        1, activity_text_height / line_height));
    lv_label_set_long_mode(result.activity_summary, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(result.activity_summary, line_space, 0);

    const int row_height = landscape ? 118 : 184;
    const int row_gap = landscape ? 12 : 18;
    const int row_top = landscape ? 90 : 106;
    const int row_width = landscape ? 1448 : 1216;
    for (int i = 0; i < 4; ++i) {
      result.steps[i] = lv_obj_create(activity_card);
      Panel(result.steps[i], 26, kMainPanel);
      lv_obj_set_pos(result.steps[i], 48,
                     row_top + i * (row_height + row_gap));
      lv_obj_set_size(result.steps[i], row_width, row_height);
      lv_obj_set_style_border_width(result.steps[i], 2, 0);
      lv_obj_set_style_border_color(result.steps[i], kMainLine, 0);
      lv_obj_set_style_border_opa(result.steps[i], LV_OPA_10, 0);

      result.step_icons[i] = Label(result.steps[i],
          i18n::Format("%02d", i + 1).c_str(), &lv_font_montserrat_32, kMuted);
      lv_obj_set_pos(result.step_icons[i], 28, landscape ? 34 : 52);
      lv_obj_set_width(result.step_icons[i], 70);
      lv_obj_set_style_text_align(result.step_icons[i], LV_TEXT_ALIGN_CENTER, 0);

      result.step_titles[i] = Label(result.steps[i], kRomInstallerStepTitles[i],
          &lv_font_montserrat_32, kMutedStrong);
      lv_obj_set_pos(result.step_titles[i], 126, landscape ? 14 : 34);
      lv_obj_set_width(result.step_titles[i], landscape ? 850 : 720);

      result.step_details[i] = Label(result.steps[i], kRomInstallerStepDetails[i],
          &lv_font_montserrat_20, kMuted);
      lv_obj_set_pos(result.step_details[i], 126, landscape ? 66 : 94);
      lv_obj_set_width(result.step_details[i], landscape ? 930 : 800);
      lv_label_set_long_mode(result.step_details[i], LV_LABEL_LONG_DOT);

      result.step_states[i] = Label(result.steps[i], "UP NEXT",
          &lv_font_montserrat_20, kMuted);
      lv_obj_align(result.step_states[i], LV_ALIGN_RIGHT_MID, -30, 0);
      lv_obj_set_width(result.step_states[i], 220);
      lv_obj_set_style_text_align(result.step_states[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_style_text_letter_space(result.step_states[i], 2, 0);
      lv_obj_add_flag(result.steps[i], LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    result.notice = Label(activity_card,
        LV_SYMBOL_WARNING "  Keep the device powered on until the operation completes.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(result.notice, 48, 280);
    lv_obj_set_width(result.notice, landscape ? 1448 : 1216);
  }
  if (result.payload_parallel) {
    i18n::BindLabel(result.activity_title, "PARTITION PROGRESS");
    lv_obj_set_pos(result.activity_summary, 48, 100);
    lv_obj_set_height(result.activity_summary, landscape ? 40 : 70);
    lv_obj_add_flag(result.notice, LV_OBJ_FLAG_HIDDEN);
    result.payload_list = lv_obj_create(activity_card);
    Clear(result.payload_list);
    lv_obj_set_pos(result.payload_list, 48, landscape ? 150 : 190);
    lv_obj_set_size(result.payload_list, landscape ? 1448 : 1216,
                    activity_height - 240 - (landscape ? 150 : 190));
    lv_obj_add_flag(result.payload_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(result.payload_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(result.payload_list, LV_SCROLLBAR_MODE_AUTO);
    result.payload_cards.resize(request.partitions.size());
    result.payload_names.resize(request.partitions.size());
    result.payload_states.resize(request.partitions.size());
    result.payload_bars.resize(request.partitions.size());
    for (size_t i = 0; i < request.partitions.size(); ++i) {
      auto *row = lv_obj_create(result.payload_list);
      Clear(row);
      lv_obj_set_pos(row, landscape ? (i % 2) * 748 : 0,
                     landscape ? (i / 2) * 115 : i * 86);
      lv_obj_set_size(row, landscape ? 700 : 1216, landscape ? 100 : 76);
      lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      result.payload_cards[i] = row;
      result.payload_names[i] = Label(row, "Waiting", &lv_font_montserrat_32, kText);
      lv_obj_set_width(result.payload_names[i], LV_PCT(48));
      lv_label_set_long_mode(result.payload_names[i], LV_LABEL_LONG_DOT);
      result.payload_states[i] = Label(row, "Queued", &lv_font_montserrat_24, kMutedStrong);
      lv_obj_set_width(result.payload_states[i], LV_PCT(50));
      lv_obj_align(result.payload_states[i], LV_ALIGN_TOP_RIGHT, 0, 6);
      lv_obj_set_style_text_align(result.payload_states[i], LV_TEXT_ALIGN_RIGHT, 0);
      auto *bar = lv_bar_create(row);
      result.payload_bars[i] = bar;
      lv_obj_set_pos(bar, 0, landscape ? 60 : 50);
      lv_obj_set_size(bar, LV_PCT(100), 12);
      lv_bar_set_range(bar, 0, 100);
      lv_obj_set_style_radius(bar, 9, LV_PART_MAIN);
      lv_obj_set_style_radius(bar, 9, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(bar, kMainLine, LV_PART_MAIN);
      lv_obj_set_style_bg_color(bar, kAccent, LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
      lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
  }
  result.details = Button(activity_card, "View technical details", [screen] {
    ShowTechnicalLog(screen);
  });
  lv_obj_set_pos(result.details, 48,
                 installer || result.payload_parallel ? activity_height - 200 :
                     (landscape ? 650 : 400));
  lv_obj_set_size(result.details, landscape ? 1448 : 1216, 132);

  const bool backup = request.job == Job::kBackup ||
      request.job == Job::kUploadBackup || request.job == Job::kRestore;
  const bool format = request.job == Job::kFormatData;
  const bool wipe = request.job == Job::kWipe || request.job == Job::kFormatUsb ||
      request.job == Job::kClearSnapshotCow;
  const bool mount = request.job == Job::kMount || request.job == Job::kUnmount;
  result.done = Button(screen,
      format ? "Reboot options" : backup ? "Back to backups" : wipe ? "Back to wipe" : "Done",
      [=] {
        callback(format ? Action::kOpenReboot : backup ? Action::kBackup :
                 wipe ? Action::kWipe : mount ? Action::kMounts :
                 Action::kBackHome, context);
      }, true);
  lv_obj_set_size(result.done, landscape ? 1100 : 1280,
                  landscape ? 116 : 150);
  lv_obj_align(result.done, LV_ALIGN_BOTTOM_MID, 0,
               landscape ? -70 : -130);
  result.done_label = lv_obj_get_child(result.done, 0);
  lv_obj_set_style_text_align(result.done_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(result.done, LV_OBJ_FLAG_HIDDEN);

  if (request.job == Job::kSideload || (request.job == Job::kPcBackupOperation &&
      (request.backup_action == "backup" || request.backup_action == "restore"))) {
    result.cancel = Button(
        screen, request.job == Job::kSideload ? "Cancel sideload" :
            request.backup_action == "restore" ? "Cancel restore" : "Cancel backup",
        [callback, context, job = request.job] {
          callback(job == Job::kSideload ? Action::kCancelSideload : Action::kCancelPcBackup, context);
        });
    lv_obj_set_size(result.cancel, landscape ? 1100 : 1280,
                    landscape ? 116 : 150);
    lv_obj_align(result.cancel, LV_ALIGN_BOTTOM_MID, 0,
                 landscape ? -70 : -130);
  }

  if (installer) BuildInstallerPrompt(&result, screen, landscape);
  ReflowOperationHeader(result);

  AnimateEnter(card, 45, 24);
  AnimateEnter(activity_card, 85, 24);
  return result;
}

OperationScene BuildOperationScene(lv_obj_t *screen, const char *path,
                                   ActionCallback callback, void *context) {
  JobRequest request;
  request.job = Job::kInstall;
  request.title = "Install ZIP";
  request.path = path;
  return BuildJobScene(screen, request, callback, context);
}

void RefreshOperationScene(const OperationScene &scene) {
  const auto statistics = scene.transfer_speed ? aera::pc::GetBackupTransferStatistics()
                                               : aera::pc::BackupTransferStatistics{};
  const bool sideload = scene.job == Job::kSideload;
  const auto sideload_status = sideload ? RecoverySideloadStatus()
                                       : SideloadStatus{};
  const int progress = sideload && sideload_status.total_bytes > 0
      ? std::clamp(static_cast<int>(
            sideload_status.received_bytes * 100ULL /
            sideload_status.total_bytes), 0, 100)
      : RecoveryProgress();
  const bool indeterminate = statistics.finalizing || (scene.indeterminate_progress &&
      (sideload ? sideload_status.total_bytes == 0 : progress <= 0));
  lv_bar_set_value(scene.progress, indeterminate ? 0 : progress, LV_ANIM_ON);
  i18n::BindLabel(scene.percent, indeterminate
      ? (statistics.finalizing ? "Working..." : sideload ? "Waiting" : "Installing")
      : (std::to_string(progress) + "%").c_str());
  if (scene.progress_pulse && indeterminate) {
    if (lv_obj_has_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN))
      StartIndeterminateProgress(scene.progress_pulse);
    lv_obj_remove_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN);
  } else if (scene.progress_pulse) {
    lv_anim_delete(scene.progress_pulse, SetIndeterminateProgressX);
    lv_obj_add_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN);
  }

  const unsigned seconds = lv_tick_elaps(scene.started) / 1000;
  const std::string elapsed =
      i18n::Format("%u:%02u elapsed", seconds / 60, seconds % 60);
  i18n::BindLabel(scene.elapsed, elapsed.c_str());

  const auto raw = RecoveryOperationDetail();
  const auto explained_job = scene.job == Job::kPcBackupOperation
      ? (scene.pc_restore ? Job::kRestore : Job::kBackup) : scene.job;
  auto friendly = Explain(raw, explained_job, progress);
  if (scene.job == Job::kPcBackupOperation && scene.cancel) {
    const auto cancellation = aera::pc::GetBackupCancellation();
    if (cancellation.requested) lv_obj_add_state(scene.cancel, LV_STATE_DISABLED);
  }
  if (scene.pc_app_backup) {
    const auto lines = Lines(raw);
    friendly.title = lines.empty() ? (scene.pc_restore ? "Preparing to restore" : "Preparing your backup") : lines[0];
    if (friendly.title == "App Backup Vault")
      friendly.title = scene.pc_restore ? "Preparing to restore" : "Preparing your backup";
    if (friendly.title == "Restoring apps")
      friendly.title = i18n::Format("Restoring %s", i18n::Translate("Apps"));
    friendly.amount = lines.size() > 1 ? lines[1] : "";
    friendly.files = lines.size() > 2 ? std::string(i18n::Translate("Files")) + ": " + lines[2] : "";
    friendly.activity = lines.size() > 3 ? lines[3] : friendly.explanation;
  }
  if (scene.payload_parallel) {
    const auto state = RecoveryPayloadFlashProgress();
    size_t active = 0, verified = 0, queued = 0, failed = 0;
    for (size_t i = 0; i < state.partitions.size(); ++i) {
      const auto phase = state.partitions[i].phase;
      if (phase == PayloadPhase::Writing || phase == PayloadPhase::Verifying) ++active;
      if (phase == PayloadPhase::Verified) ++verified;
      if (phase == PayloadPhase::Failed) ++failed;
      if (phase == PayloadPhase::Queued || phase == PayloadPhase::Preparing || phase == PayloadPhase::Written) ++queued;
    }
    // Stable rows prevent partitions jumping around while the user scrolls.
    for (size_t row = 0; row < scene.payload_cards.size(); ++row) {
      if (row >= state.partitions.size()) { lv_obj_add_flag(scene.payload_cards[row], LV_OBJ_FLAG_HIDDEN); continue; }
      const auto& p = state.partitions[row];
      const char *phase = "Queued";
      int value = 0;
      const int portion = p.bytes ? int(100.0 * p.done / p.bytes) : 0;
      switch (p.phase) {
        case PayloadPhase::Preparing: phase = "Preparing"; break;
        case PayloadPhase::Writing: phase = "Writing"; value = portion; break;
        case PayloadPhase::Written: phase = "Awaiting readback"; value = 100; break;
        case PayloadPhase::Verifying: phase = "Verifying"; value = portion; break;
        case PayloadPhase::Verified: phase = "Verified"; value = 100; break;
        case PayloadPhase::Failed: phase = "Failed"; break;
        case PayloadPhase::NotFlashed: phase = "Not flashed"; break;
        default: break;
      }
      i18n::BindLabel(scene.payload_names[row], p.name.c_str());
      const std::string detail = std::string(phase) +
          (p.phase == PayloadPhase::Writing || p.phase == PayloadPhase::Verifying ?
           "  " + std::to_string(portion) + "%" : "");
      i18n::BindLabel(scene.payload_states[row], detail.c_str());
      lv_bar_set_value(scene.payload_bars[row], value, LV_ANIM_OFF);
      lv_obj_remove_flag(scene.payload_cards[row], LV_OBJ_FLAG_HIDDEN);
    }
    friendly.title = active ? "Flashing payload images" : verified == state.partitions.size() && verified ?
        "Payload images verified" : "Preparing payload targets";
    friendly.explanation = "Each partition is written and read back for SHA-256 verification. The active slot stays unchanged.";
    friendly.files = std::to_string(verified) + " / " + std::to_string(state.partitions.size()) + " verified";
    friendly.amount = state.available ? std::to_string(active) + " active / " + std::to_string(queued) + " queued" : "Preparing targets";
    friendly.activity = failed ? std::to_string(failed) + " failed; no new writes will start. Finishing active workers." : friendly.amount;
  }
  if (sideload) {
    const std::string history =
        CleanInstallerHistory(RecoveryInstallerStatus(),
                              std::max(1U, scene.installer_lines));
    const auto installer_lines = Lines(history);
    const std::string installer =
        installer_lines.empty() ? "" : installer_lines.back();
    if (sideload_status.cancel_requested) {
      friendly.title = "Cancelling sideload";
      friendly.explanation =
          "Stopping the package stream and restoring normal ADB.";
      friendly.amount = sideload_status.total_bytes == 0
          ? "No package received"
          : DataSize(sideload_status.received_bytes) + " / " +
                DataSize(sideload_status.total_bytes);
      friendly.activity = "Waiting for the sideload service to stop safely...";
      if (scene.cancel) {
        lv_obj_add_state(scene.cancel, LV_STATE_DISABLED);
        if (lv_obj_get_child_count(scene.cancel) > 0)
          i18n::BindLabel(lv_obj_get_child(scene.cancel, 0), "Cancelling...");
      }
    } else if (sideload_status.total_bytes == 0) {
      friendly.title = "Waiting for package";
      friendly.explanation =
          "Run adb sideload package.zip on the connected computer.";
      friendly.amount = "USB sideload is ready";
      friendly.activity =
          "AERA is waiting for the computer to begin sending a package.";
    } else {
      friendly.title = installer.empty()
          ? "Receiving package" : "Receiving and installing package";
      friendly.explanation = installer.empty()
          ? "Package blocks are streaming directly into the AERA installer."
          : installer;
      friendly.amount = DataSize(sideload_status.received_bytes) + " / " +
                        DataSize(sideload_status.total_bytes);
      friendly.files = std::to_string(progress) + "% received";
      friendly.activity = history.empty()
          ? "Keep the USB cable connected until sideload completes."
          : history;
    }
  } else if (scene.job == Job::kInstall) {
    const std::string history =
        CleanInstallerHistory(RecoveryInstallerStatus(),
                              std::max(1U, scene.installer_lines));
    const auto presentation = RecoveryInstallerPresentation();
    const auto installer_lines = Lines(history);
    const std::string installer =
        installer_lines.empty() ? "" : installer_lines.back();
    if (presentation.active) {
      friendly.title = presentation.stage_title.empty()
          ? (presentation.package_name.empty() ? "AERA interactive installer"
                                               : presentation.package_name)
          : presentation.stage_title;
      friendly.explanation = presentation.stage_detail.empty()
          ? "The package is preparing its next step."
          : presentation.stage_detail;
      if (presentation.stage_count > 0) {
        friendly.amount = i18n::Format(
            "Step %d of %d", presentation.stage, presentation.stage_count);
      } else {
        friendly.amount = "Waiting for confirmation";
      }
      friendly.files = presentation.device;
      friendly.activity = presentation.stage_detail;
      if (presentation.rebooting) {
        friendly.title = "Restarting AERA Recovery";
        if (presentation.reboot_seconds > 0) {
          friendly.explanation = i18n::Format(
              "The update is complete. Recovery will restart in %d second%s.",
              presentation.reboot_seconds,
              presentation.reboot_seconds == 1 ? "" : "s");
          friendly.amount = i18n::Format(
              "Restarting in %d", presentation.reboot_seconds);
        } else {
          friendly.explanation =
              "The update is complete. Restarting recovery now.";
          friendly.amount = "Restarting now";
        }
        friendly.activity = friendly.explanation;
      }
      SetInstallerTimelineVisible(scene, true);
      RefreshInstallerTimeline(scene, presentation.stage,
                               friendly.explanation,
                               IsRecoveryUpdater(presentation));
      if (scene.activity_title)
        i18n::BindLabel(scene.activity_title, "INSTALLATION PROGRESS");
      std::string identity = presentation.package_name;
      if (!presentation.device.empty()) {
        if (!identity.empty()) identity += "  /  ";
        identity += presentation.device;
      }
      if (!presentation.author.empty()) {
        if (!identity.empty()) identity += "  /  ";
        identity += presentation.author;
      }
      if (!identity.empty()) i18n::BindLabel(scene.destination, identity.c_str());
    } else if (!installer.empty()) {
      SetInstallerTimelineVisible(scene, false);
      if (scene.activity_title)
        i18n::BindLabel(scene.activity_title, "INSTALLER OUTPUT");
      const std::string lower = Lower(installer);
      if (lower.find("installing aera to slot") != std::string::npos ||
          lower.find("flashing aera") != std::string::npos) {
        friendly.title = "Writing AERA recovery";
      } else if (lower.find("finished installing") != std::string::npos) {
        friendly.title = "Installation complete";
      } else if (lower.find("reboot") != std::string::npos ||
                 (installer.size() >= 2 &&
                  std::isdigit(static_cast<unsigned char>(installer[0])) &&
                  installer[1] == 's')) {
        friendly.title = "Restarting recovery";
      } else {
        friendly.title = "Installing AERA update";
      }
      friendly.explanation = installer;
      friendly.amount = installer;
      friendly.files.clear();
      friendly.activity = history;
    } else {
      SetInstallerTimelineVisible(scene, false);
      friendly.amount = "Waiting for installer output";
      friendly.activity = "Waiting for installer output...";
    }
  }
  if (statistics.finalizing) {
    friendly.title = "Saving file on computer";
    friendly.explanation = "Keep the browser open while the computer finishes saving.";
  }
  if (scene.job == Job::kPcBackupOperation && aera::pc::GetBackupCancellation().requested)
    friendly.title = "Cancelling...";
  i18n::BindLabel(scene.status, friendly.title.c_str());
  i18n::BindLabel(scene.detail, friendly.explanation.c_str());
  ReflowOperationHeader(scene);
  i18n::BindLabel(scene.metrics,
      friendly.amount.empty() ? "Working..." : friendly.amount.c_str());
  i18n::BindLabel(scene.files, friendly.files.c_str());
  if (scene.transfer_speed) {
    char rate[64];
    snprintf(rate, sizeof(rate), "%.1f MB/s", statistics.bytes_per_second / 1000000.0);
    i18n::BindLabel(scene.transfer_speed, rate);
  }
  if (scene.activity_summary &&
      !lv_obj_has_flag(scene.activity_summary, LV_OBJ_FLAG_HIDDEN))
    i18n::BindLabel(scene.activity_summary, friendly.activity.c_str());

  if (scene.installer_prompt) {
    const auto prompt = RecoveryInstallerPrompt();
    if (prompt.active) {
      i18n::BindLabel(scene.installer_prompt_title,
                      prompt.title.empty() ? "Ready to install?"
                                           : prompt.title.c_str());
      i18n::BindLabel(scene.installer_prompt_message,
                      prompt.message.empty()
                          ? "Review the package before continuing."
                          : prompt.message.c_str());
      if (scene.installer_prompt_accept &&
          lv_obj_get_child_count(scene.installer_prompt_accept) > 0) {
        i18n::BindLabel(lv_obj_get_child(scene.installer_prompt_accept, 0),
                        prompt.accept.empty() ? "Install now"
                                              : prompt.accept.c_str());
      }
      if (scene.installer_prompt_decline &&
          lv_obj_get_child_count(scene.installer_prompt_decline) > 0) {
        i18n::BindLabel(lv_obj_get_child(scene.installer_prompt_decline, 0),
                        prompt.decline.empty() ? "Cancel"
                                               : prompt.decline.c_str());
      }
      if (lv_obj_has_flag(scene.installer_prompt, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(scene.installer_prompt, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_get_child_count(scene.installer_prompt) > 0)
          AnimateEnter(lv_obj_get_child(scene.installer_prompt, 0), 0, 30);
      }
    } else {
      lv_obj_add_flag(scene.installer_prompt, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void CompleteOperationScene(const OperationScene &scene, bool success,
                            const char *detail) {
  const bool sideload_cancelled =
      scene.job == Job::kSideload &&
      RecoverySideloadStatus().cancel_requested;
  RecoveryVibrate(Haptic::kAction);
  if (scene.installer_prompt)
    lv_obj_add_flag(scene.installer_prompt, LV_OBJ_FLAG_HIDDEN);
  RefreshOperationScene(scene);
  lv_anim_delete(scene.activity, SetActivityBorderOpacity);
  if (scene.progress_pulse)
    lv_anim_delete(scene.progress_pulse, SetProgressPulseOpacity);
  if (scene.progress_pulse)
    lv_anim_delete(scene.progress_pulse, SetIndeterminateProgressX);
  if (success) {
    lv_bar_set_value(scene.progress, 100, LV_ANIM_ON);
    i18n::BindLabel(scene.percent, "100%");
  }
  const auto result_color = success ? design::kGreen : design::kRed;
  if (scene.transfer_speed) lv_obj_add_flag(scene.transfer_speed, LV_OBJ_FLAG_HIDDEN);
  if (scene.job != Job::kInstall && scene.job != Job::kSideload) {
    lv_obj_set_style_bg_color(scene.activity,
        success ? design::kGreenSoft : design::kRedSoft, 0);
    lv_obj_set_style_border_color(scene.activity, result_color, 0);
    lv_obj_set_style_border_opa(scene.activity, LV_OPA_50, 0);
  }
  if (lv_obj_get_child_count(scene.activity) > 0) {
    auto *icon = lv_obj_get_child(scene.activity, 0);
    i18n::BindLabel(icon, success ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(icon, result_color, 0);
  }
  lv_obj_set_style_bg_color(scene.progress, result_color, LV_PART_INDICATOR);
  if (scene.progress_pulse) {
    lv_obj_add_flag(scene.progress_pulse, LV_OBJ_FLAG_HIDDEN);
  }
  i18n::BindLabel(scene.status,
      scene.job == Job::kPcBackupOperation && aera::pc::GetBackupCancellation().requested ? "Operation cancelled" :
      sideload_cancelled ? "Sideload cancelled" :
      success ? "Operation complete" : "Operation stopped");
  lv_obj_set_style_text_color(scene.status, result_color, 0);
  lv_obj_set_style_text_color(scene.percent, result_color, 0);
  const bool structured_installer = scene.steps[0] &&
      RecoveryInstallerPresentation().active;
  if (structured_installer) {
    const auto presentation = RecoveryInstallerPresentation();
    SetInstallerTimelineVisible(scene, true);
    RefreshInstallerTimeline(scene,
        std::max(1, presentation.stage),
        detail && *detail ? detail : "",
        IsRecoveryUpdater(presentation), success, !success);
    if (scene.activity_title)
      i18n::BindLabel(scene.activity_title,
          success ? "INSTALLATION COMPLETE" : "INSTALLATION STOPPED");
  }
  if (scene.format_data && success) {
    i18n::BindLabel(scene.detail, "Data was formatted successfully. Reboot recovery before using /data again.");
    i18n::BindLabel(scene.activity_summary,
        "Android may need a moment to recreate shared storage on the next boot.");
  } else if (sideload_cancelled) {
    i18n::BindLabel(scene.detail,
                    "Normal ADB and the previous USB mode were restored.");
    i18n::BindLabel(
        scene.activity_summary,
        "The sideload session ended without completing the package.");
  } else {
    i18n::BindLabel(scene.detail, detail && *detail ? detail :
        success ? "Everything finished successfully." : "Open technical details to see what went wrong.");
    if (!structured_installer)
      i18n::BindLabel(scene.activity_summary, success ?
          "The requested operation completed successfully. It is now safe to continue." :
          "AERA could not finish this operation. Open technical details for troubleshooting information.");
  }
  if (scene.notice) {
    i18n::BindLabel(scene.notice, success ?
        LV_SYMBOL_OK "  Finished safely. You can now continue." :
        LV_SYMBOL_WARNING "  Review technical details before trying again.");
    lv_obj_set_style_text_color(scene.notice, result_color, 0);
  }
  if (scene.details)
    lv_obj_remove_flag(scene.details, LV_OBJ_FLAG_HIDDEN);
  if (scene.cancel)
    lv_obj_add_flag(scene.cancel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(scene.done, LV_OBJ_FLAG_HIDDEN);
  ReflowOperationHeader(scene);
}

}  // namespace aeraui
