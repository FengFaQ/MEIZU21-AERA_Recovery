/* SPDX-License-Identifier: Apache-2.0 */
#include "service.hpp"
#include "../../../aera_shared_storage.hpp"
#include "../../core/operation_affinity.hpp"

#include "protocol.hpp"
#include "../browser/runtime.hpp"
#include "../plugins/plugin_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <minuitwrp/minui.h>
#include <pixelflinger/pixelflinger.h>

namespace aeraui::recorder {
namespace {

constexpr char kLogTag[] = "AeraRecorder";
constexpr char kStorage[] = "/sdcard/AERA/Recordings";

uint64_t NowNs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + now.tv_nsec;
}

bool SafeDirectory(const char *path, mode_t mode) {
  struct stat info{};
  if (lstat(path, &info) == 0)
    return S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode);
  return errno == ENOENT && mkdir(path, mode) == 0;
}

bool PrepareStorage() {
  if (!SafeDirectory("/sdcard/AERA", 0770) ||
      !SafeDirectory(kStorage, 0770))
    return false;
  chmod(kStorage, 0770);
  chown(kStorage, 0, 1023);
  return access(kStorage, R_OK | W_OK) == 0;
}

std::string RecordingName() {
  const time_t current = time(nullptr);
  struct tm local{};
  char stamp[32] = "unknown";
  if (localtime_r(&current, &local))
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  return std::string("AERA-") + stamp + ".mp4";
}

class Process {
 public:
  ~Process() { Stop(); }

  bool Start(const std::string &runtime, size_t shared_bytes, int &frame_fd,
             int &control_fd, std::string &error) {
    frame_fd = control_fd = -1;
    constexpr char prefix[] = "/tmp/aera-rec-";
    if (pid_ >= 0 || runtime.size() != sizeof(prefix) - 1 + 6 ||
        runtime.compare(0, sizeof(prefix) - 1, prefix)) {
      error = "The verified recorder runtime path is invalid.";
      return false;
    }
    struct stat root{};
    if (lstat(runtime.c_str(), &root) || !S_ISDIR(root.st_mode) || root.st_uid ||
        (root.st_mode & 0777) != 0700) {
      error = "The recorder runtime is no longer private.";
      return false;
    }
    int frame = memfd_create("aera-recorder-pixels",
                             MFD_CLOEXEC | MFD_ALLOW_SEALING);
    int channels[2] = {-1, -1};
    if (frame < 0 || ftruncate(frame, shared_bytes) ||
        fcntl(frame, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels)) {
      if (frame >= 0) close(frame);
      if (channels[0] >= 0) close(channels[0]);
      if (channels[1] >= 0) close(channels[1]);
      error = "Could not create the isolated recording channel.";
      return false;
    }
    const int child_frame = fcntl(frame, F_DUPFD_CLOEXEC, 10);
    const int child_control = fcntl(channels[1], F_DUPFD_CLOEXEC, 10);
    if (child_frame < 0 || child_control < 0) {
      if (child_frame >= 0) close(child_frame);
      if (child_control >= 0) close(child_control);
      close(frame); close(channels[0]); close(channels[1]);
      error = "Could not protect the recorder descriptors.";
      return false;
    }
    const pid_t child = aeraui::operation_affinity::ForkWorker();
    if (!child) {
      if (dup2(child_frame, 3) < 0 || dup2(child_control, 4) < 0) _exit(78);
      close(child_frame); close(child_control);
      execl("/system/bin/aera-browser-jail", "aera-browser-jail", "--recorder",
            runtime.c_str(), nullptr);
      _exit(78);
    }
    close(child_frame); close(child_control); close(channels[1]);
    if (child < 0) {
      close(frame); close(channels[0]);
      error = "Could not start the isolated recorder supervisor.";
      return false;
    }
    pid_ = child;
    frame_fd = frame;
    control_fd = channels[0];
    error.clear();
    return true;
  }

  bool Running() {
    if (pid_ < 0) return false;
    int status = 0;
    const pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_ || (result < 0 && errno == ECHILD)) pid_ = -1;
    return pid_ >= 0;
  }

  void Stop() {
    if (pid_ < 0) return;
    int status = 0;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (!result) kill(pid_, SIGTERM);
    for (int i = 0; !result && i < 30; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      result = waitpid(pid_, &status, WNOHANG);
    }
    if (!result) {
      kill(pid_, SIGKILL);
      while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
    pid_ = -1;
  }

 private:
  pid_t pid_ = -1;
};

class Service {
 public:
  ~Service() { ResetRuntime(); }

  bool Start(uint32_t display_width, uint32_t display_height) {
    if (state_ != State::kIdle && state_ != State::kError) return false;
    if (!Installed()) {
      status_ = "Install AERA Recorder from Plugin Manager first.";
      state_ = State::kError;
      return false;
    }
    if (!PrepareStorage()) {
      status_ = "AERA/Recordings is unavailable. Unlock internal storage first.";
      state_ = State::kError;
      return false;
    }
    ResetRuntime();
    const uint32_t source_short = std::min(display_width, display_height);
    const uint32_t requested = std::min(short_edge_, source_short);
    const double scale = static_cast<double>(requested) / source_short;
    width_ = std::max(2U, static_cast<uint32_t>(display_width * scale) & ~1U);
    height_ = std::max(2U, static_cast<uint32_t>(display_height * scale) & ~1U);
    const uint64_t bytes = static_cast<uint64_t>(width_) * height_ * 4U;
    if (!width_ || !height_ || bytes > kMaximumFrameBytes) {
      status_ = "The selected recording resolution is unsupported.";
      state_ = State::kError;
      return false;
    }
    frame_bytes_ = static_cast<uint32_t>(bytes);
    const std::string name = RecordingName();
    path_ = std::string(kStorage) + "/" + name;
    worker_path_ = "/recordings/" + name;
    state_ = State::kPreparing;
    status_ = "Preparing isolated encoder";
    preparation_.cancel.store(false);
    preparation_.progress.store(0);
    preparation_.done.store(false);
    preparation_.verified = false;
    preparation_.directory.clear();
    preparation_.error.clear();
    preparation_thread_ = std::thread([this] {
      operation_affinity::RestoreForWorker();
      web::PreparePluginRuntime(preparation_, "recorder", "app-runtime",
                                "recorder");
    });
    return true;
  }

  void Stop() {
    if (state_ == State::kPreparing) {
      preparation_.cancel.store(true);
      status_ = "Cancelling recorder preparation";
      return;
    }
    if (state_ == State::kStarting) {
      ResetRuntime();
      state_ = State::kIdle;
      status_ = "Recording cancelled";
      return;
    }
    if (state_ != State::kRecording || control_ < 0) return;
    Message message;
    message.kind = Kind::kStop;
    if (!Write(message)) {
      Fail("The recorder stopped before the MP4 could be finalized.");
      return;
    }
    state_ = State::kFinalizing;
    status_ = "Finalizing MP4";
    finalizing_since_ns_ = NowNs();
  }

  void Poll() {
    if (state_ == State::kPreparing && preparation_.done.load()) {
      if (preparation_thread_.joinable()) preparation_thread_.join();
      if (!preparation_.verified) {
        Fail(preparation_.error.empty() ? "Recorder preparation failed."
                                        : preparation_.error.c_str());
        return;
      }
      int frame = -1;
      int control = -1;
      std::string error;
      if (!process_.Start(preparation_.directory,
                          static_cast<size_t>(frame_bytes_) * kFrameSlots,
                          frame, control, error) || !Adopt(frame, control)) {
        Fail(error.empty() ? "Could not open the recorder channel."
                           : error.c_str());
        return;
      }
      Message request;
      request.kind = Kind::kStart;
      request.width = width_;
      request.height = height_;
      request.stride = width_ * 4U;
      request.fps = fps_;
      request.data_bytes = frame_bytes_;
      snprintf(request.text, sizeof(request.text), "%s", worker_path_.c_str());
      if (!Write(request)) {
        Fail("Could not configure the recording encoder.");
        return;
      }
      // Consume preparation before another engine/scene poll can enter this
      // block again while the worker is still starting up.
      state_ = State::kStarting;
      status_ = "Starting MP4 encoder";
    }

    for (int i = 0; i < 16 && control_ >= 0; ++i) {
      Message message;
      const ssize_t count = recv(control_, &message, sizeof(message),
                                 MSG_DONTWAIT | MSG_TRUNC);
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (count < 0 && errno == EINTR) continue;
      if (count != sizeof(message) || message.magic != kMagic ||
          !memchr(message.text, '\0', sizeof(message.text))) {
        Fail("The recorder closed or sent an invalid response.");
        return;
      }
      frames_ = message.frames;
      if (message.kind == Kind::kAck) {
        if (frame_pending_ && message.sequence == sequence_)
          frame_pending_ = false;
      } else if (message.kind == Kind::kStatus) {
        status_ = message.text;
        if (!strcmp(message.text, "Recording")) {
          state_ = State::kRecording;
          started_ns_ = NowNs();
          next_frame_ns_ = started_ns_;
        }
      } else if (message.kind == Kind::kDone) {
        aera::shared_storage::Normalize(path_);
        status_ = "Saved to AERA/Recordings";
        state_ = State::kIdle;
        CloseChannel();
        process_.Stop();
        web::RemoveRuntime(preparation_.directory);
        preparation_.directory.clear();
      } else if (message.kind == Kind::kError) {
        Fail(message.text[0] ? message.text : "The encoder reported an error.");
        return;
      }
    }
    if ((state_ == State::kStarting || state_ == State::kRecording ||
         state_ == State::kFinalizing) &&
        !process_.Running()) {
      Fail("AERA Recorder stopped unexpectedly.");
      return;
    }
    if (state_ == State::kFinalizing && finalizing_since_ns_ &&
        NowNs() - finalizing_since_ns_ > 10000000000ULL)
      Fail("The MP4 encoder timed out while finalizing.");
  }

  void Capture() {
    if (state_ != State::kRecording || shared_ == nullptr) return;
    const uint64_t now = NowNs();
    if (now < next_frame_ns_) return;
    const uint64_t interval = 1000000000ULL / fps_;
    next_frame_ns_ = std::max(next_frame_ns_ + interval, now + interval);
    if (frame_pending_) {
      ++dropped_;
      return;
    }
    const gr_surface presented = gr_drm_get_presented_surface();
    const auto *source = static_cast<const GRSurface *>(presented);
    if (!source || !source->data || source->width <= 0 || source->height <= 0 ||
        source->row_bytes <= 0) {
      ++dropped_;
      return;
    }
    const uint32_t next = sequence_ + 1;
    uint8_t *destination = shared_ +
        static_cast<size_t>(next % kFrameSlots) * frame_bytes_;
    for (uint32_t y = 0; y < height_; ++y) {
      const uint32_t sy = static_cast<uint64_t>(y) * source->height / height_;
      const uint8_t *row = source->data +
          static_cast<size_t>(sy) * source->row_bytes;
      uint8_t *out = destination + static_cast<size_t>(y) * width_ * 4U;
      for (uint32_t x = 0; x < width_; ++x) {
        const uint32_t sx = static_cast<uint64_t>(x) * source->width / width_;
        if (source->format == GGL_PIXEL_FORMAT_BGRA_8888) {
          const uint8_t *pixel = row + static_cast<size_t>(sx) * 4U;
          out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0]; out[3] = 255;
        } else if (source->format == GGL_PIXEL_FORMAT_RGBA_8888 ||
                   source->format == GGL_PIXEL_FORMAT_RGBX_8888) {
          const uint8_t *pixel = row + static_cast<size_t>(sx) * 4U;
          out[0] = pixel[0]; out[1] = pixel[1]; out[2] = pixel[2]; out[3] = 255;
        } else if (source->format == GGL_PIXEL_FORMAT_RGB_565) {
          const uint8_t *pixel = row + static_cast<size_t>(sx) * 2U;
          const uint16_t value = pixel[0] | (static_cast<uint16_t>(pixel[1]) << 8);
          out[0] = static_cast<uint8_t>(((value >> 11) & 31) * 255 / 31);
          out[1] = static_cast<uint8_t>(((value >> 5) & 63) * 255 / 63);
          out[2] = static_cast<uint8_t>((value & 31) * 255 / 31);
          out[3] = 255;
        } else {
          ++dropped_;
          return;
        }
        out += 4;
      }
    }
    Message message;
    message.kind = Kind::kFrame;
    message.sequence = next;
    message.width = width_;
    message.height = height_;
    message.stride = width_ * 4U;
    message.fps = fps_;
    message.data_bytes = frame_bytes_;
    message.timestamp_ns = now - started_ns_;
    message.frames = frames_;
    message.dropped = dropped_;
    if (Write(message)) {
      sequence_ = next;
      frame_pending_ = true;
    } else {
      ++dropped_;
    }
  }

  Snapshot SnapshotValue() const {
    Snapshot result;
    result.state = state_;
    result.status = status_;
    result.path = path_;
    result.width = width_;
    result.height = height_;
    result.fps = fps_;
    result.elapsed_ms = started_ns_ &&
        (state_ == State::kRecording || state_ == State::kFinalizing)
        ? (NowNs() - started_ns_) / 1000000ULL : 0;
    result.frames = frames_;
    result.dropped = dropped_;
    return result;
  }

  void SetProfile(uint32_t short_edge, uint32_t fps) {
    if (state_ == State::kRecording || state_ == State::kPreparing ||
        state_ == State::kFinalizing) return;
    short_edge_ = short_edge == 1080 ? 1080 : 720;
    fps_ = fps == 60 ? 60 : 30;
  }

  uint32_t ShortEdge() const { return short_edge_; }
  uint32_t Fps() const { return fps_; }
  bool Active() const {
    return state_ == State::kPreparing || state_ == State::kRecording ||
        state_ == State::kStarting || state_ == State::kFinalizing;
  }

 private:
  bool Adopt(int frame_fd, int control_fd) {
    struct stat info{};
    int type = 0;
    socklen_t type_size = sizeof(type);
    const size_t expected = static_cast<size_t>(frame_bytes_) * kFrameSlots;
    const int seals = frame_fd >= 0 ? fcntl(frame_fd, F_GET_SEALS) : -1;
    const bool valid = frame_fd >= 0 && control_fd >= 0 &&
        !fstat(frame_fd, &info) && S_ISREG(info.st_mode) &&
        static_cast<size_t>(info.st_size) == expected && seals >= 0 &&
        (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) ==
            (F_SEAL_SHRINK | F_SEAL_GROW) &&
        !getsockopt(control_fd, SOL_SOCKET, SO_TYPE, &type, &type_size) &&
        type == SOCK_SEQPACKET;
    if (!valid) {
      if (frame_fd >= 0) close(frame_fd);
      if (control_fd >= 0) close(control_fd);
      return false;
    }
    void *mapping = mmap(nullptr, expected, PROT_READ | PROT_WRITE, MAP_SHARED,
                         frame_fd, 0);
    close(frame_fd);
    if (mapping == MAP_FAILED) {
      close(control_fd);
      return false;
    }
    shared_ = static_cast<uint8_t *>(mapping);
    shared_bytes_ = expected;
    control_ = control_fd;
    fcntl(control_, F_SETFL, fcntl(control_, F_GETFL) | O_NONBLOCK);
    return true;
  }

  bool Write(const Message &message) {
    return control_ >= 0 &&
        send(control_, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL) ==
            static_cast<ssize_t>(sizeof(message));
  }

  void CloseChannel() {
    if (control_ >= 0) close(control_);
    control_ = -1;
    if (shared_) munmap(shared_, shared_bytes_);
    shared_ = nullptr;
    shared_bytes_ = 0;
    frame_pending_ = false;
  }

  void ResetRuntime() {
    preparation_.cancel.store(true);
    if (preparation_thread_.joinable()) preparation_thread_.join();
    if (control_ >= 0) {
      Message close_message;
      close_message.kind = Kind::kClose;
      Write(close_message);
    }
    CloseChannel();
    process_.Stop();
    if (!preparation_.directory.empty())
      web::RemoveRuntime(preparation_.directory);
    preparation_.directory.clear();
    sequence_ = 0;
    frames_ = 0;
    dropped_ = 0;
    started_ns_ = 0;
    next_frame_ns_ = 0;
    finalizing_since_ns_ = 0;
  }

  void Fail(const char *message) {
    status_ = message ? message : "AERA Recorder failed.";
    state_ = State::kError;
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", status_.c_str());
    CloseChannel();
    process_.Stop();
    if (!preparation_.directory.empty())
      web::RemoveRuntime(preparation_.directory);
    preparation_.directory.clear();
  }

  web::Preparation preparation_;
  std::thread preparation_thread_;
  Process process_;
  State state_ = State::kIdle;
  std::string status_ = "Ready to record";
  std::string path_;
  std::string worker_path_;
  uint8_t *shared_ = nullptr;
  size_t shared_bytes_ = 0;
  int control_ = -1;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t short_edge_ = 720;
  uint32_t fps_ = 30;
  uint32_t frame_bytes_ = 0;
  uint32_t sequence_ = 0;
  uint64_t frames_ = 0;
  uint64_t dropped_ = 0;
  uint64_t started_ns_ = 0;
  uint64_t next_frame_ns_ = 0;
  uint64_t finalizing_since_ns_ = 0;
  bool frame_pending_ = false;
};

Service &Instance() {
  static Service service;
  return service;
}

}  // namespace

bool Installed() {
  return web::PluginRuntimeInstalled("recorder", "app-runtime", "recorder");
}
void SetProfile(uint32_t short_edge, uint32_t fps) {
  Instance().SetProfile(short_edge, fps);
}
uint32_t ProfileShortEdge() { return Instance().ShortEdge(); }
uint32_t ProfileFps() { return Instance().Fps(); }
bool Start(uint32_t display_width, uint32_t display_height) {
  return Instance().Start(display_width, display_height);
}
void Stop() { Instance().Stop(); }
void Poll() { Instance().Poll(); }
void CapturePresentedFrame() { Instance().Capture(); }
Snapshot GetSnapshot() { return Instance().SnapshotValue(); }
bool Active() { return Instance().Active(); }

}  // namespace aeraui::recorder
