/* SPDX-License-Identifier: Apache-2.0 */
#include "../core/hardware_keys.hpp"
#include <cassert>
using namespace aeraui::hardware_keys;
int main() {
  State s;
  assert(Symbol(KEY_UP)==0xff52 && Symbol(KEY_F12)==0xffc9);
  assert(Symbol(KEY_RIGHTCTRL)==0xffe4 && Symbol(KEY_POWER)==0);
  assert(!s.Update(BTN_TOUCH,1) && !s.Update(KEY_A,3));
  assert(s.Update(KEY_A,1) && s.Text(KEY_A)=='a');
  s.Update(KEY_LEFTSHIFT,1);assert(s.Text(KEY_A)=='A' && s.Text(KEY_1)=='!');
  s.Update(KEY_RIGHTSHIFT,1);s.Update(KEY_LEFTSHIFT,0);assert(s.Shift());
  s.Update(KEY_CAPSLOCK,1);assert(s.Text(KEY_A)=='a');
  s.Update(KEY_CAPSLOCK,2);assert(s.caps); // repeat cannot toggle locks
  s.Update(KEY_RIGHTSHIFT,0);assert(s.Text(KEY_A)=='A');
  s.Update(KEY_CAPSLOCK,0);s.Update(KEY_CAPSLOCK,1);assert(!s.caps);
  assert(s.Text(KEY_Y,true)=='z' && s.Text(KEY_Z,true)=='y');
  s.Update(KEY_LEFTCTRL,1);assert(s.Ctrl());s.Reset();assert(!s.Ctrl()&&!s.Shift());
  assert(s.Text(KEY_KP8)=='8');s.Update(KEY_NUMLOCK,1);assert(s.Text(KEY_KP8)==0);
  for(unsigned key=0;key<=KEY_MAX;++key) {
    auto symbol=Symbol(key);
    assert(!symbol || (symbol>=32&&symbol<=126) || (symbol>=0xff00&&symbol<=0xffff));
  }
}
