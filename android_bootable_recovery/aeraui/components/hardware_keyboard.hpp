/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "phone_keyboard.hpp"
#include "ui_components.hpp"
#include "../core/hardware_keys.hpp"

namespace aeraui::hardware_keyboard {
inline lv_obj_t *focus = nullptr;
inline void ClearFocus() {
  if(focus)lv_obj_remove_state(focus,LV_STATE_FOCUSED);
  focus=nullptr;
}
inline void FocusDeleted(lv_event_t *e) {
  if (focus == lv_event_get_target_obj(e)) focus = nullptr;
}
inline bool Inside(lv_obj_t *obj, lv_obj_t *root) {
  for (;obj;obj=lv_obj_get_parent(obj)) if(obj==root)return true;
  return false;
}
inline lv_obj_t *Scope() {
  auto *screen=lv_screen_active();
  lv_obj_update_layout(screen);
  for(int i=static_cast<int>(lv_obj_get_child_count(screen))-1;i>=0;--i) {
    auto *obj=lv_obj_get_child(screen,i);
    if(!lv_obj_is_visible(obj))continue;
    auto *marker=lv_obj_get_user_data(obj);
    if(marker==&widgets::kModalMarker||marker==&widgets::kPersistentModalMarker)return obj;
  }
  return screen;
}
inline void Collect(lv_obj_t *obj,std::vector<lv_obj_t *> &items) {
  if(!lv_obj_is_visible(obj)||lv_obj_has_state(obj,LV_STATE_DISABLED))return;
  if(lv_obj_check_type(obj,&lv_keyboard_class))return;
  if(lv_obj_check_type(obj,&lv_textarea_class)||lv_obj_check_type(obj,&lv_button_class)) {
    items.push_back(obj);return;
  }
  for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i)Collect(lv_obj_get_child(obj,i),items);
}
inline bool Handle(unsigned code,int value,const hardware_keys::State &keys) {
  if(value==0)return true;
  auto *scope=Scope();
  auto *keyboard=phone_keyboard::VisibleKeyboard();
  auto *input=text_edit::FocusedInput();
  if(keyboard)input=lv_keyboard_get_textarea(keyboard);
  if(input&&(!Inside(input,scope)||!lv_obj_is_visible(input)||lv_obj_has_state(input,LV_STATE_DISABLED)))input=nullptr;
  if(focus&&(!Inside(focus,scope)||!lv_obj_is_visible(focus))) {
    lv_obj_remove_state(focus,LV_STATE_FOCUSED);focus=nullptr;
  }
  if(code==KEY_TAB&&!keys.Ctrl()&&!keys.Alt()) {
    std::vector<lv_obj_t *> items;Collect(scope,items);
    if(items.empty())return true;
    auto it=std::find(items.begin(),items.end(),focus?focus:input);
    int index=it==items.end()?(keys.Shift()?0:-1):static_cast<int>(it-items.begin());
    index=(index+(keys.Shift()?-1:1)+static_cast<int>(items.size()))%items.size();
    if(focus)lv_obj_remove_state(focus,LV_STATE_FOCUSED);
    focus=items[index];
    lv_obj_remove_event_cb(focus,FocusDeleted);
    lv_obj_add_event_cb(focus,FocusDeleted,LV_EVENT_DELETE,nullptr);
    lv_obj_add_state(focus,LV_STATE_FOCUSED);
    lv_obj_scroll_to_view(focus,LV_ANIM_OFF);
    lv_obj_send_event(focus,LV_EVENT_FOCUSED,nullptr);
    if(keyboard)lv_keyboard_set_textarea(keyboard,
        lv_obj_check_type(focus,&lv_textarea_class)?focus:nullptr);
    return true;
  }
  if(code==KEY_ESC) {
    if(value==2)return true;
    if(keyboard) {lv_obj_send_event(keyboard,LV_EVENT_CANCEL,nullptr);return true;}
    return false; // Engine's normal Back path closes overlays first.
  }
  if(focus&&!lv_obj_check_type(focus,&lv_textarea_class)&&
      (code==KEY_ENTER||code==KEY_SPACE)) {
    if(value==1&&!lv_obj_has_state(focus,LV_STATE_DISABLED))lv_obj_send_event(focus,LV_EVENT_CLICKED,nullptr);
    return true;
  }
  if(focus&&!lv_obj_check_type(focus,&lv_textarea_class))return true;
  if(!input)return true;
  if(keys.Ctrl()) {
    if(value==2)return true;
    switch(code) {
      case KEY_A:text_edit::SelectAll(input);break;
      case KEY_C:
        if(lv_textarea_text_is_selected(input))text_edit::Copy(input);
        else text_edit::SendControl(input,3);
        break;
      case KEY_D:text_edit::SendControl(input,4);break;
      case KEY_X:text_edit::Cut(input);break;
      case KEY_V:text_edit::Paste(input);break;
      default:break;
    }
    return true;
  }
  if(keys.Alt()||keys.down[KEY_LEFTMETA]||keys.down[KEY_RIGHTMETA])return true;
  switch(code) {
    case KEY_BACKSPACE:lv_textarea_delete_char(input);break;
    case KEY_DELETE:lv_textarea_delete_char_forward(input);break;
    case KEY_LEFT:lv_textarea_cursor_left(input);break;
    case KEY_RIGHT:lv_textarea_cursor_right(input);break;
    case KEY_UP:lv_textarea_cursor_up(input);break;
    case KEY_DOWN:lv_textarea_cursor_down(input);break;
    case KEY_HOME:lv_textarea_set_cursor_pos(input,0);break;
    case KEY_END:lv_textarea_set_cursor_pos(input,LV_TEXTAREA_CURSOR_LAST);break;
    case KEY_ENTER:case KEY_KPENTER:
      if(value==2)break;
      if(!lv_textarea_get_one_line(input))lv_textarea_add_char(input,'\n');
      else if(keyboard)lv_obj_send_event(keyboard,LV_EVENT_READY,nullptr);
      else lv_obj_send_event(input,LV_EVENT_READY,nullptr);
      break;
    default: {
      unsigned c=keys.Text(code,RecoveryKeyboardLayout()==KeyboardLayout::kQwertz);
      if(c)lv_textarea_add_char(input,c);
    }
  }
  return true;
}
} // namespace aeraui::hardware_keyboard
