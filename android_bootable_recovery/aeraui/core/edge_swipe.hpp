// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>

namespace aeraui {

inline int32_t EdgeSwipeInward(int32_t start_x, int32_t current_x,
                             bool right_edge) {
  return std::max(0, right_edge ? start_x - current_x : current_x - start_x);
}

inline bool EdgeSwipeShouldCommit(bool captured, int32_t last_inward,
                                  int32_t visible_width) {
  // Capture reserves the contact, but does not irreversibly commit Back.
  // Only the last live touch coordinate counts, not the furthest excursion
  // or a controller's potentially zeroed release coordinates.
  return captured && last_inward >= std::max(1, visible_width / 6);
}

}  // namespace aeraui
