// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sys/types.h>
#include "../../core/operation_affinity.hpp"
#include <unistd.h>

namespace aeraui::audio {

inline pid_t StartBridge() {
  constexpr const char* kDeviceBridge = "/system/bin/aera-audio-bridge";
  constexpr const char* kNullBridge = "/system/bin/aera-audio-null-bridge";
  const char* bridge = !access(kDeviceBridge, X_OK) ? kDeviceBridge : kNullBridge;
  if (access(bridge, X_OK)) return -1;

  const pid_t child = aeraui::operation_affinity::ForkWorker();
  if (!child) {
    execl(bridge, bridge, "--browser-audio", nullptr);
    _exit(78);
  }
  return child < 0 ? -1 : child;
}

}  // namespace aeraui::audio
