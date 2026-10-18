// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#include "aeraui/platform/aera_ui_host.h"
#include "aeraui/platform/aera_ui_host.hpp"

#include <stdarg.h>

#include <mutex>
#include <string>

#include "aera_rpc/aera_dispatcher.hpp"

namespace {

constexpr size_t kLogBufferSize = 1024;
std::mutex gLogMutex;
FILE* gCommandOutput = nullptr;

void Write(const char* color, const char* format, va_list arguments) {
  char buffer[kLogBufferSize];
  vsnprintf(buffer, sizeof(buffer), format, arguments);
  std::lock_guard<std::mutex> lock(gLogMutex);
  fputs(buffer, stdout);
  if (gCommandOutput != nullptr) {
    fputs(buffer, gCommandOutput);
    fflush(gCommandOutput);
  }
  (void)color;
}

}  // namespace

extern "C" void gui_print(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  Write("normal", format, arguments);
  va_end(arguments);
}

extern "C" void gui_print_color(const char* color, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  Write(color, format, arguments);
  va_end(arguments);
}

extern "C" void gui_set_FILE(FILE* file) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  gCommandOutput = file;
}

extern "C" void gui_aera_progress_overall(int percent) {
  aera::rpc::Dispatcher::Progress("overall", percent);
}

extern "C" void gui_aera_progress_item(int percent) {
  aera::rpc::Dispatcher::Progress("item", percent);
}

extern "C" void gui_aera_progress_detail(
    const char* phase, int percent, const char* label,
    unsigned long long current_bytes, unsigned long long total_bytes,
    unsigned long long bytes_per_second, unsigned long long eta_seconds,
    unsigned long long current_files, unsigned long long total_files,
    const char* size_text, const char* file_text) {
  Json::Value details(Json::objectValue);
  details["label"] = label ? label : "";
  details["current_bytes"] = Json::UInt64(current_bytes);
  details["total_bytes"] = Json::UInt64(total_bytes);
  details["bytes_per_second"] = Json::UInt64(bytes_per_second);
  details["eta_seconds"] = Json::UInt64(eta_seconds);
  details["current_files"] = Json::UInt64(current_files);
  details["total_files"] = Json::UInt64(total_files);
  details["size_text"] = size_text ? size_text : "";
  details["file_text"] = file_text ? file_text : "";
  aera::rpc::Dispatcher::Progress(phase ? phase : "overall", percent, details);
}

void gui_msg(const char* text) {
  if (text != nullptr) gui_msg(Msg(text));
}

void gui_warn(const char* text) {
  if (text != nullptr) gui_msg(Msg(msg::kWarning, text));
}

void gui_err(const char* text) {
  if (text != nullptr) gui_msg(Msg(msg::kError, text));
}

void gui_highlight(const char* text) {
  if (text != nullptr) gui_msg(Msg(msg::kHighlight, text));
}

void gui_msg(Message message) {
  const std::string output = static_cast<std::string>(message) + "\n";
  std::lock_guard<std::mutex> lock(gLogMutex);
  fputs(output.c_str(), stdout);
  if (gCommandOutput != nullptr) {
    fputs(output.c_str(), gCommandOutput);
    fflush(gCommandOutput);
  }
}

void gui_err(Message message) { gui_msg(message); }
