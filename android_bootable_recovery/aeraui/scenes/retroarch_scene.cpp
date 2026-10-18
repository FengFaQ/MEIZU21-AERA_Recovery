/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "ui_components.hpp"
#include "browser/runtime.hpp"
#include "retroarch/launcher.hpp"
#include "retroarch/protocol.hpp"
#include "retroarch/session.hpp"
#include "retroarch_icon.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <thread>
#include <unistd.h>

namespace aeraui {
namespace {
using namespace widgets;

struct RetroArchScene {
  lv_obj_t *screen = nullptr;
  lv_obj_t *viewport = nullptr;
  lv_obj_t *image = nullptr;
  lv_obj_t *loading = nullptr;
  lv_obj_t *title = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *navigation = nullptr;
  lv_image_dsc_t descriptor{};
  retro::Session session;
  retro::Process process;
  web::Preparation preparation;
  std::thread worker;
  lv_timer_t *timer = nullptr;
  bool launched = false;
  bool frame_visible = false;
  int view_left = 0;
  int view_top = 0;
  int view_width = 0;
  int view_height = 0;

  ~RetroArchScene() {
    if (timer) lv_timer_delete(timer);
    preparation.cancel.store(true);
    if (worker.joinable()) worker.join();
    if (image) {
      lv_image_set_src(image, nullptr);
      lv_image_cache_drop(&descriptor);
    }
    if (session.Connected()) session.Send(retro::Kind::kClose);
    session.Close();
    process.Stop();
    web::RemoveRuntime(preparation.directory);
  }
};

void LaunchRetroArch(RetroArchScene *scene) {
  if (!scene->preparation.verified || scene->launched) return;
  scene->launched = true;
  int frame_fd = -1, control_fd = -1;
  std::string error;
  if (!scene->process.Start(scene->preparation.directory, frame_fd,
                            control_fd, error) ||
      !scene->session.Adopt(frame_fd, control_fd)) {
    i18n::BindLabel(scene->title, "RetroArch could not start");
    i18n::BindLabel(scene->detail,
        error.empty() ? scene->session.Status().c_str() : error.c_str());
    return;
  }
  i18n::BindLabel(scene->title, "Starting RetroArch");
  i18n::BindLabel(scene->detail,
      "Loading the recovery-safe menu, bundled test core, and your content library.");
  lv_bar_set_value(scene->progress, 100, LV_ANIM_ON);
}

}  // namespace

void BuildRetroArchScene(lv_obj_t *screen, ActionCallback callback,
                         void *context) {
  auto *scene = new RetroArchScene;
  scene->screen = screen;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<RetroArchScene *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, scene);

  MainBackground(screen);
  const int screen_width = lv_obj_get_width(screen);
  const int screen_height = lv_obj_get_height(screen);
  const bool landscape = screen_width > screen_height;
  scene->viewport = lv_obj_create(screen);
  Clear(scene->viewport);
  lv_obj_set_pos(scene->viewport, 0, 0);
  lv_obj_set_size(scene->viewport, screen_width, screen_height);
  lv_obj_add_flag(scene->viewport, LV_OBJ_FLAG_HIDDEN);
  scene->image = lv_image_create(scene->viewport);
  lv_image_set_pivot(scene->image, 0, 0);
  const int image_scale = std::min(screen_width * 256 / retro::kWidth,
                                   screen_height * 256 / retro::kHeight);
  scene->view_width = retro::kWidth * image_scale / 256;
  scene->view_height = retro::kHeight * image_scale / 256;
  scene->view_left = (screen_width - scene->view_width) / 2;
  scene->view_top = (screen_height - scene->view_height) / 2;
  lv_image_set_scale(scene->image, image_scale);
  lv_obj_set_pos(scene->image, scene->view_left, scene->view_top);
  lv_obj_remove_flag(scene->image, LV_OBJ_FLAG_CLICKABLE);
  scene->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  scene->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  scene->descriptor.header.flags =
      LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;
  scene->descriptor.header.w = retro::kWidth;
  scene->descriptor.header.h = retro::kHeight;
  scene->descriptor.header.stride = retro::kWidth * 4;
  scene->descriptor.data_size = retro::kFrameBytes;
  lv_obj_add_event_cb(scene->viewport, [](lv_event_t *event) {
    const auto code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    auto *scene = static_cast<RetroArchScene *>(lv_event_get_user_data(event));
    if (!scene->session.Connected()) return;
    auto *input = lv_indev_active();
    if (!input) return;
    lv_point_t point{};
    lv_indev_get_point(input, &point);
    if (point.x < scene->view_left || point.x >= scene->view_left + scene->view_width ||
        point.y < scene->view_top || point.y >= scene->view_top + scene->view_height)
      return;
    const int x = std::clamp<int>((point.x - scene->view_left) * retro::kWidth /
                                  scene->view_width,
                                  0, retro::kWidth - 1);
    const int y = std::clamp<int>((point.y - scene->view_top) * retro::kHeight /
                                  scene->view_height,
                                  0, retro::kHeight - 1);
    const auto kind = code == LV_EVENT_PRESSED ? retro::Kind::kTouchDown :
        code == LV_EVENT_PRESSING ? retro::Kind::kTouchMove :
                                    retro::Kind::kTouchUp;
    scene->session.Send(kind, x, y);
  }, LV_EVENT_ALL, scene);

  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  scene->loading = lv_obj_create(screen);
  Panel(scene->loading, 44, kMainPanel);
  lv_obj_set_size(scene->loading, landscape ? 1600 : 1312,
                  landscape ? 1000 : 1110);
  lv_obj_align(scene->loading, LV_ALIGN_CENTER, 0, 0);
  auto *icon = RetroArchIconPlate(scene->loading, kText, 130);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 100);
  scene->title = Label(scene->loading, "Preparing RetroArch",
                       &lv_font_montserrat_48, kText);
  lv_obj_set_width(scene->title, 1150);
  lv_obj_set_style_text_align(scene->title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(scene->title, LV_ALIGN_TOP_MID, 0, 285);
  scene->detail = Label(scene->loading,
      "Verifying the signed runtime and expanding it into private RAM.",
      &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(scene->detail, 1080);
  lv_obj_set_style_text_align(scene->detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(scene->detail, 16, 0);
  lv_obj_align(scene->detail, LV_ALIGN_TOP_MID, 0, 405);
  scene->progress = lv_bar_create(scene->loading);
  lv_obj_set_size(scene->progress, 1020, 16);
  lv_obj_align(scene->progress, LV_ALIGN_TOP_MID, 0, 620);
  lv_obj_set_style_bg_color(scene->progress, kCyan, LV_PART_INDICATOR);
  auto *note = Label(scene->loading,
      "Includes the public-domain 2048 test core. No ROMs or BIOS files are bundled.\n\n"
      "Game audio uses AERA's protected speaker bridge. Content is opened read-only from storage.",
      &lv_font_montserrat_32, kMuted);
  lv_obj_set_width(note, 1080);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(note, 14, 0);
  lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 730);
  scene->navigation = Navigation(screen, Action::kNone, callback, context, true);

  scene->worker = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, "retroarch",
                              "app-runtime", "retroarch");
  });
  scene->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *scene = static_cast<RetroArchScene *>(lv_timer_get_user_data(timer));
    if (!scene->preparation.done.load(std::memory_order_acquire)) {
      lv_bar_set_value(scene->progress, scene->preparation.progress.load(),
                       LV_ANIM_OFF);
      return;
    }
    if (!scene->preparation.verified) {
      i18n::BindLabel(scene->title, "RetroArch unavailable");
      i18n::BindLabel(scene->detail, scene->preparation.error.c_str());
      return;
    }
    LaunchRetroArch(scene);
    if (!scene->session.Connected()) return;
    scene->session.AcknowledgeFrame();
    if (scene->session.Poll()) {
      scene->descriptor.data = scene->session.Pixels();
      lv_image_cache_drop(&scene->descriptor);
      lv_image_set_src(scene->image, &scene->descriptor);
      lv_obj_invalidate(scene->image);
      lv_obj_remove_flag(scene->viewport, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(scene->loading, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(scene->navigation);
      scene->frame_visible = true;
    }
    if (!scene->process.Running() && scene->frame_visible) {
      scene->session.Close();
      lv_obj_add_flag(scene->viewport, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(scene->loading, LV_OBJ_FLAG_HIDDEN);
      i18n::BindLabel(scene->title, "RetroArch closed");
      i18n::BindLabel(scene->detail,
          "Return Home, or reopen RetroArch to start a fresh session.");
    }
  }, 8, scene);
}

}  // namespace aeraui
