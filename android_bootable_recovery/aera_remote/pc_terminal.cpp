/* SPDX-License-Identifier: Apache-2.0 */
#include "pc_terminal.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#ifndef AERA_PC_TERMINAL_BINARY
#define AERA_PC_TERMINAL_BINARY "/system/bin/ttyd"
#endif
#ifndef AERA_PC_TERMINAL_SHELL
#define AERA_PC_TERMINAL_SHELL "/system/bin/sh"
#endif
extern char **environ;
namespace aera::pc::terminal {
namespace {
std::mutex mutex;
pid_t child = -1;
std::string directory, session, cookie, credential;
std::string Token() {
  std::array<unsigned char, 32> bytes{};
  if (RAND_bytes(bytes.data(), bytes.size()) != 1) return {};
  const char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto value : bytes) {
    result += digits[value >> 4]; result += digits[value & 15];
  }
  return result;
}
void StopLocked() {
  if (child > 0) {
    const pid_t pid = child;
    if (waitpid(pid, nullptr, WNOHANG) == 0) {
      // ttyd shuts down its PTYs and their shell children on SIGTERM.
      kill(pid, SIGTERM);
      bool exited = false;
      for (int i = 0; i < 40; ++i) {
        if (waitpid(pid, nullptr, WNOHANG) != 0) { exited = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
      if (!exited) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
    }
  }
  child = -1;
  if (!directory.empty()) { unlink((directory + "/tty.sock").c_str()); rmdir(directory.c_str()); }
  directory.clear(); session.clear(); cookie.clear(); credential.clear();
}
int Connect(const std::string &path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un address{}; address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) { close(fd); return -1; }
  memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    close(fd); return -1;
  }
  return fd;
}
struct Buffer {
  std::array<char, 32768> data{};
  size_t begin = 0, end = 0;
  bool Empty() const { return begin == end; }
  void Consume(size_t count) { begin += count; if (begin == end) begin = end = 0; }
};
}
bool Available() { return access(AERA_PC_TERMINAL_BINARY, X_OK) == 0; }
std::string Open(const std::string &owner, std::string *error) {
  std::lock_guard<std::mutex> guard(mutex);
  if (child > 0 && waitpid(child, nullptr, WNOHANG) == 0) {
    if (session == owner) return cookie;
    *error = "Another computer has an open terminal session."; return {};
  }
  child = -1; StopLocked();
  if (!Available()) { *error = "The terminal backend is not included in this build."; return {}; }
  cookie = Token(); const auto password = Token();
  if (cookie.empty() || password.empty()) { StopLocked(); *error = "Could not create a terminal session."; return {}; }
  std::string basic = "aera:" + password;
  std::array<unsigned char, 128> encoded{};
  const int length = EVP_EncodeBlock(encoded.data(), reinterpret_cast<const unsigned char *>(basic.data()), basic.size());
  credential.assign(reinterpret_cast<const char *>(encoded.data()), length);
  char temporary[] = "/tmp/aera-pc-terminal-XXXXXX";
  if (!mkdtemp(temporary)) { StopLocked(); *error = "Could not prepare the terminal socket."; return {}; }
  directory = temporary; session = owner;
  std::string socket_path = directory + "/tty.sock";
  // Never expose a second listener; the directory is private (0700).
  std::array<char *, 20> args{
    const_cast<char *>(AERA_PC_TERMINAL_BINARY),
    const_cast<char *>("-i"), socket_path.data(),
    const_cast<char *>("-b"), const_cast<char *>("/terminal"),
    const_cast<char *>("-c"), basic.data(),
    const_cast<char *>("-W"), const_cast<char *>("-m"), const_cast<char *>("1"),
    const_cast<char *>("-t"), const_cast<char *>("fontSize=15"),
    const_cast<char *>("-t"), const_cast<char *>("disableReconnect=true"),
    const_cast<char *>("-t"), const_cast<char *>("theme={\"background\":\"#191b1f\",\"foreground\":\"#f4f6f8\",\"cursor\":\"#16c8ff\"}"),
    const_cast<char *>(AERA_PC_TERMINAL_SHELL), nullptr
  };
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  const int result = posix_spawn(&child, AERA_PC_TERMINAL_BINARY, &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result != 0) { child = -1; StopLocked(); *error = "Could not start the terminal backend."; return {}; }
  for (int i = 0; i < 80; ++i) {
    struct stat info{};
    if (lstat(socket_path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode)) {
      chmod(socket_path.c_str(), 0600); return cookie;
    }
    if (waitpid(child, nullptr, WNOHANG) != 0) { child = -1; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  StopLocked(); *error = "The terminal backend did not become ready."; return {};
}
bool Close(const std::string &owner) {
  std::lock_guard<std::mutex> guard(mutex);
  if (!session.empty() && session != owner) return false;
  StopLocked(); return true;
}
void Stop() { std::lock_guard<std::mutex> guard(mutex); StopLocked(); }
bool Proxy(SSL *tls, const Request &request, const std::string &provided,
           const std::function<bool(const std::string &)> &authorized) {
  std::string owner, socket_path, basic;
  {
    std::lock_guard<std::mutex> guard(mutex);
    if (child <= 0 || provided.empty() || provided != cookie) return false;
    owner = session; socket_path = directory + "/tty.sock"; basic = credential;
  }
  if (!authorized(owner)) return false;
  const int upstream = Connect(socket_path);
  if (upstream < 0) return false;
  std::string headers = "GET " + request.path + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Basic " +
      basic + "\r\nAccept-Encoding: gzip\r\nConnection: " + (request.websocket ? "Upgrade" : "close") + "\r\n";
  if (request.websocket) {
    headers += "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + request.websocket_key + "\r\n";
    if (!request.websocket_protocol.empty()) headers += "Sec-WebSocket-Protocol: " + request.websocket_protocol + "\r\n";
  }
  headers += "\r\n";
  timeval timeout{2, 0};
  setsockopt(upstream, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  size_t sent = 0;
  while (sent < headers.size()) {
    const ssize_t count = send(upstream, headers.data() + sent, headers.size() - sent, MSG_NOSIGNAL);
    if (count > 0) sent += count;
    else if (count < 0 && errno == EINTR) continue;
    else { close(upstream); return false; }
  }
  const int client = SSL_get_fd(tls), flags = fcntl(client, F_GETFL, 0);
  if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) != 0 ||
      fcntl(upstream, F_SETFL, O_NONBLOCK) != 0) { close(upstream); return false; }
  SSL_set_mode(tls, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  Buffer to_client, to_backend;
  bool read_wants_write = false, write_wants_read = false, upstream_closed = false;
  auto valid = [&] {
    { std::lock_guard<std::mutex> guard(mutex);
      if (provided != cookie || owner != session || child <= 0) return false; }
    return authorized(owner);
  };
  // Fixed queues provide backpressure instead of unbounded terminal buffers.
  while (valid()) {
    pollfd sockets[] = {
      {client, static_cast<short>((to_backend.Empty() || write_wants_read ? POLLIN : 0) |
          (!to_client.Empty() || read_wants_write ? POLLOUT : 0)), 0},
      {upstream, static_cast<short>((to_client.Empty() && !upstream_closed ? POLLIN : 0) |
          (!to_backend.Empty() ? POLLOUT : 0)), 0}
    };
    const int ready = poll(sockets, 2, SSL_pending(tls) > 0 && to_backend.Empty() ? 0 : 250);
    if (ready < 0) { if (errno == EINTR) continue; break; }
    if (!valid()) break;
    if (!to_client.Empty() && (sockets[0].revents & (POLLOUT | POLLIN))) {
      const int count = SSL_write(tls, to_client.data.data() + to_client.begin, to_client.end - to_client.begin);
      if (count > 0) { to_client.Consume(count); write_wants_read = false; }
      else {
        const int error = SSL_get_error(tls, count); write_wants_read = error == SSL_ERROR_WANT_READ;
        if (!write_wants_read && error != SSL_ERROR_WANT_WRITE) break;
      }
    }
    if (!to_backend.Empty() && (sockets[1].revents & POLLOUT)) {
      const ssize_t count = send(upstream, to_backend.data.data() + to_backend.begin,
          to_backend.end - to_backend.begin, MSG_NOSIGNAL);
      if (count > 0) to_backend.Consume(count);
      else if (count < 0 && errno != EAGAIN && errno != EINTR) break;
    }
    if (to_backend.Empty() && (SSL_pending(tls) > 0 ||
        (sockets[0].revents & (POLLIN | (read_wants_write ? POLLOUT : 0))))) {
      const int count = SSL_read(tls, to_backend.data.data(), to_backend.data.size());
      if (count > 0) { to_backend.end = count; read_wants_write = false; }
      else {
        const int error = SSL_get_error(tls, count); read_wants_write = error == SSL_ERROR_WANT_WRITE;
        if (!read_wants_write && error != SSL_ERROR_WANT_READ) break;
      }
    }
    if (to_client.Empty() && !upstream_closed && (sockets[1].revents & (POLLIN | POLLHUP))) {
      const ssize_t count = read(upstream, to_client.data.data(), to_client.data.size());
      if (count > 0) to_client.end = count;
      else if (count == 0) upstream_closed = true;
      else if (errno != EAGAIN && errno != EINTR) break;
    }
    if ((upstream_closed && to_client.Empty()) ||
        (sockets[0].revents & (POLLHUP | POLLERR | POLLNVAL)) ||
        (sockets[1].revents & (POLLERR | POLLNVAL))) break;
  }
  close(upstream); fcntl(client, F_SETFL, flags);
  if (!authorized(owner)) Close(owner);
  return true;
}
}
