/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "protocol.hpp"
#include <string>

namespace aeraui::retro {
class Session {
 public:
  ~Session() { Close(); Unmap(); }
  Session() = default;
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  bool Adopt(int frame_fd, int control_fd);
  bool Send(Kind kind, int x = 0, int y = 0, uint32_t value = 0);
  bool Poll();
  bool AcknowledgeFrame();
  void Close();
  bool Connected() const { return control_ >= 0; }
  const uint8_t *Pixels() const {
    return shared_ ? shared_ + FrameSlot(sequence_) * kFrameBytes : nullptr;
  }
  const std::string &Status() const { return status_; }

 private:
  bool Write(const Message &message);
  void Fail(const char *message);
  void Unmap();
  int control_ = -1;
  const uint8_t *shared_ = nullptr;
  std::string status_;
  uint32_t sequence_ = 0;
  bool frame_pending_ = false;
};
}  // namespace aeraui::retro
