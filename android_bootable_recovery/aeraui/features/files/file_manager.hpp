/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <sys/types.h>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace aeraui::file_manager {

enum class Operation { kCopy, kMove, kDelete, kMeasure };
enum class Conflict { kReplace, kKeepBoth, kSkip };

struct Request {
  Operation operation = Operation::kCopy;
  Conflict conflict = Conflict::kKeepBoth;
  std::vector<std::string> sources;
  std::string destination;
};

struct Metadata {
  bool exists = false;
  bool directory = false;
  bool regular = false;
  bool symlink = false;
  uint64_t size = 0;
  mode_t mode = 0;
  uid_t uid = 0;
  gid_t gid = 0;
  std::time_t modified = 0;
  std::string link_target;
};

struct Snapshot {
  unsigned percent = 0;
  bool done = false;
  bool cancelled = false;
  bool success = false;
  uint64_t completed_bytes = 0;
  uint64_t total_bytes = 0;
  uint64_t result_bytes = 0;
  size_t completed_items = 0;
  size_t total_items = 0;
  size_t errors = 0;
  size_t skipped = 0;
  std::string status;
  std::string detail;
};

struct Progress {
  std::atomic<bool> cancel{ false };
  std::atomic<bool> done{ false };
  std::atomic<bool> success{ false };
  std::atomic<bool> cancelled{ false };
  std::atomic<unsigned> percent{ 0 };
  std::atomic<uint64_t> completed_bytes{ 0 };
  std::atomic<uint64_t> total_bytes{ 0 };
  std::atomic<uint64_t> result_bytes{ 0 };
  std::atomic<size_t> completed_items{ 0 };
  std::atomic<size_t> total_items{ 0 };
  std::atomic<size_t> errors{ 0 };
  std::atomic<size_t> skipped{ 0 };
  mutable std::mutex text_mutex;
  std::string status;
  std::string detail;

  void Reset();
  Snapshot Get() const;
};

Metadata Inspect(const std::string& path);
std::string TypeName(const Metadata& metadata);
std::string PermissionText(mode_t mode);
std::string BaseName(const std::string& path);
std::string Parent(const std::string& path);
std::string Join(const std::string& directory, const std::string& name);
bool ValidName(const std::string& name, std::string& error);
bool Exists(const std::string& path);
bool IsTextFile(const std::string& path, uint64_t maximum_bytes);
bool ReadText(const std::string& path, uint64_t maximum_bytes, std::string& text, bool& truncated,
              std::string& error);
bool WriteTextAtomic(const std::string& path, const std::string& text, std::string& error);
bool CreateDirectory(const std::string& path, std::string& error);
bool Rename(const std::string& source, const std::string& destination, std::string& error);
void Run(const Request& request, Progress& progress);

}  // namespace aeraui::file_manager
