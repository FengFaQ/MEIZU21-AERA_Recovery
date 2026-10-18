/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace aeraui {

// AERA pages are authored once at this logical size. Adaptive builds render
// that canvas across the complete native panel and map touch coordinates back
// into the same space. Using separate X/Y scales avoids letterboxing on phone
// panels whose aspect ratios differ slightly.
struct DisplayTransform {
  static constexpr int32_t kDesignWidth = 1440;
  static constexpr int32_t kDesignHeight = 3168;

  int32_t physical_width = 0;
  int32_t physical_height = 0;
  int32_t logical_width = 0;
  int32_t logical_height = 0;

  static constexpr DisplayTransform Native(int32_t width, int32_t height) {
    return {width, height, width, height};
  }

  static constexpr DisplayTransform Adaptive(
      int32_t width, int32_t height,
      int32_t logical_height = kDesignHeight) {
    return {width, height, kDesignWidth, logical_height};
  }

  constexpr bool IsValid() const {
    return physical_width > 0 && physical_height > 0 && logical_width > 0 &&
           logical_height > 0;
  }

  constexpr bool IsScaled() const {
    return physical_width != logical_width || physical_height != logical_height;
  }

  int32_t ToLogicalX(int32_t x) const {
    return MapCoordinate(x, physical_width, logical_width);
  }

  int32_t ToLogicalY(int32_t y) const {
    return MapCoordinate(y, physical_height, logical_height);
  }

 private:
  static int32_t MapCoordinate(int32_t value, int32_t source_size,
                               int32_t destination_size) {
    if (source_size <= 1 || destination_size <= 1) return 0;
    value = std::clamp(value, 0, source_size - 1);
    return static_cast<int32_t>(
        (static_cast<int64_t>(value) * (destination_size - 1) +
         (source_size - 1) / 2) /
        (source_size - 1));
  }
};

}  // namespace aeraui
