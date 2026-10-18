/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aeraui::plugin_api {

class Session final {
 public:
  ~Session() { Close(); }
  bool Adopt(int control_fd);
  std::vector<Message> Poll();
  bool Send(Kind kind, uint32_t request_id = 0, uint32_t value = 0,
            uint32_t flags = 0, const char *title = nullptr,
            const char *text = nullptr);
  bool Connected() const { return control_ >= 0; }
  bool Negotiated() const { return negotiated_; }
  const std::string &Status() const { return status_; }
  void Close();

 private:
  void Fail(const char *message);
  bool Write(const Message &message);
  int control_ = -1;
  bool negotiated_ = false;
  uint64_t rate_window_ms_ = 0;
  uint32_t rate_messages_ = 0;
  std::string status_;
};

}  // namespace aeraui::plugin_api
