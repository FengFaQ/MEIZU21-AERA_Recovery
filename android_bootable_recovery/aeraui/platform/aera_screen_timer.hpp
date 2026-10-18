// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pthread.h>
#include <time.h>

#include <string>

class AeraScreenTimer {
 public:
  AeraScreenTimer();

  void SetTimeout(int seconds);
  void CheckTimeout();
  void Wake();
  void Toggle();
  void Blank();
  bool IsScreenOff();
  void SetAmbientEnabled(bool enabled, int brightness = 2);

 private:
  enum class State { kOn, kDim, kOff, kBlanked, kAmbient };
  bool ambient_enabled_ = false;
  int ambient_brightness_ = 2;
  std::string AmbientBrightness();

  void ResetClock();
  std::string CurrentBrightness();

  pthread_mutex_t mutex_{};
  State state_ = State::kOn;
  timespec started_{};
  int timeout_seconds_ = 0;
  std::string original_brightness_;
};

extern AeraScreenTimer aeraScreenTimer;
