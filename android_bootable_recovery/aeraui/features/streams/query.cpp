/* SPDX-License-Identifier: Apache-2.0 */
#include "query.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>

namespace aeraui::streams {
namespace {

constexpr size_t kMaximumResponse = 2U * 1024U * 1024U;

bool SafeRuntime(const std::string& runtime) {
  if (runtime.size() != 24 || runtime.compare(0, 18, "/tmp/aera-streams-")) return false;
  struct stat info{};
  return !lstat(runtime.c_str(), &info) && S_ISDIR(info.st_mode) && info.st_uid == 0 &&
         ((info.st_mode & 0777) == 0700 || (info.st_mode & 0777) == 0755);
}

}  // namespace

bool RunQuery(const std::string& runtime, const std::vector<std::string>& arguments,
              std::atomic<bool>& cancel, std::string& json, std::string& error) {
  json.clear();
  error.clear();
  if (!SafeRuntime(runtime) || arguments.empty() || arguments.size() > 3) {
    error = "The AERA Streams request was rejected.";
    return false;
  }
  int output[2];
  if (pipe2(output, O_CLOEXEC)) {
    error = "Could not create the extractor channel.";
    return false;
  }
  const pid_t child = fork();
  if (!child) {
    if (dup2(output[1], STDOUT_FILENO) < 0) _exit(78);
    const int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) dup2(null_fd, STDERR_FILENO);
    close(output[0]);
    close(output[1]);
    std::vector<char*> argv{ const_cast<char*>("aera-browser-jail"),
                             const_cast<char*>("--streams-engine"),
                             const_cast<char*>(runtime.c_str()) };
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execv("/system/bin/aera-browser-jail", argv.data());
    _exit(78);
  }
  close(output[1]);
  if (child < 0) {
    close(output[0]);
    error = "Could not start the extractor.";
    return false;
  }
  fcntl(output[0], F_SETFL, fcntl(output[0], F_GETFL) | O_NONBLOCK);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
  bool eof = false;
  while (!eof && !cancel.load() && std::chrono::steady_clock::now() < deadline) {
    pollfd event{ output[0], POLLIN | POLLHUP, 0 };
    const int ready = poll(&event, 1, 100);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) break;
    if (ready > 0 && (event.revents & (POLLIN | POLLHUP))) {
      char buffer[16384];
      for (;;) {
        const ssize_t count = read(output[0], buffer, sizeof(buffer));
        if (count > 0) {
          if (json.size() + static_cast<size_t>(count) > kMaximumResponse) {
            cancel.store(true);
            break;
          }
          json.append(buffer, static_cast<size_t>(count));
          continue;
        }
        if (!count) eof = true;
        if (count < 0 && errno == EINTR) continue;
        break;
      }
    }
  }
  close(output[0]);
  int status = 0;
  pid_t result = waitpid(child, &status, WNOHANG);
  if (!result) {
    kill(child, SIGTERM);
    for (int i = 0; !result && i < 10; ++i) {
      usleep(20000);
      result = waitpid(child, &status, WNOHANG);
    }
  }
  if (!result) {
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
  }
  if (cancel.load()) {
    error = "AERA Streams request cancelled.";
    return false;
  }
  // The extractor emits a bounded JSON error before returning non-zero. Let
  // the native scene parse and show that useful service message.
  if (!eof || json.empty()) {
    error = "The stream service did not return a usable response.";
    return false;
  }
  return true;
}

}  // namespace aeraui::streams
