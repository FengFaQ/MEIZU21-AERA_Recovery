// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <sys/types.h>

namespace aeraui::telegram {
class Process {
 public:
  ~Process() { Stop(); }
  bool Start(const std::string &runtime, int &control_fd, std::string &error);
  bool Running();
  void Stop();
 private:
  pid_t pid_ = -1;
};
}  // namespace aeraui::telegram
