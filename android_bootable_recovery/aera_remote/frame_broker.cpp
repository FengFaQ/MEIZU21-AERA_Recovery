/* SPDX-License-Identifier: Apache-2.0 */

#include "frame_broker.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <utility>

#include <minuitwrp/minui.h>

namespace aera::remote::frames {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kCaptureFloor = std::chrono::milliseconds(34);
constexpr auto kIdleRefresh = std::chrono::milliseconds(900);
constexpr auto kRequestLifetime = std::chrono::seconds(4);
constexpr char kFramePath[] = "/tmp/.aera_remote_frame.jpg";

struct State {
  std::mutex lock;
  std::string jpeg;
  uint64_t generation = 0;
  Clock::time_point requested_until{};
  Clock::time_point captured_at{};
  bool deferred = false;
};

State& Shared() {
  static State state;
  return state;
}

bool ReadFrame(std::string* data) {
  std::ifstream input(kFramePath, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const std::streamsize length = input.tellg();
  if (length <= 0 || length > 16 * 1024 * 1024) return false;
  data->resize(static_cast<size_t>(length));
  input.seekg(0, std::ios::beg);
  input.read(data->data(), length);
  return input.gcount() == length;
}

}  // namespace

int Width() {
  return std::max(1, gr_fb_width());
}

int Height() {
  return std::max(1, gr_fb_height());
}

void Request() {
  auto& state = Shared();
  std::lock_guard<std::mutex> guard(state.lock);
  state.requested_until = Clock::now() + kRequestLifetime;
}

bool ShouldRender() {
  auto& state = Shared();
  const auto now = Clock::now();
  std::lock_guard<std::mutex> guard(state.lock);
  if (now >= state.requested_until) return false;
  const auto age = now - state.captured_at;
  return state.deferred ? age >= kCaptureFloor : age >= kIdleRefresh;
}

void CaptureAfterRender() {
  auto& state = Shared();
  const auto now = Clock::now();
  {
    std::lock_guard<std::mutex> guard(state.lock);
    if (now >= state.requested_until) return;
    if (state.captured_at.time_since_epoch().count() != 0 &&
        now - state.captured_at < kCaptureFloor) {
      state.deferred = true;
      return;
    }
  }

  if (gr_save_screenshot_scaled_jpeg(kFramePath, 720, 84) != 0) return;
  std::string completed;
  if (!ReadFrame(&completed)) return;

  std::lock_guard<std::mutex> guard(state.lock);
  state.jpeg = std::move(completed);
  ++state.generation;
  state.captured_at = now;
  state.deferred = false;
}

bool Latest(std::string* jpeg, uint64_t* generation) {
  if (!jpeg) return false;
  auto& state = Shared();
  std::lock_guard<std::mutex> guard(state.lock);
  if (state.jpeg.empty()) return false;
  *jpeg = state.jpeg;
  if (generation) *generation = state.generation;
  return true;
}

void Reset() {
  auto& state = Shared();
  std::lock_guard<std::mutex> guard(state.lock);
  state.jpeg.clear();
  state.generation = 0;
  state.requested_until = {};
  state.captured_at = {};
  state.deferred = false;
}

}  // namespace aera::remote::frames
