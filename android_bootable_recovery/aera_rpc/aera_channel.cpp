/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "aera_channel.hpp"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "aera_dispatcher.hpp"
#include "aera_protocol.hpp"
#include "../data.hpp"
#include "aeraui/platform/aera_ui_host.hpp"
#include "../orscmd/orscmd.h"
#include "../twcommon.h"

namespace aera::rpc {
namespace {

constexpr size_t kMaximumRequestBytes = 1024U * 1024U;
int g_input = -1;
int g_cancel = -1;
std::string g_pending;

void CloseInput() {
  if (g_input >= 0) close(g_input);
  g_input = -1;
}

void OpenInput() {
  CloseInput();
  unlink(AERA_RPC_INPUT_FILE);
  unlink(AERA_RPC_OUTPUT_FILE);
  if (mkfifo(AERA_RPC_INPUT_FILE, 0660) != 0 ||
      mkfifo(AERA_RPC_OUTPUT_FILE, 0666) != 0) {
    LOGERR("AERA RPC: could not create command channels: %s\n", strerror(errno));
    return;
  }
  g_input = open(AERA_RPC_INPUT_FILE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (g_input < 0)
    LOGERR("AERA RPC: could not open %s: %s\n", AERA_RPC_INPUT_FILE, strerror(errno));
  set_select_fd();
}

void OpenCancel() {
  if (g_cancel >= 0) close(g_cancel);
  g_cancel = -1;
  unlink(AERA_RPC_CANCEL_FILE);
  if (mkfifo(AERA_RPC_CANCEL_FILE, 0660) != 0) {
    LOGERR("AERA RPC: could not create cancellation channel: %s\n", strerror(errno));
    return;
  }
  g_cancel = open(AERA_RPC_CANCEL_FILE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (g_cancel < 0)
    LOGERR("AERA RPC: could not open %s: %s\n", AERA_RPC_CANCEL_FILE, strerror(errno));
  set_select_fd();
}

void RespondFailure(const Request &request, const std::string &code,
                    const std::string &message, int result) {
  FILE *output = std::fopen(AERA_RPC_OUTPUT_FILE, "w");
  if (!output) return;
  WriteError(output, request.id, code, message);
  WriteResult(output, request.id, result);
  std::fclose(output);
}

}  // namespace

void Channel::Setup() {
  if (g_input < 0) OpenInput();
  if (g_cancel < 0) OpenCancel();
}

void Channel::Shutdown() {
  CloseInput();
  if (g_cancel >= 0) close(g_cancel);
  g_cancel = -1;
  g_pending.clear();
  set_select_fd();
}

void Channel::Rearm() {
  g_pending.clear();
  OpenInput();
}

int Channel::InputFd() { return g_input; }
int Channel::CancelFd() { return g_cancel; }

void Channel::HandleInput() {
  if (g_input < 0) return;
  char buffer[8192];
  bool complete = false;
  for (;;) {
    const ssize_t count = read(g_input, buffer, sizeof(buffer));
    if (count > 0) {
      g_pending.append(buffer, static_cast<size_t>(count));
      if (g_pending.size() > kMaximumRequestBytes) {
        complete = true;
        break;
      }
      continue;
    }
    if (count == 0) complete = true;
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  if (!complete) return;
  CloseInput();
  set_select_fd();

  Request request = ParseRequest(g_pending);
  g_pending.clear();
  if (!request.valid()) {
    RespondFailure(request, request.error, "Invalid AERA RPC request", 2);
    Rearm();
    return;
  }
  if (Dispatcher::Active() || DataManager::GetIntValue("tw_busy") != 0) {
    RespondFailure(request, "busy", "Another recovery operation is active", 1);
    Rearm();
    return;
  }
  FILE *output = std::fopen(AERA_RPC_OUTPUT_FILE, "w");
  if (!output) {
    LOGERR("AERA RPC: could not open response channel: %s\n", strerror(errno));
    Rearm();
    return;
  }
  if (!Dispatcher::Start(output, request)) {
    WriteError(output, request.id, "dispatcher_unavailable",
               "The AERA command dispatcher could not start");
    WriteResult(output, request.id, 1);
    std::fclose(output);
    Rearm();
  }
}

bool Channel::HandleCancel() {
  if (g_cancel < 0) return false;
  char bytes[64];
  const ssize_t count = read(g_cancel, bytes, sizeof(bytes));
  if (count <= 0) {
    OpenCancel();
    return false;
  }
  const bool cancelled = Dispatcher::Cancel();
  OpenCancel();
  return cancelled;
}

}  // namespace aera::rpc
