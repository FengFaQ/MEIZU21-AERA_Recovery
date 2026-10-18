/* SPDX-License-Identifier: Apache-2.0 */
#include "ui_components.hpp"
#include "phone_keyboard.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <png.h>

using namespace aeraui;
namespace aeraui {
void RecoveryVibrate(Haptic) {}
KeyboardLayout RecoveryKeyboardLayout() { return KeyboardLayout::kQwerty; }
}  // namespace aeraui
namespace {
lv_point_t pointer{};
bool down = false;
lv_indev_t *device = nullptr;

void Contact(lv_point_t point, bool pressed, int milliseconds = 20) {
  pointer = point;
  down = pressed;
  lv_tick_inc(milliseconds);
  lv_indev_read(device);
  lv_timer_handler();
}

lv_obj_t *MakeInput(int y) {
  auto *input = widgets::TextArea(lv_screen_active());
  lv_obj_set_pos(input, 64, y);
  lv_obj_set_size(input, 1312, 180);
  lv_obj_set_style_text_font(input, design::UiFont(&lv_font_montserrat_40), 0);
  lv_obj_set_style_text_color(input, design::kText, 0);
  lv_obj_set_style_bg_color(input, design::kMainPanel, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(input, 24, 0);
  return input;
}

lv_obj_t *Root() {
  auto *screen = lv_screen_active();
  for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
    auto *child = lv_obj_get_child(screen, i);
    if (lv_obj_get_user_data(child) == &widgets::kPersistentModalMarker) return child;
  }
  return nullptr;
}

lv_obj_t *Button(lv_obj_t *root, const char *text) {
  if (!root) return nullptr;
  if (lv_obj_check_type(root, &lv_label_class) &&
      !std::strcmp(lv_label_get_text(root), text)) return lv_obj_get_parent(root);
  for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
    if (auto *found = Button(lv_obj_get_child(root, i), text)) return found;
  return nullptr;
}

lv_obj_t *Lens() {
  auto *root = Root();
  if (!root) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
    auto *child = lv_obj_get_child(root, i);
    if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) &&
        lv_obj_get_child_count(child) == 2 &&
        lv_obj_check_type(lv_obj_get_child(child, 0), &lv_label_class)) return child;
  }
  return nullptr;
}

std::string Text(lv_obj_t *input) { return lv_textarea_get_text(input); }

void Word(lv_obj_t *input, const char *text, uint32_t cursor,
          uint32_t start, uint32_t end) {
  lv_textarea_set_text(input, text);
  lv_textarea_set_cursor_pos(input, cursor);
  text_edit::SelectWord(input);
  auto *label = lv_textarea_get_label(input);
  assert(lv_label_get_text_selection_start(label) == start);
  assert(lv_label_get_text_selection_end(label) == end);
}

lv_point_t Letter(lv_obj_t *input, uint32_t index) {
  lv_obj_update_layout(input);
  auto *label = lv_textarea_get_label(input);
  lv_point_t point{};
  lv_label_get_letter_pos(label, index, &point);
  lv_area_t bounds{};
  lv_obj_get_coords(label, &bounds);
  point.x += bounds.x1;
  point.y += bounds.y1;
  return point;
}

void Contained(lv_obj_t *child, lv_obj_t *parent) {
  lv_area_t a{}, b{};
  lv_obj_get_coords(child, &a);
  lv_obj_get_coords(parent, &b);
  assert(a.x1 >= b.x1 && a.x2 <= b.x2);
  assert(a.y1 >= b.y1 && a.y2 <= b.y2);
}

void Image(const char *prefix, const std::vector<uint8_t> &pixels,
           int width, int height, int size, const char *language) {
  for (int i = 0; i < 3; ++i) { lv_tick_inc(20); lv_timer_handler(); }
  auto *root = Root();
  assert(root);
  auto *menu = lv_obj_get_parent(Button(root, i18n::Translate("Copy")));
  Contained(menu, lv_screen_active());
  for (uint32_t i = 0; i < lv_obj_get_child_count(menu); ++i) {
    auto *button = lv_obj_get_child(menu, i);
    Contained(button, menu);
    Contained(lv_obj_get_child(button, 0), button);
  }
  if (!prefix) return;
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = width;
  image.height = height;
  image.format = PNG_FORMAT_BGRA;
  const std::string path = std::string(prefix) + "-" + language + "-" +
      std::to_string(width) + "-size" + std::to_string(size) + ".png";
  assert(png_image_write_to_file(&image, path.c_str(), 0, pixels.data(), 0, nullptr));
}
}  // namespace

int main(int argc, char **argv) {
  lv_init();
  auto *display = lv_display_create(1440, 3168);
  std::vector<uint8_t> pixels(1440 * 3168 * 4);
  lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size(),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
  });
  device = lv_indev_create();
  lv_indev_set_type(device, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(device, [](lv_indev_t *, lv_indev_data_t *data) {
    data->point = pointer;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  });
  i18n::Initialize("en");
  lv_obj_set_style_bg_color(lv_screen_active(), design::kMainCanvas, 0);
  auto *input = MakeInput(500);
  auto *other = MakeInput(900);

  Word(input, "hello world", 8, 6, 11);
  assert(text_edit::Copy(input));
  assert(!Root());
  lv_textarea_set_text(other, "A");
  assert(text_edit::Paste(other));
  assert(Text(other) == "Aworld");
  Word(input, "hello world", 8, 6, 11);
  lv_textarea_set_max_length(input, 32);
  lv_textarea_add_text(input, "a");
  assert(Text(input) == "hello a");
  Word(input, "one two", 5, 4, 7);
  lv_textarea_delete_char(input);
  assert(Text(input) == "one ");
  assert(!Root());

  Word(input, "A Gr\xc3\xbc\xc3\x9f \xce\xa9\xe4\xb8\xad Z", 4, 2, 6);
  assert(text_edit::Cut(input));
  assert(Text(input) == "A  \xce\xa9\xe4\xb8\xad Z");
  lv_textarea_set_text(other, "");
  assert(text_edit::Paste(other));
  assert(Text(other) == "Gr\xc3\xbc\xc3\x9f");
  text_edit::SelectAll(other);
  lv_textarea_add_text(other, "new");
  assert(Text(other) == "new");

  lv_textarea_set_text(input, "1234");
  lv_textarea_set_accepted_chars(input, "0123456789");
  text_edit::SelectAll(input);
  lv_textarea_add_text(input, "x");
  assert(Text(input) == "1234");
  assert(lv_textarea_text_is_selected(input));
  lv_textarea_add_text(input, "9");
  assert(Text(input) == "9");
  lv_textarea_set_accepted_chars(input, nullptr);
  lv_textarea_set_max_length(input, 0);

  lv_textarea_set_text(input, "https://example.org/a?q=123");
  text_edit::SetSelectAllOnHold(input, true);
  text_edit::SelectWord(input);
  assert(lv_label_get_text_selection_start(lv_textarea_get_label(input)) == 0);
  assert(lv_label_get_text_selection_end(lv_textarea_get_label(input)) == Text(input).size());
  assert(text_edit::Copy(input));
  lv_textarea_set_password_mode(other, true);
  lv_textarea_set_text(other, "secret");
  text_edit::SelectWord(other);
  assert(!text_edit::Copy(other) && !text_edit::Cut(other));
  assert(!Button(Root(), "Copy") && !Button(Root(), "Cut"));
  assert(Button(Root(), "Paste"));
  assert(text_edit::Paste(other));
  assert(Text(other) == "secrethttps://example.org/a?q=123");
  lv_textarea_set_password_mode(other, false);
  text_edit::SetSelectAllOnHold(input, false);

  // Real long-press and handle dragging, through LVGL's pointer input path.
  lv_textarea_set_text(input, "hello world");
  auto point = Letter(input, 1);
  point.x += 8; point.y += 16;
  Contact(point, true);
  for (int i = 0; i < 12; ++i) Contact(point, true, 50);
  Contact(point, false);
  assert(Root());
  assert(lv_label_get_text_selection_start(lv_textarea_get_label(input)) == 0);
  assert(lv_label_get_text_selection_end(lv_textarea_get_label(input)) == 5);
  auto *end_handle = lv_obj_get_child(Root(), 1);
  lv_area_t handle{};
  lv_obj_get_coords(end_handle, &handle);
  const lv_point_t grab{(handle.x1 + handle.x2) / 2, handle.y1 + 46};
  const auto from = Letter(input, 5);
  const auto to = Letter(input, 11);
  Contact(grab, true);
  assert(Lens());
  const lv_point_t slipped{grab.x + to.x - from.x, grab.y + to.y - from.y + 100};
  Contact(slipped, true);
  assert(Lens());
  Contained(Lens(), lv_screen_active());
  lv_area_t lens_bounds{};
  lv_obj_get_coords(Lens(), &lens_bounds);
  assert(lens_bounds.y2 < slipped.y);
  if (argc > 1) {
    const std::string prefix = std::string(argv[1]) + "-magnifier";
    Image(prefix.c_str(), pixels, 1440, 3168, 1, "en");
  }
  Contact(slipped, false);
  assert(!Lens());
  assert(lv_label_get_text_selection_end(lv_textarea_get_label(input)) == 11);
  auto *copy = Button(Root(), "Copy");
  assert(copy);
  lv_obj_send_event(copy, LV_EVENT_CLICKED, nullptr);
  assert(!Root());
  lv_textarea_set_text(other, "");
  assert(text_edit::Paste(other));
  assert(Text(other) == "hello world");
  text_edit::SelectAll(input);
  Contact({1400, 1800}, true);
  Contact({1400, 1800}, false);
  assert(!Root());

  Word(input, "alpha beta\ngamma delta\nthird line", 2, 0, 5);
  end_handle = lv_obj_get_child(Root(), 1);
  lv_obj_get_coords(end_handle, &handle);
  const lv_point_t multiline_grab{(handle.x1 + handle.x2) / 2, handle.y1 + 110};
  const auto first_line = Letter(input, 5);
  const auto second_line = Letter(input, 16);
  Contact(multiline_grab, true);
  assert(Lens());
  Contact({multiline_grab.x, multiline_grab.y + 12}, true);
  assert(lv_label_get_text_selection_end(lv_textarea_get_label(input)) == 5);
  const lv_point_t moved{multiline_grab.x + second_line.x - first_line.x,
                         multiline_grab.y + second_line.y - first_line.y};
  Contact(moved, true);
  assert(lv_label_get_text_selection_end(lv_textarea_get_label(input)) == 16);
  Contact(moved, false);
  assert(!Lens());
  text_edit::Dismiss(lv_screen_active());

  // A large editor anchors the menu to the selected line, not its outer box.
  lv_obj_set_height(input, 1500);
  Word(input, "first\nsecond\nthird\nfourth\nfifth target", 34, 32, 38);
  auto *menu = lv_obj_get_parent(Button(Root(), "Copy"));
  lv_obj_update_layout(menu);
  lv_area_t menu_bounds{};
  lv_obj_get_coords(menu, &menu_bounds);
  const auto selected_line = Letter(input, 32);
  assert(menu_bounds.y1 > lv_obj_get_y(input));
  assert(menu_bounds.y2 < selected_line.y);
  text_edit::Dismiss(lv_screen_active());
  lv_obj_set_height(input, 180);

  // Browser address fields are near the top: the lens must stay above the grip.
  lv_obj_set_y(input, 188);
  lv_textarea_set_one_line(input, true);
  Word(input, "https://example.org", 2, 0, 5);
  end_handle = lv_obj_get_child(Root(), 1);
  lv_obj_get_coords(end_handle, &handle);
  const lv_point_t browser_grab{(handle.x1 + handle.x2) / 2, handle.y1 + 46};
  Contact(browser_grab, true);
  assert(Lens());
  lv_obj_get_coords(Lens(), &lens_bounds);
  assert(lens_bounds.y2 <= browser_grab.y - 72);
  Contained(Lens(), lv_screen_active());
  Contact(browser_grab, false);
  text_edit::Dismiss(lv_screen_active());
  lv_obj_set_y(input, 500);
  lv_textarea_set_one_line(input, false);

  Word(input, "hello world", 8, 6, 11);
  auto *keyboard = lv_keyboard_create(lv_screen_active());
  phone_keyboard::Apply(keyboard);
  lv_keyboard_set_textarea(keyboard, input);
  uint32_t key = 0;
  while (std::strcmp(lv_keyboard_get_button_text(keyboard, key), "x")) ++key;
  lv_buttonmatrix_set_selected_button(keyboard, key);
  lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, &key);
  assert(Text(input) == "hello x");
  assert(!Root());
  text_edit::SelectAll(input);
  key = 0;
  while (std::strcmp(lv_keyboard_get_button_text(keyboard, key), LV_SYMBOL_BACKSPACE)) ++key;
  lv_buttonmatrix_set_selected_button(keyboard, key);
  lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, &key);
  assert(Text(input).empty());
  assert(!Root());
  lv_obj_delete(keyboard);

  for (bool landscape : {false, true}) {
    const int width = landscape ? 3168 : 1440;
    const int height = landscape ? 1440 : 3168;
    lv_display_set_resolution(display, width, height);
    for (const char *language : {"en", "de_DE"}) {
      i18n::SetLanguage(language);
      for (int size : {0, 1, 2, 3}) {
        design::ApplyInterfaceSize(size);
        lv_obj_set_style_text_font(input, design::UiFont(&lv_font_montserrat_40), 0);
        Word(input, "hello world", 8, 6, 11);
        Image(argc > 1 ? argv[1] : nullptr, pixels, width, height, size, language);
        assert(text_edit::Dismiss(lv_screen_active()));
      }
    }
  }
  i18n::SetLanguage("en");
  auto *doomed = MakeInput(700);
  lv_textarea_set_text(doomed, "delete me");
  text_edit::SelectAll(doomed);
  lv_obj_add_event_cb(doomed, [](lv_event_t *event) {
    lv_obj_delete(lv_event_get_current_target_obj(event));
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  assert(text_edit::Cut(doomed));
  assert(!Root());
  text_edit::SelectAll(input);
  lv_obj_delete(input);
  assert(!Root());
  lv_obj_delete(other);
  lv_indev_delete(device);
  std::puts("Text selection, UTF-8 clipboard, replacement, handles, password protection and layouts passed.");
  lv_deinit();
}
