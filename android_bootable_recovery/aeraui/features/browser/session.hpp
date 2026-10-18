// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace aeraui::web {
enum class KeyboardRequest { kNone, kShow, kHide };
enum class DownloadStatus { kActive, kFinished, kFailed, kCancelled };
struct DownloadItem {
  uint32_t id = 0;
  std::string name;
  std::string error;
  uint64_t received_bytes = 0;
  uint64_t total_bytes = 0;
  uint64_t speed_bytes_per_second = 0;
  uint64_t sample_bytes = 0;
  uint64_t sample_milliseconds = 0;
  unsigned progress = 0;
  DownloadStatus status = DownloadStatus::kActive;
};
struct DownloadSummary {
  bool available = false;
  unsigned active_count = 0;
  unsigned progress = 0;
  uint64_t received_bytes = 0;
  uint64_t total_bytes = 0;
  uint64_t speed_bytes_per_second = 0;
  DownloadStatus status = DownloadStatus::kFinished;
  std::string name;
};
// A small read-only snapshot for the status bar and Quick Settings. Browser
// file access remains confined to the download directory in its jail.
DownloadSummary CurrentDownloadSummary();
// Native side of the bridge. Adopt() is only for a future trusted launcher:
// the current recovery never calls it and has no path/FD/environment override.
// Browser IPC can never initiate recovery operations or select local files.
class Session {
 public:
  ~Session() { Close(); Unmap(); }
  Session() = default;
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  bool Adopt(int frame_fd, int control_fd);
  void Close();
  bool Connected() const { return control_ >= 0; }
  bool HasFrame() const { return sequence_ != 0; }
  bool Send(Kind kind, int x = 0, int y = 0, uint32_t value = 0, const char *text = "");
  bool SetZoom(unsigned percent);
  bool CancelDownload(uint32_t id);
  bool Poll(); // Nonblocking, at most eight messages; true if pixels changed.
  // Called after LVGL finishes rendering and flushing the current slot.
  bool AcknowledgeFrame();
  const uint8_t *Pixels() const {
    return shared_ ? shared_ + FrameSlot(sequence_) * kFrameBytes : nullptr;
  }
  const std::string &Status() const { return status_; }
  bool CanBack() const { return can_back_; }
  bool CanForward() const { return can_forward_; }
  unsigned Progress() const { return progress_; }
  unsigned ZoomPercent() const { return zoom_percent_; }
  const std::vector<DownloadItem> &Downloads() const { return downloads_; }
  uint32_t DownloadRevision() const { return download_revision_; }
  const std::string &SettingsNotice() const { return settings_notice_; }
  uint32_t SettingsRevision() const { return settings_revision_; }
  KeyboardRequest TakeKeyboardRequest(uint32_t *purpose = nullptr);
 private:
  bool Write(const Message &message);
  void Fail(const char *reason);
  void ResetState();
  void Unmap();
  void PublishDownloads();
  int control_ = -1;
  const uint8_t *shared_ = nullptr;
  std::string status_;
  uint32_t sequence_ = 0;
  uint32_t keyboard_purpose_ = 0;
  unsigned progress_ = 0;
  unsigned zoom_percent_ = 100;
  std::vector<DownloadItem> downloads_;
  uint32_t download_revision_ = 0;
  std::string settings_notice_;
  uint32_t settings_revision_ = 0;
  KeyboardRequest keyboard_request_ = KeyboardRequest::kNone;
  bool can_back_ = false, can_forward_ = false;
  bool frame_pending_ = false;
};
}  // namespace aeraui::web
