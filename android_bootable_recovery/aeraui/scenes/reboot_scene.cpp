/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "ui_components.hpp"
#include "power_transition.hpp"
#include <cctype>

namespace aeraui {
void BuildRebootScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  using namespace widgets;
  using namespace design;
  Header(screen, "Reboot", "Choose a destination or manage the active boot slot.",
         callback, context);
  const bool landscape = Landscape(screen);

  std::string active = RecoverySlot();
  std::transform(active.begin(), active.end(), active.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::toupper(value));
                 });
  if (active != "A" && active != "B") active = "—";

  auto *slot_card = lv_obj_create(screen);
  Panel(slot_card, 38, kMainSheet);
  lv_obj_set_pos(slot_card, 64, landscape ? 340 : 450);
  lv_obj_set_size(slot_card, 1312, 374);
  lv_obj_set_style_border_width(slot_card, 1, 0);
  lv_obj_set_style_border_color(slot_card, kMainLine, 0);
  lv_obj_set_style_border_opa(slot_card, LV_OPA_30, 0);

  auto *slot_icon = lv_obj_create(slot_card);
  Panel(slot_icon, 25, IconBackground());
  lv_obj_set_pos(slot_icon, 32, 34);
  lv_obj_set_size(slot_icon, 94, 94);
  auto *slot_symbol = Label(slot_icon, LV_SYMBOL_SHUFFLE,
                            &lv_font_montserrat_32, kAccent);
  lv_obj_center(slot_symbol);

  auto *caption = Label(slot_card, "ACTIVE BOOT SLOT",
                        &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(caption, 158, 36);
  lv_obj_set_style_text_letter_space(caption, 2, 0);
  auto *current = Label(slot_card, ("Slot " + active).c_str(),
                        &lv_font_montserrat_48, kText);
  lv_obj_set_pos(current, 158, 78);
  auto *hint = Label(slot_card,
      "The selected slot is used the next time Android starts.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 34, 154);
  lv_obj_set_width(hint, 1220);

  auto *feedback = Label(slot_card, "", &lv_font_montserrat_24, kRed);
  lv_obj_set_pos(feedback, 34, 332);
  lv_obj_set_width(feedback, 1220);

  for (int index = 0; index < 2; ++index) {
    const std::string slot(1, static_cast<char>('A' + index));
    const bool selected = active == slot;
    const std::string slot_label = i18n::Format("Slot %s", slot.c_str());
    auto *button = Button(slot_card, slot_label.c_str(), [=] {
      if (selected) return;
      const std::string title =
          i18n::Format("Switch to Slot %s?", slot.c_str());
      const std::string detail = i18n::Format(
          "AERA will mark Slot %s active. This does not reboot the device yet.",
          slot.c_str());
      Sheet(screen, title.c_str(), detail.c_str(), [=] {
        if (RecoverySetActiveSlot(slot)) {
          callback(Action::kOpenReboot, context);
        } else {
          i18n::BindLabel(feedback,
                            "Could not change the active boot slot.");
        }
      });
    }, selected);
    lv_obj_set_pos(button, 34 + index * 630, 212);
    lv_obj_set_size(button, 606, 104);
    if (selected) {
      auto *check = Label(button, LV_SYMBOL_OK, &lv_font_montserrat_24,
                          kOnAccent);
      lv_obj_align(check, LV_ALIGN_RIGHT_MID, -34, 0);
    }
  }
  AnimateEnter(slot_card, 20, 10);

  auto *section = Label(screen, "REBOOT DESTINATION",
                        &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(section, landscape ? 1450 : 80,
                 landscape ? 340 : 888);
  lv_obj_set_style_text_letter_space(section, 2, 0);

  struct Destination { const char *icon, *name, *detail; Action action; };
  const std::array<Destination, 5> destinations{{
    {LV_SYMBOL_HOME, "Android", "Leave recovery and start the system", Action::kRebootSystem},
    {LV_SYMBOL_REFRESH, "Recovery", "Restart AERA Recovery Project", Action::kRebootRecovery},
    {LV_SYMBOL_SETTINGS, "Bootloader", "Restart in hardware fastboot mode", Action::kRebootBootloader},
    {LV_SYMBOL_USB, "Fastbootd", "Userspace fastboot for logical partitions", Action::kRebootFastbootd},
    {LV_SYMBOL_POWER, "Power off", "Turn off the device", Action::kPowerOff}}};
  auto *list = Scroll(screen, landscape ? 392 : 948,
                      landscape ? 820 : 1730);
  if (landscape) {
    lv_obj_set_x(list, 1436);
    lv_obj_set_width(list, 1668);
  }
  for (size_t i = 0; i < destinations.size(); ++i) {
    const auto d = destinations[i];
    const int column = static_cast<int>(i % 2);
    const int row = static_cast<int>(i / 2);
    auto *card = lv_button_create(list);
    Panel(card, 34, kMainSheet);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, column * (landscape ? 842 : 672),
                   row * (landscape ? 372 : 292));
    lv_obj_set_size(card, landscape ? 818 : 640,
                    landscape ? 344 : 260);
    lv_obj_set_style_transform_scale(card, 256, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card,
                                  d.action == Action::kPowerOff ? kRed : kMainLine,
                                  0);
    lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
    OnClick(card, [=] {
      const bool power_off = d.action == Action::kPowerOff;
      const std::string title = power_off
          ? "Power off device?" : std::string("Reboot to ") + d.name + "?";
      std::string detail = std::string(d.detail) + ".\n\n";
      if (active != "—" && !power_off)
        detail += "Active slot: " + active + "  •  ";
      detail += "The current recovery session will end.";
      Sheet(screen, title, detail, [=] {
        if (d.action == Action::kRebootFastbootd) {
          const uint32_t transition_duration =
              RecoveryFastbootWifiStatus().enabled ? 1080 : 720;
          ModeTransition(screen, true,
                         [=] { callback(d.action, context); },
                         transition_duration);
          return;
        }
        const std::string transition_title = power_off
            ? "Powering off" : std::string("Rebooting to ") + d.name;
        PowerTransition(screen, transition_title,
                        [=] { callback(d.action, context); });
      },
            0, false, SheetPresentation::kCompactGlass,
            power_off ? "Slide to power off" : "Slide to reboot");
    });

    const bool power_off = d.action == Action::kPowerOff;
    auto *plate = lv_obj_create(card);
    Panel(plate, 24,
          RecoveryTintedIconBackgrounds()
              ? (power_off ? kRedSoft : kAccentSoft)
              : kMainPanel);
    lv_obj_set_pos(plate, 30, 30);
    lv_obj_set_size(plate, 92, 92);
    auto *icon = Label(plate, d.icon, &lv_font_montserrat_32,
                       power_off ? kRed : kAccent);
    lv_obj_center(icon);
    auto *name = Label(card, d.name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 148, 46);
    lv_obj_set_width(name, landscape ? 590 : 420);
    auto *detail = Label(card, d.detail, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail, 30, 154);
    lv_obj_set_width(detail, landscape ? 738 : 560);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    AnimateEnter(card, 60 + static_cast<uint32_t>(i) * 28, 10);
  }
  Navigation(screen, Action::kSettings, callback, context);
}
}  // namespace aeraui
