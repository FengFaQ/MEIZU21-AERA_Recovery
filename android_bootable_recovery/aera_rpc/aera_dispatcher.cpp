/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "aera_dispatcher.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

#include <aeraui/backend.hpp>

#include "aera_channel.hpp"
#include "aera_engine.hpp"
#include "aeraui/platform/aera_ui_host.h"
#include "../partitions.hpp"

namespace aera::rpc {
namespace {

std::mutex g_lock;
FILE *g_output = nullptr;
FILE *g_log = nullptr;
Request g_request;
std::atomic<bool> g_active{false};
std::atomic<bool> g_cancelled{false};

void FlushLog() {
  if (g_log) std::fflush(g_log);
}

class SessionEvents final : public EventSink {
 public:
  void Log(const std::string &text) override {
    std::lock_guard<std::mutex> guard(g_lock);
    FlushLog();
    WriteLog(g_output, g_request.id, text);
  }

  void Data(const std::string &name, const Json::Value &value) override {
    std::lock_guard<std::mutex> guard(g_lock);
    FlushLog();
    WriteData(g_output, g_request.id, name, value);
  }

  void Error(const std::string &code, const std::string &message) override {
    std::lock_guard<std::mutex> guard(g_lock);
    FlushLog();
    WriteError(g_output, g_request.id, code, message);
  }
};

void CloseSession(int result) {
  std::lock_guard<std::mutex> guard(g_lock);
  gui_set_FILE(nullptr);
  if (g_log) {
    std::fclose(g_log);
    g_log = nullptr;
  }
  WriteResult(g_output, g_request.id, result);
  if (g_output) {
    std::fclose(g_output);
    g_output = nullptr;
  }
  g_request = Request{};
  g_cancelled.store(false, std::memory_order_release);
  g_active.store(false, std::memory_order_release);
}

}  // namespace

bool Dispatcher::Start(FILE *output, const Request &request) {
  if (!output || !request.valid()) return false;
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_active.load(std::memory_order_acquire)) return false;
  g_output = output;
  g_request = request;
  g_log = OpenLogStream(g_output, g_request.id);
  if (!g_log) {
    g_output = nullptr;
    g_request = Request{};
    return false;
  }
  g_cancelled.store(false, std::memory_order_release);
  g_active.store(true, std::memory_order_release);
  gui_set_FILE(g_log);

  // AERA's native UI does not run the legacy XML action-page dispatcher.
  // Execute protocol requests on their own worker so the render/input loop
  // remains responsive while recovery operations are in progress.
  std::thread([] { Dispatcher::RunPending(); }).detach();
  return true;
}

int Dispatcher::RunPending() {
  if (!g_active.load(std::memory_order_acquire)) return 2;
  Request request;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    request = g_request;
  }
  SessionEvents events;
  const int result = Execute(request, events);
  CloseSession(result);
  Channel::Rearm();
  return result;
}

bool Dispatcher::Active() {
  return g_active.load(std::memory_order_acquire);
}

bool Dispatcher::Cancel() {
  if (!Active()) return false;
  g_cancelled.store(true, std::memory_order_release);
  const bool sideload = aeraui::RecoveryCancelSideload();
  const int backup = PartitionManager.Cancel_Backup();
  std::lock_guard<std::mutex> guard(g_lock);
  FlushLog();
  WriteLog(g_output, g_request.id, "Cancellation requested\n");
  return sideload || backup == 0 || Active();
}

void Dispatcher::Progress(const std::string &phase, int percent,
                          const Json::Value &details) {
  if (!Active()) return;
  std::lock_guard<std::mutex> guard(g_lock);
  FlushLog();
  WriteProgress(g_output, g_request.id, phase, percent, details);
}

}  // namespace aera::rpc
