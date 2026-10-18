/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <cstring>

namespace aeraui::media {
constexpr uint32_t kMagic = 0x414d5031U;
constexpr int kLandscapeWidth = 1280;
constexpr int kLandscapeHeight = 720;
constexpr int kMaxDimension = 1280;
constexpr uint32_t kMaxPixels = kLandscapeWidth * kLandscapeHeight;
constexpr uint32_t kFrameBytes = kMaxPixels * 4U;
constexpr uint32_t kFrameSlots = 2;
constexpr uint32_t kSharedBytes = kFrameBytes * kFrameSlots;
constexpr uint32_t FrameSlot(uint32_t sequence) { return sequence % kFrameSlots; }
constexpr uint32_t FrameBytes(int width, int height) {
  return width > 0 && height > 0
      ? static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4U : 0;
}
constexpr bool ValidFrame(int width, int height, uint32_t bytes) {
  return width > 0 && height > 0 && width <= kMaxDimension &&
      height <= kMaxDimension &&
      static_cast<uint32_t>(width) * static_cast<uint32_t>(height) <=
          kMaxPixels &&
      bytes == FrameBytes(width, height);
}

enum class Kind : uint32_t {
  kOpen = 1, kPlay, kPause, kSeekRelative, kClose, kAck, kThumbnail,
  kFrame = 32, kThumbnailFrame, kStatus, kError, kEnded
};

struct Message {
  uint32_t magic = kMagic;
  Kind kind = Kind::kStatus;
  uint32_t sequence = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t value = 0;
  int64_t position_ms = 0;
  int64_t duration_ms = 0;
  char text[512]{};
};
static_assert(sizeof(Message) == 552);

inline bool Valid(const Message &message, bool from_worker) {
  if (message.magic != kMagic ||
      !memchr(message.text, '\0', sizeof(message.text))) return false;
  if (from_worker)
    return ((message.kind == Kind::kFrame ||
             message.kind == Kind::kThumbnailFrame) &&
            ValidFrame(message.x, message.y, message.value)) ||
           message.kind == Kind::kStatus || message.kind == Kind::kError ||
           message.kind == Kind::kEnded;
  return message.kind == Kind::kOpen || message.kind == Kind::kPlay ||
         message.kind == Kind::kPause || message.kind == Kind::kSeekRelative ||
         message.kind == Kind::kClose || message.kind == Kind::kAck ||
         message.kind == Kind::kThumbnail;
}
}  // namespace aeraui::media
