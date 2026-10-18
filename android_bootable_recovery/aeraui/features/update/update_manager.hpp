/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aeraui::update {

enum class Channel {
  kStable,
  kNightly,
};

enum class Phase {
  kIdle,
  kChecking,
  kAvailable,
  kUpToDate,
  kDownloading,
  kVerifying,
  kReady,
  kError,
};

struct Release {
  std::string version;
  std::string build_type;
  uint64_t build_time = 0;
  std::string filename;
  uint64_t size = 0;
  std::string sha256;
  std::string url;
  std::vector<std::string> changelog;
};

struct Snapshot {
  Phase phase = Phase::kIdle;
  std::string device;
  uint64_t local_build_time = 0;
  Release release;
  std::string package_path;
  std::string message;
  uint64_t downloaded = 0;
  uint64_t total = 0;
  unsigned progress = 0;
  bool checked = false;
  bool available = false;
};

const char *CatalogUrl();
Channel GetChannel();
bool SetChannel(Channel channel);
Snapshot GetSnapshot();
bool Check();
bool PrepareDownload();
bool Download();
void SetOffline();
void Cancel();
void MarkInstalled();

}  // namespace aeraui::update
