/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdio>
#include <string>

#include <json/json.h>

#include "aera_protocol.hpp"

namespace aera::rpc {

class Dispatcher {
 public:
  static bool Start(FILE *output, const Request &request);
  static int RunPending();
  static bool Active();
  static bool Cancel();
  static void Progress(const std::string &phase, int percent,
                       const Json::Value &details = Json::Value(Json::objectValue));
};

}  // namespace aera::rpc
