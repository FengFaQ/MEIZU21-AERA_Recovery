/* SPDX-License-Identifier: Apache-2.0 */
#include "wallpaper.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "picture_decode.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace aeraui::wallpaper {
namespace {

std::shared_ptr<PictureData> gPicture;
std::string gPath;

struct Instance {
  std::shared_ptr<PictureData> picture;
  lv_image_dsc_t descriptor{};
};

void DeleteInstance(lv_event_t *event) {
  auto *instance = static_cast<Instance *>(lv_event_get_user_data(event));
  if (!instance) return;
  lv_image_cache_drop(&instance->descriptor);
  delete instance;
}

}  // namespace

bool Load(const std::string &path, std::string *error) {
  if (path.empty()) {
    Clear();
    return true;
  }
  if (gPicture && gPicture->pixels && gPath == path) return true;

  const uint32_t horizontal = std::max(
      1, static_cast<int>(lv_display_get_horizontal_resolution(nullptr)));
  const uint32_t vertical = std::max(
      1, static_cast<int>(lv_display_get_vertical_resolution(nullptr)));
  const uint32_t limit = std::max(horizontal, vertical);
  auto picture = std::make_shared<PictureData>();
  DecodePictureThumbnail(path, limit, limit, *picture);
  if (!picture->pixels) {
    if (error) *error = picture->error.empty()
        ? "AERA could not decode this image." : picture->error;
    return false;
  }

  gPicture = std::move(picture);
  gPath = path;
  return true;
}

void Clear() {
  gPicture.reset();
  gPath.clear();
}

bool Active() { return gPicture && gPicture->pixels; }

const std::string &Path() { return gPath; }

int PreviewHeight(int width, int fallback) {
  if (!Active() || gPicture->width == 0) return fallback;
  return std::max(1, static_cast<int>(
      (static_cast<int64_t>(width) * gPicture->height + gPicture->width / 2) /
      gPicture->width));
}

static bool AttachPicture(lv_obj_t *target, std::shared_ptr<PictureData> picture, bool contain) {
  if (!target || !picture || !picture->pixels) return false;
  // Bake rounded preview corners into alpha. Some hardware drawing paths do
  // not apply a parent's corner clip to a scaled image child.
  const int radius=lv_obj_get_style_radius(target,LV_PART_MAIN);
  if(radius>0 && !contain) {
    lv_obj_update_layout(target);
    const int w=lv_obj_get_width(target),h=lv_obj_get_height(target);
    if(w>0 && h>0 && w<=1024 && h<=2048) {
      auto rounded=std::make_shared<PictureData>();
      rounded->width=w;rounded->height=h;
      rounded->pixels=static_cast<uint8_t *>(malloc(size_t(w)*h*4));
      if(rounded->pixels) {
        const double scale=std::max(double(w)/picture->width,double(h)/picture->height);
        const double ox=(picture->width-w/scale)/2,oy=(picture->height-h/scale)/2;
        const double r=std::min({radius,w/2,h/2});
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
          const int sx=std::clamp(int(ox+(x+0.5)/scale),0,int(picture->width)-1);
          const int sy=std::clamp(int(oy+(y+0.5)/scale),0,int(picture->height)-1);
          auto *out=rounded->pixels+(size_t(y)*w+x)*4;
          const auto *in=picture->pixels+(size_t(sy)*picture->width+sx)*4;
          std::copy(in,in+4,out);
          const double dx=std::max({r-(x+0.5),(x+0.5)-(w-r),0.0});
          const double dy=std::max({r-(y+0.5),(y+0.5)-(h-r),0.0});
          out[3]=uint8_t(in[3]*std::clamp(r+0.5-std::hypot(dx,dy),0.0,1.0));
        }
        picture=std::move(rounded);
      }
    }
  }
  auto *instance = new Instance;
  instance->picture = std::move(picture);
  auto &descriptor = instance->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = instance->picture->width;
  descriptor.header.h = instance->picture->height;
  descriptor.header.stride = instance->picture->width * 4;
  descriptor.data_size = instance->picture->width *
      instance->picture->height * 4;
  descriptor.data = instance->picture->pixels;

  auto *image = lv_image_create(target);
  lv_obj_set_pos(image, 0, 0);
  lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
  lv_image_set_src(image, &descriptor);
  lv_image_set_inner_align(image, contain ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_COVER);
  lv_image_set_antialias(image, true);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(image, DeleteInstance, LV_EVENT_DELETE, instance);
  lv_obj_move_background(image);
  return true;
}

bool Attach(lv_obj_t *target, bool contain) {
  return AttachPicture(target, gPicture, contain);
}

bool AttachLock(lv_obj_t *target, const std::string &path, int blur) {
  static std::string custom_path;
  static std::shared_ptr<PictureData> custom, softened, previous;
  static int previous_blur = -1;
  auto source = gPicture;
  if (!path.empty()) {
    if (!custom || custom_path != path) {
      auto image = std::make_shared<PictureData>();
      const int limit = std::max(lv_display_get_horizontal_resolution(nullptr),
                                  lv_display_get_vertical_resolution(nullptr));
      DecodePictureThumbnail(path, limit, limit, *image);
      custom = image; custom_path = path;
    }
    if (custom->pixels) source = custom;
  }
  if (!source || !source->pixels) return false;
  blur = std::clamp(blur, 0, 100);
  if (!blur) return AttachPicture(target, source, false);
  if (previous != source || previous_blur != blur) {
    auto image = std::make_shared<PictureData>();
    const double ratio = std::min(1.0, 768.0 / std::max(source->width, source->height));
    const int w = std::max(1, int(source->width * ratio));
    const int h = std::max(1, int(source->height * ratio));
    image->width = w; image->height = h;
    image->pixels = static_cast<uint8_t *>(malloc(size_t(w) * h * 4));
    if (!image->pixels) return AttachPicture(target, source, false);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c)
        image->pixels[(y*w+x)*4+c] = source->pixels[
            ((uint64_t(y)*source->height/h)*source->width + uint64_t(x)*source->width/w)*4+c];
    std::vector<uint8_t> temporary(size_t(w)*h*4);
    const int radius = std::max(1, blur * 24 / 100);
    // Two separable box passes approximate a smooth Gaussian. Cached pixels,
    // not a live framebuffer blur: no text bleed or continuous GPU work.
    for (int pass = 0; pass < 4; ++pass) {
      const bool horizontal = pass % 2 == 0;
      const int length = horizontal ? w : h, lines = horizontal ? h : w;
      auto index = [=](int line, int p, int c) {
        p = std::clamp(p, 0, length-1);
        return ((horizontal ? line*w+p : p*w+line)*4+c);
      };
      for (int line = 0; line < lines; ++line) for (int c = 0; c < 4; ++c) {
        int sum = 0;
        for (int p = -radius; p <= radius; ++p) sum += image->pixels[index(line,p,c)];
        for (int p = 0; p < length; ++p) {
          temporary[index(line,p,c)] = sum / (2*radius+1);
          sum += image->pixels[index(line,p+radius+1,c)] - image->pixels[index(line,p-radius,c)];
        }
      }
      std::copy(temporary.begin(), temporary.end(), image->pixels);
    }
    softened = image; previous = source; previous_blur = blur;
  }
  return AttachPicture(target, softened, false);
}

}  // namespace aeraui::wallpaper
