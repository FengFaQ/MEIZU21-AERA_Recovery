/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>
#include <sys/types.h>

namespace aeraui::plugin_api {
class Process final {
 public:
  ~Process() { Stop(); }
  Process() = default;
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;
  bool Start(const std::string &runtime, int &control_fd, std::string &error);
  bool Running();
  void Stop();
 private:
  pid_t pid_ = -1;
};
}  // namespace aeraui::plugin_api
