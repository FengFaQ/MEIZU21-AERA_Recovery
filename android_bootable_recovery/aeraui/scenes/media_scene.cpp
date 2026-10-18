/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "ui_components.hpp"
#include "browser/runtime.hpp"
#include "media/launcher.hpp"
#include "media/protocol.hpp"
#include "media/session.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace aeraui {
namespace {
using namespace widgets;

struct MediaFile {
  std::string path;
  std::string name;
  bool video = false;
};

bool MediaSuffix(const std::string &name, bool &video) {
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string suffix = name.substr(dot);
  for (char &character : suffix)
    character = static_cast<char>(
        tolower(static_cast<unsigned char>(character)));
  video = suffix == ".mp4" || suffix == ".mkv" || suffix == ".webm" ||
      suffix == ".mov";
  return video || suffix == ".mp3" || suffix == ".flac" ||
      suffix == ".wav" || suffix == ".m4a" || suffix == ".aac" ||
      suffix == ".ogg" || suffix == ".opus";
}

void ScanMedia(const std::string &path, int depth,
               std::vector<MediaFile> &output) {
  if (depth > 5 || output.size() >= 400) return;
  DIR *directory = opendir(path.c_str());
  if (!directory) return;
  while (auto *entry = readdir(directory)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        entry->d_name[0] == '.') continue;
    const std::string child = path + "/" + entry->d_name;
    struct stat info{};
    if (lstat(child.c_str(), &info) || S_ISLNK(info.st_mode)) continue;
    if (S_ISDIR(info.st_mode)) {
      ScanMedia(child, depth + 1, output);
    } else if (S_ISREG(info.st_mode)) {
      bool video = false;
      if (MediaSuffix(entry->d_name, video))
        output.push_back({child, entry->d_name, video});
    }
    if (output.size() >= 400) break;
  }
  closedir(directory);
}

std::string Time(int64_t milliseconds) {
  if (milliseconds < 0) milliseconds = 0;
  const int seconds = static_cast<int>(milliseconds / 1000);
  char value[32];
  snprintf(value, sizeof(value), "%d:%02d", seconds / 60, seconds % 60);
  return value;
}

struct MediaThumbnail {
  std::string path;
  int row = 0;
  lv_obj_t *preview = nullptr;
  lv_obj_t *placeholder = nullptr;
  lv_obj_t *image = nullptr;
  std::vector<uint8_t> pixels;
  lv_image_dsc_t descriptor{};
  int state = 0;  // dormant, requested, visible, or unavailable
  int32_t token = 0;
};

struct MediaScene {
  lv_obj_t *screen = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *viewer = nullptr;
  lv_obj_t *video = nullptr;
  lv_obj_t *empty = nullptr;
  lv_obj_t *chrome = nullptr;
  lv_obj_t *controls = nullptr;
  lv_obj_t *back = nullptr;
  lv_obj_t *viewer_title = nullptr;
  lv_obj_t *time = nullptr;
  lv_obj_t *play = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *grid = nullptr;
  lv_image_dsc_t descriptor{};
  std::vector<uint8_t> frame_pixels =
      std::vector<uint8_t>(media::kFrameBytes);
  media::Session session;
  media::Process process;
  web::Preparation preparation;
  std::thread worker;
  lv_timer_t *timer = nullptr;
  bool launched = false;
  bool has_frame = false;
  bool descriptor_bound = false;
  bool seeking = false;
  bool viewer_open = false;
  bool pending_open = false;
  bool chrome_visible = false;
  uint32_t chrome_hide_at = 0;
  int last_progress = -1;
  int thumbnail_loading = -1;
  int thumbnail_width = 0;
  int thumbnail_height = 0;
  int thumbnail_row_stride = 0;
  int32_t next_thumbnail_token = 1;
  uint32_t thumbnail_started_at = 0;
  int64_t last_position_second = -1;
  int64_t last_duration_second = -1;
  bool last_playing = false;
  bool have_playing = false;
  std::string selected;
  std::vector<std::unique_ptr<MediaThumbnail>> thumbnails;

  ~MediaScene() {
    if (timer) lv_timer_delete(timer);
    preparation.cancel.store(true);
    if (worker.joinable()) worker.join();
    if (video && descriptor_bound) {
      lv_image_set_src(video, nullptr);
      lv_image_cache_drop(&descriptor);
    }
    for (auto &thumbnail : thumbnails) {
      if (thumbnail->image) {
        lv_image_set_src(thumbnail->image, nullptr);
        lv_image_cache_drop(&thumbnail->descriptor);
      }
    }
    if (session.Connected()) session.Send(media::Kind::kClose);
    session.Close();
    process.Stop();
    web::RemoveRuntime(preparation.directory);
  }
};

bool TickReached(uint32_t now, uint32_t target);

bool ThumbnailNearViewport(const MediaScene *scene,
                           const MediaThumbnail *thumbnail, int margin) {
  if (!scene->grid) return false;
  const int scroll = lv_obj_get_scroll_y(scene->grid);
  const int viewport = lv_obj_get_height(scene->grid);
  const int top = thumbnail->row * scene->thumbnail_row_stride;
  const int bottom = top + scene->thumbnail_row_stride;
  return bottom >= scroll - margin && top <= scroll + viewport + margin;
}

void DropThumbnail(MediaThumbnail *thumbnail) {
  if (thumbnail->image) {
    lv_obj_delete(thumbnail->image);
    thumbnail->image = nullptr;
    lv_image_cache_drop(&thumbnail->descriptor);
  }
  thumbnail->descriptor = {};
  thumbnail->pixels.clear();
  thumbnail->pixels.shrink_to_fit();
  thumbnail->state = 0;
  if (thumbnail->placeholder)
    lv_obj_remove_flag(thumbnail->placeholder, LV_OBJ_FLAG_HIDDEN);
}

void ShowThumbnail(MediaThumbnail *thumbnail, int width, int height,
                   int source_width, int source_height) {
  if (!thumbnail || thumbnail->pixels.empty()) return;
  auto &descriptor = thumbnail->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = source_width;
  descriptor.header.h = source_height;
  descriptor.header.stride = source_width * 4;
  descriptor.data_size = thumbnail->pixels.size();
  descriptor.data = thumbnail->pixels.data();
  thumbnail->image = lv_image_create(thumbnail->preview);
  lv_image_set_src(thumbnail->image, &descriptor);
  lv_image_set_antialias(thumbnail->image, true);
  const uint32_t scale = std::max(
      (static_cast<uint32_t>(width) * 256 + source_width - 1) / source_width,
      (static_cast<uint32_t>(height) * 256 + source_height - 1) / source_height);
  lv_image_set_scale(thumbnail->image, scale);
  lv_obj_center(thumbnail->image);
  lv_obj_remove_flag(thumbnail->image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(thumbnail->placeholder, LV_OBJ_FLAG_HIDDEN);
  thumbnail->state = 2;
}

void MaintainThumbnails(MediaScene *scene) {
  if (!scene->grid || scene->viewer_open || !scene->session.Connected()) return;
  const int keep_margin = lv_obj_get_height(scene->grid);
  for (auto &thumbnail : scene->thumbnails) {
    if (thumbnail->state == 2 &&
        !ThumbnailNearViewport(scene, thumbnail.get(), keep_margin))
      DropThumbnail(thumbnail.get());
  }
  if (scene->thumbnail_loading >= 0) {
    if (TickReached(lv_tick_get(), scene->thumbnail_started_at + 4500)) {
      scene->thumbnails[scene->thumbnail_loading]->state = 3;
      scene->thumbnail_loading = -1;
    } else {
      return;
    }
  }
  if (scene->session.Playing()) return;

  const int preload_margin = scene->thumbnail_row_stride;
  const int center = lv_obj_get_scroll_y(scene->grid) +
      lv_obj_get_height(scene->grid) / 2;
  int best = -1;
  int best_distance = 0x7fffffff;
  for (size_t index = 0; index < scene->thumbnails.size(); ++index) {
    auto *thumbnail = scene->thumbnails[index].get();
    if (thumbnail->state != 0 ||
        !ThumbnailNearViewport(scene, thumbnail, preload_margin)) continue;
    const int distance = std::abs(
        thumbnail->row * scene->thumbnail_row_stride +
        scene->thumbnail_row_stride / 2 - center);
    if (distance < best_distance) {
      best = static_cast<int>(index);
      best_distance = distance;
    }
  }
  if (best < 0) return;
  auto *thumbnail = scene->thumbnails[best].get();
  if (++scene->next_thumbnail_token <= 0) scene->next_thumbnail_token = 1;
  thumbnail->token = scene->next_thumbnail_token;
  if (!scene->session.Send(media::Kind::kThumbnail,
                           thumbnail->path.c_str(), thumbnail->token)) {
    thumbnail->state = 3;
    return;
  }
  thumbnail->state = 1;
  scene->thumbnail_loading = best;
  scene->thumbnail_started_at = lv_tick_get();
}

bool TickReached(uint32_t now, uint32_t target) {
  return target != 0 && static_cast<int32_t>(now - target) >= 0;
}

void AnimateCoordinate(lv_obj_t *object, bool horizontal, int32_t end,
                       uint32_t duration) {
  if (!object) return;
  lv_anim_delete(object, nullptr);
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, object);
  lv_anim_set_values(&animation,
                     horizontal ? lv_obj_get_x(object) : lv_obj_get_y(object),
                     end);
  lv_anim_set_duration(&animation, duration);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&animation,
      horizontal
          ? [](void *target, int32_t value) {
              lv_obj_set_x(static_cast<lv_obj_t *>(target), value);
            }
          : [](void *target, int32_t value) {
              lv_obj_set_y(static_cast<lv_obj_t *>(target), value);
            });
  lv_anim_start(&animation);
}

void ShowChrome(MediaScene *scene) {
  if (!scene || !scene->chrome) return;
  const int height = lv_obj_get_height(scene->screen);
  AnimateCoordinate(scene->back, true, 34, 180);
  AnimateCoordinate(scene->controls, false, height - 224, 220);
  scene->chrome_visible = true;
  scene->chrome_hide_at = lv_tick_get() + 1000;
}

void UpdateChrome(MediaScene *scene) {
  if (!scene || !scene->chrome || !scene->viewer_open ||
      !scene->chrome_visible || scene->seeking ||
      !TickReached(lv_tick_get(), scene->chrome_hide_at)) return;
  // Moving resident controls avoids opacity layers and subtree teardown while
  // the OpenGL streaming texture is active.
  AnimateCoordinate(scene->back, true, -138, 180);
  AnimateCoordinate(scene->controls, false,
                    lv_obj_get_height(scene->screen) + 18, 220);
  scene->chrome_visible = false;
  scene->chrome_hide_at = 0;
}

void SetPlayIcon(MediaScene *scene) {
  if (!scene || !scene->play || lv_obj_get_child_count(scene->play) == 0)
    return;
  const bool playing = scene->session.Playing();
  if (scene->have_playing && playing == scene->last_playing) return;
  scene->have_playing = true;
  scene->last_playing = playing;
  lv_label_set_text_static(lv_obj_get_child(scene->play, 0),
                           playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void UpdateTimeline(MediaScene *scene, int64_t position) {
  if (!scene || !scene->time) return;
  const int64_t position_second = std::max<int64_t>(0, position) / 1000;
  const int64_t duration_second =
      std::max<int64_t>(0, scene->session.Duration()) / 1000;
  if (position_second == scene->last_position_second &&
      duration_second == scene->last_duration_second) return;
  scene->last_position_second = position_second;
  scene->last_duration_second = duration_second;
  const std::string text = Time(position) + "  /  " +
      Time(scene->session.Duration());
  i18n::BindLabel(scene->time, text.c_str());
}

void LayoutViewer(MediaScene *scene) {
  const int width = lv_obj_get_width(scene->screen);
  const int height = lv_obj_get_height(scene->screen);
  lv_obj_set_pos(scene->viewer, 0, 0);
  lv_obj_set_size(scene->viewer, width, height);
  lv_obj_set_pos(scene->video, 0, 0);
  lv_obj_set_size(scene->video, width, height);
  lv_image_set_inner_align(scene->video, LV_IMAGE_ALIGN_CONTAIN);
  lv_obj_set_width(scene->empty, std::max(240, width - 160));
  lv_obj_center(scene->empty);

  lv_obj_set_pos(scene->chrome, 0, 0);
  lv_obj_set_size(scene->chrome, width, height);
  lv_obj_set_pos(scene->controls, 32, height - 224);
  lv_obj_set_size(scene->controls, width - 64, 190);
  lv_obj_set_pos(scene->play, 28, 43);
  lv_obj_set_size(scene->play, 104, 104);
  lv_obj_set_pos(scene->progress, 166, 39);
  lv_obj_set_size(scene->progress, width - 246, 18);
  lv_obj_set_pos(scene->time, 166, 89);
  lv_obj_set_pos(scene->viewer_title, 166, 132);
  lv_obj_set_width(scene->viewer_title, width - 246);
}

void TogglePlayback(MediaScene *scene) {
  if (!scene || !scene->session.Connected()) return;
  if (scene->session.Duration() <= 0 && !scene->selected.empty()) {
    scene->session.Send(media::Kind::kOpen, scene->selected.c_str());
  } else {
    scene->session.Send(scene->session.Playing() ? media::Kind::kPause
                                                 : media::Kind::kPlay);
  }
}

void CloseViewer(MediaScene *scene) {
  if (!scene || !scene->viewer_open) return;
  if (scene->session.Connected() && scene->session.Playing())
    scene->session.Send(media::Kind::kPause);
  scene->viewer_open = false;
  scene->pending_open = false;
  lv_obj_add_flag(scene->viewer, LV_OBJ_FLAG_HIDDEN);
}

void SendOpen(MediaScene *scene) {
  if (!scene->session.Connected() || scene->selected.empty()) return;
  scene->session.Send(media::Kind::kOpen, scene->selected.c_str());
  scene->pending_open = false;
}

void Open(MediaScene *scene, const std::string &path) {
  if (scene->thumbnail_loading >= 0) {
    scene->thumbnails[scene->thumbnail_loading]->state = 0;
    scene->thumbnail_loading = -1;
  }
  scene->selected = path;
  scene->viewer_open = true;
  scene->pending_open = true;
  scene->has_frame = false;
  scene->last_progress = -1;
  scene->last_position_second = -1;
  scene->last_duration_second = -1;
  scene->have_playing = false;
  // Keep the variable image descriptor bound for the scene's whole lifetime.
  // Unbinding and rebinding it between files leaves the OpenGL streaming unit
  // holding a stale draw source while the new player is being prepared.
  lv_obj_add_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
  i18n::BindLabel(scene->empty,
                    scene->session.Connected()
                        ? LV_SYMBOL_VIDEO "\nOpening video…"
                        : LV_SYMBOL_VIDEO "\nPreparing player…");
  lv_obj_remove_flag(scene->empty, LV_OBJ_FLAG_HIDDEN);
  i18n::BindLabel(scene->viewer_title,
                    path.substr(path.find_last_of('/') + 1).c_str());
  lv_slider_set_value(scene->progress, 0, LV_ANIM_OFF);
  UpdateTimeline(scene, 0);
  lv_obj_remove_flag(scene->viewer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(scene->viewer);
  LayoutViewer(scene);
  ShowChrome(scene);
  if (scene->session.Connected()) SendOpen(scene);
}

lv_obj_t *ViewerButton(lv_obj_t *parent, const char *text,
                       std::function<void()> action) {
  auto *button = Button(parent, text, std::move(action));
  lv_obj_set_style_bg_color(button, Color(0x15181d), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, Color(0xffffff), 0);
  lv_obj_set_style_border_opa(button, LV_OPA_20, 0);
  return button;
}

void BuildViewer(MediaScene *scene) {
  scene->viewer = lv_obj_create(scene->screen);
  Clear(scene->viewer);
  lv_obj_set_user_data(scene->viewer, &kPersistentModalMarker);
  lv_obj_add_event_cb(scene->viewer, [](lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_CANCEL)
      CloseViewer(static_cast<MediaScene *>(lv_event_get_user_data(event)));
  }, LV_EVENT_CANCEL, scene);
  lv_obj_set_style_bg_color(scene->viewer, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scene->viewer, LV_OPA_COVER, 0);

  scene->empty = Label(scene->viewer, LV_SYMBOL_VIDEO "\nOpening video…",
                       &lv_font_montserrat_40, kMutedStrong);
  lv_obj_set_style_text_align(scene->empty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(scene->empty, 30, 0);

  scene->video = lv_image_create(scene->viewer);
  lv_obj_remove_flag(scene->video, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
  scene->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  scene->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  scene->descriptor.header.flags =
      LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;
  scene->descriptor.header.w = media::kLandscapeWidth;
  scene->descriptor.header.h = media::kLandscapeHeight;
  scene->descriptor.header.stride = media::kLandscapeWidth * 4;
  scene->descriptor.data_size = media::kFrameBytes;
  scene->descriptor.data = scene->frame_pixels.data();

  auto reveal = [](lv_event_t *event) {
    ShowChrome(static_cast<MediaScene *>(lv_event_get_user_data(event)));
  };
  lv_obj_add_flag(scene->viewer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(scene->video, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scene->viewer, reveal, LV_EVENT_CLICKED, scene);
  lv_obj_add_event_cb(scene->video, reveal, LV_EVENT_CLICKED, scene);

  scene->chrome = lv_obj_create(scene->viewer);
  Clear(scene->chrome);
  lv_obj_remove_flag(scene->chrome, LV_OBJ_FLAG_CLICKABLE);

  scene->controls = lv_obj_create(scene->chrome);
  Clear(scene->controls);
  lv_obj_remove_flag(scene->controls, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(scene->controls, 44, 0);
  lv_obj_set_style_bg_color(scene->controls, Color(0x080a0d), 0);
  lv_obj_set_style_bg_opa(scene->controls, 218, 0);
  lv_obj_set_style_bg_grad_color(scene->controls, Color(0x242932), 0);
  lv_obj_set_style_bg_grad_dir(scene->controls, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(scene->controls, 1, 0);
  lv_obj_set_style_border_color(scene->controls, Color(0xffffff), 0);
  lv_obj_set_style_border_opa(scene->controls, LV_OPA_20, 0);

  scene->progress = lv_slider_create(scene->controls);
  lv_slider_set_range(scene->progress, 0, 10000);
  RangeSlider(scene->progress);
  lv_obj_set_style_pad_all(scene->progress, 15, LV_PART_KNOB);
  lv_obj_add_event_cb(scene->progress, [](lv_event_t *event) {
    auto *scene = static_cast<MediaScene *>(lv_event_get_user_data(event));
    const auto code = lv_event_get_code(event);
    if (!scene) return;
    if (code == LV_EVENT_PRESSED) {
      scene->seeking = true;
      ShowChrome(scene);
    }
    if (code == LV_EVENT_VALUE_CHANGED && scene->seeking) {
      const int64_t target = scene->session.Duration() > 0
          ? scene->session.Duration() *
                lv_slider_get_value(scene->progress) / 10000
          : 0;
      UpdateTimeline(scene, target);
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
      if (!scene->seeking) return;
      const int64_t duration = scene->session.Duration();
      const int64_t target = duration > 0
          ? duration * lv_slider_get_value(scene->progress) / 10000
          : 0;
      const int64_t delta = target - scene->session.Position();
      const int seconds = static_cast<int>(
          delta >= 0 ? (delta + 500) / 1000 : (delta - 500) / 1000);
      if (seconds)
        scene->session.Send(media::Kind::kSeekRelative, nullptr, seconds);
      scene->seeking = false;
      ShowChrome(scene);
    }
  }, LV_EVENT_ALL, scene);

  scene->time = Label(scene->controls, "0:00  /  0:00",
                      &lv_font_montserrat_24, Color(0xe7ebf0));
  scene->viewer_title = Label(scene->controls, "",
                              &lv_font_montserrat_24, Color(0xb8bec6));
  lv_label_set_long_mode(scene->viewer_title, LV_LABEL_LONG_DOT);

  scene->play = ViewerButton(scene->controls, LV_SYMBOL_PLAY, [scene] {
    TogglePlayback(scene);
    ShowChrome(scene);
  });
  lv_obj_set_style_radius(scene->play, 52, 0);
  lv_obj_set_style_bg_color(scene->play, kAccent, 0);
  lv_obj_set_style_bg_color(scene->play, kAccentPressed, LV_STATE_PRESSED);
  if (lv_obj_get_child_count(scene->play))
    lv_obj_set_style_text_color(lv_obj_get_child(scene->play, 0),
                                kOnAccent, 0);

  scene->back = ViewerButton(scene->chrome, LV_SYMBOL_LEFT,
                             [scene] { CloseViewer(scene); });
  lv_obj_set_pos(scene->back, 34, 34);
  lv_obj_set_size(scene->back, 104, 104);
  lv_obj_set_style_radius(scene->back, 52, 0);

  LayoutViewer(scene);
  lv_obj_add_flag(scene->viewer, LV_OBJ_FLAG_HIDDEN);
}
}  // namespace

void BuildMediaScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *scene = new MediaScene;
  scene->screen = screen;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<MediaScene *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, scene);

  Header(screen, "AERA Media", "Choose music or video from your library.",
         callback, context);
  const bool landscape = Landscape(screen);
  std::vector<MediaFile> files;
  for (const char *root : {"/sdcard/Music", "/sdcard/Movies",
                           "/sdcard/Download", "/sdcard/DCIM",
                           "/sdcard/AERA"})
    ScanMedia(root, 0, files);
  std::sort(files.begin(), files.end(), [](const auto &first,
                                           const auto &second) {
    return first.path > second.path;
  });

  const std::string count = files.size() == 1
      ? i18n::Format("%zu ITEM", files.size())
      : i18n::Format("%zu ITEMS", files.size());
  scene->status = Kicker(screen,
      ("PREPARING PLAYER  ·  " + count).c_str(), kMutedStrong);
  lv_obj_set_pos(scene->status, 80, landscape ? 306 : 426);

  scene->grid = Scroll(screen, landscape ? 350 : 500,
      lv_obj_get_height(screen) - (landscape ? 350 : 500) - 28);
  const int columns = landscape ? 4 : 2;
  const int tile_width = landscape ? 748 : 640;
  const int tile_height = landscape ? 310 : 352;
  const int row_stride = tile_height + 16;
  scene->thumbnail_width = tile_width - 20;
  scene->thumbnail_height = tile_height - 20;
  scene->thumbnail_row_stride = row_stride;
  scene->thumbnails.reserve(files.size());

  for (size_t index = 0; index < files.size(); ++index) {
    const auto &file = files[index];
    const int column = static_cast<int>(index) % columns;
    const int row = static_cast<int>(index) / columns;
    auto *tile = lv_button_create(scene->grid);
    Panel(tile, 34, kMainPanel);
    Interactive(tile, kMainSelected);
    lv_obj_set_pos(tile, column * (landscape ? 764 : 664), row * row_stride);
    lv_obj_set_size(tile, tile_width, tile_height);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, kMainLine, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_30, 0);

    auto *preview = lv_obj_create(tile);
    Clear(preview);
    lv_obj_set_pos(preview, 10, 10);
    lv_obj_set_size(preview, tile_width - 20, tile_height - 20);
    lv_obj_set_style_radius(preview, 27, 0);
    lv_obj_set_style_bg_color(preview, Color(0x111419), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);

    auto *icon = Label(preview,
        file.video ? LV_SYMBOL_VIDEO : LV_SYMBOL_AUDIO,
        &lv_font_montserrat_48, kAccent);
    lv_obj_set_style_transform_scale(icon, landscape ? 440 : 400, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -24);

    auto *caption = lv_obj_create(tile);
    Clear(caption);
    lv_obj_set_pos(caption, 24, tile_height - 96);
    lv_obj_set_size(caption, tile_width - 48, 72);
    lv_obj_set_style_radius(caption, 22, 0);
    lv_obj_set_style_bg_color(caption, kCanvas, 0);
    lv_obj_set_style_bg_opa(caption, LV_OPA_80, 0);
    lv_obj_set_style_border_width(caption, 1, 0);
    lv_obj_set_style_border_color(caption, kMainLine, 0);
    lv_obj_set_style_border_opa(caption, LV_OPA_30, 0);
    lv_obj_remove_flag(caption, LV_OBJ_FLAG_CLICKABLE);
    auto *name = Label(caption, file.name.c_str(),
                       &lv_font_montserrat_24, kText);
    lv_obj_set_pos(name, 22, 18);
    lv_obj_set_width(name, tile_width - 92);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    OnClick(tile, [scene, path = file.path] { Open(scene, path); });
    AnimateEnter(tile, 8 + std::min<size_t>(index, 18) * 3, 6);

    if (file.video) {
      auto thumbnail = std::make_unique<MediaThumbnail>();
      thumbnail->path = file.path;
      thumbnail->row = row;
      thumbnail->preview = preview;
      thumbnail->placeholder = icon;
      scene->thumbnails.push_back(std::move(thumbnail));
    }
  }

  if (files.empty()) {
    auto *empty = Label(scene->grid,
        "No supported music or videos were found in internal storage.",
        &lv_font_montserrat_32, kMutedStrong);
    lv_obj_set_width(empty, 1050);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 280);
  }

  Navigation(screen, Action::kBackHome, callback, context, true);
  BuildViewer(scene);

  scene->worker = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, "media", "app-runtime",
                              "media");
  });
  scene->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *scene = static_cast<MediaScene *>(lv_timer_get_user_data(timer));
    if (!scene->preparation.done.load(std::memory_order_acquire)) return;
    if (!scene->preparation.verified) {
      i18n::BindLabel(scene->status, scene->preparation.error.c_str());
      return;
    }
    if (!scene->launched) {
      scene->launched = true;
      int frame = -1;
      int control = -1;
      std::string error;
      if (!scene->process.Start(scene->preparation.directory, frame, control,
                                error) ||
          !scene->session.Adopt(frame, control)) {
        i18n::BindLabel(scene->status,
            error.empty() ? scene->session.Status().c_str() : error.c_str());
        return;
      }
      i18n::BindLabel(scene->status, "PLAYER READY");
    }
    if (!scene->session.Connected()) return;
    if (scene->pending_open) SendOpen(scene);
    if (scene->session.Poll()) {
      const int width = scene->session.FrameWidth();
      const int height = scene->session.FrameHeight();
      const uint32_t bytes = media::FrameBytes(width, height);
      const uint8_t *pixels = scene->session.Pixels();
      if (!pixels || bytes > scene->frame_pixels.size()) {
        scene->session.Close();
        i18n::BindLabel(scene->status, "Media supplied an invalid frame.");
        return;
      }
      if (scene->session.FrameKind() == media::Kind::kThumbnailFrame) {
        const int loading = scene->thumbnail_loading;
        if (loading >= 0 &&
            scene->thumbnails[loading]->token == scene->session.FrameToken()) {
          auto *thumbnail = scene->thumbnails[loading].get();
          thumbnail->pixels.assign(pixels, pixels + bytes);
          scene->session.AcknowledgeFrame();
          ShowThumbnail(thumbnail, scene->thumbnail_width,
                        scene->thumbnail_height, width, height);
          scene->thumbnail_loading = -1;
        } else {
          scene->session.AcknowledgeFrame();
        }
      } else {
      // The worker owns the shared slots and can overwrite them after an ACK.
      // Keep LVGL and screenshot capture on a stable host-owned frame instead.
      memcpy(scene->frame_pixels.data(), pixels, bytes);
      scene->session.AcknowledgeFrame();
      // USER1 uses the OpenGL streaming-texture path. Rebinding and dropping
      // the variable image on every frame races its texture/cache lifetime
      // and eventually makes Scudo catch an invalid free. Bind only when the
      // source geometry changes; subsequent frames update the stable pixels.
      const bool geometry_changed = !scene->has_frame ||
          scene->descriptor.header.w != static_cast<uint32_t>(width) ||
          scene->descriptor.header.h != static_cast<uint32_t>(height);
      if (geometry_changed) {
        scene->descriptor.header.w = width;
        scene->descriptor.header.h = height;
        scene->descriptor.header.stride = width * 4;
        scene->descriptor.data_size = bytes;
        scene->descriptor.data = scene->frame_pixels.data();
      }
      if (!scene->descriptor_bound) {
        lv_image_set_src(scene->video, &scene->descriptor);
        scene->descriptor_bound = true;
      }
      lv_obj_invalidate(scene->video);
      lv_obj_remove_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(scene->empty, LV_OBJ_FLAG_HIDDEN);
      if (!scene->has_frame) ShowChrome(scene);
      scene->has_frame = true;
      }
    }
    if (scene->viewer_open) UpdateChrome(scene);
    if (scene->viewer_open && !scene->seeking) {
      const int64_t duration = scene->session.Duration();
      const int value = duration > 0
          ? std::clamp<int64_t>(
                scene->session.Position() * 10000 / duration, 0, 10000)
          : 0;
      if (value != scene->last_progress) {
        scene->last_progress = value;
        lv_slider_set_value(scene->progress, value, LV_ANIM_OFF);
      }
      UpdateTimeline(scene, scene->session.Position());
    }
    if (scene->viewer_open) SetPlayIcon(scene);
    else MaintainThumbnails(scene);
  }, 12, scene);
}
}  // namespace aeraui
