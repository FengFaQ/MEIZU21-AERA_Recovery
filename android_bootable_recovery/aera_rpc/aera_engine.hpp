/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <string>

#include <json/json.h>

#include "aera_protocol.hpp"

namespace aera::rpc {

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void Log(const std::string &text) = 0;
  virtual void Data(const std::string &name, const Json::Value &value) = 0;
  virtual void Error(const std::string &code, const std::string &message) = 0;
};

int Execute(const Request &request, EventSink &events);

}  // namespace aera::rpc
