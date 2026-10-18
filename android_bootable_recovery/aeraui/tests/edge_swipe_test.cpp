// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include "edge_swipe.hpp"

int main() {
  for (int width : {1080, 1440, 2880, 3168}) {
    for (bool right : {false, true}) {
      const int start = right ? width - 10 : 10;
      const int direction = right ? -1 : 1;
      const int threshold = width / 6;
      auto inward = [&](int offset) {
        return aeraui::EdgeSwipeInward(start, start + direction * offset, right);
      };
      assert(!aeraui::EdgeSwipeShouldCommit(false, inward(threshold + 80), width));
      assert(aeraui::EdgeSwipeShouldCommit(true, inward(threshold), width));
      assert(!aeraui::EdgeSwipeShouldCommit(true, inward(threshold - 1), width));
      // Cross the threshold, reverse, return to/past the starting edge.
      assert(aeraui::EdgeSwipeShouldCommit(true, inward(threshold + 80), width));
      assert(!aeraui::EdgeSwipeShouldCommit(true, inward(12), width));
      assert(!aeraui::EdgeSwipeShouldCommit(true, inward(0), width));
      assert(!aeraui::EdgeSwipeShouldCommit(true, inward(-10), width));
      // Pull inward again before release: a deliberate Back still works.
      assert(aeraui::EdgeSwipeShouldCommit(true, inward(threshold + 30), width));
    }
  }
  puts("Back gesture commit/cancel/re-arm: both edges, portrait and landscape passed");
}
