/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdio>
#include <string>

#include <json/json.h>

namespace aera::rpc {

struct Request {
  int version = 0;
  std::string id;
  std::string operation;
  Json::Value arguments{Json::objectValue};
  std::string error;

  bool valid() const { return error.empty(); }
};

Request ParseRequest(const std::string &json);
void WriteLog(FILE *output, const std::string &id, const std::string &text);
void WriteData(FILE *output, const std::string &id, const std::string &name,
               const Json::Value &value);
void WriteProgress(FILE *output, const std::string &id,
                   const std::string &phase, int percent,
                   const Json::Value &details = Json::Value(Json::objectValue));
void WriteError(FILE *output, const std::string &id, const std::string &code,
                const std::string &message);
void WriteResult(FILE *output, const std::string &id, int code);
FILE *OpenLogStream(FILE *output, const std::string &id);

}  // namespace aera::rpc
