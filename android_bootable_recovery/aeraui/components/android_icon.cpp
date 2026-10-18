/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "android_icon.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <androidfw/Asset.h>
#include <androidfw/AssetManager.h>
#include <androidfw/ResourceTypes.h>
#include <utils/String8.h>
#include <webp/decode.h>

#include "picture_decode.hpp"

#define NANOSVG_CPLUSPLUS
#include "nanosvg.h"
#define NANOSVGRAST_CPLUSPLUS
#include "nanosvgrast.h"

namespace aeraui {
namespace {

constexpr size_t kMaximumAssetBytes = 8 * 1024 * 1024;
constexpr uint32_t kMaximumBitmapSide = 4096;

struct Value {
  std::string text;
  uint32_t color = 0;
  float number = 0.0f;
  bool has_text = false;
  bool has_color = false;
  bool has_number = false;
};

struct Canvas {
  explicit Canvas(uint32_t side) : side(side), pixels(size_t(side) * side * 4) {}
  uint32_t side;
  std::vector<uint8_t> pixels;
};

uint32_t AttributeId(const char *name) {
  struct Entry { const char *name; uint32_t id; };
  static constexpr Entry entries[] = {
      {"src", 0x01010119}, {"drawable", 0x01010199},
      {"color", 0x010101a5}, {"pivotX", 0x010101b5},
      {"pivotY", 0x010101b6}, {"scaleX", 0x01010324},
      {"scaleY", 0x01010325}, {"rotation", 0x01010326},
      {"viewportWidth", 0x01010402}, {"viewportHeight", 0x01010403},
      {"fillColor", 0x01010404}, {"pathData", 0x01010405},
      {"strokeColor", 0x01010406}, {"strokeWidth", 0x01010407},
      {"translateX", 0x0101045a}, {"translateY", 0x0101045b},
      {"strokeAlpha", 0x010104cb}, {"fillAlpha", 0x010104cc},
      {"fillType", 0x0101051e},
  };
  for (const auto &entry : entries)
    if (!strcmp(name, entry.name)) return entry.id;
  return 0;
}

std::vector<std::string> SiblingApks(const std::string &base) {
  std::vector<std::string> paths;
  const size_t slash = base.find_last_of('/');
  if (slash == std::string::npos) return paths;
  const std::string directory = base.substr(0, slash);
  DIR *handle = opendir(directory.c_str());
  if (!handle) return paths;
  while (const dirent *entry = readdir(handle)) {
    const std::string name = entry->d_name;
    if (name == "." || name == ".." || name == "base.apk" ||
        name.size() < 4 || name.compare(name.size() - 4, 4, ".apk")) continue;
    paths.push_back(directory + "/" + name);
  }
  closedir(handle);
  std::sort(paths.begin(), paths.end());
  return paths;
}

float TypedFloat(uint32_t data) {
  float value = 0.0f;
  memcpy(&value, &data, sizeof(value));
  return value;
}

float ComplexFloat(uint32_t data) {
  static constexpr float multipliers[] = {
      1.0f / 256.0f, 1.0f / 32768.0f,
      1.0f / 8388608.0f, 1.0f / 2147483648.0f};
  return static_cast<int32_t>(data & 0xffffff00) *
      multipliers[(data >> 4) & 3];
}

std::string Tag(const android::ResXMLTree &tree) {
  size_t length = 0;
  const char16_t *text = tree.getElementName(&length);
  return text ? android::String8(text, length).c_str() : "";
}

Value Attribute(const android::ResXMLTree &tree,
                const android::ResTable &table, const char *wanted) {
  for (size_t index = 0; index < tree.getAttributeCount(); ++index) {
    size_t length = 0;
    const char16_t *name = tree.getAttributeName(index, &length);
    const uint32_t wanted_id = AttributeId(wanted);
    if ((!name || android::String8(name, length) != wanted) &&
        (!wanted_id || tree.getAttributeNameResID(index) != wanted_id)) continue;
    android::Res_value value{};
    if (tree.getAttributeValue(index, &value) == android::BAD_TYPE) return {};
    const uint32_t original_reference = value.data;
    ssize_t block = -1;
    if (value.dataType == android::Res_value::TYPE_REFERENCE ||
        value.dataType == android::Res_value::TYPE_DYNAMIC_REFERENCE)
      block = table.resolveReference(&value, 0);

    Value result;
    if (block < 0 && original_reference >= 0x01060000 &&
        original_reference <= 0x0106ffff) {
      if (original_reference == 0x0106000b) result.color = 0xffffffff;
      else if (original_reference == 0x0106000c) result.color = 0xff000000;
      else if (original_reference == 0x0106000d) result.color = 0x00000000;
      else return result;
      result.has_color = true;
      return result;
    }
    if (value.dataType == android::Res_value::TYPE_STRING) {
      const char16_t *text = nullptr;
      if (block >= 0)
        text = table.valueToString(&value, static_cast<size_t>(block), nullptr,
                                   &length);
      else
        text = tree.getAttributeStringValue(index, &length);
      if (text) {
        result.text = android::String8(text, length).c_str();
        result.has_text = true;
      }
    } else if (value.dataType >= android::Res_value::TYPE_FIRST_COLOR_INT &&
               value.dataType <= android::Res_value::TYPE_LAST_COLOR_INT) {
      result.color = value.data;
      result.has_color = true;
    } else if (value.dataType == android::Res_value::TYPE_FLOAT) {
      result.number = TypedFloat(value.data);
      result.has_number = true;
    } else if (value.dataType == android::Res_value::TYPE_DIMENSION ||
               value.dataType == android::Res_value::TYPE_FRACTION) {
      result.number = ComplexFloat(value.data);
      result.has_number = true;
    } else if (value.dataType >= android::Res_value::TYPE_FIRST_INT &&
               value.dataType <= android::Res_value::TYPE_LAST_INT) {
      result.number = static_cast<float>(value.data);
      result.has_number = true;
    }
    return result;
  }
  return {};
}

float Number(const android::ResXMLTree &tree, const android::ResTable &table,
             const char *name, float fallback) {
  const Value value = Attribute(tree, table, name);
  return value.has_number ? value.number : fallback;
}

std::string XmlEscape(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char c : value) {
    if (c == '&') escaped += "&amp;";
    else if (c == '<') escaped += "&lt;";
    else if (c == '>') escaped += "&gt;";
    else if (c == '\"') escaped += "&quot;";
    else escaped += c;
  }
  return escaped;
}

std::string HexColor(uint32_t argb) {
  char value[8];
  snprintf(value, sizeof(value), "#%06x", argb & 0x00ffffff);
  return value;
}

void Composite(Canvas &destination, const Canvas &source) {
  for (size_t offset = 0; offset < destination.pixels.size(); offset += 4) {
    const unsigned source_alpha = source.pixels[offset + 3];
    if (!source_alpha) continue;
    const unsigned destination_alpha = destination.pixels[offset + 3];
    const unsigned output_alpha = source_alpha +
        (destination_alpha * (255 - source_alpha) + 127) / 255;
    if (!output_alpha) continue;
    for (unsigned channel = 0; channel < 3; ++channel) {
      const unsigned source_premultiplied =
          source.pixels[offset + channel] * source_alpha;
      const unsigned destination_premultiplied =
          destination.pixels[offset + channel] * destination_alpha;
      destination.pixels[offset + channel] = static_cast<uint8_t>(
          (source_premultiplied +
           (destination_premultiplied * (255 - source_alpha) + 127) / 255 +
           output_alpha / 2) / output_alpha);
    }
    destination.pixels[offset + 3] = static_cast<uint8_t>(output_alpha);
  }
}

void ScaleBitmap(const uint8_t *source, uint32_t source_width,
                 uint32_t source_height, Canvas &destination) {
  if (!source || !source_width || !source_height ||
      source_width > kMaximumBitmapSide || source_height > kMaximumBitmapSide)
    return;
  const double scale = std::min(double(destination.side) / source_width,
                                double(destination.side) / source_height);
  const uint32_t width = std::max(1u,
      static_cast<uint32_t>(std::round(source_width * scale)));
  const uint32_t height = std::max(1u,
      static_cast<uint32_t>(std::round(source_height * scale)));
  const uint32_t offset_x = (destination.side - width) / 2;
  const uint32_t offset_y = (destination.side - height) / 2;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t from_y = std::min(source_height - 1,
        static_cast<uint32_t>((uint64_t(y) * source_height) / height));
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t from_x = std::min(source_width - 1,
          static_cast<uint32_t>((uint64_t(x) * source_width) / width));
      memcpy(destination.pixels.data() +
                 (uint64_t(y + offset_y) * destination.side + x + offset_x) * 4,
             source + (uint64_t(from_y) * source_width + from_x) * 4, 4);
    }
  }
}

bool WriteTemporary(const void *data, size_t length, std::string &path) {
  char name[] = "/tmp/aera-android-icon-XXXXXX";
  const int fd = mkstemp(name);
  if (fd < 0) return false;
  bool success = !fchmod(fd, 0600);
  size_t written = 0;
  while (success && written < length) {
    const ssize_t count = write(fd, static_cast<const uint8_t *>(data) + written,
                                length - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) success = false;
    else written += static_cast<size_t>(count);
  }
  close(fd);
  if (!success) unlink(name);
  else path = name;
  return success;
}

class Renderer {
 public:
  Renderer(const std::string &apk, uint32_t size) : size_(size) {
    android::ResTable_config configuration{};
    configuration.density = 480;
    configuration.sdkVersion = 35;
    ready_ = assets_.addAssetPath(android::String8(apk.c_str()), &cookie_);
    if (ready_) {
      for (const auto &split : SiblingApks(apk)) {
        int32_t split_cookie = 0;
        assets_.addAssetPath(android::String8(split.c_str()), &split_cookie);
      }
      assets_.setConfiguration(configuration, "en-US");
    }
  }

  bool Render(const std::string &path, Canvas &output, unsigned depth = 0) {
    if (!ready_ || path.empty() || depth > 8) return false;
    std::unique_ptr<android::Asset> asset(assets_.openNonAsset(
        path.c_str(), android::Asset::ACCESS_BUFFER));
    if (!asset) return false;
    const off64_t asset_length = asset->getLength();
    if (asset_length <= 0 || static_cast<uint64_t>(asset_length) > kMaximumAssetBytes)
      return false;
    const auto *data = static_cast<const uint8_t *>(asset->getBuffer(true));
    const size_t length = static_cast<size_t>(asset_length);
    if (!data) return false;
    if (length >= 12 && !memcmp(data, "RIFF", 4) &&
        !memcmp(data + 8, "WEBP", 4))
      return Webp(data, length, output);
    if (length >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8))
      return Picture(data, length, output);
    if (length >= 2 && data[0] == 0xff && data[1] == 0xd8)
      return Picture(data, length, output);
    return Xml(data, length, output, depth);
  }

 private:
  bool Picture(const void *data, size_t length, Canvas &output) {
    std::string temporary;
    if (!WriteTemporary(data, length, temporary)) return false;
    PictureData picture;
    DecodePicture(temporary, picture);
    unlink(temporary.c_str());
    if (!picture.pixels) return false;
    ScaleBitmap(picture.pixels, picture.width, picture.height, output);
    return true;
  }

  bool Webp(const uint8_t *data, size_t length, Canvas &output) {
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(data, length, &width, &height) || width <= 0 || height <= 0 ||
        width > static_cast<int>(kMaximumBitmapSide) ||
        height > static_cast<int>(kMaximumBitmapSide)) return false;
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    if (!WebPDecodeBGRAInto(data, length, pixels.data(), pixels.size(), width * 4))
      return false;
    ScaleBitmap(pixels.data(), width, height, output);
    return true;
  }

  bool RenderValue(const Value &value, Canvas &output, unsigned depth) {
    if (value.has_color) {
      for (size_t offset = 0; offset < output.pixels.size(); offset += 4) {
        output.pixels[offset] = value.color & 0xff;
        output.pixels[offset + 1] = (value.color >> 8) & 0xff;
        output.pixels[offset + 2] = (value.color >> 16) & 0xff;
        output.pixels[offset + 3] = (value.color >> 24) & 0xff;
      }
      return true;
    }
    return value.has_text && Render(value.text, output, depth + 1);
  }

  bool RasterizeSvg(std::string svg, Canvas &output) {
    std::vector<char> source(svg.begin(), svg.end());
    source.push_back('\0');
    NSVGimage *image = nsvgParse(source.data(), "px", 96.0f);
    NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
    if (!image || !rasterizer) {
      if (image) nsvgDelete(image);
      if (rasterizer) nsvgDeleteRasterizer(rasterizer);
      return false;
    }
    std::vector<uint8_t> rgba(size_t(size_) * size_ * 4);
    nsvgRasterize(rasterizer, image, 0, 0, 1.0f, rgba.data(), size_, size_,
                  size_ * 4);
    for (size_t offset = 0; offset < rgba.size(); offset += 4) {
      output.pixels[offset] = rgba[offset + 2];
      output.pixels[offset + 1] = rgba[offset + 1];
      output.pixels[offset + 2] = rgba[offset];
      output.pixels[offset + 3] = rgba[offset + 3];
    }
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);
    return true;
  }

  bool Vector(android::ResXMLTree &tree, const android::ResTable &table) {
    const float viewport_width = Number(tree, table, "viewportWidth", 108.0f);
    const float viewport_height = Number(tree, table, "viewportHeight", 108.0f);
    if (viewport_width <= 0 || viewport_height <= 0) return false;
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << size_
        << "\" height=\"" << size_ << "\" viewBox=\"0 0 "
        << viewport_width << ' ' << viewport_height << "\">";
    int groups = 0;
    while (true) {
      const auto event = tree.next();
      if (event == android::ResXMLTree::END_DOCUMENT ||
          event == android::ResXMLTree::BAD_DOCUMENT) break;
      const std::string tag = Tag(tree);
      if (event == android::ResXMLTree::START_TAG && tag == "group") {
        const float pivot_x = Number(tree, table, "pivotX", 0.0f);
        const float pivot_y = Number(tree, table, "pivotY", 0.0f);
        svg << "<g transform=\"translate("
            << Number(tree, table, "translateX", 0.0f) + pivot_x << ' '
            << Number(tree, table, "translateY", 0.0f) + pivot_y << ") rotate("
            << Number(tree, table, "rotation", 0.0f) << ") scale("
            << Number(tree, table, "scaleX", 1.0f) << ' '
            << Number(tree, table, "scaleY", 1.0f) << ") translate("
            << -pivot_x << ' ' << -pivot_y << ")\">";
        ++groups;
      } else if (event == android::ResXMLTree::END_TAG && tag == "group" && groups) {
        svg << "</g>";
        --groups;
      } else if (event == android::ResXMLTree::START_TAG && tag == "path") {
        const Value path = Attribute(tree, table, "pathData");
        if (!path.has_text) continue;
        const Value fill = Attribute(tree, table, "fillColor");
        const Value stroke = Attribute(tree, table, "strokeColor");
        const float fill_alpha = Number(tree, table, "fillAlpha", 1.0f);
        const float stroke_alpha = Number(tree, table, "strokeAlpha", 1.0f);
        svg << "<path d=\"" << XmlEscape(path.text) << "\"";
        if (fill.has_color) {
          svg << " fill=\"" << HexColor(fill.color) << "\" fill-opacity=\""
              << fill_alpha * ((fill.color >> 24) & 0xff) / 255.0f << "\"";
        } else {
          svg << " fill=\"none\"";
        }
        if (stroke.has_color) {
          svg << " stroke=\"" << HexColor(stroke.color)
              << "\" stroke-opacity=\""
              << stroke_alpha * ((stroke.color >> 24) & 0xff) / 255.0f
              << "\" stroke-width=\""
              << Number(tree, table, "strokeWidth", 0.0f) << "\"";
        }
        if (Number(tree, table, "fillType", 0.0f) != 0.0f)
          svg << " fill-rule=\"evenodd\"";
        svg << "/>";
      }
    }
    while (groups-- > 0) svg << "</g>";
    svg << "</svg>";
    Canvas output(size_);
    if (!RasterizeSvg(svg.str(), output)) return false;
    rendered_ = std::move(output.pixels);
    return true;
  }

  bool Xml(const void *data, size_t length, Canvas &output, unsigned depth) {
    android::ResXMLTree tree;
    if (tree.setTo(data, length) != android::NO_ERROR) return false;
    while (true) {
      const auto event = tree.next();
      if (event == android::ResXMLTree::START_TAG) break;
      if (event == android::ResXMLTree::END_DOCUMENT ||
          event == android::ResXMLTree::BAD_DOCUMENT) return false;
    }
    const android::ResTable &table = assets_.getResources();
    const std::string root = Tag(tree);
    if (root == "vector") {
      rendered_.clear();
      if (!Vector(tree, table) || rendered_.empty()) return false;
      output.pixels = std::move(rendered_);
      return true;
    }

    if (root == "adaptive-icon") {
      bool drew = false;
      Value monochrome;
      while (true) {
        const auto event = tree.next();
        if (event == android::ResXMLTree::END_DOCUMENT ||
            event == android::ResXMLTree::BAD_DOCUMENT) break;
        if (event != android::ResXMLTree::START_TAG) continue;
        const std::string child = Tag(tree);
        if (child != "background" && child != "foreground" &&
            child != "monochrome") continue;
        const Value drawable = Attribute(tree, table, "drawable");
        if (child == "monochrome") {
          monochrome = drawable;
          continue;
        }
        Canvas layer(size_);
        if (RenderValue(drawable, layer, depth)) {
          Composite(output, layer);
          drew = true;
        }
      }
      if (!drew && (monochrome.has_color || monochrome.has_text)) {
        Canvas layer(size_);
        if (RenderValue(monochrome, layer, depth)) {
          Composite(output, layer);
          drew = true;
        }
      }
      return drew;
    }

    if (root == "layer-list" || root == "selector") {
      bool drew = false;
      while (true) {
        const auto event = tree.next();
        if (event == android::ResXMLTree::END_DOCUMENT ||
            event == android::ResXMLTree::BAD_DOCUMENT) break;
        if (event != android::ResXMLTree::START_TAG || Tag(tree) != "item") continue;
        Canvas layer(size_);
        if (RenderValue(Attribute(tree, table, "drawable"), layer, depth)) {
          Composite(output, layer);
          drew = true;
          if (root == "selector") break;
        }
      }
      return drew;
    }

    if (root == "bitmap" || root == "inset" || root == "ripple") {
      Value drawable = Attribute(tree, table, root == "bitmap" ? "src" : "drawable");
      if (RenderValue(drawable, output, depth)) return true;
    }

    if (root == "shape") {
      while (true) {
        const auto event = tree.next();
        if (event == android::ResXMLTree::END_DOCUMENT ||
            event == android::ResXMLTree::BAD_DOCUMENT) break;
        if (event == android::ResXMLTree::START_TAG && Tag(tree) == "solid")
          return RenderValue(Attribute(tree, table, "color"), output, depth);
      }
    }
    return false;
  }

  android::AssetManager assets_;
  int32_t cookie_ = 0;
  uint32_t size_;
  bool ready_ = false;
  std::vector<uint8_t> rendered_;
};

}  // namespace

bool DecodeAndroidIcon(const std::string &apk,
                       const std::vector<std::string> &resources,
                       uint32_t size, std::vector<uint8_t> &pixels) {
  if (!size || size > 512) return false;
  Renderer renderer(apk, size);
  for (const auto &resource : resources) {
    Canvas canvas(size);
    if (renderer.Render(resource, canvas)) {
      pixels = std::move(canvas.pixels);
      return true;
    }
  }
  return false;
}

bool ReadAndroidPackageName(const std::string &apk, std::string &package_name) {
  package_name.clear();
  android::AssetManager assets;
  int32_t cookie = 0;
  if (!assets.addAssetPath(android::String8(apk.c_str()), &cookie)) return false;
  std::unique_ptr<android::Asset> manifest(assets.openNonAsset(
      cookie, "AndroidManifest.xml", android::Asset::ACCESS_BUFFER));
  if (!manifest) return false;
  android::ResXMLTree tree;
  if (tree.setTo(manifest->getBuffer(true), manifest->getLength()) !=
      android::NO_ERROR) return false;
  while (true) {
    const auto event = tree.next();
    if (event == android::ResXMLTree::END_DOCUMENT ||
        event == android::ResXMLTree::BAD_DOCUMENT) return false;
    if (event != android::ResXMLTree::START_TAG) continue;
    size_t tag_length = 0;
    const char16_t *tag = tree.getElementName(&tag_length);
    if (!tag || android::String8(tag, tag_length) != "manifest") return false;
    for (size_t index = 0; index < tree.getAttributeCount(); ++index) {
      size_t name_length = 0;
      const char16_t *name = tree.getAttributeName(index, &name_length);
      if (!name || android::String8(name, name_length) != "package") continue;
      size_t value_length = 0;
      const char16_t *value = tree.getAttributeStringValue(index, &value_length);
      if (!value || !value_length) return false;
      package_name = android::String8(value, value_length).c_str();
      return true;
    }
    return false;
  }
}

}  // namespace aeraui
