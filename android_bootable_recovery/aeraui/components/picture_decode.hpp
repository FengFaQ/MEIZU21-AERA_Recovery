/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace aeraui {
struct PictureData {
  std::atomic<bool> ready{false}, cancelled{false};
  uint32_t width = 0, height = 0;
  uint8_t *pixels = nullptr; // BGRA, owned here until viewer and worker exit.
  std::string error;
  ~PictureData() { free(pixels); }
};
bool IsPicture(const std::string &path);
void DecodePicture(const std::string &path, PictureData &result);
void DecodePictureThumbnail(const std::string &path, uint32_t max_width,
                            uint32_t max_height, PictureData &result);
} // namespace aeraui
