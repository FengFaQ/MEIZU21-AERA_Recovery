/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "package_review.hpp"
#include "ui_components.hpp"
#include "update/payload_inspector.hpp"
#include "update/payload_arb.hpp"
#include "update/payload_full_flash.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdio>
#include <set>

namespace aeraui {
namespace {
using namespace widgets;
struct ArbProgress {
  payload::Info info;
  payload::DeviceArb device;
  std::atomic<bool> done{false};
};
std::string Size(uint64_t bytes) {
  char value[48];
  if (bytes >= (1ULL << 30)) snprintf(value, sizeof(value), "%.2f GiB", bytes / double(1ULL << 30));
  else if (bytes >= (1ULL << 20)) snprintf(value, sizeof(value), "%.1f MiB", bytes / double(1ULL << 20));
  else snprintf(value, sizeof(value), "%.1f KiB", bytes / 1024.0);
  return value;
}
void Layout(lv_obj_t *area) {
  lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(area, 18, 0);
  lv_obj_set_style_pad_right(area, 16, 0);
  auto *sheet = lv_obj_get_parent(area);
  lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
  lv_obj_set_style_blur_backdrop(sheet, false, 0);
}
lv_obj_t *Row(lv_obj_t *parent, const char *key, const std::string &value) {
  if (value.empty()) return nullptr;
  auto *row = lv_obj_create(parent);
  Clear(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(row, 68, 0);
  lv_obj_set_style_pad_ver(row, 14, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  auto *name = Label(row, key, &lv_font_montserrat_28, kMuted);
  lv_obj_set_width(name, LV_PCT(32));
  auto *text = Label(row, value.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_width(text, LV_PCT(65));
  lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
  return text;
}
void ArbRow(lv_obj_t *parent, const std::shared_ptr<ArbProgress> &progress, int kind = 0) {
  auto *label = Row(parent, kind == 1 ? "ARB source" : kind == 2 ? "Slot A ARB" :
      kind == 3 ? "Slot B ARB" : "Package ARB", "Loading...");
  struct Watch {
    lv_obj_t *label;
    std::shared_ptr<ArbProgress> progress;
    int kind;
    lv_timer_t *timer = nullptr;
  };
  auto *watch = new Watch{label, progress, kind};
  lv_obj_add_event_cb(label, [](lv_event_t *event) {
    auto *watch = static_cast<Watch *>(lv_event_get_user_data(event));
    if (watch->timer) lv_timer_delete(watch->timer);
    delete watch;
  }, LV_EVENT_DELETE, watch);
  watch->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *watch = static_cast<Watch *>(lv_timer_get_user_data(timer));
    if (!watch->progress->done.load(std::memory_order_acquire)) return;
    const auto &info = watch->progress->info;
    const auto &slot = watch->kind == 2 ? watch->progress->device.slot_a : watch->progress->device.slot_b;
    const auto value = watch->kind >= 2 ? (slot.available ? std::to_string(slot.index) : std::string("Unknown")) :
        watch->kind == 1 ? info.arb_detail :
        info.arb_available ? std::to_string(info.arb_index) : std::string("Unavailable");
    i18n::BindLabel(watch->label, value.c_str());
    watch->timer = nullptr;
    lv_timer_delete(timer);
  }, 100, watch);
}
struct InstallGate {
  std::shared_ptr<ArbProgress> progress;
  bool includes_firmware = false;
  bool acknowledged = false;
  bool warned = false;
  lv_obj_t *overlay = nullptr;
  lv_obj_t *body = nullptr;
  lv_obj_t *details = nullptr;
  lv_obj_t *advanced = nullptr;
  lv_obj_t *fast_flash = nullptr;
  lv_obj_t *slider = nullptr;
  payload::ArbDecision Decision() const {
    if (!progress->done.load(std::memory_order_acquire)) return payload::ArbDecision::Unknown;
    const auto &info = progress->info;
    return payload::CompareArb(includes_firmware,
        {info.arb_available, info.arb_index, info.arb_detail}, progress->device);
  }
  bool AllowsInstall() const {
    return progress->done.load(std::memory_order_acquire) &&
        payload::ArbAllowsInstall(Decision(), acknowledged);
  }
};
void FitReview(const InstallGate &gate) {
  auto *sheet = lv_obj_get_parent(gate.body);
  lv_obj_set_height(gate.body, LV_SIZE_CONTENT);
  lv_obj_update_layout(sheet);
  const int content_height = lv_obj_get_height(gate.body);
  // Shared Sheet reserves 670 logical pixels for its heading and swipe footer.
  // Use only the space the metadata needs, capped to the available screen.
  const int height = std::min(content_height + 670,
      static_cast<int>(lv_obj_get_height(gate.overlay)) - 80);
  lv_obj_set_height(sheet, height);
  lv_obj_set_height(gate.body, std::max(80, height - 670));
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -40);
}
void UpdateGate(const std::shared_ptr<InstallGate> &shared);
void ShowArbWarning(const std::shared_ptr<InstallGate> &gate, const char *message) {
  const bool upgrade = gate->Decision() == payload::ArbDecision::Upgrade;
  Sheet(gate->overlay, upgrade ? "Anti-rollback upgrade" : "Installation blocked", "", {}, upgrade ? 1000 : 900,
      false, SheetPresentation::kStandard, "", [gate, message, upgrade](lv_obj_t *area) {
    Layout(area);
    if (upgrade) {
      auto *sheet = lv_obj_get_parent(area);
      lv_obj_update_layout(sheet);
      // Reserve a fixed two-button footer; long warning text stays scrollable.
      lv_obj_set_height(area, std::max(80, static_cast<int>(lv_obj_get_height(sheet)) - 496));
    }
    const auto &info = gate->progress->info;
    const auto &device = gate->progress->device;
    Row(area, "Package ARB", info.arb_available ? std::to_string(info.arb_index) : "Unknown");
    Row(area, "Slot A ARB", device.slot_a.available ? std::to_string(device.slot_a.index) : "Unknown");
    Row(area, "Slot B ARB", device.slot_b.available ? std::to_string(device.slot_b.index) : "Unknown");
    auto *text = Label(area, message, &lv_font_montserrat_32, kText);
    lv_obj_set_width(text, LV_PCT(100));
  }, [gate, upgrade](lv_obj_t *, lv_obj_t *close) {
    auto *sheet = lv_obj_get_parent(close);
    auto style_button = [](lv_obj_t *button) {
      lv_obj_set_size(button, LV_PCT(100), 100);
      lv_obj_set_style_radius(button, 24, 0);
      auto *label = lv_obj_get_child(button, 0);
      lv_obj_set_width(label, LV_PCT(100));
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(label);
    };
    i18n::BindLabel(lv_obj_get_child(close, 0), "Abort");
    style_button(close);
    lv_obj_set_style_bg_color(close, Color(0xB32632), 0);
    lv_obj_set_style_bg_color(close, Color(0x8D1D27), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(close, 0), lv_color_white(), 0);
    if (upgrade) {
      auto *warning = lv_obj_get_parent(sheet);
      auto *confirm = Button(sheet, "I understand - continue", [gate, warning] {
        gate->acknowledged = true;
        lv_obj_delete_async(warning);
        UpdateGate(gate);
      });
      style_button(confirm);
      lv_obj_align(confirm, LV_ALIGN_BOTTOM_MID, 0, -128);
    }
  }, [overlay = gate->overlay] { lv_obj_delete_async(overlay); });
}
void UpdateGate(const std::shared_ptr<InstallGate> &shared) {
  auto &gate = *shared;
  if (!gate.progress->done.load(std::memory_order_acquire)) return;
  using D = payload::ArbDecision;
  const auto decision = gate.Decision();
  if (decision == D::Downgrade) {
    lv_obj_add_flag(gate.details, LV_OBJ_FLAG_HIDDEN);
    if (gate.advanced) lv_obj_add_flag(gate.advanced, LV_OBJ_FLAG_HIDDEN);
    if (gate.fast_flash) lv_obj_add_flag(gate.fast_flash, LV_OBJ_FLAG_HIDDEN);
  }
  if (gate.AllowsInstall()) lv_obj_remove_flag(gate.slider, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(gate.slider, LV_OBJ_FLAG_HIDDEN);
  FitReview(gate);
  if (!gate.warned && (decision == D::Upgrade || decision == D::Downgrade)) {
    gate.warned = true;
    ShowArbWarning(shared, decision == D::Downgrade ?
        "Installation blocked: this package has a lower ARB index than firmware in at least one slot. Flashing it can brick this device. Abort this installation." :
        "Warning: this update raises the firmware ARB index compared with at least one slot. It may permanently prevent returning to older firmware. Confirm below before swiping to install.");
  }
}
void Section(lv_obj_t *parent, const char *text) {
  auto *label = Label(parent, text, &lv_font_montserrat_28, kAccent);
  lv_obj_set_style_pad_top(label, 24, 0);
}
void FullInfo(lv_obj_t *screen, const payload::Info &info, const std::string &path,
              const std::shared_ptr<ArbProgress> &arb) {
  Sheet(screen, "Update information", "", {}, 2000, false,
        SheetPresentation::kStandard, "Swipe to confirm", [info, path, arb](lv_obj_t *area) {
    Layout(area);
    Section(area, "PACKAGE");
    Row(area, info.name_from_filename ? "Package name" : "Version", info.target_build);
    Row(area, "Product", info.target_device);
    Row(area, "Device codename", info.device_codename);
    Row(area, "Build ID", info.build_id);
    Row(area, "System fingerprint", info.system_fingerprint);
    Row(area, "Android", info.target_sdk);
    Row(area, "Security patch", info.security_patch);
    Row(area, "File", path);
    Section(area, "PAYLOAD");
    Row(area, "Update type", info.incremental ? "Incremental" : "Full");
    Row(area, "Partition coverage", info.partial ? "Partial update" : "Full update");
    Row(area, "Payload size", Size(info.payload_bytes));
    Row(area, "Expanded images", Size(info.expanded_bytes));
    Row(area, "Format revision", std::to_string(info.format_version) + "." + std::to_string(info.minor_version));
    Row(area, "Dynamic partitions", info.dynamic_partitions ? "Yes" : "No");
    Row(area, "Snapshots", info.snapshots ? "Requested by package" : "Not requested");
    Row(area, "Virtual A/B compression", info.virtual_ab_compression ? "Requested by package" : "Not requested");
    Row(area, "Operations", std::to_string(info.operations));
    Row(area, "Operation types", info.operation_types);
    ArbRow(area, arb);
    ArbRow(area, arb, true);
    Section(area, "PARTITIONS");
    for (const auto &partition : info.partitions)
      Row(area, partition.name.c_str(), Size(partition.bytes));
  });
}
struct Result {
  payload::Info info;
  std::atomic<bool> done{false};
  std::shared_ptr<ArbProgress> arb = std::make_shared<ArbProgress>();
};

lv_obj_t *AdvancedPage(lv_obj_t *screen, const char *title) {
  auto *page = lv_obj_create(screen);
  Clear(page);
  lv_obj_set_user_data(page, &kModalMarker);
  lv_obj_add_flag(page, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(page, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(page, lv_obj_get_width(screen), lv_obj_get_height(screen));
  lv_obj_set_pos(page, -lv_obj_get_style_pad_left(screen, LV_PART_MAIN),
                       -lv_obj_get_style_pad_top(screen, LV_PART_MAIN));
  lv_obj_set_style_bg_color(page, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(page, 64, 0);
  lv_obj_set_style_pad_top(page, 96, 0);
  lv_obj_set_style_pad_row(page, 32, 0);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  // Wallpaper images are real child objects. Keep them out of the content
  // flex layout, otherwise a full-height image pushes every control offscreen.
  auto *background = lv_obj_create(page);
  Clear(background);
  lv_obj_add_flag(background, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(background, LV_OBJ_FLAG_FLOATING);
  lv_obj_remove_flag(background, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(background, lv_obj_get_width(screen), lv_obj_get_height(screen));
  lv_obj_set_pos(background, -64, -96);
  MainBackground(background);
  lv_obj_move_background(background);
  auto *back = Button(page, LV_SYMBOL_LEFT "  Back", [page] { lv_obj_delete_async(page); });
  lv_obj_set_size(back, 240, 90);
  Panel(back, 24, kMainPanel);
  auto *heading = Label(page, title, &lv_font_montserrat_48, kText);
  lv_obj_set_width(heading, LV_PCT(100));
  return page;
}
void Explanation(lv_obj_t *parent, const std::string &text) {
  auto *label = Label(parent, text.c_str(), &lv_font_montserrat_28, kMuted);
  lv_obj_set_width(label, LV_PCT(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}
void SelectPayload(lv_obj_t *parent, const payload::Info &info, const std::string &path,
                   const std::shared_ptr<InstallGate> &gate, bool flash,
                   ActionCallback callback, void *context, bool direct = false) {
  auto *page = AdvancedPage(parent, direct ? "Direct flash selected partitions" : flash ? "Flash selected partitions" : "Extract selected images");
  const std::string slot = RecoverySlot();
  const auto protected_partitions = flash ? payload::ProtectedPartitions() : std::set<std::string>{};
  Explanation(page, flash ? "Choose images to write to slot " + slot + ". The active slot will not change."
      : "Save verified .img files under AERA/Extracted on the selected storage. No partitions are written.");
  auto *list = lv_obj_create(page);
  Clear(list);
  lv_obj_set_width(list, LV_PCT(100));
  lv_obj_set_flex_grow(list, 1);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 16, 0);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  auto selected = std::make_shared<std::set<std::string>>();
  auto *summary = Label(page, "No partitions selected", &lv_font_montserrat_28, kMuted);
  lv_obj_set_width(summary, LV_PCT(100));
  auto *review = Button(page, flash ? "Review selected flash" : "Review extraction",
      [page, info, path, gate, flash, direct, callback, context, slot, selected, protected_partitions] {
    if (selected->empty()) return;
    if (flash && (!gate || !gate->AllowsInstall())) {
      Sheet(page, "Package checks", "Wait for the ARB check and acknowledge any upgrade warning on the package review.");
      return;
    }
    JobRequest request;
    request.job = flash ? Job::kFlashPayload : Job::kExtractPayload;
    request.title = direct ? "Direct flash selected partitions" : flash ? "Flash selected partitions" : "Extract selected images";
    request.payload_direct = direct;
    request.payload_override_protection = flash && std::any_of(selected->begin(), selected->end(),
        [&](const auto &name) { return protected_partitions.count(name); });
    request.path = path;
    request.partitions.assign(selected->begin(), selected->end());
    request.payload_manifest_hash = info.manifest_hash;
    request.payload_slot = slot;
    request.payload_arb_acknowledged = gate && gate->acknowledged;
    uint64_t bytes = 0;
    std::string names;
    for (const auto &partition : info.partitions) if (selected->count(partition.name)) {
      if (!names.empty()) names += ", ";
      names += partition.name;
      bytes += partition.bytes;
    }
    std::string detail = direct ? names + "\n\nWrites directly to current slot " + slot +
        ". No temporary images are saved. The written images are read back and SHA-256 checked before success. A damaged package or interrupted operation may leave the selected partitions incomplete. Do not reboot after a failure: reflash known-good images. This is not a normal OTA; no snapshot rollback, postinstall or slot activation is performed." :
        names + "\n\nRequired storage: " + Size(bytes) +
        (flash ? "\n\nWrites to current slot " + slot +
            ". All selected images are extracted and hash-verified before flashing. Mixing firmware versions can prevent booting. This is a partial image flash, not a normal OTA installation. No postinstall or slot activation is performed."
            : "\n\nDestination: " + RecoveryStorage() + "/AERA/Extracted\nA new folder will be created; existing files are never overwritten.");
    if (request.payload_override_protection)
      detail += "\n\n" + std::string(i18n::Translate("Manually selected protected partitions will be overwritten."));
    Sheet(page, request.title, detail, [request, callback, context] {
      SetJobRequest(request);
      callback(Action::kRunOperation, context);
    }, 1700, false, SheetPresentation::kStandard, flash ? "Swipe to flash selected" : "Swipe to extract");
  });
  lv_obj_set_size(review, LV_PCT(100), 110);
  lv_obj_set_style_radius(review, 24, 0);
  lv_obj_set_style_text_align(lv_obj_get_child(review, 0), LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_state(review, LV_STATE_DISABLED);
  std::vector<std::string> names;
  for (const auto &p : info.partitions) names.push_back(p.name);
  const auto targets = flash ? RecoveryPayloadTargets(names) : std::vector<PayloadFlashTarget>{};
  const auto target_for = [&](const auto &partition) {
    return std::find_if(targets.begin(), targets.end(),
        [&](const auto &v) { return v.name == partition.name; });
  };
  const auto selectable = [&](const auto &partition) {
    const auto target = target_for(partition);
    const bool target_exists = target != targets.end() && !target->path.empty() &&
        (!target->raw || partition.bytes <= target->bytes);
    return partition.extractable && (!flash || target_exists);
  };
  auto *selection_header = lv_obj_create(list);
  Clear(selection_header);
  lv_obj_set_size(selection_header, LV_PCT(100), 96);
  lv_obj_set_flex_flow(selection_header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(selection_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  auto *caption = Label(selection_header, "SELECT PARTITIONS", &lv_font_montserrat_28, kMuted);
  lv_obj_set_flex_grow(caption, 1);
  auto *select_all = Button(selection_header, "Select all", [] {});
  lv_obj_set_size(select_all, 280, 88);
  Panel(select_all, 24, kMainPanel);
  lv_obj_add_state(select_all, LV_STATE_DISABLED);
  auto *select_all_label = lv_obj_get_child(select_all, 0);
  lv_obj_set_style_text_align(select_all_label, LV_TEXT_ALIGN_CENTER, 0);
  struct SelectionRow {
    std::string name;
    uint64_t bytes;
    lv_obj_t *row;
    lv_obj_t *check;
    lv_obj_t *mark;
    bool protected_partition;
  };
  auto rows = std::make_shared<std::vector<SelectionRow>>();
  const auto refresh_selection = [selected, rows, summary, review, select_all_label] {
    uint64_t bytes = 0;
    for (const auto &entry : *rows) {
      const bool active = selected->count(entry.name);
      if (active) bytes += entry.bytes;
      lv_obj_set_style_border_width(entry.row, active ? 3 : 1, 0);
      lv_obj_set_style_border_color(entry.row, active ? kAccent : kMainLine, 0);
      lv_obj_set_style_border_color(entry.check, active ? kAccent : kMuted, 0);
      if (active) lv_obj_remove_flag(entry.mark, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(entry.mark, LV_OBJ_FLAG_HIDDEN);
    }
    i18n::BindLabel(summary, (std::to_string(selected->size()) + " selected / " + Size(bytes)).c_str());
    const bool all = std::any_of(rows->begin(), rows->end(), [](const auto &row) { return !row.protected_partition; }) &&
        std::all_of(rows->begin(), rows->end(), [&](const auto &row) {
          return row.protected_partition || selected->count(row.name); });
    i18n::BindLabel(select_all_label, all
        ? "Deselect all" : "Select all");
    if (selected->empty()) lv_obj_add_state(review, LV_STATE_DISABLED);
    else lv_obj_remove_state(review, LV_STATE_DISABLED);
  };
  OnClick(select_all, [selected, rows, refresh_selection] {
    if (rows->empty()) return;
    const bool all = std::all_of(rows->begin(), rows->end(), [&](const auto &row) {
      return row.protected_partition || selected->count(row.name); });
    if (all) selected->clear();
    else for (const auto &entry : *rows) if (!entry.protected_partition) selected->insert(entry.name);
    refresh_selection();
  });
  for (const auto &partition : info.partitions) {
    const bool protected_partition = protected_partitions.count(partition.name);
    if (!selectable(partition)) continue;
    auto *row = lv_button_create(list);
    Panel(row, 24, kMainPanel);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 28, 0);
    lv_obj_set_style_min_height(row, 116, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, kMainLine, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);
    auto *check = lv_obj_create(row);
    Clear(check);
    lv_obj_set_size(check, 40, 40);
    lv_obj_set_style_radius(check, 10, 0);
    lv_obj_set_style_border_width(check, 2, 0);
    lv_obj_set_style_border_color(check, kMuted, 0);
    auto *mark = Label(check, LV_SYMBOL_OK, &lv_font_montserrat_28, kAccent);
    lv_obj_center(mark);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
    auto *name = Label(row, partition.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const auto target = target_for(partition);
    if (flash && target != targets.end() && target->raw)
      Label(row, "Firmware", &lv_font_montserrat_28, kMuted);
    Label(row, Size(partition.bytes).c_str(), &lv_font_montserrat_28, kMuted);
    if (protected_partition) {
      Label(row, partition.name == "abl" ? "Keep current ABL" : "Keep AERA installed",
            &lv_font_montserrat_24, kMuted);
    }
    rows->push_back({partition.name, partition.bytes, row, check, mark, protected_partition});
    OnClick(row, [selected, name = partition.name, refresh_selection] {
      if (!selected->erase(name)) selected->insert(name);
      refresh_selection();
    });
  }
  if (std::any_of(rows->begin(), rows->end(), [](const auto &row) { return !row.protected_partition; }))
    lv_obj_remove_state(select_all, LV_STATE_DISABLED);
  const auto unavailable_count = std::count_if(info.partitions.begin(), info.partitions.end(),
      [&](const auto &p) { return !selectable(p); });
  if (unavailable_count) {
    auto *toggle = Button(list, (std::string(flash ? "Extraction only / unavailable" : "Unavailable images") +
        " (" + std::to_string(unavailable_count) + ")  " LV_SYMBOL_DOWN).c_str(), [] {});
    lv_obj_set_size(toggle, LV_PCT(100), 104);
    lv_obj_set_style_radius(toggle, 24, 0);
    auto *details = lv_obj_create(list);
    Clear(details);
    lv_obj_set_size(details, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(details, 24, 0);
    if (flash) Explanation(details, "These images have no verified flash target, exceed its capacity, or cannot be extracted. Shared data partitions are excluded. Use extraction or normal installation where appropriate.");
    for (const auto &p : info.partitions) {
      if (selectable(p)) continue;
      Row(details, p.name.c_str(), Size(p.bytes));
      if (!p.extractable) Explanation(details, p.extraction_error);
      else if (flash) {
        const auto target = target_for(p);
        Explanation(details, target == targets.end() ? "No verified target" :
            target->path.empty() ? target->reason : "Image exceeds physical partition capacity");
      }
    }
    lv_obj_add_flag(details, LV_OBJ_FLAG_HIDDEN);
    OnClick(toggle, [details] {
      if (lv_obj_has_flag(details, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(details, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(details, LV_OBJ_FLAG_HIDDEN);
    });
  }
}
void AdvancedZip(lv_obj_t *screen, const payload::Info &info, const std::string &path,
                 const std::shared_ptr<InstallGate> &gate, ActionCallback callback, void *context) {
  auto *page = AdvancedPage(screen, "Advanced ZIP options");
  if (!info.is_payload || !info.valid || info.incremental) {
    Explanation(page, info.incremental ? "This incremental payload depends on existing partition data. Use normal installation."
        : "This ZIP uses its own installer. Individual payload partition extraction is not available.");
    return;
  }
  Explanation(page, "Choose how to use the images in this full OTA.");
  auto *package = lv_obj_create(page);
  Panel(package, 28, kMainPanel);
  lv_obj_set_size(package, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(package, 32, 0);
  lv_obj_set_flex_flow(package, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(package, 20, 0);
  Section(package, "SELECTED PACKAGE");
  Explanation(package, info.target_build.empty() ? path.substr(path.find_last_of('/') + 1) : info.target_build);
  Row(package, "Images", std::to_string(info.partitions.size()));
  Row(package, "Expanded size", Size(info.expanded_bytes));
  for (int mode : {0, 1, 2}) {
    const bool flash = mode != 2, direct = mode == 1;
    auto *button = Button(page, direct ? "Direct flash selected partitions" : flash ? "Flash selected partitions" : "Extract selected images",
        [page, info, path, gate, flash, direct, callback, context] {
      SelectPayload(page, info, path, gate, flash, callback, context, direct);
    });
    lv_obj_set_size(button, LV_PCT(100), 180);
    Panel(button, 28, kMainPanel);
    auto *title = lv_obj_get_child(button, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);
    auto *subtitle = Label(button, direct ? "Experimental: no temporary images; verified readback" : flash ? "Extract and verify all images before writing" : "Save images to storage without flashing",
        &lv_font_montserrat_28, kMuted);
    lv_obj_set_width(subtitle, LV_PCT(85));
    lv_obj_align(subtitle, LV_ALIGN_BOTTOM_LEFT, 16, -12);
    auto *arrow = Label(button, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kMuted);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);
  }
  Explanation(page, "Extraction temporarily needs enough space for all selected expanded images. Partial flashing does not run the package installer or its device-compatibility checks: use only firmware intended for this device.");
}
void ReviewFastFlash(const payload::Info &info, const std::string &path,
                     const std::shared_ptr<InstallGate> &gate, ActionCallback callback, void *context) {
  if (!gate->AllowsInstall()) {
    Sheet(gate->overlay, "Package checks", "Wait for the ARB check and acknowledge any upgrade warning first.");
    return;
  }
  std::vector<std::string> names, required;
  for (const auto &p : info.partitions) names.push_back(p.name);
  std::string error;
  const auto slot = RecoverySlot();
  if ((slot != "A" && slot != "B") ||
      !payload::PlanFullFlash(info, RecoveryPayloadTargets(names), required, error, payload::ProtectedPartitions())) {
    Sheet(gate->overlay, "Fast flash unavailable", error.empty() ? "The current slot could not be identified." : error);
    return;
  }
  JobRequest request;
  request.job = Job::kFlashPayload;
  request.title = "Fast flash (experimental)";
  request.path = path;
  request.partitions = std::move(required);
  request.payload_manifest_hash = info.manifest_hash;
  request.payload_slot = slot;
  request.payload_direct = true;
  request.payload_full = true;
  request.payload_arb_acknowledged = gate->acknowledged;
  Sheet(gate->overlay, request.title, "", [request, gate, callback, context] {
    if (!gate->AllowsInstall()) return;
    SetJobRequest(request);
    callback(Action::kRunOperation, context);
  }, 1400, false, SheetPresentation::kStandard, "Swipe to fast flash", [info, slot, request](lv_obj_t *area) {
    Layout(area);
    Row(area, "Target", "Current slot " + slot + " (unchanged)");
    Row(area, "Images", std::to_string(request.partitions.size()));
    for (const auto &partition : info.partitions)
      if (std::find(request.partitions.begin(), request.partitions.end(), partition.name) == request.partitions.end())
        Row(area, partition.name == "abl" ? "Keep current ABL" : "Keep AERA installed", partition.name);
    Row(area, "Expanded size", Size(info.expanded_bytes));
    Explanation(area, "Otaripper schedules all selected partitions through one shared worker pool, without temporary extraction. Each image is read back and SHA-256 verified. Scroll the progress list to see every partition.");
    Explanation(area, "Use only firmware intended for this device. This bypasses the normal OTA installer and its compatibility checks, snapshot rollback and postinstall. An interruption may leave the current slot unbootable. Do not reboot after a failure; reflash known-good firmware.");
  });
}
struct Review {
  lv_obj_t *screen;
  lv_obj_t *overlay;
  lv_timer_t *timer = nullptr;
  std::string path;
  ActionCallback callback;
  void *context;
  std::shared_ptr<Result> result = std::make_shared<Result>();
};
void Ready(lv_timer_t *timer) {
  auto *state = static_cast<Review *>(lv_timer_get_user_data(timer));
  if (!state->result->done.load(std::memory_order_acquire)) return;
  const auto info = state->result->info;
  const auto arb = state->result->arb;
  auto *screen = state->screen;
  const auto path = state->path;
  const auto callback = state->callback;
  auto *context = state->context;
  lv_obj_delete(state->overlay);
  if (!info.error.empty()) {
    widgets::Sheet(screen, "Cannot inspect package", info.error);
    return;
  }
  const std::string detail = path + "\n\n" + payload::Summary(info);
  JobRequest request;
  request.job = Job::kInstall;
  request.path = path;
  request.title = "Install ZIP";
  Handler install = [request, callback, context] {
        SetJobRequest(request);
        callback(Action::kRunOperation, context);
      };
  if (!info.is_payload) {
    Sheet(screen, "Install this package?", detail, install, 1500, false,
        SheetPresentation::kStandard, "Swipe to install", [screen, info, path, detail, callback, context](lv_obj_t *area) {
      Layout(area);
      Explanation(area, detail);
      auto *advanced = Button(area, "Advanced  " LV_SYMBOL_RIGHT,
          [screen, info, path, callback, context] { AdvancedZip(screen, info, path, {}, callback, context); });
      lv_obj_set_size(advanced, LV_PCT(100), 96);
    });
    return;
  }
  auto gate = std::make_shared<InstallGate>();
  gate->progress = arb;
  gate->includes_firmware = std::any_of(info.partitions.begin(), info.partitions.end(),
      [](const auto &p) { return p.name == "xbl_config"; });
  Sheet(screen, "Review Android update", "", [gate, install] {
    if (gate->AllowsInstall()) install();
  }, 1900, false,
        SheetPresentation::kStandard, "Swipe to install", [info, path, arb, gate, callback, context](lv_obj_t *area) {
    Layout(area);
    gate->body = area;
    if (info.name_from_filename) Section(area, "PACKAGE NAME");
    auto *version = Label(area, info.target_build.empty() ? "Android update" : info.target_build.c_str(),
                          &lv_font_montserrat_40, kText);
    lv_obj_set_width(version, LV_PCT(100));
    Row(area, "Device", info.target_device);
    Row(area, "Android", info.target_sdk);
    Row(area, "Security patch", info.security_patch);
    Row(area, "Update type", info.incremental ? "Incremental OTA" : "Full OTA");
    Row(area, "Payload size", Size(info.payload_bytes));
    ArbRow(area, arb);
    auto *details = Button(area, "Full information  " LV_SYMBOL_RIGHT,
        [info, path, arb, gate] { FullInfo(gate->overlay, info, path, arb); });
    lv_obj_set_size(details, LV_PCT(100), 96);
    lv_obj_set_style_radius(details, 24, 0);
    lv_obj_set_style_border_width(details, 1, 0);
    lv_obj_set_style_border_color(details, kMainLine, 0);
    gate->details = details;
    if (!info.incremental) {
      auto *fast = Button(area, "Fast flash (experimental)",
          [info, path, gate, callback, context] { ReviewFastFlash(info, path, gate, callback, context); });
      lv_obj_set_size(fast, LV_PCT(100), 96);
      lv_obj_set_style_radius(fast, 24, 0);
      lv_obj_set_style_border_width(fast, 1, 0);
      lv_obj_set_style_border_color(fast, kMainLine, 0);
      gate->fast_flash = fast;
    }
    auto *advanced = Button(area, "Advanced  " LV_SYMBOL_RIGHT,
        [info, path, gate, callback, context] {
      AdvancedZip(gate->overlay, info, path, gate, callback, context);
    });
    lv_obj_set_size(advanced, LV_PCT(100), 96);
    lv_obj_set_style_radius(advanced, 24, 0);
    lv_obj_set_style_border_width(advanced, 1, 0);
    lv_obj_set_style_border_color(advanced, kMainLine, 0);
    gate->advanced = advanced;
  }, [gate](lv_obj_t *slider, lv_obj_t *close) {
    gate->slider = slider;
    gate->overlay = lv_obj_get_parent(lv_obj_get_parent(slider));
    FitReview(*gate);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_HIDDEN);
    i18n::BindLabel(lv_obj_get_child(close, 0), "Abort");
    struct Watch { std::shared_ptr<InstallGate> gate; lv_timer_t *timer = nullptr; };
    auto *watch = new Watch{gate};
    lv_obj_add_event_cb(slider, [](lv_event_t *event) {
      auto *watch = static_cast<Watch *>(lv_event_get_user_data(event));
      if (watch->timer) lv_timer_delete(watch->timer);
      delete watch;
    }, LV_EVENT_DELETE, watch);
    watch->timer = lv_timer_create([](lv_timer_t *timer) {
      auto *watch = static_cast<Watch *>(lv_timer_get_user_data(timer));
      if (!watch->gate->progress->done.load(std::memory_order_acquire)) return;
      UpdateGate(watch->gate);
      watch->timer = nullptr;
      lv_timer_delete(timer);
    }, 100, watch);
  });
}
}
void ReviewPackage(lv_obj_t *screen, const std::string &path,
                   ActionCallback callback, void *context) {
  using namespace widgets;
  auto *overlay = lv_obj_create(screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
  auto *label = Label(overlay, "Reading package information...",
                      &lv_font_montserrat_32, kText);
  lv_obj_center(label);
  auto *cancel = Button(overlay, "Cancel", [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -100);
  auto *state = new Review{screen, overlay, nullptr, path, callback, context};
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    auto *state = static_cast<Review *>(lv_event_get_user_data(event));
    if (state->timer) lv_timer_delete(state->timer);
    delete state;
  }, LV_EVENT_DELETE, state);
  // Worker owns only its result; closing the page never leaves a UI pointer
  // accessible to the background reader and never waits for storage I/O.
  std::thread([result = state->result, path] {
    result->info = payload::InspectZip(path);
    result->done.store(true, std::memory_order_release);
    if (result->info.valid) {
      result->arb->info = payload::InspectZip(path, true);
      result->arb->device = payload::ReadDeviceArb();
      if (result->arb->info.arb_detail.empty())
        result->arb->info.arb_detail = "The package could not be read for the ARB scan";
    }
    result->arb->done.store(true, std::memory_order_release);
  }).detach();
  state->timer = lv_timer_create(Ready, 50, state);
}
}
