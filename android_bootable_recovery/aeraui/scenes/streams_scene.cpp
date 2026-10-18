/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include "browser/runtime.hpp"
#include "phone_keyboard.hpp"
#include "picture_decode.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "streams/launcher.hpp"
#include "streams/protocol.hpp"
#include "streams/query.hpp"
#include "streams/session.hpp"
#include "ui_components.hpp"

#include <json/json.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

struct StreamItem {
  int service = 0;
  int64_t duration = 0;
  int64_t count = 0;
  std::string type;
  std::string name;
  std::string uploader;
  std::string url;
  std::string thumbnail;
};

struct RemoteThumbnailData {
  PictureData picture;
  std::atomic<bool> ready{false};
};

struct RemoteThumbnail {
  std::string url;
  int row = 0;
  lv_obj_t* preview = nullptr;
  lv_obj_t* placeholder = nullptr;
  lv_obj_t* image = nullptr;
  std::shared_ptr<RemoteThumbnailData> data;
  lv_image_dsc_t descriptor{};
  int state = 0;
};

struct StreamChoice {
  std::string label;
  std::string detail;
  std::string url;
  bool audio = false;
};

enum class QueryKind { kNone, kBrowse, kSearch, kPlaylist, kResolve };

struct StreamsScene {
  lv_obj_t* screen = nullptr;
  lv_obj_t* services = nullptr;
  lv_obj_t* categories = nullptr;
  lv_obj_t* input = nullptr;
  lv_obj_t* search = nullptr;
  lv_obj_t* status = nullptr;
  lv_obj_t* results = nullptr;
  lv_obj_t* keyboard = nullptr;
  lv_obj_t* quality = nullptr;
  lv_obj_t* quality_list = nullptr;
  lv_obj_t* viewer = nullptr;
  lv_obj_t* video = nullptr;
  lv_obj_t* empty = nullptr;
  lv_obj_t* chrome = nullptr;
  lv_obj_t* controls = nullptr;
  lv_obj_t* play = nullptr;
  lv_obj_t* seek_back = nullptr;
  lv_obj_t* seek_forward = nullptr;
  lv_obj_t* rotate = nullptr;
  lv_obj_t* timeline = nullptr;
  lv_obj_t* time = nullptr;
  lv_obj_t* viewer_title = nullptr;
  lv_obj_t* viewer_back = nullptr;
  web::Preparation preparation;
  std::thread prepare_thread;
  std::thread query_thread;
  std::atomic<bool> query_cancel{ false };
  std::atomic<bool> query_done{ false };
  std::mutex query_mutex;
  std::string query_json;
  std::string query_error;
  QueryKind query_kind = QueryKind::kNone;
  streams::Session session;
  streams::Process process;
  lv_timer_t* timer = nullptr;
  lv_image_dsc_t descriptor{};
  std::vector<uint8_t> frame_pixels = std::vector<uint8_t>(streams::kFrameBytes);
  std::vector<StreamItem> items;
  std::vector<std::unique_ptr<RemoteThumbnail>> thumbnails;
  int loading_thumbnail = -1;
  int thumbnail_width = 0;
  int thumbnail_height = 0;
  int result_stride = 0;
  int selected_service = 0;
  std::string selected_kiosk;
  bool runtime_ready = false;
  bool player_started = false;
  bool viewer_open = false;
  bool descriptor_bound = false;
  bool has_frame = false;
  bool seeking = false;
  int last_progress = -1;
  int64_t last_second = -1;
  int64_t last_duration = -1;
  std::string selected_title;
  ActionCallback callback = nullptr;
  void* context = nullptr;
  bool chrome_visible = true;
  bool viewer_orientation_changed = false;
  uint32_t chrome_hide_at = 0;
  uint32_t orientation_settle_at = 0;

  ~StreamsScene() {
    if (timer) lv_timer_delete(timer);
    preparation.cancel.store(true);
    query_cancel.store(true);
    if (prepare_thread.joinable()) prepare_thread.join();
    if (query_thread.joinable()) query_thread.join();
    if (video && descriptor_bound) {
      lv_image_set_src(video, nullptr);
      lv_image_cache_drop(&descriptor);
    }
    for (auto& thumbnail : thumbnails) {
      if (thumbnail->data) thumbnail->data->picture.cancelled = true;
      if (thumbnail->image) lv_image_cache_drop(&thumbnail->descriptor);
    }
    if (session.Connected()) session.Send(streams::Kind::kClose);
    session.Close();
    process.Stop();
    web::RemoveRuntime(preparation.directory);
  }
};

lv_obj_t* SearchInput(lv_obj_t* parent, const char* placeholder) {
  auto* input = TextArea(parent);
  lv_textarea_set_one_line(input, true);
  lv_obj_set_size(input, 1000, 116);
  lv_textarea_set_placeholder_text(input, i18n::Translate(placeholder));
  lv_textarea_set_max_length(input, 180);
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_text_color(input, kMutedStrong, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(input, kMainBottom, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, kLineBright, 0);
  lv_obj_set_style_border_color(input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(input, 2, 0);
  lv_obj_set_style_border_width(input, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(input, 32, 0);
  lv_obj_set_style_pad_all(input, 30, 0);
  return input;
}

std::string Clock(int64_t milliseconds) {
  const int seconds = static_cast<int>(std::max<int64_t>(0, milliseconds) / 1000);
  char value[32];
  if (seconds >= 3600)
    snprintf(value, sizeof(value), "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
  else
    snprintf(value, sizeof(value), "%d:%02d", seconds / 60, seconds % 60);
  return value;
}

bool Parse(const std::string& text, Json::Value& root, std::string& error) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::istringstream stream(text);
  if (!Json::parseFromStream(builder, stream, &root, &error) || !root.isObject()) {
    error = "The stream service returned malformed data.";
    return false;
  }
  if (!root.get("ok", false).asBool()) {
    error = root.get("error", "The stream service rejected the request.").asString();
    return false;
  }
  return true;
}

bool SafeThumbnailUrl(const std::string& url) {
  return url.size() >= 12 && url.size() <= 2048 &&
         url.compare(0, 8, "https://") == 0 &&
         url.find_first_of("\r\n\0", 0, 3) == std::string::npos;
}

bool DownloadThumbnail(const std::string& url, const std::string& path) {
  if (!SafeThumbnailUrl(url)) return false;
  const pid_t child = fork();
  if (child < 0) return false;
  if (!child) {
    rlimit size{2U * 1024U * 1024U, 2U * 1024U * 1024U};
    if (setrlimit(RLIMIT_FSIZE, &size) != 0) _exit(126);
    const int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    const char* busybox = access("/sbin/busybox", X_OK) == 0
                              ? "/sbin/busybox" : "/system/bin/busybox";
    const char* arguments[] = {busybox, "wget", "-q", "-T", "8", "-t", "1",
                               "-O", path.c_str(), url.c_str(), nullptr};
    execv(busybox, const_cast<char* const*>(arguments));
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  struct stat info{};
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
         !lstat(path.c_str(), &info) && S_ISREG(info.st_mode) &&
         info.st_size > 0 && info.st_size <= 2 * 1024 * 1024;
}

void ClearThumbnails(StreamsScene* scene) {
  scene->loading_thumbnail = -1;
  for (auto& thumbnail : scene->thumbnails) {
    if (thumbnail->data) thumbnail->data->picture.cancelled = true;
    if (thumbnail->image) {
      lv_image_set_src(thumbnail->image, nullptr);
      lv_image_cache_drop(&thumbnail->descriptor);
      thumbnail->image = nullptr;
    }
  }
  scene->thumbnails.clear();
}

void ShowThumbnail(RemoteThumbnail* thumbnail) {
  if (!thumbnail->data || !thumbnail->data->picture.pixels) {
    thumbnail->state = 3;
    thumbnail->data.reset();
    return;
  }
  auto& picture = thumbnail->data->picture;
  auto& descriptor = thumbnail->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = picture.width;
  descriptor.header.h = picture.height;
  descriptor.header.stride = picture.width * 4;
  descriptor.data_size = picture.width * picture.height * 4;
  descriptor.data = picture.pixels;
  thumbnail->image = lv_image_create(thumbnail->preview);
  lv_obj_set_size(thumbnail->image, LV_PCT(100), LV_PCT(100));
  lv_image_set_src(thumbnail->image, &descriptor);
  lv_image_set_antialias(thumbnail->image, true);
  lv_image_set_inner_align(thumbnail->image, LV_IMAGE_ALIGN_COVER);
  lv_obj_center(thumbnail->image);
  lv_obj_remove_flag(thumbnail->image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(thumbnail->placeholder, LV_OBJ_FLAG_HIDDEN);
  thumbnail->state = 2;
}

bool ThumbnailNearViewport(const StreamsScene* scene,
                           const RemoteThumbnail* thumbnail) {
  const int scroll = lv_obj_get_scroll_y(scene->results);
  const int viewport = lv_obj_get_height(scene->results);
  const int top = thumbnail->row * scene->result_stride;
  const int margin = scene->result_stride;
  return top + scene->result_stride >= scroll - margin &&
         top <= scroll + viewport + margin;
}

void ThumbnailTick(StreamsScene* scene) {
  if (!scene || !scene->results) return;
  if (scene->loading_thumbnail >= 0) {
    auto* thumbnail = scene->thumbnails[scene->loading_thumbnail].get();
    if (thumbnail->data && thumbnail->data->ready.load(std::memory_order_acquire)) {
      ShowThumbnail(thumbnail);
      scene->loading_thumbnail = -1;
    }
  }
  if (scene->loading_thumbnail >= 0) return;
  for (size_t index = 0; index < scene->thumbnails.size(); ++index) {
    auto* thumbnail = scene->thumbnails[index].get();
    if (thumbnail->state != 0 || thumbnail->url.empty() ||
        !ThumbnailNearViewport(scene, thumbnail)) continue;
    thumbnail->state = 1;
    thumbnail->data = std::make_shared<RemoteThumbnailData>();
    scene->loading_thumbnail = static_cast<int>(index);
    const auto data = thumbnail->data;
    const std::string url = thumbnail->url;
    const int width = scene->thumbnail_width;
    const int height = scene->thumbnail_height;
    std::thread([data, url, width, height] {
      char path[] = "/tmp/aera-stream-thumb-XXXXXX";
      const int fd = mkstemp(path);
      if (fd >= 0) {
        close(fd);
        if (DownloadThumbnail(url, path) && !data->picture.cancelled.load())
          DecodePictureThumbnail(path, width, height, data->picture);
        unlink(path);
      }
      data->ready.store(true, std::memory_order_release);
    }).detach();
    break;
  }
}

void SetKeyboard(StreamsScene* scene, bool visible) {
  if (!scene || !scene->keyboard) return;
  if (visible) {
    lv_keyboard_set_textarea(scene->keyboard, scene->input);
    lv_obj_add_state(scene->input, LV_STATE_FOCUSED);
    lv_obj_remove_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(scene->keyboard);
  } else {
    lv_obj_add_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(scene->input, LV_STATE_FOCUSED);
  }
}

void StyleServiceButtons(StreamsScene* scene) {
  if (!scene || !scene->services) return;
  const uint32_t count = lv_obj_get_child_count(scene->services);
  for (uint32_t index = 0; index < count; ++index) {
    auto* button = lv_obj_get_child(scene->services, index);
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
    const bool selected = id == scene->selected_service;
    lv_obj_set_style_bg_color(button, selected ? kAccentSoft : kMainPanel, 0);
    lv_obj_set_style_border_width(button, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(button, selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_border_opa(button, selected ? LV_OPA_60 : LV_OPA_30, 0);
    if (lv_obj_get_child_count(button))
      lv_obj_set_style_text_color(lv_obj_get_child(button, 0), selected ? kAccent : kMutedStrong,
                                  0);
  }
}

void BeginQuery(StreamsScene* scene, QueryKind kind, std::vector<std::string> arguments) {
  if (!scene || !scene->runtime_ready ||
      (scene->query_thread.joinable() && !scene->query_done.load()))
    return;
  if (scene->query_thread.joinable()) scene->query_thread.join();
  scene->query_cancel.store(false);
  scene->query_done.store(false);
  scene->query_kind = kind;
  const char* status = "Loading stream details…";
  if (kind == QueryKind::kBrowse) status = "Loading feed…";
  else if (kind == QueryKind::kSearch) status = "Search";
  else if (kind == QueryKind::kPlaylist) status = "Loading stream details…";
  i18n::BindLabel(scene->status, status);
  lv_obj_add_state(scene->search, LV_STATE_DISABLED);
  const std::string runtime = scene->preparation.directory;
  scene->query_thread = std::thread([scene, runtime, arguments = std::move(arguments)] {
    std::string json;
    std::string error;
    streams::RunQuery(runtime, arguments, scene->query_cancel, json, error);
    {
      std::lock_guard<std::mutex> lock(scene->query_mutex);
      scene->query_json = std::move(json);
      scene->query_error = std::move(error);
    }
    scene->query_done.store(true, std::memory_order_release);
  });
}

void Browse(StreamsScene* scene, const std::string& kiosk = {}) {
  if (!scene) return;
  scene->selected_kiosk = kiosk;
  std::vector<std::string> arguments{
      "browse", std::to_string(scene->selected_service)};
  if (!kiosk.empty()) arguments.push_back(kiosk);
  BeginQuery(scene, QueryKind::kBrowse, std::move(arguments));
}

void PopulateCategories(StreamsScene* scene, const Json::Value& root) {
  if (!scene || !scene->categories) return;
  lv_obj_clean(scene->categories);
  scene->selected_kiosk = root.get("selectedKiosk", "").asString();
  const Json::Value kiosks = root["kiosks"];
  if (!kiosks.isArray() || kiosks.empty()) {
    lv_obj_add_flag(scene->categories, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(scene->categories, LV_OBJ_FLAG_HIDDEN);
  for (const auto& kiosk : kiosks) {
    const std::string id = kiosk.get("id", "").asString();
    const std::string name = kiosk.get("name", id).asString();
    if (id.empty()) continue;
    const bool selected = id == scene->selected_kiosk;
    auto* button = Button(scene->categories, name.c_str(), [scene, id] { Browse(scene, id); });
    lv_obj_set_size(button, std::clamp<int>(190 + static_cast<int>(name.size()) * 9,
                                            220, 430), 86);
    lv_obj_set_style_bg_color(button, selected ? kAccentSoft : kMainPanel, 0);
    lv_obj_set_style_border_width(button, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(button, selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_border_opa(button, selected ? LV_OPA_60 : LV_OPA_30, 0);
    if (lv_obj_get_child_count(button))
      lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                  selected ? kAccent : kMutedStrong, 0);
  }
}

void Search(StreamsScene* scene) {
  if (!scene) return;
  const std::string query = lv_textarea_get_text(scene->input);
  if (query.empty()) {
    i18n::BindLabel(scene->status, "Search");
    return;
  }
  SetKeyboard(scene, false);
  BeginQuery(scene, QueryKind::kSearch,
             { "search", std::to_string(scene->selected_service), query });
}

bool TickReached(uint32_t now, uint32_t target) {
  return target != 0 && static_cast<int32_t>(now - target) >= 0;
}

void AnimateCoordinate(lv_obj_t* object, bool horizontal, int32_t end,
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
  lv_anim_set_exec_cb(
      &animation, horizontal
                      ? [](void* target, int32_t value) {
                          lv_obj_set_x(static_cast<lv_obj_t*>(target), value);
                        }
                      : [](void* target, int32_t value) {
                          lv_obj_set_y(static_cast<lv_obj_t*>(target), value);
                        });
  lv_anim_start(&animation);
}

void ShowChrome(StreamsScene* scene) {
  if (!scene || !scene->chrome || !scene->viewer_open) return;
  const int width = lv_obj_get_width(scene->screen);
  const int height = lv_obj_get_height(scene->screen);
  AnimateCoordinate(scene->viewer_back, true, 34, 180);
  AnimateCoordinate(scene->rotate, true, width - 138, 180);
  AnimateCoordinate(scene->controls, false, height - 264, 220);
  scene->chrome_visible = true;
  scene->chrome_hide_at = lv_tick_get() + 2200;
}

void UpdateChrome(StreamsScene* scene) {
  if (!scene || !scene->chrome || !scene->viewer_open ||
      !scene->chrome_visible || scene->seeking ||
      !TickReached(lv_tick_get(), scene->chrome_hide_at)) return;
  AnimateCoordinate(scene->viewer_back, true, -138, 180);
  AnimateCoordinate(scene->rotate, true,
                    lv_obj_get_width(scene->screen) + 34, 180);
  AnimateCoordinate(scene->controls, false,
                    lv_obj_get_height(scene->screen) + 18, 220);
  scene->chrome_visible = false;
  scene->chrome_hide_at = 0;
}

void LayoutViewer(StreamsScene* scene) {
  if (!scene || !scene->viewer) return;
  const int width = lv_obj_get_width(scene->screen);
  const int height = lv_obj_get_height(scene->screen);
  lv_obj_set_pos(scene->viewer, 0, 0);
  lv_obj_set_size(scene->viewer, width, height);

  const int frame_width = std::max(1, scene->session.FrameWidth());
  const int frame_height = std::max(1, scene->session.FrameHeight());
  const double scale = std::min(static_cast<double>(width) / frame_width,
                                static_cast<double>(height) / frame_height);
  const int video_width = std::max(1, static_cast<int>(frame_width * scale));
  const int video_height = std::max(1, static_cast<int>(frame_height * scale));
  lv_obj_set_size(scene->video, video_width, video_height);
  lv_obj_center(scene->video);
  // STRETCH is safe here because the destination rectangle was calculated
  // from the frame's own aspect ratio.  It also avoids driver-dependent
  // LV_IMAGE_ALIGN_CONTAIN behaviour for mutable streaming descriptors.
  lv_image_set_inner_align(scene->video, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_width(scene->empty, std::max(240, width - 160));
  lv_obj_center(scene->empty);

  lv_obj_set_pos(scene->chrome, 0, 0);
  lv_obj_set_size(scene->chrome, width, height);
  lv_obj_set_size(scene->controls, width - 64, 230);
  lv_obj_set_x(scene->controls, 32);
  lv_obj_set_y(scene->controls,
               scene->chrome_visible ? height - 264 : height + 18);
  lv_obj_set_pos(scene->timeline, 34, 34);
  lv_obj_set_size(scene->timeline, width - 132, 18);
  lv_obj_set_pos(scene->time, 34, 78);
  lv_obj_set_pos(scene->viewer_title, 250, 78);
  lv_obj_set_width(scene->viewer_title, std::max(220, width - 534));
  const int center = (width - 64) / 2;
  lv_obj_set_pos(scene->seek_back, center - 174, 116);
  lv_obj_set_pos(scene->play, center - 52, 106);
  lv_obj_set_pos(scene->seek_forward, center + 70, 116);
  lv_obj_set_x(scene->viewer_back, scene->chrome_visible ? 34 : -138);
  lv_obj_set_x(scene->rotate,
               scene->chrome_visible ? width - 138 : width + 34);
}

void BeginOrientationRelayout(StreamsScene* scene) {
  if (!scene || !scene->viewer_open) return;
  // A mutable OpenGL image can otherwise leave its last portrait-sized draw
  // in the backing texture while LVGL rotates and resizes the live object.
  // Keep it hidden until the rotated viewport has settled; PollPlayer then
  // clears the full modal once before publishing a new fitted frame.
  lv_obj_add_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
  scene->orientation_settle_at = lv_tick_get() + 80;
  LayoutViewer(scene);
  lv_obj_invalidate(scene->viewer);
}

void CloseViewer(StreamsScene* scene) {
  if (!scene || !scene->viewer_open) return;
  if (scene->session.Connected() && scene->session.Playing())
    scene->session.Send(streams::Kind::kPause);
  scene->viewer_open = false;
  lv_obj_add_flag(scene->viewer, LV_OBJ_FLAG_HIDDEN);
  if (scene->viewer_orientation_changed && scene->callback) {
    scene->viewer_orientation_changed = false;
    scene->callback(Action::kToggleVideoRotation, scene->context);
  }
}

void UpdatePlay(StreamsScene* scene) {
  if (!scene || !scene->play || !lv_obj_get_child_count(scene->play)) return;
  lv_label_set_text_static(lv_obj_get_child(scene->play, 0),
                           scene->session.Playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void UpdateTime(StreamsScene* scene, int64_t position) {
  const int64_t second = std::max<int64_t>(0, position) / 1000;
  const int64_t duration = std::max<int64_t>(0, scene->session.Duration()) / 1000;
  if (second == scene->last_second && duration == scene->last_duration) return;
  scene->last_second = second;
  scene->last_duration = duration;
  const std::string value = Clock(position) + "  /  " + Clock(scene->session.Duration());
  lv_label_set_text(scene->time, value.c_str());
}

void OpenPlayer(StreamsScene* scene, const StreamChoice& choice) {
  if (!scene || choice.url.empty() || !scene->session.Connected()) return;
  if (scene->quality) lv_obj_add_flag(scene->quality, LV_OBJ_FLAG_HIDDEN);
  scene->viewer_open = true;
  scene->has_frame = false;
  scene->orientation_settle_at = 0;
  scene->last_progress = -1;
  scene->last_second = -1;
  scene->last_duration = -1;
  i18n::BindLabel(scene->viewer_title, scene->selected_title.c_str());
  i18n::BindLabel(scene->empty,
                  choice.audio
                      ? LV_SYMBOL_AUDIO "\nPreparing player…"
                      : LV_SYMBOL_VIDEO "\nOpening video…");
  lv_obj_remove_flag(scene->empty, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
  lv_slider_set_value(scene->timeline, 0, LV_ANIM_OFF);
  lv_obj_remove_flag(scene->viewer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(scene->viewer);
  LayoutViewer(scene);
  ShowChrome(scene);
  scene->session.Send(streams::Kind::kOpen, choice.url.c_str());
}

void ShowChoices(StreamsScene* scene, const Json::Value& root) {
  std::vector<StreamChoice> choices;
  const std::string hls = root.get("hls", "").asString();
  if (!hls.empty())
    choices.push_back({i18n::Translate("Auto"),
                       i18n::Translate("Adaptive quality"), hls, false});
  const Json::Value streams_json = root["streams"];
  if (streams_json.isArray()) {
    for (const auto& stream : streams_json) {
      const std::string kind = stream.get("kind", "").asString();
      const std::string url = stream.get("url", "").asString();
      if (url.empty() || (kind != "video" && kind != "video-only" && kind != "audio"))
        continue;
      std::string label = stream.get("label", kind == "audio" ? "Audio" : "Video").asString();
      std::string detail = kind == "audio" ? i18n::Translate("Audio")
                                             : stream.get("format", "Video").asString();
      if (kind == "video-only")
        detail += std::string("  ·  ") + i18n::Translate("Video only");
      if (kind != "audio" && stream.get("fps", 0).asInt() > 30)
        detail += "  ·  " + std::to_string(stream["fps"].asInt()) + " FPS";
      choices.push_back(
          {std::move(label), std::move(detail), url, kind == "audio"});
      if (choices.size() >= 10) break;
    }
  }
  if (choices.empty()) {
    i18n::BindLabel(scene->status, "No streams");
    return;
  }
  scene->selected_title = root.get("name", "AERA Streams").asString();
  lv_obj_clean(scene->quality_list);
  int y = 0;
  for (const auto& choice : choices) {
    Row(scene->quality_list, y,
        choice.audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_VIDEO,
        choice.label, choice.detail, [scene, choice] { OpenPlayer(scene, choice); });
    y += 174;
  }
  lv_obj_remove_flag(scene->quality, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(scene->quality);
}

void Resolve(StreamsScene* scene, const StreamItem& item) {
  scene->selected_title = item.name;
  BeginQuery(scene, QueryKind::kResolve, { "resolve", item.url });
}

void OpenItem(StreamsScene* scene, const StreamItem& item) {
  if (item.type == "stream") {
    Resolve(scene, item);
  } else if (item.type == "playlist") {
    BeginQuery(scene, QueryKind::kPlaylist, { "playlist", item.url });
  } else {
    i18n::BindLabel(scene->status, "No results");
  }
}

void PopulateResults(StreamsScene* scene, const Json::Value& root) {
  scene->items.clear();
  ClearThumbnails(scene);
  lv_obj_clean(scene->results);
  const Json::Value items = root["items"];
  if (items.isArray()) {
    for (const auto& item : items) {
      const std::string type = item.get("type", "").asString();
      if (type != "stream" && type != "playlist" && type != "channel") continue;
      StreamItem parsed;
      parsed.type = type;
      parsed.service = item.get("serviceId", 0).asInt();
      parsed.duration = item.get("duration", 0).asInt64();
      parsed.count = item.get("streamCount", 0).asInt64();
      parsed.name = item.get("name", "Untitled").asString();
      parsed.uploader = item.get("uploader", "").asString();
      parsed.url = item.get("url", "").asString();
      parsed.thumbnail = item.get("thumbnail", "").asString();
      if (!parsed.url.empty()) scene->items.push_back(std::move(parsed));
    }
  }
  const int width = std::max(600, static_cast<int>(lv_obj_get_width(scene->results)));
  scene->result_stride = 246;
  scene->thumbnail_width = 316;
  scene->thumbnail_height = 178;
  for (size_t index = 0; index < scene->items.size(); ++index) {
    const StreamItem item = scene->items[index];
    auto* card = lv_button_create(scene->results);
    Panel(card, 34, kMainPanel);
    Interactive(card, kMainSelected);
    lv_obj_set_pos(card, 0, static_cast<int>(index) * scene->result_stride);
    lv_obj_set_size(card, width, 230);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, kMainLine, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_30, 0);

    auto* preview = lv_obj_create(card);
    Clear(preview);
    lv_obj_set_pos(preview, 22, 26);
    lv_obj_set_size(preview, scene->thumbnail_width, scene->thumbnail_height);
    lv_obj_set_style_radius(preview, 25, 0);
    lv_obj_set_style_bg_color(preview, kMainSheet, 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    const char* symbol = item.type == "playlist" ? LV_SYMBOL_LIST :
                         item.type == "channel" ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_PLAY;
    auto* placeholder = Label(preview, symbol, &lv_font_montserrat_48, kAccent);
    lv_obj_set_style_transform_scale(placeholder, 300, 0);
    lv_obj_center(placeholder);

    auto* title = Label(card, item.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(title, 374, 38);
    lv_obj_set_width(title, width - 500);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    std::string detail = item.uploader.empty()
                             ? i18n::Translate(item.type == "playlist"
                                                   ? "Playlists"
                                                   : item.type == "channel"
                                                         ? "Channels"
                                                         : "Videos")
                             : item.uploader;
    if (item.type == "stream" && item.duration > 0)
      detail += "  ·  " + Clock(item.duration * 1000);
    if (item.type == "playlist" && item.count > 0)
      detail += "  ·  " + std::to_string(item.count) + " " +
                i18n::Translate("Tracks");
    auto* copy = Label(card, detail.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(copy, 374, 108);
    lv_obj_set_width(copy, width - 500);
    lv_label_set_long_mode(copy, LV_LABEL_LONG_DOT);
    auto* kind = Kicker(card, item.type == "playlist" ? "Playlists" :
                              item.type == "channel" ? "Channels" : "Play",
                        item.type == "channel" ? kMutedStrong : kAccent);
    lv_obj_set_pos(kind, 374, 157);
    auto* open = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kAccent);
    lv_obj_align(open, LV_ALIGN_RIGHT_MID, -42, 0);
    OnClick(card, [scene, item] { OpenItem(scene, item); });
    AnimateEnter(card, 6 + std::min<size_t>(index, 14) * 4, 8);

    auto thumbnail = std::make_unique<RemoteThumbnail>();
    thumbnail->url = item.thumbnail;
    thumbnail->row = static_cast<int>(index);
    thumbnail->preview = preview;
    thumbnail->placeholder = placeholder;
    scene->thumbnails.push_back(std::move(thumbnail));
  }
  if (scene->items.empty()) {
    auto* empty = Label(scene->results, "No results",
                        &lv_font_montserrat_32, kMutedStrong);
    lv_obj_set_width(empty, width - 120);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 160);
  }
  const std::string title = root.get("title", "Videos").asString();
  const std::string summary =
      std::string(i18n::Translate(title.c_str())) + "  ·  " +
      i18n::Format("%zu items", scene->items.size());
  i18n::BindLabel(scene->status, summary.c_str());
}

void FinishQuery(StreamsScene* scene) {
  if (!scene || !scene->query_done.exchange(false)) return;
  if (scene->query_thread.joinable()) scene->query_thread.join();
  std::string json;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(scene->query_mutex);
    json.swap(scene->query_json);
    error.swap(scene->query_error);
  }
  lv_obj_remove_state(scene->search, LV_STATE_DISABLED);
  if (!error.empty()) {
    i18n::BindLabel(scene->status, error.c_str());
    scene->query_kind = QueryKind::kNone;
    return;
  }
  Json::Value root;
  if (!Parse(json, root, error)) {
    i18n::BindLabel(scene->status, error.c_str());
  } else if (scene->query_kind == QueryKind::kBrowse) {
    PopulateCategories(scene, root);
    PopulateResults(scene, root);
  } else if (scene->query_kind == QueryKind::kSearch ||
             scene->query_kind == QueryKind::kPlaylist) {
    PopulateResults(scene, root);
  } else if (scene->query_kind == QueryKind::kResolve) {
    ShowChoices(scene, root);
  }
  scene->query_kind = QueryKind::kNone;
}

void BuildViewer(StreamsScene* scene) {
  scene->viewer = lv_obj_create(scene->screen);
  Clear(scene->viewer);
  lv_obj_set_user_data(scene->viewer, &kPersistentModalMarker);
  lv_obj_set_size(scene->viewer, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(scene->viewer, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scene->viewer, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(
      scene->viewer,
      [](lv_event_t* event) {
        if (lv_event_get_code(event) == LV_EVENT_CANCEL)
          CloseViewer(static_cast<StreamsScene*>(lv_event_get_user_data(event)));
      },
      LV_EVENT_CANCEL, scene);

  scene->empty = Label(scene->viewer, LV_SYMBOL_VIDEO "\nOpening video…", &lv_font_montserrat_40,
                       kMutedStrong);
  lv_obj_set_style_text_align(scene->empty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(scene->empty, 28, 0);
  lv_obj_center(scene->empty);

  scene->video = lv_image_create(scene->viewer);
  lv_obj_add_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
  scene->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  scene->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  scene->descriptor.header.flags = LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;
  scene->descriptor.header.w = streams::kLandscapeWidth;
  scene->descriptor.header.h = streams::kLandscapeHeight;
  scene->descriptor.header.stride = streams::kLandscapeWidth * 4;
  scene->descriptor.data_size = streams::kFrameBytes;
  scene->descriptor.data = scene->frame_pixels.data();

  auto reveal = [](lv_event_t* event) {
    ShowChrome(static_cast<StreamsScene*>(lv_event_get_user_data(event)));
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
  lv_obj_set_style_radius(scene->controls, 46, 0);
  lv_obj_set_style_bg_color(scene->controls, Color(0x090b0f), 0);
  lv_obj_set_style_bg_opa(scene->controls, 225, 0);
  lv_obj_set_style_border_width(scene->controls, 1, 0);
  lv_obj_set_style_border_color(scene->controls, Color(0xffffff), 0);
  lv_obj_set_style_border_opa(scene->controls, LV_OPA_20, 0);

  scene->play = Button(scene->controls, LV_SYMBOL_PLAY, [scene] {
    if (!scene->session.Connected()) return;
    scene->session.Send(scene->session.Playing() ? streams::Kind::kPause
                                                 : streams::Kind::kPlay);
    ShowChrome(scene);
  }, true);
  lv_obj_set_size(scene->play, 104, 104);
  lv_obj_set_style_radius(scene->play, 52, 0);

  auto seek = [scene](int64_t offset) {
    if (!scene->session.Connected()) return;
    const int64_t duration = scene->session.Duration();
    const int64_t unbounded =
        std::max<int64_t>(0, scene->session.Position() + offset);
    const int64_t target = std::clamp<int64_t>(
        unbounded, 0, duration > 0 ? duration : unbounded);
    scene->session.Send(streams::Kind::kSeekAbsolute, nullptr, target);
    ShowChrome(scene);
  };
  scene->seek_back = Button(scene->controls, "-10", [seek] { seek(-10000); });
  scene->seek_forward = Button(scene->controls, "+10", [seek] { seek(10000); });
  for (auto* button : {scene->seek_back, scene->seek_forward}) {
    lv_obj_set_size(button, 94, 84);
    lv_obj_set_style_radius(button, 42, 0);
  }

  scene->timeline = lv_slider_create(scene->controls);
  RangeSlider(scene->timeline);
  lv_slider_set_range(scene->timeline, 0, 10000);
  lv_obj_set_style_pad_all(scene->timeline, 15, LV_PART_KNOB);
  lv_obj_add_event_cb(
      scene->timeline,
      [](lv_event_t* event) {
        auto* scene = static_cast<StreamsScene*>(lv_event_get_user_data(event));
        const auto code = lv_event_get_code(event);
        if (code == LV_EVENT_PRESSED) {
          scene->seeking = true;
          ShowChrome(scene);
        }
        if (code == LV_EVENT_VALUE_CHANGED && scene->seeking) {
          const int64_t target =
              scene->session.Duration() > 0
                  ? scene->session.Duration() * lv_slider_get_value(scene->timeline) / 10000
                  : 0;
          UpdateTime(scene, target);
        }
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
          if (!scene->seeking) return;
          const int64_t target =
              scene->session.Duration() > 0
                  ? scene->session.Duration() * lv_slider_get_value(scene->timeline) / 10000
                  : 0;
          scene->session.Send(streams::Kind::kSeekAbsolute, nullptr, target);
          scene->seeking = false;
          ShowChrome(scene);
        }
      },
      LV_EVENT_ALL, scene);

  scene->time = Label(scene->controls, "0:00  /  0:00", &lv_font_montserrat_24, Color(0xe8ecf2));
  scene->viewer_title =
      Label(scene->controls, "AERA Streams", &lv_font_montserrat_24, Color(0xb8bec6));
  lv_label_set_long_mode(scene->viewer_title, LV_LABEL_LONG_DOT);

  scene->viewer_back = Button(scene->chrome, LV_SYMBOL_LEFT, [scene] { CloseViewer(scene); });
  lv_obj_set_pos(scene->viewer_back, 34, 34);
  lv_obj_set_size(scene->viewer_back, 104, 104);
  lv_obj_set_style_radius(scene->viewer_back, 52, 0);
  scene->rotate = Button(scene->chrome, LV_SYMBOL_REFRESH, [scene] {
    if (!scene->callback) return;
    scene->viewer_orientation_changed = !scene->viewer_orientation_changed;
    scene->callback(Action::kToggleVideoRotation, scene->context);
    ShowChrome(scene);
  });
  lv_obj_set_size(scene->rotate, 104, 104);
  lv_obj_set_style_radius(scene->rotate, 52, 0);

  LayoutViewer(scene);
  lv_obj_add_event_cb(scene->screen, [](lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_SIZE_CHANGED)
      BeginOrientationRelayout(
          static_cast<StreamsScene*>(lv_event_get_user_data(event)));
  }, LV_EVENT_SIZE_CHANGED, scene);
  lv_obj_add_flag(scene->viewer, LV_OBJ_FLAG_HIDDEN);
}

void BuildQuality(StreamsScene* scene) {
  scene->quality = lv_obj_create(scene->screen);
  Clear(scene->quality);
  lv_obj_set_user_data(scene->quality, &kPersistentModalMarker);
  lv_obj_set_size(scene->quality, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(scene->quality, Color(0x020305), 0);
  lv_obj_set_style_bg_opa(scene->quality, LV_OPA_80, 0);
  lv_obj_add_event_cb(
      scene->quality,
      [](lv_event_t* event) {
        if (lv_event_get_code(event) == LV_EVENT_CANCEL)
          lv_obj_add_flag(static_cast<StreamsScene*>(lv_event_get_user_data(event))->quality,
                          LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CANCEL, scene);
  auto* sheet = lv_obj_create(scene->quality);
  Panel(sheet, 46, kMainSheet);
  lv_obj_set_size(sheet, std::min(1320, lv_obj_get_width(scene->screen) - 64),
                  std::min(1740, lv_obj_get_height(scene->screen) - 180));
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -32);
  // The quality sheet is built before LVGL's next layout pass. Resolve its
  // dimensions now so the description and scroll viewport do not inherit a
  // stale zero width/height and hide every generated quality row.
  lv_obj_update_layout(sheet);
  auto* close = Button(sheet, LV_SYMBOL_CLOSE,
                       [scene] { lv_obj_add_flag(scene->quality, LV_OBJ_FLAG_HIDDEN); });
  lv_obj_set_pos(close, 30, 30);
  lv_obj_set_size(close, 104, 104);
  auto* title = Label(sheet, "Choose quality", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 166, 40);
  auto* copy = Label(sheet, "Auto adapts to the connection. Direct options keep a fixed quality.",
                     &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 50, 164);
  lv_obj_set_width(copy, lv_obj_get_width(sheet) - 100);
  scene->quality_list = lv_obj_create(sheet);
  Clear(scene->quality_list);
  lv_obj_set_pos(scene->quality_list, 32, 238);
  lv_obj_set_size(scene->quality_list, lv_obj_get_width(sheet) - 64,
                  lv_obj_get_height(sheet) - 270);
  lv_obj_add_flag(scene->quality_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->quality_list, LV_DIR_VER);
  lv_obj_add_flag(scene->quality, LV_OBJ_FLAG_HIDDEN);
}

void PollPlayer(StreamsScene* scene) {
  if (!scene->session.Connected()) return;
  if (scene->orientation_settle_at &&
      TickReached(lv_tick_get(), scene->orientation_settle_at)) {
    scene->orientation_settle_at = 0;
    LayoutViewer(scene);
    lv_obj_invalidate(scene->viewer);
    // Finish the all-black modal redraw before the next mutable video frame
    // is exposed.  This removes the old portrait rectangle from the GPU
    // display texture instead of blending it with the landscape placement.
    lv_refr_now(lv_display_get_default());
  }
  if (scene->session.Poll()) {
    const int width = scene->session.FrameWidth();
    const int height = scene->session.FrameHeight();
    const uint32_t bytes = streams::FrameBytes(width, height);
    const uint8_t* pixels = scene->session.Pixels();
    if (!pixels || bytes > scene->frame_pixels.size()) {
      scene->session.Close();
      return;
    }
    memcpy(scene->frame_pixels.data(), pixels, bytes);
    scene->session.AcknowledgeFrame();
    const bool geometry = !scene->has_frame ||
                          scene->descriptor.header.w != static_cast<uint32_t>(width) ||
                          scene->descriptor.header.h != static_cast<uint32_t>(height);
    if (geometry) {
      scene->descriptor.header.w = width;
      scene->descriptor.header.h = height;
      scene->descriptor.header.stride = width * 4;
      scene->descriptor.data_size = bytes;
      LayoutViewer(scene);
    }
    if (!scene->descriptor_bound) {
      lv_image_set_src(scene->video, &scene->descriptor);
      scene->descriptor_bound = true;
    }
    lv_obj_invalidate(scene->video);
    if (!scene->orientation_settle_at) {
      lv_obj_remove_flag(scene->video, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(scene->empty, LV_OBJ_FLAG_HIDDEN);
    }
    scene->has_frame = true;
  }
  if (!scene->viewer_open) return;
  UpdatePlay(scene);
  if (!scene->seeking) {
    const int64_t duration = scene->session.Duration();
    const int value =
        duration > 0 ? std::clamp<int64_t>(scene->session.Position() * 10000 / duration, 0, 10000)
                     : 0;
    if (value != scene->last_progress) {
      scene->last_progress = value;
      lv_slider_set_value(scene->timeline, value, LV_ANIM_OFF);
    }
    UpdateTime(scene, scene->session.Position());
  }
  UpdateChrome(scene);
}

}  // namespace

void BuildStreamsScene(lv_obj_t* screen, ActionCallback callback, void* context) {
  auto* scene = new StreamsScene;
  scene->screen = screen;
  scene->callback = callback;
  scene->context = context;
  lv_obj_add_event_cb(
      screen,
      [](lv_event_t* event) { delete static_cast<StreamsScene*>(lv_event_get_user_data(event)); },
      LV_EVENT_DELETE, scene);

  Header(screen, "AERA Streams", "Browse videos and music from open streaming services.", callback,
         context);
  const bool landscape = Landscape(screen);
  const int content_y = landscape ? 310 : 430;

  scene->services = lv_obj_create(screen);
  Clear(scene->services);
  lv_obj_set_pos(scene->services, 64, content_y);
  lv_obj_set_size(scene->services, lv_obj_get_width(screen) - 128, 112);
  lv_obj_set_flex_flow(scene->services, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(scene->services, 16, 0);
  lv_obj_add_flag(scene->services, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->services, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(scene->services, LV_SCROLLBAR_MODE_OFF);
  struct Service {
    int id;
    const char* name;
  };
  constexpr Service services[] = { { 0, "YouTube" },
                                   { 1, "SoundCloud" },
                                   { 3, "PeerTube" },
                                   { 4, "Bandcamp" },
                                   { 2, "media.ccc.de" } };
  for (const auto& service : services) {
    auto* button = Button(scene->services, service.name, [scene, service] {
      scene->selected_service = service.id;
      StyleServiceButtons(scene);
      Browse(scene);
    });
    lv_obj_set_size(button, 264, 94);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(service.id)));
  }
  StyleServiceButtons(scene);

  scene->categories = lv_obj_create(screen);
  Clear(scene->categories);
  lv_obj_set_pos(scene->categories, 64, content_y + 122);
  lv_obj_set_size(scene->categories, lv_obj_get_width(screen) - 128, 98);
  lv_obj_set_flex_flow(scene->categories, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(scene->categories, 14, 0);
  lv_obj_add_flag(scene->categories, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->categories, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(scene->categories, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(scene->categories, LV_OBJ_FLAG_HIDDEN);

  scene->input = SearchInput(screen, "Search this service");
  lv_obj_set_pos(scene->input, 64, content_y + 236);
  lv_obj_set_width(scene->input, lv_obj_get_width(screen) - 390);
  lv_textarea_set_one_line(scene->input, true);
  scene->search = Button(screen, "Search", [scene] { Search(scene); }, true);
  lv_obj_set_pos(scene->search, lv_obj_get_width(screen) - 306, content_y + 236);
  lv_obj_set_size(scene->search, 242, 116);
  lv_obj_add_state(scene->search, LV_STATE_DISABLED);

  scene->status = Kicker(screen, "Preparing", kMutedStrong);
  lv_obj_set_pos(scene->status, 80, content_y + 374);
  scene->results =
      Scroll(screen, content_y + 426,
             lv_obj_get_height(screen) - (content_y + 426) - 28);

  scene->keyboard = lv_keyboard_create(screen);
  phone_keyboard::Apply(scene->keyboard);
  // LVGL keyboards default to a bottom alignment. Streams uses explicit
  // screen coordinates, so without resetting the alignment its portrait Y
  // offset is applied below the display and the keyboard is never visible.
  lv_obj_set_align(scene->keyboard, LV_ALIGN_TOP_LEFT);
  if (landscape) {
    lv_obj_set_pos(scene->keyboard, lv_obj_get_width(screen) / 2, 300);
    lv_obj_set_size(scene->keyboard, lv_obj_get_width(screen) / 2, 900);
  } else {
    constexpr int height = 1000;
    lv_obj_set_pos(scene->keyboard, 0, lv_obj_get_height(screen) - height);
    lv_obj_set_size(scene->keyboard, lv_obj_get_width(screen), height);
  }
  lv_keyboard_set_textarea(scene->keyboard, scene->input);
  lv_obj_set_user_data(scene->keyboard, &kPersistentModalMarker);
  lv_obj_add_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(
      scene->input,
      [](lv_event_t* event) {
        SetKeyboard(static_cast<StreamsScene*>(lv_event_get_user_data(event)), true);
      },
      LV_EVENT_CLICKED, scene);
  lv_obj_add_event_cb(
      scene->keyboard,
      [](lv_event_t *event) {
        if (lv_event_get_code(event) == LV_EVENT_CANCEL)
          SetKeyboard(static_cast<StreamsScene *>(lv_event_get_user_data(event)),
                      false);
      },
      LV_EVENT_CANCEL, scene);
  lv_obj_add_event_cb(
      scene->keyboard,
      [](lv_event_t* event) {
        auto* scene = static_cast<StreamsScene*>(lv_event_get_user_data(event));
        if (lv_event_get_code(event) == LV_EVENT_READY)
          Search(scene);
        else if (lv_event_get_code(event) == LV_EVENT_CANCEL)
          SetKeyboard(scene, false);
      },
      LV_EVENT_ALL, scene);

  Navigation(screen, Action::kBackHome, callback, context, true);
  BuildQuality(scene);
  BuildViewer(scene);

  scene->prepare_thread = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, "streams", "app-runtime", "streams");
  });
  scene->timer = lv_timer_create(
      [](lv_timer_t* timer) {
        auto* scene = static_cast<StreamsScene*>(lv_timer_get_user_data(timer));
        if (scene->preparation.done.load(std::memory_order_acquire) && !scene->runtime_ready) {
          if (scene->prepare_thread.joinable()) scene->prepare_thread.join();
          if (!scene->preparation.verified) {
            i18n::BindLabel(scene->status,
                            scene->preparation.error.c_str());
            return;
          }
          scene->runtime_ready = true;
          int frame = -1;
          int control = -1;
          std::string error;
          if (!scene->process.Start(scene->preparation.directory, frame, control, error) ||
              !scene->session.Adopt(frame, control)) {
            i18n::BindLabel(
                scene->status,
                error.empty() ? scene->session.Status().c_str()
                              : error.c_str());
            return;
          }
          scene->player_started = true;
          lv_obj_remove_state(scene->search, LV_STATE_DISABLED);
          Browse(scene);
        }
        FinishQuery(scene);
        ThumbnailTick(scene);
        PollPlayer(scene);
      },
      12, scene);
}

}  // namespace aeraui
