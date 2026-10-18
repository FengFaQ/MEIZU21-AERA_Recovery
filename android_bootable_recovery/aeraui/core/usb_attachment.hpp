/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <vector>

namespace aeraui {

// Enumerate physical USB disks, not each partition or USB input devices.
// Sysfs-only: no mounts, partition-manager access, or block-device reads.
inline std::set<std::string> UsbAttachments(const std::string &root = "/sys/class/block") {
  std::set<std::string> result;
  DIR *directory = opendir(root.c_str());
  if (!directory) return result;
  while (const auto *entry = readdir(directory)) {
    if (entry->d_name[0] == '.') continue;
    const std::string path = root + "/" + entry->d_name;
    char resolved[PATH_MAX];
    if (!realpath(path.c_str(), resolved) ||
        std::string(resolved).find("/usb") == std::string::npos ||
        access((path + "/partition").c_str(), F_OK) == 0) continue;
    std::string sequence;
    std::ifstream(path + "/diskseq") >> sequence;
    result.insert(std::string(resolved) + ":" + sequence);
  }
  closedir(directory);
  return result;
}

struct InputAttachments {
  std::set<std::string> mice, keyboards;
};

inline InputAttachments ExternalInputAttachments() {
  InputAttachments result;
  DIR *directory = opendir("/dev/input");
  if (!directory) return result;
  while (const auto *entry = readdir(directory)) {
    if (std::string(entry->d_name).find("event") != 0) continue;
    const std::string path = std::string("/dev/input/") + entry->d_name;
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    input_id id{};
    unsigned char keys[(KEY_MAX + 8) / 8]{};
    unsigned char relative[(REL_MAX + 8) / 8]{};
    const bool external = ioctl(fd, EVIOCGID, &id) == 0 &&
        (id.bustype == BUS_USB || id.bustype == BUS_BLUETOOTH);
    const bool have_keys = external && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0;
    const bool have_rel = external && ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relative)), relative) >= 0;
    char physical[256]{};
    ioctl(fd, EVIOCGPHYS(sizeof(physical) - 1), physical);
    close(fd);
    const auto bit = [](const unsigned char *bits, unsigned code) {
      return (bits[code / 8] & (1U << (code % 8))) != 0;
    };
    const std::string identity = path + ":" + physical;
    if (have_keys && have_rel && bit(keys, BTN_LEFT) &&
        bit(relative, REL_X) && bit(relative, REL_Y)) result.mice.insert(identity);
    if (have_keys && bit(keys, KEY_A) && bit(keys, KEY_Z) &&
        bit(keys, KEY_SPACE) && bit(keys, KEY_ENTER)) result.keyboards.insert(identity);
  }
  closedir(directory);
  return result;
}

struct AttachmentChanges {
  bool connected = false, disconnected = false;
  explicit operator bool() const { return connected || disconnected; }
};

class UsbAttachmentTracker {
 public:
  AttachmentChanges Update(const std::set<std::string> &current) {
    AttachmentChanges changes;
    if (initialized_) {
      for (const auto &disk : current)
        if (!previous_.count(disk)) changes.connected = true;
      for (const auto &disk : previous_)
        if (!current.count(disk)) changes.disconnected = true;
    }
    previous_ = current;
    initialized_ = true;
    return changes;
  }
 private:
  bool initialized_ = false;
  std::set<std::string> previous_;
};

}  // namespace aeraui
