/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "payload_workers.hpp"

extern char **environ;
namespace aeraui::payload {
inline const char *OtaripperExecutable() {
#ifndef __ANDROID__
  // Host integration tests only. Recovery always uses its trusted ramdisk binary.
  if (const char *test = getenv("AERA_TEST_OTARIPPER")) return test;
#endif
  return "/system/bin/aera-otaripper";
}

// Extracts to files or an exclusively claimed block fd. The caller validated the manifest,
// names, extents, size limits and free space, and verifies all resulting hashes.
inline bool RunOtaripper(int package_fd, const std::vector<std::string> &names,
                        const std::string &directory, uint64_t total,
                        const std::function<void(const std::string &, uint64_t, uint64_t)> &progress,
                        std::string &error, int target_fd = -1,
                        const std::string &manifest_hash = {}, unsigned worker_limit = 0) {
  const bool direct = target_fd >= 0;
  if (direct && (names.size() != 1 || manifest_hash.size() != 32)) {
    error = "Invalid direct-flash request"; return false;
  }
  struct TargetDescriptors {
    int source = -1, inherited = -1;
    ~TargetDescriptors() { if (source >= 0) close(source); if (inherited >= 0) close(inherited); }
  } target;
  std::string selected;
  for (const auto &name : names) {
    if (!selected.empty()) selected += ',';
    selected += name;
  }
  const unsigned workers = AvailableWorkers();
  const auto threads = std::to_string(worker_limit ? std::min(workers, worker_limit) : workers);
  // Give the child the exact already-open package, not a pathname that could be
  // replaced between validation and extraction. No shell interpretation.
  int source = fcntl(package_fd, F_DUPFD_CLOEXEC, 200);
  int inherited = source >= 0 ? fcntl(source, F_DUPFD_CLOEXEC, 200) : -1;
  int pipefd[2];
  if (inherited < 0) { if (source >= 0) close(source); error = "Cannot retain package descriptor"; return false; }
  if (pipe2(pipefd, O_CLOEXEC)) {
    close(source); close(inherited); error = "Cannot create extractor progress pipe"; return false;
  }
  std::vector<std::string> args = {OtaripperExecutable(), "--aera", "--strict", "--no-open",
      "--threads", threads, "--partitions", selected, "--output-dir", directory,
      "/proc/self/fd/" + std::to_string(inherited)};
  if (direct) {
    target.source = fcntl(target_fd, F_DUPFD_CLOEXEC, 300);
    target.inherited = target.source >= 0 ? fcntl(target.source, F_DUPFD_CLOEXEC, 300) : -1;
    if (target.inherited < 0) {
      close(source); close(inherited); close(pipefd[0]); close(pipefd[1]);
      error = "Cannot retain direct target descriptor"; return false;
    }
    std::string hex;
    for (unsigned char c : manifest_hash) { hex += "0123456789abcdef"[c >> 4]; hex += "0123456789abcdef"[c & 15]; }
    args.insert(args.end(), {"--aera-block-fd", std::to_string(target.inherited), "--aera-manifest", hex});
  }
  std::vector<char *> argv;
  for (auto &arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  const bool initialized = rc == 0;
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, source, inherited);
  if (!rc && direct) rc = posix_spawn_file_actions_adddup2(&actions, target.source, target.inherited);
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!rc) rc = posix_spawn_file_actions_addclose(&actions, source);
  if (!rc) rc = posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  if (!rc) rc = posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  pid_t child = -1;
  const std::string stage = (direct ? "Direct flashing with otaripper (" : "Extracting with otaripper (") + threads + " workers)";
  if (progress) progress(stage, 0, total * 2);
  if (!rc) rc = posix_spawn(&child, argv[0], &actions, nullptr, argv.data(), environ);
  if (initialized) posix_spawn_file_actions_destroy(&actions);
  close(source); close(inherited); close(pipefd[1]);
  if (rc) {
    close(pipefd[0]); error = "Cannot start otaripper: " + std::string(strerror(rc)); return false;
  }
  std::string line, diagnostic;
  char buffer[2048];
  uint64_t previous = 0;
  bool pipe_ok = true;
  for (;;) {
    const ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { pipe_ok = count == 0; break; }
    for (ssize_t i = 0; i < count; ++i) {
      if (buffer[i] == '\n') {
        unsigned long long done = 0, expected = 0;
        if (sscanf(line.c_str(), "AERA_PROGRESS %llu %llu", &done, &expected) == 2 &&
            expected == total && done <= total && done >= previous) {
          previous = done;
          if (progress) progress(stage, done, total * 2);
        } else {
          diagnostic += line + '\n';
          if (diagnostic.size() > 4096) diagnostic.erase(0, diagnostic.size() - 4096);
        }
        line.clear();
      } else if (line.size() < 4096) line += buffer[i];
    }
  }
  close(pipefd[0]);
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  if (!pipe_ok || waited != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
    error = std::string(direct ? "Direct flash failed; target may be incomplete: " :
        "Otaripper extraction failed (no partitions flashed): ") + diagnostic + line;
    return false;
  }
  return true;
}
} // namespace aeraui::payload
