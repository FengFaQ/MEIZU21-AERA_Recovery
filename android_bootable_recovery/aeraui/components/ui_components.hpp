/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include "design.hpp"
#include "text_edit.hpp"
#include "scene.hpp"
#include "aeraui/status_bar.hpp"

namespace aeraui::widgets {
using namespace design;
using Handler = std::function<void()>;
inline char kModalMarker;
// Long-lived overlays such as the Media viewer own timers and reusable child
// objects. Back must ask them to close instead of deleting their subtree.
inline char kPersistentModalMarker;
// Claimed only after an actual long press; normal edge gestures stay unchanged.
inline lv_obj_t *kPointerDragOwner = nullptr;
inline bool PointerDragActive() {
  return kPointerDragOwner != nullptr && lv_obj_is_valid(kPointerDragOwner);
}
inline int kPreviousNavigationIndex = -1;

inline lv_color_t IconBackground() {
  return RecoveryTintedIconBackgrounds() ? kAccentSoft : kMainPanel;
}

inline bool Landscape(lv_obj_t *object) {
  lv_obj_t *screen = object == nullptr ? nullptr : lv_obj_get_screen(object);
  return screen != nullptr && lv_obj_get_width(screen) > lv_obj_get_height(screen);
}

inline bool DismissModal(lv_obj_t *screen) {
  if (text_edit::Dismiss(screen)) return true;
  for (int i = static_cast<int>(lv_obj_get_child_count(screen)) - 1; i >= 0; --i) {
    auto *child = lv_obj_get_child(screen, i);
    void *marker = lv_obj_get_user_data(child);
    if (marker == &kPersistentModalMarker &&
        !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_send_event(child, LV_EVENT_CANCEL, nullptr);
      return true;
    }
    if (marker == &kModalMarker && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_delete_async(child);
      return true;
    }
  }
  return false;
}

inline void OnClick(lv_obj_t *object, Handler callback) {
  auto *handler = new Handler(std::move(callback));
  lv_obj_add_event_cb(object, [](lv_event_t *event) {
    auto *fn = static_cast<Handler *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) delete fn;
    else if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
      RecoveryVibrate(Haptic::kTouch);
      (*fn)();
    }
  }, LV_EVENT_ALL, handler);
}

struct PressOrHoldActions {
  Handler click;
  Handler hold;
  bool held = false;
};

inline void OnPressOrHold(lv_obj_t *object, Handler click, Handler hold) {
  auto *actions = new PressOrHoldActions{
      std::move(click), std::move(hold), false};
  lv_obj_add_event_cb(object, [](lv_event_t *event) {
    auto *actions = static_cast<PressOrHoldActions *>(
        lv_event_get_user_data(event));
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
  }, LV_EVENT_ALL, actions);
}

inline lv_obj_t *TextArea(lv_obj_t *parent) {
  auto *input = lv_textarea_create(parent);
  // Touch keyboards do not consistently put their textarea into LV_STATE_FOCUSED.
  // Keep the insertion caret visible regardless of how the field was activated.
  lv_obj_set_style_bg_opa(input, LV_OPA_TRANSP, LV_PART_CURSOR);
  lv_obj_set_style_border_color(input, kAccent, LV_PART_CURSOR);
  lv_obj_set_style_border_opa(input, LV_OPA_COVER, LV_PART_CURSOR);
  lv_obj_set_style_border_width(input, 4, LV_PART_CURSOR);
  lv_obj_set_style_border_side(input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
  lv_obj_set_style_pad_left(input, -2, LV_PART_CURSOR);
  lv_obj_set_style_anim_duration(input, 500, LV_PART_CURSOR);
  text_edit::Attach(input);
  return input;
}

inline void FitLabelToLines(
    lv_obj_t *label, int width, int max_lines,
    std::initializer_list<const lv_font_t *> candidates) {
  if (label == nullptr || width <= 0 || max_lines <= 0 ||
      candidates.size() == 0) return;
  const char *text = lv_label_get_text(label);
  const int letter_space =
      lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
  const int line_space =
      lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
  const lv_font_t *selected = UiFont(*candidates.begin());
  bool fits = false;
  for (const lv_font_t *candidate : candidates) {
    const lv_font_t *font = UiFont(candidate);
    lv_point_t measured{};
    lv_text_get_size(&measured, text == nullptr ? "" : text, font,
                     letter_space, line_space, width, LV_TEXT_FLAG_NONE);
    selected = font;
    const int limit = max_lines * lv_font_get_line_height(font) +
                      (max_lines - 1) * line_space;
    if (measured.y <= limit) {
      fits = true;
      break;
    }
  }
  const int height = max_lines * lv_font_get_line_height(selected) +
                     (max_lines - 1) * line_space;
  lv_obj_set_style_text_font(label, selected, 0);
  lv_label_set_long_mode(label, fits ? LV_LABEL_LONG_MODE_WRAP
                                     : LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(label, width, height);
}

inline void FitButtonLabel(lv_obj_t *button) {
  if (button == nullptr || lv_obj_get_child_count(button) == 0) return;
  auto *label = lv_obj_get_child(button, 0);
  const int width = std::max(40, static_cast<int>(lv_obj_get_width(button)) - 48);
  FitLabelToLines(label, width, 1,
                  {&lv_font_montserrat_32, &lv_font_montserrat_28,
                   &lv_font_montserrat_24, &lv_font_montserrat_20,
                   &lv_font_montserrat_18, &lv_font_montserrat_16});
  lv_obj_center(label);
}

inline lv_obj_t *Button(lv_obj_t *parent, const char *text, Handler action,
                        bool primary = false) {
  auto *button = lv_button_create(parent);
  Clear(button);
  lv_obj_set_style_bg_color(button, primary ? kAccent : kMainPanel, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, primary ? kAccentPressed : kMainSelected,
                            LV_STATE_PRESSED);
  lv_obj_set_style_radius(button, 64, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_transform_scale(button, 250, LV_STATE_PRESSED);
  auto *label = Label(button, text, &lv_font_montserrat_32,
                      primary ? kOnAccent : kText);
  lv_obj_add_event_cb(button, [](lv_event_t *event) {
    FitButtonLabel(lv_event_get_target_obj(event));
  }, LV_EVENT_SIZE_CHANGED, nullptr);
  FitButtonLabel(button);
  if (action) OnClick(button, std::move(action));
  return button;
}

inline void CenterButtonContent(lv_obj_t *button) {
  if (button == nullptr || lv_obj_get_child_count(button) == 0) return;
  auto *content = lv_obj_get_child(button, 0);
  lv_obj_set_size(content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_center(content);
  // Font Awesome symbols carry extra advance space on their right side.
  // Compensate it so the visible glyph, rather than its font box, is centered.
  lv_obj_set_style_translate_x(content, 18, 0);
}

inline void Header(lv_obj_t *screen, const char *title, const char *subtitle,
                    ActionCallback callback, void *context) {
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  auto *heading = Label(screen, title, &lv_font_montserrat_48, kText);
  const bool landscape = Landscape(screen);
  lv_obj_set_pos(heading, 80, landscape ? 210 : 252);
  auto *copy = Label(screen, subtitle, &lv_font_montserrat_32, kMuted);
  lv_obj_set_pos(copy, landscape ? 600 : 80, landscape ? 220 : 338);
  lv_obj_set_width(copy, landscape ? lv_obj_get_width(screen) - 680 : 1280);
  AnimateEnter(heading, 0, 12);
}

inline int NavigationHeight(lv_obj_t *screen) {
  const bool landscape = Landscape(screen);
  const DockLayout layout = RecoveryDockLayout();
  const bool compact = layout == DockLayout::kCompact;
  const bool minimal = layout == DockLayout::kMinimal;
  const bool icons_only = layout == DockLayout::kIcons;
  const bool text_only = layout == DockLayout::kTextOnly;
  return landscape ? (minimal ? 138 : 160)
                   : (minimal ? 170 : compact || icons_only || text_only ? 188 : 226);
}

inline void UpdateNavigationDockAppearance(lv_obj_t *bar, int transparency,
                                           int blur) {
  if (bar == nullptr || lv_obj_get_child_count(bar) == 0) return;
  auto *shelf = lv_obj_get_child(bar, 0);
  const DockLayout layout = RecoveryDockLayout();
  const bool bare = layout == DockLayout::kMinimal ||
      layout == DockLayout::kTextOnly;
  transparency = std::clamp(transparency, 0, 100);
  blur = std::clamp(blur, 0, 100);
  lv_opa_t surface_opa = static_cast<lv_opa_t>(
      (100 - transparency) * LV_OPA_COVER / 100);
  const int surface_style = SurfaceStyleValue();
  if (surface_style == 1)
    surface_opa = static_cast<lv_opa_t>(surface_opa * 70 / 100);
  else if (surface_style == 2)
    surface_opa = static_cast<lv_opa_t>(surface_opa * 14 / 100);
  lv_obj_set_style_bg_opa(shelf, bare ? LV_OPA_TRANSP : surface_opa, 0);
  lv_obj_set_style_blur_backdrop(
      shelf, (blur > 0 || surface_style == 1) && !bare, 0);
  /* The former 40% already gives the strongest useful dock blur. Keep the
   * control's friendly 0-100% scale while mapping its full range to that
   * restrained radius. */
  lv_obj_set_style_blur_radius(
      shelf, std::max(blur * 24 / 250, surface_style == 1 ? 16 : 0), 0);
  lv_obj_invalidate(shelf);
}

inline lv_obj_t *Navigation(lv_obj_t *screen, Action active,
                            ActionCallback callback, void *context,
                            bool app_surface = false) {
  struct Tab { const char *icon; const char *text; Action action; };
  constexpr std::array<Tab, 4> tabs{{
    {LV_SYMBOL_HOME, "Home", Action::kBackHome},
    {LV_SYMBOL_SAVE, "Backup", Action::kBackup},
    {LV_SYMBOL_TRASH, "Wipe", Action::kWipe},
    {LV_SYMBOL_LIST, "Menu", Action::kSettings}}};
  // All dock styles share actions and safe-area placement. Selection uses
  // the theme accent while the underlying surfaces stay neutral.
  const bool landscape = Landscape(screen);
  const DockLayout layout = RecoveryDockLayout();
  const bool compact = layout == DockLayout::kCompact;
  const bool minimal = layout == DockLayout::kMinimal;
  const bool icons_only = layout == DockLayout::kIcons;
  const bool text_only = layout == DockLayout::kTextOnly;
  const int bar_width = landscape
      ? (icons_only ? 1120 : minimal ? 1360 : compact || text_only ? 1600 : 1800)
      : (icons_only ? 860 : minimal ? 1120 : compact || text_only ? 1280 : 1440);
  const int bar_height = NavigationHeight(screen);
  const int shelf_x = landscape || compact || minimal || icons_only || text_only ? 0 : 120;
  const int shelf_y = minimal ? 4 : landscape ? 8 : 18;
  const int shelf_width = landscape || compact || minimal || icons_only || text_only
      ? bar_width : 1200;
  const int shelf_height = minimal ? bar_height - 8 :
      landscape ? 140 : compact || icons_only || text_only ? 156 : 180;
  auto *bar = lv_obj_create(screen);
  Clear(bar);
  lv_obj_set_size(bar, bar_width, bar_height);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  // Plugin surfaces are gesture-first. Keep their content full-screen and let
  // the engine's bottom-edge Recents gesture replace the recovery dock.
  if (app_surface) {
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    return bar;
  }

  // Keep the first child as the shared surface for live appearance updates.
  // Text Only leaves it transparent.
  auto *shelf = lv_obj_create(bar);
  Panel(shelf, shelf_height / 2, kMainSheet);
  lv_obj_remove_flag(shelf, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(shelf, shelf_x, shelf_y);
  lv_obj_set_size(shelf, shelf_width, shelf_height);
  lv_obj_set_style_bg_color(shelf, kMainSheet, 0);
  lv_obj_set_style_bg_grad_color(shelf, kMainPanel, 0);
  lv_obj_set_style_bg_grad_dir(
      shelf, SurfaceStyleValue() >= 1 && SurfaceStyleValue() <= 2
                 ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(shelf, minimal ? 0 : 1, 0);
  lv_obj_set_style_border_color(shelf, kMainLine, 0);
  lv_obj_set_style_border_opa(shelf, LV_OPA_30, 0);
  lv_obj_set_style_shadow_color(shelf, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(shelf, 12, 0);
  lv_obj_set_style_shadow_offset_y(shelf, 6, 0);
  lv_obj_set_style_shadow_opa(shelf, LV_OPA_10, 0);
  lv_obj_set_style_blur_quality(shelf, LV_BLUR_QUALITY_SPEED, 0);
  if (SurfaceStyleValue() == 1) {
    lv_obj_set_style_border_opa(shelf, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(shelf, 0, 0);
  } else if (SurfaceStyleValue() == 2) {
    lv_obj_set_style_border_width(shelf, minimal ? 0 : 2, 0);
    lv_obj_set_style_border_opa(shelf, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(shelf, 0, 0);
  } else if (SurfaceStyleValue() == 3) {
    lv_obj_set_style_shadow_width(shelf, 22, 0);
    lv_obj_set_style_shadow_offset_y(shelf, 7, 0);
    lv_obj_set_style_shadow_opa(shelf, LV_OPA_30, 0);
  }
  if (text_only) {
    lv_obj_set_style_bg_grad_dir(shelf, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(shelf, 0, 0);
    lv_obj_set_style_shadow_width(shelf, 0, 0);
  }
  const int slot_width = shelf_width / static_cast<int>(tabs.size());

  for (size_t i = 0; i < tabs.size(); ++i) {
    const auto tab = tabs[i];
    const bool selected = active == tab.action;
    auto *button = lv_button_create(bar);
    Clear(button);
    lv_obj_set_pos(button, shelf_x + static_cast<int>(i) * slot_width, shelf_y);
    lv_obj_set_size(button, slot_width, shelf_height);
    lv_obj_set_style_radius(button, minimal ? 34 : shelf_height / 2, 0);
    if (minimal && selected) {
      lv_obj_set_style_bg_color(button, kAccentSoft, 0);
      lv_obj_set_style_bg_opa(button, LV_OPA_50, 0);
    }
    lv_obj_set_style_bg_color(button, Color(0xffffff), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(button, 250, LV_STATE_PRESSED);
    if (text_only) {
      lv_obj_set_style_radius(button, 16, 0);
      lv_obj_set_style_shadow_width(button, 0, 0);
      lv_obj_set_style_bg_color(button, kMainSelected, LV_STATE_PRESSED);
    }
    OnClick(button, [=] { callback(tab.action, context); });

    if (text_only) {
      auto *text = Label(button, tab.text, &lv_font_montserrat_36,
                          selected ? kAccent : kMutedStrong);
      FitLabelToLines(text, slot_width - 32, 1,
          {&lv_font_montserrat_36, &lv_font_montserrat_32,
           &lv_font_montserrat_28, &lv_font_montserrat_24});
      lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(text);
      if (selected) {
        auto *mark = lv_obj_create(button);
        Clear(mark);
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(mark, 72, 3);
        lv_obj_set_style_bg_color(mark, kAccent, 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_align(mark, LV_ALIGN_BOTTOM_MID, 0, -22);
      }
      AnimateEnter(button, 45 + static_cast<uint32_t>(i) * 22, 7);
      continue;
    }

    auto *icon = Label(button, tab.icon, &lv_font_montserrat_48,
                        selected ? kAccent : kMutedStrong);
    if (icons_only) lv_obj_center(icon);
    else if (compact) lv_obj_align(icon, LV_ALIGN_LEFT_MID, 38, 0);
    else lv_obj_align(icon, LV_ALIGN_TOP_MID, 0,
                      minimal ? 14 : landscape ? 18 : 28);
    auto *text = Label(button, tab.text, &lv_font_montserrat_32,
                        selected ? kAccent : kMutedStrong);
    if (icons_only) lv_obj_add_flag(text, LV_OBJ_FLAG_HIDDEN);
    else if (compact) lv_obj_align(text, LV_ALIGN_LEFT_MID, 112, 0);
    else lv_obj_align(text, LV_ALIGN_BOTTOM_MID, 0,
                      minimal ? -10 : landscape ? -12 : -20);
    if (selected) {
      lv_obj_set_style_transform_scale(icon, 192, 0);
      lv_obj_set_style_translate_y(icon, 8, 0);
      lv_anim_t icon_pop;
      lv_anim_init(&icon_pop);
      lv_anim_set_var(&icon_pop, icon);
      lv_anim_set_values(&icon_pop, 192, 256);
      lv_anim_set_duration(&icon_pop, 380);
      lv_anim_set_delay(&icon_pop, 55);
      lv_anim_set_path_cb(&icon_pop, lv_anim_path_overshoot);
      lv_anim_set_exec_cb(&icon_pop, [](void *target, int32_t value) {
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t *>(target), value, 0);
      });
      lv_anim_start(&icon_pop);
      lv_anim_t rise;
      lv_anim_init(&rise);
      lv_anim_set_var(&rise, icon);
      lv_anim_set_values(&rise, 8, 0);
      lv_anim_set_duration(&rise, 300);
      lv_anim_set_delay(&rise, 35);
      lv_anim_set_path_cb(&rise, lv_anim_path_ease_out);
      lv_anim_set_exec_cb(&rise, [](void *target, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(target), value, 0);
      });
      lv_anim_start(&rise);
    }
    AnimateEnter(button, 45 + static_cast<uint32_t>(i) * 22, 7);
  }
  UpdateNavigationDockAppearance(bar, RecoveryDockTransparency(), RecoveryDockBlur());
  return bar;
}

inline void StyleSwitch(lv_obj_t *toggle, bool enabled) {
  lv_obj_set_size(toggle, 108, 60);
  lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(toggle, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER,
                          LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(toggle, kAccent,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(toggle, kText, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(toggle, -8, LV_PART_KNOB);
  if (enabled) lv_obj_add_state(toggle, LV_STATE_CHECKED);
  else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
}

inline lv_obj_t *SettingsRow(lv_obj_t *parent, int y, const char *title,
                      const char *detail, bool enabled,
                      std::function<bool(bool)> setter,
                      lv_obj_t *message) {
  auto *row = lv_button_create(parent);
  Panel(row, 30, kMainPanel);
  Interactive(row, kMainSelected);
  lv_obj_set_pos(row, 0, y);
  lv_obj_update_layout(parent);
  const int width = std::max(320, static_cast<int>(lv_obj_get_width(parent)));
  lv_obj_set_size(row, width, 160);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_color(row, kMainLine, 0);
  lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
  auto *name = Label(row, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 32, 24);
  FitLabelToLines(name, width - 250, 1,
                  {&lv_font_montserrat_32, &lv_font_montserrat_28, &lv_font_montserrat_24});
  auto *copy = Label(row, detail, &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(copy, 32, 88);
  lv_obj_set_width(copy, width - 250);
  FitLabelToLines(copy, width - 250, 2,
                  {&lv_font_montserrat_20, &lv_font_montserrat_18});
  auto *toggle = lv_switch_create(row);
  StyleSwitch(toggle, enabled);
  lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -32, 0);
  OnClick(row, [toggle, setter = std::move(setter), message] {
    const bool desired = !lv_obj_has_state(toggle, LV_STATE_CHECKED);
    if (setter(desired)) {
      if (desired) lv_obj_add_state(toggle, LV_STATE_CHECKED);
      else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
      i18n::BindLabel(message, "Settings saved");
      lv_obj_set_style_text_color(message, kGreen, 0);
    } else {
      i18n::BindLabel(message,
          "This setting could not be changed.");
      lv_obj_set_style_text_color(message, kRed, 0);
    }
  });
  return row;
}

inline lv_obj_t *Scroll(lv_obj_t *parent, int y, int height) {
  auto *list = lv_obj_create(parent);
  Clear(list);
  const bool landscape = Landscape(parent);
  lv_obj_set_pos(list, 64, y);
  lv_obj_set_size(list, landscape ? lv_obj_get_width(lv_obj_get_screen(parent)) - 128
                                  : 1312,
                  height);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 5, LV_PART_SCROLLBAR);
  return list;
}

inline lv_obj_t *Row(lv_obj_t *parent, int y, const char *symbol,
                     const std::string &title, const std::string &detail,
                     Handler action, const char *trailing = LV_SYMBOL_RIGHT,
                     Handler hold = {}) {
  Handler click = action;
  auto *row = Button(parent, "", hold ? Handler{} : std::move(action));
  if (hold) OnPressOrHold(row, std::move(click), std::move(hold));
  lv_obj_set_pos(row, 0, y);
  // Newly-created scroll containers have not necessarily completed an LVGL
  // layout pass yet. Reading their width too early returns zero and collapses
  // preference labels into a one-character-wide column.
  lv_obj_update_layout(parent);
  const int width = std::max(320, static_cast<int>(lv_obj_get_width(parent)));
  lv_obj_set_size(row, width, 174);
  // Rows retain color feedback, without a scaled temporary layer on touch.
  lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(row, 24, 0);
  auto *icon = Label(row, symbol, &lv_font_montserrat_32, kAccent);
  lv_obj_set_pos(icon, 32, 63);
  auto *name = Label(row, title.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 116, 34);
  SingleLineLabel(name, width - 272, &lv_font_montserrat_32);
  auto *copy = Label(row, detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 116, 96);
  SingleLineLabel(copy, width - 272, &lv_font_montserrat_24);
  auto *end = Label(row, trailing, &lv_font_montserrat_32, kMuted);
  lv_obj_align(end, LV_ALIGN_RIGHT_MID, -28, 0);
  return row;
}

inline std::string Size(uint64_t bytes) {
  char result[64];
  if (bytes >= 1073741824ULL)
    snprintf(result, sizeof(result), "%.1f GB", bytes / 1073741824.0);
  else if (bytes >= 1048576ULL)
    snprintf(result, sizeof(result), "%.1f MB", bytes / 1048576.0);
  else if (bytes >= 1024)
    snprintf(result, sizeof(result), "%.1f KB", bytes / 1024.0);
  else snprintf(result, sizeof(result), "%llu bytes", (unsigned long long)bytes);
  return result;
}

inline std::string ReadLog() {
  FILE *file = fopen("/tmp/recovery.log", "re");
  if (!file) return "Recovery log is not available yet.";
  fseek(file, 0, SEEK_END);
  const long length = ftell(file);
  const long start = std::max(0L, length - 10000);
  fseek(file, start, SEEK_SET);
  char buffer[10001];
  const size_t count = fread(buffer, 1, 10000, file);
  fclose(file);
  std::string result(buffer, count);
  if (start > 0) {
    const auto line = result.find('\n');
    if (line != std::string::npos) result.erase(0, line + 1);
  }
  return result.empty() ? "No output yet." : result;
}

constexpr int32_t kConfirmTrackWidth = 1140;
constexpr int32_t kConfirmTrackHeight = 128;
constexpr int32_t kConfirmKnobSize = 104;
constexpr int32_t kConfirmKnobInset = 12;
constexpr int32_t kConfirmFillInset = 6;
constexpr int32_t kConfirmTravel =
    kConfirmTrackWidth - kConfirmKnobSize - 2 * kConfirmKnobInset;

enum class SheetPresentation {
  kStandard,
  kCompactGlass,
};

struct SheetState {
  lv_obj_t *overlay = nullptr;
  Handler confirm;
  lv_obj_t *fill = nullptr;
  lv_obj_t *knob = nullptr;
  lv_obj_t *slider_copy = nullptr;
  int32_t touch_origin_x = 0;
  int32_t slider_offset = 0;
  int32_t slider_travel = kConfirmTravel;
  bool dragging = false;
  bool confirming = false;
};

inline void SetConfirmOffset(void *object, int32_t value) {
  auto *state = static_cast<SheetState *>(object);
  if (state == nullptr || state->knob == nullptr) return;
  state->slider_offset = std::clamp(value, 0, state->slider_travel);
  lv_obj_set_x(state->knob, kConfirmKnobInset + state->slider_offset);
  if (state->slider_offset == 0) {
    lv_obj_add_flag(state->fill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(state->fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(state->fill,
                     kConfirmKnobInset + kConfirmKnobSize / 2 +
                         state->slider_offset - kConfirmFillInset);
  }
  const int32_t opacity =
      std::clamp(255 - state->slider_offset * 220 /
          std::max(1, state->slider_travel), 25, 255);
  lv_obj_set_style_text_opa(state->slider_copy,
                            static_cast<lv_opa_t>(opacity), 0);
}

inline void FinishConfirmation(lv_anim_t *animation) {
  auto *state = static_cast<SheetState *>(lv_anim_get_user_data(animation));
  if (state == nullptr || !state->confirm) return;
  lv_obj_add_flag(state->overlay, LV_OBJ_FLAG_HIDDEN);
  state->confirm();
}

inline void ConfirmSliderTouch(lv_event_t *event) {
  auto *state = static_cast<SheetState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->confirming) return;
  auto *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);

  switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED: {
      lv_area_t bounds{};
      lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
      state->dragging = point.x <= bounds.x1 + kConfirmKnobSize + 48;
      state->touch_origin_x = point.x - state->slider_offset;
      lv_anim_delete(state, SetConfirmOffset);
      break;
    }
    case LV_EVENT_PRESSING:
      if (state->dragging)
        SetConfirmOffset(state, point.x - state->touch_origin_x);
      break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
      if (!state->dragging) break;
      state->dragging = false;
      const bool accepted =
          state->slider_offset >= state->slider_travel * 82 / 100;
      lv_anim_t settle;
      lv_anim_init(&settle);
      lv_anim_set_var(&settle, state);
      lv_anim_set_values(&settle, state->slider_offset,
                         accepted ? state->slider_travel : 0);
      lv_anim_set_duration(&settle, accepted ? 140 : 280);
      lv_anim_set_path_cb(&settle, lv_anim_path_ease_out);
      lv_anim_set_exec_cb(&settle, SetConfirmOffset);
      if (accepted) {
        state->confirming = true;
        lv_anim_set_user_data(&settle, state);
        lv_anim_set_completed_cb(&settle, FinishConfirmation);
      }
      lv_anim_start(&settle);
      break;
    }
    default:
      break;
  }
}

// A restrained review sheet using the same reliable custom drag model as the
// lock screen. Confirmation is impossible unless the gesture starts at the
// handle and crosses most of the track.
inline void Sheet(lv_obj_t *screen, const std::string &title,
                   const std::string &copy, Handler confirm = {},
                   int preferred_height = 0,
                   bool dismiss_on_backdrop = false,
                   SheetPresentation presentation = SheetPresentation::kStandard,
                   const char *confirm_text = "Swipe to confirm",
                   std::function<void(lv_obj_t *)> build_body = {},
                   std::function<void(lv_obj_t *, lv_obj_t *)> setup_actions = {},
                   Handler on_close = {}) {
  const bool compact_glass =
      presentation == SheetPresentation::kCompactGlass;
  auto *overlay = lv_obj_create(screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  // A modal must cover its owner, including when the owner uses flex layout
  // or padding. Otherwise it consumes layout space and its actions fall below
  // the visible page (Advanced payload review is one such owner).
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(overlay, lv_obj_get_width(screen), lv_obj_get_height(screen));
  lv_obj_set_pos(overlay, -lv_obj_get_style_pad_left(screen, LV_PART_MAIN),
                          -lv_obj_get_style_pad_top(screen, LV_PART_MAIN));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_30, 0);
  if (dismiss_on_backdrop) {
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        overlay,
        [](lv_event_t *event) {
          if (lv_event_get_target_obj(event) ==
              lv_event_get_current_target_obj(event))
            lv_obj_delete_async(lv_event_get_current_target_obj(event));
        },
        LV_EVENT_CLICKED, nullptr);
  }
  auto *state = new SheetState;
  state->overlay = overlay;
  state->confirm = std::move(confirm);
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    auto *state = static_cast<SheetState *>(lv_event_get_user_data(event));
    lv_anim_delete(state, SetConfirmOffset);
    delete state;
  }, LV_EVENT_DELETE, state);

  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_style_bg_grad_dir(sheet, LV_GRAD_DIR_NONE, 0);
  lv_obj_set_style_border_width(sheet, 1, 0);
  lv_obj_set_style_border_color(sheet, kMainLine, 0);
  lv_obj_set_style_border_opa(sheet, LV_OPA_50, 0);
  const bool landscape = Landscape(screen);
  const int sheet_width = compact_glass
      ? (landscape
          ? std::min(1700, static_cast<int>(lv_obj_get_width(screen)) - 256)
          : std::min(1220, static_cast<int>(lv_obj_get_width(screen)) - 144))
      : (landscape
          ? std::min(2200, static_cast<int>(lv_obj_get_width(screen)) - 128)
          : 1312);
  const int default_height = compact_glass
      ? (landscape ? 660 : 760)
      : (state->confirm ? 1180 : 1000);
  const int requested_height = preferred_height > 0 ? preferred_height : default_height;
  const int sheet_height = std::min(
      requested_height, static_cast<int>(lv_obj_get_height(screen)) - 80);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_set_style_pad_all(sheet, compact_glass ? 48 : 56, 0);
  lv_obj_set_style_bg_opa(sheet,
      IsLightMode() ? LV_OPA_90 : LV_OPA_80, 0);
  lv_obj_set_style_blur_backdrop(sheet, true, 0);
  lv_obj_set_style_blur_radius(sheet, 18, 0);
  lv_obj_set_style_blur_quality(sheet, LV_BLUR_QUALITY_SPEED, 0);
  lv_obj_set_style_shadow_color(sheet, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(sheet, 40, 0);
  lv_obj_set_style_shadow_offset_y(sheet, 10, 0);
  lv_obj_set_style_shadow_opa(sheet, LV_OPA_30, 0);

  auto *grabber = lv_obj_create(sheet);
  Clear(grabber);
  lv_obj_set_size(grabber, 112, 8);
  lv_obj_align(grabber, LV_ALIGN_TOP_MID, 0, -27);
  lv_obj_set_style_radius(grabber, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(grabber, kMutedStrong, 0);
  lv_obj_set_style_bg_opa(grabber, LV_OPA_30, 0);

  auto *heading = Label(sheet, title.c_str(), &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 0, compact_glass ? 24 : 34);
  lv_obj_set_width(heading, sheet_width - (compact_glass ? 96 : 112));
  lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);

  auto *area = lv_obj_create(sheet);
  Clear(area);
  lv_obj_set_pos(area, 0, compact_glass ? 112 : 132);
  lv_obj_set_size(area, sheet_width - (compact_glass ? 96 : 112),
                  compact_glass ? (landscape ? 124 : 190)
                                : (state->confirm ? sheet_height - 670
                                                  : sheet_height - 390));
  lv_obj_add_flag(area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(area, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(area, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(area, 5, LV_PART_SCROLLBAR);
  if (build_body) {
    build_body(area);
  } else {
  auto *body = Label(area, copy.c_str(), &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(body, sheet_width - (compact_glass ? 136 : 152));
  lv_obj_set_style_text_line_space(body, 16, 0);
  }

  lv_obj_t *confirm_slider = nullptr;
  if (state->confirm) {
    auto *divider = lv_obj_create(sheet);
    Clear(divider);
    lv_obj_set_size(divider, sheet_width - (compact_glass ? 96 : 112), 1);
    if (compact_glass)
      lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, landscape ? 286 : 348);
    else
      lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, -360);
    lv_obj_set_style_bg_color(divider, kMainLine, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_40, 0);

    auto *slider = lv_obj_create(sheet);
    confirm_slider = slider;
    Clear(slider);
    const int track_width = compact_glass ? 1020 : kConfirmTrackWidth;
    state->slider_travel =
        track_width - kConfirmKnobSize - 2 * kConfirmKnobInset;
    lv_obj_set_size(slider, track_width, kConfirmTrackHeight);
    if (compact_glass)
      lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, landscape ? 330 : 394);
    else
      lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -190);
    lv_obj_set_style_radius(slider, kConfirmTrackHeight / 2, 0);
    lv_obj_set_style_bg_color(slider, kInset, 0);
    lv_obj_set_style_bg_opa(slider,
                            compact_glass ? LV_OPA_80 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(slider, 1, 0);
    lv_obj_set_style_border_color(slider, kMainLine, 0);
    lv_obj_set_style_border_opa(slider, LV_OPA_50, 0);
    lv_obj_set_style_clip_corner(slider, true, 0);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slider, ConfirmSliderTouch, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(slider, ConfirmSliderTouch, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(slider, ConfirmSliderTouch, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(slider, ConfirmSliderTouch, LV_EVENT_PRESS_LOST, state);

    state->fill = lv_obj_create(slider);
    Clear(state->fill);
    lv_obj_set_pos(state->fill, kConfirmFillInset, kConfirmFillInset);
    lv_obj_set_size(state->fill, 1,
                    kConfirmTrackHeight - 2 * kConfirmFillInset);
    lv_obj_set_style_radius(state->fill,
                            (kConfirmTrackHeight - 2 * kConfirmFillInset) / 2,
                            0);
    // Use the real theme accent as a restrained tint. This keeps the fill
    // visually tied to the handle and avoids the muddy, offset-looking block
    // produced by the opaque accent-soft surface.
    lv_obj_set_style_bg_color(state->fill, kAccent, 0);
    lv_obj_set_style_bg_opa(state->fill, LV_OPA_30, 0);
    lv_obj_add_flag(state->fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(state->fill, LV_OBJ_FLAG_CLICKABLE);

    state->slider_copy =
        Label(slider, confirm_text, &lv_font_montserrat_32, kMutedStrong);
    lv_obj_align(state->slider_copy, LV_ALIGN_CENTER, 36, 0);
    lv_obj_remove_flag(state->slider_copy, LV_OBJ_FLAG_CLICKABLE);

    state->knob = lv_obj_create(slider);
    Clear(state->knob);
    lv_obj_set_size(state->knob, kConfirmKnobSize, kConfirmKnobSize);
    lv_obj_set_pos(state->knob, kConfirmKnobInset, kConfirmKnobInset);
    lv_obj_set_style_radius(state->knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(state->knob, kAccent, 0);
    lv_obj_set_style_bg_opa(state->knob, LV_OPA_COVER, 0);
    lv_obj_remove_flag(state->knob, LV_OBJ_FLAG_CLICKABLE);
    auto *chevron = Label(state->knob, LV_SYMBOL_RIGHT,
                           &lv_font_montserrat_48, Color(0x071116));
    lv_obj_center(chevron);
    lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
  }

  auto *close = Button(sheet, state->confirm ? "Cancel" : "Close", [overlay, on_close] {
    if (on_close) on_close();
    else lv_obj_delete_async(overlay);
  });
  lv_obj_set_size(close, compact_glass ? 280 : 1140,
                  compact_glass ? 92 : 100);
  lv_obj_align(close,
               compact_glass ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_MID,
               0, compact_glass ? -4 : -8);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(close, LV_OPA_10, LV_STATE_PRESSED);
  if (setup_actions) setup_actions(confirm_slider, close);
  AnimateEnter(sheet, 0, 52);
}
}  // namespace aeraui::widgets
