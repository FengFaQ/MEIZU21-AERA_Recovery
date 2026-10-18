/* SPDX-License-Identifier: Apache-2.0 */

#include "input.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <mutex>
#include <sys/ioctl.h>
#include <unistd.h>

#include <minuitwrp/minui.h>

namespace aera::remote::input {
namespace {

struct Device {
  std::mutex lock;
  int descriptor = -1;
  int contact = 1;
  bool touching = false;
};

Device& Shared() {
  static Device device;
  return device;
}

bool Emit(int descriptor, uint16_t type, uint16_t code, int32_t value) {
  input_event event{};
  event.type = type;
  event.code = code;
  event.value = value;
  return write(descriptor, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event));
}

bool Sync(int descriptor) {
  return Emit(descriptor, EV_SYN, SYN_REPORT, 0);
}

bool ConfigureAxis(int descriptor, unsigned int code, int maximum) {
  uinput_abs_setup axis{};
  axis.code = code;
  axis.absinfo.minimum = 0;
  axis.absinfo.maximum = maximum;
  return ioctl(descriptor, UI_ABS_SETUP, &axis) == 0;
}

bool Open(Device* device) {
  if (device->descriptor >= 0) return true;
  int descriptor = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0)
    descriptor = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) return false;

  const std::array<int, 4> event_types = {EV_SYN, EV_KEY, EV_ABS, EV_MSC};
  for (int type : event_types)
    if (ioctl(descriptor, UI_SET_EVBIT, type) != 0) {
      close(descriptor);
      return false;
    }
  if (ioctl(descriptor, UI_SET_KEYBIT, BTN_TOUCH) != 0) {
    close(descriptor);
    return false;
  }
  for (int key : {KEY_BACK, KEY_HOME, KEY_MENU, KEY_POWER, KEY_VOLUMEUP,
                  KEY_VOLUMEDOWN, KEY_ENTER, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT})
    ioctl(descriptor, UI_SET_KEYBIT, key);
  for (int axis : {ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X,
                   ABS_MT_POSITION_Y})
    if (ioctl(descriptor, UI_SET_ABSBIT, axis) != 0) {
      close(descriptor);
      return false;
    }

  const int width = std::max(1, gr_fb_width());
  const int height = std::max(1, gr_fb_height());
  if (!ConfigureAxis(descriptor, ABS_MT_SLOT, 0) ||
      !ConfigureAxis(descriptor, ABS_MT_TRACKING_ID, 65535) ||
      !ConfigureAxis(descriptor, ABS_MT_POSITION_X, width - 1) ||
      !ConfigureAxis(descriptor, ABS_MT_POSITION_Y, height - 1)) {
    close(descriptor);
    return false;
  }

  uinput_setup setup{};
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0xAE12;
  setup.id.product = 0x0001;
  setup.id.version = 1;
  std::strncpy(setup.name, "AERA Remote Input", UINPUT_MAX_NAME_SIZE - 1);
  if (ioctl(descriptor, UI_DEV_SETUP, &setup) != 0 ||
      ioctl(descriptor, UI_DEV_CREATE) != 0) {
    close(descriptor);
    return false;
  }
  device->descriptor = descriptor;
  return true;
}

int KeyCode(const std::string& name) {
  if (name == "back") return KEY_BACK;
  if (name == "home") return KEY_HOME;
  if (name == "menu") return KEY_MENU;
  if (name == "power") return KEY_POWER;
  if (name == "volume_up") return KEY_VOLUMEUP;
  if (name == "volume_down") return KEY_VOLUMEDOWN;
  if (name == "enter") return KEY_ENTER;
  if (name == "up") return KEY_UP;
  if (name == "down") return KEY_DOWN;
  if (name == "left") return KEY_LEFT;
  if (name == "right") return KEY_RIGHT;
  return -1;
}

}  // namespace

bool Touch(const std::string& action, int x, int y) {
  auto& device = Shared();
  std::lock_guard<std::mutex> guard(device.lock);
  if (!Open(&device)) return false;
  x = std::clamp(x, 0, std::max(0, gr_fb_width() - 1));
  y = std::clamp(y, 0, std::max(0, gr_fb_height() - 1));

  if (action == "down") {
    if (device.touching) return false;
    Emit(device.descriptor, EV_ABS, ABS_MT_SLOT, 0);
    Emit(device.descriptor, EV_ABS, ABS_MT_TRACKING_ID, device.contact++ & 0xffff);
    Emit(device.descriptor, EV_ABS, ABS_MT_POSITION_X, x);
    Emit(device.descriptor, EV_ABS, ABS_MT_POSITION_Y, y);
    Emit(device.descriptor, EV_KEY, BTN_TOUCH, 1);
    device.touching = true;
    return Sync(device.descriptor);
  }
  if (action == "move") {
    if (!device.touching) return false;
    Emit(device.descriptor, EV_ABS, ABS_MT_SLOT, 0);
    Emit(device.descriptor, EV_ABS, ABS_MT_POSITION_X, x);
    Emit(device.descriptor, EV_ABS, ABS_MT_POSITION_Y, y);
    return Sync(device.descriptor);
  }
  if (action == "up") {
    if (!device.touching) return true;
    Emit(device.descriptor, EV_ABS, ABS_MT_SLOT, 0);
    Emit(device.descriptor, EV_ABS, ABS_MT_TRACKING_ID, -1);
    Emit(device.descriptor, EV_KEY, BTN_TOUCH, 0);
    device.touching = false;
    return Sync(device.descriptor);
  }
  return false;
}

bool Key(const std::string& name) {
  const int code = KeyCode(name);
  if (code < 0) return false;
  auto& device = Shared();
  std::lock_guard<std::mutex> guard(device.lock);
  if (!Open(&device)) return false;
  const bool pressed = Emit(device.descriptor, EV_KEY, code, 1) && Sync(device.descriptor);
  const bool released = Emit(device.descriptor, EV_KEY, code, 0) && Sync(device.descriptor);
  return pressed && released;
}

void Shutdown() {
  auto& device = Shared();
  std::lock_guard<std::mutex> guard(device.lock);
  if (device.descriptor < 0) return;
  ioctl(device.descriptor, UI_DEV_DESTROY);
  close(device.descriptor);
  device.descriptor = -1;
  device.touching = false;
}

}  // namespace aera::remote::input
