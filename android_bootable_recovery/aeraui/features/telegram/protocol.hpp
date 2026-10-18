// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <cstring>

namespace aeraui::telegram {
constexpr uint32_t kMagic = 0x4154474dU;
constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kTextBytes = 2048;
enum class Kind : uint32_t {
  kConfigure = 1, kPhone, kCode, kPassword, kEmail, kEmailCode,
  kRegister, kLoadChats, kOpenChat, kSendText, kClose, kSendFile,
  kDownloadFile, kEditMessage, kDeleteMessage,
  kState = 64, kStatus, kError, kChat, kChatsDone, kMessage, kMessagesDone,
  kAttachment, kFileProgress, kFileReady, kPhotoReady, kAvatarReady,
  kMessageStatus,
  kMessageDeleted,
};
enum class AuthState : uint32_t {
  kStarting = 0, kNeedConfiguration, kNeedPhone, kNeedCode, kNeedPassword,
  kNeedEmail, kNeedEmailCode, kNeedRegistration, kConfirmElsewhere, kReady,
  kClosing,
  kNeedVault,
};
struct Message {
  uint32_t magic = kMagic;
  uint32_t version = kProtocolVersion;
  Kind kind = Kind::kStatus;
  uint32_t value = 0;
  int64_t primary = 0;
  int64_t secondary = 0;
  char text[kTextBytes]{};
};
static_assert(sizeof(Message) == 2080);
inline bool Valid(const Message &message, bool from_worker) {
  if (message.magic != kMagic || message.version != kProtocolVersion ||
      !memchr(message.text, '\0', sizeof(message.text))) return false;
  if (from_worker)
    return message.kind >= Kind::kState &&
        message.kind <= Kind::kMessageDeleted;
  return message.kind >= Kind::kConfigure && message.kind <= Kind::kDeleteMessage;
}
}  // namespace aeraui::telegram
