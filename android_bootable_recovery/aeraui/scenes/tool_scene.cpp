/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "phone_keyboard.hpp"
#include "picture_decode.hpp"
#include "ui_components.hpp"
#include "circle_battery.hpp"
#include "partition_layout.hpp"
#include "wallpaper.hpp"
#include "root/root_manager.hpp"
#include "../../aera_remote/pc_connection.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>

namespace aeraui {
namespace {
using namespace widgets;
UserDecryptRequest selected_user_decrypt;
struct Tools {
  lv_obj_t *screen = nullptr, *list = nullptr, *summary = nullptr;
  lv_obj_t *navigation = nullptr;
  lv_obj_t *selection_detail = nullptr, *review = nullptr;
  Action tool = Action::kNone;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  std::vector<Volume> volumes;
  std::set<std::string> selected;
  std::vector<std::pair<std::string, lv_obj_t *>> usb_advanced;
  std::string restore_folder;
  bool compression = true;
  lv_obj_t *format_input = nullptr, *format_submit = nullptr;
  lv_timer_t *pc_timer = nullptr;
  lv_obj_t *pc_switch = nullptr, *pc_address = nullptr;
  lv_obj_t *pc_certificate = nullptr, *pc_details = nullptr, *pc_forget = nullptr;
  std::string pc_signature;
  lv_obj_t *temperature_controls = nullptr;
  lv_obj_t *temperature_sliders[6]{}, *temperature_amounts[6]{}, *temperature_panels[6]{};
  lv_obj_t *temperature_toggles[8]{};
  lv_obj_t *temperature_colors_heading = nullptr;
  TemperatureDisplay temperature_saved;
  lv_obj_t *battery_choices[2]{};
  lv_obj_t *battery_checks[2]{};
  lv_obj_t *battery_size_panel = nullptr, *battery_size_slider = nullptr, *battery_size_amount = nullptr;
  lv_obj_t *battery_percentage_row = nullptr;
  widgets::CircleBattery battery_preview_circle;
  lv_obj_t *battery_preview_classic = nullptr, *battery_preview_percentage = nullptr;
  lv_obj_t *battery_preview_body = nullptr, *battery_preview_tip = nullptr;
  int battery_saved_size = kBatteryCircleSizeDefault;
};

lv_obj_t *WorkflowModeCard(lv_obj_t *parent, int x, int y, int width,
                           const char *icon, const char *title,
                           const char *description, bool active,
                           Handler action);
void RestoreFolders(Tools *state);
std::string SnapshotCowDetail(const SnapshotCowStatus &status);
void ConfirmSnapshotCowCleanup(Tools *state);

void Open(Tools *state, Action action) { state->callback(action, state->context); }
bool AutoSaveSettings(Tools *state) {
  if (RecoveryAutoSavePreferences()) return true;
  Sheet(state->screen, "Could not save preferences",
        "Changes still apply to this session. Unlock and mount settings storage, then try again.");
  return false;
}
int theme_scroll_restore = -1;
void RefreshTheme(Tools *state) {
  if (!AutoSaveSettings(state)) return;
  if (state != nullptr && state->list != nullptr)
    theme_scroll_restore = lv_obj_get_scroll_y(state->list);
  Open(state, Action::kTheme);
}

struct WallpaperEntry {
  std::string name;
  std::string path;
  bool directory = false;
};

struct WallpaperThumbnail {
  std::string path;
  int y = 0;
  lv_obj_t *preview = nullptr;
  lv_obj_t *placeholder = nullptr;
  lv_obj_t *image = nullptr;
  std::shared_ptr<PictureData> data;
  lv_image_dsc_t descriptor{};
  int state = 0;
};

struct WallpaperPickerState {
  lv_obj_t *list = nullptr;
  lv_obj_t *status = nullptr;
  lv_timer_t *timer = nullptr;
  std::vector<std::unique_ptr<WallpaperThumbnail>> thumbnails;
  int loading = -1;
};

bool WallpaperStoragePath(const std::string &path) {
  for (const char *root : {"/sdcard", "/mnt/nas", "/usb_otg",
                           "/external_sd"}) {
    const size_t length = strlen(root);
    if (path == root || (path.compare(0, length, root) == 0 &&
        path.size() > length && path[length] == '/')) return true;
  }
  return false;
}

std::string WallpaperStorageRoot(const std::string &path) {
  for (const char *root : {"/sdcard", "/mnt/nas", "/usb_otg",
                           "/external_sd"}) {
    const size_t length = strlen(root);
    if (path == root || (path.compare(0, length, root) == 0 &&
        path.size() > length && path[length] == '/')) return root;
  }
  return {};
}

std::string WallpaperParent(const std::string &path) {
  const std::string root = WallpaperStorageRoot(path);
  if (root.empty() || path == root) return {};
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos || slash < root.size()
      ? root : path.substr(0, slash);
}

std::vector<WallpaperEntry> WallpaperRoots() {
  std::vector<WallpaperEntry> roots;
  auto add = [&roots](const std::string &name, const std::string &path) {
    struct stat info{};
    if (stat(path.c_str(), &info) || !S_ISDIR(info.st_mode)) return;
    if (std::any_of(roots.begin(), roots.end(), [&](const auto &entry) {
          return entry.path == path;
        })) return;
    roots.push_back({name, path, true});
  };
  add("Internal storage", "/sdcard");
  for (const auto &volume : RecoveryVolumes("storage")) {
    std::string path = volume.path;
    std::string name = volume.name;
    if (path == "/data/media/0" || path == "INTERNAL") {
      path = "/sdcard";
      name = "Internal storage";
    } else if (path == "/mnt/nas") {
      name = "Network storage";
    } else if (path == "/usb_otg") {
      name = "USB OTG";
    } else if (path == "/external_sd") {
      name = "SD card";
    }
    add(name, path);
  }
  add("Network storage", "/mnt/nas");
  add("USB OTG", "/usb_otg");
  add("SD card", "/external_sd");
  return roots;
}

bool WallpaperThumbnailVisible(const WallpaperPickerState *picker,
                               const WallpaperThumbnail *thumbnail) {
  const int scroll = lv_obj_get_scroll_y(picker->list);
  const int height = lv_obj_get_height(picker->list);
  return thumbnail->y + 150 >= scroll - 150 &&
         thumbnail->y <= scroll + height + 150;
}

void ShowWallpaperThumbnail(WallpaperThumbnail *thumbnail) {
  if (!thumbnail->data || !thumbnail->data->pixels) {
    thumbnail->state = 3;
    thumbnail->data.reset();
    return;
  }
  auto &descriptor = thumbnail->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = thumbnail->data->width;
  descriptor.header.h = thumbnail->data->height;
  descriptor.header.stride = thumbnail->data->width * 4;
  descriptor.data_size = thumbnail->data->width * thumbnail->data->height * 4;
  descriptor.data = thumbnail->data->pixels;
  thumbnail->image = lv_image_create(thumbnail->preview);
  lv_obj_set_size(thumbnail->image, LV_PCT(100), LV_PCT(100));
  lv_image_set_src(thumbnail->image, &descriptor);
  lv_image_set_inner_align(thumbnail->image, LV_IMAGE_ALIGN_COVER);
  lv_image_set_antialias(thumbnail->image, true);
  lv_obj_center(thumbnail->image);
  lv_obj_remove_flag(thumbnail->image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(thumbnail->placeholder, LV_OBJ_FLAG_HIDDEN);
  thumbnail->state = 2;
}

void WallpaperPickerTick(lv_timer_t *timer) {
  auto *picker = static_cast<WallpaperPickerState *>(
      lv_timer_get_user_data(timer));
  if (!picker || !picker->list) return;
  if (picker->loading >= 0) {
    auto *thumbnail = picker->thumbnails[picker->loading].get();
    if (thumbnail->data &&
        thumbnail->data->ready.load(std::memory_order_acquire)) {
      ShowWallpaperThumbnail(thumbnail);
      picker->loading = -1;
    }
  }
  if (picker->loading >= 0) return;
  for (size_t i = 0; i < picker->thumbnails.size(); ++i) {
    auto *thumbnail = picker->thumbnails[i].get();
    if (thumbnail->state != 0 ||
        !WallpaperThumbnailVisible(picker, thumbnail)) continue;
    thumbnail->state = 1;
    thumbnail->data = std::make_shared<PictureData>();
    picker->loading = static_cast<int>(i);
    std::thread([data = thumbnail->data, path = thumbnail->path] {
      DecodePictureThumbnail(path, 96, 96, *data);
    }).detach();
    break;
  }
}

void DeleteWallpaperPicker(lv_event_t *event) {
  auto *picker = static_cast<WallpaperPickerState *>(
      lv_event_get_user_data(event));
  if (!picker) return;
  if (picker->timer) lv_timer_delete(picker->timer);
  for (auto &thumbnail : picker->thumbnails) {
    if (thumbnail->data) thumbnail->data->cancelled = true;
    if (thumbnail->image) {
      lv_obj_delete(thumbnail->image);
      thumbnail->image = nullptr;
      lv_image_cache_drop(&thumbnail->descriptor);
    }
  }
  delete picker;
}

void OpenWallpaperPicker(Tools *tools, const std::string &requested = {}, bool lock = false) {
  const bool roots = requested.empty() ||
      !WallpaperStoragePath(requested) || requested.find("/../") != std::string::npos;
  const std::string path = roots ? std::string{} : requested;
  // The screen already has final coordinates. A just-created percentage-sized
  // overlay does not until LVGL's next layout pass, so querying it here could
  // collapse the picker rows to zero width.
  const int overlay_width = lv_obj_get_width(tools->screen);
  const int overlay_height = lv_obj_get_height(tools->screen);
  const int list_width = std::max(1, overlay_width - 48);
  auto *overlay = lv_obj_create(tools->screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_size(overlay, overlay_width, overlay_height);
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

  auto *close = Button(overlay, LV_SYMBOL_LEFT, [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_pos(close, 24, 36);
  lv_obj_set_size(close, 120, 112);
  auto *title = Label(overlay, "Choose wallpaper", &lv_font_montserrat_48,
                      kText);
  lv_obj_set_pos(title, 174, 38);
  auto *where = Label(overlay, roots ? "Available storage" : path.c_str(),
                      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(where, 174, 102);
  lv_obj_set_width(where, overlay_width - 220);
  lv_label_set_long_mode(where, LV_LABEL_LONG_DOT);

  auto *list = lv_obj_create(overlay);
  Clear(list);
  lv_obj_set_pos(list, 24, 184);
  lv_obj_set_size(list, list_width, overlay_height - 208);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  auto *picker = new WallpaperPickerState;
  picker->list = list;
  lv_obj_add_event_cb(overlay, DeleteWallpaperPicker, LV_EVENT_DELETE, picker);

  std::vector<WallpaperEntry> entries = roots
      ? WallpaperRoots() : std::vector<WallpaperEntry>{};
  if (!roots) entries.push_back({"Up one level", WallpaperParent(path), true});
  if (!roots) {
    if (DIR *directory = opendir(path.c_str())) {
      while (auto *item = readdir(directory)) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (item->d_name[0] == '.' &&
            !RecoveryPreference(Preference::kHiddenFiles)) continue;
        const std::string child = path + "/" + item->d_name;
        struct stat info{};
        if (lstat(child.c_str(), &info) || S_ISLNK(info.st_mode)) continue;
        if (S_ISDIR(info.st_mode))
          entries.push_back({item->d_name, child, true});
        else if (S_ISREG(info.st_mode) && IsPicture(child))
          entries.push_back({item->d_name, child, false});
        if (entries.size() >= 400) break;
      }
      closedir(directory);
    }
  }
  const size_t fixed = roots ? 0 : 1;
  std::sort(entries.begin() + static_cast<ptrdiff_t>(fixed), entries.end(),
            [](const auto &left, const auto &right) {
              if (left.directory != right.directory)
                return left.directory > right.directory;
              return left.name < right.name;
            });

  int y = 0;
  for (size_t index = 0; index < entries.size(); ++index) {
    const auto entry = entries[index];
    const bool up = !roots && index == 0;
    auto *row = Button(list, "", [tools, overlay, entry, lock] {
      if (entry.directory) {
        OpenWallpaperPicker(tools, entry.path, lock);
        lv_obj_delete_async(overlay);
        return;
      }
      std::string error;
      if (lock) {
        PictureData picture;
        DecodePictureThumbnail(entry.path, 1440, 3168, picture);
        if (!picture.pixels) {
          Sheet(tools->screen, "Wallpaper unavailable", picture.error);
          return;
        }
        auto theme = RecoveryLockTheme();
        theme.wallpaper = entry.path;
        if (!RecoverySetLockTheme(theme)) return;
        AutoSaveSettings(tools);
        lv_obj_delete_async(overlay);
        RefreshTheme(tools);
        return;
      }
      if (!wallpaper::Load(entry.path, &error)) {
        Sheet(tools->screen, "Wallpaper unavailable", error);
        return;
      }
      if (!RecoverySetWallpaperPath(entry.path)) {
        Sheet(tools->screen, "Wallpaper unavailable",
              "The selected wallpaper path could not be stored.");
        return;
      }
      lv_obj_delete_async(overlay);
      RefreshTheme(tools);
    });
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, list_width, 150);
    lv_obj_set_style_radius(row, up || roots ? 28 : 0, 0);
    lv_obj_set_style_bg_color(row, up || roots ? kMainPanel : kMainCanvas, 0);
    lv_obj_set_style_bg_opa(row, up || roots ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

    lv_obj_t *visual = nullptr;
    if (!entry.directory) {
      auto thumbnail = std::make_unique<WallpaperThumbnail>();
      thumbnail->path = entry.path;
      thumbnail->y = y;
      thumbnail->preview = lv_obj_create(row);
      Clear(thumbnail->preview);
      lv_obj_set_pos(thumbnail->preview, 20, 27);
      lv_obj_set_size(thumbnail->preview, 96, 96);
      lv_obj_set_style_radius(thumbnail->preview, 22, 0);
      lv_obj_set_style_clip_corner(thumbnail->preview, true, 0);
      lv_obj_set_style_bg_color(thumbnail->preview, kMainPanel, 0);
      lv_obj_set_style_bg_opa(thumbnail->preview, LV_OPA_COVER, 0);
      thumbnail->placeholder = Label(thumbnail->preview, LV_SYMBOL_IMAGE,
                                     &lv_font_montserrat_32, kMuted);
      lv_obj_center(thumbnail->placeholder);
      visual = thumbnail->preview;
      picker->thumbnails.push_back(std::move(thumbnail));
    } else {
      visual = IconPlate(row, up ? LV_SYMBOL_UP : LV_SYMBOL_DIRECTORY,
                         kAccent, kAccentSoft, 84);
      lv_obj_set_pos(visual, 26, 33);
    }
    (void)visual;
    auto *name = Label(row, entry.name.c_str(), &lv_font_montserrat_28, kText);
    lv_obj_set_pos(name, 148, 32);
    SingleLineLabel(name, list_width - 260,
                    &lv_font_montserrat_28);
    auto *detail = Label(row,
        entry.directory ? (up ? "Return to the parent folder" : "Folder")
                        : "Use as AERA wallpaper",
        &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(detail, 148, 88);
    auto *arrow = Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_24, kDim);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -28, 0);
    y += 154;
  }
  if (entries.empty()) {
    auto *empty = Label(list, roots ? "No storage is available"
                                    : "No images in this folder",
                        &lv_font_montserrat_28, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 160);
  }
  picker->timer = lv_timer_create(WallpaperPickerTick, 50, picker);
  lv_obj_move_foreground(overlay);
}
void Run(Tools *state, const JobRequest &request) {
  SetJobRequest(request);
  Open(state, Action::kRunOperation);
}

void CloseBackupActions(lv_obj_t *overlay) {
  if (!overlay) return;
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_delete_async(overlay);
}

void ConfirmBackupDeletion(Tools *state, const std::string &path,
                           const std::string &name) {
  Sheet(state->screen, "Delete backup?",
        "Permanently delete " + name +
            " and every partition stored inside it?\n\n"
            "This cannot be undone.",
        [state, path] {
          if (RecoveryDeleteBackup(path)) {
            RestoreFolders(state);
          } else {
            Sheet(state->screen, "Could not delete backup",
                  "AERA could not remove this backup. Check that its "
                  "storage is still mounted and writable.");
          }
        }, 0, false, SheetPresentation::kStandard, "Swipe to delete");
}

void BackupActions(Tools *state, const std::string &path,
                   const std::string &name) {
  const bool can_upload = RecoveryBackupCanUpload(path);
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_30, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    if (lv_event_get_target_obj(event) ==
        lv_event_get_current_target_obj(event))
      CloseBackupActions(lv_event_get_current_target_obj(event));
  }, LV_EVENT_CLICKED, nullptr);

  const bool landscape = Landscape(state->screen);
  const int width = landscape
      ? std::min(1800, static_cast<int>(lv_obj_get_width(state->screen)) - 256)
      : 1312;
  const int height = can_upload ? 700 : 530;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, width, height);
  lv_obj_align(sheet, landscape ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID,
               0, landscape ? 0 : -40);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_50, 0);
  lv_obj_set_style_bg_opa(sheet, IsLightMode() ? LV_OPA_90 : LV_OPA_80, 0);
  lv_obj_set_style_blur_backdrop(sheet, true, 0);
  lv_obj_set_style_blur_radius(sheet, 18, 0);
  lv_obj_set_style_blur_quality(sheet, LV_BLUR_QUALITY_SPEED, 0);

  auto *grabber = lv_obj_create(sheet);
  Clear(grabber);
  lv_obj_set_size(grabber, 112, 8);
  lv_obj_align(grabber, LV_ALIGN_TOP_MID, 0, 18);
  lv_obj_set_style_radius(grabber, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(grabber, kMutedStrong, 0);
  lv_obj_set_style_bg_opa(grabber, LV_OPA_30, 0);

  auto *title = Label(sheet, "Backup actions", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 48, 56);
  auto *detail = Label(sheet, name.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(detail, 48, 124);
  SingleLineLabel(detail, width - 190, &lv_font_montserrat_24);
  auto *close = Button(sheet, LV_SYMBOL_CLOSE,
                       [overlay] { CloseBackupActions(overlay); });
  lv_obj_set_pos(close, width - 138, 42);
  lv_obj_set_size(close, 96, 96);

  int y = 200;
  if (can_upload) {
    auto *upload = Button(sheet, LV_SYMBOL_UPLOAD "  Upload to network storage",
                          [state, overlay, path, name] {
      CloseBackupActions(overlay);
      JobRequest request;
      request.job = Job::kUploadBackup;
      request.title = "Upload Backup";
      request.path = path;
      request.name = name;
      Run(state, request);
    }, true);
    lv_obj_set_pos(upload, 48, y);
    lv_obj_set_size(upload, width - 96, 140);
    y += 164;
  }

  auto *remove = Button(sheet, LV_SYMBOL_TRASH "  Delete backup",
                        [state, overlay, path, name] {
    CloseBackupActions(overlay);
    ConfirmBackupDeletion(state, path, name);
  });
  lv_obj_set_pos(remove, 48, y);
  lv_obj_set_size(remove, width - 96, 140);
  lv_obj_set_style_border_width(remove, 1, 0);
  lv_obj_set_style_border_color(remove, kRed, 0);
  lv_obj_set_style_border_opa(remove, LV_OPA_60, 0);
  if (lv_obj_get_child_count(remove) > 0)
    lv_obj_set_style_text_color(lv_obj_get_child(remove, 0), kRed, 0);
}

std::string WipeDescription(const std::string &path) {
  if (path == "DALVIK") return "Generated app cache; rebuilt by Android";
  if (path == "INTERNAL") return "Deletes photos, downloads and files";
  if (path == "/data") return "Erase apps and settings on the data partition";
  if (path == "/metadata") return "Encryption metadata; may make data inaccessible";
  if (path == "/frp") return "Erase Factory Reset Protection data and any OEM-unlock state stored here";
  return i18n::Format("Erase %s", path.c_str());
}

void UsbFormatOptions(Tools *state, const std::string &path) {
  if (state->tool != Action::kWipe || state->selected.size() != 1 ||
      !state->selected.count(path)) return;
  const auto target = RecoveryUsbFormatTarget(path);
  if (!target.available || !target.error.empty()) {
    Sheet(state->screen, "USB format unavailable", target.error.empty()
        ? "The selected USB partition is no longer connected." : target.error);
    return;
  }
  const std::string description = target.name + "\n" + target.path + "\n" +
      target.block + "\n" + Size(target.bytes) + " / " + target.filesystem +
      "\n\nChoose a filesystem. Formatting erases all files on this USB partition. Other partitions are not changed.";
  Sheet(state->screen, "Format USB storage", description, {}, 1400, true,
        SheetPresentation::kStandard, "Swipe to confirm",
        [state, target, description](lv_obj_t *area) {
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(area, 20, 0);
    auto *info = Label(area, description.c_str(), &lv_font_montserrat_28, kMutedStrong);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    for (const auto &fs : target.filesystems) {
      const std::string label = fs == "vfat" ? "FAT32 — files up to 4 GB" :
          fs == "exfat" ? "exFAT — large files" : fs == "f2fs" ? "F2FS" : "ext4 — Linux filesystem";
      auto *choice = Button(area, label.c_str(), [state, target, fs] {
        if (state->selected.size() != 1 || !state->selected.count(target.path)) return;
        const auto current = RecoveryUsbFormatTarget(target.path);
        if (!current.available || current.identity != target.identity || !current.error.empty()) {
          Sheet(state->screen, "USB storage changed", "Reconnect the drive and select it again.");
          return;
        }
        JobRequest request;
        request.job = Job::kFormatUsb;
        request.title = "Format USB storage";
        request.path = target.path;
        request.usb_identity = target.identity;
        request.usb_filesystem = fs;
        request.confirmation = "format-usb";
        Sheet(state->screen, "Review USB format", target.name + "\n" + target.block +
              "\n" + Size(target.bytes) + "\n\n" + target.filesystem + " → " +
              (fs == "vfat" ? "FAT32" : fs) +
              "\n\nAll files on this USB partition will be permanently erased. This cannot be undone.",
              [state, request] { Run(state, request); }, 0, false,
              SheetPresentation::kStandard, "Swipe to format USB");
      });
      Panel(choice, 24, kMainPanel);
      lv_obj_set_size(choice, LV_PCT(100), 110);
    }
  });
}

void UpdateSelection(Tools *state) {
  for (const auto &entry : state->usb_advanced) {
    if (state->selected.size() == 1 && state->selected.count(entry.first))
      lv_obj_remove_flag(entry.second, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(entry.second, LV_OBJ_FLAG_HIDDEN);
  }
  uint64_t bytes = 0;
  for (const auto &v : state->volumes)
    if (state->selected.count(v.path)) bytes += v.bytes;
  std::string summary =
      i18n::Format("%zu selected", state->selected.size());
  if (state->tool == Action::kBackup)
    summary = i18n::Format("Estimated backup: %s", Size(bytes).c_str());
  i18n::BindLabel(state->summary, summary.c_str());
  if (state->review) {
    LayoutPartitionReview(state->screen, state->list, state->review,
                          !state->selected.empty());
    for (const auto &entry : state->usb_advanced) {
      auto *button = entry.second;
      if (lv_obj_has_flag(button, LV_OBJ_FLAG_HIDDEN)) continue;
      const int x = lv_obj_get_x(state->review);
      const int width = (lv_obj_get_width(state->review) - 24) / 2;
      lv_obj_set_pos(button, x, lv_obj_get_y(state->review));
      lv_obj_set_size(button, width, lv_obj_get_height(state->review));
      lv_obj_set_x(state->review, x + width + 24);
      lv_obj_set_width(state->review, width);
    }
  }
  if (state->selection_detail) {
    std::string detail = state->selected.size() == 1
        ? i18n::Format("%zu partition selected", state->selected.size())
        : i18n::Format("%zu partitions selected", state->selected.size());
    if (state->tool == Action::kBackup) {
      detail += " / ";
      detail += i18n::Translate(state->compression
          ? "Before compression; archive size varies"
          : "Archive overhead not included");
    }
    else if (!state->restore_folder.empty())
      detail = state->restore_folder.substr(state->restore_folder.find_last_of('/') + 1);
    i18n::BindLabel(state->selection_detail, detail.c_str());
  }
}

void LayoutWipeRow(lv_obj_t *row, lv_obj_t *name, lv_obj_t *detail,
                   lv_obj_t *indicator) {
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
  lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(name, LV_SIZE_CONTENT);
  lv_obj_set_height(detail, LV_SIZE_CONTENT);
  lv_obj_update_layout(row);
  const int detail_y = std::max(76, static_cast<int>(
      lv_obj_get_y(name) + lv_obj_get_height(name) + 14));
  lv_obj_set_y(detail, detail_y);
  lv_obj_update_layout(row);
  lv_obj_set_height(row, std::max(142, detail_y +
      static_cast<int>(lv_obj_get_height(detail)) + 20));
  lv_obj_align(indicator, LV_ALIGN_LEFT_MID, 24, 0);
  lv_obj_update_layout(row);
}

void PartitionRows(Tools *state) {
  for (const auto &entry : state->usb_advanced) lv_obj_delete(entry.second);
  state->usb_advanced.clear();
  lv_obj_clean(state->list);
  // Resolve the real scroll width before laying out rows. Without this pass,
  // LVGL can still report its creation-time width and the entire row collapses
  // into the 600 px fallback.
  lv_obj_update_layout(state->list);
  const int row_width = std::max(600, static_cast<int>(
      lv_obj_get_width(state->list)));
  int row_y = 0;
  for (size_t i = 0; i < state->volumes.size(); ++i) {
    const auto v = state->volumes[i];
    const bool selected = state->selected.count(v.path) != 0;
    {
      const bool wipe = state->tool == Action::kWipe;
      auto *row = Button(state->list, "", [] {});
      lv_obj_set_pos(row, 0, row_y);
      lv_obj_set_size(row, row_width, 142);
      lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_radius(row, 18, 0);
      auto *check = lv_checkbox_create(row);
      lv_checkbox_set_text(check, "");
      lv_obj_set_pos(check, 24, 46);
      lv_obj_set_style_pad_all(check, 0, LV_PART_MAIN);
      lv_obj_set_style_text_font(check, UiFont(&lv_font_montserrat_32), LV_PART_MAIN);
      lv_obj_set_style_pad_all(check, 8, LV_PART_INDICATOR);
      lv_obj_set_style_radius(check, 10, LV_PART_INDICATOR);
      lv_obj_set_style_border_width(check, 2, LV_PART_INDICATOR);
      lv_obj_set_style_border_color(check, kMainLine, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(check, kMainPanel, LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(check, LV_OPA_COVER, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(check, kAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_border_color(check, kAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_text_color(check, kCanvas, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_image_src(check, LV_SYMBOL_OK, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_image_recolor(check, kCanvas, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_image_opa(check, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
      if (selected) lv_obj_add_state(check, LV_STATE_CHECKED);
      auto *name = Label(row, v.name.c_str(), &lv_font_montserrat_36, kText);
      lv_obj_set_pos(name, 112, 14);
      lv_obj_set_width(name, wipe ? row_width - 150 : row_width - 430);
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      const auto detail = wipe ? WipeDescription(v.path) : v.path;
      auto *path = Label(row, detail.c_str(), &lv_font_montserrat_32,
          wipe && (v.path == "/metadata" || v.path == "INTERNAL" || v.path == "/frp") ? kAmber : kMuted);
      lv_obj_set_pos(path, 112, 76);
      lv_obj_set_width(path, wipe ? row_width - 150 : row_width - 430);
      lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
      if (!wipe) {
        auto *size = Label(row, Size(v.bytes).c_str(), &lv_font_montserrat_32, kMutedStrong);
        lv_obj_set_width(size, 270);
        lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(size, LV_ALIGN_RIGHT_MID, -28, 0);
      }
      OnClick(row, [state, v, check] {
        if (state->selected.erase(v.path)) lv_obj_remove_state(check, LV_STATE_CHECKED);
        else { state->selected.insert(v.path); lv_obj_add_state(check, LV_STATE_CHECKED); }
        UpdateSelection(state);
      });
      if (wipe) LayoutWipeRow(row, name, path, check);
      if (wipe && RecoveryUsbFormatTarget(v.path).available) {
        auto *advanced = Button(state->screen, "Advanced", [state, v] { UsbFormatOptions(state, v.path); }, true);
        lv_obj_set_size(advanced, Landscape(state->screen) ? 940 : 1280, 110);
        lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
        state->usb_advanced.emplace_back(v.path, advanced);
      }
      row_y += wipe ? lv_obj_get_height(row) + 6 : 148;
    }
  }
  if (state->tool == Action::kWipe) {
    const auto cow_status = RecoverySnapshotCowStatus();
    const bool cow_ready = cow_status.supported &&
        cow_status.metadata_readable && cow_status.safe_to_remove &&
        !cow_status.partitions.empty();
    auto *row = Button(state->list, "", [state] {
      ConfirmSnapshotCowCleanup(state);
    });
    lv_obj_set_pos(row, 0, row_y);
    lv_obj_set_size(row, row_width, 142);
    lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 18, 0);

    auto *indicator = lv_obj_create(row);
    Clear(indicator);
    lv_obj_set_pos(indicator, 24, 43);
    lv_obj_set_size(indicator, 54, 54);
    lv_obj_set_style_radius(indicator, 10, 0);
    lv_obj_set_style_border_width(indicator, 2, 0);
    lv_obj_set_style_border_color(indicator,
        cow_ready ? kAccent : kMainLine, 0);
    lv_obj_set_style_bg_color(indicator, kMainPanel, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    auto *symbol = Label(indicator, LV_SYMBOL_TRASH,
        &lv_font_montserrat_24, cow_ready ? kAccent : kMuted);
    lv_obj_center(symbol);

    auto *name = Label(row, "Snapshot COW cleanup",
        &lv_font_montserrat_36, cow_ready ? kText : kMuted);
    lv_obj_set_pos(name, 112, 14);
    lv_obj_set_width(name, row_width - 150);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const auto detail = SnapshotCowDetail(cow_status);
    auto *status = Label(row, detail.c_str(), &lv_font_montserrat_32,
        cow_ready ? kMuted : kMutedStrong);
    lv_obj_set_pos(status, 112, 76);
    lv_obj_set_width(status, row_width - 150);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    LayoutWipeRow(row, name, status, indicator);
    if (!cow_ready) {
      lv_obj_add_state(row, LV_STATE_DISABLED);
      lv_obj_set_style_opa(row, LV_OPA_50, LV_STATE_DISABLED);
    }
  }
  if (state->volumes.empty() && state->tool != Action::kWipe) {
    auto *label = Label(state->list,
        "No available partitions. Check storage and encryption state.",
        &lv_font_montserrat_32, kMuted);
    lv_obj_set_width(label, 1250);
    lv_obj_set_pos(label, 16, 50);
  }
  UpdateSelection(state);
}

void Review(Tools *state) {
  if (state->selected.empty()) {
    Sheet(state->screen, "Select partitions", "Choose at least one partition first.");
    return;
  }
  JobRequest request;
  request.job = state->tool == Action::kBackup ? Job::kBackup :
                state->tool == Action::kRestore ? Job::kRestore : Job::kWipe;
  request.title = state->tool == Action::kBackup ? "Backup" :
                  state->tool == Action::kRestore ? "Restore" : "Wipe";
  request.path = state->tool == Action::kRestore ? state->restore_folder : RecoveryStorage();
  request.compression = state->compression;
  request.partitions.assign(state->selected.begin(), state->selected.end());
  std::string text;
  if (request.job == Job::kBackup) {
    uint64_t bytes = 0;
    for (const auto &v : state->volumes) if (state->selected.count(v.path)) bytes += v.bytes;
    text = "Destination\n" + RecoveryBackupRoot() +
           "\n\nA new dated backup folder will be created.\n\nEstimated size: " + Size(bytes) +
           "\nBefore compression; actual archive size may differ.\n";
    if (RecoveryDataLocked()) text += "\nStorage is locked. This is not a decrypted user-data backup.\n";
  } else if (request.job == Job::kRestore) {
    text = "Source\n" + request.path +
           "\n\nRestoring overwrites the selected partitions.\n";
  } else text = "This permanently erases the selected contents.\n";
  text += "\nSelected partitions\n";
  for (const auto &v : state->volumes) {
    if (state->selected.count(v.path))
      text += "\n" + v.name + "  (" + v.path + ")";
  }
  if (request.job == Job::kWipe) {
    text += "\n\nWhat will be erased\n";
    for (const auto &v : state->volumes)
      if (state->selected.count(v.path)) text += "\n" + v.name + ": " + WipeDescription(v.path);
  }
  text += "\n\nActive slot: " + RecoverySlot();
  if (request.job == Job::kBackup)
    text += state->compression ? "\nCompression on. Digests included." :
                                "\nCompression off. Digests included.";
  if (request.job == Job::kRestore) text += "\nDigest verification enabled.";
  Sheet(state->screen, "Review " + request.title, text,
        [state, request] { Run(state, request); });
}

void RestoreFolders(Tools *state) {
  state->restore_folder.clear();
  state->selected.clear();
  state->volumes.clear();
  lv_obj_clean(state->list);
  if (state->review)
    LayoutPartitionReview(state->screen, state->list, state->review, false);
  const auto root = RecoveryBackupRoot();
  std::vector<std::string> folders;
  DIR *directory = opendir(root.c_str());
  if (directory) {
    while (auto *entry = readdir(directory)) {
      if (entry->d_name[0] == '.') continue;
      struct stat info{};
      const std::string path = root + "/" + entry->d_name;
      if (!stat(path.c_str(), &info) && S_ISDIR(info.st_mode)) folders.push_back(path);
    }
    closedir(directory);
  }
  std::sort(folders.rbegin(), folders.rend());
  const std::string summary =
      i18n::Format("%zu saved backups", folders.size());
  i18n::BindLabel(state->summary, summary.c_str());
  if (state->selection_detail) i18n::BindLabel(state->selection_detail, "Choose a backup, then select partitions to restore");
  for (size_t i = 0; i < folders.size(); ++i) {
    const auto path = folders[i];
    const auto name = path.substr(path.find_last_of('/') + 1);
    Row(state->list, static_cast<int>(i) * 190, LV_SYMBOL_SAVE,
        name, "Inspect available partitions", [state, path] {
      state->restore_folder = path;
      state->selected.clear();
      state->volumes = RecoveryRestoreVolumes(path);
      if (state->volumes.empty()) {
        Sheet(state->screen, "Backup unavailable",
              "No supported partitions were found. Encrypted backup archives "
              "need a separate archive-password workflow.\n\n" + path);
        state->restore_folder.clear();
        return;
      }
      PartitionRows(state);
    }, LV_SYMBOL_RIGHT, [state, path, name] {
      BackupActions(state, path, name);
    });
  }
  if (folders.empty()) {
    auto *empty = Label(state->list,
        ("No backups found here.\n\n" + root +
         "\n\nChoose another storage location or create a backup first.").c_str(),
        &lv_font_montserrat_32, kMuted);
    lv_obj_set_width(empty, 1240);
    lv_obj_set_pos(empty, 16, 48);
  }
}

void StorageChooser(Tools *state) {
  lv_obj_t *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  MainBackground(overlay);
  auto *heading = Label(overlay, "Choose storage", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 80, 250);
  auto *list = Scroll(overlay, 420, 2050);
  auto volumes = RecoveryVolumes("storage");
  for (size_t i = 0; i < volumes.size(); ++i) {
    const auto v = volumes[i];
    Row(list, static_cast<int>(i) * 190, LV_SYMBOL_DRIVE, v.name, v.path, [state, v] {
      if (!RecoverySetStorage(v.path)) {
        Sheet(state->screen, "Storage unavailable", "Could not mount " + v.path);
      } else Open(state, state->tool);
    });
  }
  auto *close = Button(overlay, "Back", [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_set_size(close, 1260, 140);
  lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -180);
}

void WipeTabs(Tools *state) {
  const bool format = state->tool == Action::kFormatData;
  const bool landscape = Landscape(state->screen);
  const int card_y = landscape ? 310 : 430;
  WorkflowModeCard(state->screen, 64, card_y, landscape ? 470 : 630,
                   LV_SYMBOL_LIST, "Wipe partitions",
                   "Choose individual partitions to erase.", !format,
                   [state] { Open(state, Action::kWipe); });
  WorkflowModeCard(state->screen, landscape ? 550 : 714, card_y,
                   landscape ? 470 : 662, LV_SYMBOL_TRASH, "Format Data",
                   "Erase internal storage and reset encryption.", format,
                   [state] { Open(state, Action::kFormatData); });
}

std::string SnapshotCowDetail(const SnapshotCowStatus &status) {
  if (!status.supported)
    return "Dynamic partitions are not available on this device.";
  if (!status.metadata_readable)
    return "Super metadata could not be read.";
  if (!status.safe_to_remove)
    return "Cleanup is locked while an Android snapshot update is active.";
  if (status.partitions.empty())
    return "No stale snapshot COW partitions were found.";
  return i18n::Format("%zu partitions / %s can be released",
                      status.partitions.size(), Size(status.bytes).c_str());
}

void ConfirmSnapshotCowCleanup(Tools *state) {
  const auto status = RecoverySnapshotCowStatus();
  if (!status.supported || !status.metadata_readable ||
      !status.safe_to_remove || status.partitions.empty()) {
    Sheet(state->screen, "Snapshot cleanup unavailable",
          SnapshotCowDetail(status));
    return;
  }

  std::string copy = i18n::Format(
      "Release %s used by these stale snapshot partitions?\n\n",
      Size(status.bytes).c_str());
  for (const auto &partition : status.partitions)
    copy += "- " + partition + "\n";
  copy += "\nRegular Android partitions and their data are not touched.";
  Sheet(state->screen, "Remove snapshot COWs?", copy,
        [state] {
          JobRequest request;
          request.job = Job::kClearSnapshotCow;
          request.title = "Snapshot COW cleanup";
          request.confirmation = "remove-cow";
          Run(state, request);
        }, 0, false, SheetPresentation::kStandard, "Swipe to remove COWs");
}

void UpdateFormatConfirmation(Tools *state) {
  const bool confirmed = std::string(lv_textarea_get_text(state->format_input)) == "yes";
  if (confirmed) lv_obj_remove_state(state->format_submit, LV_STATE_DISABLED);
  else lv_obj_add_state(state->format_submit, LV_STATE_DISABLED);
  auto *text = lv_obj_get_child(state->format_submit, 0);
  lv_obj_set_style_text_color(text, confirmed ? kText : kMuted, 0);
}

void BuildFormatData(Tools *state, bool fastboot_mode = false) {
  Header(state->screen, fastboot_mode ? "Fastbootd · Format Data" : "Wipe",
         fastboot_mode
             ? "Erase encrypted internal storage while userspace fastboot remains active."
             : "Erase selected partitions or format internal storage.",
         fastboot_mode ? nullptr : state->callback,
         fastboot_mode ? nullptr : state->context);
  if (!fastboot_mode) WipeTabs(state);
  const bool landscape = Landscape(state->screen);
  const int mode_offset = fastboot_mode ? 0 : (landscape ? 80 : 56);
  auto *title = Label(state->screen, "Erase all internal data", &lv_font_montserrat_48, kRed);
  lv_obj_set_pos(title, 80, (landscape ? 480 : 654) + mode_offset);
  auto *warning = Label(state->screen,
      "Formatting /data deletes apps, files, photos, videos\n"
      "and backups stored in internal storage.\n\n"
      "This cannot be undone.", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(warning, 80, (landscape ? 560 : 744) + mode_offset);
  lv_obj_set_width(warning, landscape ? 980 : 1270);
  lv_obj_set_style_text_line_space(warning, 12, 0);
  auto *encryption = Label(state->screen,
      "Resets storage encryption; Android may encrypt it again.\n"
      "Adopted storage, if present, may also be erased.", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(encryption, 80, (landscape ? 770 : 1040) + mode_offset);
  lv_obj_set_width(encryption, landscape ? 980 : 1270);
  lv_obj_set_style_text_line_space(encryption, 12, 0);
  auto *prompt = Label(state->screen, "Type yes to enable Format Data", &lv_font_montserrat_32, kText);
  lv_obj_set_pos(prompt, 80, (landscape ? 900 : 1228) + mode_offset);
  state->format_input = TextArea(state->screen);
  lv_obj_set_pos(state->format_input, 80,
                 (landscape ? 960 : 1310) + mode_offset);
  lv_obj_set_size(state->format_input, landscape ? 940 : 1280, 150);
  lv_textarea_set_one_line(state->format_input, true);
  // Don't truncate to three letters: "yesplease" must not become "yes".
  lv_textarea_set_max_length(state->format_input, 32);
  lv_textarea_set_text(state->format_input, "");
  lv_textarea_set_placeholder_text(state->format_input, "yes");
  lv_obj_set_style_text_font(state->format_input, UiFont(&lv_font_montserrat_48), 0);
  lv_obj_set_style_text_color(state->format_input, kText, 0);
  lv_obj_set_style_text_color(state->format_input, kMuted, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(state->format_input, kMainPanel, 0);
  lv_obj_set_style_bg_opa(state->format_input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(state->format_input, kMainLine, 0);
  lv_obj_set_style_border_color(state->format_input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(state->format_input, 2, 0);
  lv_obj_set_style_radius(state->format_input, 24, 0);
  lv_obj_set_style_pad_all(state->format_input, 32, 0);

  auto *keyboard = lv_keyboard_create(state->screen);
  phone_keyboard::Apply(keyboard);
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard, landscape ? 1100 : 48, landscape ? 330 : 1540);
  lv_obj_set_size(keyboard, landscape ? 2004 : 1344,
                  landscape ? 850 : 780);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(keyboard, state->format_input);

  state->format_submit = Button(state->screen, "Format data now", [state] {
    JobRequest request;
    request.job = Job::kFormatData; request.title = "Format Data"; request.path = "/data";
    request.confirmation = lv_textarea_get_text(state->format_input);
    if (!FormatDataAuthorized(request)) return;
    lv_textarea_set_text(state->format_input, "");
    Run(state, request);
  });
  lv_obj_set_pos(state->format_submit, 80,
                 (landscape ? 1140 : 2600) + (landscape ? mode_offset : 0));
  lv_obj_set_size(state->format_submit, landscape ? 450 : 1280, 132);
  lv_obj_set_style_bg_color(state->format_submit, kRed, 0);
  lv_obj_set_style_bg_color(state->format_submit, kRedSoft, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(state->format_submit, kMainPanel, LV_STATE_DISABLED);
  UpdateFormatConfirmation(state);
  lv_obj_add_event_cb(state->format_input, [](lv_event_t *event) {
    UpdateFormatConfirmation(static_cast<Tools *>(lv_event_get_user_data(event)));
  }, LV_EVENT_VALUE_CHANGED, state);
  // Neither typing nor the keyboard's Enter/Ready action starts a format.
  auto *cancel = Button(state->screen, "Cancel", [state, fastboot_mode] {
    Open(state, fastboot_mode ? Action::kBack : Action::kWipe);
  });
  lv_obj_set_pos(cancel, landscape ? 570 : 80,
                 (landscape ? 1140 : 2770) + (landscape ? mode_offset : 0));
  lv_obj_set_size(cancel, landscape ? 450 : 1280, landscape ? 132 : 112);
}

lv_obj_t *WorkflowModeCard(lv_obj_t *parent, int x, int y, int width,
                           const char *icon, const char *title,
                           const char *description, bool active,
                           Handler action) {
  auto *card = lv_button_create(parent);
  Panel(card, 34, kMainPanel);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, 220);
  lv_obj_set_style_border_width(
      card, active ? 3
                   : RecoverySurfaceStyle() == SurfaceStyle::kOutline ? 2 : 1,
      0);
  lv_obj_set_style_border_color(card, active ? kAccent : kMainLine, 0);
  lv_obj_set_style_border_opa(card, active ? LV_OPA_COVER : LV_OPA_50, 0);

  auto *symbol = Label(card, icon, &lv_font_montserrat_48, kAccent);
  lv_obj_set_pos(symbol, 34, 30);
  auto *heading = Label(card, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(heading, 112, 28);
  SingleLineLabel(heading, width - 176, &lv_font_montserrat_32);
  auto *copy = Label(card, description, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 34, 112);
  FitLabelToLines(copy, width - 68, 2,
                  {&lv_font_montserrat_24, &lv_font_montserrat_20,
                   &lv_font_montserrat_18});
  OnClick(card, std::move(action));
  return card;
}

void BuildPartitions(Tools *state) {
  const bool backup = state->tool == Action::kBackup;
  const bool restore = state->tool == Action::kRestore;
  Header(state->screen, backup || restore ? "Backups" : "Wipe",
         backup || restore ? "Create a backup or restore a saved one." :
         "Choose exactly what to erase.", state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->summary = Label(state->screen, "", &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(state->summary, 80, landscape ? 760 : 770);
  state->list = Scroll(state->screen, landscape ? 350 : 870,
                       landscape ? 900 : 1610);
  if (landscape) {
    lv_obj_set_x(state->list, 1100);
    lv_obj_set_width(state->list, 2004);
  }

  if (backup || restore) {
    const int card_y = landscape ? 310 : 430;
    WorkflowModeCard(state->screen, 64, card_y, landscape ? 470 : 630,
                     LV_SYMBOL_UPLOAD, "Create backup",
                     "Save selected partitions to recovery storage.", backup,
                     [state] { Open(state, Action::kBackup); });
    WorkflowModeCard(state->screen, landscape ? 550 : 714, card_y,
                     landscape ? 470 : 662, LV_SYMBOL_REFRESH,
                     "Restore backup", "Recover partitions from a saved backup.",
                     restore, [state] { Open(state, Action::kRestore); });

    auto *storage_heading = Label(state->screen,
        backup ? "BACKUP DESTINATION" : "BACKUP LIBRARY",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(storage_heading, 80, landscape ? 560 : 690);

    lv_obj_set_pos(state->summary, 80, landscape ? 800 : 930);
    lv_obj_set_style_text_font(state->summary, UiFont(&lv_font_montserrat_48), 0);
    state->selection_detail = Label(state->screen, "", &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(state->selection_detail, 80, landscape ? 870 : 1002);
    lv_obj_set_width(state->selection_detail, landscape ? 940 : 1280);
    lv_label_set_long_mode(state->selection_detail, LV_LABEL_LONG_DOT);
    if (!landscape) {
      lv_obj_set_pos(state->list, 64, 1070);
      lv_obj_set_height(state->list, 1434);
    }
    const std::string storage_action =
        std::string(i18n::Translate("Choose storage")) + "  " LV_SYMBOL_DOWN;
    auto *storage = Button(state->screen, storage_action.c_str(),
                            [state] { StorageChooser(state); });
    lv_obj_set_pos(storage, 64, landscape ? 610 : 732);
    lv_obj_set_size(storage, landscape ? 470 : 630, 104);
    std::string storage_text = RecoveryStorage();
    struct statvfs capacity{};
    if (statvfs(storage_text.c_str(), &capacity) == 0)
      storage_text = i18n::Format(
          "%s / %s free", storage_text.c_str(),
          Size(uint64_t(capacity.f_bavail) * capacity.f_frsize).c_str());
    auto *path = Label(state->screen, storage_text.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(path, 80, landscape ? 735 : 862);
    lv_obj_set_width(path, landscape ? 940 : 1260);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    if (backup) {
      auto *compression = Button(state->screen, state->compression ? "Compression: on" : "Compression: off", [] {});
      lv_obj_set_pos(compression, landscape ? 550 : 714,
                     landscape ? 610 : 732);
      lv_obj_set_size(compression, landscape ? 470 : 662, 104);
      auto *label = lv_obj_get_child(compression, 0);
      OnClick(compression, [state, label] {
        state->compression = !state->compression;
        i18n::BindLabel(label, state->compression ? "Compression: on" : "Compression: off");
        UpdateSelection(state);
      });
    } else {
      auto *folders = Button(state->screen, "Refresh backups", [state] { RestoreFolders(state); });
      lv_obj_set_pos(folders, landscape ? 550 : 714,
                     landscape ? 610 : 732);
      lv_obj_set_size(folders, landscape ? 470 : 662, 104);
    }
  } else {
    WipeTabs(state);
    auto *notice = Label(state->screen,
        "Only selected partitions will be wiped.", &lv_font_montserrat_32, kAmber);
    lv_obj_set_pos(notice, 80, landscape ? 570 : 690);
    lv_obj_set_width(notice, landscape ? 940 : 1250);
    auto *hint = Label(state->screen,
        "To erase all internal data and reset encryption, use Format Data.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(hint, 80, landscape ? 650 : 758);
    lv_obj_set_width(hint, landscape ? 940 : 1270);
    lv_obj_set_pos(state->summary, 80, landscape ? 760 : 866);
    if (!landscape) {
      lv_obj_set_pos(state->list, 64, 948);
      lv_obj_set_height(state->list, 1650);
    }
  }
  auto *review = Button(state->screen, backup ? "Review backup" :
                        restore ? "Review restore" : "Review wipe",
                        [state] { Review(state); }, true);
  state->review = review;
  LayoutPartitionReview(state->screen, state->list, review, false);
  if (restore) RestoreFolders(state);
  else {
    state->volumes = RecoveryVolumes(backup ? "backup" : "wipe");
    PartitionRows(state);
  }
}

void BuildMounts(Tools *state) {
  Header(state->screen, "Mounts", "Manage the device's recovery volumes.", state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 510,
                       landscape ? 900 : 2260);
  lv_obj_update_layout(state->list);
  const int row_width = std::max(600, static_cast<int>(lv_obj_get_width(state->list)));

  std::vector<Volume> mounted;
  std::vector<Volume> available;
  const auto mount_volumes = RecoveryVolumes("mount");
  const bool has_data = std::any_of(
      mount_volumes.begin(), mount_volumes.end(),
      [](const Volume &volume) { return volume.path == "/data"; });
  for (const auto &volume : mount_volumes) {
    // /storage is a legacy bind alias for data/media. The usable internal
    // storage is already exposed through /sdcard when /data is mounted, so a
    // second independent-looking row reports a misleading state.
    if (has_data && volume.path == "/storage") continue;
    (volume.selected ? mounted : available).push_back(volume);
  }

  int y = 0;
  const auto add_group = [state, row_width, &y](
      const char *heading, const std::vector<Volume> &volumes, bool is_mounted) {
    const std::string title = std::string(i18n::Translate(heading)) + "  " +
        std::to_string(volumes.size());
    auto *section = Label(state->list, title.c_str(), &lv_font_montserrat_28,
                          is_mounted ? kAccent : kMutedStrong);
    lv_obj_set_pos(section, 24, y + 12);
    lv_obj_set_width(section, row_width - 48);
    y += 72;

    for (const auto &volume : volumes) {
      auto *row = Button(state->list, "", [state, volume] {
        JobRequest request;
        request.job = volume.selected ? Job::kUnmount : Job::kMount;
        request.path = volume.path;
        request.title = volume.selected ? "Unmount volume" : "Mount volume";
        Sheet(state->screen, request.title, volume.name + "\n\n" + volume.path +
              "\n\nRecovery's configured mount and read-only rules apply.",
              [state, request] { Run(state, request); });
      });
      lv_obj_set_pos(row, 0, y);
      lv_obj_set_size(row, row_width, 166);
      lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
      lv_obj_set_style_radius(row, 20, 0);
      lv_obj_set_style_bg_color(row, kMainPanel, 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_40, 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_border_color(row, is_mounted ? kAccent : kMainLine, 0);
      lv_obj_set_style_border_opa(row, is_mounted ? LV_OPA_40 : LV_OPA_30, 0);

      auto *rail = lv_obj_create(row);
      Clear(rail);
      lv_obj_remove_flag(rail, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_pos(rail, 0, 20);
      lv_obj_set_size(rail, 6, 126);
      lv_obj_set_style_radius(rail, 3, 0);
      lv_obj_set_style_bg_color(rail, is_mounted ? kAccent : kMainLine, 0);
      lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);

      auto *icon = Label(row, LV_SYMBOL_DRIVE, &lv_font_montserrat_32,
                         is_mounted ? kAccent : kMutedStrong);
      lv_obj_set_pos(icon, 32, 65);

      auto *name = Label(row, volume.name.c_str(), &lv_font_montserrat_32, kText);
      lv_obj_set_pos(name, 112, 29);
      SingleLineLabel(name, row_width - 520, &lv_font_montserrat_32);

      auto *path = Label(row, volume.path.c_str(), &lv_font_montserrat_24,
                         is_mounted ? kMutedStrong : kMuted);
      lv_obj_set_pos(path, 112, 94);
      SingleLineLabel(path, row_width - 520, &lv_font_montserrat_24);

      auto *status = Label(row, is_mounted ? "Mounted" : "Not mounted",
                           &lv_font_montserrat_24,
                           is_mounted ? kAccent : kMutedStrong);
      SingleLineLabel(status, 220, &lv_font_montserrat_24);
      lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_align(status, LV_ALIGN_RIGHT_MID, -154, 0);

      auto *toggle = lv_switch_create(row);
      lv_obj_set_size(toggle, 104, 58);
      lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -24, 0);
      lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_bg_color(toggle, kMainLine, LV_PART_MAIN);
      lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_color(toggle, kAccent,
                               LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB);
      lv_obj_set_style_bg_color(toggle, kText, LV_PART_KNOB);
      lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
      lv_obj_set_style_pad_all(toggle, -8, LV_PART_KNOB);
      if (is_mounted) lv_obj_add_state(toggle, LV_STATE_CHECKED);

      y += 182;
    }
    y += 18;
  };

  add_group("Mounted", mounted, true);
  add_group("Not mounted", available, false);
}

void PreferenceColumn(lv_obj_t *object, int gap = 12) {
  lv_obj_set_width(object, LV_PCT(100));
  lv_obj_set_height(object, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(object, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(object, gap, 0);
}

lv_obj_t *PreferenceEntry(Tools *state, const char *symbol,
                          const char *title, const std::string &description,
                          Handler action, bool toggle = false) {
  auto *row = lv_button_create(state->list);
  Clear(row);
  lv_obj_set_style_bg_color(row, kMainSelected, LV_STATE_PRESSED);
  if (action) OnClick(row, std::move(action));
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(row, 154, 0);
  lv_obj_set_style_pad_all(row, 24, 0);
  lv_obj_set_style_pad_column(row, 28, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(row, kMainLine, 0);
  lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
  lv_obj_set_style_radius(row, 0, 0);
  lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                       LV_FLEX_ALIGN_CENTER);
  auto *icon = Label(row, symbol, &lv_font_montserrat_32, kAccent);
  lv_obj_set_width(icon, 56);
  auto *text = lv_obj_create(row);
  Clear(text);
  lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE);
  PreferenceColumn(text, 10);
  lv_obj_set_flex_grow(text, 1);
  auto *name = Label(text, title, &lv_font_montserrat_32, kText);
  lv_obj_set_width(name, LV_PCT(100));
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
  if (!description.empty()) {
    auto *copy = Label(text, description.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_width(copy, LV_PCT(100));
    lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);
  }
  if (!toggle) Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_28, kMuted);
  return row;
}

void PreferenceToggle(Tools *state, const char *symbol, const char *title,
                      const char *description, Preference preference) {
  auto *row = PreferenceEntry(state, symbol, title, description, {}, true);
  auto *toggle = lv_switch_create(row);
  StyleSwitch(toggle, RecoveryPreference(preference));
  OnClick(row, [state, toggle, preference] {
    const bool enabled = !RecoveryPreference(preference);
    if (!RecoverySetPreference(preference, enabled)) {
      Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
      return;
    }
    if (enabled) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    AutoSaveSettings(state);
  });
}

void PreferenceSection(Tools *state, const char *title) {
  auto *label = Label(state->list, title, &lv_font_montserrat_32, kAccent);
  lv_obj_set_width(label, LV_PCT(100));
  lv_obj_set_style_pad_top(label, 32, 0);
  lv_obj_set_style_pad_bottom(label, 14, 0);
  lv_obj_set_style_pad_left(label, 24, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

void HapticValue(lv_obj_t *label, int milliseconds) {
  i18n::BindLabel(label, milliseconds == 0 ? "Off" :
      (std::to_string(milliseconds) + " ms").c_str());
}

void HapticSlider(Tools *state, lv_obj_t *parent, const char *title,
                  const char *description, Haptic haptic, int maximum) {
  auto *card = lv_obj_create(parent);
  Clear(card);
  PreferenceColumn(card, 18);
  lv_obj_set_style_pad_all(card, 24, 0);
  auto *name = Label(card, title, &lv_font_montserrat_32, kText);
  lv_obj_set_width(name, LV_PCT(100));
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
  auto *copy = Label(card, description, &lv_font_montserrat_24, kMuted);
  lv_obj_set_width(copy, LV_PCT(100));
  lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);
  auto *controls = lv_obj_create(card);
  Clear(controls);
  lv_obj_set_width(controls, LV_PCT(100));
  lv_obj_set_height(controls, 104);
  lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                       LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(controls, 32, 0);
  auto *value = Label(controls, "", &lv_font_montserrat_24, kAccent);
  lv_obj_set_width(value, 180);
  HapticValue(value, RecoveryHapticDuration(haptic));
  auto *slider = lv_slider_create(controls);
  lv_obj_set_height(slider, 24);
  lv_obj_set_flex_grow(slider, 1);
  lv_slider_set_range(slider, 0, maximum);
  lv_slider_set_value(slider, RecoveryHapticDuration(haptic), LV_ANIM_OFF);
  RangeSlider(slider);
  struct Binding {
    lv_obj_t *value;
    Haptic haptic;
    Tools *state;
  };
  auto *binding = new Binding{value, haptic, state};
  lv_obj_add_event_cb(slider, [](lv_event_t *event) {
    auto *binding = static_cast<Binding *>(lv_event_get_user_data(event));
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
      delete binding;
      return;
    }
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
    const int duration = lv_slider_get_value(lv_event_get_target_obj(event));
    HapticValue(binding->value, duration);
    if (code == LV_EVENT_RELEASED) {
      if (RecoverySetHapticDuration(binding->haptic, duration)) {
        AutoSaveSettings(binding->state);
        RecoveryVibrate(binding->haptic);
      }
    }
  }, LV_EVENT_ALL, binding);
  auto *reset = Button(controls, LV_SYMBOL_REFRESH, [state, slider, value, haptic] {
    const int duration = RecoveryDefaultHapticDuration(haptic);
    if (!RecoverySetHapticDuration(haptic, duration)) return;
    lv_slider_set_value(slider, duration, LV_ANIM_OFF);
    HapticValue(value, duration);
    AutoSaveSettings(state);
    RecoveryVibrate(haptic);
  });
  lv_obj_set_size(reset, 104, 104);
}

struct AccentPreset {
  const char *name;
  uint32_t rgb;
};

constexpr std::array<AccentPreset, 7> kAccentPresets{{
    {"AERA Cyan", 0x16c8ff}, {"Azure", 0x4c8dff},
    {"Violet", 0xa991ff}, {"Magenta", 0xf05dce},
    {"Lime", 0xa6e35a}, {"Orange", 0xff8a32},
    {"Emerald", 0x42d392},
}};

bool IsPresetAccent(uint32_t rgb) {
  return std::any_of(kAccentPresets.begin(), kAccentPresets.end(),
      [rgb](const AccentPreset &preset) { return preset.rgb == rgb; });
}

const char *AccentName(uint32_t rgb) {
  for (const auto &preset : kAccentPresets)
    if (preset.rgb == rgb) return preset.name;
  return "User selected";
}

lv_obj_t *HueSaturationDisk(lv_obj_t *parent, int size) {
  auto *pixels = new std::vector<lv_color32_t>(
      static_cast<size_t>(size) * static_cast<size_t>(size));
  constexpr double kPi = 3.14159265358979323846;
  const double center = (size - 1) * 0.5;
  const double radius = center - 1.0;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const double dx = x - center;
      const double dy = y - center;
      const double distance = std::sqrt(dx * dx + dy * dy);
      auto &pixel = (*pixels)[static_cast<size_t>(y) * size + x];
      if (distance > radius + 1.0) {
        pixel = lv_color32_make(0, 0, 0, 0);
        continue;
      }
      double degrees = std::atan2(dy, dx) * 180.0 / kPi;
      if (degrees < 0.0) degrees += 360.0;
      const uint8_t saturation = static_cast<uint8_t>(std::clamp(
          distance * 100.0 / radius, 0.0, 100.0));
      const lv_color_t color = lv_color_hsv_to_rgb(
          static_cast<uint16_t>(degrees), saturation, 100);
      const lv_opa_t alpha = distance <= radius ? LV_OPA_COVER
          : static_cast<lv_opa_t>(std::clamp(
                (radius + 1.0 - distance) * 255.0, 0.0, 255.0));
      pixel = lv_color32_make(color.red, color.green, color.blue, alpha);
    }
  }

  auto *canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(canvas, pixels->data(), size, size,
                       LV_COLOR_FORMAT_ARGB8888);
  lv_obj_set_size(canvas, size, size);
  lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, [](lv_event_t *event) {
    delete static_cast<std::vector<lv_color32_t> *>(
        lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, pixels);
  return canvas;
}

struct AccentPickerState {
  std::function<void(uint32_t)> selected;
  Tools *tools = nullptr;
  lv_obj_t *overlay = nullptr;
  lv_obj_t *wheel = nullptr;
  lv_obj_t *marker = nullptr;
  lv_obj_t *preview = nullptr;
  lv_obj_t *hex = nullptr;
  uint32_t rgb = kDefaultAccentRgb;
  int wheel_size = 0;
};

void UpdateAccentPicker(AccentPickerState *picker, double dx, double dy) {
  constexpr double kPi = 3.14159265358979323846;
  const double radius = picker->wheel_size * 0.5 - 3.0;
  double distance = std::sqrt(dx * dx + dy * dy);
  if (distance > radius && distance > 0.0) {
    dx *= radius / distance;
    dy *= radius / distance;
    distance = radius;
  }
  double degrees = std::atan2(dy, dx) * 180.0 / kPi;
  if (degrees < 0.0) degrees += 360.0;
  const uint8_t saturation = static_cast<uint8_t>(std::clamp(
      distance * 100.0 / radius, 0.0, 100.0));
  const lv_color_t color = lv_color_hsv_to_rgb(
      static_cast<uint16_t>(degrees), saturation, 100);
  picker->rgb = (static_cast<uint32_t>(color.red) << 16) |
                (static_cast<uint32_t>(color.green) << 8) |
                static_cast<uint32_t>(color.blue);

  const int marker_size = 46;
  lv_obj_set_pos(picker->marker,
      static_cast<int>(picker->wheel_size * 0.5 + dx) - marker_size / 2,
      static_cast<int>(picker->wheel_size * 0.5 + dy) - marker_size / 2);
  lv_obj_set_style_bg_color(picker->marker, color, 0);
  lv_obj_set_style_bg_color(picker->preview, color, 0);
  lv_obj_set_style_text_color(picker->hex,
      lv_color_luminance(color) < 118 ? Color(0xffffff) : Color(0x101318), 0);
  char value[16];
  std::snprintf(value, sizeof(value), "#%06X", picker->rgb);
  i18n::BindLabel(picker->hex, value);
}

void UpdateAccentPickerFromTouch(AccentPickerState *picker) {
  lv_indev_t *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);
  lv_area_t area{};
  lv_obj_get_coords(picker->wheel, &area);
  UpdateAccentPicker(picker,
      point.x - area.x1 - picker->wheel_size * 0.5,
      point.y - area.y1 - picker->wheel_size * 0.5);
}

void OpenAccentPicker(Tools *tools, std::function<void(uint32_t)> selected = {},
                      uint32_t initial = kDefaultAccentRgb) {
  constexpr double kPi = 3.14159265358979323846;
  auto *picker = new AccentPickerState;
  picker->tools = tools;
  picker->selected = std::move(selected);
  picker->rgb = picker->selected ? initial : RecoveryAccentColor();
  picker->overlay = lv_obj_create(tools->screen);
  lv_obj_set_user_data(picker->overlay, &kModalMarker);
  Clear(picker->overlay);
  lv_obj_set_size(picker->overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(picker->overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(picker->overlay, LV_OPA_70, 0);
  lv_obj_add_event_cb(picker->overlay, [](lv_event_t *event) {
    delete static_cast<AccentPickerState *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, picker);

  const bool landscape = Landscape(tools->screen);
  const int screen_width = lv_obj_get_width(tools->screen);
  const int screen_height = lv_obj_get_height(tools->screen);
  const int sheet_width = landscape ? std::min(2100, screen_width - 160) : 1312;
  const int sheet_height = landscape ? std::min(1120, screen_height - 100) : 1450;
  auto *sheet = lv_obj_create(picker->overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_center(sheet);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);

  auto *heading = Label(sheet, picker->selected ? "Choose clock colour" : "Choose your accent",
                        &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 56, 48);
  auto *copy = Label(sheet,
      "Drag anywhere on the colour disk. The centre is softer; the edge is vivid.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 56, 116);
  lv_obj_set_width(copy, sheet_width - 112);

  picker->wheel_size = landscape ? 680 : 760;
  auto *wheel_holder = lv_obj_create(sheet);
  Clear(wheel_holder);
  lv_obj_set_size(wheel_holder, picker->wheel_size, picker->wheel_size);
  lv_obj_set_pos(wheel_holder, landscape ? 70 : (sheet_width - picker->wheel_size) / 2,
                 landscape ? 232 : 208);
  lv_obj_set_style_radius(wheel_holder, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(wheel_holder, kMainPanel, 0);
  lv_obj_set_style_bg_opa(wheel_holder, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_color(wheel_holder, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(wheel_holder, 30, 0);
  lv_obj_set_style_shadow_opa(wheel_holder, LV_OPA_30, 0);
  picker->wheel = HueSaturationDisk(wheel_holder, picker->wheel_size);
  lv_obj_set_pos(picker->wheel, 0, 0);

  picker->marker = lv_obj_create(wheel_holder);
  Clear(picker->marker);
  lv_obj_set_size(picker->marker, 46, 46);
  lv_obj_set_style_radius(picker->marker, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(picker->marker, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(picker->marker, 6, 0);
  lv_obj_set_style_border_color(picker->marker, Color(0xffffff), 0);
  lv_obj_set_style_shadow_color(picker->marker, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(picker->marker, 10, 0);
  lv_obj_set_style_shadow_opa(picker->marker, LV_OPA_50, 0);
  lv_obj_remove_flag(picker->marker, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(wheel_holder, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wheel_holder, [](lv_event_t *event) {
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)
      UpdateAccentPickerFromTouch(static_cast<AccentPickerState *>(
          lv_event_get_user_data(event)));
  }, LV_EVENT_ALL, picker);

  const int controls_x = landscape ? 850 : 80;
  const int controls_y = landscape ? 286 : 1012;
  picker->preview = lv_obj_create(sheet);
  Clear(picker->preview);
  lv_obj_set_pos(picker->preview, controls_x, controls_y);
  lv_obj_set_size(picker->preview, landscape ? sheet_width - controls_x - 70
                                             : sheet_width - 160, 126);
  lv_obj_set_style_radius(picker->preview, 38, 0);
  lv_obj_set_style_bg_opa(picker->preview, LV_OPA_COVER, 0);
  picker->hex = Label(picker->preview, "", &lv_font_montserrat_32,
                      Color(0x101318));
  lv_obj_center(picker->hex);

  const int buttons_y = landscape ? 684 : 1182;
  const int button_width = landscape ? (sheet_width - controls_x - 94) / 2
                                     : (sheet_width - 184) / 2;
  auto *cancel = Button(sheet, "Cancel", [picker] {
    lv_obj_delete_async(picker->overlay);
  });
  lv_obj_set_pos(cancel, controls_x, buttons_y);
  lv_obj_set_size(cancel, button_width, 126);
  auto *use = Button(sheet, "Use colour", [picker] {
    const uint32_t rgb = picker->rgb;
    if (picker->selected) {
      picker->selected(rgb);
      lv_obj_delete_async(picker->overlay);
      return;
    }
    if (!RecoverySetAccentColor(rgb)) {
      Sheet(picker->tools->screen, "Theme unavailable",
            "The selected accent could not be stored.");
      return;
    }
    ApplyAccent(rgb);
    RefreshTheme(picker->tools);
  }, true);
  lv_obj_set_pos(use, controls_x + button_width + 24, buttons_y);
  lv_obj_set_size(use, button_width, 126);

  const lv_color_hsv_t hsv = lv_color_to_hsv(Color(picker->rgb));
  const double angle = hsv.h * kPi / 180.0;
  const double radius = (picker->wheel_size * 0.5 - 3.0) * hsv.s / 100.0;
  UpdateAccentPicker(picker, std::cos(angle) * radius,
                     std::sin(angle) * radius);
  AnimateEnter(sheet, 0, 18);
}

// Keep the preview and the real lock screen on the same renderer.
struct LockEditor {
  Tools *tools;
  lv_obj_t *preview;
  LockTheme theme;
  void Refresh() {
    lv_obj_clean(preview);
    BuildLockScene(preview, nullptr, nullptr, true, &theme);
  }
  void Save() {
    if (RecoverySetLockTheme(theme)) AutoSaveSettings(tools);
  }
};

void EditLockText(const std::shared_ptr<LockEditor> &editor, bool colour) {
  auto *overlay = lv_obj_create(editor->tools->screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  auto *title = Label(overlay, colour ? "Custom clock colour" : "Owner text",
                      &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 64, 100);
  auto *input = TextArea(overlay);
  lv_obj_set_pos(input, 64, 220); lv_obj_set_size(input, lv_obj_get_width(editor->tools->screen)-128, 120);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_max_length(input, colour ? 6 : 40);
  char hex[8]; snprintf(hex, sizeof(hex), "%06X", editor->theme.rgb);
  lv_textarea_set_text(input, colour ? hex : editor->theme.owner.c_str());
  if (colour) lv_textarea_set_accepted_chars(input, "0123456789abcdefABCDEF");
  auto *keyboard = lv_keyboard_create(overlay);
  phone_keyboard::Apply(keyboard);
  lv_keyboard_set_textarea(keyboard, input);
  auto *save = Button(overlay, "Apply", [editor, overlay, input, colour] {
    const std::string text = lv_textarea_get_text(input);
    if (colour) {
      if (text.size()!=6) return;
      editor->theme.rgb = static_cast<uint32_t>(strtoul(text.c_str(), nullptr, 16));
      editor->theme.colour = 2;
    } else editor->theme.owner = text;
    editor->Save(); editor->Refresh(); lv_obj_delete_async(overlay);
  });
  lv_obj_set_pos(save, 64, 390); lv_obj_set_size(save, 580, 112);
  auto *cancel = Button(overlay, "Cancel", [overlay]{lv_obj_delete_async(overlay);});
  lv_obj_set_pos(cancel, 680, 390); lv_obj_set_size(cancel, 580, 112);
}

int BuildLockEditor(Tools *tools, int top) {
  auto *section = lv_obj_create(tools->list); Clear(section);
  lv_obj_set_pos(section, 16, top); lv_obj_set_width(section, 1280);
  auto *heading = Label(section, "Lock screen", &lv_font_montserrat_40, kText);
  lv_obj_set_pos(heading, 0, 0);
  auto *copy = Label(section, "Personalise your lock screen. Storage unlock is unchanged.", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 0, 66); lv_obj_set_width(copy, 1280);
  auto *preview = lv_obj_create(section); Clear(preview);
  const int pw = 520, ph = pw * 3168 / 1440;
  lv_obj_set_size(preview, pw, ph); lv_obj_set_pos(preview, (1280-pw)/2, 130);
  lv_obj_set_style_radius(preview, 44, 0); lv_obj_set_style_clip_corner(preview, true, 0);
  lv_obj_set_style_border_width(preview, 3, 0);lv_obj_set_style_border_color(preview, kMainLine, 0);
  lv_obj_set_style_border_post(preview, true, 0);
  lv_obj_update_layout(preview);
  auto editor = std::make_shared<LockEditor>(LockEditor{tools, preview, RecoveryLockTheme()});
  editor->Refresh();
  int y = 130 + ph + 60;
  auto *wallpaper = Button(section, "Choose lock-screen wallpaper", [tools]{OpenWallpaperPicker(tools,{},true);});
  Panel(wallpaper,24,kMainPanel);Interactive(wallpaper,kMainSelected);
  lv_obj_set_pos(wallpaper,0,y);lv_obj_set_size(wallpaper,820,108);
  auto *reset = Button(section,"Use main wallpaper",[editor]{editor->theme.wallpaper.clear();editor->Save();editor->Refresh();});
  Panel(reset,24,kMainPanel);Interactive(reset,kMainSelected);
  lv_obj_set_pos(reset,840,y);lv_obj_set_size(reset,440,108);y+=156;
  auto range = [&](const char *title,int LockTheme::*member,int maximum,int minimum=0) {
    auto *card=lv_obj_create(section);Panel(card,32,kMainPanel);
    lv_obj_set_pos(card,0,y);lv_obj_set_size(card,1280,220);
    auto *label=Label(card,title,&lv_font_montserrat_32,kText);lv_obj_set_pos(label,36,24);
    auto *copy=Label(card,member==&LockTheme::date_size?"Resize the date independently of the clock":member==&LockTheme::clock_size?"Resize your chosen clock style":member==&LockTheme::dim?"Darken the wallpaper behind the clock":"Soften the wallpaper without blurring the clock",&lv_font_montserrat_24,kMuted);
    lv_obj_set_pos(copy,36,76);
    auto *amount=Label(card,"",&lv_font_montserrat_24,kAccent);
    lv_label_set_text_fmt(amount,"%d%%",editor->theme.*member);
    lv_obj_align(amount,LV_ALIGN_TOP_RIGHT,-38,34);
    auto *slider=lv_slider_create(card);RangeSlider(slider);
    lv_obj_set_pos(slider,52,166);lv_obj_set_size(slider,1176,20);
    lv_slider_set_range(slider,minimum,maximum);lv_slider_set_value(slider,editor->theme.*member,LV_ANIM_OFF);
    struct Binding {std::shared_ptr<LockEditor> editor;int LockTheme::*member;lv_obj_t *amount;};
    auto *binding=new Binding{editor,member,amount};
    lv_obj_add_event_cb(slider,[](lv_event_t *e){
      auto *b=static_cast<Binding *>(lv_event_get_user_data(e));
      auto code=lv_event_get_code(e);
      if(code==LV_EVENT_DELETE){delete b;return;}
      if(code==LV_EVENT_VALUE_CHANGED) {
        b->editor->theme.*(b->member)=lv_slider_get_value(lv_event_get_target_obj(e));
        lv_label_set_text_fmt(b->amount,"%d%%",b->editor->theme.*(b->member));
        b->editor->Refresh();
      }
      if(code==LV_EVENT_RELEASED) b->editor->Save();
    },LV_EVENT_ALL,binding);
    y+=240;
  };
  range("Wallpaper dimming",&LockTheme::dim,90);
  range("Wallpaper blur",&LockTheme::blur,100);
  auto choices = [&](const char *title, int LockTheme::*member, std::initializer_list<const char *> names) {
    auto *label = Label(section, title, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(label, 0, y); y+=64;
    auto buttons = std::make_shared<std::vector<lv_obj_t *>>();
    const int count = names.size(), width = (1280-(count-1)*16)/count;
    int index=0;
    for (auto name : names) {
      const int selected=index++;
      auto *button = Button(section, name, [editor, member, selected, buttons] {
        editor->theme.*member=selected;
        for (size_t i=0;i<buttons->size();++i) {
          lv_obj_set_style_border_color((*buttons)[i], i==static_cast<size_t>(selected)?kAccent:kMainLine,0);
          lv_obj_set_style_border_width((*buttons)[i], i==static_cast<size_t>(selected)?3:RecoverySurfaceStyle()==SurfaceStyle::kOutline?2:1,0);
        }
        editor->Save(); editor->Refresh();
      });
      Panel(button,24,kMainPanel); Interactive(button,kMainSelected);
      lv_obj_set_pos(button, selected*(width+16), y);lv_obj_set_size(button, width, 108);
      lv_obj_set_style_border_width(button, editor->theme.*member==selected?3:RecoverySurfaceStyle()==SurfaceStyle::kOutline?2:1,0);
      lv_obj_set_style_border_color(button, editor->theme.*member==selected?kAccent:kMainLine,0);
      buttons->push_back(button);
    }
    y+=156;
  };
  choices("Clock style", &LockTheme::clock, {"Classic","Analogue","Stacked"});
  choices("Clock position", &LockTheme::placement, {"Top","Centre","Bottom"});
  range("Clock size",&LockTheme::clock_size,180,60);
  auto *colour_title=Label(section,"Clock colour",&lv_font_montserrat_32,kText);
  lv_obj_set_pos(colour_title,0,y);y+=64;
  auto colour_cards=std::make_shared<std::vector<lv_obj_t *>>();
  auto update_colours=[editor,colour_cards] {
    for(size_t i=0;i<colour_cards->size();++i) {
      bool active=editor->theme.colour==static_cast<int>(i);
      auto *card=(*colour_cards)[i];
      lv_obj_set_style_border_width(card,active?4:RecoverySurfaceStyle()==SurfaceStyle::kOutline?2:1,0);
      lv_obj_set_style_border_color(card,active?kAccent:kMainLine,0);
    }
  };
  for(int i=0;i<3;++i) {
    auto *card=lv_button_create(section);Panel(card,32,kMainPanel);Interactive(card,kMainSelected);
    lv_obj_set_pos(card,(i%4)*320,y+(i/4)*220);lv_obj_set_size(card,304,190);
    colour_cards->push_back(card);
    const uint32_t rgb=i==0?RecoveryAccentColor():i==1?0xffffff:editor->theme.rgb;
    auto *dot=i==2?HueSaturationDisk(card,72):lv_obj_create(card);
    if(i!=2) {Clear(dot);lv_obj_set_size(dot,72,72);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);
      lv_obj_set_style_bg_color(dot,Color(rgb),0);lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);}
    lv_obj_set_pos(dot,24,24);lv_obj_remove_flag(dot,LV_OBJ_FLAG_CLICKABLE);
    auto *name=Label(card,i==0?"Follow accent":i==1?"White":"User select",&lv_font_montserrat_24,kText);
    lv_obj_set_pos(name,24,116);
    OnClick(card,[editor,i,rgb,update_colours] {
      if(i==2) {
        OpenAccentPicker(editor->tools,[editor,update_colours](uint32_t colour) {
          editor->theme.colour=2;editor->theme.rgb=colour;editor->Save();editor->Refresh();update_colours();
        },editor->theme.rgb);
      } else {
        editor->theme.colour=i<2?i:2;editor->theme.rgb=rgb;
        editor->Save();editor->Refresh();update_colours();
      }
    });
  }
  update_colours();y+=220+16;
  choices("Date style",&LockTheme::date_style,{"Full","Short","Numeric","Stacked","Hidden"});
  range("Date size",&LockTheme::date_size,200,60);
  choices("Date colour",&LockTheme::date_colour,{"White","Clock colour","Accent"});
  choices("Date position",&LockTheme::date_placement,{"Below clock","Above clock","Bottom"});
  auto *aod=lv_button_create(section);Panel(aod,32,kMainPanel);Interactive(aod,kMainSelected);
  lv_obj_set_pos(aod,0,y);lv_obj_set_size(aod,1280,editor->theme.aod?320:160);
  auto *aod_label=Label(aod,"Always-on clock",&lv_font_montserrat_32,kText);lv_obj_set_pos(aod_label,36,24);
  auto *aod_hint=Label(aod,"Dim clock on black. Uses more power than screen off. Wake with Power.",&lv_font_montserrat_24,kMuted);
  lv_obj_set_pos(aod_hint,36,88);lv_obj_set_width(aod_hint,1120);
  auto *aod_switch=lv_switch_create(aod);StyleSwitch(aod_switch,editor->theme.aod);lv_obj_align(aod_switch,LV_ALIGN_TOP_RIGHT,-36,24);
  // Only the heading toggles AOC; dragging its brightness must not toggle it.
  auto *aod_header=lv_obj_create(aod);Clear(aod_header);lv_obj_set_size(aod_header,1280,150);
  lv_obj_add_flag(aod_header,LV_OBJ_FLAG_CLICKABLE);
  OnClick(aod_header,[editor]{editor->theme.aod=!editor->theme.aod;editor->Save();RefreshTheme(editor->tools);});
  if(editor->theme.aod) {
    auto *title=Label(aod,"AOC brightness",&lv_font_montserrat_28,kText);lv_obj_set_pos(title,36,165);
    auto *amount=Label(aod,"",&lv_font_montserrat_24,kAccent);
    lv_label_set_text_fmt(amount,"%d%%",editor->theme.aod_brightness);lv_obj_align(amount,LV_ALIGN_TOP_RIGHT,-38,165);
    auto *slider=lv_slider_create(aod);RangeSlider(slider);lv_obj_set_pos(slider,52,254);lv_obj_set_size(slider,1176,20);
    lv_slider_set_range(slider,1,100);lv_slider_set_value(slider,editor->theme.aod_brightness,LV_ANIM_OFF);
    struct AocBinding {std::shared_ptr<LockEditor> editor;lv_obj_t *amount;};
    auto *binding=new AocBinding{editor,amount};
    lv_obj_add_event_cb(slider,[](lv_event_t *e){
      auto *b=static_cast<AocBinding *>(lv_event_get_user_data(e));
      if(lv_event_get_code(e)==LV_EVENT_DELETE){delete b;return;}
      if(lv_event_get_code(e)==LV_EVENT_VALUE_CHANGED){
        b->editor->theme.aod_brightness=lv_slider_get_value(lv_event_get_target_obj(e));
        lv_label_set_text_fmt(b->amount,"%d%%",b->editor->theme.aod_brightness);
      }
      if(lv_event_get_code(e)==LV_EVENT_RELEASED)b->editor->Save();
    },LV_EVENT_ALL,binding);
  }
  y+=editor->theme.aod?344:184;
  choices("Unlock gesture", &LockTheme::unlock, {"Slider","Swipe up","Hold"});
  choices("Slide / hold unlock animation", &LockTheme::animation, {"None","Fade"});
  auto *owner=Button(section,"Edit owner text",[editor]{EditLockText(editor,false);});
  Panel(owner,24,kMainPanel);Interactive(owner,kMainSelected);
  lv_obj_set_pos(owner,0,y);lv_obj_set_size(owner,1280,108);y+=156;
  lv_obj_set_height(section,y);
  for (uint32_t i=0;i<lv_obj_get_child_count(section);++i) {
    auto *child=lv_obj_get_child(section,i);
    if (lv_obj_check_type(child,&lv_button_class)) lv_obj_set_style_radius(child,24,0);
  }
  return top+y+48;
}

void BuildTheme(Tools *state) {
  Header(state->screen, "Theme Engine",
         "Choose wallpaper, appearance, surfaces, density and accent for every AERA page.", state->callback,
         state->context);
  constexpr int kSurfaceSectionOffset = 930;
  const bool landscape = Landscape(state->screen);
  const int list_y = landscape ? 340 : 460;
  const int dock_gap = landscape ? 20 : 28;
  const int list_height = std::max(
      640, static_cast<int>(lv_obj_get_height(state->screen)) - list_y -
               NavigationHeight(state->screen) - dock_gap);
  state->list = Scroll(state->screen, list_y, list_height);
  if (landscape) {
    lv_obj_set_x(state->list, 884);
    lv_obj_set_width(state->list, 1400);
  }

  auto *preview = lv_obj_create(state->list);
  Panel(preview, 42, kMainPanel);
  lv_obj_set_pos(preview, 16, 0);
  lv_obj_set_size(preview, 1280, 310);
  auto *kicker = Kicker(preview, "LIVE PREVIEW", kAccent);
  lv_obj_set_pos(kicker, 42, 34);
  auto *title = Label(preview, AccentName(RecoveryAccentColor()),
                      &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 42, 92);
  auto *sample = Button(preview, "Primary action", [] {}, true);
  lv_obj_set_pos(sample, 820, 80);
  lv_obj_set_size(sample, 400, 126);
  auto *hint = Label(preview,
      "Buttons, toggles, sliders, progress, navigation and focus states",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 42, 232);

  auto *appearance = Label(state->list, "Appearance",
                        &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(appearance, 32, 380);

  struct ModePreset {
    const char *name;
    const char *detail;
    AppearanceMode mode;
  };
  const std::array<ModePreset, 3> modes{{
      {"Graphite", "Dark, textured and focused", AppearanceMode::kGraphite},
      {"Light", "Warm white with subtle grain", AppearanceMode::kLight},
      {"AMOLED", "True black from edge to edge", AppearanceMode::kAmoled},
  }};
  const AppearanceMode selected_appearance = RecoveryAppearanceMode();
  for (size_t i = 0; i < modes.size(); ++i) {
    const auto mode = modes[i];
    const bool selected = selected_appearance == mode.mode;
    auto *card = lv_button_create(state->list);
    Clear(card);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 426, 450);
    lv_obj_set_size(card, 410, 170);
    lv_obj_set_style_radius(card, 32, 0);
    lv_obj_set_style_bg_color(card,
        mode.mode == AppearanceMode::kLight ? Color(0xf2f0eb)
        : mode.mode == AppearanceMode::kAmoled ? Color(0x000000)
                                                : Color(0x24272c), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, selected ? 4 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_transform_scale(card, 250, LV_STATE_PRESSED);
    auto *name = Label(card, mode.name, &lv_font_montserrat_32,
        mode.mode == AppearanceMode::kLight ? Color(0x171a1e)
                                            : Color(0xf5f7fa));
    lv_obj_set_pos(name, 32, 30);
    auto *description = Label(card, mode.detail, &lv_font_montserrat_24,
        mode.mode == AppearanceMode::kLight ? Color(0x575d64)
                                            : Color(0xaeb6c0));
    lv_obj_set_pos(description, 32, 94);
    FitLabelToLines(description, 340, 2,
                    {&lv_font_montserrat_24, &lv_font_montserrat_20,
                     &lv_font_montserrat_18});
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -32, 38);
    }
    OnClick(card, [state, mode] {
      if (!RecoverySetAppearanceMode(mode.mode)) {
        Sheet(state->screen, "Theme unavailable",
              "The selected appearance could not be stored.");
        return;
      }
      ApplyAppearanceMode(static_cast<int>(mode.mode));
      ApplySurfaceStyle(static_cast<int>(RecoverySurfaceStyle()));
      ApplyAccent(RecoveryAccentColor());
      RefreshTheme(state);
    });
  }

  auto *wallpaper_section =
      Label(state->list, "Wallpaper", &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(wallpaper_section, 32, 690);
  auto *wallpaper_card = lv_obj_create(state->list);
  Panel(wallpaper_card, 32, kMainPanel);
  lv_obj_set_pos(wallpaper_card, 16, 760);
  const int wallpaper_preview_height = std::max(172, wallpaper::PreviewHeight(260, 172));
  const int wallpaper_card_height = std::max(296, wallpaper_preview_height + 48);
  lv_obj_set_size(wallpaper_card, 1280, wallpaper_card_height);
  auto *wallpaper_preview = lv_obj_create(wallpaper_card);
  Clear(wallpaper_preview);
  lv_obj_set_pos(wallpaper_preview, 24, 24);
  lv_obj_set_size(wallpaper_preview, 260, wallpaper_preview_height);
  lv_obj_set_style_radius(wallpaper_preview, 24, 0);
  lv_obj_set_style_clip_corner(wallpaper_preview, true, 0);
  lv_obj_set_style_bg_color(wallpaper_preview, kMainSheet, 0);
  lv_obj_set_style_bg_opa(wallpaper_preview, LV_OPA_COVER, 0);
  const bool custom_wallpaper = wallpaper::Attach(wallpaper_preview, true);
  if (!custom_wallpaper) {
    auto *image = Label(wallpaper_preview, LV_SYMBOL_IMAGE,
                        &lv_font_montserrat_48, kMuted);
    lv_obj_center(image);
  }
  auto *wallpaper_title = Label(
      wallpaper_card, custom_wallpaper ? "Custom wallpaper" : "Appearance background",
      &lv_font_montserrat_28, kText);
  lv_obj_set_pos(wallpaper_title, 316, wallpaper_card_height / 2 - 55);
  const std::string selected_wallpaper = RecoveryWallpaperPath();
  const size_t wallpaper_slash = selected_wallpaper.find_last_of('/');
  const std::string wallpaper_detail = custom_wallpaper
      ? selected_wallpaper.substr(wallpaper_slash == std::string::npos
                                      ? 0 : wallpaper_slash + 1)
      : "Graphite, Light or AMOLED supplies the background";
  auto *wallpaper_copy = Label(wallpaper_card, wallpaper_detail.c_str(),
                               &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(wallpaper_copy, 316, wallpaper_card_height / 2 + 3);
  SingleLineLabel(wallpaper_copy, 470, &lv_font_montserrat_20);
  auto *choose_wallpaper = Button(wallpaper_card, "Choose image", [state] {
    OpenWallpaperPicker(state);
  });
  lv_obj_set_pos(choose_wallpaper, 816, wallpaper_card_height / 2 - 116);
  lv_obj_set_size(choose_wallpaper, 424, 108);
  auto *clear_wallpaper = Button(wallpaper_card, "Reset wallpaper", [state] {
    if (!RecoverySetWallpaperPath({})) {
      Sheet(state->screen, "Wallpaper unavailable",
            "AERA could not restore the appearance background.");
      return;
    }
    wallpaper::Clear();
    RefreshTheme(state);
  });
  lv_obj_set_pos(clear_wallpaper, 816, wallpaper_card_height / 2 + 8);
  lv_obj_set_size(clear_wallpaper, 424, 108);
  if (!custom_wallpaper) {
    lv_obj_add_state(clear_wallpaper, LV_STATE_DISABLED);
    lv_obj_set_style_opa(clear_wallpaper, LV_OPA_40, LV_STATE_DISABLED);
  }

  auto *surface_section =
      Label(state->list, "Surface style", &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(surface_section, 32, 1030);
  struct SurfacePreset {
    const char *name;
    const char *detail;
    SurfaceStyle style;
  };
  const std::array<SurfacePreset, 4> surfaces{{
      {"Solid", "Opaque and clean", SurfaceStyle::kSolid},
      {"Frosted", "Translucent blur", SurfaceStyle::kFrosted},
      {"Outline", "Borders, little fill", SurfaceStyle::kOutline},
      {"Elevated", "Shadow and depth", SurfaceStyle::kElevated},
  }};
  const SurfaceStyle selected_surface = RecoverySurfaceStyle();
  for (size_t i = 0; i < surfaces.size(); ++i) {
    const auto preset = surfaces[i];
    const bool selected = selected_surface == preset.style;
    auto *card = lv_button_create(state->list);
    Clear(card);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 320, 1100);
    lv_obj_set_size(card, 304, 170);
    lv_obj_set_style_radius(card, 30, 0);
    lv_obj_set_style_bg_color(card, kMainPanel, 0);
    lv_obj_set_style_bg_opa(
        card, preset.style == SurfaceStyle::kOutline ? LV_OPA_10
              : preset.style == SurfaceStyle::kFrosted ? LV_OPA_70
                                                       : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, selected ? 4 :
        preset.style == SurfaceStyle::kOutline ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale(card, 250, LV_STATE_PRESSED);
    if (preset.style == SurfaceStyle::kFrosted) {
      lv_obj_set_style_blur_backdrop(card, true, 0);
      lv_obj_set_style_blur_radius(card, 16, 0);
      lv_obj_set_style_blur_quality(card, LV_BLUR_QUALITY_SPEED, 0);
    } else if (preset.style == SurfaceStyle::kElevated) {
      lv_obj_set_style_shadow_color(card, Color(0x000000), 0);
      lv_obj_set_style_shadow_width(card, 18, 0);
      lv_obj_set_style_shadow_offset_y(card, 6, 0);
      lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    }
    auto *name = Label(card, preset.name, &lv_font_montserrat_28, kText);
    lv_obj_set_pos(name, 22, 26);
    auto *detail = Label(card, preset.detail, &lv_font_montserrat_18, kMuted);
    lv_obj_set_pos(detail, 22, 88);
    FitLabelToLines(detail, 258, 2,
                    {&lv_font_montserrat_18, &lv_font_montserrat_16});
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_20, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -18, 22);
    }
    OnClick(card, [state, preset] {
      if (!RecoverySetSurfaceStyle(preset.style)) {
        Sheet(state->screen, "Surface unavailable",
              "The selected surface style could not be stored.");
        return;
      }
      ApplyAppearanceMode(static_cast<int>(RecoveryAppearanceMode()));
      ApplySurfaceStyle(static_cast<int>(preset.style));
      ApplyAccent(RecoveryAccentColor());
      RefreshTheme(state);
    });
  }

  auto *home_icons_section = Label(state->list, "Home icon style",
                                    &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(home_icons_section, 32, 1310);
  for (int index = 0; index < 2; ++index) {
    const bool themed = index == 1;
    const bool selected = RecoveryThemedHomeIcons() == themed;
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + index * 640, 1380);
    lv_obj_set_size(card, 624, 170);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, themed ? "Themed" : "Classic",
                        &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 28, 28);
    auto *detail = Label(card, themed ? "Edge-to-edge artwork for every app"
                                     : "Accent icons on clean cards",
                          &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(detail, 28, 92);
    FitLabelToLines(detail, 552, 2,
                    {&lv_font_montserrat_20, &lv_font_montserrat_18});
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_24, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -24, 28);
    }
    OnClick(card, [state, themed] {
      if (!RecoverySetThemedHomeIcons(themed)) {
        Sheet(state->screen, "Theme unavailable",
              "The home icon style could not be stored.");
        return;
      }
      RefreshTheme(state);
    });
  }

  auto *icon_background_section =
      Label(state->list, "Icon backgrounds", &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(icon_background_section, 32, 690 + kSurfaceSectionOffset);
  struct IconBackgroundPreset {
    const char *name;
    const char *detail;
    bool tinted;
  };
  const std::array<IconBackgroundPreset, 2> icon_backgrounds{{
      {"Neutral", "Clean surface behind each icon", false},
      {"Accent tint", "Background follows the selected accent", true},
  }};
  for (size_t i = 0; i < icon_backgrounds.size(); ++i) {
    const auto preset = icon_backgrounds[i];
    const bool selected = RecoveryTintedIconBackgrounds() == preset.tinted;
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 640,
                   760 + kSurfaceSectionOffset);
    lv_obj_set_size(card, 624, 170);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *plate = lv_obj_create(card);
    Panel(plate, 22, preset.tinted ? kAccentSoft : kMainSheet);
    lv_obj_set_pos(plate, 28, 31);
    lv_obj_set_size(plate, 108, 108);
    auto *icon = Label(plate, LV_SYMBOL_SETTINGS, &lv_font_montserrat_48,
                       kAccent);
    lv_obj_center(icon);
    auto *name = Label(card, preset.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 164, 28);
    auto *detail = Label(card, preset.detail, &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(detail, 164, 94);
    lv_obj_set_width(detail, 420);
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_24, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -24, 28);
    }
    OnClick(card, [state, preset] {
      if (!RecoverySetTintedIconBackgrounds(preset.tinted)) {
        Sheet(state->screen, "Theme unavailable",
              "The icon background style could not be stored.");
        return;
      }
      RefreshTheme(state);
    });
  }

  auto *size_section = Label(state->list, "Density",
                             &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(size_section, 32, 1000 + kSurfaceSectionOffset);
  struct SizePreset {
    const char *name;
    const char *detail;
    InterfaceSize size;
  };
  const std::array<SizePreset, 4> sizes{{
      {"Compact", "More content on screen", InterfaceSize::kSmall},
      {"Standard", "Balanced AERA layout", InterfaceSize::kNormal},
      {"Comfortable", "Larger text and targets", InterfaceSize::kLarge},
      {"Spacious", "Maximum text and spacing", InterfaceSize::kSpacious},
  }};
  const InterfaceSize selected_size = RecoveryInterfaceSize();
  for (size_t i = 0; i < sizes.size(); ++i) {
    const auto preset = sizes[i];
    const bool selected = selected_size == preset.size;
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 320,
                   1070 + kSurfaceSectionOffset);
    lv_obj_set_size(card, 304, 170);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, preset.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 28, 28);
    auto *detail = Label(card, preset.detail, &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(detail, 28, 96);
    FitLabelToLines(detail, 244, 2,
                    {&lv_font_montserrat_20, &lv_font_montserrat_18,
                     &lv_font_montserrat_16});
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -28, 34);
    }
    OnClick(card, [state, preset] {
      if (!RecoverySetInterfaceSize(preset.size)) {
        Sheet(state->screen, "Density unavailable",
              "The interface density could not be changed.");
        return;
      }
      ApplyInterfaceSize(static_cast<int>(preset.size));
      RefreshTheme(state);
    });
  }

  auto *grid_section = Label(state->list, "Home plugin grid",
                             &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(grid_section, 32, 1310 + kSurfaceSectionOffset);
  struct GridPreset { const char *name; const char *detail; int columns; };
  const std::array<GridPreset, 4> grids{{
      {"2 x 3", "Wide cards - 6 apps per page", 2},
      {"3 x 3", "Compact cards - 9 apps per page", 3},
      {"4 x 4", "Icons and names - 16 apps per page", 4},
      {"5 x 5", "Small icons - 25 apps per page", 5},
  }};
  const int selected_columns = RecoveryHomeGridColumns();
  for (size_t i = 0; i < grids.size(); ++i) {
    const auto grid = grids[i];
    const bool selected = selected_columns == grid.columns;
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 320,
                   1380 + kSurfaceSectionOffset);
    lv_obj_set_size(card, 304, 170);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, grid.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 32, 28);
    auto *detail = Label(card, grid.detail, &lv_font_montserrat_18, kMuted);
    lv_obj_set_pos(detail, 32, 94);
    FitLabelToLines(detail, 240, 2,
                    {&lv_font_montserrat_18, &lv_font_montserrat_16});
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -32, 38);
    }
    OnClick(card, [state, grid] {
      if (!RecoverySetHomeGridColumns(grid.columns)) {
        Sheet(state->screen, "Layout unavailable",
              "The Home plugin grid could not be changed.");
        return;
      }
      RefreshTheme(state);
    });
  }

  auto *keyboard_section = Label(state->list, "Keyboard layout",
                                 &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(keyboard_section, 32, 1620 + kSurfaceSectionOffset);
  struct KeyboardPreset {
    const char *name;
    const char *detail;
    KeyboardLayout layout;
  };
  const std::array<KeyboardPreset, 2> keyboard_layouts{{
      {"QWERTY", "International default", KeyboardLayout::kQwerty},
      {"QWERTZ", "Y and Z exchanged", KeyboardLayout::kQwertz},
  }};
  const KeyboardLayout selected_keyboard = RecoveryKeyboardLayout();
  for (size_t i = 0; i < keyboard_layouts.size(); ++i) {
    const auto preset = keyboard_layouts[i];
    const bool selected = selected_keyboard == preset.layout;
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + static_cast<int32_t>(i) * 640,
                   1690 + kSurfaceSectionOffset);
    lv_obj_set_size(card, 624, 170);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, preset.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 32, 28);
    auto *detail = Label(card, preset.detail, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail, 32, 94);
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -32, 38);
    }
    OnClick(card, [state, preset] {
      if (!RecoverySetKeyboardLayout(preset.layout)) {
        Sheet(state->screen, "Keyboard unavailable",
              "The keyboard layout could not be stored.");
        return;
      }
      RefreshTheme(state);
    });
  }

  auto *section = Label(state->list, "Accent palettes",
                        &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(section, 32, 1920 + kSurfaceSectionOffset);
  const uint32_t selected_rgb = RecoveryAccentColor();
  for (size_t i = 0; i < kAccentPresets.size(); ++i) {
    const auto preset = kAccentPresets[i];
    const int32_t column = static_cast<int32_t>(i % 4);
    const int32_t row = static_cast<int32_t>(i / 4);
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + column * 320,
                   1990 + kSurfaceSectionOffset + row * 220);
    lv_obj_set_size(card, 304, 190);
    lv_obj_set_style_border_width(
        card, selected_rgb == preset.rgb ? 4
              : RecoverySurfaceStyle() == SurfaceStyle::kOutline ? 2 : 1, 0);
    lv_obj_set_style_border_color(card,
        selected_rgb == preset.rgb ? Color(preset.rgb) : kMainLine, 0);

    auto *swatch = lv_obj_create(card);
    Clear(swatch);
    lv_obj_set_size(swatch, 72, 72);
    lv_obj_set_pos(swatch, 24, 24);
    lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(swatch, Color(preset.rgb), 0);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
    auto *name = Label(card, preset.name, &lv_font_montserrat_24, kText);
    lv_obj_set_pos(name, 24, 116);
    if (selected_rgb == preset.rgb) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_32,
                          Color(preset.rgb));
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -28, 38);
    }
    OnClick(card, [state, preset] {
      if (!RecoverySetAccentColor(preset.rgb)) {
        Sheet(state->screen, "Theme unavailable",
              "The selected accent could not be stored.");
        return;
      }
      ApplyAccent(preset.rgb);
      RefreshTheme(state);
    });
  }

  const bool custom_selected = !IsPresetAccent(selected_rgb);
  auto *custom = lv_button_create(state->list);
  Panel(custom, 32, kMainPanel);
  Interactive(custom, kMainSelected);
  lv_obj_set_pos(custom, 16 + 3 * 320, 1990 + kSurfaceSectionOffset + 220);
  lv_obj_set_size(custom, 304, 190);
  lv_obj_set_style_border_width(
      custom, custom_selected ? 4
              : RecoverySurfaceStyle() == SurfaceStyle::kOutline ? 2 : 1, 0);
  lv_obj_set_style_border_color(custom,
      custom_selected ? Color(selected_rgb) : kMainLine, 0);
  auto *disk = HueSaturationDisk(custom, 72);
  lv_obj_set_pos(disk, 24, 24);
  auto *custom_name = Label(custom, "User select",
                            &lv_font_montserrat_24, kText);
  lv_obj_set_pos(custom_name, 24, 116);
  if (custom_selected) {
    auto *selected = lv_obj_create(custom);
    Clear(selected);
    lv_obj_set_size(selected, 38, 38);
    lv_obj_align(selected, LV_ALIGN_TOP_RIGHT, -28, 38);
    lv_obj_set_style_radius(selected, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(selected, Color(selected_rgb), 0);
    lv_obj_set_style_bg_opa(selected, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(selected, 3, 0);
    lv_obj_set_style_border_color(selected, kText, 0);
    lv_obj_remove_flag(selected, LV_OBJ_FLAG_CLICKABLE);
  }
  OnClick(custom, [state] { OpenAccentPicker(state); });

  auto *note = Label(state->list,
      "AERA Cyan is the default. Choose User select for any colour. Changes\n"
      "apply immediately and are saved with your recovery preferences.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(note, 32, 2470 + kSurfaceSectionOffset);
  lv_obj_set_width(note, 1220);

  auto *dock_section = Label(state->list, "Navigation dock",
                             &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(dock_section, 32, 2590 + kSurfaceSectionOffset);
  const std::array<std::pair<const char *, DockLayout>, 5> dock_modes{{
      {"Glass", DockLayout::kGlass}, {"Compact", DockLayout::kCompact},
      {"Minimal", DockLayout::kMinimal}, {"Icons only", DockLayout::kIcons},
      {"Text Only", DockLayout::kTextOnly}}};
  const int dock_extra_height =
      (static_cast<int>((dock_modes.size() + 3) / 4) - 1) * 152;
  for (size_t i = 0; i < dock_modes.size(); ++i) {
    const auto mode = dock_modes[i];
    const bool selected = RecoveryDockLayout() == mode.second;
    auto *card = Button(state->list, mode.first, [state, mode] {
      if (!RecoverySetDockLayout(mode.second)) {
        Sheet(state->screen, "Layout unavailable",
              "The navigation dock style could not be changed.");
        return;
      }
      RefreshTheme(state);
    });
    Panel(card, 34, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + static_cast<int>(i % 4) * 320,
                   2660 + kSurfaceSectionOffset +
                       static_cast<int>(i / 4) * 152);
    lv_obj_set_size(card, 304, 128);
    lv_obj_set_style_radius(card, 34, 0);
    lv_obj_set_style_border_width(
        card, selected ? 3
                       : RecoverySurfaceStyle() == SurfaceStyle::kOutline
                             ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_border_opa(card, selected ? LV_OPA_COVER : LV_OPA_40, 0);
    if (selected) {
      auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_24, kAccent);
      lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -20, 18);
    }
  }

  auto dock_slider = [state](int y, const char *name, const char *description,
                             int value, bool blur) {
    auto *card = lv_obj_create(state->list);
    Panel(card, 32, kMainPanel);
    lv_obj_set_pos(card, 16, y);
    lv_obj_set_size(card, 1280, 220);
    auto *title = Label(card, name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(title, 36, 24);
    auto *copy = Label(card, description, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(copy, 36, 76);
    auto *amount = Label(card, (std::to_string(value) + "%").c_str(),
                         &lv_font_montserrat_24, kAccent);
    lv_obj_align(amount, LV_ALIGN_TOP_RIGHT, -38, 34);
    auto *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 52, 166);
    lv_obj_set_size(slider, 1176, 20);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    RangeSlider(slider);
    struct DockBinding { Tools *state; lv_obj_t *amount; bool blur; };
    auto *binding = new DockBinding{state, amount, blur};
    lv_obj_add_event_cb(slider, [](lv_event_t *event) {
      auto *binding = static_cast<DockBinding *>(lv_event_get_user_data(event));
      if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        delete binding;
        return;
      }
      const auto code = lv_event_get_code(event);
      if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
      const int value = lv_slider_get_value(lv_event_get_target_obj(event));
      const std::string text = std::to_string(value) + "%";
      lv_label_set_text(binding->amount, text.c_str());
      const int transparency = binding->blur
          ? RecoveryDockTransparency() : value;
      const int blur = binding->blur ? value : RecoveryDockBlur();
      UpdateNavigationDockAppearance(binding->state->navigation,
                                     transparency, blur);
      if (code == LV_EVENT_RELEASED) {
        if (binding->blur) RecoverySetDockBlur(value);
        else RecoverySetDockTransparency(value);
        AutoSaveSettings(binding->state);
      }
    }, LV_EVENT_ALL, binding);
  };
  dock_slider(2820 + kSurfaceSectionOffset + dock_extra_height, "Transparency",
              "0% is solid; 100% leaves only the controls visible",
              RecoveryDockTransparency(), false);
  dock_slider(3060 + kSurfaceSectionOffset + dock_extra_height, "Backdrop blur",
              "GPU-friendly live blur behind the dock surface",
              RecoveryDockBlur(), true);

  auto *font_section = Label(state->list, "Typography",
                             &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(font_section, 32,
                 3340 + kSurfaceSectionOffset + dock_extra_height);
  auto *font_hint = Label(
      state->list,
      "Choose the typeface used across the AERA interface.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(font_hint, 32,
                 3390 + kSurfaceSectionOffset + dock_extra_height);

  struct FontCards {
    struct Card {
      lv_obj_t *object;
      lv_obj_t *check;
      std::string id;
    };
    std::vector<Card> cards;
  };
  auto *font_cards = new FontCards;
  lv_obj_add_event_cb(state->list, [](lv_event_t *event) {
    delete static_cast<FontCards *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, font_cards);

  struct FontChoice {
    std::string id;
    std::string family;
    std::string path;
  };
  std::vector<FontChoice> font_choices{{"", "AERA Default", ""}};
  for (const auto &plugin : plugins::Installed()) {
    if (!plugins::IsThemeExtension(plugin)) continue;
    std::string path;
    std::string error;
    plugins::Plugin verified;
    if (plugins::ResolvePayload(plugin.id, verified, path, error) &&
        plugins::IsThemeExtension(verified)) {
      font_choices.push_back({verified.id, verified.font_family, path});
    }
  }
  const std::string stored_font = RecoveryUiFont();
  const bool stored_font_available = std::any_of(
      font_choices.begin(), font_choices.end(),
      [&](const FontChoice &choice) { return choice.id == stored_font; });
  const std::string selected_font = stored_font_available ? stored_font : "";
  for (size_t i = 0; i < font_choices.size(); ++i) {
    const FontChoice choice = font_choices[i];
    const bool selected = selected_font == choice.id;
    const int column = static_cast<int>(i % 2);
    const int row = static_cast<int>(i / 2);
    auto *card = lv_button_create(state->list);
    Panel(card, 32, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 16 + column * 640,
                   3450 + kSurfaceSectionOffset + dock_extra_height + row * 220);
    lv_obj_set_size(card, 624, 196);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, choice.family.c_str(), &lv_font_montserrat_28,
                       kText);
    lv_obj_set_pos(name, 28, 24);
    auto *sample = lv_label_create(card);
    i18n::BindLabel(sample, "Aa 123  The quick brown fox");
    const lv_font_t *preview = choice.path.empty()
        ? static_cast<const lv_font_t *>(&lv_font_montserrat_36)
        : fonts::PreviewFont(choice.path, 36);
    lv_obj_set_style_text_font(
        sample, preview ? preview : UiFont(&lv_font_montserrat_36), 0);
    lv_obj_set_style_text_color(sample, kMutedStrong, 0);
    lv_obj_set_pos(sample, 28, 86);
    lv_obj_set_width(sample, 540);
    lv_label_set_long_mode(sample, LV_LABEL_LONG_DOT);
    auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_28, kAccent);
    lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -28, 28);
    if (!selected) lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
    font_cards->cards.push_back({card, check, choice.id});
    OnClick(card, [state, font_cards, choice] {
      bool applied = false;
      if (choice.id.empty()) {
        applied = fonts::SelectUiFont({}, {}) && RecoverySetUiFont({});
      } else {
        applied = fonts::SelectUiFont(choice.id, choice.path) &&
                  RecoverySetUiFont(choice.id);
      }
      if (!applied) {
        Sheet(state->screen, "Font unavailable",
              "AERA could not load this font extension.");
        return;
      }
      // Existing labels keep the same stable wrapper pointers while their
      // underlying typefaces change. Refresh every style and layout so the
      // selected font is visible immediately across the complete UI.
      lv_obj_report_style_change(nullptr);
      lv_obj_update_layout(state->screen);
      for (const auto &item : font_cards->cards) {
        const bool active = item.id == choice.id;
        lv_obj_set_style_border_width(item.object, active ? 3 : 1, 0);
        lv_obj_set_style_border_color(item.object,
                                      active ? kAccent : kMainLine, 0);
        if (active)
          lv_obj_remove_flag(item.check, LV_OBJ_FLAG_HIDDEN);
        else
          lv_obj_add_flag(item.check, LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_invalidate(state->screen);
      AutoSaveSettings(state);
    });
  }

  const int font_rows = std::max(
      1, static_cast<int>((font_choices.size() + 1) / 2));
  const int reset_y = BuildLockEditor(state, kSurfaceSectionOffset + dock_extra_height +
      std::max(3760, 3450 + font_rows * 220 + 70));
  auto *reset = Button(state->list, "Reset theme settings", [state] {
    Sheet(
        state->screen, "Reset theme settings",
        "Restore default colors, appearance, wallpaper, layout, font and lock screen?",
        [state] {
          if (!RecoveryResetThemeSettings() || !fonts::SelectUiFont({}, {})) {
            Sheet(state->screen, "Theme unavailable",
                  "AERA could not restore every theme setting.");
            return;
          }
          ApplyAppearanceMode(static_cast<int>(RecoveryAppearanceMode()));
          wallpaper::Clear();
          ApplySurfaceStyle(static_cast<int>(RecoverySurfaceStyle()));
          ApplyInterfaceSize(static_cast<int>(RecoveryInterfaceSize()));
          ApplyAccent(RecoveryAccentColor());
          lv_obj_report_style_change(nullptr);
          RefreshTheme(state);
        },
        0, false, SheetPresentation::kStandard, "Swipe to confirm");
  });
  lv_obj_set_pos(reset, 16, reset_y);
  lv_obj_set_size(reset, 1280, 124);

  // Preserve all section spacing below the variable-height wallpaper card.
  for (uint32_t i = lv_obj_get_index(surface_section);
       i < lv_obj_get_child_count(state->list); ++i) {
    auto *child = lv_obj_get_child(state->list, i);
    lv_obj_set_y(child, lv_obj_get_style_y(child, LV_PART_MAIN) + wallpaper_card_height - 220);
  }

  if (theme_scroll_restore >= 0) {
    lv_obj_update_layout(state->list);
    lv_obj_scroll_to_y(state->list, theme_scroll_restore, LV_ANIM_OFF);
    theme_scroll_restore = -1;
  }
}

void BuildPreferences(Tools *state) {
  Header(state->screen, "Preferences", "Display, files, backups and connection settings.",
         state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  const int list_y = landscape ? 340 : 480;
  state->list = Scroll(state->screen, list_y,
      std::max(1, static_cast<int>(lv_obj_get_height(state->screen)) -
          list_y - NavigationHeight(state->screen) - 28));
  if (landscape) {
    lv_obj_set_x(state->list, 884);
    lv_obj_set_width(state->list, 1400);
  }
  lv_obj_set_flex_flow(state->list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(state->list, 8, 0);
  lv_obj_set_style_pad_bottom(state->list, 32, 0);
  lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_ACTIVE);

  PreferenceSection(state, "Display & regional");
  std::string language_name = i18n::CurrentLanguage();
  for (const auto &language : i18n::AvailableLanguages())
    if (language_name == language.code) {
      language_name = language.native_name;
      break;
    }
  PreferenceEntry(state, LV_SYMBOL_KEYBOARD, "Language", language_name,
                  [state] { Open(state, Action::kLanguage); });
  const std::string status_detail = std::string(i18n::Translate("Battery style")) +
      " / " + i18n::Translate("Network speed");
  PreferenceEntry(state, LV_SYMBOL_BATTERY_FULL, "Status bar", status_detail,
                  [state] { Open(state, Action::kStatusBar); });

  auto *brightness = lv_obj_create(state->list);
  Clear(brightness);
  PreferenceColumn(brightness, 12);
  lv_obj_set_style_pad_all(brightness, 24, 0);
  auto *brightness_heading = lv_obj_create(brightness);
  Clear(brightness_heading);
  lv_obj_set_width(brightness_heading, LV_PCT(100));
  lv_obj_set_height(brightness_heading, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(brightness_heading, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(brightness_heading, LV_FLEX_ALIGN_START,
                       LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(brightness_heading, 28, 0);
  auto *brightness_icon = Label(brightness_heading, LV_SYMBOL_EYE_OPEN,
                                &lv_font_montserrat_32, kAccent);
  lv_obj_set_width(brightness_icon, 56);
  auto *title = Label(brightness_heading, "Brightness", &lv_font_montserrat_32, kText);
  lv_obj_set_flex_grow(title, 1);
  lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
  auto *value = Label(brightness_heading,
      (std::to_string(RecoveryBrightness()) + "%").c_str(),
      &lv_font_montserrat_32, kAccent);
  lv_obj_set_width(value, 146);
  lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
  auto *brightness_track = lv_obj_create(brightness);
  Clear(brightness_track);
  lv_obj_set_size(brightness_track, LV_PCT(100), 96);
  auto *slider = lv_slider_create(brightness_track);
  lv_obj_set_size(slider, LV_PCT(96), 24);
  lv_obj_center(slider);
  lv_slider_set_range(slider, 10, 100);
  lv_slider_set_value(slider, std::max(10, RecoveryBrightness()), LV_ANIM_OFF);
  RangeSlider(slider);
  lv_obj_add_event_cb(slider, [](lv_event_t *event) {
    const auto code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
    const int percent = lv_slider_get_value(lv_event_get_target_obj(event));
    lv_label_set_text(static_cast<lv_obj_t *>(lv_event_get_user_data(event)),
                      (std::to_string(percent) + "%").c_str());
    if (code == LV_EVENT_RELEASED ||
        std::abs(percent - RecoveryBrightness()) >= 2)
      RecoverySetBrightness(percent);
  }, LV_EVENT_ALL, value);
  lv_obj_add_event_cb(slider, [](lv_event_t *event) {
    AutoSaveSettings(static_cast<Tools *>(lv_event_get_user_data(event)));
  }, LV_EVENT_RELEASED, state);

  if (RecoveryScreenTimeoutSupported()) {
    auto *timeout = lv_obj_create(state->list);
    Clear(timeout);
    PreferenceColumn(timeout, 12);
    lv_obj_set_style_pad_all(timeout, 24, 0);
    auto *heading = lv_obj_create(timeout);
    Clear(heading);
    lv_obj_set_size(heading, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(heading, 28, 0);
    auto *icon = Label(heading, LV_SYMBOL_EYE_CLOSE, &lv_font_montserrat_32, kAccent);
    lv_obj_set_width(icon, 56);
    auto *title = Label(heading, "Screen-off timeout", &lv_font_montserrat_32, kText);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    auto *value = Label(heading, (std::to_string(RecoveryScreenTimeout()) + " s").c_str(),
                        &lv_font_montserrat_32, kAccent);
    lv_obj_set_width(value, 146);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    auto *track = lv_obj_create(timeout);
    Clear(track);
    lv_obj_set_size(track, LV_PCT(100), 96);
    auto *slider = lv_slider_create(track);
    lv_obj_set_size(slider, LV_PCT(96), 24);
    lv_obj_center(slider);
    lv_slider_set_range(slider, 30, 300);
    lv_slider_set_value(slider, RecoveryScreenTimeout(), LV_ANIM_OFF);
    RangeSlider(slider);
    lv_obj_add_event_cb(slider, [](lv_event_t *event) {
      const auto code = lv_event_get_code(event);
      if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
      const int seconds = lv_slider_get_value(lv_event_get_target_obj(event));
      lv_label_set_text(static_cast<lv_obj_t *>(lv_event_get_user_data(event)),
                        (std::to_string(seconds) + " s").c_str());
      if (code == LV_EVENT_RELEASED) RecoverySetScreenTimeout(seconds);
    }, LV_EVENT_ALL, value);
    lv_obj_add_event_cb(slider, [](lv_event_t *event) {
      AutoSaveSettings(static_cast<Tools *>(lv_event_get_user_data(event)));
    }, LV_EVENT_RELEASED, state);
  }

  PreferenceToggle(state, LV_SYMBOL_BELL, "24-hour clock",
                   "Off uses 12-hour time with AM / PM", Preference::kClock24);
  auto zone_text = [] {
    const int offset = RecoveryUtcOffset(), magnitude = std::abs(offset);
    char text[64];
    snprintf(text, sizeof(text), "UTC %c%02d:%02d", offset < 0 ? '-' : '+',
             magnitude / 60, magnitude % 60);
    return std::string(text);
  };
  auto *zone = PreferenceEntry(state, LV_SYMBOL_SETTINGS, "Time zone",
                               zone_text(), {});
  auto *zone_value = lv_obj_get_child(lv_obj_get_child(zone, 1), 1);
  OnClick(zone, [state, zone_value, zone_text] {
    Sheet(state->screen, "Time zone", "", {}, 840, true,
          SheetPresentation::kStandard, "Swipe to confirm",
          [state, zone_value, zone_text](lv_obj_t *area) {
      lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(area, 28, 0);
      auto *current = Label(area, zone_text().c_str(), &lv_font_montserrat_48, kText);
      lv_obj_set_width(current, LV_PCT(100));
      lv_obj_set_style_text_align(current, LV_TEXT_ALIGN_CENTER, 0);
      auto *controls = lv_obj_create(area);
      Clear(controls);
      lv_obj_set_width(controls, LV_PCT(100));
      lv_obj_set_height(controls, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(controls, 40, 0);
      for (int direction : {-1, 1}) {
        auto *button = Button(controls,
            direction < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS,
            [state, direction, current, zone_value, zone_text] {
          const int next = std::clamp(RecoveryUtcOffset() + 15 * direction, -720, 840);
          if (!RecoverySetUtcOffset(next)) {
            Sheet(state->screen, "Time offset unavailable",
                  "The time offset could not be changed.");
            return;
          }
          lv_label_set_text(current, zone_text().c_str());
          lv_label_set_text(zone_value, zone_text().c_str());
          AutoSaveSettings(state);
        });
        lv_obj_set_size(button, 180, 124);
      }
      auto *hint = Label(area, "Fixed offset, 15-minute steps; no automatic DST.",
                         &lv_font_montserrat_24, kMuted);
      lv_obj_set_width(hint, LV_PCT(100));
      lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    });
  });

  PreferenceSection(state, "Navigation & feedback");
  PreferenceToggle(state, LV_SYMBOL_HOME, "Gesture navigation",
                   "Swipe up for Home; swipe up and hold for Recents",
                   Preference::kRecents);
  if (RecoveryHapticsAvailable()) {
    const std::string feedback = std::string(i18n::Translate("Touch feedback")) +
        " / " + i18n::Translate("Keyboard feedback") +
        " / " + i18n::Translate("Operation feedback");
    PreferenceEntry(state, LV_SYMBOL_BELL, "Haptics", feedback, [state] {
      Sheet(state->screen, "Haptics", "", {}, 1460, true,
            SheetPresentation::kStandard, "Swipe to confirm",
            [state](lv_obj_t *area) {
        lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(area, 16, 0);
        HapticSlider(state, area, "Touch feedback",
                     "Buttons, cards and navigation", Haptic::kTouch, 300);
        HapticSlider(state, area, "Keyboard feedback",
                     "Keys in PIN, Wi-Fi and text entry", Haptic::kKeyboard, 300);
        HapticSlider(state, area, "Operation feedback",
                     "A stronger pulse when recovery work finishes", Haptic::kAction, 500);
        auto *test = Button(area, "Test operation vibration", [] {
          RecoveryVibrate(Haptic::kAction);
        });
        lv_obj_set_width(test, LV_PCT(100));
        lv_obj_set_height(test, 124);
      });
    });
  }

  PreferenceSection(state, "Files & installation");
  PreferenceToggle(state, LV_SYMBOL_FILE, "Show hidden files",
                   "Include dot-prefixed files and folders", Preference::kHiddenFiles);
  PreferenceToggle(state, LV_SYMBOL_OK, "Verify ZIP signatures",
                   "Only install packages signed by a trusted recovery key", Preference::kVerifyZip);
  PreferenceToggle(state, LV_SYMBOL_REFRESH, "Keep AERA installed",
                   "Restore the running AERA recovery to both slots after ZIP installs",
                   Preference::kPreserveRecovery);
  if (RecoveryAblPreservationSupported())
    PreferenceToggle(state, LV_SYMBOL_SAVE, "Keep current ABL",
                     "Restore the active-slot ABL to both slots after ZIP installs",
                     Preference::kPreserveAbl);

  PreferenceSection(state, "Backup & connection");
  PreferenceToggle(state, LV_SYMBOL_SAVE, "Compress backups by default",
                   "Smaller archives; backup and restore may take longer", Preference::kCompression);
  PreferenceToggle(state, LV_SYMBOL_OK, "Generate backup checksums",
                   "Create MD5/SHA-256 files for recovery backups", Preference::kBackupChecksums);
  PreferenceToggle(state, LV_SYMBOL_OK, "Verify backup checksums",
                   "Verify existing MD5/SHA-256 files before restoring", Preference::kVerifyBackupChecksums);
  if (RecoverySha256Available())
    PreferenceToggle(state, LV_SYMBOL_OK, "SHA-256 backup checksums",
                     "On: SHA-256 / Off: legacy MD5 checksums", Preference::kSha256);
  auto *mtp = PreferenceEntry(state, LV_SYMBOL_USB, "USB file transfer",
      "MTP makes accessible storage available to your computer.", {}, true);
  auto *mtp_switch = lv_switch_create(mtp);
  StyleSwitch(mtp_switch, RecoveryMtpEnabled());
  OnClick(mtp, [state, mtp_switch] {
    const bool enabled = !RecoveryMtpEnabled();
    if (!RecoverySetMtp(enabled)) {
      Sheet(state->screen, "USB transfer unavailable",
            "Check that storage is unlocked and mounted.");
      return;
    }
    StyleSwitch(mtp_switch, RecoveryMtpEnabled());
  });

  auto *reset = Button(state->list, "Reset preferences", [state] {
    Sheet(state->screen, "Reset preferences",
          "Restore default preferences? Theme, language, Wi-Fi and plugin settings are kept.",
          [state] {
            if (!RecoveryResetPreferences()) {
              Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
              return;
            }
            if (AutoSaveSettings(state)) Open(state, Action::kPreferences);
          }, 0, false, SheetPresentation::kStandard, "Swipe to confirm");
  });
  lv_obj_set_width(reset, LV_PCT(100));
  lv_obj_set_height(reset, 124);
}

void RefreshBatterySettings(Tools *state) {
  const bool circle = RecoveryBatteryStyle() == BatteryStyle::kCircle;
  if (circle) {
    lv_obj_remove_flag(state->battery_size_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(state->battery_percentage_row, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(state->battery_size_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(state->battery_percentage_row, LV_OBJ_FLAG_HIDDEN);
  }
  const int size = RecoveryBatteryCircleSize();
  lv_slider_set_value(state->battery_size_slider, size, LV_ANIM_OFF);
  lv_label_set_text(state->battery_size_amount, (std::to_string(size * 100 / kBatteryCircleSizeDefault) + "%").c_str());
  ResizeCircleBattery(state->battery_preview_circle, size);
  UpdateCircleBattery(state->battery_preview_circle, 76, false);
  lv_obj_align(state->battery_preview_circle.root, LV_ALIGN_TOP_MID, 0, 28 + (80 - size) / 2);
  const bool percentage = RecoveryBatteryPercentage();
  if (percentage) lv_obj_remove_flag(state->battery_preview_percentage, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(state->battery_preview_percentage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_width(state->battery_preview_classic, percentage ? 188 : 80);
  lv_obj_set_x(state->battery_preview_body, percentage ? 100 : 0);
  lv_obj_set_x(state->battery_preview_tip, percentage ? 171 : 71);
}

constexpr bool TemperatureDisplay::* kTemperatureToggles[] = {
  &TemperatureDisplay::cpu, &TemperatureDisplay::battery, &TemperatureDisplay::fahrenheit,
  &TemperatureDisplay::decimals, &TemperatureDisplay::labels, &TemperatureDisplay::colors,
  &TemperatureDisplay::cpu_load, &TemperatureDisplay::ram,
};
constexpr int TemperatureDisplay::* kTemperatureSliders[] = {
  &TemperatureDisplay::size, &TemperatureDisplay::position, &TemperatureDisplay::cpu_warm,
  &TemperatureDisplay::cpu_hot, &TemperatureDisplay::battery_warm, &TemperatureDisplay::battery_hot,
};

void RefreshTemperatureSettings(Tools *state) {
  const auto display = RecoveryTemperatureDisplay();
  const bool temperatures = display.cpu || display.battery;
  const bool enabled = temperatures || display.cpu_load || display.ram;
  if (enabled) lv_obj_remove_flag(state->temperature_controls, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(state->temperature_controls, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 8; ++i)
    StyleSwitch(state->temperature_toggles[i], display.*kTemperatureToggles[i]);
  for (int i : {2, 3, 5})
    lv_obj_set_flag(lv_obj_get_parent(state->temperature_toggles[i]), LV_OBJ_FLAG_HIDDEN, !temperatures);
  lv_obj_set_flag(state->temperature_colors_heading, LV_OBJ_FLAG_HIDDEN, !temperatures);
  for (int i = 0; i < 6; ++i) {
    const int value = display.*kTemperatureSliders[i];
    lv_slider_set_value(state->temperature_sliders[i], i == 0 ? (value - 20) / 4 : value, LV_ANIM_OFF);
    const int degrees = display.fahrenheit ? int(std::lround(value * 1.8 + 32)) : value;
    const std::string amount = i == 0 ? std::to_string(value * 100 / 24) + "%" :
        i == 1 ? std::to_string(value) + "%" : std::to_string(degrees) +
        (display.fahrenheit ? "\xc2\xb0" "F" : "\xc2\xb0" "C");
    lv_label_set_text(state->temperature_amounts[i], amount.c_str());
    if (i >= 2) {
      const bool visible = display.colors && (i < 4 ? display.cpu : display.battery);
      if (visible) lv_obj_remove_flag(state->temperature_panels[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(state->temperature_panels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void BuildTemperatureSettings(Tools *state) {
  Header(state->screen, "SysInfo", "", state->callback, state->context);
  const int top = Landscape(state->screen) ? 340 : 452;
  state->list = Scroll(state->screen, top,
      std::max(1, int(lv_obj_get_height(state->screen)) - top - NavigationHeight(state->screen) - 28));
  lv_obj_set_flex_flow(state->list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(state->list, 16, 0);
  lv_obj_set_style_pad_bottom(state->list, 32, 0);
  PreferenceSection(state, "Sensors");
  state->temperature_saved = RecoveryTemperatureDisplay();
  auto *main_list = state->list;
  const char *titles[] = {"CPU temperature", "Battery temperature", "Use Fahrenheit",
                         "Show decimal places", "Show labels", "Temperature colors", "CPU usage", "RAM usage"};
  auto add_toggle = [&](int i) {
    auto *row = PreferenceEntry(state, LV_SYMBOL_SETTINGS, titles[i], "", {}, true);
    auto *toggle = lv_switch_create(row);
    state->temperature_toggles[i] = toggle;
    OnClick(row, [state, i] {
      auto display = RecoveryTemperatureDisplay();
      display.*kTemperatureToggles[i] = !(display.*kTemperatureToggles[i]);
      if (!RecoverySetTemperatureDisplay(display)) {
        Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
        return;
      }
      state->temperature_saved = RecoveryTemperatureDisplay();
      RefreshTemperatureSettings(state);
      RefreshStatusBars();
    });
  };
  for (int i : {0, 1, 6, 7, 2, 3, 4}) {
    if (i == 2) {
      state->temperature_controls = lv_obj_create(main_list);
      Clear(state->temperature_controls);
      PreferenceColumn(state->temperature_controls, 16);
      state->list = state->temperature_controls;
      PreferenceSection(state, "Display");
    }
    add_toggle(i);
  }
  const char *slider_titles[] = {"Text size", "Spacing", "CPU orange from",
                                "CPU red from", "Battery orange from", "Battery red from"};
  const int minimum[] = {0, 0, 30, 31, 25, 26};
  const int maximum[] = {3, 100, 109, 110, 59, 60};
  for (int i = 0; i < 6; ++i) {
    if (i == 2) {
      PreferenceSection(state, "Colors");
      state->temperature_colors_heading = lv_obj_get_child(state->list, -1);
      add_toggle(5);
    }
    auto *panel = lv_obj_create(state->list);
    state->temperature_panels[i] = panel;
    Clear(panel);
    PreferenceColumn(panel, 12);
    lv_obj_set_style_pad_all(panel, 24, 0);
    auto *heading = lv_obj_create(panel);
    Clear(heading);
    lv_obj_set_size(heading, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto *title = Label(heading, slider_titles[i], &lv_font_montserrat_32, kText);
    lv_obj_set_width(title, 0);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    auto *amount = Label(heading, "", &lv_font_montserrat_24, kAccent);
    state->temperature_amounts[i] = amount;
    lv_obj_set_width(amount, 140);
    lv_obj_set_style_text_align(amount, LV_TEXT_ALIGN_RIGHT, 0);
    auto *track = lv_obj_create(panel);
    Clear(track);
    lv_obj_set_size(track, LV_PCT(100), 96);
    auto *slider = lv_slider_create(track);
    state->temperature_sliders[i] = slider;
    lv_obj_set_size(slider, LV_PCT(96), 24);
    lv_obj_center(slider);
    lv_slider_set_range(slider, minimum[i], maximum[i]);
    RangeSlider(slider);
    lv_obj_add_event_cb(slider, [](lv_event_t *event) {
      const auto code = lv_event_get_code(event);
      if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
      auto *state = static_cast<Tools *>(lv_event_get_user_data(event));
      auto display = code == LV_EVENT_PRESS_LOST ? state->temperature_saved : RecoveryTemperatureDisplay();
      for (int j = 0; j < 6; ++j) {
        if (lv_event_get_target_obj(event) != state->temperature_sliders[j]) continue;
        const int value = lv_slider_get_value(state->temperature_sliders[j]);
        display.*kTemperatureSliders[j] = code == LV_EVENT_PRESS_LOST ?
            state->temperature_saved.*kTemperatureSliders[j] : j == 0 ? 20 + value * 4 : value;
      }
      const bool save = code == LV_EVENT_RELEASED;
      if (!RecoverySetTemperatureDisplay(display, save)) {
        RecoverySetTemperatureDisplay(state->temperature_saved, false);
        i18n::BindLabel(state->summary, "This setting could not be changed.");
        lv_obj_set_style_text_color(state->summary, kRed, 0);
      } else {
        if (save) state->temperature_saved = RecoveryTemperatureDisplay();
        i18n::BindLabel(state->summary, save ? "Settings saved" : "");
        lv_obj_set_style_text_color(state->summary, kGreen, 0);
      }
      RefreshTemperatureSettings(state);
      RefreshStatusBars();
    }, LV_EVENT_ALL, state);
  }
  state->list = main_list;
  auto *reset = Button(main_list, "Reset SysInfo", [state] {
    Sheet(state->screen, "Reset SysInfo", "Restore default SysInfo settings?", [state] {
      if (!RecoverySetTemperatureDisplay({})) {
        Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
        return;
      }
      state->temperature_saved = RecoveryTemperatureDisplay();
      RefreshTemperatureSettings(state);
      lv_obj_update_layout(state->list);
      lv_obj_scroll_to_y(state->list, 0, LV_ANIM_OFF);
      RefreshStatusBars();
    }, 0, false, SheetPresentation::kStandard, "Swipe to confirm");
  });
  lv_obj_set_size(reset, LV_PCT(100), 124);
  state->summary = Label(main_list, "", &lv_font_montserrat_24, kMuted);
  lv_obj_set_width(state->summary, LV_PCT(100));
  RefreshTemperatureSettings(state);
}

void BuildStatusBarSettings(Tools *state) {
  Header(state->screen, "Status bar", "", state->callback, state->context);
  const int top = Landscape(state->screen) ? 340 : 452;
  state->list = Scroll(state->screen, top,
      std::max(1, int(lv_obj_get_height(state->screen)) - top - NavigationHeight(state->screen) - 28));
  lv_obj_set_flex_flow(state->list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(state->list, 16, 0);
  lv_obj_set_style_pad_bottom(state->list, 32, 0);
  PreferenceSection(state, "Battery style");
  auto *choices = lv_obj_create(state->list);
  Clear(choices);
  lv_obj_set_size(choices, LV_PCT(100), 248);
  lv_obj_set_flex_flow(choices, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(choices, 24, 0);
  const auto current_style = RecoveryBatteryStyle();
  for (int i = 0; i < 2; ++i) {
    auto *card = Button(choices, "", {}, current_style == static_cast<BatteryStyle>(i));
    state->battery_choices[i] = card;
    Panel(card, 28, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_size(card, 1, 248);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_pad_all(card, 0, 0);
    const bool selected = current_style == static_cast<BatteryStyle>(i);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? kAccent : kMainLine, 0);
    auto *name = Label(card, i == 0 ? "Classic" : "Circle", &lv_font_montserrat_32, kText);
    lv_obj_set_width(name, LV_PCT(86));
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 150);
    auto *check = Label(card, LV_SYMBOL_OK, &lv_font_montserrat_24, kAccent);
    state->battery_checks[i] = check;
    lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -24, 22);
    if (!selected) lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
    if (i == 1) {
      const auto preview = CreateCircleBattery(card);
      state->battery_preview_circle = preview;
      UpdateCircleBattery(preview, 76, false);
      lv_obj_align(preview.root, LV_ALIGN_TOP_MID, 0, 28);
    } else {
      auto *preview = lv_obj_create(card);
      state->battery_preview_classic = preview;
      Clear(preview);
      lv_obj_set_size(preview, 188, 80);
      lv_obj_align(preview, LV_ALIGN_TOP_MID, 0, 28);
      auto *number = Label(preview, "76%", &lv_font_montserrat_32, kMutedStrong);
      state->battery_preview_percentage = number;
      lv_obj_set_pos(number, 0, 22);
      auto *body = lv_obj_create(preview);
      state->battery_preview_body = body;
      Clear(body);
      lv_obj_set_pos(body, 100, 22);
      lv_obj_set_size(body, 68, 36);
      lv_obj_set_style_radius(body, 6, 0);
      lv_obj_set_style_border_width(body, 2, 0);
      lv_obj_set_style_border_color(body, kMutedStrong, 0);
      auto *fill = lv_obj_create(body);
      Clear(fill);
      lv_obj_set_pos(fill, 3, 3);
      lv_obj_set_size(fill, 44, 26);
      lv_obj_set_style_radius(fill, 3, 0);
      lv_obj_set_style_bg_color(fill, kText, 0);
      lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
      auto *tip = lv_obj_create(preview);
      state->battery_preview_tip = tip;
      Clear(tip);
      lv_obj_set_pos(tip, 171, 32);
      lv_obj_set_size(tip, 6, 16);
      lv_obj_set_style_radius(tip, 2, 0);
      lv_obj_set_style_bg_color(tip, kMutedStrong, 0);
      lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    }
    OnClick(card, [state, i] {
      if (!RecoverySetBatteryStyle(static_cast<BatteryStyle>(i))) {
        Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
        return;
      }
      for (int j = 0; j < 2; ++j) {
        lv_obj_set_style_border_width(state->battery_choices[j], i == j ? 3 : 1, 0);
        lv_obj_set_style_border_color(state->battery_choices[j], i == j ? kAccent : kMainLine, 0);
        if (i == j) lv_obj_remove_flag(state->battery_checks[j], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(state->battery_checks[j], LV_OBJ_FLAG_HIDDEN);
      }
      RefreshBatterySettings(state);
      RefreshStatusBars();
    });
  }
  auto *percentage_row = PreferenceEntry(state, LV_SYMBOL_BATTERY_FULL, "Battery percentage", "", {}, true);
  state->battery_percentage_row = percentage_row;
  auto *percentage_toggle = lv_switch_create(percentage_row);
  StyleSwitch(percentage_toggle, RecoveryBatteryPercentage());
  OnClick(percentage_row, [state, percentage_toggle] {
    const bool show = !RecoveryBatteryPercentage();
    if (!RecoverySetBatteryPercentage(show)) {
      Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
      return;
    }
    StyleSwitch(percentage_toggle, show);
    RefreshBatterySettings(state);
    RefreshStatusBars();
  });

  auto *size_panel = lv_obj_create(state->list);
  state->battery_size_panel = size_panel;
  Clear(size_panel);
  PreferenceColumn(size_panel, 12);
  lv_obj_set_style_pad_all(size_panel, 24, 0);
  auto *size_heading = lv_obj_create(size_panel);
  Clear(size_heading);
  lv_obj_set_size(size_heading, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(size_heading, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(size_heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  auto *size_title = Label(size_heading, "Circle size", &lv_font_montserrat_32, kText);
  lv_obj_set_width(size_title, 0);
  lv_obj_set_flex_grow(size_title, 1);
  lv_label_set_long_mode(size_title, LV_LABEL_LONG_WRAP);
  state->battery_size_amount = Label(size_heading, "", &lv_font_montserrat_24, kAccent);
  lv_obj_set_width(state->battery_size_amount, 130);
  lv_obj_set_style_text_align(state->battery_size_amount, LV_TEXT_ALIGN_RIGHT, 0);
  auto *size_track = lv_obj_create(size_panel);
  Clear(size_track);
  lv_obj_set_size(size_track, LV_PCT(100), 96);
  auto *size_slider = lv_slider_create(size_track);
  state->battery_size_slider = size_slider;
  lv_obj_set_size(size_slider, LV_PCT(96), 24);
  lv_obj_center(size_slider);
  lv_slider_set_range(size_slider, kBatteryCircleSizeMin, kBatteryCircleSizeMax);
  RangeSlider(size_slider);
  state->battery_saved_size = RecoveryBatteryCircleSize();
  lv_obj_add_event_cb(size_slider, [](lv_event_t *event) {
    const auto code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    auto *state = static_cast<Tools *>(lv_event_get_user_data(event));
    const int size = code == LV_EVENT_PRESS_LOST ? state->battery_saved_size :
        lv_slider_get_value(lv_event_get_target_obj(event));
    const bool save = code == LV_EVENT_RELEASED;
    if (RecoverySetBatteryCircleSize(size, save)) {
      if (save) state->battery_saved_size = size;
      i18n::BindLabel(state->summary, save ? "Settings saved" : "");
      lv_obj_set_style_text_color(state->summary, kGreen, 0);
    } else {
      RecoverySetBatteryCircleSize(state->battery_saved_size, false);
      i18n::BindLabel(state->summary, "This setting could not be changed.");
      lv_obj_set_style_text_color(state->summary, kRed, 0);
    }
    RefreshBatterySettings(state);
    RefreshStatusBars();
  }, LV_EVENT_ALL, state);
  PreferenceSection(state, "Network");
  const auto display = RecoveryWifiStatusDisplay();
  auto *speed_row = PreferenceEntry(state, LV_SYMBOL_WIFI, "Network speed", "", {}, true);
  auto *speed_toggle = lv_switch_create(speed_row);
  StyleSwitch(speed_toggle, display.show_speed);
  OnClick(speed_row, [state, speed_toggle] {
    auto display = RecoveryWifiStatusDisplay();
    display.show_speed = !display.show_speed;
    if (!RecoverySetWifiStatusDisplay(display)) {
      Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
      return;
    }
    StyleSwitch(speed_toggle, display.show_speed);
    RefreshStatusBars();
  });
  auto *name_row = PreferenceEntry(state, LV_SYMBOL_WIFI, "Show network name", "", {}, true);
  auto *name_toggle = lv_switch_create(name_row);
  StyleSwitch(name_toggle, display.show_name);
  OnClick(name_row, [state, name_toggle] {
    auto display = RecoveryWifiStatusDisplay();
    display.show_name = !display.show_name;
    if (!RecoverySetWifiStatusDisplay(display)) {
      Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
      return;
    }
    StyleSwitch(name_toggle, display.show_name);
    RefreshStatusBars();
  });
  PreferenceEntry(state, LV_SYMBOL_SETTINGS, "SysInfo", "",
                  [state] { Open(state, Action::kTemperatures); });
  state->summary = Label(state->list, "", &lv_font_montserrat_24, kMuted);
  lv_obj_set_width(state->summary, LV_PCT(100));
  RefreshBatterySettings(state);
}

void RefreshPcConnection(Tools *state) {
  const auto pc = aera::pc::GetStatus();
  const std::string signature = std::to_string(pc.enabled) + "|" + pc.address +
      "|" + pc.ip_address + "|" + pc.certificate;
  if (signature == state->pc_signature) return;
  state->pc_signature = signature;
  StyleSwitch(state->pc_switch, pc.enabled);
  const std::string address = !pc.enabled ? i18n::Translate("Off") :
      pc.ip_address.empty() ? i18n::Translate("Not connected") :
      pc.address == pc.ip_address ? pc.address : pc.address + "\n" + pc.ip_address;
  lv_label_set_text(state->pc_address, address.c_str());
  lv_label_set_text(state->pc_certificate, pc.enabled ? pc.certificate.c_str() : "");
  lv_obj_update_layout(state->pc_details);
  lv_obj_set_y(state->pc_forget,
      lv_obj_get_y(state->pc_details) + lv_obj_get_height(state->pc_details) + 32);
}

void BuildPcConnection(Tools *state) {
  Header(state->screen, "PC connection",
         "Transfer and install ZIPs and images from your computer",
         state->callback, state->context);
  const int top = Landscape(state->screen) ? 340 : 480;
  state->list = Scroll(state->screen, top,
      std::max(1, static_cast<int>(lv_obj_get_height(state->screen)) - top -
          NavigationHeight(state->screen) - 28));
  lv_obj_set_style_pad_bottom(state->list, 24, 0);
  auto *message = Label(state->list, "Changes apply immediately",
                        &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(message, 24, 364);
  lv_obj_set_width(message, LV_PCT(95));
  auto *row = SettingsRow(state->list, 0, "PC connection",
      "Transfer and install ZIPs and images from your computer",
      aera::pc::GetStatus().enabled,
      [state](bool enabled) {
        const bool ok = aera::pc::SetEnabled(enabled);
        RefreshPcConnection(state);
        return ok;
      }, message);
  state->pc_switch = lv_obj_get_child(row, 2);
  SettingsRow(state->list, 180, "Auto-enable PC connection", "Start after unlocking",
      RecoveryPcAutoEnable(), [](bool enabled) { return RecoverySetPcAutoEnable(enabled); },
      message);
  state->pc_details = lv_obj_create(state->list);
  Clear(state->pc_details);
  lv_obj_set_pos(state->pc_details, 0, 432);
  lv_obj_set_size(state->pc_details, lv_obj_get_width(state->list), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(state->pc_details, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(state->pc_details, 18, 0);
  auto detail = [](lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
    auto *label = Label(parent, text, font, color);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
  };
  auto section = [state, &detail](const char *title) {
    auto *card = lv_obj_create(state->pc_details);
    Panel(card, 32, kMainPanel);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 32, 0);
    lv_obj_set_style_pad_row(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    detail(card, title, &lv_font_montserrat_28, kText);
    return card;
  };
  auto command = [](lv_obj_t *parent, const char *text, const char *symbol) {
    auto *card = lv_obj_create(parent);
    Panel(card, 26, kMainPanel);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 128, 0);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_style_pad_column(card, 26, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Label(card, symbol, &lv_font_montserrat_28, kAccent);
    auto *label = Label(card, text, &lv_font_montserrat_36, kMutedStrong);
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
  };
  auto *usb = section("USB");
  detail(usb, "Run on your computer", &lv_font_montserrat_24, kMuted);
  command(usb, "adb -d forward tcp:8443 tcp:443", LV_SYMBOL_RIGHT);
  detail(usb, "Open in your browser", &lv_font_montserrat_24, kMuted);
  command(usb, "https://localhost:8443", LV_SYMBOL_RIGHT);
  auto *wifi = section("Wi-Fi");
  detail(wifi, "Open in your browser", &lv_font_montserrat_24, kMuted);
  state->pc_address = command(wifi, "", LV_SYMBOL_RIGHT);
  auto *certificate = section("SHA-256");
  state->pc_certificate = detail(certificate, "", &lv_font_montserrat_24, kMuted);
  state->pc_forget = Button(state->list, "Forget remembered computers", [state] {
    Sheet(state->screen, "Forget remembered computers", "Remove all saved PC connection approvals?",
        [state] {
          if (!aera::pc::ForgetComputers())
            Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
        });
  });
  lv_obj_set_size(state->pc_forget, lv_obj_get_width(state->list), 124);
  Panel(state->pc_forget, 32, kMainPanel);
  RefreshPcConnection(state);
  state->pc_timer = lv_timer_create([](lv_timer_t *timer) {
    RefreshPcConnection(static_cast<Tools *>(lv_timer_get_user_data(timer)));
  }, 500, state);
}

void BuildLogs(Tools *state) {
  Header(state->screen, "Recovery log", "Latest output from this recovery session.", state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 490 : 620,
                       landscape ? 760 : 2130);
  auto *text = Label(state->list, ReadLog().c_str(), &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_width(text, 1270);
  auto *refresh = Button(state->screen, "Refresh", [state, text] {
    i18n::BindLabel(text, ReadLog().c_str());
    lv_obj_scroll_to_y(state->list, LV_COORD_MAX, LV_ANIM_OFF);
  });
  lv_obj_set_pos(refresh, 80, landscape ? 340 : 452);
  lv_obj_set_size(refresh, landscape ? 560 : 1280, 120);
}

void BuildLanguage(Tools *state) {
  Header(state->screen, "Language",
         "Mostly machine-translated. Please report mistakes.",
         state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  const int top = landscape ? 340 : 452;
  state->list = Scroll(state->screen, top,
      std::max(1, static_cast<int>(lv_obj_get_height(state->screen)) - top -
          NavigationHeight(state->screen) - 28));
  lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_ON);
  lv_obj_set_style_width(state->list, 14, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(state->list, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(state->list, LV_RADIUS_CIRCLE,
                          LV_PART_SCROLLBAR);
  lv_obj_update_layout(state->list);
  const int row_width =
      std::max(600, static_cast<int>(lv_obj_get_width(state->list)) - 28);
  const auto &languages = i18n::AvailableLanguages();
  for (size_t i = 0; i < languages.size(); ++i) {
    const auto language = languages[i];
    const bool selected =
        std::string(i18n::CurrentLanguage()) == language.code;
    auto *row = Button(state->list, "", [] {}, selected);
    lv_obj_set_pos(row, 0, static_cast<int>(i) * 154);
    lv_obj_set_size(row, row_width, 142);
    lv_obj_set_style_radius(row, 22, 0);

    auto *native = Label(row, language.native_name, &lv_font_montserrat_32,
                         kText);
    lv_obj_set_pos(native, 28, 20);
    lv_obj_set_width(native, row_width - 180);
    lv_label_set_long_mode(native, LV_LABEL_LONG_DOT);
    auto *english = Label(row, language.name, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(english, 30, 80);
    lv_obj_set_width(english, row_width - 180);
    if (selected) {
      auto *check = Label(row, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
      lv_obj_align(check, LV_ALIGN_RIGHT_MID, -38, 0);
    }
    OnClick(row, [state, language] {
      if (!RecoverySetLanguage(language.code) ||
          !i18n::SetLanguage(language.code)) {
        Sheet(state->screen, "Language unavailable",
              "AERA could not apply the selected language.");
        return;
      }
      if (AutoSaveSettings(state)) Open(state, Action::kLanguage);
    });
  }
}

const char *CredentialName(int type) {
  if (type == 2) return "Pattern";
  if (type == 3) return "PIN";
  if (type == 0) return "No credential";
  return "Password";
}

void BuildUsers(Tools *state) {
  Header(state->screen, "Android Users",
         "Unlock additional Android users only when you need them.",
         state->callback, state->context);
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 452,
                       landscape ? 930 : 2290);
  const auto users = RecoveryAndroidUsers();
  lv_obj_update_layout(state->list);
  const int row_width =
      std::max(600, static_cast<int>(lv_obj_get_width(state->list)));
  for (size_t i = 0; i < users.size(); ++i) {
    const auto user = users[i];
    auto *row = Button(state->list, "", [] {});
    lv_obj_set_pos(row, 0, static_cast<int>(i) * 170);
    lv_obj_set_size(row, row_width, 154);
    lv_obj_set_style_radius(row, 24, 0);
    auto *name = Label(row, user.name.c_str(), &lv_font_montserrat_36, kText);
    lv_obj_set_pos(name, 28, 20);
    lv_obj_set_width(name, row_width - 330);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const std::string detail = i18n::Format(
        "User %d / %s", user.id, CredentialName(user.credential_type));
    auto *credential =
        Label(row, detail.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(credential, 30, 82);
    lv_obj_set_width(credential, row_width - 330);
    auto *status = Label(row, user.decrypted ? "Unlocked" : "Unlock",
                         &lv_font_montserrat_24,
                         user.decrypted ? kGreen : kAccent);
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, -68, 0);
    if (user.decrypted) {
      lv_obj_add_state(row, LV_STATE_DISABLED);
      lv_obj_set_style_opa(row, LV_OPA_80, LV_STATE_DISABLED);
    } else {
      auto *arrow =
          Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kAccent);
      lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -22, 0);
      OnClick(row, [state, user] {
        SetUserDecryptRequest({user});
        Open(state, Action::kDecryptUser);
      });
    }
  }
  if (users.empty()) {
    auto *empty = Label(
        state->list,
        "No Android users were found. Unlock /data first, then try again.",
        &lv_font_montserrat_32, kMuted);
    lv_obj_set_width(empty, row_width - 40);
    lv_obj_set_pos(empty, 20, 80);
  }
}

void BuildMenu(Tools *state) {
  Header(state->screen, "Menu", "Tools for your recovery session.",
         state->callback, state->context);
  // Utility cards scroll. About and Reboot are full-width destinations pinned
  // above Navigation, with About immediately above the final Reboot action.
  const bool landscape = Landscape(state->screen);
  state->list = Scroll(state->screen, landscape ? 340 : 452,
                       landscape ? 438 : 1900);
  struct Item { const char *icon, *title, *detail; Action action; };
  const std::array<Item, 12> items{{
    {LV_SYMBOL_DRIVE, "Mounts", "Mount or unmount recovery volumes", Action::kMounts},
    {LV_SYMBOL_USB, "ADB Sideload", "Receive and install a ZIP package over USB", Action::kSideload},
    {LV_SYMBOL_LIST, "Recovery log", "Read output and troubleshoot operations", Action::kLogs},
    {LV_SYMBOL_WIFI, "Wi-Fi", "Networks, saved credentials and connection test", Action::kWifi},
    {LV_SYMBOL_SHUFFLE, "Network Storage", "Connect and mount SFTP or SMB storage", Action::kNas},
    {LV_SYMBOL_SETTINGS, "Root Manager", "Patch init_boot and manage KernelSU modules", Action::kRootManager},
    {LV_SYMBOL_KEYBOARD, "Android Users", "Unlock additional users on demand", Action::kUsers},
    {LV_SYMBOL_TINT, "Theme Engine", "Global accent colors and interface appearance", Action::kTheme},
    {LV_SYMBOL_SETTINGS, "Preferences", "Display, files, backups, time and USB", Action::kPreferences},
    {LV_SYMBOL_USB, "PC connection", "Transfer and install ZIPs and images from your computer", Action::kPcConnection},
    {"A", "About AERA", "Project identity, contributors and build information", Action::kAbout},
    {LV_SYMBOL_POWER, "Reboot", "Android, recovery, bootloader or power off", Action::kOpenReboot}}};
  size_t visible_index = 0;
  for (const auto item : items) {
    if (item.action == Action::kRootManager && !root::Enabled()) continue;
    const size_t i = visible_index++;
    const bool reboot = item.action == Action::kOpenReboot;
    const bool about = item.action == Action::kAbout;
    const bool pinned = about || reboot;
    const int columns = landscape ? 3 : 2;
    const int column = static_cast<int>(i % columns);
    const int row = static_cast<int>(i / columns);
    auto *card = lv_button_create(pinned ? state->screen : state->list);
    Panel(card, 34, kMainSheet);
    Interactive(card, kMainSelected);
    // These two deliberate destinations form a full-width stack immediately
    // above the four-button navigation dock.
    const int card_width = landscape ? 992 : 640;
    const int card_step = landscape ? 284 : 306;
    const int card_height = landscape ? 260 : 280;
    const int pinned_y = about ? (landscape ? 798 : 2398)
                               : (landscape ? 1010 : 2648);
    lv_obj_set_pos(card, pinned ? 64 : column * (landscape ? 1016 : 672),
                   pinned ? pinned_y : row * card_step);
    lv_obj_set_size(card, pinned ? (landscape ? 3040 : 1312) : card_width,
                    pinned ? (landscape ? 190 : about ? 218 : 232)
                           : card_height);
    lv_obj_set_style_transform_scale(card, 256, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, reboot ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, reboot ? kRed : kMainLine, 0);
    lv_obj_set_style_border_opa(card, reboot ? LV_OPA_60 : LV_OPA_30, 0);
    OnClick(card, [state, item] { Open(state, item.action); });

    auto *plate = lv_obj_create(card);
    Panel(plate, 22, IconBackground());
    lv_obj_set_pos(plate, 28, pinned ? (landscape ? 47 : about ? 55 : 68) : 28);
    lv_obj_set_size(plate, pinned ? 96 : 108, pinned ? 96 : 108);
    auto *icon = Label(plate, item.icon, &lv_font_montserrat_48,
                       reboot ? kRed : kAccent);
    lv_obj_center(icon);
    auto *title = Label(card, item.title, &lv_font_montserrat_36, kText);
    lv_obj_set_pos(title, pinned ? 152 : 164,
                   pinned ? (landscape ? 36 : about ? 42 : 40) : 42);
    lv_obj_set_width(title, pinned ? (landscape ? 2750 : 1040)
                                  : card_width - 252);
    FitLabelToLines(title,
                    pinned ? (landscape ? 2750 : 1040)
                           : card_width - 252,
                    1, {&lv_font_montserrat_36, &lv_font_montserrat_32,
                        &lv_font_montserrat_28, &lv_font_montserrat_24,
                        &lv_font_montserrat_20, &lv_font_montserrat_18,
                        &lv_font_montserrat_16});
    auto *detail = Label(card, item.detail, &lv_font_montserrat_32, kMuted);
    lv_obj_set_pos(detail, pinned ? 152 : 30,
                   pinned ? (landscape ? 112 : about ? 122 : 126)
                          : (landscape ? 150 : 164));
    FitLabelToLines(detail,
                    pinned ? (landscape ? 2750 : 1040)
                           : card_width - 100,
                    2, {&lv_font_montserrat_32, &lv_font_montserrat_28,
                        &lv_font_montserrat_24, &lv_font_montserrat_20,
                        &lv_font_montserrat_18, &lv_font_montserrat_16});
    auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_36,
                        reboot ? kRed : kMutedStrong);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -30, 0);
    AnimateEnter(card, 30 + static_cast<uint32_t>(i) * 28, 12);
  }
}
}  // namespace

void SetUserDecryptRequest(const UserDecryptRequest &request) {
  selected_user_decrypt = request;
}

UserDecryptRequest GetUserDecryptRequest() {
  return selected_user_decrypt;
}

void BuildToolScene(lv_obj_t *screen, Action tool, ActionCallback callback, void *context) {
  auto *state = new Tools;
  state->screen = screen;
  state->tool = tool;
  state->compression = RecoveryPreference(Preference::kCompression);
  state->callback = callback;
  state->context = context;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    auto *state = static_cast<Tools *>(lv_event_get_user_data(event));
    if (state->pc_timer) lv_timer_delete(state->pc_timer);
    if (state->tool == Action::kStatusBar) {
      RecoverySetBatteryCircleSize(state->battery_saved_size, false);
      RefreshStatusBars();
    }
    if (state->tool == Action::kTemperatures) {
      RecoverySetTemperatureDisplay(state->temperature_saved, false);
      RefreshStatusBars();
    }
    delete state;
  }, LV_EVENT_DELETE, state);
  if (tool == Action::kBackup || tool == Action::kWipe || tool == Action::kRestore)
    BuildPartitions(state);
  else if (tool == Action::kFormatData) BuildFormatData(state);
  else if (tool == Action::kMounts) BuildMounts(state);
  else if (tool == Action::kPreferences) BuildPreferences(state);
  else if (tool == Action::kLanguage) BuildLanguage(state);
  else if (tool == Action::kStatusBar) BuildStatusBarSettings(state);
  else if (tool == Action::kTemperatures) BuildTemperatureSettings(state);
  else if (tool == Action::kPcConnection) BuildPcConnection(state);
  else if (tool == Action::kTheme) BuildTheme(state);
  else if (tool == Action::kLogs) BuildLogs(state);
  else if (tool == Action::kUsers) BuildUsers(state);
  else BuildMenu(state);
  state->navigation =
      Navigation(screen,
                 tool == Action::kBackup || tool == Action::kRestore
                     ? Action::kBackup
                     : tool == Action::kWipe || tool == Action::kFormatData
                           ? Action::kWipe
                           : Action::kSettings,
                 callback, context);
}

void BuildFastbootFormatScene(lv_obj_t *screen, ActionCallback callback,
                              void *context) {
  auto *state = new Tools;
  state->screen = screen;
  state->tool = Action::kFormatData;
  state->callback = callback;
  state->context = context;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<Tools *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, state);
  BuildFormatData(state, true);
}
}  // namespace aeraui
