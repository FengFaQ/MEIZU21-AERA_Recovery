// Copyright 2026 AERA Recovery Project contributors.
// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace aera {
// No nested owner may clear the outer batch's saved service list. Cleanup also
// runs when preparation fails partway through stopping/unmounting dependencies.
template<class Prepare, class Operation, class Restore>
bool RunImageFlashBatch(bool& active, Prepare prepare, Operation operation, Restore restore) {
  if (active) return false;
  active = true;
  struct Cleanup {
    bool& active;
    Restore& restore;
    ~Cleanup() { active = false; restore(); }
  } cleanup{active, restore};
  return prepare() && operation();
}
} // namespace aera
