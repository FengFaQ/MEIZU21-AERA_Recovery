/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include "file_manager.hpp"
#include "phone_keyboard.hpp"
#include "picture_decode.hpp"
#include "picture_viewer.hpp"
#include "scene.hpp"
#include "ui_components.hpp"
#include "package_review.hpp"

namespace aeraui {
namespace {
using namespace widgets;
enum class SortMode { kName, kSize, kDate, kType };
enum class ClipboardMode { kNone, kCopy, kCut };
struct Clipboard {
  ClipboardMode mode = ClipboardMode::kNone;
  std::vector<std::string> paths;
};
struct Entry {
  std::string name, path;
  bool directory = false;
  bool symlink = false;
  bool text = false;
  uint64_t bytes = 0;
  mode_t mode = 0;
  uid_t uid = 0;
  gid_t gid = 0;
  std::time_t modified = 0;
};
struct Files {
  lv_obj_t* screen = nullptr;
  lv_obj_t* list = nullptr;
  lv_obj_t* path_label = nullptr;
  lv_obj_t* summary = nullptr;
  lv_obj_t* storage_label = nullptr;
  lv_obj_t* action_bar = nullptr;
  lv_obj_t* work_overlay = nullptr;
  lv_obj_t* work_title = nullptr;
  lv_obj_t* work_detail = nullptr;
  lv_obj_t* work_bar = nullptr;
  lv_obj_t* work_cancel = nullptr;
  lv_timer_t* work_timer = nullptr;
  ActionCallback callback = nullptr;
  void* context = nullptr;
  std::vector<Entry> entries;
  size_t visible = 100;
  bool selecting = false;
  std::set<std::string> selected;
  file_manager::Progress work;
  std::thread worker;
  std::function<void(const file_manager::Snapshot&)> work_finished;
  bool work_refresh = true;
  bool clear_clipboard_after_work = false;

  ~Files() {
    work.cancel.store(true);
    if (work_timer != nullptr) lv_timer_delete(work_timer);
    if (worker.joinable()) worker.join();
  }
};
Files *gFiles = nullptr;
std::string gDirectory;
SortMode gSortMode = SortMode::kName;
bool gSortAscending = true;
Clipboard gClipboard;
constexpr uint64_t kTextPreviewBytes = 128 * 1024;
constexpr uint64_t kTextEditBytes = 128 * 1024;
constexpr uint64_t kTextEditorRenderBytes = 8 * 1024;
constexpr size_t kTextPageBytes = 4 * 1024;
constexpr size_t kTextPageLines = 100;

struct FileListMetrics {
  int row_height;
  int row_pitch;
  int text_x;
  int icon_y;
  int name_y;
  int detail_y;
  const lv_font_t* icon_font;
  const lv_font_t* name_font;
  const lv_font_t* detail_font;
};

FileListMetrics FileListLayout() {
  switch (RecoveryInterfaceSize()) {
    case InterfaceSize::kSmall:
      return {150, 156, 108, 53, 27, 82, &lv_font_montserrat_32,
              &lv_font_montserrat_32, &lv_font_montserrat_24};
    case InterfaceSize::kSpacious:
      return {264, 272, 144, 102, 46, 148, &lv_font_montserrat_48,
              &lv_font_montserrat_48, &lv_font_montserrat_36};
    case InterfaceSize::kLarge:
      return {228, 236, 128, 84, 38, 126, &lv_font_montserrat_48,
              &lv_font_montserrat_40, &lv_font_montserrat_32};
    case InterfaceSize::kNormal:
    default:
      return {174, 180, 116, 63, 34, 96, &lv_font_montserrat_32,
              &lv_font_montserrat_32, &lv_font_montserrat_24};
  }
}

bool Zip(const std::string &name) {
  if (name.size() < 4) return false;
  std::string suffix = name.substr(name.size() - 4);
  for (char &c : suffix) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return suffix == ".zip";
}
bool Image(const std::string &name) {
  if (name.size() < 4) return false;
  std::string suffix = name.substr(name.size() - 4);
  for (char &c : suffix)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return suffix == ".img";
}
std::string Lower(std::string value) {
  for (char &character : value)
    character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
  return value;
}

std::string SuggestedImageTarget(const std::string &name) {
  const std::string lower = Lower(name);
  if (lower.find("init_boot") != std::string::npos) return "/init_boot";
  if (lower.find("vendor_boot") != std::string::npos) return "/vendor_boot";
  if (lower.find("system_ext") != std::string::npos) return "/system_ext";
  if (lower.find("system_dlkm") != std::string::npos) return "/system_dlkm";
  if (lower.find("vendor_dlkm") != std::string::npos) return "/vendor_dlkm";
  if (lower.find("odm_dlkm") != std::string::npos) return "/odm_dlkm";
  if (lower.find("my_engineering") != std::string::npos) return "/my_engineering";
  if (lower.find("my_manifest") != std::string::npos) return "/my_manifest";
  if (lower.find("my_product") != std::string::npos) return "/my_product";
  if (lower.find("my_company") != std::string::npos) return "/my_company";
  if (lower.find("my_carrier") != std::string::npos) return "/my_carrier";
  if (lower.find("my_region") != std::string::npos) return "/my_region";
  if (lower.find("my_bigball") != std::string::npos) return "/my_bigball";
  if (lower.find("my_heytap") != std::string::npos) return "/my_heytap";
  if (lower.find("my_stock") != std::string::npos) return "/my_stock";
  if (lower.find("my_preload") != std::string::npos) return "/my_preload";
  if (lower.find("product") != std::string::npos) return "/product";
  if (lower.find("vendor") != std::string::npos) return "/vendor";
  if (lower.find("system") != std::string::npos) return "/system";
  if (lower.find("odm") != std::string::npos) return "/odm";
  if (lower.find("abl") != std::string::npos) return "/abl";
  if (lower.find("dtbo") != std::string::npos) return "/dtbo";
  if (lower.find("recovery") != std::string::npos ||
      lower.find("twrp") != std::string::npos ||
      lower.find("orangefox") != std::string::npos ||
      lower.find("aera") != std::string::npos) return "/recovery";
  if (lower.find("boot") != std::string::npos) return "/boot";
  return {};
}

const char *ImageTargetDescription(const std::string &path) {
  if (path == "/init_boot") return "Early Android ramdisk and root patches";
  if (path == "/boot") return "Kernel and Android boot ramdisk";
  if (path == "/vendor_boot") return "Vendor ramdisk and device boot components";
  if (path == "/dtbo") return "Device-tree overlays used by the kernel";
  if (path == "/recovery") return "AERA or another compatible recovery image";
  if (path == "/abl") return "Android bootloader image - device-specific and high risk";
  if (path == "/system" || path == "/system_root")
    return "Android operating-system image in Super";
  if (path == "/system_ext") return "Android system extensions in Super";
  if (path == "/system_dlkm") return "System kernel modules in Super";
  if (path == "/vendor") return "Hardware vendor image in Super";
  if (path == "/vendor_dlkm") return "Vendor kernel modules in Super";
  if (path == "/product") return "Product applications and configuration in Super";
  if (path == "/odm") return "Device-manufacturer image in Super";
  if (path == "/odm_dlkm") return "ODM kernel modules in Super";
  if (path.rfind("/my_", 0) == 0) return "Device-specific logical image in Super";
  return "Raw flashable partition";
}

std::string Parent(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  const auto slash = path.find_last_of('/');
  return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}
void Populate(Files *state);
void RenderEntries(Files* state);
void UpdateActionBar(Files* state);
void OpenFile(Files* state, const Entry& entry);

struct MenuAction {
  const char* label;
  const char* icon;
  Handler action;
  bool destructive = false;
};

struct ChoiceControl {
  lv_obj_t* button = nullptr;
  lv_obj_t* marker = nullptr;
};

struct PressActions {
  Handler click;
  Handler hold;
  bool held = false;
};

void OnClickOrHold(lv_obj_t* object, Handler click, Handler hold) {
  auto* actions = new PressActions{ std::move(click), std::move(hold), false };
  lv_obj_add_event_cb(
      object,
      [](lv_event_t* event) {
        auto* actions = static_cast<PressActions*>(lv_event_get_user_data(event));
        switch (lv_event_get_code(event)) {
          case LV_EVENT_DELETE:
            delete actions;
            break;
          case LV_EVENT_PRESSED:
            actions->held = false;
            break;
          case LV_EVENT_LONG_PRESSED:
            actions->held = true;
            RecoveryVibrate(Haptic::kTouch);
            actions->hold();
            break;
          case LV_EVENT_CLICKED:
            if (actions->held) {
              actions->held = false;
              break;
            }
            RecoveryVibrate(Haptic::kTouch);
            actions->click();
            break;
          default:
            break;
        }
      },
      LV_EVENT_ALL, actions);
}

void CloseOverlay(lv_obj_t* overlay) {
  if (overlay != nullptr) lv_obj_delete_async(overlay);
}

void SetChoiceSelected(const ChoiceControl& control, bool selected) {
  if (selected) {
    lv_obj_add_state(control.button, LV_STATE_CHECKED);
    lv_obj_remove_flag(control.marker, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_state(control.button, LV_STATE_CHECKED);
    lv_obj_add_flag(control.marker, LV_OBJ_FLAG_HIDDEN);
  }
}

void ActionSheet(Files* state, const std::string& title, const std::string& detail,
                 std::vector<MenuAction> actions) {
  auto* overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      overlay,
      [](lv_event_t* event) {
        if (lv_event_get_target_obj(event) ==
            lv_event_get_current_target_obj(event)) {
          CloseOverlay(lv_event_get_current_target_obj(event));
        }
      },
      LV_EVENT_CLICKED, nullptr);

  const bool landscape = Landscape(state->screen);
  const int width =
      landscape ? std::min(1900, static_cast<int>(lv_obj_get_width(state->screen)) - 128) : 1312;
  const int rows = static_cast<int>((actions.size() + 1) / 2);
  const int height = std::min(landscape ? 1160 : 1280, 360 + rows * 150);
  auto* sheet = lv_obj_create(overlay);
  Panel(sheet, 44, kMainSheet);
  lv_obj_set_size(sheet, width, height);
  lv_obj_align(sheet, landscape ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0, landscape ? 0 : -40);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_30, 0);

  auto* heading = Label(sheet, title.c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 48, 42);
  SingleLineLabel(heading, width - 190, &lv_font_montserrat_48);
  auto* subtitle = Label(sheet, detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, 48, 112);
  SingleLineLabel(subtitle, width - 96, &lv_font_montserrat_24);
  auto* close = Button(sheet, LV_SYMBOL_CLOSE, [overlay] { CloseOverlay(overlay); });
  lv_obj_set_pos(close, width - 138, 28);
  lv_obj_set_size(close, 98, 98);

  const int gap = 20;
  const int button_width = (width - 96 - gap) / 2;
  for (size_t index = 0; index < actions.size(); ++index) {
    MenuAction action = std::move(actions[index]);
    const int column = static_cast<int>(index % 2);
    const int row = static_cast<int>(index / 2);
    auto* button = Button(sheet, "", [overlay, action] {
      CloseOverlay(overlay);
      action.action();
    });
    lv_obj_set_pos(button, 48 + column * (button_width + gap), 186 + row * 150);
    lv_obj_set_size(button, button_width, 140);
    lv_obj_set_style_radius(button, 26, 0);
    if (action.destructive) {
      lv_obj_set_style_border_width(button, 1, 0);
      lv_obj_set_style_border_color(button, kRed, 0);
      lv_obj_set_style_border_opa(button, LV_OPA_60, 0);
    }
    auto* icon =
        Label(button, action.icon, &lv_font_montserrat_36, action.destructive ? kRed : kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 28, 0);
    auto* label =
        Label(button, action.label, &lv_font_montserrat_32, action.destructive ? kRed : kText);
    FitLabelToLines(label, button_width - 116, 1,
                    { &lv_font_montserrat_32, &lv_font_montserrat_28,
                      &lv_font_montserrat_24 });
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 92, 0);
  }
  AnimateEnter(sheet, 0, 34);
}

using NameHandler = std::function<bool(const std::string&, std::string&)>;

void NameDialog(Files* state, const char* title, const char* placeholder,
                const std::string& initial, const char* confirm, NameHandler handler) {
  auto* overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
  const bool landscape = Landscape(state->screen);
  const int screen_width = lv_obj_get_width(state->screen);
  const int width = landscape ? std::min(1900, screen_width - 128)
                              : std::min(1312, screen_width - 80);
  const int height = landscape ? 1250 : 1450;
  const int margin = 48;
  const int button_height = 126;
  auto* sheet = lv_obj_create(overlay);
  Panel(sheet, 44, kMainSheet);
  lv_obj_set_size(sheet, width, height);
  lv_obj_align(sheet, landscape ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0, landscape ? 0 : -40);
  lv_obj_set_style_pad_all(sheet, 0, 0);

  auto* heading = Label(sheet, title, &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, margin, 42);
  lv_obj_set_width(heading, width - 2 * margin);
  auto* input = TextArea(sheet);
  lv_obj_set_pos(input, margin, 120);
  lv_obj_set_size(input, width - 2 * margin, 126);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_max_length(input, NAME_MAX);
  lv_textarea_set_placeholder_text(input, i18n::Translate(placeholder));
  lv_textarea_set_text(input, initial.c_str());
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_bg_color(input, kMainPanel, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(input, 2, 0);
  lv_obj_set_style_radius(input, 24, 0);
  lv_obj_set_style_pad_all(input, 28, 0);
  auto* error = Label(sheet, "", &lv_font_montserrat_24, kRed);
  lv_obj_set_pos(error, margin + 8, 264);
  lv_obj_set_width(error, width - 2 * margin - 16);

  auto* keyboard = lv_keyboard_create(sheet);
  phone_keyboard::Apply(keyboard);
  const int button_y = height - margin - button_height;
  const int keyboard_y = 320;
  const int keyboard_height = button_y - keyboard_y - 48;
  lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, margin, keyboard_y);
  lv_obj_set_size(keyboard, width - 2 * margin, keyboard_height);
  lv_keyboard_set_textarea(keyboard, input);

  auto* cancel = Button(sheet, "Cancel", [overlay] { CloseOverlay(overlay); });
  const int button_width = (width - 2 * margin - 20) / 2;
  lv_obj_set_pos(cancel, margin, button_y);
  lv_obj_set_size(cancel, button_width, button_height);
  auto* accept = Button(
      sheet, confirm,
      [state, input, error, overlay, handler] {
        const std::string name = lv_textarea_get_text(input);
        std::string message;
        if (!file_manager::ValidName(name, message) || !handler(name, message)) {
          i18n::BindLabel(error, message.c_str());
          return;
        }
        CloseOverlay(overlay);
        Populate(state);
      },
      true);
  lv_obj_set_pos(accept, margin + button_width + 20, button_y);
  lv_obj_set_size(accept, button_width, button_height);
  lv_obj_add_event_cb(
      keyboard,
      [](lv_event_t* event) {
        lv_obj_send_event(static_cast<lv_obj_t*>(lv_event_get_user_data(event)),
                          LV_EVENT_CLICKED, nullptr);
      },
      LV_EVENT_READY, accept);
  lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
  AnimateEnter(sheet, 0, 38);
}

void WorkTick(lv_timer_t* timer) {
  auto* state = static_cast<Files*>(lv_timer_get_user_data(timer));
  const file_manager::Snapshot snapshot = state->work.Get();
  i18n::BindLabel(state->work_title,
                  snapshot.status.empty() ? "Preparing" : snapshot.status.c_str());
  std::string detail = snapshot.detail;
  if (snapshot.total_bytes != 0) {
    detail += (detail.empty() ? "" : "\n") + i18n::Format("%s of %s",
                                                          Size(snapshot.completed_bytes).c_str(),
                                                          Size(snapshot.total_bytes).c_str());
  } else if (snapshot.total_items != 0) {
    detail += (detail.empty() ? "" : "\n") +
              i18n::Format("%zu of %zu items", snapshot.completed_items, snapshot.total_items);
  }
  i18n::BindLabel(state->work_detail, detail.c_str());
  lv_bar_set_value(state->work_bar, static_cast<int32_t>(snapshot.percent), LV_ANIM_ON);
  if (!snapshot.done) return;

  state->work_timer = nullptr;
  lv_timer_delete(timer);
  if (state->worker.joinable()) state->worker.join();
  auto finished = std::move(state->work_finished);
  if (state->work_overlay != nullptr) {
    lv_obj_delete_async(state->work_overlay);
    state->work_overlay = nullptr;
  }
  if (state->clear_clipboard_after_work && snapshot.success) {
    gClipboard = Clipboard{};
  } else if (state->clear_clipboard_after_work) {
    gClipboard.paths.erase(
        std::remove_if(gClipboard.paths.begin(), gClipboard.paths.end(),
                       [](const std::string& path) { return !file_manager::Exists(path); }),
        gClipboard.paths.end());
    if (gClipboard.paths.empty()) gClipboard = Clipboard{};
  }
  state->clear_clipboard_after_work = false;
  state->selected.clear();
  state->selecting = false;
  if (state->work_refresh) Populate(state);
  UpdateActionBar(state);
  if (finished) finished(snapshot);
  if (snapshot.errors != 0) {
    const std::string copy =
        i18n::Format("%zu error(s) occurred.\n\n%s", snapshot.errors,
                     snapshot.detail.empty() ? i18n::Translate("Some items could not be processed.")
                                             : snapshot.detail.c_str());
    Sheet(state->screen, "File operation incomplete", copy);
  }
}

void StartWork(Files* state, const char* title, const file_manager::Request& request, bool refresh,
               bool clear_clipboard,
               std::function<void(const file_manager::Snapshot&)> finished = {}) {
  if (state->worker.joinable() || state->work_overlay != nullptr) return;
  state->work.Reset();
  state->work_refresh = refresh;
  state->clear_clipboard_after_work = clear_clipboard;
  state->work_finished = std::move(finished);

  auto* overlay = lv_obj_create(state->screen);
  state->work_overlay = overlay;
  lv_obj_set_user_data(overlay, &kPersistentModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_add_event_cb(
      overlay,
      [](lv_event_t* event) {
        if (lv_event_get_code(event) == LV_EVENT_CANCEL) {
          auto* state = static_cast<Files*>(lv_event_get_user_data(event));
          state->work.cancel.store(true);
          if (state->work_cancel != nullptr)
            lv_obj_add_state(state->work_cancel, LV_STATE_DISABLED);
        }
      },
      LV_EVENT_ALL, state);

  const bool landscape = Landscape(state->screen);
  const int width = landscape ? 1760 : 1240;
  const int height = landscape ? 760 : 900;
  auto* panel = lv_obj_create(overlay);
  Panel(panel, 44, kMainSheet);
  lv_obj_set_size(panel, width, height);
  lv_obj_center(panel);
  lv_obj_set_style_pad_all(panel, 54, 0);
  auto* tag = Kicker(panel, "FILE OPERATION", kAccent);
  lv_obj_set_pos(tag, 0, 0);
  state->work_title = Label(panel, title, &lv_font_montserrat_48, kText);
  lv_obj_set_pos(state->work_title, 0, 68);
  lv_obj_set_width(state->work_title, width - 108);
  state->work_detail = Label(panel, "Preparing", &lv_font_montserrat_28, kMutedStrong);
  lv_obj_set_pos(state->work_detail, 0, 154);
  lv_obj_set_size(state->work_detail, width - 108, 220);
  lv_label_set_long_mode(state->work_detail, LV_LABEL_LONG_MODE_WRAP);
  state->work_bar = lv_bar_create(panel);
  lv_obj_set_pos(state->work_bar, 0, height - 300);
  lv_obj_set_size(state->work_bar, width - 108, 18);
  lv_bar_set_range(state->work_bar, 0, 100);
  lv_bar_set_value(state->work_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(state->work_bar, kInset, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->work_bar, kAccent, LV_PART_INDICATOR);
  state->work_cancel = Button(panel, "Cancel", [state] {
    state->work.cancel.store(true);
    if (state->work_cancel != nullptr) lv_obj_add_state(state->work_cancel, LV_STATE_DISABLED);
  });
  lv_obj_set_size(state->work_cancel, width - 108, 116);
  lv_obj_align(state->work_cancel, LV_ALIGN_BOTTOM_MID, 0, 0);

  state->worker = std::thread([state, request] { file_manager::Run(request, state->work); });
  state->work_timer = lv_timer_create(WorkTick, 100, state);
  AnimateEnter(panel, 0, 30);
}

void OpenImageTargetPicker(Files *state, const Entry &entry) {
  const auto targets = RecoveryImageVolumes();
  if (targets.empty()) {
    Sheet(state->screen, "No flashable partitions",
          "This device tree did not expose any partitions that safely accept raw images.");
    return;
  }

  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);

  const bool landscape = Landscape(state->screen);
  const int sheet_width = landscape
      ? std::min(2200, static_cast<int>(lv_obj_get_width(state->screen)) - 128)
      : 1312;
  const int sheet_height = landscape
      ? std::min(1280, static_cast<int>(lv_obj_get_height(state->screen)) - 80)
      : 2200;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, landscape ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID,
               0, landscape ? 0 : -40);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_20, 0);

  auto *grabber = lv_obj_create(sheet);
  Clear(grabber);
  lv_obj_set_size(grabber, 112, 8);
  lv_obj_align(grabber, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_radius(grabber, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(grabber, kMutedStrong, 0);
  lv_obj_set_style_bg_opa(grabber, LV_OPA_30, 0);

  auto *tag = Kicker(sheet, "SELECT TARGET PARTITION", kAccent);
  lv_obj_set_pos(tag, 48, 58);
  auto *title = Label(sheet, "Flash image", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 48, 118);
  const std::string subtitle_text =
      "Choose exactly where this raw image will be written. Active slot: " +
      RecoverySlot();
  auto *subtitle = Label(sheet, subtitle_text.c_str(),
                         &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(subtitle, 48, 188);

  auto *file = lv_obj_create(sheet);
  Panel(file, 26, kMainPanel);
  lv_obj_set_pos(file, 48, 264);
  lv_obj_set_size(file, sheet_width - 96, 210);
  auto *file_icon = Label(file, LV_SYMBOL_FILE, &lv_font_montserrat_48, kViolet);
  lv_obj_align(file_icon, LV_ALIGN_LEFT_MID, 32, 0);
  auto *file_name = Label(file, entry.name.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(file_name, 112, 40);
  SingleLineLabel(file_name, sheet_width - 280, &lv_font_montserrat_32);
  const std::string file_detail = Size(entry.bytes) + "  /  " + entry.path;
  auto *file_path = Label(file, file_detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(file_path, 112, 108);
  SingleLineLabel(file_path, sheet_width - 280, &lv_font_montserrat_24);

  const std::string active_slot = RecoverySlot();
  auto both_slots = std::make_shared<bool>(false);
  auto *slot_mode = lv_obj_create(sheet);
  Panel(slot_mode, 28, kInset);
  lv_obj_set_pos(slot_mode, 48, 504);
  lv_obj_set_size(slot_mode, sheet_width - 96, 136);
  const int mode_width = (sheet_width - 120) / 2;
  auto *current_slot = lv_button_create(slot_mode);
  Panel(current_slot, 22, kAccent);
  lv_obj_set_pos(current_slot, 12, 12);
  lv_obj_set_size(current_slot, mode_width, 112);
  const std::string current_slot_text =
      i18n::Format("Current slot %s", active_slot.c_str());
  auto *current_label = Label(current_slot, current_slot_text.c_str(),
                              &lv_font_montserrat_24, kOnAccent);
  lv_obj_center(current_label);
  auto *both_slot_button = lv_button_create(slot_mode);
  Panel(both_slot_button, 22, kInset);
  lv_obj_set_pos(both_slot_button, mode_width + 12, 12);
  lv_obj_set_size(both_slot_button, mode_width, 112);
  auto *both_label = Label(both_slot_button, "Both slots A + B",
                           &lv_font_montserrat_24, kMutedStrong);
  lv_obj_center(both_label);
  OnClick(current_slot, [both_slots, current_slot, current_label,
                         both_slot_button, both_label] {
    *both_slots = false;
    lv_obj_set_style_bg_color(current_slot, kAccent, 0);
    lv_obj_set_style_bg_opa(current_slot, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(current_label, kOnAccent, 0);
    lv_obj_set_style_bg_color(both_slot_button, kInset, 0);
    lv_obj_set_style_text_color(both_label, kMutedStrong, 0);
  });
  OnClick(both_slot_button, [both_slots, current_slot, current_label,
                             both_slot_button, both_label] {
    *both_slots = true;
    lv_obj_set_style_bg_color(current_slot, kInset, 0);
    lv_obj_set_style_text_color(current_label, kMutedStrong, 0);
    lv_obj_set_style_bg_color(both_slot_button, kAccent, 0);
    lv_obj_set_style_bg_opa(both_slot_button, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(both_label, kOnAccent, 0);
  });

  auto *list = lv_obj_create(sheet);
  Clear(list);
  lv_obj_set_pos(list, 48, 680);
  lv_obj_set_size(list, sheet_width - 96, sheet_height - 920);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 5, LV_PART_SCROLLBAR);

  auto select_target = [state, overlay, entry, both_slots](const Volume &target) {
      lv_obj_delete_async(overlay);
      JobRequest request;
      request.job = Job::kFlashImage;
      request.title = "Flash Image";
      request.path = entry.path;
      request.partitions = {target.path};
      request.both_slots = *both_slots && target.slot_select && !target.logical;
      const std::string slot_destination = request.both_slots
          ? i18n::Translate("Both slots A + B")
          : target.logical
              ? i18n::Format("Logical partition  /  slot %s",
                             RecoverySlot().c_str())
              : i18n::Format("Current slot %s", RecoverySlot().c_str());
      const std::string warning = i18n::Format(
          "Image\n%s\n\nTarget\n%s  /  %s\n\nDestination\n%s\n\n"
          "This writes directly to the selected partition. An incorrect image "
          "or target can prevent the device from booting.",
          entry.name.c_str(), target.name.c_str(), target.path.c_str(),
          slot_destination.c_str());
      const std::string title =
          i18n::Format("Flash to %s?", target.name.c_str());
      Sheet(state->screen, title, warning,
            [state, request] {
        SetJobRequest(request);
        state->callback(Action::kRunOperation, state->context);
      });
  };

  const int target_width = sheet_width - 96;
  auto add_target = [select_target, target_width](
      lv_obj_t *parent, int y, const Volume &target, bool recommended) {
    auto *row = lv_button_create(parent);
    Panel(row, 28, kMainPanel);
    Interactive(row, kMainSelected);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, target_width, 184);
    lv_obj_set_style_border_width(row, 0, 0);
    OnClick(row, [select_target, target] { select_target(target); });

    auto *icon = IconPlate(row, LV_SYMBOL_UPLOAD, kAccent,
                           recommended ? kAccentSoft : kMainSelected, 76);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 28, 0);
    auto *name = Label(row, target.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 132, 28);
    SingleLineLabel(name, target_width - 430, &lv_font_montserrat_32);
    std::string target_description = ImageTargetDescription(target.path);
    if (target.logical)
      target_description += "  /  " + Size(target.bytes) + " allocated";
    auto *description = Label(row, target_description.c_str(),
                              &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(description, 132, 92);
    SingleLineLabel(description, target_width - 300,
                    &lv_font_montserrat_24);
    if (recommended) {
      auto *tag = Kicker(row, "RECOMMENDED", kAccent);
      lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, -62, 28);
    }
    auto *arrow = Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kMutedStrong);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -28, 0);
  };

  const std::string suggestion = SuggestedImageTarget(entry.name);
  const auto recommended = std::find_if(
      targets.begin(), targets.end(), [&](const Volume &target) {
        return target.path == suggestion ||
               (suggestion == "/system" && target.path == "/system_root");
      });
  if (recommended != targets.end()) {
    auto *recommended_title = Label(list, "DETECTED FROM FILENAME",
                                    &lv_font_montserrat_18, kAccent);
    lv_obj_set_pos(recommended_title, 8, 8);
    lv_obj_set_style_text_letter_space(recommended_title, 3, 0);
    add_target(list, 58, *recommended, true);

    std::vector<Volume> advanced_targets;
    for (const auto &target : targets)
      if (target.path != recommended->path) advanced_targets.push_back(target);

    auto *advanced = lv_obj_create(list);
    Clear(advanced);
    lv_obj_set_pos(advanced, 0, 420);
    lv_obj_set_size(advanced, target_width,
                    static_cast<int>(advanced_targets.size()) * 196);
    for (size_t index = 0; index < advanced_targets.size(); ++index)
      add_target(advanced, static_cast<int>(index) * 196,
                 advanced_targets[index], false);
    lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);

    auto *toggle = lv_button_create(list);
    Panel(toggle, 28, kInset);
    Interactive(toggle, kMainSelected);
    lv_obj_set_pos(toggle, 0, 266);
    lv_obj_set_size(toggle, target_width, 120);
    const std::string show_text = i18n::Format(
        "Show %zu advanced partitions", advanced_targets.size());
    auto *toggle_label = Label(toggle, show_text.c_str(),
                               &lv_font_montserrat_24, kMutedStrong);
    lv_obj_align(toggle_label, LV_ALIGN_LEFT_MID, 32, 0);
    auto *toggle_icon = Label(toggle, LV_SYMBOL_DOWN,
                              &lv_font_montserrat_24, kMutedStrong);
    lv_obj_align(toggle_icon, LV_ALIGN_RIGHT_MID, -32, 0);
    OnClick(toggle, [advanced, toggle_label, toggle_icon, show_text] {
      const bool hidden = lv_obj_has_flag(advanced, LV_OBJ_FLAG_HIDDEN);
      if (hidden) {
        lv_obj_remove_flag(advanced, LV_OBJ_FLAG_HIDDEN);
        i18n::BindLabel(toggle_label, "Hide advanced partitions");
        i18n::BindLabel(toggle_icon, LV_SYMBOL_UP);
      } else {
        lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
        i18n::BindLabel(toggle_label, show_text.c_str());
        i18n::BindLabel(toggle_icon, LV_SYMBOL_DOWN);
        lv_obj_scroll_to_y(lv_obj_get_parent(advanced), 0, LV_ANIM_ON);
      }
    });
  } else {
    auto *available_title = Label(list, "AVAILABLE PARTITIONS",
                                  &lv_font_montserrat_18, kMuted);
    lv_obj_set_pos(available_title, 8, 8);
    lv_obj_set_style_text_letter_space(available_title, 3, 0);
    for (size_t index = 0; index < targets.size(); ++index)
      add_target(list, 58 + static_cast<int>(index) * 196,
                 targets[index], false);
  }

  auto *cancel = Button(sheet, "Cancel", [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_size(cancel, sheet_width - 96, 132);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -40);
  AnimateEnter(sheet, 0, 30);
}

std::string DateText(std::time_t value) {
  std::tm time{};
  if (localtime_r(&value, &time) == nullptr) return "Unknown";
  char result[64];
  if (std::strftime(result, sizeof(result), "%Y-%m-%d  %H:%M:%S", &time) == 0) return "Unknown";
  return result;
}

void ShowInformation(Files* state, const Entry& entry, uint64_t measured_size) {
  const auto metadata = file_manager::Inspect(entry.path);
  if (!metadata.exists) {
    Sheet(state->screen, "Item unavailable", "The file or folder no longer exists.");
    return;
  }
  std::string copy = i18n::Format(
      "Path\n%s\n\nType\n%s\n\nSize\n%s\n\nModified\n%s\n\n"
      "Owner\n%u:%u\n\nPermissions\n%s",
      entry.path.c_str(), i18n::Translate(file_manager::TypeName(metadata).c_str()),
      Size(metadata.directory ? measured_size : metadata.size).c_str(),
      DateText(metadata.modified).c_str(), metadata.uid, metadata.gid,
      file_manager::PermissionText(metadata.mode).c_str());
  if (metadata.symlink && !metadata.link_target.empty())
    copy += i18n::Format("\n\nLink target\n%s", metadata.link_target.c_str());
  Sheet(state->screen, "Information", copy, Handler{}, 1500, true);
}

void OpenInformation(Files* state, const Entry& entry) {
  if (!entry.directory) {
    ShowInformation(state, entry, entry.bytes);
    return;
  }
  file_manager::Request request;
  request.operation = file_manager::Operation::kMeasure;
  request.sources = { entry.path };
  StartWork(state, "Calculating size", request, false, false,
            [state, entry](const file_manager::Snapshot& snapshot) {
              if (snapshot.success) ShowInformation(state, entry, snapshot.result_bytes);
            });
}

struct TextViewer {
  std::vector<std::string> pages;
  size_t page = 0;
  bool truncated = false;
  lv_obj_t* surface = nullptr;
  lv_obj_t* body = nullptr;
  lv_obj_t* status = nullptr;
  lv_obj_t* previous = nullptr;
  lv_obj_t* next = nullptr;
};

std::vector<std::string> TextPages(const std::string& text) {
  if (text.empty()) return {" "};
  std::vector<std::string> pages;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = start;
    size_t lines = 0;
    while (end < text.size() && end - start < kTextPageBytes && lines < kTextPageLines) {
      if (text[end] == '\n') ++lines;
      ++end;
    }
    if (end < text.size()) {
      while (end > start &&
             (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        --end;
      const size_t newline = text.rfind('\n', end - 1);
      if (newline != std::string::npos && newline >= start + kTextPageBytes / 2)
        end = newline + 1;
    }
    if (end == start) ++end;
    pages.emplace_back(text.substr(start, end - start));
    start = end;
  }
  return pages;
}

void ShowTextPage(TextViewer* viewer) {
  if (viewer == nullptr || viewer->pages.empty()) return;
  lv_label_set_text(viewer->body, viewer->pages[viewer->page].c_str());
  lv_obj_scroll_to_y(viewer->surface, 0, LV_ANIM_OFF);
  std::string status = i18n::Format("%zu / %zu", viewer->page + 1, viewer->pages.size());
  if (viewer->truncated)
    status += "  |  " + std::string(i18n::Translate("Preview truncated at 128 KB"));
  lv_label_set_text(viewer->status, status.c_str());
  if (viewer->page == 0)
    lv_obj_add_state(viewer->previous, LV_STATE_DISABLED);
  else
    lv_obj_remove_state(viewer->previous, LV_STATE_DISABLED);
  if (viewer->page + 1 >= viewer->pages.size())
    lv_obj_add_state(viewer->next, LV_STATE_DISABLED);
  else
    lv_obj_remove_state(viewer->next, LV_STATE_DISABLED);
}

void OpenTextEditor(Files* state, const std::string& path, const std::string& initial = {}) {
  std::string text = initial;
  if (initial.empty() && file_manager::Exists(path)) {
    bool truncated = false;
    std::string error;
    if (!file_manager::ReadText(path, kTextEditBytes, text, truncated, error)) {
      Sheet(state->screen, "Cannot edit file", error);
      return;
    }
    if (truncated) {
      Sheet(state->screen, "File is too large",
            "Text editing is limited to 128 KB to keep recovery responsive.");
      return;
    }
  }

  auto* overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, kCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  const bool landscape = Landscape(state->screen);
  const int screen_width = lv_obj_get_width(state->screen);
  const int screen_height = lv_obj_get_height(state->screen);
  const int margin = landscape ? 64 : 48;
  const int button_height = 126;
  const int button_y = landscape ? screen_height - 176
                                 : screen_height - margin - button_height;

  auto* title = Label(overlay, file_manager::BaseName(path).c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, margin, 52);
  SingleLineLabel(title, screen_width - 2 * margin - 140, &lv_font_montserrat_48);
  auto* path_label = Label(overlay, path.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(path_label, margin, 122);
  SingleLineLabel(path_label, screen_width - 2 * margin, &lv_font_montserrat_24);

  auto* editor = TextArea(overlay);
  lv_textarea_set_one_line(editor, false);
  lv_textarea_set_max_length(editor, kTextEditBytes);
  lv_textarea_set_text(editor, text.c_str());
  lv_obj_set_style_text_font(editor, UiFont(&lv_font_montserrat_24), 0);
  lv_obj_set_style_text_color(editor, kText, 0);
  lv_obj_set_style_bg_color(editor, kMainPanel, 0);
  lv_obj_set_style_bg_opa(editor, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(editor, kMainLine, 0);
  lv_obj_set_style_border_color(editor, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(editor, 2, 0);
  lv_obj_set_style_radius(editor, 24, 0);
  lv_obj_set_style_pad_all(editor, 26, 0);
  lv_obj_set_scroll_dir(editor, LV_DIR_ALL);

  auto* keyboard = lv_keyboard_create(overlay);
  phone_keyboard::Apply(keyboard, true);
  lv_keyboard_set_textarea(keyboard, editor);
  auto* error = Label(overlay, "", &lv_font_montserrat_24, kRed);

  if (landscape) {
    const int keyboard_width = std::min(1260, screen_width * 42 / 100);
    const int keyboard_x = screen_width - keyboard_width - margin;
    const int error_y = button_y - 54;
    lv_obj_set_pos(editor, margin, 190);
    lv_obj_set_size(editor, keyboard_x - 2 * margin, screen_height - 270);
    lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, keyboard_x, 190);
    lv_obj_set_size(keyboard, keyboard_width, error_y - 190 - 24);
    lv_obj_set_pos(error, keyboard_x, error_y);
    lv_obj_set_width(error, keyboard_width);
  } else {
    const int keyboard_height = std::min(900, screen_height * 29 / 100);
    const int keyboard_y = button_y - keyboard_height - 24;
    const int error_y = keyboard_y - 54;
    lv_obj_set_pos(editor, margin, 190);
    lv_obj_set_size(editor, screen_width - 2 * margin, error_y - 190 - 24);
    lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, margin, keyboard_y);
    lv_obj_set_size(keyboard, screen_width - 2 * margin, keyboard_height);
    lv_obj_set_pos(error, margin, error_y);
    lv_obj_set_width(error, screen_width - 2 * margin);
  }

  const int button_width = landscape ? 590 : (screen_width - 3 * margin) / 2;
  const int button_x = landscape ? screen_width - 2 * (button_width + 20) - margin : margin;
  auto* cancel = Button(overlay, "Cancel", [overlay] { CloseOverlay(overlay); });
  lv_obj_set_pos(cancel, button_x, button_y);
  lv_obj_set_size(cancel, button_width, button_height);
  auto* save = Button(
      overlay, "Save",
      [state, editor, error, overlay, path] {
        std::string message;
        if (!file_manager::WriteTextAtomic(path, lv_textarea_get_text(editor), message)) {
          i18n::BindLabel(error, message.c_str());
          return;
        }
        CloseOverlay(overlay);
        Populate(state);
      },
      true);
  lv_obj_set_pos(save, button_x + button_width + 20, button_y);
  lv_obj_set_size(save, button_width, button_height);
  lv_obj_send_event(editor, LV_EVENT_CLICKED, nullptr);
}

void OpenTextViewer(Files* state, const Entry& entry) {
  std::string text;
  std::string error;
  bool truncated = false;
  if (!file_manager::ReadText(entry.path, kTextPreviewBytes, text, truncated, error)) {
    Sheet(state->screen, "Cannot open file", error);
    return;
  }
  auto* overlay = lv_obj_create(state->screen);
  auto* viewer = new TextViewer;
  viewer->pages = TextPages(text);
  viewer->truncated = truncated;
  lv_obj_add_event_cb(
      overlay,
      [](lv_event_t* event) {
        delete static_cast<TextViewer*>(lv_event_get_user_data(event));
      },
      LV_EVENT_DELETE, viewer);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, kCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  const bool landscape = Landscape(state->screen);
  const int width = lv_obj_get_width(state->screen);
  const int height = lv_obj_get_height(state->screen);
  auto* title = Label(overlay, entry.name.c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 64, 56);
  SingleLineLabel(title, width - 128, &lv_font_montserrat_48);
  auto* path = Label(overlay, entry.path.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(path, 64, 128);
  SingleLineLabel(path, width - 128, &lv_font_montserrat_24);
  viewer->surface = lv_obj_create(overlay);
  Panel(viewer->surface, 24, kMainPanel);
  lv_obj_set_pos(viewer->surface, 64, 196);
  lv_obj_set_size(viewer->surface, width - 128, height - 550);
  lv_obj_add_flag(viewer->surface, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(viewer->surface, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(viewer->surface, LV_SCROLLBAR_MODE_ACTIVE);
  viewer->body = Label(viewer->surface, " ", &lv_font_montserrat_24, kText);
  lv_obj_set_width(viewer->body, width - 200);
  lv_label_set_long_mode(viewer->body, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_line_space(viewer->body, 8, 0);

  const int nav_y = height - 270;
  viewer->previous = Button(overlay, LV_SYMBOL_LEFT, [viewer] {
    if (viewer->page == 0) return;
    --viewer->page;
    ShowTextPage(viewer);
  });
  lv_obj_set_pos(viewer->previous, 64, nav_y);
  lv_obj_set_size(viewer->previous, 150, 88);
  CenterButtonContent(viewer->previous);
  viewer->status = Label(overlay, "", &lv_font_montserrat_24,
                         truncated ? kRed : kMutedStrong);
  lv_obj_set_pos(viewer->status, 230, nav_y + 24);
  lv_obj_set_size(viewer->status, width - 460, 48);
  lv_obj_set_style_text_align(viewer->status, LV_TEXT_ALIGN_CENTER, 0);
  viewer->next = Button(overlay, LV_SYMBOL_RIGHT, [viewer] {
    if (viewer->page + 1 >= viewer->pages.size()) return;
    ++viewer->page;
    ShowTextPage(viewer);
  });
  lv_obj_set_pos(viewer->next, width - 214, nav_y);
  lv_obj_set_size(viewer->next, 150, 88);
  CenterButtonContent(viewer->next);

  const bool editable = !truncated && entry.bytes <= kTextEditorRenderBytes;
  const int button_width = editable ? (width - 148) / 2 : width - 128;
  auto* close = Button(overlay, "Close", [overlay] { CloseOverlay(overlay); });
  lv_obj_set_pos(close, 64, height - 164);
  lv_obj_set_size(close, button_width, 116);
  if (editable) {
    auto* edit = Button(
        overlay, "Edit",
        [state, entry, overlay] {
          CloseOverlay(overlay);
          OpenTextEditor(state, entry.path);
        },
        true);
    lv_obj_set_pos(edit, 84 + button_width, height - 164);
    lv_obj_set_size(edit, button_width, 116);
  }
  ShowTextPage(viewer);
}

std::vector<std::string> SelectedPaths(const Files* state) {
  return { state->selected.begin(), state->selected.end() };
}

void SetClipboard(Files* state, ClipboardMode mode, std::vector<std::string> paths) {
  gClipboard.mode = mode;
  gClipboard.paths = std::move(paths);
  state->selected.clear();
  state->selecting = false;
  RenderEntries(state);
  UpdateActionBar(state);
}

void StartPaste(Files* state, file_manager::Conflict conflict) {
  if (gClipboard.mode == ClipboardMode::kNone || gClipboard.paths.empty()) return;
  file_manager::Request request;
  request.operation = gClipboard.mode == ClipboardMode::kCut ? file_manager::Operation::kMove
                                                             : file_manager::Operation::kCopy;
  request.conflict = conflict;
  request.sources = gClipboard.paths;
  request.destination = gDirectory;
  StartWork(state,
            request.operation == file_manager::Operation::kMove ? "Moving files" : "Copying files",
            request, true, true);
}

void Paste(Files* state) {
  bool conflict = false;
  for (const auto& source : gClipboard.paths) {
    if (file_manager::Exists(file_manager::Join(gDirectory, file_manager::BaseName(source)))) {
      conflict = true;
      break;
    }
  }
  if (!conflict) {
    StartPaste(state, file_manager::Conflict::kKeepBoth);
    return;
  }
  ActionSheet(state, "Items already exist", gDirectory,
              {
                  { "Replace", LV_SYMBOL_REFRESH,
                    [state] { StartPaste(state, file_manager::Conflict::kReplace); }, true },
                  { "Keep both", LV_SYMBOL_PLUS,
                    [state] { StartPaste(state, file_manager::Conflict::kKeepBoth); } },
                  { "Skip", LV_SYMBOL_RIGHT,
                    [state] { StartPaste(state, file_manager::Conflict::kSkip); } },
              });
}

void DeletePaths(Files* state, std::vector<std::string> paths) {
  if (paths.empty()) return;
  const std::string detail =
      paths.size() == 1 ? paths.front() : i18n::Format("%zu selected items", paths.size());
  Sheet(state->screen, "Delete permanently?",
        i18n::Format("%s\n\nDeleted items cannot be recovered from AERA.", detail.c_str()),
        [state, paths = std::move(paths)] {
          file_manager::Request request;
          request.operation = file_manager::Operation::kDelete;
          request.sources = paths;
          StartWork(state, "Deleting files", request, true, false);
        });
}

void RenameEntry(Files* state, const Entry& entry) {
  NameDialog(state, "Rename", "New name", entry.name, "Rename",
             [state, entry](const std::string& name, std::string& error) {
               const std::string destination = file_manager::Join(Parent(entry.path), name);
               if (name == entry.name) return true;
               if (!file_manager::Rename(entry.path, destination, error)) return false;
               gClipboard = Clipboard{};
               state->selected.erase(entry.path);
               return true;
             });
}

void OpenEntryMenu(Files* state, const Entry& entry) {
  ActionSheet(
      state, entry.name, entry.directory ? i18n::Translate("Folder") : Size(entry.bytes),
      {
          { "Open", entry.directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
            [state, entry] { OpenFile(state, entry); } },
          { "Copy", LV_SYMBOL_COPY,
            [state, entry] { SetClipboard(state, ClipboardMode::kCopy, { entry.path }); } },
          { "Cut", LV_SYMBOL_EDIT,
            [state, entry] { SetClipboard(state, ClipboardMode::kCut, { entry.path }); } },
          { "Rename", LV_SYMBOL_EDIT, [state, entry] { RenameEntry(state, entry); } },
          { "Information", LV_SYMBOL_LIST, [state, entry] { OpenInformation(state, entry); } },
          { "Delete", LV_SYMBOL_TRASH, [state, entry] { DeletePaths(state, { entry.path }); },
            true },
      });
}

bool DeferredTextProbe(const std::string &path) {
  return path == "/mnt/nas" || path.compare(0, 9, "/mnt/nas/") == 0;
}

void OpenFile(Files *state, const Entry &entry) {
  if (entry.symlink) {
    struct stat target{};
    if (stat(entry.path.c_str(), &target) != 0) {
      OpenInformation(state, entry);
      return;
    }
    if (S_ISDIR(target.st_mode)) {
      gDirectory = entry.path;
      state->visible = 100;
      Populate(state);
      return;
    }
    Entry resolved = entry;
    resolved.symlink = false;
    resolved.bytes = target.st_size < 0 ? 0 : static_cast<uint64_t>(target.st_size);
    resolved.text = S_ISREG(target.st_mode) && file_manager::IsTextFile(entry.path, 1024 * 1024);
    OpenFile(state, resolved);
    return;
  }
  if (entry.directory) {
    gDirectory = entry.path;
    state->visible = 100;
    Populate(state);
    return;
  }
  if (IsPicture(entry.name)) {
    OpenPicture(state->screen, entry.path);
  } else if (plugins::IsPackageFile(entry.name)) {
    plugins::Plugin plugin;
    std::string error;
    if (!plugins::InspectLocalPackage(entry.path, plugin, error)) {
      Sheet(state->screen, "Cannot open plugin",
            error.empty() ? "This .aerap package is invalid." : error);
      return;
    }
    auto install = [state, entry, plugin](bool allow_unofficial) {
      plugins::Request request;
      request.job = plugins::Job::kInstallLocalStorage;
      request.id = plugin.id;
      request.path = entry.path;
      request.allow_unofficial = allow_unofficial;
      SetPluginRequest(request);
      state->callback(Action::kInstallLocalPlugin, state->context);
    };
    const std::string details = i18n::Format(
        "%s\nVersion %s\n\n%s\n\nPackage: %s\nID: %s",
        plugin.name.c_str(), plugin.version.c_str(), plugin.description.c_str(),
        Size(entry.bytes).c_str(), plugin.id.c_str());
    if (plugin.trust == plugins::Trust::kOfficial) {
      Sheet(state->screen, "Install official AERA app?",
            i18n::Format("OFFICIAL / SIGNATURE VERIFIED\n\n%s",
                         details.c_str()),
            [install] { install(false); });
    } else {
      Sheet(state->screen, "Unofficial app warning", i18n::Format(
            "This package is not signed by AERA. Its code will run with "
            "recovery privileges and may read, change, or erase device data."
            "\n\n%s\n\nOnly continue if you trust where this file came from.",
            details.c_str()),
            [state, details, install] {
        Sheet(state->screen, "Install unofficial app?",
              i18n::Format(
                  "UNVERIFIED PUBLISHER\n\n%s\n\nAERA cannot verify the "
                  "developer or guarantee this package is safe.",
                  details.c_str()),
              [install] { install(true); });
      });
    }
  } else if (Zip(entry.name)) {
    ReviewPackage(state->screen, entry.path, state->callback, state->context);
  } else if (Image(entry.name)) {
    OpenImageTargetPicker(state, entry);
  } else if (entry.text || (DeferredTextProbe(entry.path) &&
                           file_manager::IsTextFile(entry.path, 1024 * 1024))) {
    OpenTextViewer(state, entry);
  } else {
    OpenInformation(state, entry);
  }
}

std::string EntryDetail(const Entry& entry) {
  if (entry.symlink) return i18n::Translate("Symbolic link");
  if (entry.directory) return i18n::Translate("Folder");
  const char* kind = plugins::IsPackageFile(entry.name) ? "AERA plugin package"
                     : Zip(entry.name)                  ? "ZIP package"
                     : Image(entry.name)                ? "Flashable image"
                     : IsPicture(entry.name)            ? "Image preview"
                     : entry.text                       ? "Text file"
                                                        : "File";
  return i18n::Format("%s  /  %s", Size(entry.bytes).c_str(), i18n::Translate(kind));
}

std::string EntryModifiedDate(const Entry& entry, bool clock24) {
  if (entry.directory || entry.symlink || entry.modified <= 0) return {};
  struct tm local_time{};
  if (localtime_r(&entry.modified, &local_time) == nullptr) return {};
  char formatted[24] = {};
  if (strftime(formatted, sizeof(formatted), clock24 ? "%Y-%m-%d  %H:%M" : "%Y-%m-%d  %I:%M %p",
               &local_time) == 0)
    return {};
  return formatted;
}

const char* EntryIcon(const Entry& entry) {
  if (entry.directory) return LV_SYMBOL_DIRECTORY;
  if (entry.symlink) return LV_SYMBOL_RIGHT;
  if (IsPicture(entry.name)) return LV_SYMBOL_IMAGE;
  if (Image(entry.name)) return LV_SYMBOL_UPLOAD;
  if (entry.text) return LV_SYMBOL_EDIT;
  return LV_SYMBOL_FILE;
}

void ToggleSelection(Files* state, const Entry& entry) {
  if (state->selected.erase(entry.path) == 0) state->selected.insert(entry.path);
  RenderEntries(state);
  UpdateActionBar(state);
}

void FileRow(Files* state, int y, const Entry& entry, bool alternate) {
  lv_obj_update_layout(state->list);
  const int width = std::max(320, static_cast<int>(lv_obj_get_width(state->list)));
  const FileListMetrics metrics = FileListLayout();
  const bool selected = state->selected.count(entry.path) != 0;
  auto* row = lv_button_create(state->list);
  Clear(row);
  lv_obj_set_style_bg_color(row, kMainPanel, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(row, kMainSelected, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(row, 0, 0);
  OnClickOrHold(
      row,
      [state, entry] {
        if (state->selecting)
          ToggleSelection(state, entry);
        else
          OpenFile(state, entry);
      },
      [state, entry] {
        if (state->selecting)
          ToggleSelection(state, entry);
        else
          OpenEntryMenu(state, entry);
      });
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_size(row, width, metrics.row_height);
  lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(row, selected ? kAccentSoft : kMainPanel, 0);
  lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : (alternate ? LV_OPA_20 : LV_OPA_TRANSP),
                          0);
  lv_obj_set_style_radius(row, 24, 0);
  if (selected) {
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, kAccent, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_70, 0);
  } else {
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, kMainLine, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
  }
  auto* icon = Label(row, EntryIcon(entry), metrics.icon_font,
                     selected ? kAccent : kMutedStrong);
  lv_obj_set_pos(icon, 32, metrics.icon_y);
  auto* name = Label(row, entry.name.c_str(), metrics.name_font, kText);
  lv_obj_set_pos(name, metrics.text_x, metrics.name_y);
  const int text_margin = state->selecting ? metrics.text_x + 104
                                           : metrics.text_x + 54;
  SingleLineLabel(name, width - text_margin, metrics.name_font);
  const std::string detail = EntryDetail(entry);
  auto* copy = Label(row, detail.c_str(), metrics.detail_font, kMuted);
  lv_obj_set_pos(copy, metrics.text_x, metrics.detail_y);
  int detail_width = width - text_margin;
  const bool clock24 = RecoveryPreference(Preference::kClock24);
  const std::string modified = EntryModifiedDate(entry, clock24);
  if (!modified.empty()) {
    const lv_font_t* date_font = UiFont(metrics.detail_font);
    lv_point_t measured{};
    lv_text_get_size(&measured, modified.c_str(), date_font, 0, 0,
                    LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int date_width = measured.x + 8;
    detail_width = std::max(120, detail_width - date_width - 28);
    auto* date = lv_label_create(row);
    lv_label_set_text(date, modified.c_str());
    lv_label_set_long_mode(date, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(date, width - (state->selecting ? 104 : 54) - date_width, metrics.detail_y);
    lv_obj_set_size(date, date_width, lv_font_get_line_height(date_font));
    lv_obj_set_style_text_align(date, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(date, date_font, 0);
    lv_obj_set_style_text_letter_space(date, 0, 0);
    lv_obj_set_style_text_line_space(date, 0, 0);
    lv_obj_set_style_text_color(date, kDim, 0);
  }
  SingleLineLabel(copy, detail_width, metrics.detail_font);
  if (state->selecting) {
    if (selected) {
      auto* mark = Label(row, LV_SYMBOL_OK, metrics.icon_font, kAccent);
      lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -34, 0);
    } else {
      auto* mark = lv_obj_create(row);
      Clear(mark);
      lv_obj_set_size(mark, 30, 30);
      lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -34, 0);
      lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(mark, 2, 0);
      lv_obj_set_style_border_color(mark, kMutedStrong, 0);
      lv_obj_set_style_border_opa(mark, LV_OPA_70, 0);
    }
  }
}

void FileUtilityRow(Files* state, int y, const char* symbol,
                    const std::string& title, const std::string& detail,
                    Handler action) {
  lv_obj_update_layout(state->list);
  const int width = std::max(320, static_cast<int>(lv_obj_get_width(state->list)));
  const FileListMetrics metrics = FileListLayout();
  auto* row = Button(state->list, "", std::move(action));
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_size(row, width, metrics.row_height);
  lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(row, 24, 0);
  auto* icon = Label(row, symbol, metrics.icon_font, kAccent);
  lv_obj_set_pos(icon, 32, metrics.icon_y);
  auto* name = Label(row, title.c_str(), metrics.name_font, kText);
  lv_obj_set_pos(name, metrics.text_x, metrics.name_y);
  SingleLineLabel(name, width - metrics.text_x - 150, metrics.name_font);
  auto* detail_label = Label(row, detail.c_str(), metrics.detail_font, kMuted);
  lv_obj_set_pos(detail_label, metrics.text_x, metrics.detail_y);
  SingleLineLabel(detail_label, width - metrics.text_x - 150,
                  metrics.detail_font);
  auto* end = Label(row, LV_SYMBOL_RIGHT, metrics.icon_font, kMuted);
  lv_obj_align(end, LV_ALIGN_RIGHT_MID, -28, 0);
}

void RenderEntries(Files *state) {
  const int32_t scroll = lv_obj_get_scroll_y(state->list);
  lv_obj_clean(state->list);
  int y = 0;
  const FileListMetrics metrics = FileListLayout();
  if (gDirectory != "/") {
    FileUtilityRow(state, y, LV_SYMBOL_UP, "Parent folder", Parent(gDirectory),
                   [state] {
      gDirectory = Parent(gDirectory);
      state->visible = 100;
      Populate(state);
    });
    y += metrics.row_pitch;
  }
  const size_t count = std::min(state->entries.size(), state->visible);
  for (size_t i = 0; i < count; ++i) {
    const auto entry = state->entries[i];
    FileRow(state, y, entry, (i % 2) != 0);
    y += metrics.row_pitch;
  }
  if (count < state->entries.size()) {
    FileUtilityRow(state, y, LV_SYMBOL_PLUS, "Show more files",
                   "Next 100 entries", [state] {
      state->visible += 100;
      RenderEntries(state);
    });
  }
  lv_obj_scroll_to_y(state->list, scroll, LV_ANIM_OFF);
}

void UpdateActionBar(Files* state) {
  const bool visible = state->selecting || gClipboard.mode != ClipboardMode::kNone;
  if (!visible) {
    if (state->action_bar != nullptr) lv_obj_add_flag(state->action_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_bottom(state->list, Landscape(state->screen) ? 190 : 242, 0);
    return;
  }
  if (state->action_bar == nullptr) {
    state->action_bar = lv_obj_create(state->screen);
    Panel(state->action_bar, 30, kMainSheet);
    lv_obj_set_style_border_width(state->action_bar, 1, 0);
    lv_obj_set_style_border_color(state->action_bar, kMainLine, 0);
    lv_obj_set_style_border_opa(state->action_bar, LV_OPA_40, 0);
  }
  lv_obj_remove_flag(state->action_bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(state->action_bar);
  const bool landscape = Landscape(state->screen);
  const InterfaceSize interface_size = RecoveryInterfaceSize();
  const int bar_height = interface_size == InterfaceSize::kSpacious ? 264
                         : interface_size == InterfaceSize::kLarge ? 228
                         : interface_size == InterfaceSize::kNormal ? 190
                                                                    : 150;
  const int button_height = interface_size == InterfaceSize::kSpacious ? 208
                            : interface_size == InterfaceSize::kLarge ? 180
                            : interface_size == InterfaceSize::kNormal ? 146
                                                                       : 108;
  const int button_width = interface_size == InterfaceSize::kSpacious ? 220
                           : interface_size == InterfaceSize::kSmall ? 170 : 190;
  const lv_font_t* action_font = interface_size == InterfaceSize::kSpacious
      ? &lv_font_montserrat_48
      : interface_size == InterfaceSize::kLarge
      ? &lv_font_montserrat_40
      : interface_size == InterfaceSize::kNormal ? &lv_font_montserrat_36
                                                  : &lv_font_montserrat_32;
  const lv_font_t* status_font = interface_size == InterfaceSize::kSpacious
      ? &lv_font_montserrat_40
      : interface_size == InterfaceSize::kLarge
      ? &lv_font_montserrat_36
      : interface_size == InterfaceSize::kNormal ? &lv_font_montserrat_32
                                                  : &lv_font_montserrat_24;
  const int width = landscape ? lv_obj_get_width(state->screen) - 128 : 1312;
  lv_obj_set_size(state->action_bar, width, bar_height);
  lv_obj_align(state->action_bar, LV_ALIGN_BOTTOM_MID, 0, -NavigationHeight(state->screen) - 16);
  lv_obj_set_style_pad_all(state->action_bar, 12, 0);
  lv_obj_set_style_pad_column(state->action_bar, 10, 0);
  lv_obj_set_flex_flow(state->action_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(state->action_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  auto enlarge = [action_font](lv_obj_t* button) {
    if (button == nullptr || lv_obj_get_child_count(button) == 0) return;
    auto* label = lv_obj_get_child(button, 0);
    FitLabelToLines(label,
                    std::max(40, static_cast<int>(lv_obj_get_width(button)) - 42),
                    1, {action_font, &lv_font_montserrat_36,
                        &lv_font_montserrat_32, &lv_font_montserrat_28,
                        &lv_font_montserrat_24});
    lv_obj_center(label);
  };

  if (state->selecting) {
    const std::string count = i18n::Format("%zu selected", state->selected.size());
    auto* label = Label(state->action_bar, count.c_str(), status_font, kText);
    lv_obj_set_size(label, interface_size == InterfaceSize::kSmall ? 250 : 170,
                    LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_flex_grow(label, 1);
    auto add = [state, button_width, button_height, &enlarge](
                   const char* text, Handler handler, bool destructive = false) {
      auto* button = Button(state->action_bar, text, std::move(handler));
      lv_obj_set_size(button, button_width, button_height);
      lv_obj_set_style_radius(button, 24, 0);
      enlarge(button);
      if (destructive && lv_obj_get_child_count(button) != 0)
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0), kRed, 0);
      return button;
    };
    auto* all = add("All", [state] {
      state->selected.clear();
      for (const auto& entry : state->entries) state->selected.insert(entry.path);
      RenderEntries(state);
      UpdateActionBar(state);
    });
    auto* copy =
        add("Copy", [state] { SetClipboard(state, ClipboardMode::kCopy, SelectedPaths(state)); });
    auto* cut =
        add("Cut", [state] { SetClipboard(state, ClipboardMode::kCut, SelectedPaths(state)); });
    auto* remove = add("Delete", [state] { DeletePaths(state, SelectedPaths(state)); }, true);
    add(LV_SYMBOL_CLOSE, [state] {
      state->selecting = false;
      state->selected.clear();
      RenderEntries(state);
      UpdateActionBar(state);
    });
    if (state->selected.empty()) {
      lv_obj_add_state(copy, LV_STATE_DISABLED);
      lv_obj_add_state(cut, LV_STATE_DISABLED);
      lv_obj_add_state(remove, LV_STATE_DISABLED);
    }
    if (state->selected.size() == state->entries.size() && !state->entries.empty())
      lv_obj_add_state(all, LV_STATE_DISABLED);
  } else {
    const std::string status = i18n::Format(
        gClipboard.mode == ClipboardMode::kCut ? "%zu item(s) cut" : "%zu item(s) copied",
        gClipboard.paths.size());
    auto* label = Label(state->action_bar, status.c_str(), status_font, kText);
    lv_obj_set_size(label, 480, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_flex_grow(label, 1);
    auto* paste = Button(state->action_bar, "Paste here", [state] { Paste(state); }, true);
    lv_obj_set_size(paste, interface_size == InterfaceSize::kSmall ? 420 : 480,
                    button_height);
    lv_obj_set_style_radius(paste, 24, 0);
    enlarge(paste);
    auto* cancel = Button(state->action_bar, LV_SYMBOL_CLOSE, [state] {
      gClipboard = Clipboard{};
      UpdateActionBar(state);
    });
    lv_obj_set_size(cancel, interface_size == InterfaceSize::kSmall ? 150 : 180,
                    button_height);
    lv_obj_set_style_radius(cancel, 24, 0);
    enlarge(cancel);
  }
  lv_obj_move_foreground(state->action_bar);
  lv_obj_set_style_pad_bottom(
      state->list, bar_height + NavigationHeight(state->screen) + 40, 0);
}

void SetSort(Files* state, SortMode mode) {
  gSortMode = mode;
  Populate(state);
}

const char* SortModeLabel(SortMode mode) {
  switch (mode) {
    case SortMode::kName: return "Name";
    case SortMode::kSize: return "Size";
    case SortMode::kDate: return "Date";
    case SortMode::kType: return "Type";
  }
  return "Name";
}

void OpenSortMenu(Files* state) {
  auto* overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      overlay,
      [](lv_event_t* event) {
        if (lv_event_get_target_obj(event) ==
            lv_event_get_current_target_obj(event)) {
          CloseOverlay(lv_event_get_current_target_obj(event));
        }
      },
      LV_EVENT_CLICKED, nullptr);

  const bool landscape = Landscape(state->screen);
  const int width = landscape
      ? std::min(1900, static_cast<int>(lv_obj_get_width(state->screen)) - 128)
      : 1312;
  const int height = 660;
  auto* sheet = lv_obj_create(overlay);
  Panel(sheet, 44, kMainSheet);
  lv_obj_set_size(sheet, width, height);
  lv_obj_align(sheet, landscape ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID,
               0, landscape ? 0 : -40);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_30, 0);

  auto* heading = Label(sheet, "Sort files", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 48, 42);
  SingleLineLabel(heading, width - 190, &lv_font_montserrat_48);
  auto sort_summary = [] {
    return i18n::Format(
        "%s  /  %s", i18n::Translate(SortModeLabel(gSortMode)),
        i18n::Translate(gSortAscending ? "Ascending" : "Descending"));
  };
  const std::string initial_summary = sort_summary();
  auto* subtitle = Label(sheet, initial_summary.c_str(),
                         &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, 48, 112);
  SingleLineLabel(subtitle, width - 96, &lv_font_montserrat_24);
  auto* close = Button(sheet, LV_SYMBOL_CLOSE, [overlay] { CloseOverlay(overlay); });
  lv_obj_set_pos(close, width - 138, 28);
  lv_obj_set_size(close, 98, 98);

  const int margin = 48;
  const int gap = 20;
  const int inner_width = width - margin * 2;
  const int half_width = (inner_width - gap) / 2;
  auto make_choice = [sheet](int x, int y, int width, int height,
                             const char* text, const char* symbol,
                             Handler handler) {
    ChoiceControl control;
    control.button = Button(sheet, "", std::move(handler));
    lv_obj_set_pos(control.button, x, y);
    lv_obj_set_size(control.button, width, height);
    lv_obj_set_style_radius(control.button, 26, 0);
    lv_obj_set_style_bg_color(control.button, kAccentSoft, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(control.button, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(control.button, 3, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(control.button, kAccent, LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(control.button, LV_OPA_80, LV_STATE_CHECKED);
    auto* icon = Label(control.button, symbol, &lv_font_montserrat_36, kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 28, 0);
    auto* label = Label(control.button, text, &lv_font_montserrat_32, kText);
    FitLabelToLines(label, width - 150, 1,
                    {&lv_font_montserrat_32, &lv_font_montserrat_28,
                     &lv_font_montserrat_24});
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 92, 0);
    control.marker = Label(control.button, LV_SYMBOL_OK,
                           &lv_font_montserrat_28, kAccent);
    lv_obj_align(control.marker, LV_ALIGN_RIGHT_MID, -26, 0);
    return control;
  };

  const std::array<SortMode, 4> modes{
      SortMode::kName, SortMode::kSize, SortMode::kDate, SortMode::kType};
  const std::array<const char*, 4> mode_icons{
      LV_SYMBOL_EDIT, LV_SYMBOL_DRIVE, LV_SYMBOL_REFRESH, LV_SYMBOL_LIST};
  auto sort_controls = std::make_shared<std::array<ChoiceControl, 4>>();
  for (size_t index = 0; index < modes.size(); ++index) {
    const int column = static_cast<int>(index % 2);
    const int row = static_cast<int>(index / 2);
    (*sort_controls)[index] = make_choice(
        margin + column * (half_width + gap), 176 + row * 142,
        half_width, 132, SortModeLabel(modes[index]), mode_icons[index],
        [state, subtitle, sort_summary, sort_controls, modes, index] {
          SetSort(state, modes[index]);
          for (size_t i = 0; i < sort_controls->size(); ++i)
            SetChoiceSelected((*sort_controls)[i], i == index);
          const std::string summary = sort_summary();
          lv_label_set_text(subtitle, summary.c_str());
        });
    SetChoiceSelected((*sort_controls)[index], gSortMode == modes[index]);
  }

  auto order_controls = std::make_shared<std::array<ChoiceControl, 2>>();
  const std::array<bool, 2> directions{true, false};
  const std::array<const char*, 2> direction_labels{"Ascending", "Descending"};
  const std::array<const char*, 2> direction_icons{LV_SYMBOL_UP, LV_SYMBOL_DOWN};
  for (size_t index = 0; index < directions.size(); ++index) {
    (*order_controls)[index] = make_choice(
        margin + static_cast<int>(index) * (half_width + gap), 468,
        half_width, 132, direction_labels[index], direction_icons[index],
        [state, subtitle, sort_summary, order_controls, directions, index] {
          gSortAscending = directions[index];
          Populate(state);
          for (size_t i = 0; i < order_controls->size(); ++i)
            SetChoiceSelected((*order_controls)[i], i == index);
          const std::string summary = sort_summary();
          lv_label_set_text(subtitle, summary.c_str());
        });
    SetChoiceSelected((*order_controls)[index],
                      gSortAscending == directions[index]);
  }

  AnimateEnter(sheet, 0, 34);
}

void OpenCreateMenu(Files* state) {
  ActionSheet(state, "Create", gDirectory,
              {
                  { "New folder", LV_SYMBOL_DIRECTORY,
                    [state] {
                      NameDialog(state, "New folder", "Folder name", "", "Create",
                                 [](const std::string& name, std::string& error) {
                                   return file_manager::CreateDirectory(
                                       file_manager::Join(gDirectory, name), error);
                                 });
                    } },
                  { "New text file", LV_SYMBOL_EDIT,
                    [state] {
                      NameDialog(state, "New text file", "File name", "new-file.txt", "Create",
                                 [state](const std::string& name, std::string& error) {
                                   const std::string path = file_manager::Join(gDirectory, name);
                                   if (file_manager::Exists(path)) {
                                     error = "An item with this name already exists.";
                                     return false;
                                   }
                                   OpenTextEditor(state, path);
                                   return true;
                                 });
                    } },
              });
}

void Populate(Files *state) {
  state->entries.clear();
  i18n::BindLabel(state->path_label, gDirectory.c_str());
  DIR *directory = opendir(gDirectory.c_str());
  const int error = errno;
  if (directory) {
    while (auto *item = readdir(directory)) {
      if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
      if (item->d_name[0] == '.' && !RecoveryPreference(Preference::kHiddenFiles)) continue;
      const std::string path = (gDirectory == "/" ? "" : gDirectory) + "/" + item->d_name;
      struct stat info{};
      if (lstat(path.c_str(), &info) != 0) continue;
      const bool folder = S_ISDIR(info.st_mode);
      const bool symlink = S_ISLNK(info.st_mode);
      if (!folder && !S_ISREG(info.st_mode) && !symlink) continue;
      const uint64_t bytes = info.st_size < 0 ? 0 : static_cast<uint64_t>(info.st_size);
      state->entries.push_back(
          { item->d_name, path, folder, symlink,
            S_ISREG(info.st_mode) && !DeferredTextProbe(path) &&
                file_manager::IsTextFile(path, 1024 * 1024), bytes,
            info.st_mode, info.st_uid, info.st_gid, info.st_mtime });
    }
    closedir(directory);
  }
  std::sort(state->entries.begin(), state->entries.end(), [](const Entry& a, const Entry& b) {
    if (a.directory != b.directory) return a.directory;
    int comparison = 0;
    switch (gSortMode) {
      case SortMode::kSize:
        comparison = a.bytes == b.bytes ? 0 : (a.bytes < b.bytes ? -1 : 1);
        break;
      case SortMode::kDate:
        comparison = a.modified == b.modified ? 0 : (a.modified < b.modified ? -1 : 1);
        break;
      case SortMode::kType: {
        const std::string left = a.directory
                                     ? ""
                                     : a.name.substr(a.name.find_last_of('.') == std::string::npos
                                                         ? a.name.size()
                                                         : a.name.find_last_of('.'));
        const std::string right = b.directory
                                      ? ""
                                      : b.name.substr(b.name.find_last_of('.') == std::string::npos
                                                          ? b.name.size()
                                                          : b.name.find_last_of('.'));
        comparison = strcasecmp(left.c_str(), right.c_str());
        break;
      }
      case SortMode::kName:
        break;
    }
    if (comparison == 0) comparison = strcasecmp(a.name.c_str(), b.name.c_str());
    return gSortAscending ? comparison < 0 : comparison > 0;
  });
  for (auto iterator = state->selected.begin(); iterator != state->selected.end();) {
    const bool present = std::any_of(state->entries.begin(), state->entries.end(),
                                     [&](const Entry& entry) { return entry.path == *iterator; });
    if (!present)
      iterator = state->selected.erase(iterator);
    else
      ++iterator;
  }
  struct statvfs storage{};
  std::string capacity;
  if (statvfs(gDirectory.c_str(), &storage) == 0)
    capacity = i18n::Format("%s available",
        Size(static_cast<uint64_t>(storage.f_bavail) * storage.f_frsize).c_str());
  if (RecoveryDataLocked()) capacity = "Internal storage is locked";
  i18n::BindLabel(state->storage_label, capacity.c_str());
  std::string summary = directory
      ? i18n::Format("%zu items", state->entries.size())
      : i18n::Format("Cannot open folder: %s", strerror(error));
  if (directory && state->entries.empty()) summary = "No files in this view";
  i18n::BindLabel(state->summary, summary.c_str());
  RenderEntries(state);
  lv_obj_scroll_to_y(state->list, 0, LV_ANIM_OFF);
  UpdateActionBar(state);
}
}  // namespace

bool NavigateFileBack() {
  if (!gFiles || gDirectory == "/") return false;
  gDirectory = Parent(gDirectory);
  gFiles->visible = 100;
  Populate(gFiles);
  return true;
}

void BuildFilesScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  const InterfaceSize interface_size = RecoveryInterfaceSize();
  ApplyInterfaceSize(static_cast<int>(interface_size));
  const bool large = interface_size == InterfaceSize::kLarge ||
                     interface_size == InterfaceSize::kSpacious;
  if (gDirectory.empty()) gDirectory = RecoveryStorage();
  if (gDirectory.empty()) gDirectory = "/sdcard";
  auto* state = new Files;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  gFiles = state;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    auto *s = static_cast<Files *>(lv_event_get_user_data(event));
    if (gFiles == s) gFiles = nullptr;
    delete s;
  }, LV_EVENT_DELETE, state);
  Header(screen, "Files", "Manage files, folders and recovery packages.", callback, context);
  const bool landscape = Landscape(screen);
  const lv_font_t* storage_font = large ? &lv_font_montserrat_32
                                        : &lv_font_montserrat_24;
  const lv_font_t* path_font = large ? &lv_font_montserrat_40
                                     : &lv_font_montserrat_32;
  const lv_font_t* summary_font = large ? &lv_font_montserrat_32
                                        : &lv_font_montserrat_24;
  state->storage_label = Label(screen, "", storage_font, kAccent);
  lv_obj_set_pos(state->storage_label, 80, landscape ? 306 : 420);

  auto *storage = Button(screen, "Storage  " LV_SYMBOL_DOWN, [state] {
    auto volumes = RecoveryVolumes("storage");
    if (volumes.empty()) {
      Sheet(state->screen, "Storage", "No storage volumes are available.");
      return;
    }
    size_t next = 0;
    for (size_t i = 0; i < volumes.size(); ++i)
      if (volumes[i].path == gDirectory) next = (i + 1) % volumes.size();
    gDirectory = volumes[next].path;
    Populate(state);
  });
  lv_obj_set_pos(storage, 64, landscape ? 350 : 496);
  const int toolbar_height = large ? 140 : 120;
  lv_obj_set_size(storage, 260, toolbar_height);
  auto *root = Button(screen, "Root", [state] { gDirectory = "/"; Populate(state); });
  lv_obj_set_pos(root, 348, landscape ? 350 : 496);
  lv_obj_set_size(root, 140, toolbar_height);
  auto* sort = Button(screen, LV_SYMBOL_LIST, [state] { OpenSortMenu(state); });
  lv_obj_set_pos(sort, 512, landscape ? 350 : 496);
  lv_obj_set_size(sort, 180, toolbar_height);
  CenterButtonContent(sort);
  auto* create = Button(screen, LV_SYMBOL_PLUS, [state] { OpenCreateMenu(state); });
  lv_obj_set_pos(create, 716, landscape ? 350 : 496);
  lv_obj_set_size(create, 180, toolbar_height);
  CenterButtonContent(create);
  auto* select = Button(screen, LV_SYMBOL_OK, [state] {
    state->selecting = !state->selecting;
    if (!state->selecting) state->selected.clear();
    RenderEntries(state);
    UpdateActionBar(state);
  });
  lv_obj_set_pos(select, 920, landscape ? 350 : 496);
  lv_obj_set_size(select, 180, toolbar_height);
  CenterButtonContent(select);
  auto *refresh = Button(screen, LV_SYMBOL_REFRESH, [state] { Populate(state); });
  lv_obj_set_pos(refresh, 1124, landscape ? 350 : 496);
  lv_obj_set_size(refresh, 180, toolbar_height);
  CenterButtonContent(refresh);

  if (large) {
    for (auto* button : {storage, root, sort, create, select, refresh}) {
      if (lv_obj_get_child_count(button) == 0) continue;
      lv_obj_set_style_text_font(lv_obj_get_child(button, 0),
                                 UiFont(&lv_font_montserrat_36), 0);
      FitButtonLabel(button);
    }
  }

  state->path_label = Label(screen, "", path_font, kText);
  lv_obj_set_pos(state->path_label, landscape ? 1460 : 80,
                 landscape ? 358 : 684);
  SingleLineLabel(state->path_label, landscape ? 1620 : 1270, path_font);
  state->summary = Label(screen, "", summary_font, kMuted);
  lv_obj_set_pos(state->summary, landscape ? 1460 : 80,
                 landscape ? 424 : 750);
  // The file surface reaches the physical bottom. Navigation is a foreground
  // overlay, while bottom padding keeps the final entry scrollable above it.
  state->list = Scroll(screen, landscape ? 500 : 826,
                       landscape ? 940 : 2342);
  lv_obj_set_style_pad_bottom(state->list, landscape ? 190 : 242, 0);
  Navigation(screen, Action::kNone, callback, context);
  Populate(state);
}
}  // namespace aeraui
