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

#include "fuse_adb_provider.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <android-base/file.h>

#include "adb.h"
#include "adb_io.h"
#include "minadbd/types.h"

void FuseAdbDataProvider::ReportProgress(uint32_t start_block,
                                         uint32_t fetch_size) const {
  if (progress_fd_ < 0 || start_block >= seen_blocks_.size() ||
      seen_blocks_[start_block] != 0 || file_size_ == 0) {
    return;
  }
  seen_blocks_[start_block] = 1;
  received_bytes_ += fetch_size;
  if (received_bytes_ > file_size_) received_bytes_ = file_size_;

  const uint32_t percent =
      static_cast<uint32_t>((received_bytes_ * 100ULL) / file_size_);
  if (percent == reported_percent_ && received_bytes_ != file_size_) return;
  reported_percent_ = percent;

  const SideloadProgressMessage message{received_bytes_, file_size_};
  if (!android::base::WriteFully(progress_fd_, &message, sizeof(message))) {
    // Progress is optional. Never interrupt the package stream if its observer
    // has gone away.
    progress_fd_ = -1;
  }
}

bool FuseAdbDataProvider::ReadBlockAlignedData(uint8_t* buffer, uint32_t fetch_size,
                                               uint32_t start_block) const {
  if (!WriteFdFmt(fd_, "%08u", start_block)) {
    fprintf(stderr, "failed to write to adb host: %s\n", strerror(errno));
    return false;
  }

  if (!ReadFdExactly(fd_, buffer, fetch_size)) {
    fprintf(stderr, "failed to read from adb host: %s\n", strerror(errno));
    return false;
  }

  ReportProgress(start_block, fetch_size);
  return true;
}
