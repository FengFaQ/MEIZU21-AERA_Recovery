/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <string>

namespace aeraui::recorder {

enum class State {
  kIdle,
  kPreparing,
  kStarting,
  kRecording,
  kFinalizing,
  kError,
};

struct Snapshot {
  State state = State::kIdle;
  std::string status;
  std::string path;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps = 30;
  uint64_t elapsed_ms = 0;
  uint64_t frames = 0;
  uint64_t dropped = 0;
};

bool Installed();
void SetProfile(uint32_t short_edge, uint32_t fps);
uint32_t ProfileShortEdge();
uint32_t ProfileFps();
bool Start(uint32_t display_width, uint32_t display_height);
void Stop();
void Poll();
void CapturePresentedFrame();
Snapshot GetSnapshot();
bool Active();

}  // namespace aeraui::recorder
