/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <unistd.h>

namespace aera::pc {
#ifndef BUILD_TWRPTAR_MAIN
bool BackupOperationActive();
bool BackupCancelRequested();
#else
inline bool BackupOperationActive() { return false; }
inline bool BackupCancelRequested() { return false; }
#endif

// Only the parent watches cancellation. Stop before reaping to prevent PID
// reuse, and signal the child's own group so compressors cannot linger.
class BackupChildCancellation {
 public:
  explicit BackupChildCancellation(pid_t child) {
    if (child <= 0 || !BackupOperationActive()) return;
    setpgid(child, child);
    watcher_ = std::thread([this, child] {
      while (!stop_.load()) {
        if (BackupCancelRequested()) {
          kill(-child, SIGTERM);
          for (unsigned i = 0; i < 20 && !stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (!stop_.load()) kill(-child, SIGKILL);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  ~BackupChildCancellation() { Stop(); }
  void Stop() { stop_.store(true); if (watcher_.joinable()) watcher_.join(); }
 private:
  std::atomic<bool> stop_{false};
  std::thread watcher_;
};
}  // namespace aera::pc
