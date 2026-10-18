// SPDX-License-Identifier: Apache-2.0
#include "session.hpp"
#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace aeraui::web;
int main() {
  int channel[2]; assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) == 0);
  int frame = memfd_create("aera-test-pixels", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  assert(frame >= 0 && ftruncate(frame, kSharedBytes) == 0);
  auto *pixels = static_cast<uint8_t *>(mmap(nullptr, kSharedBytes, PROT_READ | PROT_WRITE, MAP_SHARED, frame, 0));
  assert(pixels != MAP_FAILED);
  memset(pixels + FrameSlot(1) * kFrameBytes, 0x42, kFrameBytes);
  assert(fcntl(frame, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0);
  Session session; assert(session.Adopt(frame, channel[0]));
  assert(!session.Poll()); // An idle browser never blocks UI rendering.
  Message message; message.kind = Kind::kFrame; message.sequence = 1;
  message.x = kWidth; message.y = kHeight; message.value = kFrameBytes;
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(session.Poll()); assert(session.Pixels()[0] == 0x42 && session.Pixels()[kFrameBytes - 1] == 0x42);
  Message ack; assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) < 0);
  assert(session.AcknowledgeFrame());
  assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) == sizeof(ack));
  assert(ack.kind == Kind::kAck && ack.sequence == 1);
  memset(pixels + FrameSlot(2) * kFrameBytes, 0x24, kFrameBytes);
  assert(session.Pixels()[0] == 0x42); // The alternate slot cannot alter the displayed frame.
  message = Message{}; message.value = 51; message.x = 1;
  strcpy(message.text, "https://example.org/");
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(!session.Poll()); assert(session.Progress() == 51 && session.CanBack());
  message = Message{}; message.kind = Kind::kKeyboardShow; message.value = 2;
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(!session.Poll()); uint32_t purpose = 0;
  assert(session.TakeKeyboardRequest(&purpose) == KeyboardRequest::kShow && purpose == 2);
  assert(session.TakeKeyboardRequest() == KeyboardRequest::kNone);
  message = Message{}; message.kind = Kind::kKeyboardHide;
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(!session.Poll());
  assert(session.TakeKeyboardRequest() == KeyboardRequest::kHide);
  assert(session.Send(Kind::kTouchDown, 10, 20));
  assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) == sizeof(ack));
  assert(ack.kind == Kind::kTouchDown && ack.x == 10 && ack.y == 20);
  assert(!session.Send(Kind::kTouchDown, kViewWidth, 0));
  assert(session.Send(Kind::kTouchDown, 10, 20, 1));
  assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) == sizeof(ack));
  assert(ack.kind == Kind::kTouchDown && ack.value == 1);
  assert(session.SetZoom(175));
  assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) == sizeof(ack));
  assert(ack.kind == Kind::kSetZoom && ack.value == 175);
  assert(session.CancelDownload(7));
  assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) == sizeof(ack));
  assert(ack.kind == Kind::kDownloadCancel && ack.sequence == 7);
  assert(session.Send(Kind::kSetCookiePolicy, 0, 0, 1));
  assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) == sizeof(ack));
  assert(ack.kind == Kind::kSetCookiePolicy && ack.value == 1);
  assert(session.Send(Kind::kClearBrowsingData));
  assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) == sizeof(ack));
  assert(ack.kind == Kind::kClearBrowsingData);
  message = Message{}; message.kind = Kind::kBrowsingDataCleared;
  strcpy(message.text, "Cookies and site data cleared.");
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(!session.Poll());
  assert(session.SettingsRevision() == 1);
  assert(session.SettingsNotice() == "Cookies and site data cleared.");
  message = Message{}; message.kind = Kind::kDownloadStarted;
  message.sequence = 7; message.x = 4; message.y = 2048; message.value = 80;
  strcpy(message.text, "recovery.zip");
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(!session.Poll()); assert(session.Downloads().size() == 1);
  assert(session.Downloads()[0].name == "recovery.zip");
  assert(session.Downloads()[0].total_bytes == 2ULL * 1024 * 1024);
  assert(CurrentDownloadSummary().active_count == 1);
  message.kind = Kind::kDownloadFinished; message.x = 100;
  message.value = message.y;
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(!session.Poll());
  assert(session.Downloads()[0].status == DownloadStatus::kFinished);
  assert(CurrentDownloadSummary().active_count == 0);
  message = Message{}; message.kind = Kind::kFrame; message.sequence = 2;
  message.x = kWidth; message.y = kHeight; message.value = kFrameBytes;
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(session.Poll()); assert(session.Pixels()[0] == 0x24);
  assert(session.AcknowledgeFrame());
  assert(recv(channel[1], &ack, sizeof(ack), MSG_DONTWAIT) == sizeof(ack));
  assert(ack.kind == Kind::kAck && ack.sequence == 2);
  message = Message{}; message.kind = Kind::kFrame; message.sequence = 3; message.x = kWidth;
  message.y = kHeight; message.value = kFrameBytes + 4;
  assert(send(channel[1], &message, sizeof(message), 0) == sizeof(message));
  assert(!session.Poll()); assert(!session.Connected());
  close(channel[1]); munmap(pixels, kSharedBytes);
  // Refuse an unsealed mapping: its peer could otherwise truncate under mmap.
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) == 0);
  frame = memfd_create("aera-test-unsealed", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  assert(frame >= 0 && ftruncate(frame, kSharedBytes) == 0);
  assert(!session.Adopt(frame, channel[0])); close(channel[1]);
  puts("PASS: nonblocking double-buffer IPC, delayed ACK, input, keyboard focus, bad frame rejection and required seals");
}
