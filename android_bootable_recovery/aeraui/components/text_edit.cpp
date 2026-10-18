/* SPDX-License-Identifier: Apache-2.0 */
#include "text_edit.hpp"
#include "ui_components.hpp"
#include "src/misc/lv_text_private.h"
#include "src/core/lv_obj_event_private.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace aeraui::text_edit {
namespace {
lv_obj_t *physical_focus = nullptr;
using namespace design;
enum class Action { kCopy, kCut, kPaste, kAll, kClose };
struct State {
  ControlHandler control = nullptr;
  void *control_context = nullptr;
  lv_obj_t *input = nullptr;
  lv_obj_t *root = nullptr;
  lv_obj_t *menu = nullptr;
  lv_obj_t *lens = nullptr;
  lv_obj_t *preview = nullptr;
  lv_obj_t *preview_cursor = nullptr;
  std::array<lv_obj_t *, 2> handles{};
  bool whole_on_hold = false;
  lv_obj_t *dragging = nullptr;
  bool drag_start = false;
  int drag_line_y = 0;
  lv_point_t drag_offset{};
  std::string drag_text;
  std::vector<uint32_t> drag_bytes;
};
struct Control { State *owner; Action action; };
State *active = nullptr;
std::string clipboard;
std::vector<lv_indev_t *> observed;

void InputEvent(lv_event_t *event);
void Position(State *state);
void ShowMenu(State *state);

State *Find(lv_obj_t *input) {
  if (!input || !lv_obj_is_valid(input)) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(input); ++i) {
    auto *descriptor = lv_obj_get_event_dsc(input, i);
    if (lv_event_dsc_get_cb(descriptor) == InputEvent)
      return static_cast<State *>(lv_event_dsc_get_user_data(descriptor));
  }
  return nullptr;
}

bool Range(lv_obj_t *input, uint32_t &start, uint32_t &end) {
  if (!input || lv_textarea_get_password_mode(input)) return false;
  auto *label = lv_textarea_get_label(input);
  start = lv_label_get_text_selection_start(label);
  end = lv_label_get_text_selection_end(label);
  const auto length = lv_text_get_encoded_length(lv_textarea_get_text(input));
  return start != LV_DRAW_LABEL_NO_TXT_SEL && end != LV_DRAW_LABEL_NO_TXT_SEL &&
      start < end && end <= length;
}

void Close(State *state, bool clear = true) {
  if (!state) return;
  if (active == state) active = nullptr;
  if (clear && state->input) lv_textarea_clear_selection(state->input);
  if (state->root) lv_obj_delete(state->root);
}

void HideMenu(State *state) {
  if (state && state->menu) lv_obj_add_flag(state->menu, LV_OBJ_FLAG_HIDDEN);
}

bool Inside(lv_obj_t *object, lv_obj_t *parent) {
  for (; object; object = lv_obj_get_parent(object)) if (object == parent) return true;
  return false;
}

void Observe(lv_indev_t *input) {
  if (!input || std::find(observed.begin(), observed.end(), input) != observed.end()) return;
  observed.push_back(input);
  lv_indev_add_event_cb(input, [](lv_event_t *event) {
    auto *device = static_cast<lv_indev_t *>(lv_event_get_current_target(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
      observed.erase(std::remove(observed.begin(), observed.end(), device), observed.end());
      return;
    }
    if (lv_event_get_code(event) != LV_EVENT_PRESSED || !active) return;
    auto *target = static_cast<lv_obj_t *>(lv_event_get_param(event));
    if (Inside(target, active->root) || Inside(target, active->input)) return;
    if (target && lv_obj_check_type(target, &lv_keyboard_class) &&
        lv_keyboard_get_textarea(target) == active->input) {
      HideMenu(active);
      return;
    }
    Close(active);
  }, LV_EVENT_ALL, nullptr);
}

// LVGL exposes selection in character indices; strings and clipboard use UTF-8 bytes.
size_t ByteAt(const std::string &text, uint32_t character) {
  uint32_t index = 0;
  while (character-- && text[index]) lv_text_encoded_next(text.c_str(), &index);
  return index;
}

bool DeleteRange(lv_obj_t *input, bool notify) {
  uint32_t start, end;
  if (!Range(input, start, end)) return false;
  Close(Find(input));
  // Only plain text reaches this path: the label is the textarea's actual text.
  // Cutting it once avoids one VALUE_CHANGED callback per deleted character.
  lv_label_cut_text(lv_textarea_get_label(input), start, end - start);
  lv_textarea_set_cursor_pos(input, start);
  lv_obj_invalidate(input);
  if (notify) lv_obj_send_event(input, LV_EVENT_VALUE_CHANGED, nullptr);
  return true;
}

void SetRange(State *state, uint32_t start, uint32_t end) {
  if (!state->input || lv_textarea_get_password_mode(state->input)) return;
  const int scroll_x = lv_obj_get_scroll_x(state->input);
  const int scroll_y = lv_obj_get_scroll_y(state->input);
  lv_textarea_set_cursor_pos(state->input, end);
  // Moving the leading grip must not scroll the field back to the trailing grip.
  if (state->dragging && state->drag_start)
    lv_obj_scroll_to(state->input, scroll_x, scroll_y, LV_ANIM_OFF);
  auto *label = lv_textarea_get_label(state->input);
  lv_label_set_text_selection_start(label, start);
  lv_label_set_text_selection_end(label, end);
  lv_obj_set_style_bg_color(label, kAccent, LV_PART_SELECTED);
  lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_SELECTED);
  lv_obj_set_style_text_color(label, kOnAccent, LV_PART_SELECTED);
  Position(state);
}

lv_point_t Endpoint(State *state, uint32_t index) {
  auto *label = lv_textarea_get_label(state->input);
  lv_point_t point{};
  lv_label_get_letter_pos(label, index, &point);
  lv_area_t bounds{};
  lv_obj_get_coords(label, &bounds);
  point.x += bounds.x1;
  point.y += bounds.y1;
  return point;
}

uint32_t Hit(State *state, lv_point_t point) {
  auto *label = lv_textarea_get_label(state->input);
  lv_area_t bounds{};
  lv_obj_get_coords(label, &bounds);
  point.x -= bounds.x1;
  point.y -= bounds.y1;
  return std::min(lv_label_get_letter_on(label, &point, true),
      lv_text_get_encoded_length(lv_textarea_get_text(state->input)));
}

void Magnify(State *state, uint32_t caret, lv_point_t finger) {
  if (!state->root || state->drag_bytes.empty()) return;
  auto *source = lv_textarea_get_label(state->input);
  const auto *font = lv_obj_get_style_text_font(source, LV_PART_MAIN);
  const int line = lv_font_get_line_height(font);
  constexpr int zoom = 384;
  const int screen_width = lv_obj_get_width(lv_obj_get_screen(state->input));
  const int screen_height = lv_obj_get_height(lv_obj_get_screen(state->input));
  const int width = std::min(480, screen_width - 48);
  const int height = std::clamp(line * zoom / 256 + 56, 144, 220);
  if (!state->lens) {
    state->lens = lv_obj_create(state->root);
    lv_obj_remove_style_all(state->lens);
    lv_obj_remove_flag(state->lens, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));
    lv_obj_set_style_bg_color(state->lens, kMainPanel, 0);
    lv_obj_set_style_bg_opa(state->lens, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(state->lens, kAccent, 0);
    lv_obj_set_style_border_width(state->lens, 2, 0);
    lv_obj_set_style_radius(state->lens, 28, 0);
    state->preview = lv_label_create(state->lens);
    lv_obj_set_style_text_font(state->preview, font, 0);
    lv_obj_set_style_text_color(state->preview,
        lv_obj_get_style_text_color(source, LV_PART_MAIN), 0);
    lv_obj_set_style_text_letter_space(state->preview,
        lv_obj_get_style_text_letter_space(source, LV_PART_MAIN), 0);
    lv_obj_set_style_base_dir(state->preview,
        lv_obj_get_style_base_dir(source, LV_PART_MAIN), 0);
    lv_obj_set_style_bg_color(state->preview, kAccent, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(state->preview, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(state->preview, kOnAccent, LV_PART_SELECTED);
    lv_obj_set_style_transform_pivot_x(state->preview, 0, 0);
    lv_obj_set_style_transform_pivot_y(state->preview, 0, 0);
    lv_obj_set_style_transform_scale(state->preview, zoom, 0);
    // Rasterize the small excerpt so software and GPU renderers scale it alike.
    lv_obj_set_style_opa_layered(state->preview, 250, 0);
    state->preview_cursor = lv_obj_create(state->lens);
    lv_obj_remove_style_all(state->preview_cursor);
    lv_obj_remove_flag(state->preview_cursor, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_bg_color(state->preview_cursor, kAccent, 0);
    lv_obj_set_style_bg_opa(state->preview_cursor, LV_OPA_COVER, 0);
  }
  lv_obj_set_size(state->lens, width, height);
  const uint32_t count = state->drag_bytes.size() - 1;
  caret = std::min(caret, count);
  uint32_t first = caret > 16 ? caret - 16 : 0;
  uint32_t last = std::min(caret + 16, count);
  for (uint32_t i = first; i < caret; ++i)
    if (state->drag_text[state->drag_bytes[i]] == '\n') first = i + 1;
  for (uint32_t i = caret; i < last; ++i) {
    if (state->drag_text[state->drag_bytes[i]] == '\n') { last = i; break; }
  }
  const std::string text = state->drag_text.substr(
      state->drag_bytes[first], state->drag_bytes[last] - state->drag_bytes[first]);
  if (text != lv_label_get_text(state->preview)) lv_label_set_text(state->preview, text.c_str());
  uint32_t start, end;
  if (Range(state->input, start, end)) {
    lv_label_set_text_selection_start(state->preview, std::clamp(start, first, last) - first);
    lv_label_set_text_selection_end(state->preview, std::clamp(end, first, last) - first);
  }
  lv_obj_update_layout(state->preview);
  lv_point_t position{};
  lv_label_get_letter_pos(state->preview, caret - first, &position);
  lv_obj_set_pos(state->preview, width / 2 - position.x * zoom / 256,
      (height - line * zoom / 256) / 2);
  lv_obj_set_size(state->preview_cursor, 4, line * zoom / 256 + 8);
  lv_obj_set_pos(state->preview_cursor, width / 2 - 2,
      (height - line * zoom / 256) / 2 - 4);
  int x = std::clamp(finger.x - width / 2, 24, std::max(24, screen_width - width - 24));
  const auto endpoint = Endpoint(state, caret);
  int y = std::max(24, std::min(finger.y - height - 100, endpoint.y - height - 60));
  // At the top edge prefer the side with room before moving below the finger.
  if (y + height > finger.y - 72) {
    if (finger.x + 96 + width <= screen_width - 24) x = finger.x + 96;
    else if (finger.x - 96 - width >= 24) x = finger.x - 96 - width;
    else y = finger.y + 144;
  }
  y = std::clamp(y, 24, std::max(24, screen_height - height - 24));
  lv_obj_set_pos(state->lens, x, y);
  lv_obj_remove_flag(state->lens, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(state->lens);
}

void HandleEvent(lv_event_t *event) {
  auto *state = static_cast<State *>(lv_event_get_user_data(event));
  if (!state->input) return;
  uint32_t start, end;
  if (!Range(state->input, start, end)) return;
  auto *handle = lv_event_get_current_target_obj(event);
  const bool first = handle == state->handles[0];
  if (lv_event_get_code(event) == LV_EVENT_HIT_TEST) {
    auto *hit = lv_event_get_hit_test_info(event);
    auto *other = state->handles[first ? 1 : 0];
    if (!hit || lv_obj_has_flag(other, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t area{};
    lv_obj_get_coords(other, &area);
    if (hit->point->x < area.x1 || hit->point->x > area.x2 ||
        hit->point->y < area.y1 || hit->point->y > area.y2) return;
    lv_area_t here{};
    lv_obj_get_coords(handle, &here);
    const int here_dx = hit->point->x - (here.x1 + here.x2) / 2;
    const int here_dy = hit->point->y - here.y1 - 48;
    const int other_dx = hit->point->x - (area.x1 + area.x2) / 2;
    const int other_dy = hit->point->y - area.y1 - 48;
    hit->res = here_dx * here_dx + here_dy * here_dy <=
        other_dx * other_dx + other_dy * other_dy;
    return;
  }
  auto *device = lv_event_get_indev(event);
  if (!device) return;
  lv_point_t point{};
  lv_indev_get_point(device, &point);
  switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED: {
      const auto caret = Endpoint(state, first ? start : end);
      auto *label = lv_textarea_get_label(state->input);
      lv_area_t bounds{};
      lv_obj_get_coords(label, &bounds);
      state->dragging = handle;
      state->drag_start = first;
      state->drag_line_y = caret.y - bounds.y1;
      state->drag_offset = {point.x - caret.x, point.y - caret.y};
      state->drag_text = lv_textarea_get_text(state->input);
      state->drag_bytes.clear();
      state->drag_bytes.push_back(0);
      uint32_t byte = 0;
      while (state->drag_text[byte]) {
        lv_text_encoded_next(state->drag_text.c_str(), &byte);
        state->drag_bytes.push_back(byte);
      }
      HideMenu(state);
      Magnify(state, first ? start : end, point);
      break;
    }
    case LV_EVENT_PRESSING: {
      if (state->dragging != handle) break;
      const auto finger = point;
      point.x -= state->drag_offset.x;
      point.y -= state->drag_offset.y;
      auto *label = lv_textarea_get_label(state->input);
      lv_area_t bounds{};
      lv_obj_get_coords(label, &bounds);
      const int line = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
      const int step = std::max(1, line + lv_obj_get_style_text_line_space(label, LV_PART_MAIN));
      if (!lv_textarea_get_one_line(state->input) && lv_obj_get_height(label) > line) {
        const int delta = point.y - bounds.y1 - state->drag_line_y;
        const int tolerance = step / 2 + std::min(24, step / 4);
        if (std::abs(delta) > tolerance) {
          const int rows = (delta + (delta > 0 ? step / 2 : -step / 2)) / step;
          state->drag_line_y = std::clamp(state->drag_line_y + rows * step, 0,
              std::max(0, lv_obj_get_height(label) - line));
        }
      }
      point.y = bounds.y1 + state->drag_line_y + line / 2;
      const uint32_t hit = Hit(state, point);
      if (first) start = std::min(hit, end - 1);
      else end = std::max(hit, start + 1);
      SetRange(state, start, end);
      Magnify(state, first ? start : end, finger);
      break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
      state->dragging = nullptr;
      state->drag_text.clear();
      state->drag_bytes.clear();
      if (state->lens) lv_obj_add_flag(state->lens, LV_OBJ_FLAG_HIDDEN);
      ShowMenu(state);
      break;
    default: break;
  }
}

void MakeRoot(State *state) {
  if (active && active != state) Close(active);
  active = state;
  if (state->root) return;
  state->root = lv_obj_create(lv_obj_get_screen(state->input));
  lv_obj_remove_style_all(state->root);
  lv_obj_set_size(state->root, LV_PCT(100), LV_PCT(100));
  lv_obj_remove_flag(state->root, static_cast<lv_obj_flag_t>(
      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_user_data(state->root, &widgets::kPersistentModalMarker);
  lv_obj_add_event_cb(state->root, [](lv_event_t *event) {
    auto *state = static_cast<State *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
      state->root = state->menu = nullptr;
      state->lens = state->preview = state->preview_cursor = nullptr;
      state->dragging = nullptr;
      state->drag_text.clear();
      state->drag_bytes.clear();
      state->handles = {};
      if (active == state) active = nullptr;
    } else if (lv_event_get_code(event) == LV_EVENT_CANCEL) Close(state);
  }, LV_EVENT_ALL, state);
  for (auto &handle : state->handles) {
    handle = lv_obj_create(state->root);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 96, 144);
    lv_obj_add_flag(handle, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_ADV_HITTEST));
    lv_obj_remove_flag(handle, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_CLICK_FOCUSABLE));
    auto *stem = lv_obj_create(handle);
    lv_obj_remove_style_all(stem);
    lv_obj_remove_flag(stem, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(stem, 6, 38);
    lv_obj_set_pos(stem, 45, 0);
    lv_obj_set_style_bg_color(stem, kAccent, 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    auto *knob = lv_obj_create(handle);
    lv_obj_remove_style_all(knob);
    lv_obj_remove_flag(knob, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(knob, 42, 42);
    lv_obj_set_pos(knob, 27, 28);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, kAccent, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(handle, HandleEvent, LV_EVENT_ALL, state);
  }
}

void Position(State *state) {
  if (!state->root || !state->input) return;
  lv_obj_update_layout(state->input);
  uint32_t start, end;
  const bool selected = Range(state->input, start, end);
  lv_area_t field{};
  lv_obj_get_coords(state->input, &field);
  const int line = lv_font_get_line_height(
      lv_obj_get_style_text_font(lv_textarea_get_label(state->input), LV_PART_MAIN));
  lv_area_t screen{};
  lv_obj_get_coords(lv_obj_get_screen(state->input), &screen);
  for (size_t i = 0; i < state->handles.size(); ++i) {
    auto *handle = state->handles[i];
    lv_point_t point{};
    if (selected) point = Endpoint(state, i == 0 ? start : end);
    const bool visible = selected && (handle == state->dragging ||
        (point.x >= field.x1 && point.x <= field.x2 &&
         point.y >= field.y1 - 2 && point.y < field.y2));
    if (!visible) lv_obj_add_flag(handle, LV_OBJ_FLAG_HIDDEN);
    else {
      lv_obj_remove_flag(handle, LV_OBJ_FLAG_HIDDEN);
      const int x = std::clamp(point.x - screen.x1 - 48, 0,
          std::max(0, lv_area_get_width(&screen) - 96));
      const int y = std::clamp(point.y - screen.y1 + line - 6, 0,
          std::max(0, lv_area_get_height(&screen) - 144));
      lv_obj_set_pos(handle, x, y);
    }
  }
  if (state->menu) {
    lv_obj_update_layout(state->menu);
    const int width = lv_obj_get_width(state->menu);
    const int height = lv_obj_get_height(state->menu);
    const auto first = Endpoint(state, selected ? start : lv_textarea_get_cursor_pos(state->input));
    const auto last = selected ? Endpoint(state, end) : first;
    const int top = std::clamp(std::min(first.y, last.y), field.y1, field.y2);
    const int bottom = std::clamp(std::max(first.y, last.y) + line, field.y1, field.y2);
    const int center = first.y == last.y ? (first.x + last.x) / 2 : (field.x1 + field.x2) / 2;
    const int x = std::clamp(center - screen.x1 - width / 2, 24,
        std::max(24, lv_area_get_width(&screen) - width - 24));
    int y = top - screen.y1 - height - 22;
    if (y < 24) y = bottom - screen.y1 + 72;
    y = std::clamp(y, 24, std::max(24, lv_area_get_height(&screen) - height - 24));
    lv_obj_set_pos(state->menu, x, y);
  }
}

void ShowMenu(State *state) {
  MakeRoot(state);
  if (state->menu) lv_obj_delete(state->menu);
  state->menu = lv_obj_create(state->root);
  lv_obj_remove_style_all(state->menu);
  lv_obj_remove_flag(state->menu, static_cast<lv_obj_flag_t>(
      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));
  lv_obj_set_style_bg_color(state->menu, kMainSheet, 0);
  lv_obj_set_style_bg_opa(state->menu, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(state->menu, 24, 0);
  lv_obj_set_style_border_width(state->menu, 2, 0);
  lv_obj_set_style_border_color(state->menu, kMainLine, 0);
  const bool password = lv_textarea_get_password_mode(state->input);
  uint32_t start, end;
  const bool selected = Range(state->input, start, end);
  const bool nonempty = lv_textarea_get_text(state->input)[0] != '\0';
  struct Item { const char *tag; Action action; bool enabled; };
  std::vector<Item> items;
  if (!password) {
    items.push_back({"Copy", Action::kCopy, selected});
    items.push_back({"Cut", Action::kCut, selected});
  }
  items.push_back({"Paste", Action::kPaste, !clipboard.empty()});
  if (!password) items.push_back({"Select all", Action::kAll, nonempty});
  const int available = std::max(180, lv_obj_get_width(lv_obj_get_screen(state->input)) - 48);
  const auto *font = UiFont(&lv_font_montserrat_32);
  std::vector<int> widths;
  int total = 112;
  for (const auto &item : items) {
    lv_point_t text{};
    lv_text_get_size(&text, i18n::Translate(item.tag), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    widths.push_back(std::max(108, text.x + 48));
    total += widths.back() + 8;
  }
  const bool wrapped = total > available;
  const int width = std::min(total, available);
  const int cell = (width - 120) / (items.size() == 1 ? 1 : 2);
  const int line = lv_font_get_line_height(font);
  const int row_height = wrapped ? line * 2 + 36 : line + 48;
  const int rows = wrapped ? static_cast<int>((items.size() + 1) / 2) : 1;
  lv_obj_set_size(state->menu, width, rows * row_height + 24);
  int x = 12;
  for (size_t i = 0; i <= items.size(); ++i) {
    const bool close = i == items.size();
    auto *button = lv_button_create(state->menu);
    lv_obj_remove_style_all(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));
    const int button_width = close ? 80 : wrapped ? cell : widths[i];
    const int button_x = close ? width - 92 : wrapped ? 12 + (i % 2) * (cell + 8) : x;
    const int button_y = close ? 12 : 12 + (wrapped ? i / 2 : 0) * row_height;
    lv_obj_set_pos(button, button_x, button_y);
    lv_obj_set_size(button, button_width, row_height);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, kAccentSoft, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    auto *label = lv_label_create(button);
    if (close) lv_label_set_text(label, LV_SYMBOL_CLOSE);
    else i18n::BindLabel(label, items[i].tag);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, !close && !items[i].enabled ? kDim : kText, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, button_width - 20);
    lv_obj_center(label);
    if (!close && !items[i].enabled) lv_obj_add_state(button, LV_STATE_DISABLED);
    auto *control = new Control{state, close ? Action::kClose : items[i].action};
    lv_obj_add_event_cb(button, [](lv_event_t *event) {
      auto *control = static_cast<Control *>(lv_event_get_user_data(event));
      if (lv_event_get_code(event) == LV_EVENT_DELETE) { delete control; return; }
      if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      const Action action = control->action;
      State *state = control->owner;
      lv_obj_t *input = state->input;
      RecoveryVibrate(Haptic::kTouch);
      switch (action) {
        case Action::kCopy: Copy(input); break;
        case Action::kCut: Cut(input); break;
        case Action::kPaste: Paste(input); break;
        case Action::kAll: SelectAll(input); break;
        case Action::kClose: Close(state); break;
      }
    }, LV_EVENT_ALL, control);
    if (!close) x += button_width + 8;
  }
  Position(state);
}

bool WordCharacter(uint32_t character) {
  if (character >= 128) {
    return character != 0xa0 && character != 0x3000 &&
        !(character >= 0x2000 && character <= 0x206f) &&
        !(character >= 0x3001 && character <= 0x303f);
  }
  return (character >= 'a' && character <= 'z') ||
      (character >= 'A' && character <= 'Z') ||
      (character >= '0' && character <= '9') || character == '_';
}

bool Accepts(lv_obj_t *input, const char *text) {
  const auto character = lv_text_encoded_next(text, nullptr);
  const char *accepted = lv_textarea_get_accepted_chars(input);
  if (character == '\n' && lv_textarea_get_one_line(input)) return false;
  if (!accepted) return true;
  uint32_t index = 0;
  while (accepted[index])
    if (lv_text_encoded_next(accepted, &index) == character) return true;
  return false;
}

void InputEvent(lv_event_t *event) {
  auto *state = static_cast<State *>(lv_event_get_user_data(event));
  auto *input = lv_event_get_current_target_obj(event);
  switch (lv_event_get_code(event)) {
    case LV_EVENT_DELETE:
      if (physical_focus == input) physical_focus = nullptr;
      state->input = nullptr;
      Close(state, false);
      delete state;
      break;
    case LV_EVENT_PRESSED:
      physical_focus = input;
      if (active) Close(active);
      break;
    case LV_EVENT_FOCUSED:
      physical_focus = input;
      break;
    case LV_EVENT_LONG_PRESSED:
      Observe(lv_event_get_indev(event));
      SelectWord(input);
      break;
    case LV_EVENT_INSERT: {
      const char *text = static_cast<const char *>(lv_event_get_param(event));
      uint32_t start, end;
      if (text && Range(input, start, end)) {
        const bool erase = text[0] == LV_KEY_DEL && text[1] == '\0';
        if (!erase && !Accepts(input, text)) return;
        if (erase) lv_textarea_set_insert_replace(input, "");
        DeleteRange(input, erase);
      } else if (active == state) Close(state);
      break;
    }
    case LV_EVENT_VALUE_CHANGED:
    case LV_EVENT_DEFOCUSED:
      if (active == state) Close(state);
      break;
    case LV_EVENT_SCROLL:
    case LV_EVENT_SIZE_CHANGED:
      if (active == state) Position(state);
      break;
    default: break;
  }
}
}  // namespace

void Attach(lv_obj_t *input) {
  if (Find(input)) return;
  auto *state = new State;
  state->input = input;
  // Scroll normally; selecting starts only after a deliberate long press.
  lv_textarea_set_text_selection(input, false);
  lv_obj_add_event_cb(input, InputEvent, LV_EVENT_ALL, state);
  for (auto *device = lv_indev_get_next(nullptr); device; device = lv_indev_get_next(device))
    Observe(device);
}

lv_obj_t *FocusedInput() {
  return physical_focus && lv_obj_is_visible(physical_focus) &&
      lv_obj_get_screen(physical_focus) == lv_screen_active() &&
      !lv_obj_has_state(physical_focus, LV_STATE_DISABLED) ? physical_focus : nullptr;
}

void SetControlHandler(lv_obj_t *input, ControlHandler handler, void *context) {
  if (auto *state = Find(input)) { state->control = handler; state->control_context = context; }
}
void SendControl(lv_obj_t *input, unsigned key) {
  if (auto *state = Find(input); state && state->control)
    state->control(input, key, state->control_context);
}

void SetSelectAllOnHold(lv_obj_t *input, bool enabled) {
  if (auto *state = Find(input)) state->whole_on_hold = enabled;
}

void SelectWord(lv_obj_t *input) {
  auto *state = Find(input);
  if (!state) return;
  if (state->whole_on_hold && !lv_textarea_get_password_mode(input)) { SelectAll(input); return; }
  if (!lv_textarea_get_password_mode(input)) {
    std::vector<uint32_t> characters;
    const char *text = lv_textarea_get_text(input);
    uint32_t index = 0;
    while (text[index]) characters.push_back(lv_text_encoded_next(text, &index));
    if (!characters.empty()) {
      uint32_t start = std::min(lv_textarea_get_cursor_pos(input),
          static_cast<uint32_t>(characters.size() - 1));
      uint32_t end = start + 1;
      if (WordCharacter(characters[start])) {
        while (start > 0 && WordCharacter(characters[start - 1])) --start;
        while (end < characters.size() && WordCharacter(characters[end])) ++end;
      }
      SetRange(state, start, end);
    }
  }
  ShowMenu(state);
}

void SelectAll(lv_obj_t *input) {
  auto *state = Find(input);
  if (!state || lv_textarea_get_password_mode(input)) return;
  SetRange(state, 0, lv_text_get_encoded_length(lv_textarea_get_text(input)));
  ShowMenu(state);
}

bool Copy(lv_obj_t *input) {
  uint32_t start, end;
  if (!Range(input, start, end)) return false;
  const std::string text = lv_textarea_get_text(input);
  const size_t first = ByteAt(text, start);
  clipboard = text.substr(first, ByteAt(text, end) - first);
  Close(Find(input));
  return true;
}

bool Cut(lv_obj_t *input) {
  uint32_t start, end;
  if (!Range(input, start, end)) return false;
  const std::string text = lv_textarea_get_text(input);
  const size_t first = ByteAt(text, start);
  clipboard = text.substr(first, ByteAt(text, end) - first);
  return DeleteRange(input, true);
}

bool Paste(lv_obj_t *input) {
  if (!Find(input) || clipboard.empty()) return false;
  const std::string text = clipboard;
  HideMenu(Find(input));
  lv_textarea_add_text(input, text.c_str());
  return true;
}

bool Dismiss(lv_obj_t *screen) {
  if (!active || !active->input || lv_obj_get_screen(active->input) != screen) return false;
  Close(active);
  return true;
}
}  // namespace aeraui::text_edit
