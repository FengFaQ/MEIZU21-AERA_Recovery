/* SPDX-License-Identifier: Apache-2.0 */
#include "aera_flash_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace aera::flashlog {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> active{false};
const char *const files[] = {"summary.txt", "summary.txt.tmp", "logcat.txt", "logcat.previous.txt",
    "recovery.log", "recovery.previous.log", "kernel.log", "kernel.previous.log", ".owner"};

bool Write(int fd, const char *data, size_t bytes) {
  while (bytes) {
    const ssize_t count = write(fd, data, bytes);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += count; bytes -= count;
  }
  return true;
}
bool Text(const std::string &path, const std::string &text, bool append = false) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
      (append ? O_APPEND : O_TRUNC), 0600);
  if (fd < 0) return false;
  const bool okay = Write(fd, text.data(), text.size());
  fsync(fd); close(fd);
  return okay;
}
bool Stem(const std::string &name) {
  if (name.size() != 28 || name.compare(0, 6, "flash_") || name[14] != '_' || name[21] != '_') return false;
  for (size_t i = 6; i < 21; ++i)
    if (i != 14 && !std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  for (size_t i = 22; i < 28; ++i)
    if (!std::isalnum(static_cast<unsigned char>(name[i]))) return false;
  return true;
}
bool ArchiveName(const std::string &name) {
  if (name.size() < 28 || !Stem(name.substr(0, 28))) return false;
  const auto suffix = name.substr(28);
  return suffix == "_SUCCESS.zip" || suffix == "_FAILED.zip" || suffix == "_INCOMPLETE.zip";
}
struct Entry { std::string name; struct stat st{}; };
std::vector<Entry> Entries(const std::string &root, bool raw = false) {
  std::vector<Entry> result;
  const int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return result;
  DIR *dir = fdopendir(fd);
  if (!dir) { close(fd); return result; }
  while (auto *item = readdir(dir)) {
    const std::string name = item->d_name;
    if (!ArchiveName(name) && !(raw && Stem(name))) continue;
    Entry entry; entry.name = name;
    if (fstatat(fd, name.c_str(), &entry.st, AT_SYMLINK_NOFOLLOW)) continue;
    if (ArchiveName(name) ? !S_ISREG(entry.st.st_mode) : !S_ISDIR(entry.st.st_mode)) continue;
    result.push_back(entry);
  }
  closedir(dir);
  std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) {
    if (a.st.st_mtim.tv_sec != b.st.st_mtim.tv_sec) return a.st.st_mtim.tv_sec > b.st.st_mtim.tv_sec;
    if (a.st.st_mtim.tv_nsec != b.st.st_mtim.tv_nsec) return a.st.st_mtim.tv_nsec > b.st.st_mtim.tv_nsec;
    return a.name > b.name;
  });
  return result;
}
bool Owned(const std::string &path) {
  const int fd = open((path + "/.owner").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  char marker[32]{};
  const auto count = read(fd, marker, sizeof(marker)); close(fd);
  return count == 18 && !memcmp(marker, "AERA_FLASH_LOG_V1\n", 18);
}
void RemoveRaw(const std::string &path) {
  if (!Owned(path)) return;
  const auto stem = path.substr(path.find_last_of('/') + 1);
  if (!Stem(stem)) return;
  for (const auto &root : PlatformDirectories())
    for (const auto *suffix : {"_SUCCESS.zip.part", "_FAILED.zip.part", "_INCOMPLETE.zip.part"})
      unlink((root + "/" + stem + suffix).c_str());
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return;
  for (const auto *file : files) unlinkat(fd, file, 0);
  close(fd); rmdir(path.c_str());
}
void Prune(const std::string &root, size_t keep) {
  size_t count = 0;
  for (const auto &entry : Entries(root, true)) {
    const auto path = root + "/" + entry.name;
    if (S_ISDIR(entry.st.st_mode) && !Owned(path)) continue;
    if (++count <= keep) continue;
    if (S_ISDIR(entry.st.st_mode)) RemoveRaw(path);
    else unlink(path.c_str());
  }
}
void PruneAll(const std::string &protected_path = "") {
  struct Item { std::string path, name; mode_t mode; timespec modified; };
  std::vector<Item> entries;
  for (const auto &root : PlatformDirectories())
    for (const auto &entry : Entries(root, true)) {
      const auto path = root + "/" + entry.name;
      if (path == protected_path || (S_ISDIR(entry.st.st_mode) && !Owned(path))) continue;
      entries.push_back({path, entry.name, entry.st.st_mode, entry.st.st_mtim});
    }
  std::sort(entries.begin(), entries.end(), [](const Item &a, const Item &b) {
    if (a.modified.tv_sec != b.modified.tv_sec) return a.modified.tv_sec > b.modified.tv_sec;
    if (a.modified.tv_nsec != b.modified.tv_nsec) return a.modified.tv_nsec > b.modified.tv_nsec;
    return a.name > b.name;
  });
  const size_t keep = protected_path.empty() ? 2 : 1;
  for (size_t i = keep; i < entries.size(); ++i) {
    if (S_ISDIR(entries[i].mode)) RemoveRaw(entries[i].path);
    else unlink(entries[i].path.c_str());
  }
}
int Lock(const std::string &root) {
  const int fd = open((root + "/.capture.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
  return fd;
}
bool Package(const Options &options, const std::string &path, const std::string &suffix) {
  if (!options.archive) return false;
  const auto root = options.archive_directory.empty() ? options.directory : options.archive_directory;
  const std::string final = root + "/" + path.substr(path.find_last_of('/') + 1) + suffix + ".zip";
  const std::string partial = final + ".part";
  unlink(partial.c_str());
  if (!options.archive(path, partial) || rename(partial.c_str(), final.c_str())) {
    unlink(partial.c_str()); return false;
  }
  struct stat started{};
  if (!lstat((path + "/.owner").c_str(), &started)) {
    const timespec times[] = {started.st_mtim, started.st_mtim};
    utimensat(AT_FDCWD, final.c_str(), times, AT_SYMLINK_NOFOLLOW);
  }
  const int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd >= 0) { fsync(fd); close(fd); }
  if (options.permissions) options.permissions(final);
  RemoveRaw(path);
  return true;
}
void Recover(const Options &options) {
  for (const auto &entry : Entries(options.directory, true)) {
    if (!S_ISDIR(entry.st.st_mode)) continue;
    const auto path = options.directory + "/" + entry.name;
    if (!Owned(path)) continue;
    Text(path + "/summary.txt", "\nRecording recovered after an interrupted process or unfinished report packaging.\n", true);
    Package(options, path, "_INCOMPLETE");
  }
  Prune(options.directory, 2);
}

// Two bounded segments retain the most recent output without growing forever.
class Tail {
 public:
  Tail(std::string current, std::string previous, size_t limit)
      : current_(std::move(current)), previous_(std::move(previous)), limit_(std::max<size_t>(1, limit / 2)) {
    Open();
  }
  ~Tail() { if (fd_ >= 0) close(fd_); }
  void Add(const char *data, size_t count) {
    while (fd_ >= 0 && count) {
      if (size_ == limit_) {
        close(fd_); fd_ = -1;
        if (rename(current_.c_str(), previous_.c_str())) return;
        size_ = 0; Open();
      }
      const size_t bytes = std::min(count, limit_ - size_);
      if (fd_ < 0 || !Write(fd_, data, bytes)) { if (fd_ >= 0) close(fd_); fd_ = -1; return; }
      data += bytes; count -= bytes; size_ += bytes;
    }
  }
 private:
  void Open() { fd_ = open(current_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600); }
  std::string current_, previous_;
  size_t limit_, size_ = 0;
  int fd_ = -1;
};
class Child {
 public:
  explicit Child(const std::vector<std::string> &command) {
    if (command.empty()) return;
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) return;
    std::vector<char *> argv;
    for (const auto &arg : command) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    const int result = posix_spawn(&pid_, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr); posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (result) { close(pipefd[0]); pid_ = -1; return; }
    fd_ = pipefd[0]; fcntl(fd_, F_SETFL, O_NONBLOCK);
  }
  ~Child() { Stop(); if (fd_ >= 0) close(fd_); }
  int fd() const { return fd_; }
  void Drain(Tail &tail) {
    if (fd_ < 0) return;
    std::array<char, 16384> buffer{};
    // Yield even when a noisy service continuously fills the pipe.
    for (int i = 0; i < 64; ++i) {
      const auto count = read(fd_, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) { --i; continue; }
      if (count <= 0) break;
      tail.Add(buffer.data(), count);
    }
  }
  void Stop() {
    if (pid_ <= 0) return;
    int status;
    pid_t result;
    do { result = waitpid(pid_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0) {
      kill(-pid_, SIGTERM);
      for (int i = 0; i < 20 && (result = waitpid(pid_, &status, WNOHANG)) == 0; ++i) usleep(10000);
      if (result == 0) { kill(-pid_, SIGKILL); while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {} }
    }
    pid_ = -1;
  }
 private:
  pid_t pid_ = -1;
  int fd_ = -1;
};
}  // namespace

bool SensitiveSetting(const std::string &key) {
  std::string lower = key;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  if (lower == "aera_browser_cookies" || lower == "aera_installer_author") return false;
  for (const auto *part : {"password", "passwd", "passphrase", "credential", "secret", "token", "private_key", "certificate_key", "encryption_key", "trusted", "cookie", "authorization", "_auth", "psk", "pattern", "_pin"})
    if (lower.find(part) != std::string::npos) return true;
  return lower == "tw_crypto_pw" || lower == "bpasspass";
}
std::string SingleLine(const std::string &value) {
  std::string result;
  for (unsigned char c : value.substr(0, 4096)) {
    if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else if (c >= 32 && c != 127) result += c;
  }
  return result;
}

struct Recording::Session {
  Options options;
  std::string path;
  int lock = -1, recovery = -1;
  off_t offset = 0;
  std::atomic<bool> stop{false};
  pthread_t thread{};
  bool started = false;
  std::unique_ptr<Child> logcat_process;
  Clock::time_point begin = Clock::now();

  explicit Session(Options value) : options(std::move(value)) {}
  ~Session() {
    stop.store(true);
    if (started) pthread_join(thread, nullptr);
    if (recovery >= 0) close(recovery);
    if (lock >= 0) close(lock);
    active.store(false);
  }
  void ReadRecovery(Tail &tail, bool final = false) {
    if (recovery < 0) return;
    struct stat st{};
    if (!fstat(recovery, &st)) {
      if (st.st_size < offset) offset = 0;
      if (final) offset = std::max(offset, st.st_size - static_cast<off_t>(options.recovery_limit));
    }
    std::array<char, 16384> buffer{};
    const size_t chunks = final ? (options.recovery_limit + buffer.size() - 1) / buffer.size() : 64;
    for (size_t i = 0; i < chunks; ++i) {
      const auto count = pread(recovery, buffer.data(), buffer.size(), offset);
      if (count < 0 && errno == EINTR) { --i; continue; }
      if (count <= 0) break;
      tail.Add(buffer.data(), count); offset += count;
    }
  }
  void Run() {
    setpriority(PRIO_PROCESS, syscall(SYS_gettid), 10);
    Tail logcat(path + "/logcat.txt", path + "/logcat.previous.txt", options.logcat_limit);
    Tail output(path + "/recovery.log", path + "/recovery.previous.log", options.recovery_limit);
    auto &child = *logcat_process;
    if (child.fd() < 0) {
      const char *message = "Logcat capture could not be started.\n";
      logcat.Add(message, strlen(message));
    }
    while (!stop.load()) {
      child.Drain(logcat); ReadRecovery(output);
      pollfd descriptor{child.fd(), POLLIN, 0};
      poll(child.fd() >= 0 ? &descriptor : nullptr, child.fd() >= 0 ? 1 : 0, 100);
      if (descriptor.revents & POLLHUP) usleep(100000);
    }
    child.Stop(); child.Drain(logcat); ReadRecovery(output, true);
  }
};

Recording::Recording(const std::string &file, const std::string &method, const std::string &details) {
  bool expected = false;
  if (!active.compare_exchange_strong(expected, true)) return;
  auto session = std::make_unique<Session>(PlatformOptions(file, method, details));
  if (session->options.directory.empty() || (session->lock = Lock(session->options.directory)) < 0) return;
  auto previous = PlatformOptions("", "", "");
  previous.directory = session->options.directory;
  Recover(previous);
  char date[32]; const time_t now = time(nullptr); struct tm local{};
  localtime_r(&now, &local); strftime(date, sizeof(date), "flash_%Y%m%d_%H%M%S_", &local);
  std::string pattern = session->options.directory + "/" + date + "XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back(0);
  if (!mkdtemp(buffer.data())) return;
  session->path = buffer.data();
  if (!Text(session->path + "/.owner", "AERA_FLASH_LOG_V1\n") ||
      !Text(session->path + "/summary.txt", session->options.summary + "\nRecording state: unfinished until a final result is appended.\n")) return;
  PruneAll(session->path);
  session->recovery = open(session->options.recovery_log.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  struct stat st{};
  if (session->recovery >= 0) {
    if (fstat(session->recovery, &st) || !S_ISREG(st.st_mode)) { close(session->recovery); session->recovery = -1; }
    else session->offset = std::max<off_t>(0, st.st_size - 65536);
  }
  session->logcat_process = std::make_unique<Child>(session->options.logcat);
  if (pthread_create(&session->thread, nullptr, [](void *data) -> void * {
        static_cast<Session *>(data)->Run(); return nullptr;
      }, session.get()) == 0) session->started = true;
  else Text(session->path + "/logcat.txt", "Recording worker could not be started.\n");
  session_ = std::move(session);
}
Recording::~Recording() = default;
int Recording::Finish(int result) {
  if (!session_) return result;
  auto &session = *session_;
  session.stop.store(true);
  if (session.started) { pthread_join(session.thread, nullptr); session.started = false; }
  else {
    Tail output(session.path + "/recovery.log", session.path + "/recovery.previous.log", session.options.recovery_limit);
    session.ReadRecovery(output, true);
  }
  {
    Tail kernel(session.path + "/kernel.log", session.path + "/kernel.previous.log", 2 * 1024 * 1024);
    Child child(session.options.kernel);
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (child.fd() >= 0 && Clock::now() < deadline) {
      child.Drain(kernel); pollfd descriptor{child.fd(), POLLIN, 0};
      poll(&descriptor, 1, 30);
      if (descriptor.revents & POLLHUP) { child.Drain(kernel); break; }
    }
  }
  std::string end = "\nResult: " + std::string(result == 0 ? "SUCCESS" : "FAILED") +
      "\nReturn code: " + std::to_string(result) + "\nDuration: " +
      std::to_string(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - session.begin).count()) + " seconds\n";
  if (session.options.final_summary) end += session.options.final_summary();
  end += "\nLogcat/recovery output is size-limited; earlier output may have been rotated.\n";
  const auto summary = session.path + "/summary.txt";
  const auto temporary_summary = summary + ".tmp";
  if (!Text(temporary_summary, session.options.summary + end) ||
      rename(temporary_summary.c_str(), summary.c_str())) {
    unlink(temporary_summary.c_str());
    Text(summary, end, true);
  }
  session.options.archive_directory = PlatformOptions("", "", "").archive_directory;
  if (!Package(session.options, session.path, result == 0 ? "_SUCCESS" : "_FAILED")) {
    session.options.archive_directory = session.options.directory;
    Package(session.options, session.path, result == 0 ? "_SUCCESS" : "_FAILED");
  }
  PruneAll();
  session_.reset();
  return result;
}
std::vector<Report> Reports() {
  std::vector<Report> reports;
  const auto directories = PlatformDirectories();
  for (const auto &directory : directories) {
    const int lock = Lock(directory);
    if (lock >= 0) {
      auto options = PlatformOptions("", "", ""); options.directory = directory;
      Recover(options); close(lock);
    }
  }
  if (!active.load()) PruneAll();
  for (const auto &directory : directories) {
    for (const auto &entry : Entries(directory))
      reports.push_back({entry.name, directory + "/" + entry.name,
          static_cast<uint64_t>(entry.st.st_size), entry.st.st_mtim.tv_sec,
          static_cast<int32_t>(entry.st.st_mtim.tv_nsec)});
  }
  std::sort(reports.begin(), reports.end(), [](const Report &a, const Report &b) {
    return a.timestamp == b.timestamp ? a.nanoseconds > b.nanoseconds : a.timestamp > b.timestamp;
  });
  if (reports.size() > 2) reports.resize(2);
  return reports;
}
int OpenReport(const std::string &name, uint64_t *bytes) {
  if (!ArchiveName(name)) return -1;
  for (const auto &report : Reports()) if (report.name == name) {
    const int fd = open(report.path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) continue;
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); continue; }
    *bytes = st.st_size; return fd;
  }
  return -1;
}
}  // namespace aera::flashlog
