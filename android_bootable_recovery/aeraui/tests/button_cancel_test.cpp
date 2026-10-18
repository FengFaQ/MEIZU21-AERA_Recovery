// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <lvgl.h>

static lv_point_t point;
static bool down;
static int clicks;
static void Read(lv_indev_t *, lv_indev_data_t *data) {
  data->point = point;
  data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void Touch(lv_indev_t *input, int x, int y, bool pressed) {
  point = {x, y};
  down = pressed;
  lv_tick_inc(20);
  lv_indev_read(input);
}
int main() {
  lv_init();
  auto *display = lv_display_create(480, 800);
  auto *input = lv_indev_create();
  lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(input, display);
  lv_indev_set_read_cb(input, Read);
  auto *screen = lv_screen_active();
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  auto *button = lv_button_create(screen);
  lv_obj_set_pos(button, 100, 100);
  lv_obj_set_size(button, 160, 100);
  lv_obj_add_event_cb(button, [](lv_event_t *) { ++clicks; }, LV_EVENT_CLICKED, nullptr);
  lv_obj_update_layout(screen);
  assert(!lv_obj_has_flag(button, LV_OBJ_FLAG_PRESS_LOCK));
  Touch(input, 150, 140, true);
  Touch(input, 150, 140, false);
  assert(clicks == 1);
  for (auto outside : {lv_point_t{50, 140}, lv_point_t{350, 140},
                      lv_point_t{150, 40}, lv_point_t{150, 300}}) {
    Touch(input, 150, 140, true);
    Touch(input, outside.x, outside.y, true);
    assert(!lv_obj_has_state(button, LV_STATE_PRESSED));
    Touch(input, outside.x, outside.y, false);
    assert(clicks == 1);
  }
  Touch(input, 150, 140, true);
  Touch(input, 150, 140, false);
  assert(clicks == 2);
  // Slider dragging still needs capture outside the knob's bounds.
  auto *slider = lv_slider_create(screen);
  assert(lv_obj_has_flag(slider, LV_OBJ_FLAG_PRESS_LOCK));
  lv_deinit();
  puts("Button tap/drag-away cancellation and slider press-lock passed");
}
