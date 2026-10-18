/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <cstring>

namespace aeraui::recorder {

constexpr uint32_t kMagic = 0x41525231U;
constexpr uint32_t kFrameSlots = 2;
constexpr uint32_t kMaximumDimension = 4096;
constexpr uint64_t kMaximumFrameBytes =
    static_cast<uint64_t>(kMaximumDimension) * kMaximumDimension * 4U;

enum class Kind : uint32_t {
  kStart = 1, kFrame, kStop, kClose,
  kAck = 32, kStatus, kError, kDone,
};

struct Message {
  uint32_t magic = kMagic;
  Kind kind = Kind::kStatus;
  uint32_t sequence = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t fps = 0;
  uint32_t data_bytes = 0;
  uint64_t timestamp_ns = 0;
  uint64_t frames = 0;
  uint64_t dropped = 0;
  char text[512]{};
};

static_assert(sizeof(Message) == 568);

}  // namespace aeraui::recorder

