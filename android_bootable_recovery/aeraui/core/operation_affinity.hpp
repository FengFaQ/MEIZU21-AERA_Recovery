/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cerrno>
#include <mutex>
#include <sched.h>
#include <unistd.h>

namespace aeraui::operation_affinity {

struct Baseline {
  std::mutex mutex;
  cpu_set_t mask{};
  bool valid = false;
};

inline Baseline& State() {
  static Baseline state;
  return state;
}

// Called before the UI's first interaction boost. Preserve the original
// permitted CPUs, not a device-specific mask or a later boosted mask.
inline void Remember(const cpu_set_t& mask) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.valid && CPU_COUNT(&mask) > 0) {
    state.mask = mask;
    state.valid = true;
  }
}

// Must run inside the new worker, never on the UI thread. Linux applies the
// worker's current online/cpuset restrictions when setting this affinity.
// Children and decoder threads subsequently inherit this restored mask.
inline int RestoreForWorker() {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.valid) return ENODATA;
  return sched_setaffinity(0, sizeof(state.mask), &state.mask) == 0 ? 0 : errno;
}

// Snapshot before fork: never acquire a possibly inherited locked mutex in
// the child, and never temporarily widen the spawning UI thread's affinity.
inline pid_t ForkWorker() {
  cpu_set_t mask{};
  bool valid;
  {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    valid = state.valid;
    if (valid) mask = state.mask;
  }
  const pid_t child = fork();
  if (child == 0 && valid) sched_setaffinity(0, sizeof(mask), &mask);
  return child;
}

}  // namespace aeraui::operation_affinity
