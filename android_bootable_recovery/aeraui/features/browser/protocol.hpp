// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace aeraui::web {
// One SOCK_SEQPACKET connection plus two fixed-size shared pixel buffers. No
// pointers, file paths, shell commands or recovery operations cross this ABI.
constexpr uint32_t kMagic = 0x41525032;
// Keep responsive layout at a normal 360 CSS-pixel handset width, but render
// it at a sharp 3x phone density. Server-side mobile selection comes from the
// Android Mobile user agent; this backing resolution only controls sharpness.
constexpr int kWidth = 1080, kHeight = 2100;
constexpr int kViewWidth = 360, kViewHeight = 700;
constexpr double kDeviceScale = 3.0;
constexpr uint32_t kFrameBytes = kWidth * kHeight * 4;
constexpr uint32_t kFrameSlots = 2;
constexpr uint32_t kSharedBytes = kFrameBytes * kFrameSlots;
constexpr uint32_t FrameSlot(uint32_t sequence) {
  return sequence % kFrameSlots;
}
enum class Kind : uint32_t { kOpen = 1, kBack, kForward, kReload, kStop,
  kTouchDown, kTouchMove, kTouchUp, kKey, kAck, kClose, kSetZoom,
  kDownloadCancel, kSetCookiePolicy, kClearBrowsingData,
  kFrame = 32, kStatus, kError, kKeyboardShow, kKeyboardHide,
  kDownloadStarted, kDownloadProgress, kDownloadFinished,
  kDownloadFailed, kDownloadCancelled, kBrowsingDataCleared,
  kBrowsingDataFailed };
struct Message {
  uint32_t magic = kMagic;
  Kind kind = Kind::kStatus;
  uint32_t sequence = 0;
  int32_t x = 0, y = 0;
  uint32_t value = 0;
  char text[2048]{};
};
static_assert(sizeof(Message) == 2072);
inline bool Valid(const Message &m, bool from_worker) {
  if (m.magic != kMagic || !memchr(m.text, '\0', sizeof(m.text))) return false;
  if (from_worker)
    return (m.kind == Kind::kFrame && m.value == kFrameBytes &&
           m.x == kWidth && m.y == kHeight) ||
           (m.kind == Kind::kStatus && m.value <= 100 &&
            m.x >= 0 && m.x <= 1 && m.y >= 0 && m.y <= 1) ||
           m.kind == Kind::kError ||
           (m.kind == Kind::kKeyboardShow && m.value <= 10) ||
           m.kind == Kind::kKeyboardHide ||
           ((m.kind == Kind::kDownloadStarted ||
             m.kind == Kind::kDownloadProgress ||
             m.kind == Kind::kDownloadFinished ||
             m.kind == Kind::kDownloadFailed ||
             m.kind == Kind::kDownloadCancelled) &&
            m.sequence > 0 && m.x >= 0 && m.x <= 100 && m.y >= 0) ||
           m.kind == Kind::kBrowsingDataCleared ||
           m.kind == Kind::kBrowsingDataFailed;
  switch (m.kind) {
    case Kind::kTouchDown: case Kind::kTouchMove: case Kind::kTouchUp:
      return m.x >= 0 && m.y >= 0 && m.x < kViewWidth &&
             m.y < kViewHeight && m.value < 2;
    case Kind::kSetZoom: return m.value >= 50 && m.value <= 300;
    case Kind::kDownloadCancel: return m.sequence > 0;
    case Kind::kSetCookiePolicy: return m.value <= 2;
    case Kind::kClearBrowsingData: return true;
    case Kind::kOpen: case Kind::kBack: case Kind::kForward: case Kind::kReload:
    case Kind::kStop: case Kind::kKey: case Kind::kAck: case Kind::kClose: return true;
    default: return false;
  }
}
// Deliberately no file:, javascript:, data:, shell handling or implicit search.
inline std::string Address(std::string value) {
  const auto start = value.find_first_not_of(' ');
  if (start == std::string::npos) return {};
  value = value.substr(start, value.find_last_not_of(' ') - start + 1);
  if (value.size() > 2040) return {};
  if (value == "aera://start" || value == "aera://test") return value;
  for (unsigned char c : value) if (c <= 32 || c == 127 || c == '\\') return {};
  if (value.find("://") == std::string::npos) {
    if (value.find(':') != std::string::npos) return {};
    value = "https://" + value;
  }
  size_t prefix = value.compare(0, 8, "https://") == 0 ? 8 :
                  value.compare(0, 7, "http://") == 0 ? 7 : 0;
  if (!prefix || value.size() >= 2048) return {};
  const auto authority = value.substr(prefix, value.find_first_of("/?#", prefix) - prefix);
  if (authority.empty() || authority.find('@') != std::string::npos) return {};
  return value;
}
}  // namespace aeraui::web
