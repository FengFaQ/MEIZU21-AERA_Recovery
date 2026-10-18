/* SPDX-License-Identifier: Apache-2.0 */
#include "session.hpp"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aeraui::media {
void Session::Unmap() {
  if (shared_) munmap(const_cast<uint8_t *>(shared_), kSharedBytes);
  shared_ = nullptr;
}
void Session::Close() {
  if (control_ >= 0) close(control_);
  control_ = -1; sequence_ = 0; frame_pending_ = false;
  frame_width_ = kLandscapeWidth; frame_height_ = kLandscapeHeight;
  frame_kind_ = Kind::kFrame; frame_token_ = 0;
}
void Session::Fail(const char *message) { status_ = message; Close(); }
bool Session::Adopt(int frame_fd, int control_fd) {
  Close(); Unmap();
  struct stat info{}; int type = 0; socklen_t size = sizeof(type);
  const int seals = frame_fd >= 0 ? fcntl(frame_fd, F_GET_SEALS) : -1;
  const bool valid = frame_fd >= 0 && control_fd >= 0 && frame_fd != control_fd &&
      !fstat(frame_fd, &info) && S_ISREG(info.st_mode) &&
      info.st_size == kSharedBytes && seals >= 0 &&
      (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) ==
          (F_SEAL_SHRINK | F_SEAL_GROW) &&
      !getsockopt(control_fd, SOL_SOCKET, SO_TYPE, &type, &size) &&
      type == SOCK_SEQPACKET;
  if (!valid) {
    if (frame_fd >= 0) close(frame_fd);
    if (control_fd >= 0 && control_fd != frame_fd) close(control_fd);
    status_ = "Invalid AERA Media channel."; return false;
  }
  auto *mapped = mmap(nullptr, kSharedBytes, PROT_READ, MAP_SHARED, frame_fd, 0);
  close(frame_fd);
  if (mapped == MAP_FAILED) {
    close(control_fd); status_ = "Could not map media video frames."; return false;
  }
  shared_ = static_cast<uint8_t *>(mapped); control_ = control_fd;
  fcntl(control_, F_SETFL, fcntl(control_, F_GETFL) | O_NONBLOCK);
  status_ = "Media engine ready"; return true;
}
bool Session::Write(const Message &message) {
  if (!Connected()) return false;
  if (send(control_, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL) ==
      sizeof(message)) return true;
  Fail("AERA Media stopped responding."); return false;
}
bool Session::Send(Kind kind, const char *text, int value) {
  if (kind == Kind::kOpen) {
    // Retiring an outstanding slot before changing streams prevents an old
    // frame from colliding with the first frame produced by the new decoder.
    if (!AcknowledgeFrame()) return false;
    position_ms_ = 0;
    duration_ms_ = 0;
    playing_ = false;
  }
  Message message; message.kind = kind; message.x = value;
  if (text) snprintf(message.text, sizeof(message.text), "%s", text);
  return Valid(message, false) && Write(message);
}
bool Session::AcknowledgeFrame() {
  if (!frame_pending_) return Connected();
  Message message; message.kind = Kind::kAck; message.sequence = sequence_;
  if (!Write(message)) return false;
  frame_pending_ = false; return true;
}
bool Session::Poll() {
  bool changed = false;
  for (int i = 0; i < 12 && Connected(); ++i) {
    Message message;
    const auto count = recv(control_, &message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count < 0 && errno == EINTR) continue;
    if (count != sizeof(message) || !Valid(message, true)) {
      Fail("Media engine closed or sent an invalid response."); break;
    }
    if (message.kind == Kind::kFrame ||
        message.kind == Kind::kThumbnailFrame) {
      if (frame_pending_ || message.sequence != sequence_ + 1) {
        Fail("Media frame sequence mismatch."); break;
      }
      sequence_ = message.sequence;
      frame_width_ = message.x; frame_height_ = message.y;
      frame_kind_ = message.kind;
      frame_token_ = static_cast<int32_t>(message.position_ms);
      frame_pending_ = true; changed = true;
    } else {
      status_ = message.text;
      position_ms_ = message.position_ms;
      duration_ms_ = message.duration_ms;
      playing_ = message.value != 0;
    }
  }
  return changed;
}
}  // namespace aeraui::media
