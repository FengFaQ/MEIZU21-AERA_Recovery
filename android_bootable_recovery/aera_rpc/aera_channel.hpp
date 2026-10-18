/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace aera::rpc {

class Channel {
 public:
  static void Setup();
  static void Shutdown();
  static void Rearm();
  static int InputFd();
  static int CancelFd();
  static void HandleInput();
  static bool HandleCancel();
};

}  // namespace aera::rpc
