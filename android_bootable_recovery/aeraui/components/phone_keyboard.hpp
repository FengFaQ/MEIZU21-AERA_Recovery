/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>
#include <algorithm>
#include <cstring>
#include <vector>

#include "lvgl.h"
#include "src/misc/lv_text_private.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#include "design.hpp"
#include "aeraui/backend.hpp"

namespace aeraui::phone_keyboard {

struct SecondaryContact {
  lv_obj_t *keyboard = nullptr;
  uint32_t button = LV_BUTTONMATRIX_BUTTON_NONE;
  bool pressed = false;
};

inline std::vector<lv_obj_t *> gKeyboards;
inline SecondaryContact gSecondary;

inline lv_obj_t *VisibleKeyboard() {
  auto *screen = lv_screen_active();
  for (auto it = gKeyboards.rbegin(); it != gKeyboards.rend(); ++it) {
    auto *keyboard = *it;
    if (lv_obj_is_valid(keyboard) && lv_obj_get_screen(keyboard) == screen &&
        lv_obj_is_visible(keyboard))
      return keyboard;
  }
  return nullptr;
}

// LVGL's button matrix owns one selected-button field, so two pointer devices
// still overwrite each other on a keyboard. Resolve the secondary contact
// against the matrix geometry ourselves and dispatch its key independently.
inline uint32_t ButtonAt(lv_obj_t *keyboard, int x, int y) {
  if (keyboard == nullptr) return LV_BUTTONMATRIX_BUTTON_NONE;
  auto *matrix = reinterpret_cast<lv_buttonmatrix_t *>(keyboard);
  lv_area_t object{};
  lv_obj_get_coords(keyboard, &object);
  const int width = lv_obj_get_width(keyboard);
  const int height = lv_obj_get_height(keyboard);
  const int left = lv_obj_get_style_pad_left(keyboard, LV_PART_MAIN);
  const int right = lv_obj_get_style_pad_right(keyboard, LV_PART_MAIN);
  const int top = lv_obj_get_style_pad_top(keyboard, LV_PART_MAIN);
  const int bottom = lv_obj_get_style_pad_bottom(keyboard, LV_PART_MAIN);
  const int extra = LV_DPI_DEF / 10;
  const int row_gap = std::min(
      (lv_obj_get_style_pad_row(keyboard, LV_PART_MAIN) / 2) + 1,
      extra);
  const int column_gap = std::min(
      (lv_obj_get_style_pad_column(keyboard, LV_PART_MAIN) / 2) + 1,
      extra);
  const lv_point_t point{x, y};

  for (uint32_t id = 0; id < matrix->btn_cnt; ++id) {
    if ((matrix->ctrl_bits[id] & (LV_BUTTONMATRIX_CTRL_HIDDEN |
                                  LV_BUTTONMATRIX_CTRL_DISABLED)) != 0)
      continue;
    lv_area_t area = matrix->button_areas[id];
    area.x1 += object.x1 - (area.x1 <= left ? std::min(left, extra)
                                                : column_gap);
    area.y1 += object.y1 - (area.y1 <= top ? std::min(top, extra)
                                              : row_gap);
    area.x2 += object.x1 +
        (area.x2 >= width - right - 2 ? std::min(right, extra)
                                      : column_gap);
    area.y2 += object.y1 +
        (area.y2 >= height - bottom - 2 ? std::min(bottom, extra)
                                        : row_gap);
    if (point.x >= area.x1 && point.x <= area.x2 &&
        point.y >= area.y1 && point.y <= area.y2)
      return id;
  }
  return LV_BUTTONMATRIX_BUTTON_NONE;
}

inline bool HandleSecondaryPointer(int x, int y, bool pressed) {
  if (pressed && !gSecondary.pressed) {
    auto *keyboard = VisibleKeyboard();
    const uint32_t button = ButtonAt(keyboard, x, y);
    if (button == LV_BUTTONMATRIX_BUTTON_NONE) return false;
    gSecondary = {keyboard, button, true};
    lv_obj_invalidate(keyboard);
    return true;
  }
  if (!gSecondary.pressed) return false;

  auto *keyboard = gSecondary.keyboard;
  const uint32_t original = gSecondary.button;
  if (keyboard == nullptr || !lv_obj_is_valid(keyboard)) {
    gSecondary = {};
    return true;
  }
  // Dragging away cancels this key, matching normal AERA button behavior.
  const uint32_t current = ButtonAt(keyboard, x, y);
  gSecondary.button = current == original ? original
                                          : LV_BUTTONMATRIX_BUTTON_NONE;
  lv_obj_invalidate(keyboard);
  if (pressed) return true;

  const bool activate = gSecondary.button != LV_BUTTONMATRIX_BUTTON_NONE;
  gSecondary = {};
  if (activate) {
    const uint32_t primary = lv_buttonmatrix_get_selected_button(keyboard);
    lv_buttonmatrix_set_selected_button(keyboard, original);
    uint32_t dispatched = original;
    lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, &dispatched);
    if (lv_obj_is_valid(keyboard))
      lv_buttonmatrix_set_selected_button(keyboard, primary);
  }
  if (lv_obj_is_valid(keyboard)) lv_obj_invalidate(keyboard);
  return true;
}

constexpr lv_buttonmatrix_ctrl_t Key(unsigned width = 1,
                                     unsigned flags = 0) {
  return static_cast<lv_buttonmatrix_ctrl_t>((width & 0x0fU) | flags);
}

constexpr unsigned kLetter = LV_BUTTONMATRIX_CTRL_POPOVER;
constexpr unsigned kUtility = LV_BUTTONMATRIX_CTRL_NO_REPEAT;

inline constexpr const char *kLower[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "?123", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kUpper[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "?123", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kLowerQwertz[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    LV_SYMBOL_UP, "y", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "?123", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kUpperQwertz[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    LV_SYMBOL_UP, "Y", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "?123", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kSpecial[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "@", "#", "$", "_", "&", "-", "+", "(", ")", "/", "\n",
    "=\\<", "*", "\"", "'", ":", ";", "!", "?", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", ",", " ", ".", LV_SYMBOL_OK, ""};

inline constexpr const char *kSpecialExtra[] = {
    "~", "`", "|", "\xe2\x80\xa2", "\xe2\x88\x9a", "\xcf\x80",
    "\xc3\xb7", "\xc3\x97", "\xc2\xa7", "\xce\x94", "\n",
    "\xc2\xa3", "\xc2\xa2", "\xe2\x82\xac", "\xc2\xa5", "^",
    "\xc2\xb0", "=", "{", "}", "\\", "\n",
    "?123", "%", "\xc2\xa9", "\xc2\xae", "\xe2\x84\xa2",
    "\xe2\x9c\x93", "[", "]", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "<", " ", ">", LV_SYMBOL_OK, ""};

inline constexpr lv_buttonmatrix_ctrl_t kTextControls[] = {
    Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(),
    Key(1, kLetter), Key(1, kLetter), Key(1, kLetter), Key(1, kLetter),
    Key(1, kLetter), Key(1, kLetter), Key(1, kLetter), Key(1, kLetter),
    Key(1, kLetter), Key(1, kLetter),
    Key(1, LV_BUTTONMATRIX_CTRL_HIDDEN),
    Key(2, kLetter), Key(2, kLetter), Key(2, kLetter), Key(2, kLetter),
    Key(2, kLetter), Key(2, kLetter), Key(2, kLetter), Key(2, kLetter),
    Key(2, kLetter), Key(1, LV_BUTTONMATRIX_CTRL_HIDDEN),
    Key(2, kUtility), Key(1, kLetter), Key(1, kLetter), Key(1, kLetter),
    Key(1, kLetter), Key(1, kLetter), Key(1, kLetter), Key(1, kLetter),
    Key(2),
    Key(2, kUtility), Key(1), Key(8), Key(1), Key(2, kUtility)};

inline constexpr lv_buttonmatrix_ctrl_t kSpecialControls[] = {
    Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(),
    Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(), Key(),
    Key(2, kUtility), Key(), Key(), Key(), Key(), Key(), Key(), Key(),
    Key(2),
    Key(2, kUtility), Key(1), Key(8), Key(1), Key(2, kUtility)};

inline bool SameKey(const char *key, const char *expected) {
  return key != nullptr && std::strcmp(key, expected) == 0;
}

enum class ShiftMode { kOff, kOnce, kLocked };

struct ShiftState {
  ShiftMode mode = ShiftMode::kOff;
  uint32_t last_tap = 0;
  bool double_tap_pending = false;
  const char *activated_key = nullptr;
};

inline constexpr uint32_t kShiftDoubleTapMs = 350;

inline void HandleKey(lv_event_t *event) {
  auto *keyboard = lv_event_get_current_target_obj(event);
  auto *shift = static_cast<ShiftState *>(lv_event_get_user_data(event));
  const char *key = lv_keyboard_get_button_text(
      keyboard, lv_keyboard_get_selected_button(keyboard));
  if (key == nullptr) return;
  shift->activated_key = key;
  RecoveryVibrate(Haptic::kKeyboard);

  const auto mode = lv_keyboard_get_mode(keyboard);
  if (mode == LV_KEYBOARD_MODE_TEXT_LOWER) shift->mode = ShiftMode::kOff;

  if (SameKey(key, LV_SYMBOL_UP)) {
    const bool double_tap = shift->double_tap_pending &&
        lv_tick_elaps(shift->last_tap) <= kShiftDoubleTapMs;
    if (shift->mode == ShiftMode::kOff) {
      shift->mode = ShiftMode::kOnce;
      shift->last_tap = lv_tick_get();
      shift->double_tap_pending = true;
    } else {
      shift->mode = shift->mode == ShiftMode::kOnce && double_tap
                        ? ShiftMode::kLocked : ShiftMode::kOff;
      shift->double_tap_pending = false;
    }
    lv_keyboard_set_mode(keyboard, shift->mode == ShiftMode::kOff
        ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_invalidate(keyboard);
    return;
  }

  shift->double_tap_pending = false;
  if (SameKey(key, "=\\<")) {
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    return;
  }
  if (SameKey(key, "ABC")) {
    lv_keyboard_set_mode(keyboard, shift->mode == ShiftMode::kOff
        ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
    return;
  }

  const bool consume_shift = shift->mode == ShiftMode::kOnce &&
      mode == LV_KEYBOARD_MODE_TEXT_UPPER && key[0] >= 'A' &&
      key[0] <= 'Z' && key[1] == '\0';
  if (consume_shift) shift->mode = ShiftMode::kOff;

  // Keep LVGL's editing/Ready behavior, then reset after inserting the letter.
  // Ready or textarea callbacks can delete this keyboard and its ShiftState.
  lv_keyboard_def_event_cb(event);
  if (consume_shift && lv_obj_is_valid(keyboard) &&
      lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_UPPER)
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
}

// Scene callbacks need the original key, not the lower-case map after Shift.
inline const char *ActivatedKey(lv_obj_t *keyboard) {
  for (uint32_t i = 0; i < lv_obj_get_event_count(keyboard); ++i) {
    auto *descriptor = lv_obj_get_event_dsc(keyboard, i);
    if (lv_event_dsc_get_cb(descriptor) == HandleKey) {
      const auto *shift = static_cast<const ShiftState *>(
          lv_event_dsc_get_user_data(descriptor));
      return shift->activated_key;
    }
  }
  return nullptr;
}

template <size_t N>
inline std::array<const char *, N> MultilineMap(const char *const (&source)[N]) {
  std::array<const char *, N> result{};
  for (size_t index = 0; index < N; ++index)
    result[index] = SameKey(source[index], LV_SYMBOL_OK)
                        ? LV_SYMBOL_NEW_LINE
                        : source[index];
  return result;
}

inline const auto kLowerMultiline = MultilineMap(kLower);
inline const auto kUpperMultiline = MultilineMap(kUpper);
inline const auto kLowerQwertzMultiline = MultilineMap(kLowerQwertz);
inline const auto kUpperQwertzMultiline = MultilineMap(kUpperQwertz);
inline const auto kSpecialMultiline = MultilineMap(kSpecial);
inline const auto kSpecialExtraMultiline = MultilineMap(kSpecialExtra);

inline bool IsActionKey(const char *key) {
  return SameKey(key, LV_SYMBOL_OK) || SameKey(key, LV_SYMBOL_NEW_LINE);
}

inline bool IsModifierKey(const char *key) {
  return SameKey(key, LV_SYMBOL_UP) || SameKey(key, LV_SYMBOL_BACKSPACE) ||
         SameKey(key, "?123") || SameKey(key, "=\\<") || SameKey(key, "ABC");
}

inline uint32_t KeyCodepoint(const char *key) {
  if (key == nullptr || IsModifierKey(key) || IsActionKey(key)) return 0;
  uint32_t index = 0;
  const uint32_t codepoint = lv_text_encoded_next(key, &index);
  return key[index] == '\0' ? codepoint : 0;
}

inline const char *KeyLabel(const char *key) {
  // Reuse the built-in checkmark glyph; typing still inserts Unicode U+2713.
  return SameKey(key, "\xe2\x9c\x93") ? LV_SYMBOL_OK : key;
}

inline void DrawKey(lv_event_t *event) {
  auto *keyboard = lv_event_get_target_obj(event);
  auto *task = lv_event_get_draw_task(event);
  if (keyboard == nullptr || task == nullptr) return;
  auto *base = static_cast<lv_draw_dsc_base_t *>(
      lv_draw_task_get_draw_dsc(task));
  if (base == nullptr || base->part != LV_PART_ITEMS) return;

  const char *key = lv_buttonmatrix_get_button_text(keyboard, base->id1);
  if (key == nullptr) return;
  const bool pressed =
      (lv_keyboard_get_selected_button(keyboard) == base->id1 &&
       lv_obj_has_state(keyboard, LV_STATE_PRESSED)) ||
      (gSecondary.pressed && gSecondary.keyboard == keyboard &&
       gSecondary.button == base->id1);
  const bool action = IsActionKey(key);
  const bool modifier = IsModifierKey(key);
  const bool shifted = SameKey(key, LV_SYMBOL_UP) &&
      lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_UPPER;
  const auto *shift = static_cast<const ShiftState *>(
      lv_event_get_user_data(event));

  if (auto *fill = lv_draw_task_get_fill_dsc(task)) {
    fill->color = action || shifted
                      ? (pressed ? design::kAccentPressed : design::kAccent)
                  : modifier ? (pressed ? design::kMainSelected
                                        : design::kAccentSoft)
                  : pressed ? design::kMainSelected : design::kMainPanel;
  }
  if (auto *label = lv_draw_task_get_label_dsc(task)) {
    if (SameKey(key, "\xe2\x9c\x93")) {
      if (label->text_local) lv_free(const_cast<char *>(label->text));
      label->text = KeyLabel(key);
      label->text_local = false;
      label->text_static = true;
    }
    label->color = action || shifted ? design::kOnAccent
                   : modifier ? design::kAccent : design::kText;
    if (shifted && shift != nullptr && shift->mode == ShiftMode::kLocked)
      label->decor = LV_TEXT_DECOR_UNDERLINE;
  }
}

inline void Style(lv_obj_t *keyboard, ShiftState *shift) {
  lv_obj_set_style_bg_color(keyboard, design::kMainBottom, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(keyboard, 30, LV_PART_MAIN);
  lv_obj_set_style_pad_all(keyboard, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_row(keyboard, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_column(keyboard, 10, LV_PART_MAIN);

  lv_obj_set_style_text_font(
      keyboard, design::UiFont(&lv_font_montserrat_48), LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, design::kText, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, design::kMainPanel, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, design::kMainSelected,
                            LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_radius(keyboard, 24, LV_PART_ITEMS);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);

  lv_obj_add_event_cb(keyboard, DrawKey, LV_EVENT_DRAW_TASK_ADDED, shift);
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
}

inline void Apply(lv_obj_t *keyboard, bool multiline = false) {
  auto *shift = new ShiftState;
  gKeyboards.push_back(keyboard);
  lv_obj_add_event_cb(keyboard, [](lv_event_t *event) {
    auto *deleted = lv_event_get_target_obj(event);
    gKeyboards.erase(std::remove(gKeyboards.begin(), gKeyboards.end(), deleted),
                     gKeyboards.end());
    if (gSecondary.keyboard == deleted) gSecondary = {};
    delete static_cast<ShiftState *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, shift);
  const bool qwertz = RecoveryKeyboardLayout() == KeyboardLayout::kQwertz;
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER,
                      multiline
                          ? (qwertz ? kLowerQwertzMultiline.data()
                                   : kLowerMultiline.data())
                          : (qwertz ? kLowerQwertz : kLower),
                      kTextControls);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER,
                      multiline
                          ? (qwertz ? kUpperQwertzMultiline.data()
                                   : kUpperMultiline.data())
                          : (qwertz ? kUpperQwertz : kUpper),
                      kTextControls);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL,
                      multiline ? kSpecialMultiline.data() : kSpecial,
                      kSpecialControls);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1,
                      multiline ? kSpecialExtraMultiline.data() : kSpecialExtra,
                      kSpecialControls);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_popovers(keyboard, true);
  Style(keyboard, shift);
  lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
  lv_obj_add_event_cb(keyboard, HandleKey, LV_EVENT_VALUE_CHANGED, shift);
}

}  // namespace aeraui::phone_keyboard
