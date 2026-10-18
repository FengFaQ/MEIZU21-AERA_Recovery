/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <cstdio>
#include "ui_components.hpp"
#include "../../aera_remote/pc_connection.hpp"

namespace aeraui::widgets {

class PcTransferBanner {
 public:
  void Clear() {
    Close();
    if (banner_) lv_obj_delete(banner_);
    banner_ = name_ = metrics_ = bar_ = nullptr;
    progress_ = {};
  }
  bool Expanded() const { return detail_ != nullptr; }
  void Close() {
    if (detail_) lv_obj_delete(detail_);
    detail_ = detail_name_ = detail_amount_ = detail_speed_ = detail_eta_ = detail_bar_ = cancel_ = nullptr;
  }
  void Refresh(const aera::pc::UploadProgress &progress, bool visible = true) {
    if (!progress.active) { Clear(); return; }
    if (progress_.id != progress.id) Clear();
    progress_ = progress;
    if (!banner_) BuildBanner();
    if (!visible) Close();
    lv_obj_set_flag(banner_, LV_OBJ_FLAG_HIDDEN, !visible || Expanded());
    const int percent = progress.bytes ? static_cast<int>(
        100.0 * std::min(progress.received, progress.bytes) / progress.bytes) : 0;
    char amount[128], speed[64];
    snprintf(amount, sizeof(amount), "%.1f / %.1f MB  /  %d%%",
        progress.received / 1000000.0, progress.bytes / 1000000.0, percent);
    if (!progress.bytes) snprintf(amount, sizeof(amount), "%.1f MB", progress.received / 1000000.0);
    snprintf(speed, sizeof(speed), "%.1f MB/s", progress.bytes_per_second / 1000000.0);
    const char *stage = progress.cancelling ? i18n::Translate("Cancelling...") :
        progress.finalizing ? i18n::Translate("Finishing up") : nullptr;
    lv_label_set_text(name_, progress.name.c_str());
    const std::string compact = std::string(amount) + "  /  " + (stage ? stage : speed);
    lv_label_set_text(metrics_, compact.c_str());
    lv_bar_set_value(bar_, percent, LV_ANIM_OFF);
    if (!detail_) return;
    lv_label_set_text(detail_name_, progress.name.c_str());
    lv_label_set_text(detail_amount_, amount);
    lv_label_set_text(detail_speed_, stage ? stage : speed);
    std::string eta;
    if (!stage && progress.remaining_seconds >= 0) {
      const auto seconds = static_cast<uint64_t>(progress.remaining_seconds + .5);
      char duration[48];
      snprintf(duration, sizeof(duration), "%llu:%02llu", static_cast<unsigned long long>(seconds / 60),
          static_cast<unsigned long long>(seconds % 60));
      eta = i18n::Format("About %s remaining", duration);
    }
    lv_label_set_text(detail_eta_, eta.c_str());
    lv_bar_set_value(detail_bar_, percent, LV_ANIM_OFF);
    lv_obj_set_state(cancel_, LV_STATE_DISABLED, progress.cancelling);
  }
  void Open() {
    if (!progress_.active || detail_) return;
    using namespace design;
    detail_ = lv_obj_create(lv_layer_top());
    ClearObject(detail_);
    lv_obj_set_size(detail_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(detail_, kMainCanvas, 0);
    lv_obj_set_style_bg_opa(detail_, LV_OPA_COVER, 0);
    AttachStatusBar(detail_, nullptr, nullptr, StatusBarAction::kNone, true);
    lv_obj_update_layout(detail_);
    const int width = lv_obj_get_width(detail_);
    const int top = StatusBarHeight() + 40;
    auto *title = Label(detail_, "File transfer", &lv_font_montserrat_40, kText);
    lv_obj_set_pos(title, 60, top); lv_obj_set_width(title, width - 240);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    auto *close = Button(detail_, LV_SYMBOL_CLOSE, [this] {
      Close(); Refresh(progress_);
    });
    lv_obj_set_size(close, 100, 90); lv_obj_set_pos(close, width - 160, top - 10);
    auto *body = lv_obj_create(detail_);
    design::Clear(body);
    lv_obj_set_pos(body, 60, top + 120);
    lv_obj_set_size(body, width - 120, std::max(80, lv_obj_get_height(detail_) - top - 360));
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    detail_name_ = Label(body, progress_.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(detail_name_, 0, 0);
    lv_obj_set_size(detail_name_, width - 120, 110);
    lv_label_set_long_mode(detail_name_, LV_LABEL_LONG_DOT);
    detail_amount_ = Label(body, "", &lv_font_montserrat_32, kMutedStrong);
    lv_obj_set_pos(detail_amount_, 0, 130); lv_obj_set_width(detail_amount_, width - 120);
    detail_bar_ = ProgressBar(body, width - 120);
    lv_obj_set_pos(detail_bar_, 0, 210);
    detail_speed_ = Label(body, "", &lv_font_montserrat_32, kAccent);
    lv_obj_set_pos(detail_speed_, 0, 270); lv_obj_set_width(detail_speed_, width - 120);
    detail_eta_ = Label(body, "", &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail_eta_, 0, 345); lv_obj_set_width(detail_eta_, width - 120);
    cancel_ = Button(detail_, "Cancel transfer", [this] {
      if (aera::pc::CancelUpload(progress_.id))
        lv_obj_add_state(cancel_, LV_STATE_DISABLED);
    });
    lv_obj_set_size(cancel_, width - 120, 110);
    lv_obj_set_style_text_align(lv_obj_get_child(cancel_, 0), LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(cancel_, LV_ALIGN_BOTTOM_MID, 0, -80);
    Refresh(progress_);
  }

 private:
  static void ClearObject(lv_obj_t *object) {
    widgets::Clear(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  }
  static lv_obj_t *ProgressBar(lv_obj_t *parent, int width) {
    auto *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, width, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, design::kMainLine, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(bar, design::kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    return bar;
  }
  void BuildBanner() {
    using namespace design;
    banner_ = lv_obj_create(lv_layer_top());
    ClearObject(banner_);
    const int width = std::min(1280, lv_display_get_horizontal_resolution(lv_display_get_default()) - 100);
    Panel(banner_, 24, kMainSheet);
    lv_obj_set_style_pad_all(banner_, 0, 0);
    lv_obj_set_style_bg_opa(banner_, LV_OPA_90, 0);
    lv_obj_set_size(banner_, width, 132);
    lv_obj_align(banner_, LV_ALIGN_TOP_MID, 0, StatusBarHeight() + 10);
    name_ = Label(banner_, "", &lv_font_montserrat_24, kText);
    lv_obj_set_pos(name_, 24, 14); SingleLineLabel(name_, width - 48, &lv_font_montserrat_24);
    metrics_ = Label(banner_, "", &lv_font_montserrat_20, kMutedStrong);
    lv_obj_set_pos(metrics_, 24, 62); SingleLineLabel(metrics_, width - 48, &lv_font_montserrat_20);
    bar_ = ProgressBar(banner_, width - 48); lv_obj_set_pos(bar_, 24, 110);
    OnClick(banner_, [this] { Open(); });
  }
  aera::pc::UploadProgress progress_;
  lv_obj_t *banner_ = nullptr, *name_ = nullptr, *metrics_ = nullptr, *bar_ = nullptr;
  lv_obj_t *detail_ = nullptr, *detail_name_ = nullptr, *detail_amount_ = nullptr;
  lv_obj_t *detail_speed_ = nullptr, *detail_eta_ = nullptr, *detail_bar_ = nullptr, *cancel_ = nullptr;
};
}  // namespace aeraui::widgets
