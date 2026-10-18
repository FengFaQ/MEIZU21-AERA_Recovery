/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string>
#include <sys/types.h>

namespace aeraui::retro {
class Process {
 public:
  ~Process() { Stop(); }
  Process() = default;
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;
  bool Start(const std::string &runtime, int &frame_fd, int &control_fd,
             std::string &error);
  bool Running();
  void Stop();

 private:
  void StopAudio();
  pid_t pid_ = -1;
  pid_t audio_pid_ = -1;
};
}  // namespace aeraui::retro
