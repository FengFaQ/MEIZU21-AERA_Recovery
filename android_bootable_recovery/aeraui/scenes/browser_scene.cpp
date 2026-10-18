/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene.hpp"
#include "package_review.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <lvgl.h>

#include "design.hpp"
#include "aeraui/engine.hpp"
#include "aeraui/status_bar.hpp"

namespace aeraui {
namespace {
using namespace design;

struct Entry {
  std::string name;
  std::string path;
  bool directory = false;
  uint64_t size = 0;
};

struct BrowserState {
  ActionCallback callback;
  void *context;
  std::string path = "/sdcard";
  std::string selected;
  std::vector<Entry> entries;
  lv_obj_t *path_label = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *empty = nullptr;
  lv_obj_t *overlay = nullptr;
  lv_obj_t *confirm_name = nullptr;
  lv_obj_t *confirm_path = nullptr;
};

std::string gSelectedPackage;

bool EndsWithInsensitive(const std::string &value, const char *suffix) {
  const size_t suffix_size = std::char_traits<char>::length(suffix);
  if (value.size() < suffix_size) return false;
  return std::equal(suffix, suffix + suffix_size,
                    value.end() - static_cast<ptrdiff_t>(suffix_size),
                    [](char left, char right) {
                      return std::tolower(static_cast<unsigned char>(left)) ==
                             std::tolower(static_cast<unsigned char>(right));
                    });
}

std::string ParentPath(const std::string &path) {
  if (path.empty() || path == "/") return "/";
  const size_t end = path.find_last_not_of('/');
  if (end == std::string::npos) return "/";
  const size_t slash = path.find_last_of('/', end);
  return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

std::string SizeLabel(uint64_t bytes) {
  char text[32];
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    snprintf(text, sizeof(text), "%.1f GiB",
             static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 1024ULL * 1024ULL) {
    snprintf(text, sizeof(text), "%.1f MiB",
             static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    snprintf(text, sizeof(text), "%.1f KiB",
             static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(text, sizeof(text), "%llu B",
             static_cast<unsigned long long>(bytes));
  }
  return text;
}

void DeleteState(lv_event_t *event) {
  delete static_cast<BrowserState *>(lv_event_get_user_data(event));
}

void CloseConfirmation(lv_event_t *event) {
  auto *state = static_cast<BrowserState *>(lv_event_get_user_data(event));
  if (state != nullptr) lv_obj_add_flag(state->overlay, LV_OBJ_FLAG_HIDDEN);
}

void ConfirmPackage(lv_event_t *event) {
  auto *state = static_cast<BrowserState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->selected.empty() || state->callback == nullptr)
    return;
  gSelectedPackage = state->selected;
  state->callback(Action::kInstallPackage, state->context);
}

void Populate(BrowserState *state);

void OpenEntry(lv_event_t *event) {
  auto *state = static_cast<BrowserState *>(lv_event_get_user_data(event));
  if (state == nullptr) return;
  const size_t index = reinterpret_cast<uintptr_t>(
      lv_obj_get_user_data(lv_event_get_target_obj(event)));
  if (index >= state->entries.size()) return;
  const Entry entry = state->entries[index];
  if (entry.directory) {
    state->path = entry.path;
    Populate(state);
    return;
  }
  if (!EndsWithInsensitive(entry.name, ".zip")) return;
  ReviewPackage(lv_obj_get_screen(state->list), entry.path,
                state->callback, state->context);
}

void AddEntryRow(BrowserState *state, const Entry &entry, size_t index,
                 int32_t y) {
  const bool package = EndsWithInsensitive(entry.name, ".zip");
  const bool image = EndsWithInsensitive(entry.name, ".img");
  lv_obj_t *row = lv_button_create(state->list);
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_size(row, 1312, 190);
  NoScroll(row);
  lv_obj_set_style_radius(row, 0, 0);
  lv_obj_set_style_bg_color(row, kPanelStrong, 0);
  lv_obj_set_style_bg_color(row, kPanelPressed, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 28, 0);
  lv_obj_set_user_data(row, reinterpret_cast<void *>(index));
  lv_obj_add_event_cb(row, OpenEntry, LV_EVENT_CLICKED, state);

  const char *symbol = entry.directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
  const lv_color_t accent = package ? kAccent : (image ? kViolet : kCyan);
  const lv_color_t soft = package ? kAccentSoft : (image ? Color(0x251f39) : kCyanSoft);
  lv_obj_t *plate = IconPlate(row, symbol, accent, soft, 74);
  lv_obj_align(plate, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *name = Label(row, entry.name.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 104, 34);
  lv_obj_set_width(name, 790);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

  const std::string detail = entry.directory ? "Folder" : SizeLabel(entry.size);
  lv_obj_t *copy = Label(row, detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 104, 100);

  const char *state_text = entry.directory ? "OPEN" : (package ? "FLASH" : "IMAGE");
  lv_obj_t *tag = Kicker(row, state_text, accent);
  lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -44, 0);
  lv_obj_t *arrow = Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_24, kDim);
  lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);

  if (!entry.directory && !package) {
    lv_obj_add_state(row, LV_STATE_DISABLED);
    lv_obj_set_style_opa(row, LV_OPA_60, LV_STATE_DISABLED);
  }
}

void Populate(BrowserState *state) {
  lv_obj_clean(state->list);
  state->entries.clear();
  i18n::BindLabel(state->path_label, state->path.c_str());

  if (state->path != "/")
    state->entries.push_back({"..", ParentPath(state->path), true, 0});

  DIR *directory = opendir(state->path.c_str());
  if (directory != nullptr) {
    while (dirent *item = readdir(directory)) {
      if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
        continue;
      if (item->d_name[0] == '.' && !RecoveryPreference(Preference::kHiddenFiles)) continue;
      const std::string path = state->path == "/"
                                   ? "/" + std::string(item->d_name)
                                   : state->path + "/" + item->d_name;
      struct stat info {};
      if (stat(path.c_str(), &info) != 0) continue;
      const bool is_directory = S_ISDIR(info.st_mode);
      if (!is_directory && !EndsWithInsensitive(item->d_name, ".zip") &&
          !EndsWithInsensitive(item->d_name, ".img"))
        continue;
      state->entries.push_back(
          {item->d_name, path, is_directory, static_cast<uint64_t>(info.st_size)});
    }
    closedir(directory);
  }

  const size_t sort_start = state->path == "/" ? 0 : 1;
  std::sort(state->entries.begin() + static_cast<ptrdiff_t>(sort_start),
            state->entries.end(), [](const Entry &left, const Entry &right) {
              if (left.directory != right.directory) return left.directory;
              return left.name < right.name;
            });

  if (state->entries.empty()) {
    lv_obj_t *empty = Label(state->list, "No ZIP or image files here",
                            &lv_font_montserrat_24, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 160);
    return;
  }

  for (size_t index = 0; index < state->entries.size(); ++index)
    AddEntryRow(state, state->entries[index], index,
                static_cast<int32_t>(index) * 192);
  lv_obj_scroll_to_y(state->list, 0, LV_ANIM_OFF);
}

void MakeConfirmation(lv_obj_t *screen, BrowserState *state) {
  state->overlay = lv_obj_create(screen);
  NoScroll(state->overlay);
  lv_obj_set_pos(state->overlay, 0, 0);
  lv_obj_set_size(state->overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(state->overlay, Color(0x020204), 0);
  lv_obj_set_style_bg_opa(state->overlay, LV_OPA_80, 0);
  lv_obj_set_style_border_width(state->overlay, 0, 0);
  lv_obj_set_style_pad_all(state->overlay, 0, 0);

  lv_obj_t *sheet = lv_obj_create(state->overlay);
  Panel(sheet, 38, kPanelStrong);
  lv_obj_set_size(sheet, 1376, 1366);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -32);
  lv_obj_set_style_pad_all(sheet, 46, 0);
  lv_obj_t *tag = Kicker(sheet, "READY TO FLASH", kAccent);
  lv_obj_set_pos(tag, 0, 40);
  lv_obj_t *title = Label(sheet, "Confirm package", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 0, 104);
  state->confirm_name = Label(sheet, "Package", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->confirm_name, 0, 230);
  lv_obj_set_width(state->confirm_name, 1280);
  lv_label_set_long_mode(state->confirm_name, LV_LABEL_LONG_DOT);
  state->confirm_path = Label(sheet, "/sdcard", &lv_font_montserrat_18, kMuted);
  lv_obj_set_pos(state->confirm_path, 0, 294);
  lv_obj_set_width(state->confirm_path, 1280);
  lv_label_set_long_mode(state->confirm_path, LV_LABEL_LONG_DOT);

  lv_obj_t *warning = lv_obj_create(sheet);
  Panel(warning, 24, kInset);
  lv_obj_set_pos(warning, 0, 410);
  lv_obj_set_size(warning, 1284, 290);
  lv_obj_set_style_pad_all(warning, 30, 0);
  lv_obj_t *warning_title = Label(warning, "This can modify system partitions",
                                  &lv_font_montserrat_24, kText);
  lv_obj_set_pos(warning_title, 0, 42);
  lv_obj_t *warning_copy = Label(
      warning, "Verify the filename and battery level before continuing.",
      &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(warning_copy, 0, 116);

  lv_obj_t *cancel = lv_button_create(sheet);
  lv_obj_set_size(cancel, 604, 190);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  Panel(cancel, 24, kPanel);
  Interactive(cancel);
  lv_obj_add_event_cb(cancel, CloseConfirmation, LV_EVENT_CLICKED, state);
  lv_obj_t *cancel_text = Label(cancel, "Cancel", &lv_font_montserrat_32, kText);
  lv_obj_center(cancel_text);

  lv_obj_t *flash = lv_button_create(sheet);
  lv_obj_set_size(flash, 652, 190);
  lv_obj_align(flash, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  AccentButton(flash);
  lv_obj_add_event_cb(flash, ConfirmPackage, LV_EVENT_CLICKED, state);
  lv_obj_t *flash_text = Label(flash, "Flash package", &lv_font_montserrat_32, kCanvas);
  lv_obj_center(flash_text);
  lv_obj_add_flag(state->overlay, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void BuildBrowserScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *state = new BrowserState{callback, context};
  lv_obj_add_event_cb(screen, DeleteState, LV_EVENT_DELETE, state);
  Screen(screen);

  AttachStatusBar(screen, state->callback, state->context,
                  StatusBarAction::kBack);
  lv_obj_t *title = Label(screen, "Install ZIP", &lv_font_montserrat_48, kText);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 232);
  lv_obj_t *subtitle = Label(screen, "Select a package to add to the install queue.",
                             &lv_font_montserrat_24, kMutedStrong);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 318);

  lv_obj_t *path = lv_obj_create(screen);
  Panel(path, 0, kInset);
  lv_obj_set_pos(path, 64, 400);
  lv_obj_set_size(path, 1312, 140);
  lv_obj_set_style_pad_all(path, 26, 0);
  lv_obj_t *path_icon = Label(path, LV_SYMBOL_DIRECTORY, &lv_font_montserrat_24, kAccent);
  lv_obj_align(path_icon, LV_ALIGN_LEFT_MID, 0, 0);
  state->path_label = Label(path, state->path.c_str(), &lv_font_montserrat_24, kMutedStrong);
  lv_obj_align(state->path_label, LV_ALIGN_LEFT_MID, 60, 0);
  lv_obj_set_width(state->path_label, 1120);
  lv_label_set_long_mode(state->path_label, LV_LABEL_LONG_DOT);

  state->list = lv_obj_create(screen);
  lv_obj_set_pos(state->list, 64, 584);
  lv_obj_set_size(state->list, 1312, 2380);
  lv_obj_set_style_radius(state->list, 0, 0);
  lv_obj_set_style_bg_opa(state->list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(state->list, 0, 0);
  lv_obj_set_style_pad_all(state->list, 0, 0);
  lv_obj_set_scroll_dir(state->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_width(state->list, 8, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(state->list, kAccent, LV_PART_SCROLLBAR);

  MakeConfirmation(screen, state);
  Populate(state);
}

const char *GetSelectedPackagePath() { return gSelectedPackage.c_str(); }

void SetSelectedPackagePath(const char *path) {
  gSelectedPackage = path == nullptr ? "" : path;
}

}  // namespace aeraui
