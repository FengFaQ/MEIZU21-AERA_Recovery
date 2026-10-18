/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "picture_decode.hpp"
#include "picture_viewer.hpp"
#include "ui_components.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace aeraui {
namespace {
using namespace widgets;

struct GalleryEntry {
  std::string path;
  std::string name;
};

struct Thumbnail {
  std::string path;
  int row = 0;
  lv_obj_t *preview = nullptr;
  lv_obj_t *placeholder = nullptr;
  lv_obj_t *image = nullptr;
  std::shared_ptr<PictureData> data;
  lv_image_dsc_t descriptor{};
  int state = 0;  // dormant, decoding, visible, or unsupported.
};

struct GalleryState {
  lv_obj_t *grid = nullptr;
  lv_timer_t *timer = nullptr;
  std::vector<std::unique_ptr<Thumbnail>> thumbnails;
  int loading = -1;
  int preview_width = 0;
  int preview_height = 0;
  int row_stride = 0;
};

void ScanPictures(const std::string &path, int depth,
                  std::vector<GalleryEntry> &out) {
  if (depth > 5 || out.size() >= 300) return;
  DIR *directory = opendir(path.c_str());
  if (!directory) return;
  while (auto *entry = readdir(directory)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        entry->d_name[0] == '.') continue;
    const std::string child = path + "/" + entry->d_name;
    struct stat info{};
    if (lstat(child.c_str(), &info) || S_ISLNK(info.st_mode)) continue;
    if (S_ISDIR(info.st_mode)) ScanPictures(child, depth + 1, out);
    else if (S_ISREG(info.st_mode) && IsPicture(child))
      out.push_back({child, entry->d_name});
    if (out.size() >= 300) break;
  }
  closedir(directory);
}

void DropThumbnail(Thumbnail *thumbnail) {
  if (thumbnail->image != nullptr) {
    lv_obj_delete(thumbnail->image);
    thumbnail->image = nullptr;
    lv_image_cache_drop(&thumbnail->descriptor);
  }
  thumbnail->descriptor = {};
  thumbnail->data.reset();
  thumbnail->state = 0;
  if (thumbnail->placeholder != nullptr)
    lv_obj_remove_flag(thumbnail->placeholder, LV_OBJ_FLAG_HIDDEN);
}

void ShowThumbnail(Thumbnail *thumbnail, int width, int height) {
  if (!thumbnail->data || !thumbnail->data->pixels) {
    thumbnail->state = 3;
    thumbnail->data.reset();
    return;
  }
  auto &descriptor = thumbnail->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = thumbnail->data->width;
  descriptor.header.h = thumbnail->data->height;
  descriptor.header.stride = thumbnail->data->width * 4;
  descriptor.data_size = thumbnail->data->width * thumbnail->data->height * 4;
  descriptor.data = thumbnail->data->pixels;

  thumbnail->image = lv_image_create(thumbnail->preview);
  lv_image_set_src(thumbnail->image, &descriptor);
  lv_image_set_antialias(thumbnail->image, true);
  const uint32_t scale = std::max(
      (static_cast<uint32_t>(width) * 256 + thumbnail->data->width - 1) /
          thumbnail->data->width,
      (static_cast<uint32_t>(height) * 256 + thumbnail->data->height - 1) /
          thumbnail->data->height);
  lv_image_set_scale(thumbnail->image, scale);
  lv_obj_center(thumbnail->image);
  lv_obj_remove_flag(thumbnail->image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(thumbnail->image, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(thumbnail->placeholder, LV_OBJ_FLAG_HIDDEN);
  thumbnail->state = 2;

  lv_anim_t fade;
  lv_anim_init(&fade);
  lv_anim_set_var(&fade, thumbnail->image);
  lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_duration(&fade, 220);
  lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&fade, [](void *object, int32_t opacity) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), opacity, 0);
  });
  lv_anim_start(&fade);
}

bool NearViewport(const GalleryState *state, const Thumbnail *thumbnail,
                  int margin) {
  const int scroll = lv_obj_get_scroll_y(state->grid);
  const int viewport = lv_obj_get_height(state->grid);
  const int top = thumbnail->row * state->row_stride;
  const int bottom = top + state->row_stride;
  return bottom >= scroll - margin && top <= scroll + viewport + margin;
}

void GalleryTick(lv_timer_t *timer) {
  auto *state = static_cast<GalleryState *>(lv_timer_get_user_data(timer));
  if (state == nullptr || state->grid == nullptr) return;

  if (state->loading >= 0) {
    auto *thumbnail = state->thumbnails[state->loading].get();
    if (thumbnail->data &&
        thumbnail->data->ready.load(std::memory_order_acquire)) {
      ShowThumbnail(thumbnail, state->preview_width, state->preview_height);
      state->loading = -1;
    }
  }

  // Keep one viewport above and below warm. Everything else is released so a
  // folder with hundreds of photos never becomes hundreds of framebuffers.
  const int keep_margin = lv_obj_get_height(state->grid);
  for (auto &entry : state->thumbnails) {
    if (entry->state == 2 && !NearViewport(state, entry.get(), keep_margin))
      DropThumbnail(entry.get());
  }

  if (state->loading >= 0) return;
  const int preload_margin = state->row_stride;
  const int center = lv_obj_get_scroll_y(state->grid) +
      lv_obj_get_height(state->grid) / 2;
  int best = -1;
  int best_distance = 0x7fffffff;
  for (size_t index = 0; index < state->thumbnails.size(); ++index) {
    auto *thumbnail = state->thumbnails[index].get();
    if (thumbnail->state != 0 ||
        !NearViewport(state, thumbnail, preload_margin)) continue;
    const int distance = std::abs(
        thumbnail->row * state->row_stride + state->row_stride / 2 - center);
    if (distance < best_distance) {
      best = static_cast<int>(index);
      best_distance = distance;
    }
  }
  if (best < 0) return;

  auto *thumbnail = state->thumbnails[best].get();
  thumbnail->state = 1;
  thumbnail->data = std::make_shared<PictureData>();
  state->loading = best;
  std::thread([data = thumbnail->data, path = thumbnail->path,
               width = state->preview_width,
               height = state->preview_height] {
    DecodePictureThumbnail(path, width, height, *data);
  }).detach();
}

void DeleteGallery(lv_event_t *event) {
  auto *state = static_cast<GalleryState *>(lv_event_get_user_data(event));
  if (state == nullptr) return;
  if (state->timer != nullptr) lv_timer_delete(state->timer);
  for (auto &thumbnail : state->thumbnails) {
    if (thumbnail->data) thumbnail->data->cancelled = true;
    if (thumbnail->image != nullptr) {
      lv_obj_delete(thumbnail->image);
      thumbnail->image = nullptr;
      lv_image_cache_drop(&thumbnail->descriptor);
    }
  }
  delete state;
}
}  // namespace

void BuildGalleryScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  Header(screen, "Gallery", "Your pictures, ready to browse.", callback, context);
  const bool landscape = Landscape(screen);
  std::vector<GalleryEntry> pictures;
  for (const char *root : {"/sdcard/DCIM", "/sdcard/Pictures", "/sdcard/Download",
                           "/sdcard/AERA/screenshots"})
    ScanPictures(root, 0, pictures);
  std::sort(pictures.begin(), pictures.end(), [](const auto &a, const auto &b) {
    return a.path > b.path;
  });

  const std::string count = pictures.size() == 1
      ? i18n::Format("%zu PHOTO", pictures.size())
      : i18n::Format("%zu PHOTOS", pictures.size());
  auto *badge = Kicker(screen, count.c_str(), pictures.empty() ? kMuted : kAccent);
  lv_obj_set_pos(badge, 80, landscape ? 306 : 426);

  auto *state = new GalleryState;
  state->grid = Scroll(screen, landscape ? 350 : 500,
      lv_obj_get_height(screen) - (landscape ? 350 : 500) - 28);
  state->row_stride = landscape ? 356 : 386;
  const int columns = landscape ? 4 : 2;
  const int tile_width = landscape ? 748 : 640;
  const int tile_height = landscape ? 340 : 370;
  state->preview_width = tile_width - 20;
  state->preview_height = tile_height - 20;
  state->thumbnails.reserve(pictures.size());

  for (size_t index = 0; index < pictures.size(); ++index) {
    const auto &picture = pictures[index];
    const int column = static_cast<int>(index) % columns;
    const int row = static_cast<int>(index) / columns;
    auto *tile = lv_button_create(state->grid);
    Panel(tile, 34, kMainPanel);
    Interactive(tile, kMainSelected);
    lv_obj_set_pos(tile, column * (landscape ? 764 : 664),
                   row * state->row_stride);
    lv_obj_set_size(tile, tile_width, tile_height);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, kMainLine, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_30, 0);

    auto *preview = lv_obj_create(tile);
    Clear(preview);
    lv_obj_set_pos(preview, 10, 10);
    lv_obj_set_size(preview, state->preview_width, state->preview_height);
    lv_obj_set_style_radius(preview, 27, 0);
    lv_obj_set_style_bg_color(preview, kMainSheet, 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);

    auto *placeholder = Label(preview, LV_SYMBOL_IMAGE,
                              &lv_font_montserrat_48, kMuted);
    lv_obj_set_style_transform_scale(placeholder, 360, 0);
    lv_obj_center(placeholder);

    auto *caption = lv_obj_create(tile);
    Clear(caption);
    lv_obj_set_pos(caption, 24, tile_height - 94);
    lv_obj_set_size(caption, tile_width - 48, 70);
    lv_obj_set_style_radius(caption, 22, 0);
    lv_obj_set_style_bg_color(caption, kCanvas, 0);
    lv_obj_set_style_bg_opa(caption, LV_OPA_80, 0);
    lv_obj_set_style_border_width(caption, 1, 0);
    lv_obj_set_style_border_color(caption, kMainLine, 0);
    lv_obj_set_style_border_opa(caption, LV_OPA_30, 0);
    lv_obj_remove_flag(caption, LV_OBJ_FLAG_CLICKABLE);
    auto *name = Label(caption, picture.name.c_str(),
                       &lv_font_montserrat_24, kText);
    lv_obj_set_pos(name, 22, 18);
    lv_obj_set_width(name, tile_width - 92);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    OnClick(tile, [screen, path = picture.path] { OpenPicture(screen, path); });
    AnimateEnter(tile, 8 + std::min<size_t>(index, 18) * 3, 6);

    auto thumbnail = std::make_unique<Thumbnail>();
    thumbnail->path = picture.path;
    thumbnail->row = row;
    thumbnail->preview = preview;
    thumbnail->placeholder = placeholder;
    state->thumbnails.push_back(std::move(thumbnail));
  }

  if (pictures.empty()) {
    auto *empty = Label(state->grid,
        "No PNG or JPEG images were found in DCIM, Pictures, Download, or AERA screenshots.",
        &lv_font_montserrat_32, kMutedStrong);
    lv_obj_set_width(empty, 1050);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 280);
  } else {
    state->timer = lv_timer_create(GalleryTick, 45, state);
    GalleryTick(state->timer);
  }
  lv_obj_add_event_cb(screen, DeleteGallery, LV_EVENT_DELETE, state);
  Navigation(screen, Action::kBackHome, callback, context, true);
}
}  // namespace aeraui
