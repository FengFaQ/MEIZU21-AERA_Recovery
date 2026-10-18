// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <sys/types.h>
#include <string>

namespace aeraui::web {
class BrowserProcess {
 public:
  ~BrowserProcess() { Stop(); }
  BrowserProcess() = default;
  BrowserProcess(const BrowserProcess &) = delete;
  BrowserProcess &operator=(const BrowserProcess &) = delete;
  bool Start(const std::string &runtime, int &frame_fd, int &control_fd,
             std::string &error);
  bool Running();
  void Stop();
 private:
  void StopAudio();
  pid_t pid_ = -1;
  pid_t audio_pid_ = -1;
};
}  // namespace aeraui::web
