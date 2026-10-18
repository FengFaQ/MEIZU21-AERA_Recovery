/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "protocol.hpp"
#include <string>

namespace aeraui::media {
class Session {
 public:
  ~Session() { Close(); Unmap(); }
  bool Adopt(int frame_fd, int control_fd);
  bool Send(Kind kind, const char *text = nullptr, int value = 0);
  bool Poll();
  bool AcknowledgeFrame();
  void Close();
  bool Connected() const { return control_ >= 0; }
  const uint8_t *Pixels() const {
    return shared_ ? shared_ + FrameSlot(sequence_) * kFrameBytes : nullptr;
  }
  const std::string &Status() const { return status_; }
  int64_t Position() const { return position_ms_; }
  int64_t Duration() const { return duration_ms_; }
  bool Playing() const { return playing_; }
  int FrameWidth() const { return frame_width_; }
  int FrameHeight() const { return frame_height_; }
  Kind FrameKind() const { return frame_kind_; }
  int32_t FrameToken() const { return frame_token_; }

 private:
  bool Write(const Message &message);
  void Fail(const char *message);
  void Unmap();
  int control_ = -1;
  const uint8_t *shared_ = nullptr;
  std::string status_;
  uint32_t sequence_ = 0;
  int64_t position_ms_ = 0;
  int64_t duration_ms_ = 0;
  int frame_width_ = kLandscapeWidth;
  int frame_height_ = kLandscapeHeight;
  Kind frame_kind_ = Kind::kFrame;
  int32_t frame_token_ = 0;
  bool frame_pending_ = false;
  bool playing_ = false;
};
}  // namespace aeraui::media
