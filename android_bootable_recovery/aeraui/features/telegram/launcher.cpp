// SPDX-License-Identifier: Apache-2.0
#include "launcher.hpp"
#include "../../core/operation_affinity.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace aeraui::telegram {
namespace {
constexpr uid_t kTelegramUid = 99091;

bool DirectoryAt(int parent, const char *name, mode_t mode, int &result) {
  if (mkdirat(parent, name, mode) && errno != EEXIST) return false;
  result = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (result < 0) return false;
  struct stat info{};
  if (fstat(result, &info) || !S_ISDIR(info.st_mode)) {
    close(result); result = -1; return false;
  }
  return true;
}

bool PrepareState() {
  int data = open("/data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  int recovery = -1, aera = -1, telegram = -1;
  const bool ok = data >= 0 && DirectoryAt(data, "recovery", 0700, recovery) &&
      DirectoryAt(recovery, "AERA", 0700, aera) &&
      DirectoryAt(aera, "telegram", 0700, telegram) &&
      fchmod(telegram, 0700) == 0 &&
      fchown(telegram, kTelegramUid, kTelegramUid) == 0;
  if (telegram >= 0) close(telegram);
  if (aera >= 0) close(aera);
  if (recovery >= 0) close(recovery);
  if (data >= 0) close(data);
  return ok;
}
}  // namespace

bool Process::Start(const std::string &runtime, int &control_fd,
                    std::string &error) {
  control_fd = -1;
  if (pid_ >= 0 || runtime.size() != 19 ||
      runtime.compare(0, 13, "/tmp/aera-tg-")) {
    error = "The verified Telegram runtime path is invalid.";
    return false;
  }
  struct stat root{};
  if (lstat(runtime.c_str(), &root) || !S_ISDIR(root.st_mode) || root.st_uid ||
      (root.st_mode & 0777) != 0700 || !PrepareState()) {
    error = "Could not prepare private encrypted Telegram storage.";
    return false;
  }
  int channels[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels)) {
    error = "Could not create the private Telegram channel.";
    return false;
  }
  const int child_control = fcntl(channels[1], F_DUPFD_CLOEXEC, 10);
  if (child_control < 0) {
    close(channels[0]); close(channels[1]);
    error = "Could not protect the Telegram channel.";
    return false;
  }
  const pid_t child = aeraui::operation_affinity::ForkWorker();
  if (!child) {
    if (dup2(child_control, 4) < 0) _exit(78);
    close(child_control);
    execl("/system/bin/aera-browser-jail", "aera-browser-jail",
          "--telegram", runtime.c_str(), nullptr);
    _exit(78);
  }
  close(child_control); close(channels[1]);
  if (child < 0) {
    close(channels[0]);
    error = "Could not start the isolated Telegram supervisor.";
    return false;
  }
  pid_ = child;
  control_fd = channels[0];
  error.clear();
  return true;
}

bool Process::Running() {
  if (pid_ < 0) return false;
  int status = 0;
  const pid_t result = waitpid(pid_, &status, WNOHANG);
  if (result == pid_ || (result < 0 && errno == ECHILD)) pid_ = -1;
  return pid_ >= 0;
}

void Process::Stop() {
  if (pid_ < 0) return;
  int status = 0;
  pid_t result = waitpid(pid_, &status, WNOHANG);
  if (!result) kill(pid_, SIGTERM);
  for (int attempt = 0; !result && attempt < 25; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    result = waitpid(pid_, &status, WNOHANG);
    if (result < 0 && errno == EINTR) result = 0;
  }
  if (!result) {
    kill(pid_, SIGKILL);
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
  }
  pid_ = -1;
}
}  // namespace aeraui::telegram
