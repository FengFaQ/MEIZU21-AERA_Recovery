// SPDX-License-Identifier: Apache-2.0
#include "session.hpp"
#include "../../../aera_shared_storage.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <mutex>

namespace aeraui::web {
namespace {
std::mutex g_download_mutex;
DownloadSummary g_download_summary;
constexpr gid_t kMediaRwGid = 1023;

uint64_t MonotonicMilliseconds() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool SafeDownloadLeaf(const std::string &name) {
  return !name.empty() && name != "." && name != ".." &&
      name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}
void NormalizeDownload(int directory, const std::string &name) {
  if (!SafeDownloadLeaf(name)) return;
  int file = openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info{};
  if (file < 0 || fstat(file, &info) || !S_ISREG(info.st_mode)) {
    if (file >= 0) close(file);
    return;
  }
  if (fchown(file, 0, kMediaRwGid) != 0) {
    // Best effort: the download remains usable by the recovery host.
  }
  fchmod(file, 0660);
  close(file);
}
void RestoreDownloadStorage(const std::string *single = nullptr) {
  int directory = open("/sdcard/AERA/Downloads",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0) return;
  if (single) {
    NormalizeDownload(directory, *single);
    aera::shared_storage::Normalize("/sdcard/AERA/Downloads/" + *single);
    // Keep the directory writable by the isolated browser for the remainder
    // of this session. Only the completed file returns to media_rw here;
    // closing Browser restores the directory and any remaining partial files.
    close(directory);
    return;
  } else {
    DIR *entries = fdopendir(dup(directory));
    if (entries) {
      while (auto *entry = readdir(entries))
        NormalizeDownload(directory, entry->d_name);
      closedir(entries);
    }
  }
  if (fchown(directory, 0, kMediaRwGid) != 0) {
    // Best effort: recovery cleanup still restores the directory mode.
  }
  fchmod(directory, 0770);
  close(directory);
  aera::shared_storage::Normalize("/sdcard/AERA/Downloads", true);
}
}

DownloadSummary CurrentDownloadSummary() {
  std::lock_guard<std::mutex> lock(g_download_mutex);
  return g_download_summary;
}

void Session::PublishDownloads() {
  DownloadSummary summary;
  if (!downloads_.empty()) {
    summary.available = true;
    const DownloadItem *featured = &downloads_.front();
    for (const auto &item : downloads_) {
      if (item.status == DownloadStatus::kActive) {
        ++summary.active_count;
        featured = &item;
      }
    }
    summary.progress = featured->progress;
    summary.received_bytes = featured->received_bytes;
    summary.total_bytes = featured->total_bytes;
    summary.speed_bytes_per_second = featured->speed_bytes_per_second;
    summary.status = featured->status;
    summary.name = featured->name;
  }
  std::lock_guard<std::mutex> lock(g_download_mutex);
  g_download_summary = std::move(summary);
}

void Session::Unmap() {
  if (shared_) munmap(const_cast<uint8_t *>(shared_), kSharedBytes);
  shared_ = nullptr;
}
void Session::ResetState() {
  if (control_ >= 0) close(control_);
  control_ = -1;
  can_back_ = can_forward_ = false; progress_ = 0; sequence_ = 0;
  keyboard_request_ = KeyboardRequest::kNone; keyboard_purpose_ = 0;
  frame_pending_ = false;
  downloads_.clear(); download_revision_ = 0; zoom_percent_ = 100;
  settings_notice_.clear(); settings_revision_ = 0;
  PublishDownloads();
}
void Session::Close() {
  ResetState();
  RestoreDownloadStorage();
}
void Session::Fail(const char *reason) { status_ = reason; Close(); }
bool Session::Adopt(int frame_fd, int control_fd) {
  struct stat info{}; int type = 0; socklen_t size = sizeof(type);
  const int seals = fcntl(frame_fd, F_GET_SEALS);
  const bool valid = frame_fd >= 0 && control_fd >= 0 && frame_fd != control_fd &&
      !fstat(frame_fd, &info) && S_ISREG(info.st_mode) && info.st_size == kSharedBytes &&
      seals >= 0 && (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) == (F_SEAL_SHRINK | F_SEAL_GROW) &&
      !getsockopt(control_fd, SOL_SOCKET, SO_TYPE, &type, &size) && type == SOCK_SEQPACKET;
  if (!valid) {
    if (frame_fd >= 0) close(frame_fd);
    if (control_fd >= 0 && control_fd != frame_fd) close(control_fd);
    status_ = "Invalid isolated browser channel."; return false;
  }
  // BrowserProcess::Start() has already prepared /sdcard/AERA/Downloads for
  // the isolated WebKit UID. Close() would restore media_rw ownership here,
  // before WebKit can create the destination. Adoption resets IPC state only;
  // storage is restored on a terminal download event or a real session close.
  if (Connected()) {
    close(frame_fd);
    close(control_fd);
    status_ = "Browser session is already connected.";
    return false;
  }
  ResetState();
  Unmap();
  auto *mapped = mmap(nullptr, kSharedBytes, PROT_READ, MAP_SHARED, frame_fd, 0);
  close(frame_fd);
  if (mapped == MAP_FAILED) { close(control_fd); status_ = "Could not map browser pixels."; return false; }
  shared_ = static_cast<uint8_t *>(mapped); control_ = control_fd;
  if (fcntl(control_, F_SETFD, FD_CLOEXEC) < 0) { Fail("Could not protect browser channel."); return false; }
  status_ = "Starting browser";
  return true;
}
bool Session::Write(const Message &message) {
  if (!Connected()) return false;
  const auto count = send(control_, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL);
  if (count == sizeof(message)) return true;
  Fail("Browser input channel stopped responding."); return false;
}
bool Session::Send(Kind kind, int x, int y, uint32_t value, const char *text) {
  Message message; message.kind = kind; message.x = x; message.y = y; message.value = value;
  if (!text || strlen(text) >= sizeof(message.text)) return false;
  memcpy(message.text, text, strlen(text) + 1);
  if (!Valid(message, false)) return false;
  return Write(message);
}
bool Session::SetZoom(unsigned percent) {
  percent = std::clamp(percent, 50U, 300U);
  if (!Send(Kind::kSetZoom, 0, 0, percent)) return false;
  zoom_percent_ = percent;
  return true;
}
bool Session::CancelDownload(uint32_t id) {
  Message message; message.kind = Kind::kDownloadCancel; message.sequence = id;
  return Valid(message, false) && Write(message);
}
bool Session::AcknowledgeFrame() {
  if (!frame_pending_) return Connected();
  Message ack; ack.kind = Kind::kAck; ack.sequence = sequence_;
  if (!Write(ack)) return false;
  frame_pending_ = false;
  return true;
}
KeyboardRequest Session::TakeKeyboardRequest(uint32_t *purpose) {
  const auto request = keyboard_request_;
  if (purpose) *purpose = keyboard_purpose_;
  keyboard_request_ = KeyboardRequest::kNone;
  return request;
}
bool Session::Poll() {
  bool changed = false;
  for (int i = 0; i < 8 && Connected(); ++i) {
    Message message;
    const auto count = recv(control_, &message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count < 0 && errno == EINTR) continue;
    if (count != sizeof(message) || !Valid(message, true)) {
      Fail("Browser closed or sent an invalid response."); break;
    }
    if (message.kind == Kind::kFrame) {
      if (frame_pending_ || message.sequence != sequence_ + 1) {
        Fail("Browser frame sequence mismatch."); break;
      }
      sequence_ = message.sequence;
      // The sequence selects one of two fixed, sealed slots. LVGL consumes this
      // immutable slot before the completed display refresh acknowledges it;
      // the worker can then only write the other slot. No engine-controlled descriptor,
      // allocation size or process address crosses the boundary.
      frame_pending_ = true; changed = true;
    } else if (message.kind == Kind::kStatus) {
      progress_ = message.value; can_back_ = message.x; can_forward_ = message.y;
      status_ = message.text;
    } else if (message.kind == Kind::kKeyboardShow) {
      keyboard_purpose_ = message.value;
      keyboard_request_ = KeyboardRequest::kShow;
    } else if (message.kind == Kind::kKeyboardHide) {
      keyboard_request_ = KeyboardRequest::kHide;
    } else if (message.kind == Kind::kDownloadStarted ||
               message.kind == Kind::kDownloadProgress ||
               message.kind == Kind::kDownloadFinished ||
               message.kind == Kind::kDownloadFailed ||
               message.kind == Kind::kDownloadCancelled) {
      auto found = std::find_if(downloads_.begin(), downloads_.end(),
          [&](const DownloadItem &item) { return item.id == message.sequence; });
      if (found == downloads_.end()) {
        if (downloads_.size() >= 24) downloads_.pop_back();
        downloads_.insert(downloads_.begin(), DownloadItem{});
        found = downloads_.begin();
        found->id = message.sequence;
      }
      if (message.kind == Kind::kDownloadStarted ||
          message.kind == Kind::kDownloadProgress ||
          message.kind == Kind::kDownloadFinished) {
        found->name = message.text;
      } else if (message.text[0]) {
        found->error = message.text;
      }
      const uint64_t received = static_cast<uint64_t>(message.value) * 1024;
      const uint64_t now = MonotonicMilliseconds();
      if (!found->sample_milliseconds) {
        found->sample_milliseconds = now;
        found->sample_bytes = received;
      } else if (received >= found->sample_bytes &&
                 now > found->sample_milliseconds) {
        const uint64_t elapsed = now - found->sample_milliseconds;
        if (elapsed >= 100) {
          const uint64_t instant =
              (received - found->sample_bytes) * 1000 / elapsed;
          found->speed_bytes_per_second = found->speed_bytes_per_second
              ? (found->speed_bytes_per_second * 3 + instant) / 4
              : instant;
          found->sample_milliseconds = now;
          found->sample_bytes = received;
        }
      }
      found->progress = static_cast<unsigned>(message.x);
      found->total_bytes = static_cast<uint64_t>(message.y) * 1024;
      found->received_bytes = received;
      found->status = message.kind == Kind::kDownloadFinished
          ? DownloadStatus::kFinished
          : message.kind == Kind::kDownloadFailed ? DownloadStatus::kFailed
          : message.kind == Kind::kDownloadCancelled ? DownloadStatus::kCancelled
          : DownloadStatus::kActive;
      if (found->status != DownloadStatus::kActive)
        RestoreDownloadStorage(&found->name);
      ++download_revision_;
      PublishDownloads();
    } else if (message.kind == Kind::kBrowsingDataCleared ||
               message.kind == Kind::kBrowsingDataFailed) {
      settings_notice_ = message.text[0] ? message.text :
          message.kind == Kind::kBrowsingDataCleared
              ? "Cookies and site data cleared."
              : "Could not clear browsing data.";
      ++settings_revision_;
    } else status_ = message.text;
  }
  return changed;
}
}  // namespace aeraui::web
