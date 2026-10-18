/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "payload_otaripper.hpp"
#include <aeraui/backend.hpp>
#include <android-base/unique_fd.h>
#include <sstream>
#include <cstdio>
#include <set>

namespace aeraui::payload {
struct DirectTarget { std::string name; int fd; uint64_t bytes; };
using DirectProgress = std::function<void(size_t, PayloadPhase, uint64_t)>;

// Strictly bind progress to the reviewed mapping. Child output never supplies a
// pathname, descriptor or unchecked vector index to the parent.
inline bool ParseDirectProgress(const std::string& line, const std::vector<DirectTarget>& targets,
                                std::vector<uint64_t>& previous, std::vector<PayloadPhase>& phases,
                                const DirectProgress& progress) {
  std::istringstream stream(line);
  std::string marker, name, phase, trailing;
  uint64_t done = 0, total = 0;
  if (!(stream >> marker >> name >> phase >> done >> total) || stream >> trailing || marker != "AERA_PARTITION") return false;
  auto found = std::find_if(targets.begin(), targets.end(), [&](const auto& t) { return t.name == name; });
  if (found == targets.end()) return false;
  const size_t i = found - targets.begin();
  if (previous.size() != targets.size() || phases.size() != targets.size() || total != found->bytes || done > total || done < previous[i]) return false;
  PayloadPhase next;
  if (phase == "writing") next = PayloadPhase::Writing;
  else if (phase == "written" && done == total) next = PayloadPhase::Written;
  else if (phase == "failed") next = PayloadPhase::Failed;
  else return false;
  if ((phases[i] == PayloadPhase::Written || phases[i] == PayloadPhase::Failed) && phases[i] != next) return false;
  previous[i] = done; phases[i] = next;
  if (progress) progress(i, next, done);
  return true;
}

inline bool RunOtaripperBatch(int package_fd, const std::vector<DirectTarget>& targets,
                             const std::string& manifest_hash, unsigned workers,
                             const DirectProgress& progress, std::string& error, bool& launched) {
  launched = false;
  if (targets.empty() || targets.size() > 256 || manifest_hash.size() != 32 || !workers || workers > 16) {
    error = "Invalid shared-pool request"; return false;
  }
  std::set<std::string> names;
  std::set<int> fds;
  std::string selected;
  for (const auto& t : targets) {
    if (t.name.empty() || t.name.size() > 128 || !t.bytes || t.fd < 0 ||
        !std::all_of(t.name.begin(), t.name.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }) || !names.insert(t.name).second || !fds.insert(t.fd).second) {
      error = "Invalid or duplicate direct target"; return false;
    }
    if (!selected.empty()) selected += ',';
    selected += t.name;
  }
  struct Pair { android::base::unique_fd source, inherited; };
  auto duplicate = [](int fd) {
    Pair pair;
    pair.source.reset(fcntl(fd, F_DUPFD_CLOEXEC, 200));
    if (pair.source.get() >= 0) pair.inherited.reset(fcntl(pair.source.get(), F_DUPFD_CLOEXEC, 200));
    return pair;
  };
  auto package = duplicate(package_fd);
  if (package.inherited.get() < 0) { error = "Cannot retain package descriptor"; return false; }
  int raw_pipe[2];
  if (pipe2(raw_pipe, O_CLOEXEC)) { error = "Cannot create shared progress pipe"; return false; }
  android::base::unique_fd read_end(raw_pipe[0]), write_end(raw_pipe[1]);
  std::vector<Pair> retained;
  std::string hash;
  for (unsigned char c : manifest_hash) { hash += "0123456789abcdef"[c >> 4]; hash += "0123456789abcdef"[c & 15]; }
  std::vector<std::string> args = {OtaripperExecutable(), "--aera", "--strict", "--no-open", "--threads", std::to_string(workers),
    "--partitions", selected, "--output-dir", "/tmp", "--aera-manifest", hash,
    "/proc/self/fd/" + std::to_string(package.inherited.get())};
  for (const auto& t : targets) {
    retained.push_back(duplicate(t.fd));
    if (retained.back().inherited.get() < 0) { error = "Cannot retain target descriptors"; return false; }
    args.insert(args.end(), {"--aera-block-fd", std::to_string(retained.back().inherited.get())});
  }
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  const bool initialized = !rc;
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, package.source.get(), package.inherited.get());
  for (const auto& pair : retained) if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, pair.source.get(), pair.inherited.get());
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDERR_FILENO);
  if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  pid_t child = -1;
  if (!rc) rc = posix_spawn(&child, argv[0], &actions, nullptr, argv.data(), environ);
  if (initialized) posix_spawn_file_actions_destroy(&actions);
  write_end.reset();
  if (rc) { error = "Cannot start shared otaripper: " + std::string(strerror(rc)); return false; }
  launched = true;
  std::vector<uint64_t> previous(targets.size());
  std::vector<PayloadPhase> phases(targets.size(), PayloadPhase::Queued);
  std::string line, diagnostic;
  bool pipe_ok = true, protocol_ok = true;
  char buffer[4096];
  for (;;) {
    const ssize_t count = read(read_end.get(), buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { pipe_ok = count == 0; break; }
    for (ssize_t i = 0; i < count; ++i) {
      if (buffer[i] == '\n') {
        if (line.rfind("AERA_PARTITION ", 0) == 0) {
          if (!ParseDirectProgress(line, targets, previous, phases, progress)) protocol_ok = false;
        } else if (line.rfind("AERA_AFFINITY ", 0) == 0) {
          // Successful launches must retain affinity diagnostics too, rather
          // than keeping them only in the error buffer returned on failure.
          fprintf(stderr, "%s\n", line.c_str());
        } else if (line.rfind("AERA_PROGRESS ", 0) != 0) {
          diagnostic += line + '\n';
          if (diagnostic.size() > 8192) diagnostic.erase(0, diagnostic.size() - 8192);
        }
        line.clear();
      } else if (line.size() < 4096) line += buffer[i];
      else protocol_ok = false;
    }
  }
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  const bool complete = std::all_of(phases.begin(), phases.end(), [](auto p) { return p == PayloadPhase::Written; });
  if (!pipe_ok || !protocol_ok || waited != child || !WIFEXITED(status) || WEXITSTATUS(status) || !complete) {
    error = "Shared-pool writer failed; written targets require verification: " + diagnostic + line;
    return false;
  }
  return true;
}
} // namespace aeraui::payload
