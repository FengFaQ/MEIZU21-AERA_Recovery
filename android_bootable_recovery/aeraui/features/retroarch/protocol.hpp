/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <cstring>

namespace aeraui::retro {
constexpr uint32_t kMagic = 0x41525241U;
constexpr int kWidth = 720;
constexpr int kHeight = 1584;
constexpr uint32_t kFrameBytes = kWidth * kHeight * 4U;
constexpr uint32_t kFrameSlots = 2;
constexpr uint32_t kSharedBytes = kFrameBytes * kFrameSlots;
constexpr uint32_t FrameSlot(uint32_t sequence) {
  return sequence % kFrameSlots;
}
enum class Kind : uint32_t {
  kTouchDown = 1, kTouchMove, kTouchUp, kKey, kClose, kAck,
  kFrame = 32, kStatus, kError
};
struct Message {
  uint32_t magic = kMagic;
  Kind kind = Kind::kStatus;
  uint32_t sequence = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t value = 0;
  char text[128]{};
};
static_assert(sizeof(Message) == 152);
inline bool Valid(const Message &message, bool from_worker) {
  if (message.magic != kMagic ||
      !memchr(message.text, '\0', sizeof(message.text))) return false;
  if (from_worker)
    return (message.kind == Kind::kFrame && message.x == kWidth &&
            message.y == kHeight && message.value == kFrameBytes) ||
           message.kind == Kind::kStatus || message.kind == Kind::kError;
  if (message.kind == Kind::kTouchDown || message.kind == Kind::kTouchMove ||
      message.kind == Kind::kTouchUp)
    return message.x >= 0 && message.y >= 0 &&
           message.x < kWidth && message.y < kHeight;
  return message.kind == Kind::kKey || message.kind == Kind::kClose ||
         message.kind == Kind::kAck;
}
}  // namespace aeraui::retro
