/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <array>
#include <linux/input.h>

namespace aeraui::hardware_keys {
// Evdev positions to US X keysyms. Keep modifiers separate for Windows so
// combinations and key releases retain their original physical identity.
inline unsigned Symbol(unsigned code) {
  if (code >= KEY_A && code <= KEY_L) return "asdfghjkl"[code-KEY_A];
  if (code >= KEY_Q && code <= KEY_P) return "qwertyuiop"[code-KEY_Q];
  if (code >= KEY_Z && code <= KEY_M) return "zxcvbnm"[code-KEY_Z];
  if (code >= KEY_1 && code <= KEY_0) return "1234567890"[code-KEY_1];
  if (code >= KEY_F1 && code <= KEY_F10) return 0xffbe + code-KEY_F1;
  switch(code) {
    case KEY_F11:return 0xffc8; case KEY_F12:return 0xffc9;
    case KEY_SPACE:return ' '; case KEY_MINUS:return '-'; case KEY_EQUAL:return '=';
    case KEY_LEFTBRACE:return '['; case KEY_RIGHTBRACE:return ']';
    case KEY_BACKSLASH:return '\\'; case KEY_SEMICOLON:return ';';
    case KEY_APOSTROPHE:return '\''; case KEY_GRAVE:return '`';
    case KEY_COMMA:return ','; case KEY_DOT:return '.'; case KEY_SLASH:return '/';
    case KEY_ENTER:return 0xff0d; case KEY_KPENTER:return 0xff8d;
    case KEY_BACKSPACE:return 0xff08; case KEY_TAB:return 0xff09;
    case KEY_ESC:return 0xff1b; case KEY_DELETE:return 0xffff;
    case KEY_INSERT:return 0xff63; case KEY_HOME:return 0xff50;
    case KEY_END:return 0xff57; case KEY_PAGEUP:return 0xff55; case KEY_PAGEDOWN:return 0xff56;
    case KEY_LEFT:return 0xff51; case KEY_UP:return 0xff52;
    case KEY_RIGHT:return 0xff53; case KEY_DOWN:return 0xff54;
    case KEY_LEFTSHIFT:return 0xffe1; case KEY_RIGHTSHIFT:return 0xffe2;
    case KEY_LEFTCTRL:return 0xffe3; case KEY_RIGHTCTRL:return 0xffe4;
    case KEY_CAPSLOCK:return 0xffe5;
    case KEY_LEFTALT:return 0xffe9; case KEY_RIGHTALT:return 0xffea;
    case KEY_LEFTMETA:return 0xffeb; case KEY_RIGHTMETA:return 0xffec;
    case KEY_NUMLOCK:return 0xff7f; case KEY_SCROLLLOCK:return 0xff14;
    case KEY_KP0:return 0xffb0; case KEY_KP1:return 0xffb1; case KEY_KP2:return 0xffb2;
    case KEY_KP3:return 0xffb3; case KEY_KP4:return 0xffb4; case KEY_KP5:return 0xffb5;
    case KEY_KP6:return 0xffb6; case KEY_KP7:return 0xffb7; case KEY_KP8:return 0xffb8;
    case KEY_KP9:return 0xffb9; case KEY_KPDOT:return 0xffae;
    case KEY_KPPLUS:return 0xffab; case KEY_KPMINUS:return 0xffad;
    case KEY_KPASTERISK:return 0xffaa; case KEY_KPSLASH:return 0xffaf;
    default:return 0;
  }
}
struct State {
  std::array<bool, KEY_MAX+1> down{};
  bool caps=false, num=true;
  void Reset() { down.fill(false); }
  bool Shift() const {return down[KEY_LEFTSHIFT]||down[KEY_RIGHTSHIFT];}
  bool Ctrl() const {return down[KEY_LEFTCTRL]||down[KEY_RIGHTCTRL];}
  bool Alt() const {return down[KEY_LEFTALT]||down[KEY_RIGHTALT];}
  bool Update(unsigned code,int value) {
    if(code>KEY_MAX||value<0||value>2||!Symbol(code))return false;
    if(value==1&&!down[code]) {
      if(code==KEY_CAPSLOCK)caps=!caps;
      if(code==KEY_NUMLOCK)num=!num;
    }
    down[code]=value!=0;return true;
  }
  unsigned Text(unsigned code,bool qwertz=false) const {
    unsigned s=Symbol(code);
    if(qwertz) {if(s=='y')s='z';else if(s=='z')s='y';}
    if(s>='a'&&s<='z')return Shift()!=caps?s-'a'+'A':s;
    if(s>=0xffb0&&s<=0xffb9)return num?'0'+s-0xffb0:0;
    if(s==0xffae)return num?'.':0;
    if(s==0xffab)return '+';
    if(s==0xffad)return '-';
    if(s==0xffaa)return '*';
    if(s==0xffaf)return '/';
    if(s<32||s>126)return 0;
    if(Shift()) {
      constexpr char plain[]="1234567890-=[]\\;'`,./";
      constexpr char shifted[]="!@#$%^&*()_+{}|:\"~<>?";
      for(unsigned i=0;plain[i];++i)if(s==static_cast<unsigned>(plain[i]))return shifted[i];
    }
    return s;
  }
};
} // namespace aeraui::hardware_keys
