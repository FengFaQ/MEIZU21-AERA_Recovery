/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <json/json.h>

namespace aeraui {
struct ResticProgress {
  uint64_t bytes_done = 0;
  uint64_t bytes_total = 0;
  uint64_t files_done = 0;
  uint64_t files_total = 0;
  unsigned percent = 0;
};

inline bool ReadResticProgress(const Json::Value &message, bool restore,
                              ResticProgress &progress) {
  if (!message.isObject() || !message["message_type"].isString()) return false;
  const auto type = message["message_type"].asString();
  if (type != "status" && type != "summary") return false;
  const auto count = [&message](const char *key) -> uint64_t {
    return message[key].isUInt64() ? message[key].asUInt64() : 0;
  };
  const auto add = [](uint64_t left, uint64_t right) {
    return left + std::min(right, std::numeric_limits<uint64_t>::max() - left);
  };
  progress = {};
  if (!restore && type == "summary") {
    progress.bytes_done = progress.bytes_total = count("total_bytes_processed");
    progress.files_done = progress.files_total = count("total_files_processed");
  } else {
    progress.bytes_done = count(restore ? "bytes_restored" : "bytes_done");
    progress.bytes_total = count("total_bytes");
    progress.files_done = count(restore ? "files_restored" : "files_done");
    progress.files_total = count("total_files");
    if (restore) {
      // Restic excludes unchanged files from both restore totals and writes.
      progress.bytes_done = add(progress.bytes_done, count("bytes_skipped"));
      progress.bytes_total = add(progress.bytes_total, count("bytes_skipped"));
      progress.files_done = add(progress.files_done, count("files_skipped"));
      progress.files_total = add(progress.files_total, count("files_skipped"));
    }
  }
  const uint64_t total = progress.bytes_total ? progress.bytes_total : progress.files_total;
  const uint64_t done = progress.bytes_total ? progress.bytes_done : progress.files_done;
  // A complete counter does not establish success until the process exits.
  if (total) progress.percent = static_cast<unsigned>(
      std::clamp(static_cast<double>(done) / total * 100.0, 0.0, 99.0));
  return true;
}
}
