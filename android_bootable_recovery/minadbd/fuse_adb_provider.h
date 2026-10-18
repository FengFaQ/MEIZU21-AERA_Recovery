/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>

#include <vector>

#include "fuse_provider.h"

// This class reads data from adb server.
class FuseAdbDataProvider : public FuseDataProvider {
 public:
  FuseAdbDataProvider(int fd, uint64_t file_size, uint32_t block_size,
                      int progress_fd = -1)
      : FuseDataProvider(file_size, block_size),
        fd_(fd),
        progress_fd_(progress_fd),
        seen_blocks_(block_size == 0 ? 0 :
            file_size / block_size + (file_size % block_size == 0 ? 0 : 1),
            0) {}

  bool ReadBlockAlignedData(uint8_t* buffer, uint32_t fetch_size,
                            uint32_t start_block) const override;

  bool Valid() const override {
    return fd_ != -1;
  }

 private:
  void ReportProgress(uint32_t start_block, uint32_t fetch_size) const;

  // The underlying source to read data from (i.e. the one that talks to the host).
  int fd_;
  mutable int progress_fd_;
  mutable std::vector<uint8_t> seen_blocks_;
  mutable uint64_t received_bytes_ = 0;
  mutable uint32_t reported_percent_ = UINT32_MAX;
};
