// SPDX-License-Identifier: Apache-2.0
#include "ui_components.hpp"
#include "partition_layout.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <png.h>

using namespace aeraui;
static DockLayout layout = DockLayout::kGlass;
static bool long_labels = false;
static int clicks;
static Action last_action;
static lv_point_t touch;
static bool pressed;
namespace aeraui {
DockLayout RecoveryDockLayout() { return layout; }
int RecoveryDockTransparency() { return 60; }
int RecoveryDockBlur() { return 0; }
void RecoveryVibrate(Haptic) {}
}
namespace aeraui::fonts {
const lv_font_t *WithLanguageFallback(const lv_font_t *font) { return font; }
}
namespace aeraui::i18n {
const char *Translate(const char *text) { return text; }
void BindLabel(lv_obj_t *label, const char *text) {
  lv_label_set_text(label, long_labels && !strcmp(text, "Backup")
      ? "Sicherungen" : text);
}
}
static void Click(Action action, void *) { ++clicks; last_action = action; }
static void Read(lv_indev_t *, lv_indev_data_t *data) {
  data->point = touch;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void Input(lv_indev_t *input, int x, int y, bool down) {
  touch = {x, y}; pressed = down;
  lv_tick_inc(20); lv_indev_read(input);
}
static void Settle(lv_obj_t *screen) {
  for (int i = 0; i < 50; ++i) { lv_tick_inc(20); lv_timer_handler(); }
  lv_obj_update_layout(screen);
}
static void Contained(lv_obj_t *child, lv_obj_t *parent) {
  lv_area_t a{}, b{};
  lv_obj_get_coords(child, &a); lv_obj_get_coords(parent, &b);
  assert(a.x1 >= b.x1 && a.x2 <= b.x2);
  assert(a.y1 >= b.y1 && a.y2 <= b.y2);
}
int main(int argc, char **argv) {
  lv_init();
  std::vector<uint8_t> pixels(1440 * 3168 * 4);
  auto *display = lv_display_create(1440, 3168);
  lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size(),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
  });
  auto *input = lv_indev_create();
  lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(input, display);
  lv_indev_set_read_cb(input, Read);
  auto *screen = lv_screen_active();
  widgets::Clear(screen);
  constexpr Action actions[] = {Action::kBackHome, Action::kBackup,
                                 Action::kWipe, Action::kSettings};
  for (bool landscape : {false, true}) {
    lv_display_set_resolution(display, landscape ? 3168 : 1440,
                               landscape ? 1440 : 3168);
    for (bool light : {false, true}) {
      design::ApplySurfaceMode(light);
      design::ApplyAccent(light ? 0x008050 : 0x16c8ff);
      for (int size : {0, 1, 2}) {
        design::ApplyInterfaceSize(size);
        for (int id = 0; id <= kDockLayoutMax; ++id) {
          layout = static_cast<DockLayout>(id);
          // Backup/restore and wipe must reclaim the hidden review action's
          // space, including after deselection or returning to the library.
          for (int top : {1070, 948}) {
            auto *list = widgets::Scroll(screen, landscape ? 350 : top, 600);
            auto *review = widgets::Button(screen, "Review", [] {});
            if (landscape) {
              lv_obj_set_x(list, 1100);
              lv_obj_set_width(list, 2004);
            }
            lv_obj_update_layout(screen);
            const int bottom = lv_obj_get_height(screen) -
                widgets::NavigationHeight(screen) - (landscape ? 20 : 24);
            for (bool selected : {false, true, false}) {
              LayoutPartitionReview(screen, list, review, selected);
              lv_obj_update_layout(screen);
              assert(lv_obj_has_flag(review, LV_OBJ_FLAG_HIDDEN) == !selected);
              assert(lv_obj_get_y(list) + lv_obj_get_height(list) ==
                  bottom - (selected && !landscape ? 178 : 0));
              assert(lv_obj_get_y(review) + lv_obj_get_height(review) == bottom);
              Contained(list, screen);
              if (selected && !landscape)
                assert(lv_obj_get_y(review) -
                    (lv_obj_get_y(list) + lv_obj_get_height(list)) == 28);
            }
            lv_obj_delete(list);
            lv_obj_delete(review);
          }
          for (Action active : actions) {
            long_labels = size == 2;
            auto *bar = widgets::Navigation(screen, active, Click, nullptr);
            Settle(screen);
            Contained(bar, screen);
            assert(lv_obj_get_child_count(bar) == 5);
            for (int j = 0; j < 4; ++j) {
              auto *button = lv_obj_get_child(bar, j + 1);
              Contained(button, bar);
              if (id >= 4) {
                assert(!lv_obj_has_flag(button, LV_OBJ_FLAG_PRESS_LOCK));
                const uint32_t expected_children = active == actions[j] ? 2 : 1;
                assert(lv_obj_get_child_count(button) == expected_children);
                for (uint32_t k = 0; k < lv_obj_get_child_count(button); ++k)
                  Contained(lv_obj_get_child(button, k), button);
                const int before = clicks;
                lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
                assert(clicks == before + 1 && last_action == actions[j]);
              }
            }
            if (id >= 4) {
              auto *shelf = lv_obj_get_child(bar, 0);
              widgets::UpdateNavigationDockAppearance(bar, 0, 100);
              assert(lv_obj_get_style_bg_opa(shelf, LV_PART_MAIN) == LV_OPA_TRANSP);
              assert(!lv_obj_get_style_blur_backdrop(shelf, LV_PART_MAIN));
              widgets::UpdateNavigationDockAppearance(bar, 100, 0);
              assert(lv_obj_get_style_bg_opa(shelf, LV_PART_MAIN) == LV_OPA_TRANSP);
              assert(!lv_obj_get_style_blur_backdrop(shelf, LV_PART_MAIN));
              auto *button = lv_obj_get_child(bar, 1);
              lv_area_t a{}; lv_obj_get_coords(button, &a);
              const int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
              const int before = clicks;
              Input(input, x, y, true);
              Input(input, x, a.y1 - 60, true);
              Input(input, x, a.y1 - 60, false);
              assert(clicks == before);
              Input(input, x, y, true); Input(input, x, y, false);
              assert(clicks == before + 1);
            }
            lv_obj_delete(bar);
          }
          auto *hidden = widgets::Navigation(screen, Action::kBackHome,
                                             Click, nullptr, true);
          assert(lv_obj_has_flag(hidden, LV_OBJ_FLAG_HIDDEN));
          lv_obj_delete(hidden);
        }
      }
    }
  }
  if (argc == 2) {
    lv_display_set_resolution(display, 1440, 3168);
    design::ApplySurfaceMode(false);
    design::ApplyInterfaceSize(1);
    design::ApplyAccent(0x16c8ff);
    lv_obj_set_style_bg_color(screen, design::kMainCanvas, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    long_labels = false;
    layout = DockLayout::kTextOnly;
    auto *bar = widgets::Navigation(screen, Action::kBackHome, Click, nullptr);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 80, 150);
    Settle(screen);
    png_image png{}; png.version = PNG_IMAGE_VERSION;
    png.width = 1440; png.height = 450; png.format = PNG_FORMAT_BGRA;
    assert(png_image_write_to_file(&png, argv[1], 0, pixels.data(), 0, nullptr));
  }
  lv_deinit();
  puts("All dock layouts: portrait/landscape, themes, sizes, actions, live appearance and drag-away cancellation passed.");
}
