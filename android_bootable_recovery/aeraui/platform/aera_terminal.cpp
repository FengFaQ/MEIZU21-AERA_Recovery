// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#include <aeraui/backend.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aeraui {
namespace {

constexpr size_t kMaximumScrollback = 2000;

class TerminalSession {
 public:
  ~TerminalSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (master_ >= 0) close(master_);
  }

  void Start(int columns, int rows) {
    std::lock_guard<std::mutex> lock(mutex_);
    columns_ = std::max(1, columns);
    rows_ = std::max(1, rows);
    ReapChildLocked();
    if (master_ < 0) Launch();
    if (master_ < 0) return;
    ResizeLocked();
  }

  void ResizeLocked() {
    winsize size{};
    size.ws_col = columns_;
    size.ws_row = rows_;
    ioctl(master_, TIOCSWINSZ, &size);
  }

  bool Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    ReapChildLocked();
    if (master_ < 0) {
      if (child_ < 0) {
        Launch();
        if (master_ >= 0) ResizeLocked();
        return master_ >= 0;
      }
      return false;
    }
    bool changed = false;
    char input[4096];
    bool disconnected = false;
    for (;;) {
      const ssize_t count = read(master_, input, sizeof(input));
      if (count > 0) {
        for (ssize_t index = 0; index < count; ++index)
          Consume(static_cast<unsigned char>(input[index]));
        changed = true;
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      if (count == 0 ||
          (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        disconnected = true;
      }
      break;
    }
    if (changed) ++updates_;
    ReapChildLocked();
    if (disconnected) CloseMasterLocked();
    if (master_ < 0 && child_ < 0) {
      Launch();
      if (master_ >= 0) {
        ResizeLocked();
        changed = true;
      }
    }
    return changed;
  }

  int Updates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return updates_;
  }

  std::vector<std::string> Lines(size_t maximum) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    const size_t stored = lines_.size() + (current_.empty() ? 0 : 1);
    const size_t wanted = std::min(maximum, stored);
    result.reserve(wanted);
    const size_t first = stored > wanted ? stored - wanted : 0;
    for (size_t index = first; index < lines_.size(); ++index)
      result.push_back(lines_[index]);
    if (!current_.empty() && result.size() < wanted) result.push_back(current_);
    return result;
  }

  void Write(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    WriteLocked(text);
  }

  void WriteLocked(const std::string& text) {
    if (master_ < 0 || text.empty()) return;
    size_t offset = 0;
    while (offset < text.size()) {
      const ssize_t count = write(master_, text.data() + offset,
                                  text.size() - offset);
      if (count > 0) {
        offset += static_cast<size_t>(count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    current_.clear();
    cursor_ = 0;
    state_ = ParseState::kText;
    control_.clear();
    ++updates_;
    // Clearing AERA's scrollback alone leaves an interactive shell silently
    // waiting at a prompt which has just been erased. Ask the shell's line
    // editor to redraw the prompt without submitting or changing its input.
    WriteLocked("\x0c");
  }

  bool Running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return master_ >= 0 && child_ > 0;
  }

 private:
  enum class ParseState { kText, kEscape, kControl, kOsc, kOscEscape };

  void CloseMasterLocked() {
    if (master_ >= 0) close(master_);
    master_ = -1;
  }

  void ReapChildLocked() {
    if (child_ <= 0) return;
    int status = 0;
    const pid_t result = waitpid(child_, &status, WNOHANG);
    if (result == child_ || (result < 0 && errno == ECHILD)) {
      child_ = -1;
      CloseMasterLocked();
    }
  }

  void Launch() {
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
      if (master >= 0) close(master);
      return;
    }
    const char* slave_name = ptsname(master);
    if (slave_name == nullptr) {
      close(master);
      return;
    }
    const pid_t child = fork();
    if (child < 0) {
      close(master);
      return;
    }
    if (child == 0) {
      setsid();
      const int slave = open(slave_name, O_RDWR);
      if (slave < 0) _exit(127);
      ioctl(slave, TIOCSCTTY, 0);
      dup2(slave, STDIN_FILENO);
      dup2(slave, STDOUT_FILENO);
      dup2(slave, STDERR_FILENO);
      if (slave > STDERR_FILENO) close(slave);
      close(master);
      setenv("TERM", "xterm-256color", 1);
      setenv("SHELL", "/bin/sh", 1);
      execl("/bin/sh", "sh", static_cast<char*>(nullptr));
      _exit(127);
    }
    master_ = master;
    child_ = child;
    lines_.clear();
    current_.clear();
    cursor_ = 0;
    state_ = ParseState::kText;
    control_.clear();
    ++updates_;
  }

  void CommitLine() {
    while (!current_.empty() && current_.back() == ' ') current_.pop_back();
    lines_.push_back(current_);
    while (lines_.size() > kMaximumScrollback) lines_.pop_front();
    current_.clear();
    cursor_ = 0;
  }

  int ControlValue(int fallback) const {
    int value = 0;
    bool found = false;
    for (char character : control_) {
      if (character >= '0' && character <= '9') {
        found = true;
        value = value * 10 + character - '0';
      } else if (character == ';') {
        break;
      }
    }
    return found ? value : fallback;
  }

  void ApplyControl(unsigned char final) {
    const int amount = std::max(1, ControlValue(1));
    switch (final) {
      case 'C':
        cursor_ = std::min(current_.size(), cursor_ + amount);
        break;
      case 'D':
        cursor_ = amount > static_cast<int>(cursor_) ? 0 : cursor_ - amount;
        break;
      case 'G':
        cursor_ = std::min(current_.size(), static_cast<size_t>(amount - 1));
        break;
      case 'J':
        if (ControlValue(0) == 2) {
          lines_.clear();
          current_.clear();
          cursor_ = 0;
        }
        break;
      case 'K':
        if (ControlValue(0) == 2) {
          current_.clear();
          cursor_ = 0;
        } else if (cursor_ < current_.size()) {
          current_.erase(cursor_);
        }
        break;
      default:
        break;
    }
    control_.clear();
  }

  void Put(unsigned char byte) {
    if (cursor_ >= current_.size()) {
      current_.resize(cursor_, ' ');
      current_.push_back(static_cast<char>(byte));
    } else {
      current_[cursor_] = static_cast<char>(byte);
    }
    ++cursor_;
  }

  void Consume(unsigned char byte) {
    switch (state_) {
      case ParseState::kEscape:
        if (byte == '[') {
          control_.clear();
          state_ = ParseState::kControl;
        } else if (byte == ']') {
          state_ = ParseState::kOsc;
        } else {
          state_ = ParseState::kText;
        }
        return;
      case ParseState::kControl:
        if (byte >= 0x40 && byte <= 0x7e) {
          ApplyControl(byte);
          state_ = ParseState::kText;
        } else if (control_.size() < 64) {
          control_.push_back(static_cast<char>(byte));
        }
        return;
      case ParseState::kOsc:
        if (byte == 7) state_ = ParseState::kText;
        else if (byte == 27) state_ = ParseState::kOscEscape;
        return;
      case ParseState::kOscEscape:
        state_ = byte == '\\' ? ParseState::kText : ParseState::kOsc;
        return;
      case ParseState::kText:
        break;
    }

    if (byte == 27) {
      state_ = ParseState::kEscape;
    } else if (byte == '\n') {
      CommitLine();
    } else if (byte == '\r') {
      cursor_ = 0;
    } else if (byte == '\b' || byte == 0x7f) {
      if (cursor_ > 0) --cursor_;
    } else if (byte == '\t') {
      do Put(' '); while ((cursor_ % 8) != 0);
    } else if (byte >= 0x20) {
      Put(byte);
    }
  }

  mutable std::mutex mutex_;
  int master_ = -1;
  pid_t child_ = -1;
  int columns_ = 80;
  int rows_ = 40;
  int updates_ = 0;
  std::deque<std::string> lines_;
  std::string current_;
  size_t cursor_ = 0;
  ParseState state_ = ParseState::kText;
  std::string control_;
};

TerminalSession gTerminal;

}  // namespace

void RecoveryTerminalStart(int columns, int rows, int pixel_width,
                           int pixel_height) {
  (void)pixel_width;
  (void)pixel_height;
  gTerminal.Start(columns, rows);
}

bool RecoveryTerminalPoll() { return gTerminal.Poll(); }
int RecoveryTerminalUpdateCounter() { return gTerminal.Updates(); }
std::vector<std::string> RecoveryTerminalLines(size_t maximum_lines) {
  return gTerminal.Lines(maximum_lines);
}
void RecoveryTerminalWrite(const std::string& text) { gTerminal.Write(text); }

void RecoveryTerminalSendKey(TerminalKey key) {
  switch (key) {
    case TerminalKey::kUp: gTerminal.Write("\x1b[A"); break;
    case TerminalKey::kDown: gTerminal.Write("\x1b[B"); break;
    case TerminalKey::kLeft: gTerminal.Write("\x1b[D"); break;
    case TerminalKey::kRight: gTerminal.Write("\x1b[C"); break;
    case TerminalKey::kTab: gTerminal.Write("\t"); break;
    case TerminalKey::kEscape: gTerminal.Write("\x1b"); break;
    case TerminalKey::kInterrupt: gTerminal.Write("\x03"); break;
  }
}

void RecoveryTerminalClear() { gTerminal.Clear(); }
bool RecoveryTerminalRunning() { return gTerminal.Running(); }

}  // namespace aeraui
