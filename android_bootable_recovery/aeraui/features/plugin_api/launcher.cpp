/* SPDX-License-Identifier: Apache-2.0 */
#include "launcher.hpp"
#include "../../core/operation_affinity.hpp"

#include <aeraui/i18n.hpp>

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace aeraui::plugin_api {
bool Process::Start(const std::string &runtime, int &control_fd,
                    std::string &error) {
  control_fd = -1;
  if (pid_ >= 0 || runtime.size() != 19 ||
      runtime.compare(0, 13, "/tmp/aera-p2-")) {
    error = "The verified API 2 runtime path is invalid.";
    return false;
  }
  struct stat root{};
  if (lstat(runtime.c_str(), &root) != 0 || !S_ISDIR(root.st_mode) ||
      root.st_uid != 0 || (root.st_mode & 0777) != 0700) {
    error = "The API 2 runtime is no longer private.";
    return false;
  }
  const std::string loader = runtime + "/lib/ld-musl-aarch64.so.1";
  const std::string program = runtime + "/usr/bin/aera-plugin";
  const std::string libraries = runtime + "/usr/lib:" + runtime + "/lib";
  const std::string path = "PATH=" + runtime +
      "/usr/bin:/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/sbin";
  const std::string plugin_root = "AERA_PLUGIN_ROOT=" + runtime;
  const std::string data_dirs = "XDG_DATA_DIRS=" + runtime + "/usr/share";
  const std::string locale = i18n::CurrentLanguage();
  const std::string locale_env = "AERA_LOCALE=" + locale;
  const std::string lang_env = "LANG=" + locale + ".UTF-8";
  struct stat loader_info{}, program_info{};
  if (lstat(program.c_str(), &program_info) != 0 ||
      !S_ISREG(program_info.st_mode) || program_info.st_uid != 0 ||
      (program_info.st_mode & 0111) == 0) {
    error = "The API 2 runtime has no trusted executable.";
    return false;
  }
  const bool loader_present = lstat(loader.c_str(), &loader_info) == 0;
  const bool loader_trusted = loader_present && S_ISREG(loader_info.st_mode) &&
      loader_info.st_uid == 0 && (loader_info.st_mode & 0111) != 0;
  if (loader_present && !loader_trusted) {
    error = "The API 2 runtime has an untrusted dynamic loader.";
    return false;
  }
  int channels[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels) != 0) {
    error = "Could not create the bounded plugin channel.";
    return false;
  }
  const int child_control = fcntl(channels[1], F_DUPFD_CLOEXEC, 10);
  if (child_control < 0) {
    close(channels[0]);
    close(channels[1]);
    error = "Could not protect the plugin channel.";
    return false;
  }
  const pid_t child = aeraui::operation_affinity::ForkWorker();
  if (child == 0) {
    close(3);
    if (dup2(child_control, 4) < 0) _exit(78);
    close(child_control);
    if (setpgid(0, 0) != 0 || chdir(runtime.c_str()) != 0 ||
        syscall(SYS_close_range, 5U, ~0U, 0) != 0) {
      _exit(78);
    }
    char *const arguments[] = {
        const_cast<char *>(loader.c_str()),
        const_cast<char *>("--library-path"),
        const_cast<char *>(libraries.c_str()),
        const_cast<char *>(program.c_str()),
        const_cast<char *>("--aera-host-api=2"), nullptr};
    char *const environment[] = {
        const_cast<char *>(path.c_str()),
        const_cast<char *>("HOME=/tmp"),
        const_cast<char *>("TMPDIR=/tmp"),
        const_cast<char *>(lang_env.c_str()),
        const_cast<char *>(locale_env.c_str()),
        const_cast<char *>("AERA_PLUGIN_FD=4"),
        const_cast<char *>("AERA_HOST_API=2"),
        const_cast<char *>(plugin_root.c_str()),
        const_cast<char *>(data_dirs.c_str()), nullptr};
    // Most API 2 plugins are intentionally static, keeping their package small
    // and self-contained. Execute those directly. A dynamic plugin whose ELF
    // interpreter is not present in recovery returns here with ENOENT; only
    // then fall back to its verified, root-owned packaged musl loader.
    char *const direct_arguments[] = {
        const_cast<char *>(program.c_str()),
        const_cast<char *>("--aera-host-api=2"), nullptr};
    execve(program.c_str(), direct_arguments, environment);
    if (loader_trusted) execve(loader.c_str(), arguments, environment);
    _exit(78);
  }
  close(child_control);
  close(channels[1]);
  if (child < 0) {
    close(channels[0]);
    error = "Could not start the root plugin process.";
    return false;
  }
  // Establish the group before returning so Stop() can always terminate the
  // plugin and helpers together. EACCES means the child already entered it and
  // reached execve().
  if (setpgid(child, child) != 0 && errno != EACCES) {
    kill(child, SIGKILL);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    close(channels[0]);
    error = "Could not create the root plugin process group.";
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
  if (result == pid_) {
    // Reclaim helpers left in the plugin's private process group.
    kill(-pid_, SIGKILL);
    pid_ = -1;
  } else if (result < 0 && errno == ECHILD) {
    pid_ = -1;
  }
  return pid_ >= 0;
}

void Process::Stop() {
  if (pid_ < 0) return;
  int status = 0;
  pid_t result = waitpid(pid_, &status, WNOHANG);
  if (result == 0) kill(-pid_, SIGTERM);
  for (int attempt = 0; result == 0 && attempt < 25; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    result = waitpid(pid_, &status, WNOHANG);
    if (result < 0 && errno == EINTR) result = 0;
  }
  if (result == 0) {
    kill(-pid_, SIGKILL);
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
  }
  // The main plugin may exit before its helpers. They share this process group.
  kill(-pid_, SIGKILL);
  pid_ = -1;
}
}  // namespace aeraui::plugin_api
