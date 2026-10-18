/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <regex.h>
#include <string>

namespace aeraui::root {
inline std::string KernelKmiFromBanner(const std::string &bytes) {
  size_t offset = 0;
  while ((offset = bytes.find("Linux version ", offset)) != std::string::npos) {
    const size_t start = offset + 14;
    offset = start;
    size_t end = start;
    while (end < bytes.size() && end - start < 200 &&
           bytes[end] != '\0' && bytes[end] != ' ' && bytes[end] != '\t' && bytes[end] != '\r' && bytes[end] != '\n') ++end;
    if (end == bytes.size()) continue;  // Wait for the next chunk's complete release.
    const std::string release = bytes.substr(start, end - start);
    regex_t pattern{};
    if (regcomp(&pattern, "^([0-9]+\\.[0-9]+)\\.[0-9]+[-[:alnum:]_.+]*-(android[0-9]+)([-[:alnum:]_.+]*)$", REG_EXTENDED) != 0)
      return {};
    regmatch_t matches[4]{};
    const bool matched = regexec(&pattern, release.c_str(), 4, matches, 0) == 0;
    regfree(&pattern);
    if (matched) return release.substr(matches[2].rm_so, matches[2].rm_eo - matches[2].rm_so) + "-" +
        release.substr(matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
  }
  return {};
}
}  // namespace aeraui::root
