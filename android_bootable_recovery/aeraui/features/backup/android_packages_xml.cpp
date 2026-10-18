/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ABX v0 follows AOSP modules-utils BinaryXmlSerializer/BinaryXmlPullParser.
 * It is not the binary resource XML format used inside APKs.
 */
#include "android_packages_xml.hpp"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>

namespace aeraui::appvault {
namespace {
constexpr size_t kMaximumBytes = 32U << 20;

bool AppendUtf8(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  if (cp < 0x80)
    out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 63));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 63));
    out += static_cast<char>(0x80 | (cp & 63));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 63));
    out += static_cast<char>(0x80 | ((cp >> 6) & 63));
    out += static_cast<char>(0x80 | (cp & 63));
  }
  return true;
}

class AbxReader {
 public:
  explicit AbxReader(const std::string& bytes) : bytes_(bytes), position_(4) {}

  bool Parse(tinyxml2::XMLDocument& document) {
    std::vector<tinyxml2::XMLElement*> stack;
    bool attributes = false;
    bool started = false;
    size_t nodes = 0;
    while (valid_ && position_ < bytes_.size()) {
      const uint8_t event = static_cast<uint8_t>(Number(1));
      const uint8_t token = event & 15;
      if (token == 0) {
        if (started || position_ != 5) return Fail();
        started = true;
        continue;
      }
      if (!started) return Fail();
      if (token == 1) {
        if (!stack.empty() || !document.RootElement() || position_ != bytes_.size()) return Fail();
        return valid_;
      }
      if (token == 15) {
        if (!attributes || stack.empty()) return Fail();
        const std::string name = Interned();
        if (!valid_ || name.empty() || stack.back()->Attribute(name.c_str())) return Fail();
        const std::string value = Attribute(event & 0xf0);
        if (!valid_) return false;
        if ((event & 0xf0) != 0x10) stack.back()->SetAttribute(name.c_str(), value.c_str());
        continue;
      }
      attributes = false;
      if (++nodes > 1000000) return Fail();
      if (token == 2) {
        if (stack.size() >= 128) return Fail();
        const std::string name = Interned();
        if (!valid_ || name.empty() || (stack.empty() && document.RootElement())) return Fail();
        auto* element = document.NewElement(name.c_str());
        if (stack.empty())
          document.InsertEndChild(element);
        else
          stack.back()->InsertEndChild(element);
        stack.push_back(element);
        attributes = true;
      } else if (token == 3) {
        const std::string name = Interned();
        if (!valid_ || stack.empty() || name != stack.back()->Name()) return Fail();
        stack.pop_back();
      } else if (token == 4 || token == 5 || token == 7) {
        const std::string text = Utf();
        if (!valid_) return false;
        if (!stack.empty()) {
          auto* node = document.NewText(text.c_str());
          node->SetCData(token == 5);
          stack.back()->InsertEndChild(node);
        } else if (text.find_first_not_of(" \r\n\t") != std::string::npos)
          return Fail();
      } else if (token == 9) {
        const std::string text = Utf();
        if (!valid_) return false;
        auto* node = document.NewComment(text.c_str());
        if (stack.empty())
          document.InsertEndChild(node);
        else
          stack.back()->InsertEndChild(node);
      } else
        return Fail();
    }
    return Fail();
  }

 private:
  bool Fail() {
    valid_ = false;
    return false;
  }

  std::string InvalidString() {
    Fail();
    return {};
  }

  uint64_t Number(size_t count) {
    if (!valid_ || count > bytes_.size() - position_) {
      Fail();
      return 0;
    }
    uint64_t value = 0;
    while (count--) value = (value << 8) | static_cast<uint8_t>(bytes_[position_++]);
    return value;
  }

  std::string Utf() {
    const size_t length = Number(2);
    if (!valid_ || length > bytes_.size() - position_) return InvalidString();
    const size_t end = position_ + length;
    std::string result;
    uint32_t surrogate = 0;
    while (position_ < end) {
      const uint8_t first = static_cast<uint8_t>(bytes_[position_++]);
      uint32_t unit = first;
      size_t continuation = 0;
      if ((first & 0xe0) == 0xc0) {
        unit &= 31;
        continuation = 1;
      } else if ((first & 0xf0) == 0xe0) {
        unit &= 15;
        continuation = 2;
      } else if (first >= 0x80)
        return InvalidString();
      if (continuation > end - position_) return InvalidString();
      while (continuation--) {
        const uint8_t next = static_cast<uint8_t>(bytes_[position_++]);
        if ((next & 0xc0) != 0x80) return InvalidString();
        unit = (unit << 6) | (next & 63);
      }
      if (surrogate) {
        if (unit < 0xdc00 || unit > 0xdfff) return InvalidString();
        if (!AppendUtf8(result, 0x10000 + ((surrogate - 0xd800) << 10) + unit - 0xdc00))
          return InvalidString();
        surrogate = 0;
      } else if (unit >= 0xd800 && unit <= 0xdbff)
        surrogate = unit;
      else if (!AppendUtf8(result, unit))
        return InvalidString();
    }
    if (surrogate) return InvalidString();
    return result;
  }

  std::string Interned() {
    const size_t index = Number(2);
    if (!valid_) return {};
    if (index != 0xffff) {
      if (index >= strings_.size()) return InvalidString();
      return strings_[index];
    }
    std::string value = Utf();
    if (!valid_) return {};
    if (strings_.size() < 65535) strings_.push_back(value);
    return value;
  }

  std::string Attribute(uint8_t type) {
    if (type == 0x10) return {};
    if (type == 0x20) return Utf();
    if (type == 0x30) return Interned();
    if (type == 0xc0) return "true";
    if (type == 0xd0) return "false";
    if (type == 0x40 || type == 0x50) {
      const size_t size = Number(2);
      if (!valid_ || size > bytes_.size() - position_) return InvalidString();
      const std::string bytes = bytes_.substr(position_, size);
      position_ += size;
      static constexpr char hex[] = "0123456789abcdef";
      static constexpr char base64[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string value;
      if (type == 0x40) {
        for (uint8_t byte : bytes) {
          value += hex[byte >> 4];
          value += hex[byte & 15];
        }
      } else {
        for (size_t i = 0; i < bytes.size(); i += 3) {
          const uint32_t bits = (static_cast<uint8_t>(bytes[i]) << 16) |
                                (i + 1 < size ? static_cast<uint8_t>(bytes[i + 1]) << 8 : 0) |
                                (i + 2 < size ? static_cast<uint8_t>(bytes[i + 2]) : 0);
          value += base64[(bits >> 18) & 63];
          value += base64[(bits >> 12) & 63];
          value += i + 1 < size ? base64[(bits >> 6) & 63] : '=';
          value += i + 2 < size ? base64[bits & 63] : '=';
        }
      }
      return value;
    }
    if (type < 0x60 || type > 0xb0) return InvalidString();
    const uint64_t bits = Number(type == 0x60 || type == 0x70 || type == 0xa0 ? 4 : 8);
    std::ostringstream value;
    value.imbue(std::locale::classic());
    if (type == 0x70 || type == 0x90)
      value << std::hex << bits;
    else if (type == 0x60)
      value << static_cast<int32_t>(bits);
    else if (type == 0x80)
      value << static_cast<int64_t>(bits);
    else if (type == 0xa0) {
      const uint32_t raw = bits;
      float number;
      memcpy(&number, &raw, sizeof(number));
      value << std::setprecision(std::numeric_limits<float>::max_digits10) << number;
    } else {
      double number;
      memcpy(&number, &bits, sizeof(number));
      value << std::setprecision(std::numeric_limits<double>::max_digits10) << number;
    }
    return value.str();
  }

  const std::string& bytes_;
  size_t position_;
  bool valid_ = true;
  std::vector<std::string> strings_;
};
}  // namespace

bool ParsePackagesXml(const std::string& bytes, tinyxml2::XMLDocument& document,
                      std::string& error) {
  document.Clear();
  if (bytes.empty() || bytes.size() > kMaximumBytes) {
    error = "Android package database is missing or too large.";
    return false;
  }
  const bool binary = bytes.size() >= 4 && bytes.compare(0, 4, "ABX\0", 4) == 0;
  const bool valid = binary
                         ? AbxReader(bytes).Parse(document)
                         : bytes.find('\0') == std::string::npos &&
                               document.Parse(bytes.data(), bytes.size()) == tinyxml2::XML_SUCCESS;
  if (!valid || !document.RootElement()) {
    document.Clear();
    error = "Invalid or unsupported Android package XML.";
    return false;
  }
  return true;
}
}  // namespace aeraui::appvault
