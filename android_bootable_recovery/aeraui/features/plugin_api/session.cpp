/* SPDX-License-Identifier: Apache-2.0 */
#include "session.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace aeraui::plugin_api {
namespace {
uint64_t NowMs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
}  // namespace

bool Session::Adopt(int control_fd) {
  Close();
  int type = 0;
  socklen_t length = sizeof(type);
  if (control_fd < 0 ||
      getsockopt(control_fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 ||
      type != SOCK_SEQPACKET) {
    if (control_fd >= 0) close(control_fd);
    status_ = "Invalid AERA plugin channel.";
    return false;
  }
  const int flags = fcntl(control_fd, F_GETFL);
  if (flags < 0 || fcntl(control_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    close(control_fd);
    status_ = "Could not protect the AERA plugin channel.";
    return false;
  }
  control_ = control_fd;
  negotiated_ = false;
  rate_window_ms_ = NowMs();
  rate_messages_ = 0;
  status_ = "Waiting for plugin handshake";
  return true;
}

bool Session::Write(const Message &message) {
  if (!Connected() || !Valid(message, false)) return false;
  const ssize_t count = send(control_, &message, sizeof(message),
                             MSG_DONTWAIT | MSG_NOSIGNAL);
  if (count == static_cast<ssize_t>(sizeof(message))) return true;
  Fail("The plugin stopped responding.");
  return false;
}

bool Session::Send(Kind kind, uint32_t request_id, uint32_t value,
                   uint32_t flags, const char *title, const char *text) {
  Message message;
  message.kind = kind;
  message.request_id = request_id;
  message.value = value;
  message.flags = flags;
  if (title) snprintf(message.title, sizeof(message.title), "%s", title);
  if (text) snprintf(message.text, sizeof(message.text), "%s", text);
  return Write(message);
}

std::vector<Message> Session::Poll() {
  std::vector<Message> messages;
  for (unsigned packet = 0; packet < 24 && Connected(); ++packet) {
    Message message;
    const ssize_t count = recv(control_, &message, sizeof(message),
                               MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count < 0 && errno == EINTR) continue;
    if (count != static_cast<ssize_t>(sizeof(message)) ||
        !Valid(message, true)) {
      Fail("The plugin closed or sent an invalid message.");
      break;
    }
    const uint64_t now = NowMs();
    if (now - rate_window_ms_ >= 1000) {
      rate_window_ms_ = now;
      rate_messages_ = 0;
    }
    if (++rate_messages_ > 128) {
      Fail("The plugin exceeded the message rate limit.");
      break;
    }
    if (!negotiated_) {
      // HELLO advertises the inclusive [minimum, maximum] protocol range.
      if (message.kind != Kind::kHello || message.value == 0 ||
          message.value > message.flags || message.value > kProtocolVersion ||
          message.flags < kProtocolVersion) {
        Fail("The plugin does not support AERA Host API 2.");
        break;
      }
      negotiated_ = true;
      status_ = "Plugin connected";
      if (!Send(Kind::kHelloAck, 0, kProtocolVersion,
                kFeatureMetrics | kFeatureBackNavigation) ||
          !Send(Kind::kLifecycle, 0,
                static_cast<uint32_t>(Lifecycle::kResume))) break;
      continue;
    }
    if (message.kind == Kind::kHello) {
      Fail("The plugin repeated its handshake.");
      break;
    }
    messages.push_back(message);
  }
  return messages;
}

void Session::Fail(const char *message) {
  status_ = message;
  if (control_ >= 0) close(control_);
  control_ = -1;
  negotiated_ = false;
}

void Session::Close() {
  if (control_ >= 0) close(control_);
  control_ = -1;
  negotiated_ = false;
  rate_window_ms_ = 0;
  rate_messages_ = 0;
}

}  // namespace aeraui::plugin_api
