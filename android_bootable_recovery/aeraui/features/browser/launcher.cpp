// SPDX-License-Identifier: Apache-2.0
#include "launcher.hpp"
#include "../../core/operation_affinity.hpp"
#include "protocol.hpp"
#include "audio/bridge.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace aeraui::web {
bool BrowserProcess::Start(const std::string &runtime, int &frame_fd,
                           int &control_fd, std::string &error) {
  frame_fd = control_fd = -1;
  if (pid_ >= 0 || runtime.size() != 20 ||
      runtime.compare(0, 14, "/tmp/aera-web-")) {
    error = "The verified browser runtime path is invalid."; return false;
  }
  struct stat root{};
  if (lstat(runtime.c_str(), &root) || !S_ISDIR(root.st_mode) || root.st_uid ||
      (root.st_mode & 0777) != 0700) {
    error = "The browser runtime is no longer private."; return false;
  }
  int frame = memfd_create("aera-browser-pixels", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  int channels[2] = {-1, -1};
  if (frame < 0 || ftruncate(frame, kSharedBytes) ||
      fcntl(frame, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) ||
      socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels)) {
    if (frame >= 0) close(frame);
    if (channels[0] >= 0) close(channels[0]);
    if (channels[1] >= 0) close(channels[1]);
    error = "Could not create the private browser display channel."; return false;
  }
  audio_pid_ = aeraui::audio::StartBridge();
  // Duplicate above the fixed child ABI first, so dup2 cannot overwrite the
  // other source if recovery happened to have fd 3 or 4 available.
  const int child_frame = fcntl(frame, F_DUPFD_CLOEXEC, 10);
  const int child_control = fcntl(channels[1], F_DUPFD_CLOEXEC, 10);
  if (child_frame < 0 || child_control < 0) {
    if (child_frame >= 0) close(child_frame);
    if (child_control >= 0) close(child_control);
    close(frame); close(channels[0]); close(channels[1]);
    error = "Could not protect browser display descriptors."; return false;
  }
  pid_t child = aeraui::operation_affinity::ForkWorker();
  if (!child) {
    if (dup2(child_frame, 3) < 0 || dup2(child_control, 4) < 0) _exit(78);
    close(child_frame); close(child_control);
    execl("/system/bin/aera-browser-jail", "aera-browser-jail", "--browser",
          runtime.c_str(), nullptr);
    _exit(78);
  }
  close(child_frame); close(child_control); close(channels[1]);
  if (child < 0) {
    close(frame); close(channels[0]);
    StopAudio();
    error = "Could not start the isolated browser supervisor."; return false;
  }
  pid_ = child; frame_fd = frame; control_fd = channels[0]; error.clear();
  return true;
}

bool BrowserProcess::Running() {
  if (pid_ < 0) return false;
  int status = 0;
  const pid_t result = waitpid(pid_, &status, WNOHANG);
  if (result == pid_ || (result < 0 && errno == ECHILD)) {
    pid_ = -1;
    StopAudio();
  }
  return pid_ >= 0;
}

void BrowserProcess::Stop() {
  if (pid_ >= 0) {
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
  StopAudio();
}

void BrowserProcess::StopAudio() {
  if (audio_pid_ < 0) return;
  int status = 0;
  pid_t result = waitpid(audio_pid_, &status, WNOHANG);
  if (!result) kill(audio_pid_, SIGTERM);
  for (int attempt = 0; !result && attempt < 25; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    result = waitpid(audio_pid_, &status, WNOHANG);
    if (result < 0 && errno == EINTR) result = 0;
  }
  if (!result) {
    kill(audio_pid_, SIGKILL);
    while (waitpid(audio_pid_, &status, 0) < 0 && errno == EINTR) {}
  }
  audio_pid_ = -1;
}
}  // namespace aeraui::web
