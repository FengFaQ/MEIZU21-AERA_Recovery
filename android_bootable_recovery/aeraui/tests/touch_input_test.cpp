// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <sys/time.h>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#include "../../minuitwrp/events.cpp"

int gr_fb_width() { return 1440; }
int gr_fb_height() { return 3168; }

static input_event Send(ev &device, int type, int code, int value, bool emitted) {
  input_event event{};
  event.type = type; event.code = code; event.value = value;
  assert((vk_modify(&device, &event) == 0) == emitted);
  return event;
}
static void Axis(ev &device, int code, int value) { Send(device, EV_ABS, code, value, false); }
static void Check(const input_event &event, int x, int y, bool pressed = true) {
  assert(event.type == EV_ABS && event.code == (pressed ? 1 : 0));
  assert((event.value >> 16) == x && (event.value & 0xffff) == y);
}

int main(int argc, char **argv) {
  ev device{};
  if (argc > 1 && !strcmp(argv[1], "single")) {
    Axis(device, ABS_X, 500);
    Send(device, EV_SYN, SYN_REPORT, 0, false);
    Axis(device, ABS_Y, 2700);
    Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 500, 2700);
    Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 500, 2700, false);
    Axis(device, ABS_Y, 2800);
    Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 500, 2800);
    Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 500, 2800, false);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "legacy")) {
    Axis(device, ABS_MT_POSITION_X, 500);
    Axis(device, ABS_MT_POSITION_Y, 2700);
    Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 500, 2700);
    Axis(device, ABS_MT_TOUCH_MAJOR, 0);
    Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 500, 2700, false);
    assert(device.mt_p.x == 500 && device.mt_p.y == 2700);
    Axis(device, ABS_MT_POSITION_X, 600);
    Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 600, 2700);
    Axis(device, ABS_MT_PRESSURE, 0);
    Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 600, 2700, false);
    Check(Send(device, EV_ABS, ABS_MT_POSITION, (700 << 16) | 2800, true), 700, 2800);
    Check(Send(device, EV_ABS, ABS_MT_POSITION, INT_MIN, true), 700, 2800, false);
    return 0;
  }
  Axis(device, ABS_MT_TRACKING_ID, 1);
  Axis(device, ABS_MT_POSITION_X, 500);
  Send(device, EV_SYN, SYN_REPORT, 0, false);
  Axis(device, ABS_MT_POSITION_Y, 2700);
  Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 500, 2700);
  Axis(device, ABS_MT_POSITION_X, 530);
  Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 530, 2700);
  Axis(device, ABS_MT_TRACKING_ID, -1);
  Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 530, 2700, false);
  assert(device.mt_p.x == 530 && device.mt_p.y == 2700);
  Axis(device, ABS_MT_TRACKING_ID, -1);
  Send(device, EV_SYN, SYN_REPORT, 0, false);
  Axis(device, ABS_MT_TRACKING_ID, 2);
  Axis(device, ABS_MT_POSITION_X, 700);
  Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 700, 2700);
  Axis(device, ABS_MT_POSITION_Y, 2750);
  Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 700, 2750);
  Axis(device, ABS_MT_TRACKING_ID, -1);
  Check(Send(device, EV_SYN, SYN_REPORT, 0, true), 700, 2750, false);
  puts("Partial initial reports, one-axis motion, retained axes and duplicate releases passed");
}
