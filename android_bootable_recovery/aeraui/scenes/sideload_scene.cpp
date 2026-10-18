/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include "ui_components.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

void Step(lv_obj_t *parent, int y, const char *number, const char *title,
          const char *detail) {
  auto *badge = lv_obj_create(parent);
  Panel(badge, LV_RADIUS_CIRCLE, IconBackground());
  lv_obj_set_pos(badge, 36, y);
  lv_obj_set_size(badge, 72, 72);
  auto *digit = Label(badge, number, &lv_font_montserrat_32, kAccent);
  lv_obj_center(digit);

  auto *heading = Label(parent, title, &lv_font_montserrat_32, kText);
  lv_obj_set_pos(heading, 136, y - 2);
  lv_obj_set_width(heading, 1040);
  auto *copy = Label(parent, detail, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 136, y + 46);
  lv_obj_set_width(copy, 1040);
  lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);
}
}  // namespace

void BuildSideloadScene(lv_obj_t *screen, ActionCallback callback,
                        void *context) {
  Header(screen, "ADB Sideload",
         "Install a ZIP package from a connected computer.",
         callback, context);
  const bool landscape = Landscape(screen);

  auto *summary = lv_obj_create(screen);
  Panel(summary, 40, kMainSheet);
  lv_obj_set_pos(summary, 64, landscape ? 340 : 460);
  lv_obj_set_size(summary, landscape ? 1450 : 1312,
                  landscape ? 820 : 570);
  lv_obj_set_style_border_width(summary, 1, 0);
  lv_obj_set_style_border_color(summary, kMainLine, 0);
  lv_obj_set_style_border_opa(summary, LV_OPA_30, 0);

  auto *plate = lv_obj_create(summary);
  Panel(plate, 28, IconBackground());
  lv_obj_set_pos(plate, 46, 44);
  lv_obj_set_size(plate, 122, 122);
  auto *usb = Label(plate, LV_SYMBOL_USB, &lv_font_montserrat_48, kAccent);
  lv_obj_center(usb);

  auto *ready =
      Label(summary, "Start ADB sideload", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(ready, 204, 42);
  lv_obj_set_width(ready, landscape ? 1160 : 1040);
  auto *detail = Label(
      summary,
      "AERA will temporarily replace the normal ADB connection with the "
      "dedicated sideload service.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(detail, 204, 112);
  lv_obj_set_width(detail, landscape ? 1160 : 1030);
  lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);

  auto *notice = Label(
      summary,
      LV_SYMBOL_WARNING
      "  ADB shell disconnects while sideload is active and returns "
      "automatically when it finishes.",
      &lv_font_montserrat_28, kAmber);
  lv_obj_set_pos(notice, 46, 230);
  lv_obj_set_width(notice, landscape ? 1358 : 1220);
  lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);

  auto *safe = Label(
      summary,
      "The package is streamed directly to the existing AERA installer. "
      "Its own installer controls which partitions are changed.",
      &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(safe, 46, landscape ? 400 : 390);
  lv_obj_set_width(safe, landscape ? 1358 : 1220);
  lv_label_set_long_mode(safe, LV_LABEL_LONG_WRAP);

  auto *guide = lv_obj_create(screen);
  Panel(guide, 40, kMainSheet);
  lv_obj_set_pos(guide, landscape ? 1560 : 64,
                 landscape ? 340 : 1080);
  lv_obj_set_size(guide, landscape ? 1544 : 1312,
                  landscape ? 820 : 1330);
  lv_obj_set_style_border_width(guide, 1, 0);
  lv_obj_set_style_border_color(guide, kMainLine, 0);
  lv_obj_set_style_border_opa(guide, LV_OPA_30, 0);

  auto *guide_title =
      Label(guide, "Start a sideload session", &lv_font_montserrat_40, kText);
  lv_obj_set_pos(guide_title, 40, 34);
  Step(guide, 126, "1", "Connect the USB cable",
       "Use a direct, reliable USB connection to your computer.");
  Step(guide, 300, "2", "Start sideload in AERA",
       "Normal ADB will disconnect and the device will wait for a package.");

  auto *command = lv_obj_create(guide);
  Panel(command, 24, kInset);
  lv_obj_set_pos(command, 36, 472);
  lv_obj_set_size(command, landscape ? 1472 : 1240, 150);
  auto *command_title =
      Label(command, "THEN RUN ON YOUR COMPUTER", &lv_font_montserrat_20,
            kMuted);
  lv_obj_set_pos(command_title, 28, 20);
  lv_obj_set_style_text_letter_space(command_title, 2, 0);
  auto *command_text =
      Label(command, "adb sideload package.zip", &lv_font_montserrat_32,
            kAccent);
  lv_obj_set_pos(command_text, 28, 70);
  lv_obj_set_width(command_text, landscape ? 1390 : 1160);
  lv_label_set_long_mode(command_text, LV_LABEL_LONG_DOT);

  auto *start = Button(
      guide, LV_SYMBOL_USB "  Start ADB sideload",
      [callback, context] { callback(Action::kStartSideload, context); },
      true);
  lv_obj_set_pos(start, 36, landscape ? 658 : 1120);
  lv_obj_set_size(start, landscape ? 1472 : 1240, 132);

  AnimateEnter(summary, 30, 20);
  AnimateEnter(guide, 65, 20);
  Navigation(screen, Action::kSettings, callback, context);
}

}  // namespace aeraui
