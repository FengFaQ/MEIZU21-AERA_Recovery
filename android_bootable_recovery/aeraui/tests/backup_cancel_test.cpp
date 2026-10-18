/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_remote/backup_cancel.hpp"
#include <cassert>
#include <poll.h>
#include <sys/wait.h>

namespace aera::pc {
std::atomic<bool> active{true}, cancelled{false};
bool BackupOperationActive() { return active.load(); }
bool BackupCancelRequested() { return active.load() && cancelled.load(); }
}

int main() {
  using namespace aera::pc;
  int channel[2]; assert(pipe(channel) == 0);
  const auto child = fork(); assert(child >= 0);
  if (!child) {
    close(channel[0]); setpgid(0, 0); signal(SIGTERM, SIG_IGN);
    assert(write(channel[1], "R", 1) == 1);
    while (true) pause();
  }
  close(channel[1]); char ready;
  assert(read(channel[0], &ready, 1) == 1);
  BackupChildCancellation watcher(child);
  cancelled.store(true);
  pollfd pipe_event{channel[0], POLLIN | POLLHUP, 0};
  assert(poll(&pipe_event, 1, 5000) > 0);
  assert(read(channel[0], &ready, 1) == 0);
  watcher.Stop(); int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  close(channel[0]);

  active.store(false);
  const auto unrelated = fork(); assert(unrelated >= 0);
  if (!unrelated) { usleep(200000); _exit(0); }
  BackupChildCancellation disabled(unrelated);
  assert(waitpid(unrelated, &status, 0) == unrelated);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
