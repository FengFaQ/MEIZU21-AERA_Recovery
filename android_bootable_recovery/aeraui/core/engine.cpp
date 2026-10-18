/*
 * Copyright (C) 2026 Recovery AERA UI contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aeraui/engine.hpp"
#include "aeraui/display_transform.hpp"
#include "aeraui/i18n.hpp"
#include "operation_affinity.hpp"
#include "mouse_cursor.hpp"
#include "usb_attachment.hpp"
#include "screenshot_flash.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

#include <android/log.h>
#include <lvgl.h>
#include <minuitwrp/minui.h>
#include <pixelflinger/pixelflinger.h>
#include "design.hpp"
#include "edge_swipe.hpp"
#include "gpu_renderer.hpp"
#include "picture_decode.hpp"
#include "picture_viewer.hpp"
#include "recorder/service.hpp"
#include "scene.hpp"
#include "ui_components.hpp"
#include "pc_transfer_banner.hpp"
#include "update/update_manager.hpp"
#include "root/root_manager.hpp"
#include "../components/power_transition.hpp"
#include "../components/phone_keyboard.hpp"
#include "../components/hardware_keyboard.hpp"
#include "draw/opengles/lv_draw_opengles.h"
#include "../../aera_rpc/aera_channel.hpp"
#include "../../aera_remote/aera_remote.hpp"
#include "../../aera_remote/pc_connection.hpp"
#include "../../aera_rpc/aera_dispatcher.hpp"

extern "C" int aeraui_install_package(const char *path);
extern "C" int aeraui_decrypt_data(const char *credential, int user_id);

namespace aeraui {
namespace {

constexpr char kLogTag[] = "AERA UI";
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kTargetRefreshHz = 120;
// Rendering and DRM presentation consume several milliseconds on dodge. Wake
// after 2 ms so the complete pipeline still fits a 120 Hz frame deadline.
constexpr uint32_t kFrameIntervalMs = 2;
constexpr uint32_t kBufferAlignment = 64;

enum class UpdateTask {
  kNone,
  kCheck,
  kDownload,
};

enum class PluginTask {
  kNone,
  kInteractive,
  kBackgroundRefresh,
  kAutomaticInstall,
};

uint32_t MonotonicMilliseconds() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint32_t>(now.tv_sec * 1000ULL + now.tv_nsec / 1000000ULL);
}

struct RecentPreview {
  std::shared_ptr<PictureData> pixels;
  lv_image_dsc_t descriptor{};
};

struct RecentApp {
  Action action = Action::kNone;
  std::string plugin_id;
  std::string title;
  std::string summary;
  std::string icon;
  std::shared_ptr<RecentPreview> preview;
};

void RoundRecentPreview(PictureData &picture) {
  if (picture.pixels == nullptr || picture.width < 32 || picture.height < 32)
    return;
  const int radius = std::clamp(static_cast<int>(picture.width * 58 / 1080),
                                18, 54);
  const int outer = radius * radius;
  const int inner = (radius - 1) * (radius - 1);
  for (int y = 0; y < radius; ++y) {
    for (int x = 0; x < radius; ++x) {
      const int dx = radius - x - 1;
      const int dy = radius - y - 1;
      const int distance = dx * dx + dy * dy;
      uint8_t alpha = 255;
      if (distance >= outer) alpha = 0;
      else if (distance > inner)
        alpha = static_cast<uint8_t>(
            255 * (outer - distance) / std::max(1, outer - inner));
      if (alpha == 255) continue;
      const uint32_t right = picture.width - x - 1;
      const uint32_t bottom = picture.height - y - 1;
      picture.pixels[(static_cast<size_t>(y) * picture.width + x) * 4 + 3] =
          alpha;
      picture.pixels[(static_cast<size_t>(y) * picture.width + right) * 4 + 3] =
          alpha;
      picture.pixels[(static_cast<size_t>(bottom) * picture.width + x) * 4 + 3] =
          alpha;
      picture.pixels[(static_cast<size_t>(bottom) * picture.width + right) * 4 + 3] =
          alpha;
    }
  }
}

bool IsRecentAppAction(Action action) {
  switch (action) {
    case Action::kWeb:
    case Action::kRetroArch:
    case Action::kDoom:
    case Action::kTelegram:
    case Action::kGallery:
    case Action::kMedia:
    case Action::kStreams:
    case Action::kRecorder:
    case Action::kAppVault:
    case Action::kPluginApp:
      return true;
    default:
      return false;
  }
}

const char *PluginEntryForAction(Action action) {
  switch (action) {
    case Action::kWeb: return "browser";
    case Action::kRetroArch: return "retroarch";
    case Action::kDoom: return "doom";
    case Action::kTelegram: return "telegram";
    case Action::kGallery: return "gallery";
    case Action::kMedia: return "media";
    case Action::kStreams: return "streams";
    case Action::kRecorder: return "recorder";
    case Action::kAppVault: return "appvault";
    default: return nullptr;
  }
}

RecentApp DescribeRecentApp(Action action) {
  RecentApp result;
  result.action = action;
  switch (action) {
    case Action::kWeb:
      result.title = "AERA Browser";
      result.summary = "Private WebKit browsing";
      result.icon = LV_SYMBOL_GPS;
      break;
    case Action::kRetroArch:
      result.title = "RetroArch";
      result.summary = "Classic games in recovery";
      result.icon = LV_SYMBOL_PLAY;
      break;
    case Action::kDoom:
      result.title = "AERA Doom";
      result.summary = "Native Doom runtime";
      result.icon = LV_SYMBOL_PLAY;
      break;
    case Action::kTelegram:
      result.title = "AERA Telegram";
      result.summary = "Messaging and file sharing";
      result.icon = LV_SYMBOL_GPS;
      break;
    case Action::kGallery:
      result.title = "AERA Gallery";
      result.summary = "Photos in recovery";
      result.icon = LV_SYMBOL_IMAGE;
      break;
    case Action::kMedia:
      result.title = "AERA Media";
      result.summary = "Music and video playback";
      result.icon = LV_SYMBOL_PLAY;
      break;
    case Action::kStreams:
      result.title = "AERA Streams";
      result.summary = "Open streaming services";
      result.icon = LV_SYMBOL_VIDEO;
      break;
    case Action::kRecorder:
      result.title = "AERA Recorder";
      result.summary = "Recovery screen recording";
      result.icon = LV_SYMBOL_VIDEO;
      break;
    case Action::kAppVault:
      result.title = "App Backup Vault";
      result.summary = "Back up and restore Android apps";
      result.icon = LV_SYMBOL_SAVE;
      break;
    case Action::kPluginApp:
      result.title = "AERA Plugin";
      result.summary = "Host API 2 app";
      result.icon = LV_SYMBOL_LIST;
      result.plugin_id = GetSelectedPluginId();
      break;
    default:
      break;
  }

  const char *entry = PluginEntryForAction(action);
  for (const auto &plugin : plugins::Installed()) {
    const bool matches = action == Action::kPluginApp
        ? !result.plugin_id.empty() && plugin.id == result.plugin_id
        : entry != nullptr && plugin.entry == entry;
    if (!matches) continue;
    result.plugin_id = plugin.id;
    result.title = plugin.name;
    if (!plugin.description.empty()) result.summary = plugin.description;
    break;
  }
  return result;
}

} // namespace

class Engine::Impl final {
public:
  ~Impl() { Shutdown(); }

  bool Initialize(bool fastboot_mode, bool adaptive_resolution,
                  int32_t logical_height, bool resume_recovery) {
    if (initialized_)
      return true;

    fastboot_mode_ = fastboot_mode;
    physical_width_ = gr_fb_width();
    physical_height_ = gr_fb_height();
    transform_ = adaptive_resolution
        ? DisplayTransform::Adaptive(physical_width_, physical_height_,
                                     logical_height)
        : DisplayTransform::Native(physical_width_, physical_height_);
    width_ = transform_.logical_width;
    height_ = transform_.logical_height;
    if (!transform_.IsValid()) {
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "minui is not initialized (invalid display %dx%d)",
                          physical_width_, physical_height_);
      return false;
    }

    direct_scanout_ = ConfigureDirectScanout();
    const size_t pixel_count = static_cast<size_t>(width_) * height_;
    if (pixel_count > SIZE_MAX / kBytesPerPixel)
      return false;
    const size_t logical_buffer_size = pixel_count * kBytesPerPixel;
    if (!direct_scanout_ || transform_.IsScaled()) {
      buffer_size_ = logical_buffer_size;
      if (posix_memalign(&draw_buffer_, kBufferAlignment, buffer_size_) != 0) {
        draw_buffer_ = nullptr;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "unable to allocate %zu-byte fallback render buffer",
                            buffer_size_);
        return false;
      }
      std::memset(draw_buffer_, 0, buffer_size_);
    }

    gpu_accelerated_ =
        direct_scanout_ &&
        gpu_renderer_.Initialize(physical_width_, physical_height_);
    lv_draw_opengles_set_enabled(gpu_accelerated_);
    lv_init();
    lv_tick_set_cb(MonotonicMilliseconds);

    display_ = gpu_accelerated_ ? gpu_renderer_.CreateDisplay(width_, height_)
                                : lv_display_create(width_, height_);
    if (display_ == nullptr && gpu_accelerated_) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "GPU display setup failed; restarting LVGL in "
                          "software mode");
      lv_deinit();
      gpu_renderer_.Shutdown();
      gpu_accelerated_ = false;
      lv_draw_opengles_set_enabled(false);
      lv_init();
      lv_tick_set_cb(MonotonicMilliseconds);
      display_ = lv_display_create(width_, height_);
    }
    if (display_ == nullptr) {
      Shutdown();
      return false;
    }
    lv_display_set_user_data(display_, this);
    if (gpu_accelerated_) {
      // The OpenGL texture display supplies its own dummy LVGL draw buffer.
      // Its flush callback below composites directly into imported DRM
      // scanout images.
    } else if (direct_scanout_ && !transform_.IsScaled()) {
      lv_display_set_color_format(display_, LV_COLOR_FORMAT_XRGB8888);
      lv_display_set_buffers_with_stride(
          display_, scanout_surface1_->data, scanout_surface2_->data,
          static_cast<uint32_t>(buffer_size_),
          static_cast<uint32_t>(scanout_surface1_->row_bytes),
          LV_DISPLAY_RENDER_MODE_DIRECT);
    } else {
      // A full-frame compositor is retained only as a safe non-DRM fallback.
      lv_display_set_color_format(
          display_, direct_scanout_ ? LV_COLOR_FORMAT_XRGB8888
                                    : LV_COLOR_FORMAT_ARGB8888);
      lv_display_set_buffers(display_, draw_buffer_, nullptr,
                             static_cast<uint32_t>(buffer_size_),
                             LV_DISPLAY_RENDER_MODE_FULL);
    }
    lv_display_set_flush_cb(display_, FlushDisplay);
    // GpuRenderer reshapes its LVGL texture to the rotated logical viewport;
    // the final OpenGL scanout pass applies the one required panel rotation.

    pointer_device_ = lv_indev_create();
    if (pointer_device_ == nullptr) {
      Shutdown();
      return false;
    }
    lv_indev_set_type(pointer_device_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(pointer_device_, this);
    lv_indev_set_read_cb(pointer_device_, ReadPointer);
    /* LVGL's default 10% momentum decay feels abrupt with our normalized
     * velocity history. Four percent keeps the release natural while the
     * averaging in lv_indev prevents the old slingshot acceleration. */
    lv_indev_set_scroll_throw(pointer_device_, 4);

    mouse_device_ = lv_indev_create();
    if (!mouse_device_) { Shutdown(); return false; }
    lv_indev_set_type(mouse_device_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(mouse_device_, this);
    lv_indev_set_read_cb(mouse_device_, ReadMouse);
    mouse_x_ = width_ / 2; mouse_y_ = height_ / 2;
    mouse_cursor_ = lv_image_create(lv_layer_sys());
    lv_obj_remove_flag(mouse_cursor_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(mouse_cursor_, LV_OBJ_FLAG_HIDDEN);
    lv_indev_set_cursor(mouse_device_, mouse_cursor_);

    ApplyStoredAppearance();
    if (fastboot_mode) {
      backend_ready_ = true;
      interactive_ready_ = true;
      current_scene_ = Action::kNone;
      on_home_ = false;
      BuildFastbootScene(lv_screen_active(), HandleSceneAction, this);
    } else if (resume_recovery) {
      backend_ready_ = true;
      interactive_ready_ = true;
      boot_animation_complete_ = true;
      current_scene_ = Action::kBackHome;
      current_tool_ = Action::kNone;
      on_home_ = true;
      BuildHomeScene(lv_screen_active(), HandleSceneAction, this);
    } else {
      BuildBootScene(lv_screen_active(), HandleSceneAction, this);
    }
    initialized_ = true;
    __android_log_print(
        ANDROID_LOG_INFO, kLogTag,
        "engine initialized at logical %dx%d on physical %dx%d with %s "
        "rendering (%.2f MiB); "
        "%u Hz scheduler",
        width_, height_, physical_width_, physical_height_,
        gpu_accelerated_ ? "zero-copy Adreno/LVGL"
                         : (direct_scanout_ ? "software DRM scanout"
                                            : "minui fallback"),
        static_cast<double>(buffer_size_) / (1024.0 * 1024.0),
        kTargetRefreshHz);
    return true;
  }

  void Shutdown() {
    aera::pc::SetEnabled(false, 443, false);
    plugin_progress_.cancel.store(true);
    update::Cancel();
    if (operation_running_ && operation_scene_.job == Job::kSideload)
      RecoveryCancelSideload();
    if (operation_thread_.joinable())
      operation_thread_.join();
    if (decrypt_thread_.joinable())
      decrypt_thread_.join();
    if (wifi_thread_.joinable())
      wifi_thread_.join();
    if (nas_thread_.joinable())
      nas_thread_.join();
    if (plugin_thread_.joinable())
      plugin_thread_.join();
    if (update_thread_.joinable())
      update_thread_.join();
    ShutdownWebRuntime();
    CancelEdgeSwipe();
    lv_anim_delete(this, SetRecentLaunchProgress);
    recent_launch_active_ = false;
    if (recent_launch_layer_ != nullptr) {
      lv_obj_delete(recent_launch_layer_);
      recent_launch_layer_ = nullptr;
    }
    recent_launch_card_ = nullptr;
    recent_launch_image_ = nullptr;
    recent_launch_footer_ = nullptr;
    recent_launch_preview_.reset();
    CancelRecentsSwipe();
    if (recents_overlay_ != nullptr) {
      lv_obj_delete(recents_overlay_);
      recents_overlay_ = nullptr;
    }
    if (mouse_device_) { lv_indev_delete(mouse_device_); mouse_device_ = nullptr; }
    if (mouse_cursor_) {
      lv_image_set_src(mouse_cursor_, nullptr);
      lv_image_cache_drop(&mouse_image_);
      lv_obj_delete(mouse_cursor_); mouse_cursor_ = nullptr;
    }
    if (pointer_device_ != nullptr) {
      lv_indev_delete(pointer_device_);
    pointer_device_ = nullptr;
    }
    pc_transfer_.Clear();
    if (display_ != nullptr) {
      lv_display_delete(display_);
      display_ = nullptr;
    }
    if (lv_is_initialized())
      lv_deinit();
    gpu_renderer_.Shutdown();
    lv_draw_opengles_set_enabled(false);
    if (draw_buffer_ != nullptr) {
      std::free(draw_buffer_);
      draw_buffer_ = nullptr;
    }
    buffer_size_ = 0;
    scanout1_ = nullptr;
    scanout2_ = nullptr;
    scanout_surface1_ = nullptr;
    scanout_surface2_ = nullptr;
    direct_scanout_ = false;
    gpu_accelerated_ = false;
    backend_ready_ = false;
    boot_animation_complete_ = false;
    operation_running_ = false;
    decrypt_running_ = false;
    wifi_running_ = false;
    wifi_scene_report_ = false;
    wifi_report_state_ = nullptr;
    nas_running_ = false;
    plugin_running_ = false;
    plugin_task_ = PluginTask::kNone;
    plugin_auto_queue_.clear();
    plugin_connection_seen_ = false;
    update_running_ = false;
    update_task_ = UpdateTask::kNone;
    update_installing_ = false;
    update_connection_seen_ = false;
    decryption_active_ = false;
    secondary_decryption_ = false;
    navigation_history_.clear();
    recent_apps_.clear();
    recents_deferred_cards_.clear();
    recents_all_cards_.clear();
    current_scene_ = Action::kBackHome;
    navigating_back_ = false;
    lock_overlay_ = nullptr;
    power_overlay_ = nullptr;
    volume_overlay_ = nullptr;
    notice_overlay_ = nullptr;
    pc_overlay_ = nullptr;
    pc_connection_id_.clear();
    pc_job_id_.clear();
    last_pc_poll_ = last_pc_platform_refresh_ = last_pc_cleanup_ = 0;
    last_pc_start_attempt_ = 0;
    suspended_ = false;
    interactive_ready_ = false;
    initialized_ = false;
  }

  uint32_t RunFrame() {
    if (!initialized_)
      return kFrameIntervalMs;
    if (suspended_)
      return 250;
#ifndef TW_OEM_BUILD
    const int rpc_fd = aera::rpc::Channel::InputFd();
    if (rpc_fd >= 0) {
      pollfd rpc{rpc_fd, POLLIN, 0};
      if (poll(&rpc, 1, 0) > 0 && (rpc.revents & POLLIN))
        aera::rpc::Channel::HandleInput();
    }
    const int cancel_fd = aera::rpc::Channel::CancelFd();
    if (cancel_fd >= 0) {
      pollfd cancel{cancel_fd, POLLIN, 0};
      if (poll(&cancel, 1, 0) > 0 && (cancel.revents & POLLIN))
        aera::rpc::Channel::HandleCancel();
    }
#endif
    recorder::Poll();
    if (fastboot_mode_) PollFastbootTelemetry();
    if (operation_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (operation_thread_.joinable())
        operation_thread_.join();
      const auto cancellation = aera::pc::GetBackupCancellation();
      const bool pc_cancelled = operation_scene_.job == Job::kPcBackupOperation && cancellation.requested;
      const bool success = operation_result_.load(std::memory_order_acquire) == 0 && !pc_cancelled;
      if (!pc_job_id_.empty()) {
        aera::pc::UpdateInstallerPresentation(RecoveryInstallerPresentation());
        aera::pc::UpdateInstall(RecoveryProgress(), RecoveryOperationDetail(), widgets::ReadLog());
        aera::pc::CompleteInstall(pc_job_id_, operation_result_.load(std::memory_order_acquire));
        pc_job_id_.clear();
      }
      if (update_installing_) {
        if (success) update::MarkInstalled();
        update_installing_ = false;
      }
      if (operation_visible_) {
        const bool sideload_cancelled =
            operation_scene_.job == Job::kSideload &&
            RecoverySideloadStatus().cancel_requested;
        CompleteOperationScene(
            operation_scene_, success,
            pc_cancelled ? (cancellation.restore
                ? "Stopping a restore may leave incomplete data. Complete a full restore before booting Android."
                : "An unfinished backup cannot be restored.") : sideload_cancelled
                ? "ADB sideload was cancelled. Normal ADB has been restored."
                : success
                      ? "The requested operation completed. Review its output below."
                      : "The backend reported an error. Review the recovery log below.");
      }
      operation_running_ = false;
    }
    if (operation_running_ && operation_visible_ && MonotonicMilliseconds() - last_operation_update_ >= 500) {
      last_operation_update_ = MonotonicMilliseconds();
      RefreshOperationScene(operation_scene_);
    }
    if (decrypt_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (decrypt_thread_.joinable()) decrypt_thread_.join();
      const bool success =
          decrypt_result_.load(std::memory_order_acquire) == 0;
      CompleteDecryptAttempt(decrypt_scene_, success);
      decrypt_running_ = false;
      if (success) {
        decryption_active_ = false;
        if (secondary_decryption_) {
          secondary_decryption_ = false;
          HandleSceneAction(Action::kUsers, this);
        } else {
          decryption_completion_ = DecryptionCompletion::kSuccess;
          ShowPreparing(true);
        }
      }
    }
    if (wifi_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (wifi_thread_.joinable()) wifi_thread_.join();
      const bool success = wifi_result_.load(std::memory_order_acquire) == 0;
      if (wifi_scene_report_ && current_scene_ == Action::kWifi &&
          wifi_scene_.state == wifi_report_state_) {
        CompleteWifiOperation(wifi_scene_, success);
      }
      wifi_scene_report_ = false;
      wifi_report_state_ = nullptr;
      wifi_running_ = false;
    }
    if (nas_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (nas_thread_.joinable()) nas_thread_.join();
      const bool success = nas_result_.load(std::memory_order_acquire) == 0;
      CompleteNasOperation(nas_scene_, success);
      nas_running_ = false;
    }
    if (plugin_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (plugin_thread_.joinable()) plugin_thread_.join();
      const bool success = plugin_result_.load(std::memory_order_acquire) == 0;
      const std::string message = success ? plugin_progress_.status : plugin_progress_.error;
      const PluginTask completed = plugin_task_;
      const plugins::Job completed_job = plugin_job_;
      plugin_running_ = false;
      plugin_task_ = PluginTask::kNone;
      if (completed == PluginTask::kInteractive) {
        if (current_scene_ == Action::kPlugins) {
          CompletePluginOperation(plugin_scene_, success, message.c_str());
          if (success && completed_job == plugins::Job::kRefresh &&
              RecoveryPreference(Preference::kPluginAutoUpdate))
            BeginAutomaticPluginUpdates();
        } else if (on_home_) {
          ShowHome(false);
          if (!success) widgets::Sheet(lv_screen_active(), "Operation failed", message);
        }
      } else if (completed == PluginTask::kBackgroundRefresh) {
        if (success)
          BeginAutomaticPluginUpdates();
        else
          RefreshPluginSurfaces();
      } else if (completed == PluginTask::kAutomaticInstall) {
        if (current_scene_ == Action::kPlugins)
          CompletePluginOperation(plugin_scene_, success, message.c_str());
        StartNextAutomaticPluginUpdate();
      }
    }
    if (plugin_running_ && (plugin_task_ == PluginTask::kInteractive ||
        plugin_task_ == PluginTask::kAutomaticInstall) &&
        current_scene_ == Action::kPlugins &&
        MonotonicMilliseconds() - last_plugin_update_ >= 120) {
      last_plugin_update_ = MonotonicMilliseconds();
      UpdatePluginProgress(plugin_scene_, plugin_progress_.value.load(),
                           plugin_progress_.downloaded_bytes.load(),
                           plugin_progress_.total_bytes.load(),
                           plugin_progress_.completed_plugins.load(),
                           plugin_progress_.total_plugins.load());
    }
    if (update_complete_.exchange(false, std::memory_order_acq_rel)) {
      if (update_thread_.joinable()) update_thread_.join();
      const bool success =
          update_result_.load(std::memory_order_acquire) == 0;
      const UpdateTask completed = update_task_;
      const bool manual = update_manual_;
      update_running_ = false;
      update_task_ = UpdateTask::kNone;
      if (current_scene_ == Action::kUpdates)
        RefreshUpdateScene(update_scene_);
      const auto snapshot = update::GetSnapshot();
      if (completed == UpdateTask::kCheck && success && snapshot.available &&
          !manual &&
          update_banner_build_time_ != snapshot.release.build_time) {
        update_banner_build_time_ = snapshot.release.build_time;
        ShowUpdateBanner(snapshot.release.version);
      } else if (completed == UpdateTask::kDownload && success &&
                 !snapshot.package_path.empty()) {
        JobRequest request;
        request.job = Job::kInstall;
        request.path = snapshot.package_path;
        request.title = "Install AERA Update";
        request.present_before_run = true;
        update_installing_ = true;
        StartJob(request);
      }
    }
    if (update_running_ && current_scene_ == Action::kUpdates &&
        MonotonicMilliseconds() - last_update_refresh_ >= 120) {
      last_update_refresh_ = MonotonicMilliseconds();
      RefreshUpdateScene(update_scene_);
    }
    PollAutomaticUpdates();
    PollPcConnection();
    PollUsbAttachment();
    PollRecentsHold();
    const uint32_t next = lv_timer_handler();
#ifndef TW_OEM_BUILD
    // Capture only after LVGL has flushed and DRM exposes the newly presented
    // scanout buffer. Network workers only consume completed JPEG frames.
    aera::remote::CaptureAfterRender();
#endif
    return std::clamp(next, 1U, kFrameIntervalMs);
  }

  Action TakeAction() {
    const Action action = pending_action_;
    pending_action_ = Action::kNone;
    return action;
  }
  bool TakeScreenshotRequest() {
    const bool requested = screenshot_requested_;
    screenshot_requested_ = false;
    return requested;
  }

  DecryptionCompletion TakeDecryptionCompletion() {
    const DecryptionCompletion result = decryption_completion_;
    decryption_completion_ = DecryptionCompletion::kNone;
    return result;
  }

  void NavigateHome() {
    if (pc_transfer_.Expanded()) {
      pc_transfer_.Close();
      pc_transfer_.Refresh(aera::pc::GetUploadProgress());
    }
    if (lock_overlay_ != nullptr) return;
    if (recents_overlay_ != nullptr) {
      DismissRecents();
      return;
    }
    if (backend_ready_ && !operation_running_ && !nas_running_ &&
        !plugin_running_ &&
        widgets::DismissModal(lv_screen_active())) return;
    if (backend_ready_ && current_tool_ == Action::kFiles && !operation_running_ &&
        !nas_running_ && !plugin_running_ && NavigateFileBack()) return;
    if (backend_ready_ && !operation_running_ && !nas_running_ &&
        !plugin_running_ &&
        current_tool_ == Action::kFormatData) {
      HandleSceneAction(Action::kWipe, this);
      return;
    }
    if (backend_ready_ && !decryption_active_ && !on_home_ &&
        !operation_running_ && !nas_running_ && !plugin_running_) {
      navigation_history_.clear();
      ShowHome();
    }
  }

  void NavigateBack(bool feedback = true) {
    if (pc_transfer_.Expanded()) {
      pc_transfer_.Close();
      pc_transfer_.Refresh(aera::pc::GetUploadProgress());
      return;
    }
    if (feedback) RecoveryVibrate(Haptic::kTouch);
    if (recents_overlay_ != nullptr) {
      DismissRecents();
      return;
    }
    if (lock_overlay_ != nullptr || !backend_ready_ || decryption_active_ ||
        operation_running_ || nas_running_ || plugin_running_)
      return;
    if (widgets::DismissModal(lv_screen_active())) return;
    if (fastboot_mode_) {
      if (current_tool_ == Action::kFormatData) ShowFastboot();
      return;
    }
    if (current_scene_ == Action::kFiles && NavigateFileBack()) return;
    if (current_scene_ == Action::kPlugins && NavigatePluginBack(plugin_scene_)) return;
    if (on_home_) return;

    Action target = Action::kBackHome;
    if (!navigation_history_.empty()) {
      target = navigation_history_.back();
      navigation_history_.pop_back();
    }
    navigating_back_ = true;
    HandleSceneAction(target, this);
    navigating_back_ = false;
  }

  void BeginDecryption(int credential_type, bool file_based, int user_id,
                       int pattern_grid_size) {
    if (interactive_ready_ || backend_ready_) return;
    interactive_ready_ = true;
    decryption_active_ = true;
    secondary_decryption_ = false;
    credential_type_ = credential_type;
    crypto_user_id_ = user_id;
    // The renderer starts before /data is guaranteed to be mounted. Retry the
    // early UI cache now so the lock screen uses the selected appearance and
    // language as soon as /data/recovery becomes available.
    ApplyStoredAppearance();
    lv_obj_t *boot_screen = lv_screen_active();
    CompleteBootScene(boot_screen);
    lv_obj_t *decrypt = lv_obj_create(nullptr);
    decrypt_scene_ = BuildDecryptScene(
        decrypt, credential_type, file_based, user_id, pattern_grid_size,
        HandleSceneAction, this);
    on_home_ = false;
    lv_screen_load_anim(decrypt, LV_SCR_LOAD_ANIM_FADE_IN, 820, 0, true);
  }

  void SetBackendReady() {
    if (backend_ready_)
      return;
    // Shared-storage preferences may not be readable during the early boot
    // renderer. Reload them before constructing the first interactive scene.
    ApplyStoredAppearance();
    backend_ready_ = true;
    if (interactive_ready_) {
      ShowHome();
      return;
    }
    interactive_ready_ = true;
    // Backend readiness starts both halves of one synchronized handoff: the
    // wordmark zooms on the global overlay while the functional home screen
    // fades up underneath it.
    lv_obj_t *boot_screen = lv_screen_active();
    CompleteBootScene(boot_screen);
    lv_obj_t *home = lv_obj_create(nullptr);
    BuildHomeScene(home, HandleSceneAction, this);
    on_home_ = true;
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_FADE_IN, 820, 0, true);
  }

  void SetFastbootMode(bool enabled, bool show_recovery_home) {
    if (!initialized_ || fastboot_mode_ == enabled) return;
    CancelEdgeSwipe();
    CancelRecentsSwipe();
    DismissRecents();
    DismissPowerMenu();
    widgets::DismissModal(lv_screen_active());
    navigation_history_.clear();
    fastboot_mode_ = enabled;
    backend_ready_ = true;
    interactive_ready_ = true;
    if (enabled) {
      ShowFastboot(true);
    } else {
      ApplyStoredAppearance();
      if (show_recovery_home) {
        ShowHome(true, true);
      } else {
        // A cold fastbootd boot has not passed recovery's decryption gate.
        // Leave the current frame in place until BeginDecryption() replaces
        // it, while allowing the normal backend-ready handoff afterwards.
        backend_ready_ = false;
        interactive_ready_ = false;
      }
    }
  }

  hardware_keys::State physical_keys_;
  void ResetKeyboard() {
    physical_keys_.Reset();
  }
  bool KeyboardKey(unsigned code, int value) {
    if (!physical_keys_.Update(code, value)) return false;
    if (suspended_ || lock_overlay_ || power_overlay_) { ResetKeyboard(); return true; }
    if (!hardware_keyboard::Handle(code, value, physical_keys_) &&
        code == KEY_ESC && value == 1) NavigateBack();
    return true;
  }

  void SetMouseConnected(bool connected) {
    if (!mouse_device_) return;
    if (!connected && mouse_connected_) {
      mouse_pressed_ = false;
      lv_indev_reset(mouse_device_, nullptr); // Disconnect must not click.
    }
    mouse_connected_ = connected;
    if (!connected || suspended_) lv_obj_add_flag(mouse_cursor_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(mouse_cursor_, LV_OBJ_FLAG_HIDDEN);
  }

  void MouseMove(int dx, int dy) {
    if (!mouse_device_ || suspended_) return;
    const int w = landscape_ ? height_ : width_;
    const int h = landscape_ ? width_ : height_;
    mouse_x_ = std::clamp(mouse_x_ + std::clamp(dx, -4096, 4096), 0, w - 1);
    mouse_y_ = std::clamp(mouse_y_ + std::clamp(dy, -4096, 4096), 0, h - 1);
  }

  void MouseButton(bool pressed) {
    if (pressed) hardware_keyboard::ClearFocus();
    if (!mouse_device_ || suspended_) return;
    mouse_pressed_ = pressed;
    // Consume every button edge even when press+release arrive in one frame.
    lv_indev_read(mouse_device_);
  }

  void MouseWheel(int steps) {
    if (!mouse_device_ || suspended_ || mouse_pressed_) return;
    lv_point_t point{mouse_x_, mouse_y_};
    lv_obj_t *target = lv_indev_search_obj(lv_layer_sys(), &point);
    if (!target) target = lv_indev_search_obj(lv_layer_top(), &point);
    if (!target) target = lv_indev_search_obj(lv_screen_active(), &point);
    for (; target; target = lv_obj_get_parent(target)) {
      if (lv_obj_has_flag(target, LV_OBJ_FLAG_SCROLLABLE) &&
          (lv_obj_get_scroll_dir(target) & LV_DIR_VER) &&
          (lv_obj_get_scroll_top(target) > 0 || lv_obj_get_scroll_bottom(target) > 0)) {
        lv_obj_scroll_by(target, 0, std::clamp(steps, -8, 8) * 96, LV_ANIM_OFF);
        return;
      }
    }
  }

  void SetPointer(const PointerEvent &event) {
    if (event.pressed) hardware_keyboard::ClearFocus();
    const int32_t logical_x = transform_.ToLogicalX(event.x);
    const int32_t logical_y = transform_.ToLogicalY(event.y);
    // The Qualcomm OpenGL scanout presents LVGL's 90-degree texture in the
    // opposite quarter-turn from lv_display_rotate_point().  Keep AERA's
    // pointer state in the coordinates that are actually visible on screen;
    // ReadPointer() converts them back to LVGL's pre-rotation coordinate
    // space below.  Doing this here also gives the browser, picture viewer,
    // and edge gestures the same landscape coordinates as normal LVGL UI.
    int32_t visible_x = logical_x;
    int32_t visible_y = logical_y;
    if (landscape_) {
      if (gpu_accelerated_) {
        visible_x = logical_y;
        visible_y = width_ - logical_x - 1;
      } else {
        visible_x = height_ - logical_y - 1;
        visible_y = logical_x;
      }
    }
    RoutePointer(event.slot, visible_x, visible_y, event.pressed);
  }

  // Routes one contact event, in visible-screen coordinates.
  void RoutePointer(int slot, int32_t visible_x, int32_t visible_y,
                    bool pressed) {
    bool first_contact = false;
    if (slot == 0) {
      first_contact = pressed && !slot0_down_;
      slot0_down_ = pressed;
      // A new contact ends one that was held and never released.
      if (first_contact) recents_pending_ = false;
    }
    // Global raw-input consumers live below lock/operation overlays. Keep
    // those consumers from stealing contacts that belong to trusted AERA UI.
    if (widgets::PointerDragActive() || suspended_ || lock_overlay_ != nullptr ||
        power_overlay_ != nullptr ||
        !backend_ready_ ||
        operation_running_ || nas_running_ || plugin_running_ ||
        update_installing_ ||
        (update_running_ && update_task_ == UpdateTask::kDownload)) {
      if (slot != 0) return;
      pointer_.x = visible_x;
      pointer_.y = visible_y;
      pointer_.pressed = pressed;
      CancelEdgeSwipe();
      CancelRecentsSwipe();
      return;
    }
    if (slot == 0 && recents_swipe_active_) {
      pointer_.x = visible_x;
      pointer_.y = visible_y;
      pointer_.pressed = false;
      if (pressed)
        UpdateRecentsSwipe(visible_x, visible_y);
      else
        FinishRecentsSwipe();
      return;
    }
    if (recents_overlay_ != nullptr) {
      if (slot != 0) return;
      pointer_.x = visible_x;
      pointer_.y = visible_y;
      pointer_.pressed = recents_transition_active_ ? false : pressed;
      CancelEdgeSwipe();
      return;
    }
    // A contact that lands in the bottom strip may be the Recents swipe.
    // Decide by movement, as Android's gesture navigation does: nothing is
    // shown while it stays within the touch slop; once it moves up, mostly
    // vertically, the swipe starts from where it landed. A tap, or a contact
    // that moves along the edge, goes to what is under it.
    if (slot == 0 && recents_pending_) {
      const int32_t upward = recents_pending_y_ - visible_y;
      const int32_t sideways = std::abs(visible_x - recents_pending_x_);
      const int32_t slop = std::max(16, RecentsStripHeight() / 3);
      if (pressed && upward <= slop && sideways <= slop) return;
      recents_pending_ = false;
      if (pressed && upward > sideways) {
        BeginRecentsSwipe(recents_pending_x_, recents_pending_y_);
        UpdateRecentsSwipe(visible_x, visible_y);
        return;
      }
      // Released within the slop, it is released where it landed: some
      // controllers zero a coordinate on release.
      if (!pressed) {
        visible_x = recents_pending_x_;
        visible_y = recents_pending_y_;
      }
      slot0_down_ = true;
      RoutePointer(0, recents_pending_x_, recents_pending_y_, true);
      if (!pressed && pointer_.pressed) {
        // AERA's own UI has the press: LVGL reads it once before the release,
        // so a tap is a click.
        CancelEdgeSwipe();
        pointer_release_on_read_ = true;
        slot0_down_ = false;
        return;
      }
      slot0_down_ = pressed;
    }
    if (first_contact && RecoveryPreference(Preference::kRecents) &&
        !fastboot_mode_ && !decryption_active_) {
      const int32_t visible_width = landscape_ ? height_ : width_;
      const int32_t visible_height = landscape_ ? width_ : height_;
      const int32_t side_guard = std::max(76, visible_width / 22);
      if (visible_y >= visible_height - RecentsStripHeight() &&
          visible_x > side_guard && visible_x < visible_width - side_guard) {
        pointer_.x = visible_x;
        pointer_.y = visible_y;
        pointer_.pressed = false;
        pointer_release_on_read_ = false;
        CancelEdgeSwipe();
        if (pointer_device_ != nullptr)
          lv_indev_reset(pointer_device_, nullptr);
        recents_pending_ = true;
        recents_pending_x_ = visible_x;
        recents_pending_y_ = visible_y;
        return;
      }
    }
    if (PictureViewerHandlePointer(slot, visible_x, visible_y,
                                   pressed)) {
      CancelEdgeSwipe();
      pointer_.pressed = false;
      if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
      return;
    }
    if (BrowserHandlePointer(slot, visible_x, visible_y, pressed)) {
      CancelEdgeSwipe();
      pointer_.pressed = false;
      if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
      return;
    }
    if (slot != 0) {
      phone_keyboard::HandleSecondaryPointer(visible_x, visible_y, pressed);
      return;
    }
    const bool was_pressed = pointer_.pressed;
    pointer_.x = visible_x;
    pointer_.y = visible_y;
    pointer_.pressed = pressed;
    pointer_release_on_read_ = false;

    // The raw input loop deliberately coalesces motion before each rendered
    // frame so sliders track the newest coordinate instead of replaying an
    // event backlog. A very quick tap can contain both contact edges in that
    // same drain pass, however, which used to leave LVGL seeing only the final
    // released state. Process state edges immediately; motion remains
    // coalesced and fast keyboard taps can no longer disappear between frames.
    if (pressed != was_pressed && pointer_device_ != nullptr)
      lv_indev_read(pointer_device_);

    if (pressed && !was_pressed) {
      const int32_t visible_width = landscape_ ? height_ : width_;
      const int32_t edge = std::max(72, visible_width / 20);
      if (pointer_.x <= edge) {
        BeginEdgeSwipe(false);
      } else if (pointer_.x >= visible_width - edge) {
        BeginEdgeSwipe(true);
      }
      return;
    }

    if (pressed && swipe_active_) {
      if (swipe_rejected_) return;
      const int32_t vertical = std::abs(pointer_.y - swipe_start_y_);
      const int32_t visible_height = landscape_ ? width_ : height_;
      if (!edge_contact_captured_ && vertical > visible_height / 8) {
        RejectEdgeSwipe();
        return;
      }
      swipe_last_y_ = pointer_.y;
      const int32_t inward = EdgeSwipeInward(
          swipe_start_x_, pointer_.x, swipe_right_edge_);
      swipe_last_inward_ = inward;
      swipe_max_inward_ = std::max(swipe_max_inward_, inward);
      UpdateEdgeSwipe(inward);
      const int32_t visible_width = landscape_ ? height_ : width_;
      if (!edge_contact_captured_ &&
          swipe_max_inward_ >= visible_width / 6 &&
          swipe_max_inward_ > vertical * 2) {
        edge_contact_captured_ = true;
        if (pointer_device_ != nullptr)
          lv_indev_reset(pointer_device_, nullptr);
      }
      return;
    }

    if (!pressed && was_pressed && swipe_active_) {
      // Several touch controllers zero ABS_MT_POSITION_Y when the tracking ID
      // is released. Validate direction with the last coordinate seen while
      // the contact was still active.
      const bool captured = edge_contact_captured_;
      const int32_t visible_width = landscape_ ? height_ : width_;
      const bool accepted = EdgeSwipeShouldCommit(
          captured, swipe_last_inward_, visible_width);
      const bool right_edge = swipe_right_edge_;
      FinishEdgeSwipe(accepted);
      edge_contact_captured_ = false;
      // A canceled captured gesture must not release-click an underlying
      // control when we give the pointer back to LVGL.
      if (captured && pointer_device_ != nullptr)
        lv_indev_reset(pointer_device_, nullptr);
      if (accepted) {
        RecoveryVibrate(Haptic::kTouch);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "%s-edge Back gesture accepted",
                            right_edge ? "right" : "left");
      }
    }
  }

  int32_t Width() const { return width_; }
  int32_t Height() const { return height_; }
  bool IsLandscape() const { return landscape_; }
  bool IsInitialized() const { return initialized_; }

  void SetSuspended(bool suspended) {
    if (suspended) ResetKeyboard();
    suspended_ = suspended;
    SetMouseConnected(mouse_connected_);
    if (suspended_ && mouse_device_) {
      mouse_pressed_ = false; lv_indev_reset(mouse_device_, nullptr);
    }
    if (!suspended_) return;
    CancelEdgeSwipe();
    CancelRecentsSwipe();
    pointer_.pressed = false;
    if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
  }

  void ShowLockScreen(bool ambient = false) {
    ResetKeyboard();
    if (!initialized_) return;
    if(lock_overlay_ && lock_ambient_==ambient)return;
    if(lock_overlay_){lv_obj_delete(lock_overlay_);lock_overlay_=nullptr;}
    lock_ambient_=ambient;
    CancelEdgeSwipe();
    CancelRecentsSwipe();
    pointer_.pressed = false;
    if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
    lock_overlay_ = BuildLockScene(lv_layer_top(), HandleSceneAction, this,false,nullptr,ambient);
  }

  void ShowPowerMenu() {
    ResetKeyboard();
    if (!initialized_ || power_overlay_ != nullptr || suspended_) return;
    CancelEdgeSwipe();
    CancelRecentsSwipe();
    pointer_.pressed = false;
    if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);

    const bool landscape = landscape_;
    auto *overlay = lv_obj_create(lv_layer_top());
    power_overlay_ = overlay;
    design::Clear(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
      auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
      if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (self != nullptr && self->power_overlay_ ==
            lv_event_get_target_obj(event)) self->power_overlay_ = nullptr;
      } else if (lv_event_get_code(event) == LV_EVENT_CLICKED && self != nullptr) {
        auto *target = lv_event_get_target_obj(event);
        if (target == self->power_overlay_) self->DismissPowerMenu();
      }
    }, LV_EVENT_ALL, this);

    auto *panel = lv_obj_create(overlay);
    design::Panel(panel, 52, design::kMainSheet);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_size(panel, landscape ? 2100 : 1180, landscape ? 620 : 780);
    lv_obj_center(panel);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, design::kMainLine, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_60, 0);
    auto *title = design::Label(panel, "Power menu", &lv_font_montserrat_48,
                                design::kText);
    lv_obj_set_pos(title, 52, 42);
    auto *detail = design::Label(panel, "Choose where AERA should go next",
                                 &lv_font_montserrat_24, design::kMuted);
    lv_obj_set_pos(detail, 52, 108);
    auto *close = widgets::Button(panel, LV_SYMBOL_CLOSE,
                                  [this] { DismissPowerMenu(); });
    lv_obj_set_size(close, 94, 94);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -38, 34);

    struct Destination {
      const char *icon;
      const char *title;
      const char *detail;
      Action action;
    };
    constexpr std::array<Destination, 4> destinations{{
      {LV_SYMBOL_HOME, "Android", "Boot the operating system", Action::kRebootSystem},
      {LV_SYMBOL_REFRESH, "Recovery", "Restart AERA", Action::kRebootRecovery},
      {LV_SYMBOL_SETTINGS, "Bootloader", "Open fastboot mode", Action::kRebootBootloader},
      {LV_SYMBOL_POWER, "Power off", "Shut down the device", Action::kPowerOff},
    }};
    for (size_t index = 0; index < destinations.size(); ++index) {
      const auto destination = destinations[index];
      const int columns = landscape ? 4 : 2;
      const int column = static_cast<int>(index) % columns;
      const int row = static_cast<int>(index) / columns;
      const int button_width = landscape ? 470 : 518;
      const int button_height = landscape ? 330 : 226;
      auto *button = lv_button_create(panel);
      design::Panel(button, 34,
                    destination.action == Action::kPowerOff
                        ? design::kRedSoft : design::kMainPanel);
      design::Interactive(button, design::kMainSelected);
      lv_obj_set_pos(button, 52 + column * (landscape ? 494 : 542),
                     190 + row * 246);
      lv_obj_set_size(button, button_width, button_height);
      widgets::OnClick(button, [this, destination] {
        const bool power_off = destination.action == Action::kPowerOff;
        const std::string transition_title = power_off
            ? "Powering off"
            : std::string("Rebooting to ") + destination.title;
        DismissPowerMenu();
        widgets::PowerTransition(
            lv_layer_top(), transition_title,
            [this, destination] { pending_action_ = destination.action; });
      });
      auto *icon = design::Label(button, destination.icon,
                                 &lv_font_montserrat_48,
                                 destination.action == Action::kPowerOff
                                     ? design::kRed : design::kAccent);
      lv_obj_set_pos(icon, 34, 32);
      auto *name = design::Label(button, destination.title,
                                 &lv_font_montserrat_32, design::kText);
      lv_obj_set_pos(name, 108, 38);
      auto *copy = design::Label(button, destination.detail,
                                 &lv_font_montserrat_24, design::kMuted);
      lv_obj_set_pos(copy, 34, landscape ? 132 : 132);
      lv_obj_set_width(copy, button_width - 68);
    }
    lv_obj_fade_in(panel, 160, 0);
  }

  void ShowVolume(int percent) {
    if (!initialized_) return;
    percent = std::clamp(percent, 0, 100);
    const char *symbol = percent == 0 ? LV_SYMBOL_CLOSE
                                      : (percent < 55 ? LV_SYMBOL_VOLUME_MID
                                                      : LV_SYMBOL_VOLUME_MAX);
    const std::string amount = i18n::Format("Volume  %d%%", percent);

    if (volume_overlay_ == nullptr) {
      auto *panel = lv_obj_create(lv_layer_top());
      volume_overlay_ = panel;
      design::Panel(panel, 66, design::kMainSheet);
      lv_obj_set_size(panel, 1040, 196);
      lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, -220);
      lv_obj_set_style_border_width(panel, 1, 0);
      lv_obj_set_style_border_color(panel, design::kMainLine, 0);
      lv_obj_set_style_border_opa(panel, LV_OPA_60, 0);
      lv_obj_set_style_pad_all(panel, 0, 0);
      lv_obj_add_event_cb(panel, [](lv_event_t *event) {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
        auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
        if (self == nullptr || self->volume_overlay_ != target) return;
        if (self->volume_hide_timer_ != nullptr) {
          lv_timer_delete(self->volume_hide_timer_);
          self->volume_hide_timer_ = nullptr;
        }
        self->volume_overlay_ = nullptr;
        self->volume_icon_ = nullptr;
        self->volume_label_ = nullptr;
        self->volume_fill_ = nullptr;
      }, LV_EVENT_DELETE, this);

      volume_icon_ = design::Label(panel, symbol, &lv_font_montserrat_48,
          percent == 0 ? design::kMuted : design::kAccent);
      lv_obj_set_pos(volume_icon_, 62, 54);
      volume_label_ = design::Label(panel, amount.c_str(), &lv_font_montserrat_32,
                                    design::kText);
      lv_obj_set_pos(volume_label_, 150, 34);
      auto *track = lv_obj_create(panel);
      design::Clear(track);
      lv_obj_set_pos(track, 150, 108);
      lv_obj_set_size(track, 810, 24);
      lv_obj_set_style_radius(track, 12, 0);
      lv_obj_set_style_bg_color(track, design::kMainLine, 0);
      lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
      volume_fill_ = lv_obj_create(track);
      design::Clear(volume_fill_);
      lv_obj_set_height(volume_fill_, 24);
      lv_obj_set_style_radius(volume_fill_, 12, 0);
      lv_obj_set_style_bg_opa(volume_fill_, LV_OPA_COVER, 0);
      lv_obj_set_width(volume_fill_, std::max(1, displayed_volume_ * 810 / 100));

    } else {
      i18n::BindLabel(volume_icon_, symbol);
      i18n::BindLabel(volume_label_, amount.c_str());
    }

    // Keep this top-layer subtree alive and fully opaque. Opacity/transform
    // layers being created over a GPU streaming image can invalidate LVGL's
    // OpenGL layer storage. A real-coordinate slide is equally smooth and
    // does not allocate a compositing layer.
    if (lv_obj_get_y(volume_overlay_) != 220) {
      lv_anim_delete(volume_overlay_, nullptr);
      lv_anim_t enter;
      lv_anim_init(&enter);
      lv_anim_set_var(&enter, volume_overlay_);
      lv_anim_set_values(&enter, lv_obj_get_y(volume_overlay_), 220);
      lv_anim_set_duration(&enter, 230);
      lv_anim_set_path_cb(&enter, lv_anim_path_ease_out);
      lv_anim_set_exec_cb(&enter, [](void *target, int32_t value) {
        lv_obj_set_y(static_cast<lv_obj_t *>(target), value);
      });
      lv_anim_start(&enter);
    }

    if (volume_hide_timer_ == nullptr) {
      volume_hide_timer_ = lv_timer_create([](lv_timer_t *timer) {
        auto *self = static_cast<Impl *>(lv_timer_get_user_data(timer));
        if (self == nullptr) return;
        self->volume_hide_timer_ = nullptr;
        auto *panel = self->volume_overlay_;
        if (panel != nullptr) {
          lv_anim_delete(panel, nullptr);
          lv_anim_t leave;
          lv_anim_init(&leave);
          lv_anim_set_var(&leave, panel);
          lv_anim_set_values(&leave, lv_obj_get_y(panel), -220);
          lv_anim_set_duration(&leave, 210);
          lv_anim_set_path_cb(&leave, lv_anim_path_ease_in);
          lv_anim_set_exec_cb(&leave, [](void *target, int32_t value) {
            lv_obj_set_y(static_cast<lv_obj_t *>(target), value);
          });
          lv_anim_start(&leave);
        }
      }, 1350, this);
      lv_timer_set_repeat_count(volume_hide_timer_, 1);
    } else {
      lv_timer_reset(volume_hide_timer_);
      lv_timer_set_repeat_count(volume_hide_timer_, 1);
    }

    lv_obj_set_style_text_color(volume_icon_,
        percent == 0 ? design::kMuted : design::kAccent, 0);
    lv_obj_set_style_bg_color(volume_fill_,
        percent == 0 ? design::kMuted : design::kAccent, 0);
    const int start_width = lv_obj_get_width(volume_fill_);
    const int end_width = percent * 810 / 100;
    lv_anim_delete(volume_fill_, nullptr);
    lv_anim_t level;
    lv_anim_init(&level);
    lv_anim_set_var(&level, volume_fill_);
    lv_anim_set_values(&level, std::max(1, start_width),
                       std::max(1, end_width));
    lv_anim_set_duration(&level, 210);
    lv_anim_set_path_cb(&level, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&level, [](void *target, int32_t value) {
      lv_obj_set_width(static_cast<lv_obj_t *>(target), value);
    });
    lv_anim_start(&level);
    displayed_volume_ = percent;
  }

  void PollUsbAttachment() {
    const uint32_t now = MonotonicMilliseconds();
    if (now - last_usb_poll_ < 1000) return;
    last_usb_poll_ = now;
    const auto storage = usb_attachments_.Update(UsbAttachments());
    const auto inputs = ExternalInputAttachments();
    const auto mouse = mouse_attachments_.Update(inputs.mice);
    const auto keyboard = keyboard_attachments_.Update(inputs.keyboards);
    if (!storage && !mouse && !keyboard) return;
    std::string message;
    const auto append = [&message](const AttachmentChanges &changes,
                                  const char *connected, const char *disconnected) {
      for (const auto &line : {changes.disconnected ? disconnected : "",
                               changes.connected ? connected : ""}) {
        if (!*line) continue;
        if (!message.empty()) message += "\n";
        message += i18n::Translate(line);
      }
    };
    append(storage, "USB storage connected", "USB storage disconnected");
    append(mouse, "Mouse connected", "Mouse disconnected");
    append(keyboard, "Keyboard connected", "Keyboard disconnected");
    // A passive notice must never cover a destructive operation, unlock prompt,
    // or fastboot handover. Suppressed notices are not replayed later.
    if (!interactive_ready_ || suspended_ || decryption_active_ || decrypt_running_ ||
        operation_running_ || fastboot_mode_ || lock_overlay_ || power_overlay_ ||
        recents_overlay_ || StatusBarShadeOpen()) return;
    if (notice_overlay_) lv_obj_delete(notice_overlay_);
    auto *panel = lv_obj_create(lv_layer_top());
    notice_overlay_ = panel;
    design::Panel(panel, 42, design::kMainSheet);
    const int lines = 1 + std::count(message.begin(), message.end(), '\n');
    lv_obj_set_size(panel, std::min(1080, static_cast<int>(lv_obj_get_width(lv_layer_top())) - 80), 100 + lines * 60);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(panel, [](lv_event_t *event) {
      auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
      if (self->notice_overlay_ == lv_event_get_target(event)) self->notice_overlay_ = nullptr;
    }, LV_EVENT_DELETE, this);
    auto *label = design::Label(panel, message.c_str(), &lv_font_montserrat_36, design::kText);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_width(label, LV_PCT(90));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_fade_in(panel, 160, 0);
    lv_obj_delete_delayed(panel, 3500);
  }

  void ShowScreenshotResult(bool success) {
    if (!initialized_) return;
    if (success) {
      if (screenshot_flash_) lv_obj_delete(screenshot_flash_);
      screenshot_flash_ = ScreenshotFlash(lv_layer_top());
      lv_obj_add_event_cb(screenshot_flash_, [](lv_event_t *event) {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
        if (self->screenshot_flash_ == lv_event_get_target(event)) self->screenshot_flash_ = nullptr;
      }, LV_EVENT_DELETE, this);
      return;
    }
    if (notice_overlay_ != nullptr) lv_obj_delete(notice_overlay_);
    auto *panel = lv_obj_create(lv_layer_top());
    notice_overlay_ = panel;
    design::Panel(panel, 58, design::kMainSheet);
    lv_obj_set_size(panel, 930, 154);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 440);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, success ? design::kGreen
                                                 : design::kRed, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_50, 0);
    lv_obj_add_event_cb(panel, [](lv_event_t *event) {
      auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
      auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
      if (self != nullptr && self->notice_overlay_ == target)
        self->notice_overlay_ = nullptr;
    }, LV_EVENT_DELETE, this);
    auto *icon = design::Label(panel, success ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                               &lv_font_montserrat_32,
                               success ? design::kGreen : design::kRed);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 46, 0);
    auto *label = design::Label(panel,
        success ? "Screenshot saved to AERA/screenshots"
                : "Screenshot could not be saved",
        &lv_font_montserrat_24, design::kText);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 118, 0);
    lv_obj_set_style_opa(panel, LV_OPA_0, 0);
    lv_obj_fade_in(panel, 180, 0);
    lv_obj_delete_delayed(panel, 1800);
  }

  void ShowUpdateBanner(const std::string &version) {
    if (!initialized_ || suspended_) return;
    if (notice_overlay_ != nullptr) lv_obj_delete(notice_overlay_);
    auto *panel = lv_obj_create(lv_layer_top());
    notice_overlay_ = panel;
    design::Panel(panel, 42, design::kMainSheet);
    lv_obj_set_size(panel, 1080, 176);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, -196);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, design::kAccent, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_add_event_cb(panel, [](lv_event_t *event) {
      auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
      auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
      if (self != nullptr && self->notice_overlay_ == target)
        self->notice_overlay_ = nullptr;
    }, LV_EVENT_DELETE, this);
    auto *icon_plate = lv_obj_create(panel);
    design::Panel(icon_plate, 26, design::kAccentSoft);
    lv_obj_set_pos(icon_plate, 34, 30);
    lv_obj_set_size(icon_plate, 116, 116);
    auto *icon = design::Label(icon_plate, LV_SYMBOL_DOWNLOAD,
                               &lv_font_montserrat_40, design::kAccent);
    lv_obj_center(icon);
    const std::string title =
        i18n::Format("AERA %s is available", version.c_str());
    auto *label = design::Label(panel, title.c_str(),
                                &lv_font_montserrat_32, design::kText);
    lv_obj_set_pos(label, 184, 34);
    lv_obj_set_width(label, 840);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    auto *detail = design::Label(panel, "Tap to review the recovery update",
                                 &lv_font_montserrat_24,
                                 design::kMutedStrong);
    lv_obj_set_pos(detail, 184, 94);
    widgets::OnClick(panel, [this] {
      if (notice_overlay_ != nullptr) {
        auto *notice = notice_overlay_;
        notice_overlay_ = nullptr;
        lv_obj_delete_async(notice);
      }
      HandleSceneAction(Action::kUpdates, this);
    });
    lv_anim_t enter;
    lv_anim_init(&enter);
    lv_anim_set_var(&enter, panel);
    lv_anim_set_values(&enter, lv_obj_get_y(panel), 190);
    lv_anim_set_duration(&enter, 240);
    lv_anim_set_path_cb(&enter, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&enter, [](void *target, int32_t value) {
      lv_obj_set_y(static_cast<lv_obj_t *>(target), value);
    });
    lv_anim_start(&enter);
    lv_obj_delete_delayed(panel, 5000);
  }

private:
  void ApplyStoredAppearance() {
    i18n::Initialize(RecoveryLanguage());
    const std::string font_id = RecoveryUiFont();
    plugins::Plugin font_plugin;
    std::string font_path;
    std::string font_error;
    if (font_id.empty() ||
        !plugins::ResolvePayload(font_id, font_plugin, font_path, font_error) ||
        !plugins::IsThemeExtension(font_plugin) ||
        !fonts::SelectUiFont(font_id, font_path)) {
      fonts::SelectUiFont({}, {});
    }
    design::ApplyAppearanceMode(static_cast<int>(RecoveryAppearanceMode()));
    design::ApplySurfaceStyle(static_cast<int>(RecoverySurfaceStyle()));
    design::ApplyAccent(RecoveryAccentColor());
    design::ApplyInterfaceSize(static_cast<int>(RecoveryInterfaceSize()));
    const std::string wallpaper_path = RecoveryWallpaperPath();
    if (wallpaper_path.empty()) {
      wallpaper::Clear();
    } else {
      std::string ignored;
      wallpaper::Load(wallpaper_path, &ignored);
    }
  }

  void RememberRecentApp(Action action) {
    if (!IsRecentAppAction(action)) return;
    RecentApp app = DescribeRecentApp(action);
    if (app.title.empty()) return;
    const auto existing = std::find_if(
        recent_apps_.begin(), recent_apps_.end(), [&](const RecentApp &item) {
          return item.action == app.action && item.plugin_id == app.plugin_id;
        });
    if (existing != recent_apps_.end()) app.preview = existing->preview;
    recent_apps_.erase(
        std::remove_if(recent_apps_.begin(), recent_apps_.end(),
                       [&](const RecentApp &item) {
                         return item.action == app.action &&
                             item.plugin_id == app.plugin_id;
                       }),
        recent_apps_.end());
    recent_apps_.insert(recent_apps_.begin(), std::move(app));
    if (recent_apps_.size() > 7) recent_apps_.resize(7);
  }

  std::shared_ptr<RecentPreview> CaptureScreenPreview() {
    auto pixels = std::make_shared<PictureData>();
    unsigned int width = 0;
    unsigned int height = 0;
    if (gr_capture_scaled_bgra(600, 1440, &pixels->pixels, &width, &height) !=
            0 ||
        pixels->pixels == nullptr || width == 0 || height == 0)
      return nullptr;
    pixels->width = width;
    pixels->height = height;
    RoundRecentPreview(*pixels);

    auto preview = std::make_shared<RecentPreview>();
    preview->pixels = std::move(pixels);
    preview->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
    preview->descriptor.header.w = preview->pixels->width;
    preview->descriptor.header.h = preview->pixels->height;
    preview->descriptor.header.stride = preview->pixels->width * 4;
    preview->descriptor.data_size =
        preview->pixels->width * preview->pixels->height * 4;
    preview->descriptor.data = preview->pixels->pixels;
    return preview;
  }

  void CaptureRecentPreview(Action action) {
    if (!IsRecentAppAction(action) || recents_overlay_ != nullptr) return;
    RememberRecentApp(action);

    const RecentApp identity = DescribeRecentApp(action);
    auto entry = std::find_if(
        recent_apps_.begin(), recent_apps_.end(), [&](const RecentApp &item) {
          return item.action == identity.action &&
              item.plugin_id == identity.plugin_id;
        });
    if (entry == recent_apps_.end()) return;

    auto preview = CaptureScreenPreview();
    if (preview != nullptr) entry->preview = std::move(preview);
  }

  void DismissRecents() {
    if (recents_overlay_ == nullptr) return;
    auto *overlay = recents_overlay_;
    recents_overlay_ = nullptr;
    recents_transition_active_ = false;
    recents_deferred_cards_.clear();
    recents_all_cards_.clear();
    lv_obj_fade_out(overlay, 150, 0);
    lv_obj_delete_delayed(overlay, 160);
  }

  void LayoutRecentLaunch(int32_t progress) {
    if (recent_launch_card_ == nullptr || recent_launch_preview_ == nullptr)
      return;
    progress = std::clamp(progress, 0, 256);
    const int screen_width = landscape_ ? height_ : width_;
    const int screen_height = landscape_ ? width_ : height_;
    const int width = recent_launch_start_width_ +
        (screen_width - recent_launch_start_width_) * progress / 256;
    const int height = recent_launch_start_height_ +
        (screen_height - recent_launch_start_height_) * progress / 256;
    const int x = recent_launch_start_x_ * (256 - progress) / 256;
    const int y = recent_launch_start_y_ * (256 - progress) / 256;
    lv_obj_set_pos(recent_launch_card_, x, y);
    lv_obj_set_size(recent_launch_card_, width, height);
    lv_obj_set_style_radius(recent_launch_card_, 58 * (256 - progress) / 256,
                            0);
    lv_obj_set_style_border_opa(
        recent_launch_card_,
        static_cast<lv_opa_t>(153 * (256 - progress) / 256), 0);
    const uint32_t image_scale = std::max(
        (static_cast<uint32_t>(width) * 256 +
         recent_launch_preview_->pixels->width - 1) /
            recent_launch_preview_->pixels->width,
        (static_cast<uint32_t>(height) * 256 +
         recent_launch_preview_->pixels->height - 1) /
            recent_launch_preview_->pixels->height);
    lv_image_set_scale(recent_launch_image_, image_scale);
    lv_obj_center(recent_launch_image_);
    if (recent_launch_footer_ != nullptr)
      lv_obj_set_style_opa(
          recent_launch_footer_,
          static_cast<lv_opa_t>(255 * (256 - progress) / 256), 0);
    if (recents_overlay_ != nullptr)
      lv_obj_set_style_opa(
          recents_overlay_,
          static_cast<lv_opa_t>(255 * (256 - progress) / 256), 0);
    lv_obj_invalidate(recent_launch_layer_);
  }

  static void SetRecentLaunchProgress(void *target, int32_t value) {
    auto *self = static_cast<Impl *>(target);
    if (self != nullptr) self->LayoutRecentLaunch(value);
  }

  static void CompleteRecentLaunch(lv_anim_t *animation) {
    auto *self = static_cast<Impl *>(lv_anim_get_user_data(animation));
    if (self == nullptr || !self->recent_launch_active_) return;
    const RecentApp app = self->recent_launch_app_;
    if (!app.plugin_id.empty()) SetSelectedPluginId(app.plugin_id);

    // Keep the old switcher marked as present while the destination scene is
    // constructed, preventing the outgoing app from being recaptured with a
    // transition overlay on top of it.
    HandleSceneAction(app.action, self);
    if (self->recents_overlay_ != nullptr) {
      auto *overlay = self->recents_overlay_;
      self->recents_overlay_ = nullptr;
      lv_obj_delete(overlay);
    }
    self->recents_all_cards_.clear();
    self->recents_deferred_cards_.clear();
    if (self->recent_launch_layer_ != nullptr)
      lv_obj_delete(self->recent_launch_layer_);
    self->recent_launch_layer_ = nullptr;
    self->recent_launch_card_ = nullptr;
    self->recent_launch_image_ = nullptr;
    self->recent_launch_footer_ = nullptr;
    self->recent_launch_preview_.reset();
    self->recent_launch_active_ = false;
    self->recents_transition_active_ = false;
  }

  void BeginRecentLaunch(const RecentApp &app, lv_obj_t *source_card) {
    if (recent_launch_active_ || source_card == nullptr) return;
    if (app.preview == nullptr || app.preview->pixels == nullptr ||
        app.preview->pixels->pixels == nullptr) {
      DismissRecents();
      if (!app.plugin_id.empty()) SetSelectedPluginId(app.plugin_id);
      HandleSceneAction(app.action, this);
      return;
    }

    lv_obj_update_layout(recents_overlay_);
    lv_area_t area{};
    lv_obj_get_coords(source_card, &area);
    recent_launch_start_x_ = area.x1;
    recent_launch_start_y_ = area.y1;
    recent_launch_start_width_ = lv_area_get_width(&area);
    recent_launch_start_height_ = lv_area_get_height(&area);
    recent_launch_app_ = app;
    recent_launch_preview_ = app.preview;
    recent_launch_active_ = true;
    recents_transition_active_ = true;

    const int screen_width = landscape_ ? height_ : width_;
    const int screen_height = landscape_ ? width_ : height_;
    recent_launch_layer_ = lv_obj_create(lv_layer_top());
    design::Clear(recent_launch_layer_);
    lv_obj_set_size(recent_launch_layer_, screen_width, screen_height);
    lv_obj_remove_flag(recent_launch_layer_, LV_OBJ_FLAG_CLICKABLE);

    recent_launch_card_ = lv_obj_create(recent_launch_layer_);
    design::Clear(recent_launch_card_);
    lv_obj_set_pos(recent_launch_card_, recent_launch_start_x_,
                   recent_launch_start_y_);
    lv_obj_set_size(recent_launch_card_, recent_launch_start_width_,
                    recent_launch_start_height_);
    lv_obj_set_style_bg_opa(recent_launch_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(recent_launch_card_, 58, 0);
    lv_obj_set_style_border_width(recent_launch_card_, 2, 0);
    lv_obj_set_style_border_color(recent_launch_card_, design::kAccent, 0);
    lv_obj_set_style_border_opa(recent_launch_card_, LV_OPA_60, 0);
    lv_obj_set_style_border_post(recent_launch_card_, true, 0);
    lv_obj_remove_flag(recent_launch_card_, LV_OBJ_FLAG_CLICKABLE);

    recent_launch_image_ = lv_image_create(recent_launch_card_);
    lv_image_set_src(recent_launch_image_, &recent_launch_preview_->descriptor);
    lv_image_set_antialias(recent_launch_image_, true);
    lv_obj_remove_flag(recent_launch_image_, LV_OBJ_FLAG_CLICKABLE);

    recent_launch_footer_ = lv_obj_create(recent_launch_card_);
    design::Panel(recent_launch_footer_, landscape_ ? 34 : 42,
                  design::kCanvas);
    lv_obj_remove_flag(recent_launch_footer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(recent_launch_footer_,
                    recent_launch_start_width_ - (landscape_ ? 40 : 48),
                    landscape_ ? 126 : 174);
    lv_obj_align(recent_launch_footer_, LV_ALIGN_BOTTOM_MID, 0,
                 landscape_ ? -20 : -24);
    lv_obj_set_style_bg_color(recent_launch_footer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(recent_launch_footer_, LV_OPA_60, 0);
    auto *plate = design::IconPlate(
        recent_launch_footer_, app.icon.c_str(), design::kAccent,
        design::kAccentSoft, landscape_ ? 82 : 104);
    lv_obj_set_pos(plate, landscape_ ? 20 : 24, landscape_ ? 22 : 34);
    auto *name = design::Label(recent_launch_footer_, app.title.c_str(),
                               &lv_font_montserrat_36, design::kText);
    lv_obj_set_pos(name, landscape_ ? 124 : 154, landscape_ ? 18 : 30);
    auto *summary = design::Label(recent_launch_footer_, app.summary.c_str(),
                                  &lv_font_montserrat_24,
                                  design::kMutedStrong);
    lv_obj_set_pos(summary, landscape_ ? 124 : 154, landscape_ ? 70 : 88);
    auto *status = design::Label(recent_launch_footer_, "OPENING",
                                 &lv_font_montserrat_18, design::kAccent);
    lv_obj_set_style_text_letter_space(status, 2, 0);
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, landscape_ ? -24 : -30, 0);
    LayoutRecentLaunch(0);
    for (auto *card : recents_all_cards_)
      lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t zoom;
    lv_anim_init(&zoom);
    lv_anim_set_var(&zoom, this);
    lv_anim_set_values(&zoom, 0, 256);
    lv_anim_set_duration(&zoom, 300);
    lv_anim_set_path_cb(&zoom, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&zoom, SetRecentLaunchProgress);
    lv_anim_set_user_data(&zoom, this);
    lv_anim_set_completed_cb(&zoom, CompleteRecentLaunch);
    lv_anim_start(&zoom);
  }

  void PrepareRecentsTargetGeometry() {
    const bool landscape = landscape_;
    const int screen_width = landscape ? height_ : width_;
    const int screen_height = landscape ? width_ : height_;
    const int tray_y = landscape ? 310 : 444;
    const int tray_height =
        screen_height - tray_y - (landscape ? 158 : 214);
    if (recent_apps_.empty()) {
      recents_target_width_ = landscape ? 1160 : 1120;
      recents_target_height_ = std::min(
          tray_height - 36, landscape ? 690 : 1480);
      recents_target_x_ = (screen_width - recents_target_width_) / 2;
    } else {
      recents_target_width_ = landscape ? 1030 : 1080;
      recents_target_height_ = std::min(
          tray_height - 36,
          static_cast<int>((static_cast<int64_t>(recents_target_width_) *
                            screen_height + screen_width / 2) /
                           screen_width));
      recents_target_x_ =
          std::max(40, (screen_width - recents_target_width_) / 2);
    }
    recents_target_y_ =
        tray_y + (tray_height - recents_target_height_) / 2;
  }

  void ShowRecents(bool gesture_transition = false) {
    if (recents_overlay_ != nullptr || fastboot_mode_ || !backend_ready_)
      return;
    pointer_.pressed = false;
    recents_deferred_cards_.clear();
    recents_all_cards_.clear();
    if (pointer_device_ != nullptr) lv_indev_reset(pointer_device_, nullptr);
    CancelEdgeSwipe();

    const bool landscape = landscape_;
    const int screen_width = landscape ? height_ : width_;
    const int screen_height = landscape ? width_ : height_;
    PrepareRecentsTargetGeometry();
    auto *overlay = lv_obj_create(lv_layer_top());
    recents_overlay_ = overlay;
    recents_transition_active_ = gesture_transition;
    design::Clear(overlay);
    lv_obj_set_size(overlay, screen_width, screen_height);
    lv_obj_set_style_bg_color(overlay, design::kCanvas, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    if (gesture_transition)
      lv_obj_set_style_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
      auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
      auto *target = lv_event_get_target_obj(event);
      if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (self != nullptr && self->recents_overlay_ == target) {
          self->recents_overlay_ = nullptr;
          self->recents_transition_active_ = false;
          self->recents_deferred_cards_.clear();
          self->recents_all_cards_.clear();
        }
      } else if (lv_event_get_code(event) == LV_EVENT_CLICKED && self != nullptr &&
                 target == self->recents_overlay_) {
        self->DismissRecents();
      }
    }, LV_EVENT_ALL, this);

    auto *eyebrow = design::Label(overlay, "AERA APP SWITCHER",
                                  &lv_font_montserrat_18, design::kAccent);
    lv_obj_set_style_text_letter_space(eyebrow, 4, 0);
    lv_obj_set_pos(eyebrow, landscape ? 96 : 80,
                   landscape ? 154 : 218);
    auto *title = design::Label(overlay, "Recents", &lv_font_montserrat_48,
                               design::kText);
    lv_obj_set_pos(title, landscape ? 96 : 80, landscape ? 202 : 274);
    auto *detail = design::Label(
        overlay, "Tap an app to return. Swipe up and hold anywhere in AERA.",
        &lv_font_montserrat_24, design::kMutedStrong);
    lv_obj_set_pos(detail, landscape ? 350 : 80,
                   landscape ? 218 : 348);

    auto style_action = [](lv_obj_t *button, bool accent) {
      lv_obj_set_style_bg_color(button, design::kMainSheet, 0);
      lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
      lv_obj_set_style_border_width(button, 1, 0);
      lv_obj_set_style_border_color(
          button, accent ? design::kAccent : design::kMainLine, 0);
      lv_obj_set_style_border_opa(button, accent ? LV_OPA_60 : LV_OPA_40, 0);
      lv_obj_set_style_shadow_color(button, lv_color_black(), 0);
      lv_obj_set_style_shadow_width(button, 18, 0);
      lv_obj_set_style_shadow_offset_y(button, 8, 0);
      lv_obj_set_style_shadow_opa(button, LV_OPA_30, 0);
      if (lv_obj_get_child_count(button) > 0) {
        auto *label = lv_obj_get_child(button, 0);
        lv_obj_set_style_text_color(
            label, accent ? design::kAccent : design::kText, 0);
      }
    };

    // A labelled glass control reads more clearly than an isolated floating X.
    auto *done = widgets::Button(overlay, LV_SYMBOL_CLOSE "  Done",
                                 [this] { DismissRecents(); });
    lv_obj_set_size(done, landscape ? 210 : 230,
                    landscape ? 82 : 94);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, landscape ? -72 : -64,
                 landscape ? 166 : 234);
    style_action(done, true);

    // Keep navigation and list management together in a small bottom dock,
    // leaving the header focused on the switcher title.
    const bool has_recents = !recent_apps_.empty();
    auto *actions = lv_obj_create(overlay);
    design::Panel(actions, 66, design::kMainSheet);
    lv_obj_set_size(actions,
                    landscape ? (has_recents ? 620 : 310)
                              : (has_recents ? 720 : 350),
                    landscape ? 112 : 132);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0,
                 landscape ? -24 : -34);
    lv_obj_set_style_bg_opa(actions, LV_OPA_80, 0);
    lv_obj_set_style_border_width(actions, 1, 0);
    lv_obj_set_style_border_color(actions, design::kMainLine, 0);
    lv_obj_set_style_border_opa(actions, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(actions, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(actions, 22, 0);
    lv_obj_set_style_shadow_offset_y(actions, 10, 0);
    lv_obj_set_style_shadow_opa(actions, LV_OPA_30, 0);

    const int action_height = landscape ? 88 : 104;
    const int action_width = landscape ? 290 : 330;
    auto *home = widgets::Button(actions, LV_SYMBOL_HOME "  Home",
                                 [this] { ShowHome(); });
    lv_obj_set_size(home, action_width, action_height);
    lv_obj_align(home, has_recents ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER,
                 has_recents ? (landscape ? 10 : 12) : 0, 0);
    style_action(home, true);
    if (has_recents) {
      auto *clear = widgets::Button(actions, LV_SYMBOL_TRASH "  Clear all",
                                    [this] {
        recent_apps_.clear();
        // Browser is the one app runtime intentionally kept alive between
        // scenes. Other app workers are owned by their scene and stop when
        // ShowHome replaces that scene.
        ShutdownWebRuntime();
        ShowHome();
      });
      lv_obj_set_size(clear, action_width, action_height);
      lv_obj_align(clear, LV_ALIGN_RIGHT_MID, landscape ? -10 : -12, 0);
      style_action(clear, false);
    }

    const int tray_y = landscape ? 310 : 444;
    const int tray_height = screen_height - tray_y - (landscape ? 158 : 214);
    auto *tray = lv_obj_create(overlay);
    design::Clear(tray);
    lv_obj_set_pos(tray, 0, tray_y);
    lv_obj_set_size(tray, screen_width, tray_height);
    lv_obj_set_flex_flow(tray, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tray, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(tray, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tray, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(tray, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(tray, LV_SCROLLBAR_MODE_OFF);

    if (recent_apps_.empty()) {
      auto *empty = lv_obj_create(tray);
      design::Panel(empty, 54, design::kMainSheet);
      lv_obj_set_size(empty, recents_target_width_, recents_target_height_);
      lv_obj_set_style_border_width(empty, 1, 0);
      lv_obj_set_style_border_color(empty, design::kMainLine, 0);
      lv_obj_set_style_border_opa(empty, LV_OPA_50, 0);
      auto *icon = design::Label(empty, LV_SYMBOL_LIST,
                                 &lv_font_montserrat_48, design::kAccent);
      lv_obj_set_style_transform_scale(icon, 520, 0);
      lv_obj_align(icon, LV_ALIGN_CENTER, 0, -150);
      auto *empty_title = design::Label(empty, "No recent apps",
                                        &lv_font_montserrat_48,
                                        design::kText);
      lv_obj_align(empty_title, LV_ALIGN_CENTER, 0, 70);
      auto *copy = design::Label(
          empty, "Open an installed plugin and it will appear here.",
          &lv_font_montserrat_24, design::kMuted);
      lv_obj_align(copy, LV_ALIGN_CENTER, 0, 145);
    } else {
      const int card_width = recents_target_width_;
      const int card_height = recents_target_height_;
      const int side_padding = recents_target_x_;
      const bool has_current_app = IsRecentAppAction(current_scene_);
      const RecentApp current_app = has_current_app
          ? DescribeRecentApp(current_scene_) : RecentApp{};
      lv_obj_set_style_pad_left(tray, side_padding, 0);
      lv_obj_set_style_pad_right(tray, side_padding, 0);
      lv_obj_set_style_pad_column(tray, landscape ? 42 : 46, 0);
      for (size_t index = 0; index < recent_apps_.size(); ++index) {
        const RecentApp app = recent_apps_[index];
        auto *card = lv_button_create(tray);
        recents_all_cards_.push_back(card);
        design::Panel(card, 58, design::kMainSheet);
        design::Interactive(card, design::kMainSelected);
        lv_obj_set_size(card, card_width, card_height);
        lv_obj_set_style_pad_all(card, 0, 0);
        // Every app is a distinct Recents page, so keep its complete rounded
        // outline visible.  The CURRENT badge identifies the active app; an
        // unrelated dim border made the other previews look stacked beneath
        // the current one.
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, design::kAccent, 0);
        lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
        lv_obj_set_style_border_post(card, true, 0);
        lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
        lv_obj_set_style_shadow_width(card, 28, 0);
        lv_obj_set_style_shadow_offset_y(card, 14, 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
        if (gesture_transition) {
          lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
          recents_deferred_cards_.push_back(card);
        }
        widgets::OnClick(card, [this, app, card] {
          BeginRecentLaunch(app, card);
        });

        auto *preview = lv_obj_create(card);
        design::Panel(preview, 56, design::kCanvas);
        lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(preview, 0, 0);
        lv_obj_set_size(preview, card_width, card_height);
        lv_obj_set_style_bg_grad_color(preview, design::kMainPanel, 0);
        lv_obj_set_style_bg_grad_dir(preview, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(preview, 0, 0);

        if (app.preview != nullptr && app.preview->pixels != nullptr &&
            app.preview->pixels->pixels != nullptr) {
          auto *image = lv_image_create(preview);
          lv_image_set_src(image, &app.preview->descriptor);
          lv_image_set_antialias(image, true);
          const uint32_t scale = std::max(
              (static_cast<uint32_t>(card_width) * 256 +
               app.preview->pixels->width - 1) /
                  app.preview->pixels->width,
              (static_cast<uint32_t>(card_height) * 256 +
               app.preview->pixels->height - 1) /
                  app.preview->pixels->height);
          lv_image_set_scale(image, scale);
          lv_obj_center(image);
          lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
        } else {
          auto *watermark = design::Label(preview, app.icon.c_str(),
                                          &lv_font_montserrat_48,
                                          design::kAccentSoft);
          lv_obj_set_style_transform_scale(watermark,
                                            landscape ? 620 : 800, 0);
          lv_obj_set_style_opa(watermark, LV_OPA_40, 0);
          lv_obj_align(watermark, LV_ALIGN_CENTER, 0, 0);
          auto *waiting = design::Label(
              preview, "Preview appears after the app has been opened",
              &lv_font_montserrat_24, design::kMutedStrong);
          lv_obj_align(waiting, LV_ALIGN_CENTER, 0, landscape ? 130 : 190);
        }

        auto *footer = lv_obj_create(card);
        design::Panel(footer, landscape ? 34 : 42, design::kCanvas);
        lv_obj_remove_flag(footer, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(footer, card_width - (landscape ? 40 : 48),
                        landscape ? 126 : 174);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0,
                     landscape ? -20 : -24);
        lv_obj_set_style_bg_color(footer, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(footer, LV_OPA_60, 0);
        auto *plate = design::IconPlate(
            footer, app.icon.c_str(), design::kAccent, design::kAccentSoft,
            landscape ? 82 : 104);
        lv_obj_set_pos(plate, landscape ? 20 : 24,
                       landscape ? 22 : 34);
        auto *name = design::Label(footer, app.title.c_str(),
                                   &lv_font_montserrat_36, design::kText);
        lv_obj_set_pos(name, landscape ? 124 : 154,
                       landscape ? 18 : 30);
        lv_obj_set_width(name, card_width - (landscape ? 330 : 390));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        auto *summary = design::Label(footer, app.summary.c_str(),
                                      &lv_font_montserrat_24,
                                      design::kMutedStrong);
        lv_obj_set_pos(summary, landscape ? 124 : 154,
                       landscape ? 70 : 88);
        lv_obj_set_width(summary, card_width - (landscape ? 330 : 390));
        lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);
        const bool is_current = has_current_app &&
            app.action == current_app.action &&
            app.plugin_id == current_app.plugin_id;
        auto *status = design::Label(
            footer, is_current ? "CURRENT" : "OPEN",
            &lv_font_montserrat_18, design::kAccent);
        lv_obj_set_style_text_letter_space(status, 2, 0);
        lv_obj_align(status, LV_ALIGN_RIGHT_MID,
                     landscape ? -24 : -30, 0);

        if (!gesture_transition) {
          lv_obj_set_style_translate_y(card, landscape ? 120 : 230, 0);
          lv_anim_t enter;
          lv_anim_init(&enter);
          lv_anim_set_var(&enter, card);
          lv_anim_set_values(&enter, landscape ? 120 : 230, 0);
          lv_anim_set_duration(&enter, 360);
          lv_anim_set_delay(&enter, 30 + static_cast<uint32_t>(index) * 38);
          lv_anim_set_path_cb(&enter, lv_anim_path_overshoot);
          lv_anim_set_exec_cb(&enter, [](void *target, int32_t value) {
            lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(target),
                                         value, 0);
          });
          lv_anim_start(&enter);
        }
      }
    }
    if (!gesture_transition) lv_obj_fade_in(overlay, 170, 0);
  }

  // The bottom strip in which a contact may start the Recents swipe.
  int32_t RecentsStripHeight() const {
    return std::max(64, (landscape_ ? width_ : height_) / 44);
  }

  void BeginRecentsSwipe(int32_t x, int32_t y) {
    CancelRecentsSwipe();
    recents_swipe_active_ = true;
    recents_swipe_start_x_ = x;
    recents_swipe_last_x_ = x;
    recents_swipe_start_y_ = y;
    recents_swipe_last_y_ = y;
    recents_swipe_max_up_ = 0;
    recents_last_drag_update_ms_ = 0;
    recents_transition_finish_open_ = false;
    recents_transition_finish_home_ = false;
    recents_swipe_hold_committed_ = false;
    recents_swipe_hold_started_ms_ = 0;
    recents_swipe_hold_anchor_y_ = y;

    RecentApp identity;
    if (IsRecentAppAction(current_scene_)) {
      CaptureRecentPreview(current_scene_);
      identity = DescribeRecentApp(current_scene_);
      const auto entry = std::find_if(
          recent_apps_.begin(), recent_apps_.end(), [&](const RecentApp &item) {
            return item.action == identity.action &&
                item.plugin_id == identity.plugin_id;
          });
      if (entry != recent_apps_.end()) recents_drag_preview_ = entry->preview;
    } else {
      identity.title = "AERA Recovery";
      identity.summary = on_home_ ? "Home" : "Recovery workspace";
      identity.icon = LV_SYMBOL_HOME;
      recents_drag_preview_ = CaptureScreenPreview();
    }
    if (recents_drag_preview_ == nullptr) return;

    PrepareRecentsTargetGeometry();
    recents_transition_active_ = true;
    const int screen_width = landscape_ ? height_ : width_;
    const int screen_height = landscape_ ? width_ : height_;
    recents_drag_layer_ = lv_obj_create(lv_layer_top());
    design::Clear(recents_drag_layer_);
    lv_obj_set_size(recents_drag_layer_, screen_width, screen_height);
    lv_obj_set_style_bg_color(recents_drag_layer_, design::kCanvas, 0);
    lv_obj_set_style_bg_opa(recents_drag_layer_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(recents_drag_layer_, LV_OBJ_FLAG_CLICKABLE);

    recents_drag_card_ = lv_obj_create(recents_drag_layer_);
    design::Clear(recents_drag_card_);
    lv_obj_set_pos(recents_drag_card_, 0, 0);
    lv_obj_set_size(recents_drag_card_, screen_width, screen_height);
    lv_obj_set_style_bg_opa(recents_drag_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(recents_drag_card_, 2, 0);
    lv_obj_set_style_border_color(recents_drag_card_, design::kAccent, 0);
    lv_obj_set_style_border_opa(recents_drag_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_post(recents_drag_card_, true, 0);
    lv_obj_remove_flag(recents_drag_card_, LV_OBJ_FLAG_CLICKABLE);

    recents_drag_image_ = lv_image_create(recents_drag_card_);
    lv_image_set_src(recents_drag_image_, &recents_drag_preview_->descriptor);
    lv_image_set_antialias(recents_drag_image_, false);
    const uint32_t image_scale = std::max(
        (static_cast<uint32_t>(screen_width) * 256 +
         recents_drag_preview_->pixels->width - 1) /
            recents_drag_preview_->pixels->width,
        (static_cast<uint32_t>(screen_height) * 256 +
         recents_drag_preview_->pixels->height - 1) /
            recents_drag_preview_->pixels->height);
    lv_image_set_scale(recents_drag_image_, image_scale);
    lv_obj_center(recents_drag_image_);
    lv_obj_remove_flag(recents_drag_image_, LV_OBJ_FLAG_CLICKABLE);

    recents_drag_footer_ = lv_obj_create(recents_drag_card_);
    design::Panel(recents_drag_footer_, landscape_ ? 34 : 42,
                  design::kCanvas);
    lv_obj_remove_flag(recents_drag_footer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(recents_drag_footer_,
                    recents_target_width_ - (landscape_ ? 40 : 48),
                    landscape_ ? 126 : 174);
    lv_obj_align(recents_drag_footer_, LV_ALIGN_BOTTOM_MID, 0,
                 landscape_ ? -20 : -24);
    lv_obj_set_style_bg_color(recents_drag_footer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(recents_drag_footer_, LV_OPA_60, 0);
    lv_obj_set_style_opa(recents_drag_footer_, LV_OPA_TRANSP, 0);
    auto *plate = design::IconPlate(
        recents_drag_footer_, identity.icon.c_str(), design::kAccent,
        design::kAccentSoft, landscape_ ? 82 : 104);
    lv_obj_set_pos(plate, landscape_ ? 20 : 24, landscape_ ? 22 : 34);
    auto *name = design::Label(recents_drag_footer_, identity.title.c_str(),
                               &lv_font_montserrat_36, design::kText);
    lv_obj_set_pos(name, landscape_ ? 124 : 154, landscape_ ? 18 : 30);
    auto *summary = design::Label(recents_drag_footer_,
                                  identity.summary.c_str(),
                                  &lv_font_montserrat_24,
                                  design::kMutedStrong);
    lv_obj_set_pos(summary, landscape_ ? 124 : 154,
                   landscape_ ? 70 : 88);
    auto *status = design::Label(recents_drag_footer_, "CURRENT",
                                 &lv_font_montserrat_18, design::kAccent);
    lv_obj_set_style_text_letter_space(status, 2, 0);
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, landscape_ ? -24 : -30, 0);
    recents_transition_progress_ = 0;
  }

  void PollRecentsHold() {
    if (!recents_swipe_active_ || recents_swipe_hold_committed_ ||
        recents_swipe_hold_started_ms_ == 0)
      return;
    if (operation_running_ || nas_running_ || plugin_running_ ||
        update_installing_ ||
        (update_running_ && update_task_ == UpdateTask::kDownload)) {
      CancelRecentsSwipe();
      return;
    }
    constexpr uint32_t kHoldDelayMs = 220;
    if (MonotonicMilliseconds() - recents_swipe_hold_started_ms_ <
        kHoldDelayMs)
      return;

    recents_swipe_hold_committed_ = true;
    recents_transition_finish_open_ = true;
    RecoveryVibrate(Haptic::kTouch);
    if (recents_overlay_ == nullptr) {
      ShowRecents(true);
      if (recents_overlay_ == nullptr) {
        recents_swipe_hold_committed_ = false;
        recents_transition_finish_open_ = false;
        return;
      }
      if (recents_drag_layer_ != nullptr)
        lv_obj_move_foreground(recents_drag_layer_);
    }

    const size_t first_deferred = IsRecentAppAction(current_scene_) ? 1 : 0;
    for (size_t i = first_deferred; i < recents_deferred_cards_.size(); ++i) {
      auto *card = recents_deferred_cards_[i];
      lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_opa(card, LV_OPA_TRANSP, 0);
      lv_obj_fade_in(card, 160,
                     static_cast<uint32_t>((i - first_deferred) * 24));
    }
    lv_obj_set_style_opa(recents_overlay_, LV_OPA_COVER, 0);
    LayoutRecentsTransition(256);
  }

  void LayoutRecentsTransition(int32_t progress) {
    if (recents_drag_card_ == nullptr || recents_drag_preview_ == nullptr)
      return;
    recents_transition_progress_ = std::clamp(progress, 0, 256);
    const int32_t eased = recents_transition_progress_ *
        recents_transition_progress_ *
        (768 - 2 * recents_transition_progress_) / (256 * 256);
    const int32_t screen_width = landscape_ ? height_ : width_;
    const int32_t visible_height = landscape_ ? width_ : height_;
    const int32_t window_width = screen_width +
        (recents_target_width_ - screen_width) * eased / 256;
    const int32_t window_height = visible_height +
        (recents_target_height_ - visible_height) * eased / 256;
    const int32_t window_x = recents_target_x_ * eased / 256;
    const int32_t window_y = recents_target_y_ * eased / 256;
    lv_obj_set_size(recents_drag_card_, window_width, window_height);
    lv_obj_set_pos(recents_drag_card_, window_x, window_y);
    lv_obj_set_style_radius(recents_drag_card_, eased * 58 / 256, 0);
    lv_obj_set_style_border_opa(
        recents_drag_card_, static_cast<lv_opa_t>(eased * 153 / 256), 0);
    const uint32_t image_scale = std::max(
        (static_cast<uint32_t>(window_width) * 256 +
         recents_drag_preview_->pixels->width - 1) /
            recents_drag_preview_->pixels->width,
        (static_cast<uint32_t>(window_height) * 256 +
         recents_drag_preview_->pixels->height - 1) /
            recents_drag_preview_->pixels->height);
    lv_image_set_scale(recents_drag_image_, image_scale);
    lv_obj_center(recents_drag_image_);
    if (recents_overlay_ != nullptr) {
      lv_obj_set_style_opa(recents_overlay_,
                           static_cast<lv_opa_t>(eased * 255 / 256), 0);
      lv_obj_set_style_bg_opa(recents_drag_layer_, LV_OPA_TRANSP, 0);
    } else {
      lv_obj_set_style_bg_opa(
          recents_drag_layer_,
          static_cast<lv_opa_t>(eased * LV_OPA_60 / 256), 0);
    }
    if (recents_drag_footer_ != nullptr) {
      const int footer_opacity = std::clamp((eased - 112) * 2, 0, 255);
      lv_obj_set_style_opa(recents_drag_footer_,
                           static_cast<lv_opa_t>(footer_opacity), 0);
    }
    lv_obj_invalidate(recents_drag_layer_);
  }

  static void SetRecentsTransitionProgress(void *target, int32_t value) {
    auto *self = static_cast<Impl *>(target);
    if (self != nullptr) self->LayoutRecentsTransition(value);
  }

  static void CompleteRecentsTransition(lv_anim_t *animation) {
    auto *self = static_cast<Impl *>(lv_anim_get_user_data(animation));
    if (self == nullptr) return;
    const bool finish_home = self->recents_transition_finish_home_;
    self->recents_transition_finish_home_ = false;
    if (finish_home) {
      if (self->recents_overlay_ != nullptr) {
        auto *overlay = self->recents_overlay_;
        self->recents_overlay_ = nullptr;
        lv_obj_delete(overlay);
      }
      self->recents_all_cards_.clear();
      self->recents_deferred_cards_.clear();
      self->recents_transition_active_ = false;
      self->DestroyRecentsDragWindow();
      self->recents_swipe_hold_committed_ = false;
      self->recents_swipe_hold_started_ms_ = 0;
      // BeginRecentsSwipe already captured the outgoing app for this gesture.
      // Avoid doing the same framebuffer conversion again while landing home.
      if (!self->on_home_) self->ShowHome(false);
      return;
    }
    if (!self->recents_transition_finish_open_ &&
        self->recents_overlay_ != nullptr) {
      auto *overlay = self->recents_overlay_;
      self->recents_overlay_ = nullptr;
      lv_obj_delete(overlay);
    } else if (self->recents_overlay_ != nullptr) {
      for (size_t i = 0; i < self->recents_deferred_cards_.size(); ++i) {
        auto *card = self->recents_deferred_cards_[i];
        lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
        if (i > 0) {
          lv_obj_set_style_opa(card, LV_OPA_TRANSP, 0);
          lv_obj_fade_in(card, 180, static_cast<uint32_t>((i - 1) * 30));
        }
      }
      lv_obj_update_layout(self->recents_overlay_);
      self->recents_deferred_cards_.clear();
      lv_obj_set_style_opa(self->recents_overlay_, LV_OPA_COVER, 0);
    }
    self->recents_deferred_cards_.clear();
    if (!self->recents_transition_finish_open_)
      self->recents_all_cards_.clear();
    self->recents_transition_active_ = false;
    self->recents_transition_finish_open_ = false;
    self->recents_swipe_hold_committed_ = false;
    self->recents_swipe_hold_started_ms_ = 0;
    self->DestroyRecentsDragWindow();
  }

  void UpdateRecentsSwipe(int32_t x, int32_t y) {
    if (!recents_swipe_active_) return;
    recents_swipe_last_x_ = x;
    recents_swipe_last_y_ = y;
    const int32_t upward = std::max(0, recents_swipe_start_y_ - y);
    recents_swipe_max_up_ = std::max(recents_swipe_max_up_, upward);
    const int32_t visible_height = landscape_ ? width_ : height_;
    const int32_t horizontal = std::abs(x - recents_swipe_start_x_);
    const int32_t hold_distance = std::max(72, visible_height / 14);
    const int32_t hold_slop = std::max(12, visible_height / 90);
    if (!recents_swipe_hold_committed_ && upward >= hold_distance &&
        upward > horizontal * 2) {
      if (recents_swipe_hold_started_ms_ == 0 ||
          std::abs(y - recents_swipe_hold_anchor_y_) > hold_slop) {
        recents_swipe_hold_started_ms_ = MonotonicMilliseconds();
        recents_swipe_hold_anchor_y_ = y;
      }
    } else if (!recents_swipe_hold_committed_) {
      recents_swipe_hold_started_ms_ = 0;
      recents_swipe_hold_anchor_y_ = y;
    }
    const int32_t range = std::max(1, visible_height * 2 / 7);
    const int32_t progress = std::min(256, upward * 256 / range);
    const uint32_t now = MonotonicMilliseconds();
    if (progress < 256 && recents_last_drag_update_ms_ != 0 &&
        now - recents_last_drag_update_ms_ < 16)
      return;
    recents_last_drag_update_ms_ = now;
    LayoutRecentsTransition(progress);
  }

  void FinishRecentsSwipe() {
    if (!recents_swipe_active_) return;
    const int32_t visible_height = landscape_ ? width_ : height_;
    const int32_t horizontal =
        std::abs(recents_swipe_last_x_ - recents_swipe_start_x_);
    const bool accepted = recents_swipe_max_up_ >= visible_height / 11 &&
        recents_swipe_max_up_ > horizontal &&
        recents_swipe_last_y_ < recents_swipe_start_y_;
    const bool open_recents = accepted && recents_swipe_hold_committed_;
    const bool go_home = accepted && !open_recents;
    recents_swipe_active_ = false;
    recents_swipe_max_up_ = 0;
    recents_swipe_hold_started_ms_ = 0;
    recents_swipe_hold_anchor_y_ = 0;
    lv_anim_delete(this, SetRecentsTransitionProgress);
    recents_transition_finish_open_ = open_recents;
    recents_transition_finish_home_ = go_home;

    if (recents_drag_card_ == nullptr) {
      recents_swipe_hold_committed_ = false;
      recents_transition_active_ = false;
      if (open_recents && recents_overlay_ == nullptr) ShowRecents(false);
      else if (go_home && !on_home_) ShowHome();
      return;
    }

    lv_anim_t settle;
    lv_anim_init(&settle);
    lv_anim_set_var(&settle, this);
    const int32_t settle_target = open_recents
        ? 256
        : go_home ? std::max(recents_transition_progress_, 112) : 0;
    lv_anim_set_values(&settle, recents_transition_progress_, settle_target);
    lv_anim_set_duration(&settle,
        open_recents
            ? std::clamp(90 + (256 - recents_transition_progress_) * 2 / 3,
                         90, 260)
            : go_home
                ? std::clamp(105 +
                                 std::abs(settle_target -
                                          recents_transition_progress_) / 2,
                             105, 165)
                : std::clamp(120 + recents_transition_progress_ / 2,
                             140, 230));
    lv_anim_set_path_cb(&settle, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&settle, SetRecentsTransitionProgress);
    lv_anim_set_user_data(&settle, this);
    lv_anim_set_completed_cb(&settle, CompleteRecentsTransition);
    lv_anim_start(&settle);
  }

  void DestroyRecentsDragWindow() {
    if (recents_drag_layer_ != nullptr) lv_obj_delete(recents_drag_layer_);
    recents_drag_layer_ = nullptr;
    recents_drag_card_ = nullptr;
    recents_drag_image_ = nullptr;
    recents_drag_footer_ = nullptr;
    recents_drag_preview_.reset();
  }

  void CancelRecentsSwipe() {
    recents_pending_ = false;
    recents_swipe_active_ = false;
    recents_swipe_max_up_ = 0;
    recents_swipe_hold_started_ms_ = 0;
    recents_swipe_hold_anchor_y_ = 0;
    recents_swipe_hold_committed_ = false;
    recents_transition_finish_home_ = false;
    if (recent_launch_active_) return;
    lv_anim_delete(this, SetRecentsTransitionProgress);
    DestroyRecentsDragWindow();
    if (recents_transition_active_ && recents_overlay_ != nullptr) {
      auto *overlay = recents_overlay_;
      recents_overlay_ = nullptr;
      lv_obj_delete(overlay);
    }
    recents_deferred_cards_.clear();
    if (recents_overlay_ == nullptr) recents_all_cards_.clear();
    recents_transition_active_ = false;
  }

  void ShowFastboot(bool mode_transition = false) {
    current_tool_ = Action::kNone;
    on_home_ = false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    BuildFastbootScene(screen, HandleSceneAction, this);
    lv_screen_load_anim(screen,
        mode_transition ? LV_SCR_LOAD_ANIM_FADE_IN
                        : LV_SCR_LOAD_ANIM_FADE_ON,
        mode_transition ? 260 : 120, 0, true);
  }

  void DismissPowerMenu() {
    if (power_overlay_ == nullptr) return;
    auto *overlay = power_overlay_;
    power_overlay_ = nullptr;
    lv_obj_delete_async(overlay);
  }

  static void HandleSceneAction(Action action, void *context) {
    auto *self = static_cast<Impl *>(context);
    if (self == nullptr)
      return;

    if (action == Action::kBootComplete) {
      self->boot_animation_complete_ = true;
      return;
    }

    if (action == Action::kLockScreenshot) {
      if (self->lock_overlay_) self->screenshot_requested_ = true;
      return;
    }
    if (action == Action::kUnlock) {
      if (self->lock_overlay_ != nullptr) {
        lv_obj_t *lock = self->lock_overlay_;
        self->lock_overlay_ = nullptr;
        // The lock scene completes its gesture-specific exit before notifying us.
        lv_obj_delete_async(lock);
      }
      return;
    }

    if (self->decryption_active_) {
      if (action == Action::kDecryptSubmit) {
        self->StartDecryption();
      } else if (action == Action::kDecryptSkip && !self->decrypt_running_) {
        self->decryption_active_ = false;
        if (self->secondary_decryption_) {
          self->secondary_decryption_ = false;
          HandleSceneAction(Action::kUsers, self);
        } else {
          self->decryption_completion_ = DecryptionCompletion::kSkipped;
          self->ShowPreparing(false);
        }
      }
      return;
    }

    if (!self->backend_ready_)
      return;

    if (action == Action::kCancelSideload && self->operation_running_ &&
        self->operation_scene_.job == Job::kSideload) {
      RecoveryCancelSideload();
      RefreshOperationScene(self->operation_scene_);
      return;
    }

    if (action == Action::kCancelPcBackup && self->operation_running_ &&
        self->operation_scene_.job == Job::kPcBackupOperation) {
      const auto pending = aera::pc::GetBackupCancellation();
      if (!pending.active || pending.requested || self->PcBlockedByModal()) return;
      auto *screen = lv_screen_active();
      if (pending.restore) {
        widgets::Sheet(screen, "Cancel restore",
            "Stopping a restore may leave incomplete data. Complete a full restore before booting Android.",
            [screen, id = pending.id] {
              const auto current = aera::pc::GetBackupCancellation();
              if (!current.active || current.id != id || current.requested) return;
              widgets::Sheet(screen, "Confirm restore cancellation",
                  "Stopping a restore may leave incomplete data. Complete a full restore before booting Android.",
                  [id] { aera::pc::CancelBackup(id, true); });
            });
      } else {
        widgets::Sheet(screen, "Cancel backup",
            "An unfinished backup cannot be restored.",
            [id = pending.id] { aera::pc::CancelBackup(id, false); });
      }
      return;
    }

    if (action == Action::kBack) {
      // UI back controls already provide their touch haptic. Hardware Back
      // and edge gestures call NavigateBack() directly with feedback enabled.
      self->NavigateBack(false);
      return;
    }

    if (self->operation_running_ || self->nas_running_ || self->plugin_running_ ||
        (self->update_running_ &&
         self->update_task_ == UpdateTask::kDownload)) return;
    self->CancelEdgeSwipe();
    self->CancelRecentsSwipe();

    if (IsRecentAppAction(action)) self->RememberRecentApp(action);

    if (self->fastboot_mode_) {
      if (action == Action::kFormatData) {
        self->current_tool_ = Action::kFormatData;
        lv_obj_t *screen = lv_obj_create(nullptr);
        BuildFastbootFormatScene(screen, HandleSceneAction, self);
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
        return;
      }
      if (action == Action::kRunOperation) {
        self->StartJob(GetJobRequest());
        return;
      }
      if (action == Action::kOpenReboot || action == Action::kBackHome ||
          action == Action::kWipe) {
        self->ShowFastboot();
        return;
      }
      if (action != Action::kRebootSystem &&
          action != Action::kRebootRecovery &&
          action != Action::kRebootBootloader &&
          action != Action::kPowerOff) return;
    }

    if (action == Action::kQuickWifiToggle) {
      WifiRequest request;
      request.operation = RecoveryWifiStatus().enabled
                              ? WifiOperation::kDisable
                              : WifiOperation::kEnable;
      self->StartWifi(request, false);
      return;
    }

    if (action == Action::kUpdates) {
      self->ShowUpdates();
      return;
    }

    if (action == Action::kCheckUpdates) {
      if (self->current_scene_ != Action::kUpdates)
        self->ShowUpdates();
      self->StartUpdateCheck(true);
      return;
    }

    if (action == Action::kDownloadUpdate) {
      self->StartUpdateDownload();
      return;
    }

    if (action == Action::kToggleRotation ||
        action == Action::kToggleVideoRotation) {
      self->DismissRecents();
      self->landscape_ = !self->landscape_;
      self->pointer_.pressed = false;
      if (self->pointer_device_ != nullptr)
        lv_indev_reset(self->pointer_device_, nullptr);
      lv_display_set_rotation(
          self->display_, self->landscape_ ? LV_DISPLAY_ROTATION_90
                                          : LV_DISPLAY_ROTATION_0);
      __android_log_print(ANDROID_LOG_INFO, kLogTag,
                          "display orientation changed to %s",
                          self->landscape_ ? "landscape" : "portrait");
      if (action == Action::kToggleRotation)
        self->ShowHome();
      return;
    }

    if (action == Action::kToggleRecording) {
      if (recorder::Active()) recorder::Stop();
      else recorder::Start(static_cast<uint32_t>(self->physical_width_),
                           static_cast<uint32_t>(self->physical_height_));
      return;
    }

    if (action == Action::kRunOperation) {
      self->StartJob(GetJobRequest());
      return;
    }

    if (action == Action::kRunWifiOperation) {
      self->StartWifi(GetWifiRequest(), true);
      return;
    }

    if (action == Action::kRunNasOperation) {
      self->StartNas(GetNasRequest());
      return;
    }

    if (action == Action::kRunPluginOperation) {
      self->StartPlugin(GetPluginRequest());
      return;
    }

    if (action == Action::kApplyPluginAutoUpdates) {
      self->BeginAutomaticPluginUpdates();
      return;
    }

    if (action == Action::kDecryptUser) {
      const auto request = GetUserDecryptRequest();
      if (request.user.decrypted) {
        HandleSceneAction(Action::kUsers, self);
        return;
      }
      self->credential_type_ = request.user.credential_type;
      self->crypto_user_id_ = request.user.id;
      self->secondary_decryption_ = true;
      self->decryption_active_ = true;
      self->on_home_ = false;
      self->current_tool_ = Action::kUsers;
      lv_obj_t *screen = lv_obj_create(nullptr);
      self->decrypt_scene_ = BuildDecryptScene(
          screen, request.user.credential_type, true, request.user.id, 3,
          HandleSceneAction, self, request.user.name);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      if (request.user.credential_type == 0)
        self->StartDecryption("!");
      return;
    }

    if (action == Action::kBrowsePackages) {
      self->TrackScene(Action::kFiles);
      self->on_home_ = false;
      self->current_tool_ = Action::kFiles;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildFilesScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
      return;
    }

    if (action == Action::kSideload) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildSideloadScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kStartSideload) {
      JobRequest request;
      request.job = Job::kSideload;
      request.title = "ADB Sideload";
      request.present_before_run = true;
      self->StartJob(request);
      return;
    }

    if (action == Action::kFiles) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = Action::kFiles;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildFilesScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kPlugins) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = Action::kPlugins;
      lv_obj_t *screen = lv_obj_create(nullptr);
      self->plugin_scene_ = BuildPluginScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kInstallLocalPlugin) {
      self->TrackScene(Action::kPlugins);
      self->on_home_ = false;
      self->current_tool_ = Action::kPlugins;
      lv_obj_t *screen = lv_obj_create(nullptr);
      self->plugin_scene_ = BuildPluginScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      self->StartPlugin(GetPluginRequest());
      return;
    }

    if (action == Action::kNas) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      self->nas_scene_ = BuildNasScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kInstallPackage) {
      self->StartPackageInstall(GetSelectedPackagePath());
      return;
    }

    if (action == Action::kWeb) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildWebScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kRetroArch) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildRetroArchScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kDoom) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildDoomScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kTelegram) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildTelegramScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kGallery) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildGalleryScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kMedia) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildMediaScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kStreams) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildStreamsScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kRecorder) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildRecorderScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kAppVault) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildAppVaultScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kPluginApp) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildGenericPluginScene(screen, GetSelectedPluginId(), HandleSceneAction,
                              self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kRootManager) {
      if (!root::Enabled()) return;
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildRootManagerScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kAbout) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildAboutScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    if (action == Action::kTerminal) {
      self->TrackScene(action);
      self->on_home_ = false;
      self->current_tool_ = action;
      lv_obj_t *screen = lv_obj_create(nullptr);
      BuildTerminalScene(screen, HandleSceneAction, self);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      return;
    }

    const bool opens_tool =
        action == Action::kBackup || action == Action::kRestore ||
        action == Action::kWipe || action == Action::kFormatData ||
        action == Action::kSettings ||
        action == Action::kMounts || action == Action::kLogs ||
        action == Action::kPreferences || action == Action::kLanguage ||
        action == Action::kStatusBar || action == Action::kTemperatures ||
        action == Action::kTheme ||
        action == Action::kWifi || action == Action::kUsers ||
        action == Action::kPcConnection;
    if (action == Action::kOpenReboot || action == Action::kBackHome ||
        action == Action::kInstall || opens_tool) {
      self->TrackScene(action == Action::kInstall ? Action::kBackHome : action);
      lv_obj_t *screen = lv_obj_create(nullptr);
      if (action == Action::kOpenReboot) {
        self->current_tool_ = Action::kNone;
        self->on_home_ = false;
        BuildRebootScene(screen, HandleSceneAction, self);
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      } else if (opens_tool) {
        self->on_home_ = false;
        self->current_tool_ = action;
        if (action == Action::kWifi)
          self->wifi_scene_ = BuildWifiScene(screen, HandleSceneAction, self);
        else
          BuildToolScene(screen, action, HandleSceneAction, self);
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      } else {
        self->on_home_ = true;
        self->current_tool_ = Action::kNone;
        BuildHomeScene(screen, HandleSceneAction, self);
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
      }
      return;
    }

    self->pending_action_ = action;
  }

  void ShowHome(bool capture_current = true, bool mode_transition = false) {
    if (capture_current && IsRecentAppAction(current_scene_))
      CaptureRecentPreview(current_scene_);
    CancelEdgeSwipe();
    CancelRecentsSwipe();
    DismissRecents();
    navigation_history_.clear();
    current_scene_ = Action::kBackHome;
    current_tool_ = Action::kNone;
    on_home_ = true;
    lv_obj_t *screen = lv_obj_create(nullptr);
    BuildHomeScene(screen, HandleSceneAction, this);
    lv_screen_load_anim(screen,
        mode_transition ? LV_SCR_LOAD_ANIM_FADE_IN
                        : LV_SCR_LOAD_ANIM_FADE_ON,
        mode_transition ? 260 : 140, 0, true);
  }

  void ShowUpdates() {
    TrackScene(Action::kUpdates);
    on_home_ = false;
    current_tool_ = Action::kUpdates;
    lv_obj_t *screen = lv_obj_create(nullptr);
    update_scene_ = BuildUpdateScene(screen, HandleSceneAction, this);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, true);
    lv_obj_update_layout(screen);
    RefreshUpdateScene(update_scene_);
  }

  void TrackScene(Action target) {
    if (target == Action::kNone) return;
    if (current_scene_ != target && IsRecentAppAction(current_scene_) &&
        recents_overlay_ == nullptr)
      CaptureRecentPreview(current_scene_);
    if (target == Action::kBackHome) {
      if (!navigating_back_) navigation_history_.clear();
      current_scene_ = target;
      return;
    }
    if (!navigating_back_ && current_scene_ != Action::kNone &&
        current_scene_ != target) {
      if (navigation_history_.empty() ||
          navigation_history_.back() != current_scene_)
        navigation_history_.push_back(current_scene_);
      if (navigation_history_.size() > 32)
        navigation_history_.erase(navigation_history_.begin());
    }
    current_scene_ = target;
    if (IsRecentAppAction(target)) RememberRecentApp(target);
  }

  void ShowPreparing(bool decrypted) {
    CancelEdgeSwipe();
    CancelRecentsSwipe();
    DismissRecents();
    on_home_ = false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    BuildPreparingScene(screen, decrypted);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, true);
  }

  void StartDecryption(const std::string &supplied_credential = {}) {
    if (!decryption_active_ || decrypt_running_) return;
    const std::string credential = supplied_credential.empty()
                                       ? GetDecryptCredential(decrypt_scene_)
                                       : supplied_credential;
    const size_t minimum = credential_type_ == 2 ? 4U : 1U;
    if (credential.size() < minimum) {
      CompleteDecryptAttempt(decrypt_scene_, false);
      return;
    }

    SetDecryptBusy(decrypt_scene_);
    decrypt_running_ = true;
    decrypt_result_.store(-1, std::memory_order_release);
    decrypt_complete_.store(false, std::memory_order_release);
    decrypt_thread_ = std::thread([this, credential]() {
      operation_affinity::RestoreForWorker();
      const int result =
          aeraui_decrypt_data(credential.c_str(), crypto_user_id_);
      decrypt_result_.store(result, std::memory_order_release);
      decrypt_complete_.store(true, std::memory_order_release);
    });
  }

  void StartPackageInstall(const char *path) {
    if (operation_running_ || nas_running_ || path == nullptr || path[0] == '\0')
      return;

    JobRequest request;
    request.job = Job::kInstall;
    request.path = path;
    request.title = "Install ZIP";
    StartJob(request);
  }

  bool PcBlockedByModal() const {
    auto *screen = lv_screen_active();
    if (recents_overlay_) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
      auto *child = lv_obj_get_child(screen, i);
      const auto marker = lv_obj_get_user_data(child);
      if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) &&
          (marker == &widgets::kModalMarker || marker == &widgets::kPersistentModalMarker)) return true;
    }
    return false;
  }

  void PollPcConnection() {
    const uint32_t now = MonotonicMilliseconds();
    if (now - last_pc_poll_ < 250) return;
    last_pc_poll_ = now;
    aera::pc::RefreshNetwork();
    auto status = aera::pc::GetStatus();
    pc_transfer_.Refresh(aera::pc::GetUploadProgress(),
        interactive_ready_ && !decryption_active_ && !decrypt_running_ &&
        !operation_running_ && !fastboot_mode_ && !StatusBarShadeOpen() &&
        !pc_overlay_ && !lock_overlay_ && !power_overlay_ && !recents_overlay_);
    if (!status.enabled && !status.manually_disabled && interactive_ready_ &&
        !decryption_active_ && !decrypt_running_ && !wifi_running_ && !fastboot_mode_ &&
        (last_pc_start_attempt_ == 0 || now - last_pc_start_attempt_ >= 5000)) {
      if (RecoveryPcAutoEnable()) {
        last_pc_start_attempt_ = now;
        aera::pc::SetEnabled(true, 443, false);
        status = aera::pc::GetStatus();
      }
    }
    if (pc_overlay_ && status.pending_connection != pc_connection_id_) {
      lv_obj_delete(pc_overlay_);
      pc_overlay_ = nullptr; pc_connection_id_.clear();
    }
    if (!pc_job_id_.empty() && operation_running_) {
      aera::pc::UpdateInstallerPresentation(RecoveryInstallerPresentation());
      aera::pc::UpdateInstall(RecoveryProgress(), RecoveryOperationDetail(), widgets::ReadLog());
      const auto prompt = RecoveryInstallerPrompt();
      aera::pc::UpdateInstallerPrompt(prompt);
      std::string id; bool accepted = false;
      if (aera::pc::TakePromptAnswer(&id, &accepted) && prompt.active && prompt.id == id)
        RecoveryAnswerInstallerPrompt(accepted);
    }
    bool pc_busy = !interactive_ready_ || decryption_active_ || decrypt_running_ ||
        operation_running_ || wifi_running_ || nas_running_ || plugin_running_ ||
        update_running_ || fastboot_mode_ || power_overlay_ ||
        current_scene_ == Action::kRootManager || current_scene_ == Action::kAppVault;
#ifndef TW_OEM_BUILD
    pc_busy = pc_busy || aera::rpc::Dispatcher::Active();
#endif
    aera::pc::SetRecoveryBusy(pc_busy);
    if (pc_busy) return;
    if (last_pc_cleanup_ == 0 || now - last_pc_cleanup_ >= 5000) {
      last_pc_cleanup_ = now;
      aera::pc::CleanupOldTransfers();
    }
    if (status.enabled && now - last_pc_platform_refresh_ >= 2000) {
      last_pc_platform_refresh_ = now;
      aera::pc::RestoreHttpRedirect();
      aera::pc::RefreshPlatformInfo();
    }
    aera::pc::ConnectionRequest connection;
    if (!pc_overlay_ && !lock_overlay_ && !PcBlockedByModal() &&
        aera::pc::TakeConnectionRequest(&connection)) {
      ShowPcApproval(connection);
      return;
    }
    aera::pc::InstallRequest install;
    std::string reboot;
    if (!pc_overlay_ && !PcBlockedByModal() && aera::pc::TakeRebootRequest(&reboot)) {
      const Action action = reboot == "system" ? Action::kRebootSystem :
          reboot == "recovery" ? Action::kRebootRecovery :
          reboot == "bootloader" ? Action::kRebootBootloader :
          reboot == "fastbootd" ? Action::kRebootFastbootd : Action::kPowerOff;
      HandleSceneAction(action, this);
      return;
    }
    if (!pc_overlay_ && !PcBlockedByModal() &&
        aera::pc::TakeInstallRequest(&install) && aera::pc::BeginInstall(install.id)) {
      // A browser-confirmed job replaces the idle swipe lock, never decryption.
      if (lock_overlay_ && install.job.show_on_device) {
        auto *lock = lock_overlay_; lock_overlay_ = nullptr;
        lv_obj_delete(lock);
      }
      pc_job_id_ = install.id;
      SetJobRequest(install.job);
      HandleSceneAction(Action::kRunOperation, this);
    }
  }

  void ShowPcApproval(const aera::pc::ConnectionRequest &request) {
    using namespace design;
    using namespace widgets;
    auto *screen = lv_screen_active();
    auto *overlay = lv_obj_create(screen);
    Clear(overlay); lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_user_data(overlay, &kModalMarker);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    pc_overlay_ = overlay; pc_connection_id_ = request.id;
    lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
      auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
      if (self->pc_overlay_ == lv_event_get_target_obj(event)) {
        aera::pc::ResolveConnection(self->pc_connection_id_, false, false);
        self->pc_overlay_ = nullptr; self->pc_connection_id_.clear();
      }
    }, LV_EVENT_DELETE, this);
    auto *panel = lv_obj_create(overlay); Panel(panel, 36, kMainSheet);
    lv_obj_set_size(panel, std::min(1280, static_cast<int>(lv_obj_get_width(screen)) - 80), 820);
    lv_obj_center(panel);
    auto *title = Label(panel, "Allow this computer?", &lv_font_montserrat_40, kText);
    lv_obj_set_pos(title, 40, 34); lv_obj_set_width(title, 1160);
    FitLabelToLines(title, 1160, 2, {&lv_font_montserrat_40, &lv_font_montserrat_36});
    auto *name = Label(panel, request.name.c_str(), &lv_font_montserrat_36, kAccent);
    lv_obj_set_pos(name, 40, 158); lv_obj_set_width(name, 1160);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    std::string fingerprint;
    for (size_t i = 0; i < request.fingerprint.size(); i += 4) {
      if (!fingerprint.empty()) fingerprint += ' ';
      fingerprint += request.fingerprint.substr(i, 4);
    }
    auto *key = Label(panel, fingerprint.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(key, 40, 240); lv_obj_set_width(key, 1160);
    auto *description = Label(panel, "Transfer and install ZIPs and images from your computer",
                             &lv_font_montserrat_28, kText);
    lv_obj_set_pos(description, 40, 352);
    FitLabelToLines(description, 1160, 2, {&lv_font_montserrat_28, &lv_font_montserrat_24});
    auto *remember = lv_checkbox_create(panel);
    lv_checkbox_set_text(remember, i18n::Translate("Remember this computer"));
    lv_obj_set_style_text_font(remember, UiFont(&lv_font_montserrat_36), 0);
    lv_obj_set_style_text_color(remember, kText, 0);
    lv_obj_set_pos(remember, 40, 488); lv_obj_set_size(remember, 1160, 124);
    lv_obj_set_style_pad_all(remember, 24, 0);
    lv_obj_set_style_pad_column(remember, 24, 0);
    lv_obj_set_style_bg_opa(remember, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(remember, kMainPanel, 0);
    lv_obj_set_style_radius(remember, 20, 0);
    lv_obj_set_style_border_width(remember, 2, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(remember, kMuted, LV_PART_INDICATOR);
    lv_obj_set_style_radius(remember, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(remember, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(remember, kMainCanvas, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(remember, kAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(remember, kAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(remember, lv_color_black(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(remember, UiFont(&lv_font_montserrat_36), LV_PART_INDICATOR);
    if (!request.remember_available) lv_obj_add_state(remember, LV_STATE_DISABLED);
    auto *deny = Button(panel, "Deny", [request, overlay] {
      aera::pc::ResolveConnection(request.id, false, false); lv_obj_delete_async(overlay);
    });
    lv_obj_set_pos(deny, 40, 658); lv_obj_set_size(deny, 570, 110);
    auto *allow = Button(panel, "Allow", [request, remember, overlay] {
      aera::pc::ResolveConnection(request.id, true, lv_obj_has_state(remember, LV_STATE_CHECKED));
      lv_obj_delete_async(overlay);
    }, true);
    lv_obj_set_pos(allow, 630, 658); lv_obj_set_size(allow, 570, 110);
  }

  void StartJob(const JobRequest &request) {
    if (operation_running_ || wifi_running_ || nas_running_ || plugin_running_)
      return;
    operation_visible_ = request.show_on_device;
    if (operation_visible_) {
      current_tool_ = Action::kNone;
      on_home_ = false;
      lv_obj_t *screen = lv_obj_create(nullptr);
      operation_scene_ = BuildJobScene(screen, request, HandleSceneAction, this);
      lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    }

    operation_running_ = true;
    operation_result_.store(-1, std::memory_order_release);
    operation_complete_.store(false, std::memory_order_release);
    operation_thread_ = std::thread([this, request]() {
      const int affinity_error = operation_affinity::RestoreForWorker();
      cpu_set_t effective{};
      const int allowed = sched_getaffinity(0, sizeof(effective), &effective) == 0
          ? CPU_COUNT(&effective) : -1;
      __android_log_print(affinity_error ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
          kLogTag, "Operation worker affinity: %d CPUs, restore status=%d",
          allowed, affinity_error);
      if (request.present_before_run)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      const int result = RecoveryRunJob(request);
      operation_result_.store(result, std::memory_order_release);
      operation_complete_.store(true, std::memory_order_release);
    });
  }

  void StartWifi(const WifiRequest &request, bool report_to_scene) {
    if (wifi_running_ || operation_running_ || nas_running_) return;
    wifi_scene_report_ = report_to_scene;
    wifi_report_state_ = report_to_scene ? wifi_scene_.state : nullptr;
    if (wifi_scene_report_) SetWifiBusy(wifi_scene_, request);
    wifi_running_ = true;
    wifi_result_.store(-1, std::memory_order_release);
    wifi_complete_.store(false, std::memory_order_release);
    wifi_thread_ = std::thread([this, request]() {
      operation_affinity::RestoreForWorker();
      const int result = RecoveryRunWifi(request);
      wifi_result_.store(result, std::memory_order_release);
      wifi_complete_.store(true, std::memory_order_release);
    });
  }

  void StartNas(const NasRequest &request) {
    if (nas_running_ || operation_running_ || wifi_running_ || plugin_running_)
      return;
    SetNasBusy(nas_scene_, request);
    nas_running_ = true;
    nas_result_.store(-1, std::memory_order_release);
    nas_complete_.store(false, std::memory_order_release);
    nas_thread_ = std::thread([this, request]() {
      operation_affinity::RestoreForWorker();
      const int result = RecoveryRunNas(request);
      nas_result_.store(result, std::memory_order_release);
      nas_complete_.store(true, std::memory_order_release);
    });
  }

  void StartPlugin(const plugins::Request &request,
                   PluginTask task = PluginTask::kInteractive) {
    if (plugin_running_ || operation_running_ || wifi_running_ || nas_running_)
      return;
    if ((task == PluginTask::kInteractive || task == PluginTask::kAutomaticInstall) &&
        current_scene_ == Action::kPlugins)
      SetPluginBusy(plugin_scene_, request);
    plugin_progress_.value.store(0);
    plugin_progress_.downloaded_bytes.store(0);
    plugin_progress_.total_bytes.store(0);
    plugin_progress_.cancel.store(false);
    plugin_progress_.completed_plugins.store(0);
    plugin_progress_.total_plugins.store(0);
    plugin_progress_.status.clear();
    plugin_progress_.error.clear();
    plugin_running_ = true;
    plugin_task_ = task;
    plugin_job_ = request.job;
    plugin_result_.store(-1, std::memory_order_release);
    plugin_complete_.store(false, std::memory_order_release);
    plugin_thread_ = std::thread([this, request]() {
      operation_affinity::RestoreForWorker();
      const bool result = plugins::Run(request, plugin_progress_);
      plugin_result_.store(result ? 0 : -1, std::memory_order_release);
      plugin_complete_.store(true, std::memory_order_release);
    });
  }

  void RefreshPluginSurfaces() {
    if (current_scene_ == Action::kPlugins)
      RefreshPluginScene(plugin_scene_);
    if (on_home_)
      ShowHome();
  }

  void BeginAutomaticPluginUpdates() {
    plugin_auto_queue_.clear();
    if (!RecoveryPreference(Preference::kPluginAutoUpdate)) {
      RefreshPluginSurfaces();
      return;
    }
    for (const auto &update : plugins::AvailableUpdates()) {
      if (update.installed.location == plugins::Location::kStorage)
        plugin_auto_queue_.push_back(update.available.id);
    }
    StartNextAutomaticPluginUpdate();
  }

  void StartNextAutomaticPluginUpdate() {
    if (!RecoveryPreference(Preference::kPluginAutoUpdate))
      plugin_auto_queue_.clear();
    if (plugin_auto_queue_.empty()) {
      RefreshPluginSurfaces();
      return;
    }
    if (plugin_running_ || operation_running_ || wifi_running_ || nas_running_)
      return;
    if (!RecoveryWifiConnection().connected)
      return;
    const std::string id = plugin_auto_queue_.front();
    plugin_auto_queue_.erase(plugin_auto_queue_.begin());
    plugins::Request request;
    request.job = plugins::Job::kInstallStorage;
    request.id = id;
    StartPlugin(request, PluginTask::kAutomaticInstall);
  }

  void StartUpdateCheck(bool manual) {
    if (update_running_) return;
    if (!RecoveryWifiConnection().connected) {
      update::SetOffline();
      if (current_scene_ == Action::kUpdates)
        RefreshUpdateScene(update_scene_);
      return;
    }
    update_running_ = true;
    update_manual_ = manual;
    update_task_ = UpdateTask::kCheck;
    update_result_.store(-1, std::memory_order_release);
    update_complete_.store(false, std::memory_order_release);
    if (current_scene_ == Action::kUpdates)
      RefreshUpdateScene(update_scene_);
    update_thread_ = std::thread([this] {
      operation_affinity::RestoreForWorker();
      const bool success = update::Check();
      update_result_.store(success ? 0 : -1, std::memory_order_release);
      update_complete_.store(true, std::memory_order_release);
    });
  }

  void StartUpdateDownload() {
    if (update_running_ || operation_running_ || wifi_running_ ||
        nas_running_ || plugin_running_) return;
    if (!RecoveryWifiConnection().connected) {
      update::SetOffline();
      if (current_scene_ == Action::kUpdates)
        RefreshUpdateScene(update_scene_);
      return;
    }
    if (!update::PrepareDownload()) {
      RefreshUpdateScene(update_scene_);
      return;
    }
    update_running_ = true;
    update_manual_ = true;
    update_task_ = UpdateTask::kDownload;
    update_result_.store(-1, std::memory_order_release);
    update_complete_.store(false, std::memory_order_release);
    RefreshUpdateScene(update_scene_);
    update_thread_ = std::thread([this] {
      operation_affinity::RestoreForWorker();
      const bool success = update::Download();
      update_result_.store(success ? 0 : -1, std::memory_order_release);
      update_complete_.store(true, std::memory_order_release);
    });
  }

  void PollAutomaticUpdates() {
    if (!backend_ready_ || fastboot_mode_) return;
    const uint32_t now = MonotonicMilliseconds();
    if (now - last_update_connection_poll_ < 1000) return;
    last_update_connection_poll_ = now;
    const bool connected = RecoveryWifiConnection().connected;
    if (!connected) return;
    if (!plugin_auto_queue_.empty() && !plugin_running_ &&
        !update_running_ && !wifi_running_ && !nas_running_ &&
        !operation_running_) {
      StartNextAutomaticPluginUpdate();
      return;
    }
    if (!update_connection_seen_) {
      if (update_running_ || wifi_running_ || operation_running_) return;
      update_connection_seen_ = true;
      StartUpdateCheck(false);
      return;
    }
    if (plugin_connection_seen_ || update_running_ || plugin_running_ ||
        wifi_running_ || nas_running_ || operation_running_) return;
    plugin_connection_seen_ = true;
    StartPlugin({plugins::Job::kRefresh, ""},
                PluginTask::kBackgroundRefresh);
  }

  static double SmoothStep(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
  }

  static int32_t WaveExtent(int32_t depth, int32_t y, int32_t half_height) {
    if (depth <= 0 || half_height <= 0) return 0;
    const double distance = std::clamp(
        static_cast<double>(std::abs(y)) / half_height, 0.0, 1.0);
    // A wide, zero-slope crown avoids the pointed bow-tie silhouette. The
    // second factor gently tightens the shoulders near the screen edge while
    // keeping the centre lobe round and substantial.
    const double round = 1.0 - distance * distance;
    const double profile = round * (1.0 - 0.25 * distance * distance);
    return std::max(0, static_cast<int32_t>(depth * profile + 0.5));
  }

  static lv_point_precise_t DrawPoint(int32_t x, int32_t y) {
    lv_point_precise_t point{};
    point.x = x;
    point.y = y;
    return point;
  }

  static void DrawWaveLayer(lv_layer_t *layer, const lv_area_t &bounds,
                            bool right_edge, int32_t depth, lv_color_t color,
                            lv_opa_t opacity) {
    constexpr int32_t kSegments = 48;
    if (layer == nullptr || depth <= 0 || opacity == LV_OPA_TRANSP) return;
    const int32_t height = lv_area_get_height(&bounds);
    const int32_t half = height / 2;
    const int32_t centre = bounds.y1 + half;
    const int32_t edge_x = right_edge ? bounds.x2 : bounds.x1;

    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.color = color;
    triangle.opa = opacity;
    for (int32_t segment = 0; segment < kSegments; ++segment) {
      const int32_t y0 = bounds.y1 + height * segment / kSegments;
      const int32_t y1 = bounds.y1 + height * (segment + 1) / kSegments;
      const int32_t extent0 = WaveExtent(depth, y0 - centre, half);
      const int32_t extent1 = WaveExtent(depth, y1 - centre, half);
      const int32_t inner0 = edge_x + (right_edge ? -extent0 : extent0);
      const int32_t inner1 = edge_x + (right_edge ? -extent1 : extent1);

      triangle.p[0] = DrawPoint(edge_x, y0);
      triangle.p[1] = DrawPoint(inner0, y0);
      triangle.p[2] = DrawPoint(inner1, y1);
      lv_draw_triangle(layer, &triangle);
      triangle.p[0] = DrawPoint(edge_x, y0);
      triangle.p[1] = DrawPoint(inner1, y1);
      triangle.p[2] = DrawPoint(edge_x, y1);
      lv_draw_triangle(layer, &triangle);
    }
  }

  static void DrawEdgeGesture(lv_event_t *event) {
    auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
    auto *object = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (self == nullptr || object == nullptr || layer == nullptr ||
        object != self->gesture_indicator_) return;

    lv_area_t bounds{};
    lv_obj_get_coords(object, &bounds);
    const int32_t maximum = std::max(1, lv_area_get_width(&bounds) - 14);
    const int32_t depth = std::clamp(self->gesture_depth_, 0, maximum);
    // kMainSelected is derived from the live accent and current surface. Draw
    // it as one pre-blended opaque silhouette: it reads as translucent glass
    // without triangle overlap seams or the heavy full-screen blur pipeline.
    DrawWaveLayer(layer, bounds, self->swipe_right_edge_, depth,
                  design::kMainSelected, LV_OPA_COVER);

    if (depth < 30) return;
    const int32_t edge_x = self->swipe_right_edge_ ? bounds.x2 : bounds.x1;
    const int32_t direction = self->swipe_right_edge_ ? -1 : 1;
    const int32_t tip = edge_x + direction * std::max(16, depth * 45 / 100);
    const int32_t arm = tip + direction * 16;
    const int32_t centre = (bounds.y1 + bounds.y2) / 2;
    const int32_t half_arrow = 15;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = design::kAccent;
    line.width = 5;
    line.opa = static_cast<lv_opa_t>(
        std::clamp((depth - 26) * 10, 0, 230));
    line.round_start = true;
    line.round_end = true;
    line.p1 = DrawPoint(arm, centre - half_arrow);
    line.p2 = DrawPoint(tip, centre);
    lv_draw_line(layer, &line);
    line.p1 = DrawPoint(tip, centre);
    line.p2 = DrawPoint(arm, centre + half_arrow);
    lv_draw_line(layer, &line);
  }

  static void SetEdgeGestureDepth(void *target, int32_t value) {
    auto *self = static_cast<Impl *>(target);
    if (self == nullptr || self->gesture_indicator_ == nullptr) return;
    self->gesture_depth_ = std::max(0, value);
    lv_obj_invalidate(self->gesture_indicator_);
  }

  static void CompleteEdgeGesture(lv_anim_t *animation) {
    auto *self = static_cast<Impl *>(lv_anim_get_user_data(animation));
    if (self == nullptr || self->gesture_indicator_ == nullptr) return;
    lv_obj_delete(self->gesture_indicator_);
    self->gesture_indicator_ = nullptr;
    self->gesture_depth_ = 0;
    const bool navigate_back = self->gesture_navigate_back_;
    self->gesture_navigate_back_ = false;
    if (navigate_back) self->NavigateBack(false);
  }

  void BeginEdgeSwipe(bool right_edge) {
    CancelEdgeSwipe();
    swipe_active_ = true;
    edge_contact_captured_ = false;
    swipe_rejected_ = false;
    swipe_right_edge_ = right_edge;
    swipe_start_x_ = pointer_.x;
    swipe_start_y_ = pointer_.y;
    swipe_last_y_ = pointer_.y;
    swipe_max_inward_ = 0;
    swipe_last_inward_ = 0;
    gesture_depth_ = 2;

    const int32_t maximum_depth = std::clamp(width_ / 10, 104, 148);
    const int32_t wave_height = std::clamp(width_ / 4, 300, 410);
    gesture_indicator_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(gesture_indicator_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(gesture_indicator_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(gesture_indicator_, right_edge ? width_ - maximum_depth : 0,
                   std::clamp(pointer_.y - wave_height / 2, 0,
                              height_ - wave_height));
    lv_obj_set_size(gesture_indicator_, maximum_depth, wave_height);
    lv_obj_set_style_bg_opa(gesture_indicator_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gesture_indicator_, 0, 0);
    lv_obj_set_style_pad_all(gesture_indicator_, 0, 0);
    lv_obj_add_event_cb(gesture_indicator_, DrawEdgeGesture,
                        LV_EVENT_DRAW_MAIN, this);
    lv_obj_move_foreground(gesture_indicator_);
    lv_obj_invalidate(gesture_indicator_);
  }

  void UpdateEdgeSwipe(int32_t inward) {
    if (gesture_indicator_ == nullptr) return;
    const int32_t maximum = std::max(1, lv_obj_get_width(gesture_indicator_) - 14);
    const int32_t full_drag = std::max(1, width_ / 6);
    const double progress = std::clamp(
        static_cast<double>(inward) / full_drag, 0.0, 1.0);
    // Smooth-step starts almost flat and becomes increasingly elastic as the
    // finger pulls inward. Extra travel meets rubber-band resistance.
    const int32_t overshoot = inward > full_drag
        ? std::min(12, (inward - full_drag) / 9) : 0;
    gesture_depth_ = std::min(
        maximum, 2 + static_cast<int32_t>((maximum - 14) * SmoothStep(progress))
                     + overshoot);

    const int32_t wave_height = lv_obj_get_height(gesture_indicator_);
    const int32_t pulled_y = swipe_start_y_ +
        (pointer_.y - swipe_start_y_) * 2 / 5;
    lv_obj_set_y(gesture_indicator_,
                 std::clamp(pulled_y - wave_height / 2, 0,
                            height_ - wave_height));
    lv_obj_invalidate(gesture_indicator_);
  }

  void FinishEdgeSwipe(bool accepted) {
    swipe_active_ = false;
    swipe_rejected_ = false;
    gesture_navigate_back_ = accepted;
    if (gesture_indicator_ == nullptr) {
      gesture_navigate_back_ = false;
      return;
    }
    lv_anim_delete(this, SetEdgeGestureDepth);
    lv_anim_t settle;
    lv_anim_init(&settle);
    lv_anim_set_var(&settle, this);
    lv_anim_set_values(&settle, gesture_depth_, 0);
    lv_anim_set_duration(&settle, accepted ? 150 : 220);
    lv_anim_set_path_cb(&settle, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&settle, SetEdgeGestureDepth);
    lv_anim_set_user_data(&settle, this);
    lv_anim_set_completed_cb(&settle, CompleteEdgeGesture);
    lv_anim_start(&settle);
  }

  void RejectEdgeSwipe() {
    swipe_rejected_ = true;
    gesture_navigate_back_ = false;
    lv_anim_delete(this, SetEdgeGestureDepth);
    if (gesture_indicator_ != nullptr) {
      lv_obj_delete(gesture_indicator_);
      gesture_indicator_ = nullptr;
    }
    gesture_depth_ = 0;
  }

  void CancelEdgeSwipe() {
    swipe_active_ = false;
    edge_contact_captured_ = false;
    swipe_rejected_ = false;
    gesture_navigate_back_ = false;
    lv_anim_delete(this, SetEdgeGestureDepth);
    if (gesture_indicator_ != nullptr) {
      lv_obj_delete(gesture_indicator_);
      gesture_indicator_ = nullptr;
    }
    gesture_depth_ = 0;
  }

  bool PresentScaledSoftware(const uint8_t *pixels) {
    const unsigned int index = next_software_scanout_;
    GRSurface *surface = index == 0 ? scanout_surface1_ : scanout_surface2_;
    gr_surface target = index == 0 ? scanout1_ : scanout2_;
    if (surface == nullptr || target == nullptr || pixels == nullptr)
      return false;

    // This path is only used when the device has direct DRM scanout but the
    // optional Adreno renderer cannot start. It deliberately favors a small,
    // dependency-free nearest-neighbour fallback over making recovery fail.
    const auto *source = reinterpret_cast<const uint32_t *>(pixels);
    for (int32_t y = 0; y < physical_height_; ++y) {
      const int32_t source_y = static_cast<int32_t>(
          static_cast<int64_t>(y) * height_ / physical_height_);
      auto *destination = reinterpret_cast<uint32_t *>(
          surface->data + static_cast<size_t>(y) * surface->row_bytes);
      for (int32_t x = 0; x < physical_width_; ++x) {
        const int32_t source_x = static_cast<int32_t>(
            static_cast<int64_t>(x) * width_ / physical_width_);
        destination[x] = source[static_cast<size_t>(source_y) * width_ +
                                source_x];
      }
    }
    if (gr_drm_present(target) != 0) return false;
    next_software_scanout_ = 1U - index;
    return true;
  }

  static void FlushDisplay(lv_display_t *display, const lv_area_t *area,
                           uint8_t *pixels) {
    auto *self = static_cast<Impl *>(lv_display_get_user_data(display));
    if (self == nullptr || area == nullptr || pixels == nullptr) {
      lv_display_flush_ready(display);
      return;
    }

    if (self->gpu_accelerated_) {
      if (lv_display_flush_is_last(display)) {
        if (!self->gpu_renderer_.Present(display)) {
          __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                              "Adreno rejected the LVGL frame");
        } else {
          self->TrackSubmittedFrame();
          recorder::CapturePresentedFrame();
        }
      }
      lv_display_flush_ready(display);
      return;
    }

    if (self->direct_scanout_) {
      if (lv_display_flush_is_last(display)) {
        bool presented = false;
        if (self->transform_.IsScaled()) {
          presented = self->PresentScaledSoftware(pixels);
        } else {
          gr_surface target = nullptr;
          if (pixels == self->scanout_surface1_->data)
            target = self->scanout1_;
          else if (pixels == self->scanout_surface2_->data)
            target = self->scanout2_;
          presented = target != nullptr && gr_drm_present(target) == 0;
        }

        if (!presented) {
          __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                              "DRM rejected LVGL scanout buffer %p", pixels);
        } else {
          self->TrackSubmittedFrame();
          recorder::CapturePresentedFrame();
        }
      }
      lv_display_flush_ready(display);
      return;
    }

    GGLSurface surface{};
    surface.version = sizeof(surface);
    surface.width = static_cast<GGLuint>(self->width_);
    surface.height = static_cast<GGLuint>(self->height_);
    surface.stride = self->width_;
    surface.data = pixels;
    surface.format = GGL_PIXEL_FORMAT_BGRA_8888;

    if (lv_display_flush_is_last(display)) {
      gr_surface output = static_cast<gr_surface>(&surface);
      gr_surface scaled = nullptr;
      int32_t output_width = self->width_;
      int32_t output_height = self->height_;
      if (self->transform_.IsScaled()) {
        if (res_scale_surface(output, &scaled,
                              static_cast<float>(self->physical_width_) /
                                  self->width_,
                              static_cast<float>(self->physical_height_) /
                                  self->height_) == 0) {
          output = scaled;
          output_width = self->physical_width_;
          output_height = self->physical_height_;
        } else {
          __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                              "minui could not scale the adaptive frame");
        }
      }
      gr_blit(output, 0, 0, output_width, output_height, 0, 0);
      gr_flip();
      if (scaled != nullptr) res_free_surface(scaled);
      self->TrackSubmittedFrame();
      recorder::CapturePresentedFrame();
    }
    lv_display_flush_ready(display);
  }

  bool ConfigureDirectScanout() {
    if (gr_drm_get_scanout_buffers(&scanout1_, &scanout2_) != 0)
      return false;

    scanout_surface1_ = static_cast<GRSurface *>(scanout1_);
    scanout_surface2_ = static_cast<GRSurface *>(scanout2_);
    const bool valid =
        scanout_surface1_ != nullptr && scanout_surface2_ != nullptr &&
        scanout_surface1_->width == physical_width_ &&
        scanout_surface1_->height == physical_height_ &&
        scanout_surface2_->width == physical_width_ &&
        scanout_surface2_->height == physical_height_ &&
        scanout_surface1_->pixel_bytes == static_cast<int>(kBytesPerPixel) &&
        scanout_surface2_->pixel_bytes == static_cast<int>(kBytesPerPixel) &&
        scanout_surface1_->format == GGL_PIXEL_FORMAT_BGRA_8888 &&
        scanout_surface2_->format == GGL_PIXEL_FORMAT_BGRA_8888 &&
        scanout_surface1_->row_bytes == scanout_surface2_->row_bytes;
    if (!valid) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "DRM scanout format is incompatible; using fallback");
      scanout1_ = nullptr;
      scanout2_ = nullptr;
      scanout_surface1_ = nullptr;
      scanout_surface2_ = nullptr;
      return false;
    }

    buffer_size_ = static_cast<size_t>(scanout_surface1_->row_bytes) *
                   physical_height_;
    return buffer_size_ <= UINT32_MAX;
  }

  void TrackSubmittedFrame() {
    const uint32_t now = MonotonicMilliseconds();
    if (last_submit_ms_ == 0 || now - last_submit_ms_ > 250) {
      submit_window_start_ms_ = now;
      submitted_frames_ = 1;
      last_submit_ms_ = now;
      return;
    }

    ++submitted_frames_;
    last_submit_ms_ = now;
    const uint32_t elapsed = now - submit_window_start_ms_;
    if (elapsed < 1000)
      return;

    const double fps = static_cast<double>(submitted_frames_ - 1) * 1000.0 /
                       static_cast<double>(elapsed);
    __android_log_print(
        ANDROID_LOG_INFO, kLogTag,
        "active animation submitted %.1f fps (%u frames / %u ms)", fps,
        submitted_frames_, elapsed);
    submit_window_start_ms_ = now;
    submitted_frames_ = 1;
  }

  static void ReadMouse(lv_indev_t *input, lv_indev_data_t *data) {
    auto *self = static_cast<Impl *>(lv_indev_get_user_data(input));
    const int w = self->landscape_ ? self->height_ : self->width_;
    const int h = self->landscape_ ? self->width_ : self->height_;
    self->mouse_x_ = std::clamp(self->mouse_x_, 0, w - 1);
    self->mouse_y_ = std::clamp(self->mouse_y_, 0, h - 1);
    data->point = self->landscape_
        ? lv_point_t{self->mouse_y_, self->height_ - self->mouse_x_ - 1}
        : lv_point_t{self->mouse_x_, self->mouse_y_};
    data->state = self->mouse_connected_ && !self->suspended_ && self->mouse_pressed_
        ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    const uint32_t accent = lv_color_to_u32(design::kAccent) & 0xffffff;
    if (!self->mouse_image_.data || self->mouse_accent_ != accent) {
      self->mouse_accent_ = accent;
      self->mouse_pixels_ = mouse_cursor::Render(accent);
      auto &im = self->mouse_image_;
      im.header.magic = LV_IMAGE_HEADER_MAGIC;
      im.header.cf = LV_COLOR_FORMAT_ARGB8888;
      im.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
      im.header.w = mouse_cursor::kWidth; im.header.h = mouse_cursor::kHeight;
      im.header.stride = mouse_cursor::kWidth * 4;
      im.data_size = sizeof(self->mouse_pixels_);
      im.data = reinterpret_cast<const uint8_t *>(self->mouse_pixels_.data());
      lv_image_cache_drop(&im);
      lv_image_set_src(self->mouse_cursor_, &im);
      lv_obj_invalidate(self->mouse_cursor_);
    }
  }

  static void ReadPointer(lv_indev_t *input, lv_indev_data_t *data) {
    auto *self = static_cast<Impl *>(lv_indev_get_user_data(input));
    if (self == nullptr)
      return;
    if (self->landscape_) {
      // LVGL rotates pointer coordinates after this callback.  Supply the
      // inverse of that built-in transform so its result is precisely the
      // visible-screen coordinate stored in pointer_.
      data->point.x = self->pointer_.y;
      data->point.y = self->height_ - self->pointer_.x - 1;
    } else {
      data->point.x = self->pointer_.x;
      data->point.y = self->pointer_.y;
    }
    data->state = self->pointer_.pressed && !self->edge_contact_captured_
        ? LV_INDEV_STATE_PRESSED
        : LV_INDEV_STATE_RELEASED;
    if (self->pointer_release_on_read_) {
      self->pointer_release_on_read_ = false;
      self->pointer_.pressed = false;
    }
  }

  int32_t physical_width_ = 0;
  int32_t physical_height_ = 0;
  int32_t width_ = 0;
  Action current_tool_ = Action::kNone;
  Action current_scene_ = Action::kBackHome;
  int32_t height_ = 0;
  DisplayTransform transform_{};
  size_t buffer_size_ = 0;
  void *draw_buffer_ = nullptr;
  gr_surface scanout1_ = nullptr;
  gr_surface scanout2_ = nullptr;
  GRSurface *scanout_surface1_ = nullptr;
  GRSurface *scanout_surface2_ = nullptr;
  GpuRenderer gpu_renderer_{};
  lv_display_t *display_ = nullptr;
  lv_indev_t *pointer_device_ = nullptr;
  lv_indev_t *mouse_device_ = nullptr;
  lv_obj_t *mouse_cursor_ = nullptr;
  lv_image_dsc_t mouse_image_{};
  std::array<uint32_t, mouse_cursor::kWidth * mouse_cursor::kHeight> mouse_pixels_{};
  uint32_t mouse_accent_ = 0;
  int mouse_x_ = 0, mouse_y_ = 0;
  bool mouse_pressed_ = false, mouse_connected_ = false;
  lv_obj_t *gesture_indicator_ = nullptr;
  lv_obj_t *recents_overlay_ = nullptr;
  lv_obj_t *recents_drag_layer_ = nullptr;
  lv_obj_t *recents_drag_card_ = nullptr;
  lv_obj_t *recents_drag_image_ = nullptr;
  lv_obj_t *recents_drag_footer_ = nullptr;
  std::shared_ptr<RecentPreview> recents_drag_preview_;
  lv_obj_t *recent_launch_layer_ = nullptr;
  lv_obj_t *recent_launch_card_ = nullptr;
  lv_obj_t *recent_launch_image_ = nullptr;
  lv_obj_t *recent_launch_footer_ = nullptr;
  std::shared_ptr<RecentPreview> recent_launch_preview_;
  RecentApp recent_launch_app_;
  lv_obj_t *lock_overlay_ = nullptr;
  bool lock_ambient_ = false;
  lv_obj_t *power_overlay_ = nullptr;
  lv_obj_t *volume_overlay_ = nullptr;
  lv_obj_t *volume_icon_ = nullptr;
  lv_obj_t *volume_label_ = nullptr;
  lv_obj_t *volume_fill_ = nullptr;
  lv_timer_t *volume_hide_timer_ = nullptr;
  UsbAttachmentTracker usb_attachments_;
  lv_obj_t *screenshot_flash_ = nullptr;
  UsbAttachmentTracker mouse_attachments_, keyboard_attachments_;
  uint32_t last_usb_poll_ = 0;
  lv_obj_t *notice_overlay_ = nullptr;
  lv_obj_t *pc_overlay_ = nullptr;
  widgets::PcTransferBanner pc_transfer_;
  std::string pc_connection_id_, pc_job_id_;
  uint32_t last_pc_poll_ = 0, last_pc_platform_refresh_ = 0, last_pc_cleanup_ = 0;
  uint32_t last_pc_start_attempt_ = 0;
  PointerEvent pointer_{};
  OperationScene operation_scene_{};
  DecryptScene decrypt_scene_{};
  WifiScene wifi_scene_{};
  NasScene nas_scene_{};
  PluginScene plugin_scene_{};
  UpdateScene update_scene_{};
  std::string selected_package_;
  std::thread operation_thread_;
  std::thread decrypt_thread_;
  std::thread wifi_thread_;
  std::thread nas_thread_;
  std::thread plugin_thread_;
  std::thread update_thread_;
  std::atomic<int> operation_result_{-1};
  std::atomic<bool> operation_complete_{false};
  std::atomic<int> decrypt_result_{-1};
  std::atomic<bool> decrypt_complete_{false};
  std::atomic<int> wifi_result_{-1};
  std::atomic<bool> wifi_complete_{false};
  std::atomic<int> nas_result_{-1};
  std::atomic<bool> nas_complete_{false};
  std::atomic<int> plugin_result_{-1};
  std::atomic<bool> plugin_complete_{false};
  std::atomic<int> update_result_{-1};
  std::atomic<bool> update_complete_{false};
  plugins::Progress plugin_progress_{};
  std::vector<std::string> plugin_auto_queue_;
  std::vector<RecentApp> recent_apps_;
  std::vector<lv_obj_t *> recents_deferred_cards_;
  std::vector<lv_obj_t *> recents_all_cards_;
  Action pending_action_ = Action::kNone;
  bool screenshot_requested_ = false;
  DecryptionCompletion decryption_completion_ = DecryptionCompletion::kNone;
  uint32_t submit_window_start_ms_ = 0;
  uint32_t last_submit_ms_ = 0;
  uint32_t submitted_frames_ = 0;
  uint32_t last_operation_update_ = 0;
  uint32_t last_plugin_update_ = 0;
  uint32_t last_update_refresh_ = 0;
  uint32_t last_update_connection_poll_ = 0;
  uint64_t update_banner_build_time_ = 0;
  int32_t swipe_start_x_ = 0;
  int32_t swipe_start_y_ = 0;
  int32_t swipe_last_y_ = 0;
  int32_t swipe_max_inward_ = 0;
  int32_t swipe_last_inward_ = 0;
  int32_t gesture_depth_ = 0;
  int32_t recents_swipe_start_x_ = 0;
  int32_t recents_swipe_last_x_ = 0;
  int32_t recents_swipe_start_y_ = 0;
  int32_t recents_swipe_last_y_ = 0;
  int32_t recents_swipe_max_up_ = 0;
  int32_t recents_swipe_hold_anchor_y_ = 0;
  int32_t recents_transition_progress_ = 0;
  int32_t recents_target_x_ = 0;
  int32_t recents_target_y_ = 0;
  int32_t recents_target_width_ = 0;
  int32_t recents_target_height_ = 0;
  uint32_t recents_last_drag_update_ms_ = 0;
  uint32_t recents_swipe_hold_started_ms_ = 0;
  int32_t recent_launch_start_x_ = 0;
  int32_t recent_launch_start_y_ = 0;
  int32_t recent_launch_start_width_ = 0;
  int32_t recent_launch_start_height_ = 0;
  bool direct_scanout_ = false;
  unsigned int next_software_scanout_ = 0;
  bool gpu_accelerated_ = false;
  bool recents_transition_active_ = false;
  bool recents_transition_finish_open_ = false;
  bool recents_transition_finish_home_ = false;
  bool recents_swipe_hold_committed_ = false;
  bool recent_launch_active_ = false;
  bool backend_ready_ = false;
  bool boot_animation_complete_ = false;
  bool operation_running_ = false;
  bool operation_visible_ = true;
  int credential_type_ = 0;
  int crypto_user_id_ = 0;
  bool decrypt_running_ = false;
  bool wifi_running_ = false;
  bool wifi_scene_report_ = false;
  void *wifi_report_state_ = nullptr;
  bool nas_running_ = false;
  bool plugin_running_ = false;
  bool plugin_connection_seen_ = false;
  PluginTask plugin_task_ = PluginTask::kNone;
  plugins::Job plugin_job_ = plugins::Job::kRefresh;
  bool update_running_ = false;
  bool update_manual_ = false;
  bool update_installing_ = false;
  bool update_connection_seen_ = false;
  UpdateTask update_task_ = UpdateTask::kNone;
  bool decryption_active_ = false;
  bool secondary_decryption_ = false;
  bool interactive_ready_ = false;
  bool navigating_back_ = false;
  bool swipe_active_ = false;
  bool edge_contact_captured_ = false;
  bool swipe_rejected_ = false;
  bool swipe_right_edge_ = false;
  bool gesture_navigate_back_ = false;
  bool recents_swipe_active_ = false;
  // A contact held in the bottom strip, not yet a Recents swipe.
  bool recents_pending_ = false;
  int32_t recents_pending_x_ = 0;
  int32_t recents_pending_y_ = 0;
  // Slot 0 is down, whoever has it.
  bool slot0_down_ = false;
  // The pointer reads pressed once more, then released (a replayed tap).
  bool pointer_release_on_read_ = false;
  bool suspended_ = false;
  bool fastboot_mode_ = false;
  bool on_home_ = false;
  bool landscape_ = false;
  bool initialized_ = false;
  std::vector<Action> navigation_history_;
  int displayed_volume_ = 30;
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

bool Engine::Initialize(bool fastboot_mode, bool adaptive_resolution,
                        int32_t logical_height, bool resume_recovery) {
  return impl_->Initialize(fastboot_mode, adaptive_resolution, logical_height,
                           resume_recovery);
}
void Engine::Shutdown() { impl_->Shutdown(); }
void Engine::SetFastbootMode(bool enabled, bool show_recovery_home) {
  impl_->SetFastbootMode(enabled, show_recovery_home);
}
void Engine::AbandonForTerminalAction() { impl_.release(); }
uint32_t Engine::RunFrame() { return impl_->RunFrame(); }
Action Engine::TakeAction() { return impl_->TakeAction(); }
bool Engine::TakeScreenshotRequest() {
  return impl_->TakeScreenshotRequest();
}
void Engine::NavigateHome() { impl_->NavigateHome(); }
void Engine::NavigateBack() { impl_->NavigateBack(); }
void Engine::SetBackendReady() { impl_->SetBackendReady(); }
void Engine::BeginDecryption(int credential_type, bool file_based, int user_id,
                             int pattern_grid_size) {
  impl_->BeginDecryption(credential_type, file_based, user_id,
                         pattern_grid_size);
}
DecryptionCompletion Engine::TakeDecryptionCompletion() {
  return impl_->TakeDecryptionCompletion();
}
void Engine::SetPointer(const PointerEvent &event) { impl_->SetPointer(event); }
void Engine::SetMouseConnected(bool connected) { impl_->SetMouseConnected(connected); }
void Engine::MouseMove(int dx, int dy) { impl_->MouseMove(dx, dy); }
void Engine::MouseButton(bool pressed) { impl_->MouseButton(pressed); }
void Engine::MouseWheel(int steps) { impl_->MouseWheel(steps); }
bool Engine::KeyboardKey(unsigned code, int value) { return impl_->KeyboardKey(code, value); }
void Engine::ResetKeyboard() { impl_->ResetKeyboard(); }
void Engine::SetSuspended(bool suspended) { impl_->SetSuspended(suspended); }
void Engine::ShowLockScreen(bool ambient) { impl_->ShowLockScreen(ambient); }
void Engine::ShowPowerMenu() { impl_->ShowPowerMenu(); }
void Engine::ShowVolume(int percent) { impl_->ShowVolume(percent); }
void Engine::ShowScreenshotResult(bool success) {
  impl_->ShowScreenshotResult(success);
}
int32_t Engine::Width() const { return impl_->Width(); }
int32_t Engine::Height() const { return impl_->Height(); }
bool Engine::IsLandscape() const { return impl_->IsLandscape(); }
bool Engine::IsInitialized() const { return impl_->IsInitialized(); }

} // namespace aeraui
