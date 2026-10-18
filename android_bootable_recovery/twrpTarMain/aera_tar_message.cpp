// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#include "../aeraui/platform/aera_message.hpp"

#include <cctype>
#include <cstdlib>

Message::Message(msg::Kind kind, const char* text)
    : kind_(kind), text_(text != nullptr ? text : "") {}

std::string Message::ResolveTemplate() const {
  const size_t separator = text_.find('=');
  return separator == std::string::npos ? text_ : text_.substr(separator + 1);
}

std::string Message::ResolveToken(const std::string& token) const {
  if (token.empty() ||
      !std::isdigit(static_cast<unsigned char>(token.front()))) {
    return {};
  }

  char* end = nullptr;
  const long index = std::strtol(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0' || index <= 0 ||
      static_cast<size_t>(index) > arguments_.size()) {
    return {};
  }
  return arguments_[static_cast<size_t>(index - 1)];
}

Message::operator std::string() const {
  std::string result = ResolveTemplate();
  size_t cursor = 0;
  while ((cursor = result.find('{', cursor)) != std::string::npos) {
    const size_t end = result.find('}', cursor + 1);
    if (end == std::string::npos) break;
    const std::string replacement =
        ResolveToken(result.substr(cursor + 1, end - cursor - 1));
    result.replace(cursor, end - cursor + 1, replacement);
    cursor += replacement.size();
  }
  return result;
}

Message Msg(const char* text) { return Message(msg::kNormal, text); }

Message Msg(msg::Kind kind, const char* text) { return Message(kind, text); }
