// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#include "aeraui/platform/aera_message.hpp"

#include <cctype>
#include <cstdlib>

#include "aeraui/i18n.hpp"
#include "data.hpp"

Message::Message(msg::Kind kind, const char* text)
    : kind_(kind), text_(text != nullptr ? text : "") {}

std::string Message::ResolveTemplate() const {
  const size_t separator = text_.find('=');
  const std::string fallback = separator == std::string::npos
                                   ? text_
                                   : text_.substr(separator + 1);
  const char* translated = aeraui::i18n::Translate(fallback.c_str());
  return translated != nullptr && translated[0] != '\0' ? translated : fallback;
}

std::string Message::ResolveToken(const std::string& token) const {
  if (!token.empty() && std::isdigit(static_cast<unsigned char>(token[0]))) {
    char* end = nullptr;
    const long index = std::strtol(token.c_str(), &end, 10);
    if (end != token.c_str() && *end == '\0' && index > 0 &&
        static_cast<size_t>(index) <= arguments_.size()) {
      return arguments_[static_cast<size_t>(index - 1)];
    }
  }

  std::string value;
  return DataManager::GetValue(token, value) == 0 ? value : std::string();
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
