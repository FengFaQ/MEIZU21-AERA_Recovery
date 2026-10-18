/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <aeraui/backend.hpp>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>

namespace aeraui::payload {
inline double PartitionProgress(const PayloadPartitionProgress& p) {
  const double fraction = p.bytes ? double(std::min(p.done, p.bytes)) / p.bytes : 0;
  switch (p.phase) {
    case PayloadPhase::Writing: return 0.8 * fraction;
    case PayloadPhase::Written: return 0.8;
    case PayloadPhase::Verifying: return 0.8 + 0.2 * fraction;
    case PayloadPhase::Verified: return 1;
    default: return 0;
  }
}
inline int OverallProgress(const PayloadFlashProgress& state) {
  long double total = 0, done = 0;
  for (const auto& p : state.partitions) {
    total += p.bytes;
    done += p.bytes * PartitionProgress(p);
  }
  if (!total) return 0;
  const bool complete = std::all_of(state.partitions.begin(), state.partitions.end(),
      [](const auto& p) { return p.phase == PayloadPhase::Verified; });
  return complete ? 100 : std::min(99, int(100 * done / total));
}
class FlashProgressStore {
 public:
  void Reset() { std::lock_guard<std::mutex> lock(mutex_); value_ = {}; }
  void Begin(const std::vector<std::string>& names, const std::vector<uint64_t>& bytes, unsigned lanes) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = {}; value_.available = true; value_.lanes = lanes;
    for (size_t i = 0; i < names.size(); ++i) value_.partitions.push_back({names[i], bytes[i], 0, PayloadPhase::Queued});
  }
  void Set(size_t index, PayloadPhase phase, uint64_t done = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& p = value_.partitions.at(index);
    p.phase = phase; p.done = std::min(done, p.bytes);
  }
  void Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : value_.partitions)
      if (p.phase == PayloadPhase::Queued || p.phase == PayloadPhase::Preparing) p.phase = PayloadPhase::NotFlashed;
  }
  PayloadFlashProgress Read() const { std::lock_guard<std::mutex> lock(mutex_); return value_; }
 private:
  mutable std::mutex mutex_;
  PayloadFlashProgress value_;
};

// All threads must exist before any write starts. On failure stop dequeuing,
// but let already-running tasks finish and verify. Always join before returning.
inline bool RunParallelImages(size_t count, unsigned lanes, const std::function<bool(size_t)>& work) {
  if (!count || !lanes) return false;
  struct State {
    std::mutex mutex;
    std::condition_variable ready;
    bool released = false, failed = false;
    size_t next = 0, count;
    const std::function<bool(size_t)>& work;
    State(size_t n, const std::function<bool(size_t)>& fn) : count(n), work(fn) {}
  } state(count, work);
  const auto worker = +[](void* data) -> void* {
    auto& s = *static_cast<State*>(data);
    for (;;) {
      size_t index;
      {
        std::unique_lock<std::mutex> lock(s.mutex);
        s.ready.wait(lock, [&] { return s.released; });
        if (s.failed || s.next == s.count) return nullptr;
        index = s.next++;
      }
      if (!s.work(index)) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.failed = true;
        return nullptr;
      }
    }
  };
  std::vector<pthread_t> threads;
  for (unsigned i = 0; i < std::min<size_t>(lanes, count); ++i) {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, worker, &state)) {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.failed = true;
      break;
    }
    threads.push_back(thread);
  }
  { std::lock_guard<std::mutex> lock(state.mutex); state.released = true; }
  state.ready.notify_all();
  for (auto thread : threads) pthread_join(thread, nullptr);
  return !state.failed;
}
}  // namespace aeraui::payload
