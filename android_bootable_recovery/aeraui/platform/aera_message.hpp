// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace msg {

enum Kind {
  kNormal,
  kHighlight,
  kWarning,
  kError,
  kProcess,
  kGreen,
  kBlue,
  kYellow,
  kBlack,
  kPink,
};

template <typename T>
std::string to_string(const T& value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

}  // namespace msg

class Message {
 public:
  Message(msg::Kind kind, const char* text);

  template <typename T>
  Message& operator()(const T& value) {
    arguments_.push_back(msg::to_string(value));
    return *this;
  }

  operator std::string() const;
  msg::Kind GetKind() const { return kind_; }

 private:
  std::string ResolveTemplate() const;
  std::string ResolveToken(const std::string& token) const;

  msg::Kind kind_;
  std::string text_;
  std::vector<std::string> arguments_;
};

Message Msg(const char* text);
Message Msg(msg::Kind kind, const char* text);
