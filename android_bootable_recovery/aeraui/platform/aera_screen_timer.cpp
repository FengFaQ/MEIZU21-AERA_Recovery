// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#include "aeraui/platform/aera_screen_timer.hpp"
#include <algorithm>

#include "data.hpp"
#include "twrp-functions.hpp"

#include "minuitwrp/minui.h"

AeraScreenTimer aeraScreenTimer;

AeraScreenTimer::AeraScreenTimer() {
  pthread_mutex_init(&mutex_, nullptr);
  ResetClock();
  original_brightness_ = CurrentBrightness();
}

void AeraScreenTimer::ResetClock() {
  clock_gettime(CLOCK_MONOTONIC, &started_);
}

void AeraScreenTimer::SetTimeout(int seconds) {
  pthread_mutex_lock(&mutex_);
  timeout_seconds_ = seconds;
  ResetClock();
  pthread_mutex_unlock(&mutex_);
}

bool AeraScreenTimer::IsScreenOff() {
  pthread_mutex_lock(&mutex_);
  const bool off = state_ == State::kOff || state_ == State::kBlanked || state_ == State::kAmbient;
  pthread_mutex_unlock(&mutex_);
  return off;
}
void AeraScreenTimer::SetAmbientEnabled(bool enabled, int brightness) {
  pthread_mutex_lock(&mutex_);
  ambient_enabled_=enabled;
  ambient_brightness_=std::clamp(brightness,1,100);
  pthread_mutex_unlock(&mutex_);
}
std::string AeraScreenTimer::AmbientBrightness() {
  const int maximum=std::max(1,DataManager::GetIntValue("tw_brightness_max"));
  return std::to_string(std::max(1,maximum*ambient_brightness_/100));
}

std::string AeraScreenTimer::CurrentBrightness() {
  std::string value;
  if (DataManager::GetIntValue("tw_has_brightnesss_file")) {
    DataManager::GetValue("tw_brightness", value);
    if (value.empty()) value = "255";
  }
  return value;
}

void AeraScreenTimer::CheckTimeout() {
#ifndef TW_NO_SCREEN_TIMEOUT
  pthread_mutex_lock(&mutex_);
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  const timespec elapsed = TWFunc::timespec_diff(started_, now);
  if (timeout_seconds_ > 2 && elapsed.tv_sec > timeout_seconds_ - 2 &&
      state_ == State::kOn) {
    original_brightness_ = CurrentBrightness();
    TWFunc::Set_Brightness("5");
    state_ = State::kDim;
  }
  if (timeout_seconds_ > 0 && elapsed.tv_sec > timeout_seconds_ &&
      (state_ == State::kOn || state_ == State::kDim)) {
    if(ambient_enabled_) {
      TWFunc::Set_Brightness(AmbientBrightness());
      state_=State::kAmbient;
      pthread_mutex_unlock(&mutex_);
      return;
    }
    TWFunc::Set_Brightness("0");
    TWFunc::check_and_run_script("/system/bin/postscreenblank.sh", "blank");
    state_ = State::kOff;
#ifndef TW_NO_SCREEN_BLANK
    gr_fb_blank(true);
    state_ = State::kBlanked;
#endif
  }
  pthread_mutex_unlock(&mutex_);
#endif
}

void AeraScreenTimer::Wake() {
#ifndef TW_NO_SCREEN_TIMEOUT
  pthread_mutex_lock(&mutex_);
  ResetClock();
  if (state_ == State::kBlanked) {
#ifndef TW_NO_SCREEN_BLANK
    gr_fb_blank(false);
#endif
    TWFunc::check_and_run_script("/system/bin/postscreenunblank.sh", "unblank");
  }
  if (state_ != State::kOn && !original_brightness_.empty())
    TWFunc::Set_Brightness(original_brightness_);
  state_ = State::kOn;
  pthread_mutex_unlock(&mutex_);
#endif
}

void AeraScreenTimer::Blank() {
#ifndef TW_NO_SCREEN_TIMEOUT
  pthread_mutex_lock(&mutex_);
  if (state_ == State::kOn || state_ == State::kDim) {
    original_brightness_ = CurrentBrightness();
    if(ambient_enabled_) {
      TWFunc::Set_Brightness(AmbientBrightness());
      state_=State::kAmbient;
      pthread_mutex_unlock(&mutex_);
      return;
    }
    TWFunc::Set_Brightness("0");
    TWFunc::check_and_run_script("/system/bin/postscreenblank.sh", "blank");
    state_ = State::kOff;
  }
#ifndef TW_NO_SCREEN_BLANK
  if (state_ == State::kOff) {
    gr_fb_blank(true);
    state_ = State::kBlanked;
  }
#endif
  pthread_mutex_unlock(&mutex_);
#endif
}

void AeraScreenTimer::Toggle() {
  if (IsScreenOff())
    Wake();
  else
    Blank();
}
