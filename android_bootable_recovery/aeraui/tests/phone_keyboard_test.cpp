// SPDX-License-Identifier: Apache-2.0
#include "phone_keyboard.hpp"
#include "src/misc/lv_text_private.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include <png.h>

using namespace aeraui;
namespace {
KeyboardLayout layout = KeyboardLayout::kQwerty;
int vibrations = 0;

uint32_t FindKey(lv_obj_t *keyboard, const char *key) {
  const auto *matrix = reinterpret_cast<lv_buttonmatrix_t *>(keyboard);
  for (uint32_t i = 0; i < matrix->btn_cnt; ++i)
    if (phone_keyboard::SameKey(lv_keyboard_get_button_text(keyboard, i), key) &&
        !lv_buttonmatrix_has_button_ctrl(keyboard, i, LV_BUTTONMATRIX_CTRL_HIDDEN))
      return i;
  std::fprintf(stderr, "Missing keyboard key: %s\n", key);
  assert(false);
  return LV_BUTTONMATRIX_BUTTON_NONE;
}

void Tap(lv_obj_t *keyboard, const char *key) {
  uint32_t id = FindKey(keyboard, key);
  lv_buttonmatrix_set_selected_button(keyboard, id);
  lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, &id);
}

lv_obj_t *Create(lv_obj_t *input, bool multiline = false) {
  auto *keyboard = lv_keyboard_create(lv_screen_active());
  phone_keyboard::Apply(keyboard, multiline);
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard, 48, 210);
  lv_obj_set_size(keyboard, 1344, 760);
  lv_keyboard_set_textarea(keyboard, input);
  return keyboard;
}

void CheckGlyphs(lv_obj_t *keyboard, std::set<char> &ascii) {
  const auto *matrix = reinterpret_cast<lv_buttonmatrix_t *>(keyboard);
  const auto *font = lv_obj_get_style_text_font(keyboard, LV_PART_ITEMS);
  for (uint32_t i = 0; i < matrix->btn_cnt; ++i) {
    const char *key = lv_keyboard_get_button_text(keyboard, i);
    if (phone_keyboard::IsModifierKey(key) || phone_keyboard::IsActionKey(key))
      continue;
    if (key[0] && key[1] == '\0') ascii.insert(key[0]);
    key = phone_keyboard::KeyLabel(key);
    uint32_t index = 0;
    while (key[index]) {
      const uint32_t codepoint = lv_text_encoded_next(key, &index);
      if (codepoint == ' ') continue;
      lv_font_glyph_dsc_t glyph{};
      const bool found = lv_font_get_glyph_dsc(font, &glyph, codepoint, 0);
      if (!found || glyph.is_placeholder)
        std::fprintf(stderr, "Missing keyboard glyph: U+%04X\n", codepoint);
      assert(found && !glyph.is_placeholder);
    }
  }
}

void Render(lv_obj_t *keyboard, const std::vector<uint8_t> &pixels,
            const char *prefix, int size, int page) {
  lv_obj_update_layout(lv_screen_active());
  for (int i = 0; i < 3; ++i) {
    lv_tick_inc(16);
    lv_timer_handler();
  }
  const auto *matrix = reinterpret_cast<lv_buttonmatrix_t *>(keyboard);
  const auto *font = lv_obj_get_style_text_font(keyboard, LV_PART_ITEMS);
  for (uint32_t i = 0; i < matrix->btn_cnt; ++i) {
    if (lv_buttonmatrix_has_button_ctrl(keyboard, i, LV_BUTTONMATRIX_CTRL_HIDDEN))
      continue;
    const char *key = phone_keyboard::KeyLabel(
        lv_keyboard_get_button_text(keyboard, i));
    lv_point_t text{};
    lv_text_get_size(&text, key, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const auto &area = matrix->button_areas[i];
    assert(text.x + 12 <= lv_area_get_width(&area));
    assert(text.y + 12 <= lv_area_get_height(&area));
  }
  if (!prefix) return;
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = 1440;
  image.height = 1000;
  image.format = PNG_FORMAT_BGRA;
  const std::string path = std::string(prefix) + "-size" +
      std::to_string(size) + "-page" + std::to_string(page) + ".png";
  assert(png_image_write_to_file(&image, path.c_str(), 0, pixels.data(), 0, nullptr));
}
}  // namespace

namespace aeraui {
KeyboardLayout RecoveryKeyboardLayout() { return layout; }
void RecoveryVibrate(Haptic) { ++vibrations; }
}  // namespace aeraui

int main(int argc, char **argv) {
  lv_init();
  auto *display = lv_display_create(1440, 1000);
  std::vector<uint8_t> pixels(1440 * 1000 * 4);
  lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size(),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
  });
  auto *input = lv_textarea_create(lv_screen_active());
  lv_obj_set_pos(input, 48, 32);
  lv_obj_set_size(input, 1344, 140);
  auto *keyboard = Create(input);

  Tap(keyboard, LV_SYMBOL_UP);
  Tap(keyboard, "5");
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_UPPER);
  assert(std::string(lv_textarea_get_text(input)) == "5");
  lv_textarea_set_text(input, "");
  Tap(keyboard, "A");
  Tap(keyboard, "b");
  assert(std::string(lv_textarea_get_text(input)) == "Ab");
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);

  Tap(keyboard, LV_SYMBOL_UP);
  lv_tick_inc(100);
  Tap(keyboard, LV_SYMBOL_UP);
  Tap(keyboard, "C");
  Tap(keyboard, "D");
  Tap(keyboard, "?123");
  Tap(keyboard, "=\\<");
  Tap(keyboard, "%");
  Tap(keyboard, "ABC");
  Tap(keyboard, "E");
  assert(std::string(lv_textarea_get_text(input)) == "AbCD%E");
  Tap(keyboard, LV_SYMBOL_UP);
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);

  Tap(keyboard, LV_SYMBOL_UP);
  lv_tick_inc(phone_keyboard::kShiftDoubleTapMs + 1);
  Tap(keyboard, LV_SYMBOL_UP);
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);
  Tap(keyboard, LV_SYMBOL_UP);
  Tap(keyboard, ",");
  Tap(keyboard, LV_SYMBOL_BACKSPACE);
  Tap(keyboard, "?123");
  Tap(keyboard, "=\\<");
  Tap(keyboard, "\\");
  Tap(keyboard, "?123");
  Tap(keyboard, "ABC");
  Tap(keyboard, "F");
  assert(std::string(lv_textarea_get_text(input)) == "AbCD%E\\F");
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);

  // An intervening key must not turn two separate Shift taps into Caps Lock.
  Tap(keyboard, LV_SYMBOL_UP);
  Tap(keyboard, " ");
  Tap(keyboard, LV_SYMBOL_UP);
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
  Tap(keyboard, "7");
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_NUMBER);
  for (auto mode : {LV_KEYBOARD_MODE_TEXT_LOWER, LV_KEYBOARD_MODE_TEXT_UPPER,
                     LV_KEYBOARD_MODE_SPECIAL, LV_KEYBOARD_MODE_USER_1,
                     LV_KEYBOARD_MODE_NUMBER}) {
    lv_keyboard_set_mode(keyboard, mode);
    lv_textarea_set_text(input, "abcdef");
    Tap(keyboard, LV_SYMBOL_BACKSPACE);
    for (int i = 0; i < 3; ++i)
      lv_obj_send_event(keyboard, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    assert(std::string(lv_textarea_get_text(input)) == "ab");
    assert(!lv_buttonmatrix_has_button_ctrl(keyboard,
        FindKey(keyboard, LV_SYMBOL_BACKSPACE), LV_BUTTONMATRIX_CTRL_NO_REPEAT));
  }
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  Tap(keyboard, LV_SYMBOL_UP);
  lv_obj_send_event(keyboard, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
  Tap(keyboard, "A");
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_delete(keyboard);

  layout = KeyboardLayout::kQwertz;
  keyboard = Create(input, true);
  lv_textarea_set_text(input, "");
  Tap(keyboard, LV_SYMBOL_UP);
  Tap(keyboard, "Z");
  Tap(keyboard, "y");
  Tap(keyboard, "?123");
  Tap(keyboard, "=\\<");
  Tap(keyboard, LV_SYMBOL_NEW_LINE);
  assert(std::string(lv_textarea_get_text(input)) == "Zy\n");

  for (int size : {0, 1, 2, 3}) {
    design::ApplyInterfaceSize(size);
    lv_obj_set_style_text_font(keyboard, design::UiFont(&lv_font_montserrat_48),
                               LV_PART_ITEMS);
    std::set<char> ascii;
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    for (const char *digit : {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"})
      FindKey(keyboard, digit);
    Render(keyboard, pixels, argc > 1 ? argv[1] : nullptr, size, 0);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
    CheckGlyphs(keyboard, ascii);
    Render(keyboard, pixels, argc > 1 ? argv[1] : nullptr, size, 1);
    Tap(keyboard, "=\\<");
    CheckGlyphs(keyboard, ascii);
    Render(keyboard, pixels, argc > 1 ? argv[1] : nullptr, size, 2);
    const char *punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    for (const char *p = punctuation; *p; ++p) assert(ascii.count(*p));
  }
  Tap(keyboard, "ABC");
  Tap(keyboard, "?123");
  Tap(keyboard, "=\\<");
  lv_textarea_set_text(input, "");
  Tap(keyboard, "\xe2\x9c\x93");
  assert(std::string(lv_textarea_get_text(input)) == "\xe2\x9c\x93");
  lv_textarea_set_text(input, "Zy\n");
  Tap(keyboard, "ABC");
  Tap(keyboard, LV_SYMBOL_UP);
  // Secondary touches must run through the same one-shot handler.
  lv_obj_update_layout(keyboard);
  const auto *matrix = reinterpret_cast<lv_buttonmatrix_t *>(keyboard);
  const auto &area = matrix->button_areas[FindKey(keyboard, "A")];
  const int x = lv_obj_get_x(keyboard) + (area.x1 + area.x2) / 2;
  const int y = lv_obj_get_y(keyboard) + (area.y1 + area.y2) / 2;
  assert(phone_keyboard::HandleSecondaryPointer(x, y, true));
  assert(phone_keyboard::HandleSecondaryPointer(x, y, false));
  assert(std::string(lv_textarea_get_text(input)) == "Zy\nA");
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);

  // Browser page fields have no LVGL textarea and read keys in a later callback.
  std::vector<uint32_t> remote_keys;
  auto *remote = Create(nullptr);
  lv_obj_add_event_cb(remote, [](lv_event_t *e) {
    auto *keys = static_cast<std::vector<uint32_t> *>(lv_event_get_user_data(e));
    const uint32_t codepoint = phone_keyboard::KeyCodepoint(
        phone_keyboard::ActivatedKey(lv_event_get_current_target_obj(e)));
    if (codepoint >= 32) keys->push_back(codepoint);
  }, LV_EVENT_VALUE_CHANGED, &remote_keys);
  Tap(remote, LV_SYMBOL_UP);
  Tap(remote, "A");
  Tap(remote, "b");
  Tap(remote, "?123");
  Tap(remote, "=\\<");
  Tap(remote, "\xe2\x82\xac");
  Tap(remote, "?123");
  assert((remote_keys == std::vector<uint32_t>{'A', 'b', 0x20ac}));
  assert(phone_keyboard::KeyCodepoint("ABC") == 0);
  lv_obj_delete(remote);

  auto *other = Create(input);
  Tap(keyboard, LV_SYMBOL_UP);
  Tap(other, "b");
  assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_UPPER);
  lv_obj_delete(other);
  lv_obj_delete(keyboard);
  keyboard = Create(input);
  lv_textarea_set_one_line(input, true);
  lv_obj_add_event_cb(keyboard, [](lv_event_t *e) {
    lv_obj_delete(lv_event_get_current_target_obj(e));
  }, LV_EVENT_READY, nullptr);
  Tap(keyboard, LV_SYMBOL_OK);
  assert(phone_keyboard::gKeyboards.empty());
  assert(!phone_keyboard::gSecondary.pressed);
  assert(vibrations > 0);
  std::puts("Keyboard Shift, Caps Lock, symbol coverage, glyphs and layouts passed.");
  lv_deinit();
}
