/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <string>
#include <vector>

#include <lvgl.h>

#include "design.hpp"
#include "phone_keyboard.hpp"
#include "ui_components.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

struct TerminalUi {
  lv_obj_t *screen = nullptr;
  lv_obj_t *viewport = nullptr;
  lv_obj_t *output = nullptr;
  lv_obj_t *command = nullptr;
  lv_obj_t *command_bar = nullptr;
  lv_obj_t *controls = nullptr;
  lv_obj_t *keyboard = nullptr;
  lv_timer_t *timer = nullptr;
  int update_counter = -1;
  bool keyboard_visible = false;
  bool landscape = false;
};

void LayoutCommandLine(TerminalUi *state) {
  if (state == nullptr || state->viewport == nullptr ||
      state->output == nullptr || state->command_bar == nullptr) return;
  const int content_width = std::max(320,
      static_cast<int>(lv_obj_get_width(state->viewport)) - 68);
  lv_obj_set_width(state->output, content_width);
  lv_obj_update_layout(state->output);
  const int content_height = std::max(140,
      static_cast<int>(lv_obj_get_height(state->viewport)) - 68);
  const int line_y = std::max(static_cast<int>(lv_obj_get_height(state->output)) + 18,
                              content_height - 116);
  lv_obj_set_pos(state->command_bar, 0, line_y);
  lv_obj_set_size(state->command_bar, content_width, 116);
  lv_obj_set_size(state->command, content_width - 52, 104);
}

void RefreshOutput(TerminalUi *state, bool force = false) {
  if (state == nullptr) return;
  RecoveryTerminalPoll();
  const int update = RecoveryTerminalUpdateCounter();
  if (!force && update == state->update_counter) return;
  state->update_counter = update;

  const auto lines = RecoveryTerminalLines(400);
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    text += lines[i];
    if (i + 1 < lines.size()) text.push_back('\n');
  }
  if (text.empty()) text = "Starting recovery shell…";
  i18n::BindLabel(state->output, text.c_str());
  LayoutCommandLine(state);
  lv_obj_update_layout(state->viewport);
  lv_obj_scroll_to_y(state->viewport, LV_COORD_MAX, LV_ANIM_OFF);
}

void SetKeyboardVisible(TerminalUi *state, bool visible) {
  if (state == nullptr || state->keyboard_visible == visible) return;
  state->keyboard_visible = visible;
  if (visible) {
    if (state->landscape) {
      lv_obj_set_pos(state->controls, 1400, 350);
      lv_obj_set_size(state->controls, 1704, 112);
      for (int index = 0; index < 6; ++index) {
        auto *button = lv_obj_get_child(state->controls, index);
        lv_obj_set_pos(button, index * 284, 0);
        lv_obj_set_size(button, 264, 112);
      }
      lv_obj_set_pos(state->viewport, 64, 350);
      lv_obj_set_size(state->viewport, 1292, 900);
    } else {
      lv_obj_set_pos(state->controls, 64, 1910);
      lv_obj_set_size(state->controls, 1312, 112);
      lv_obj_set_height(state->viewport, 1450);
    }
    lv_obj_remove_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(state->keyboard);
    lv_keyboard_set_textarea(state->keyboard, state->command);
  } else {
    lv_obj_add_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
    if (state->landscape) {
      lv_obj_set_pos(state->viewport, 64, 350);
      lv_obj_set_size(state->viewport, 2112, 900);
      lv_obj_set_pos(state->controls, 2240, 350);
      lv_obj_set_size(state->controls, 864, 284);
      for (int index = 0; index < 6; ++index) {
        auto *button = lv_obj_get_child(state->controls, index);
        lv_obj_set_pos(button, (index % 3) * 296, (index / 3) * 132);
        lv_obj_set_size(button, 272, 112);
      }
    } else {
      lv_obj_set_pos(state->controls, 64, 2720);
      lv_obj_set_size(state->controls, 1312, 112);
      lv_obj_set_height(state->viewport, 2260);
    }
    lv_obj_clear_state(state->command, LV_STATE_FOCUSED);
  }
  RefreshOutput(state, true);
}

void SendCommand(TerminalUi *state) {
  if (state == nullptr) return;
  const std::string command = lv_textarea_get_text(state->command);
  if (!command.empty()) RecoveryTerminalWrite(command);
  RecoveryTerminalWrite("\n");
  lv_textarea_set_text(state->command, "");
  SetKeyboardVisible(state, false);
}

void TerminalTick(lv_timer_t *timer) {
  RefreshOutput(static_cast<TerminalUi *>(lv_timer_get_user_data(timer)));
}

void DeleteTerminalUi(lv_event_t *event) {
  auto *state = static_cast<TerminalUi *>(lv_event_get_user_data(event));
  if (state == nullptr) return;
  if (state->timer != nullptr) lv_timer_delete(state->timer);
  delete state;
}

lv_obj_t *Control(TerminalUi *state, int x, int width, const char *label,
                  TerminalKey key) {
  auto *button = Button(state->controls, label, [key] {
    RecoveryTerminalSendKey(key);
  });
  lv_obj_set_pos(button, x, 0);
  lv_obj_set_size(button, width, 112);
  lv_obj_set_style_radius(button, 30, 0);
  return button;
}

}  // namespace

void BuildTerminalScene(lv_obj_t *screen, ActionCallback callback,
                        void *context) {
  auto *state = new TerminalUi;
  state->screen = screen;
  state->landscape = Landscape(screen);
  lv_obj_add_event_cb(screen, DeleteTerminalUi, LV_EVENT_DELETE, state);

  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  auto *heading = Label(screen, "Terminal", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(heading, 80, state->landscape ? 210 : 252);
  auto *subtitle = Label(screen, "AERA shell  /  live recovery session",
                         &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, state->landscape ? 600 : 80,
                 state->landscape ? 220 : 332);
  auto *live = Kicker(screen, "LIVE RECOVERY SHELL", kGreen);
  lv_obj_align(live, LV_ALIGN_TOP_RIGHT, -80,
               state->landscape ? 286 : 342);

  state->viewport = lv_obj_create(screen);
  Panel(state->viewport, 34, Color(0x101216));
  lv_obj_set_pos(state->viewport, 64, state->landscape ? 350 : 430);
  lv_obj_set_size(state->viewport, state->landscape ? 2112 : 1312,
                  state->landscape ? 900 : 2260);
  lv_obj_set_style_border_width(state->viewport, 1, 0);
  lv_obj_set_style_border_color(state->viewport, kMainLine, 0);
  lv_obj_set_style_border_opa(state->viewport, LV_OPA_60, 0);
  lv_obj_set_style_pad_all(state->viewport, 34, 0);
  lv_obj_add_flag(state->viewport, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(state->viewport, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->viewport, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(state->viewport, kAccent, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(state->viewport, 5, LV_PART_SCROLLBAR);

  state->output = Label(state->viewport, "Starting recovery shell…",
                        &lv_font_montserrat_24, Color(0xe9f0f4));
  lv_obj_set_width(state->output, state->landscape ? 2024 : 1224);
  lv_obj_set_style_text_line_space(state->output, 8, 0);

  state->controls = lv_obj_create(screen);
  Clear(state->controls);
  lv_obj_set_pos(state->controls, state->landscape ? 2240 : 64,
                 state->landscape ? 350 : 2720);
  lv_obj_set_size(state->controls, state->landscape ? 864 : 1312,
                  state->landscape ? 284 : 112);
  Control(state, 0, 196, "Esc", TerminalKey::kEscape);
  Control(state, 212, 196, "Tab", TerminalKey::kTab);
  Control(state, 424, 196, LV_SYMBOL_UP, TerminalKey::kUp);
  Control(state, 636, 196, LV_SYMBOL_DOWN, TerminalKey::kDown);
  Control(state, 848, 216, "Ctrl+C", TerminalKey::kInterrupt);
  auto *clear = Button(state->controls, "Clear", [state] {
    RecoveryTerminalClear();
    RefreshOutput(state, true);
  });
  lv_obj_set_pos(clear, 1080, 0);
  lv_obj_set_size(clear, 232, 112);
  lv_obj_set_style_radius(clear, 30, 0);
  if (state->landscape) {
    const int width = 272;
    for (int index = 0; index < 5; ++index) {
      auto *button = lv_obj_get_child(state->controls, index);
      lv_obj_set_pos(button, (index % 3) * 296, (index / 3) * 132);
      lv_obj_set_size(button, width, 112);
    }
    lv_obj_set_pos(clear, 592, 132);
    lv_obj_set_size(clear, width, 112);
  }

  state->command_bar = lv_obj_create(state->viewport);
  Clear(state->command_bar);
  lv_obj_set_style_border_width(state->command_bar, 1, 0);
  lv_obj_set_style_border_side(state->command_bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(state->command_bar, kMainLine, 0);
  lv_obj_set_style_border_opa(state->command_bar, LV_OPA_60, 0);
  auto *prompt = Label(state->command_bar, "$", &lv_font_montserrat_32,
                       kAccent);
  lv_obj_set_pos(prompt, 0, 40);
  state->command = TextArea(state->command_bar);
  lv_obj_set_pos(state->command, 52, 6);
  lv_textarea_set_one_line(state->command, true);
  lv_textarea_set_placeholder_text(state->command, "Type a command…");
  lv_obj_set_style_text_font(state->command, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(state->command, kText, 0);
  lv_obj_set_style_text_color(state->command, kMuted,
                              LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_opa(state->command, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(state->command, 0, 0);
  lv_obj_set_style_pad_all(state->command, 22, 0);
  LayoutCommandLine(state);

  state->keyboard = lv_keyboard_create(screen);
  phone_keyboard::Apply(state->keyboard);
  lv_obj_set_align(state->keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(state->keyboard, state->landscape ? 1400 : 0,
                 state->landscape ? 500 : 2070);
  lv_obj_set_size(state->keyboard, state->landscape ? 1768 : 1440,
                  state->landscape ? 768 : 1098);
  lv_keyboard_set_textarea(state->keyboard, state->command);
  lv_obj_add_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_event_cb(state->command, [](lv_event_t *event) {
    SetKeyboardVisible(static_cast<TerminalUi *>(lv_event_get_user_data(event)),
                       true);
  }, LV_EVENT_CLICKED, state);
  lv_obj_add_event_cb(state->command, [](lv_event_t *event) {
    SendCommand(static_cast<TerminalUi *>(lv_event_get_user_data(event)));
  }, LV_EVENT_READY, state);
  text_edit::SetControlHandler(state->command, [](lv_obj_t *input, unsigned key, void *) {
    if (key == 3 || key == 4) {
      const char bytes[] = {static_cast<char>(key), 0};
      RecoveryTerminalWrite(bytes);
      if (key == 3) lv_textarea_set_text(input, "");
    }
  }, state);
  lv_obj_add_event_cb(state->keyboard, [](lv_event_t *event) {
    auto *state = static_cast<TerminalUi *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY) SendCommand(state);
    else if (lv_event_get_code(event) == LV_EVENT_CANCEL)
      SetKeyboardVisible(state, false);
  }, LV_EVENT_ALL, state);

  RecoveryTerminalStart(state->landscape ? 112 : 78,
                        state->landscape ? 38 : 42,
                        state->landscape ? 2024 : 1224,
                        state->landscape ? 820 : 1860);
  RefreshOutput(state, true);
  state->timer = lv_timer_create(TerminalTick, 32, state);
  AnimateEnter(state->viewport, 20, 16);
}

}  // namespace aeraui
