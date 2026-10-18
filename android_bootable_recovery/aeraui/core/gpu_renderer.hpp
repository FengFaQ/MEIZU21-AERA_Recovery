/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <linux/types.h>

#include <lvgl.h>
#include <minuitwrp/minui.h>

struct AHardwareBuffer;

namespace aeraui {

// Owns the recovery-local Adreno EGL context and imports minui's two DRM
// scanout buffers. Nothing is linked against the vendor driver: every entry
// point is resolved at runtime so the software renderer remains bootable.
class GpuRenderer final {
 public:
  GpuRenderer() = default;
  ~GpuRenderer();

  GpuRenderer(const GpuRenderer&) = delete;
  GpuRenderer& operator=(const GpuRenderer&) = delete;

  bool Initialize(int32_t width, int32_t height);
  lv_display_t* CreateDisplay(int32_t logical_width, int32_t logical_height);
  bool Present(lv_display_t* display);
  void Shutdown();
  bool IsReady() const { return ready_; }

 private:
  void* egl_library_ = nullptr;
  void* gles_library_ = nullptr;
  void* nativewindow_library_ = nullptr;
  void* egl_display_ = nullptr;
  void* egl_surface_ = nullptr;
  void* egl_context_ = nullptr;
  void* egl_images_[2] = {nullptr, nullptr};
  unsigned int textures_[2] = {0, 0};
  unsigned int framebuffers_[2] = {0, 0};
  AHardwareBuffer* hardware_buffers_[2] = {nullptr, nullptr};
  gr_surface scanouts_[2] = {nullptr, nullptr};
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t logical_width_ = 0;
  int32_t logical_height_ = 0;
  unsigned int next_buffer_ = 0;
  bool driver_initialized_ = false;
  bool ready_ = false;
};

}  // namespace aeraui
