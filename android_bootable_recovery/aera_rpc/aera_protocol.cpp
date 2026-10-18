/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "aera_protocol.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace aera::rpc {
namespace {

constexpr size_t kMaximumRequestBytes = 1024U * 1024U;

std::string Serialize(const Json::Value &value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["commentStyle"] = "None";
  return Json::writeString(builder, value);
}

void Send(FILE *output, Json::Value event, const std::string &id) {
  if (!output) return;
  if (!id.empty()) event["id"] = id;
  const std::string line = Serialize(event);
  std::fwrite(line.data(), 1, line.size(), output);
  std::fputc('\n', output);
  std::fflush(output);
}

struct LogStream {
  FILE *output = nullptr;
  std::string id;
  std::string pending;
};

void FlushLines(LogStream *stream, bool flush_tail) {
  size_t newline;
  while ((newline = stream->pending.find('\n')) != std::string::npos) {
    WriteLog(stream->output, stream->id,
             stream->pending.substr(0, newline + 1));
    stream->pending.erase(0, newline + 1);
  }
  if (flush_tail && !stream->pending.empty()) {
    WriteLog(stream->output, stream->id, stream->pending);
    stream->pending.clear();
  }
}

int LogWrite(void *cookie, const char *data, int size) {
  auto *stream = static_cast<LogStream *>(cookie);
  if (!stream || !data || size <= 0) return 0;
  stream->pending.append(data, static_cast<size_t>(size));
  FlushLines(stream, false);
  return size;
}

int LogClose(void *cookie) {
  std::unique_ptr<LogStream> stream(static_cast<LogStream *>(cookie));
  if (stream) FlushLines(stream.get(), true);
  return 0;
}

}  // namespace

Request ParseRequest(const std::string &json) {
  Request request;
  if (json.empty()) {
    request.error = "empty_request";
    return request;
  }
  if (json.size() > kMaximumRequestBytes) {
    request.error = "request_too_large";
    return request;
  }
  Json::Value root;
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::string parse_error;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(json.data(), json.data() + json.size(), &root,
                     &parse_error) || !root.isObject()) {
    request.error = "invalid_json";
    return request;
  }
  if (!root["v"].isInt() || root["v"].asInt() != 1) {
    request.error = "unsupported_version";
    return request;
  }
  if (!root["op"].isString() || root["op"].asString().empty()) {
    request.error = "missing_operation";
    return request;
  }
  if (root.isMember("args") && !root["args"].isObject()) {
    request.error = "invalid_arguments";
    return request;
  }
  request.version = 1;
  request.id = root["id"].isString() ? root["id"].asString() : "";
  request.operation = root["op"].asString();
  request.arguments = root.isMember("args")
      ? root["args"] : Json::Value(Json::objectValue);
  return request;
}

void WriteLog(FILE *output, const std::string &id, const std::string &text) {
  Json::Value event(Json::objectValue);
  event["event"] = "log";
  event["text"] = text;
  Send(output, std::move(event), id);
}

void WriteData(FILE *output, const std::string &id, const std::string &name,
               const Json::Value &value) {
  Json::Value event(Json::objectValue);
  event["event"] = "data";
  event["name"] = name;
  event["value"] = value;
  Send(output, std::move(event), id);
}

void WriteProgress(FILE *output, const std::string &id,
                   const std::string &phase, int percent,
                   const Json::Value &details) {
  Json::Value event = details.isObject() ? details : Json::Value(Json::objectValue);
  event["event"] = "progress";
  event["phase"] = phase;
  event["percent"] = std::clamp(percent, 0, 100);
  Send(output, std::move(event), id);
}

void WriteError(FILE *output, const std::string &id, const std::string &code,
                const std::string &message) {
  Json::Value event(Json::objectValue);
  event["event"] = "error";
  event["code"] = code;
  event["message"] = message;
  Send(output, std::move(event), id);
}

void WriteResult(FILE *output, const std::string &id, int code) {
  Json::Value event(Json::objectValue);
  event["event"] = "result";
  event["code"] = code;
  Send(output, std::move(event), id);
}

FILE *OpenLogStream(FILE *output, const std::string &id) {
  auto stream = std::make_unique<LogStream>();
  stream->output = output;
  stream->id = id;
  FILE *file = funopen(stream.get(), nullptr, LogWrite, nullptr, LogClose);
  if (!file) return nullptr;
  stream.release();
  setvbuf(file, nullptr, _IONBF, 0);
  return file;
}

}  // namespace aera::rpc
