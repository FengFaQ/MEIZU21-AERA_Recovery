/* SPDX-License-Identifier: Apache-2.0 */
#include "picture_decode.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <setjmp.h>
#include <memory>
#include <sys/stat.h>
#include <vector>
#include <jpeglib.h>
#include <png.h>
#include "src/libs/tjpgd/tjpgd.h"

namespace aeraui {
namespace {
constexpr uint64_t kMaxFileBytes = 32 * 1024 * 1024;
constexpr uint64_t kMaxPixels = 16 * 1024 * 1024;

bool Allocate(PictureData &out, uint32_t width, uint32_t height) {
  if (!width || !height || width > 8192 || height > 8192 ||
      uint64_t(width) * height > kMaxPixels) {
    out.error = "Image exceeds the 16-megapixel / 8192-pixel dimension limit.";
    return false;
  }
  out.pixels = static_cast<uint8_t *>(malloc(size_t(width) * height * 4));
  if (!out.pixels) {
    out.error = "Not enough memory to preview this image.";
    return false;
  }
  out.width = width;
  out.height = height;
  return true;
}

void ScaleBgra(const uint8_t *source, uint32_t source_width,
               uint32_t source_height, uint8_t *target,
               uint32_t target_width, uint32_t target_height) {
  for (uint32_t y = 0; y < target_height; ++y) {
    const uint32_t source_y = std::min(source_height - 1,
        static_cast<uint32_t>((uint64_t(y) * source_height) / target_height));
    for (uint32_t x = 0; x < target_width; ++x) {
      const uint32_t source_x = std::min(source_width - 1,
          static_cast<uint32_t>((uint64_t(x) * source_width) / target_width));
      memcpy(target + (size_t(y) * target_width + x) * 4,
             source + (size_t(source_y) * source_width + source_x) * 4, 4);
    }
  }
}

void Png(FILE *file, PictureData &out, uint32_t max_width,
         uint32_t max_height) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_stdio(&image, file)) {
    out.error = "Could not read this PNG image.";
    png_image_free(&image);
    return;
  }
  image.format = PNG_FORMAT_BGRA;
  const bool scale = max_width && max_height &&
      (image.width > max_width || image.height > max_height);
  if (!scale) {
    if (!out.cancelled && Allocate(out, image.width, image.height) &&
        !png_image_finish_read(&image, nullptr, out.pixels, 0, nullptr))
      out.error = "PNG decoding failed; the file may be damaged.";
  } else if (!image.width || !image.height || image.width > 8192 ||
             image.height > 8192 ||
             uint64_t(image.width) * image.height > kMaxPixels) {
    out.error = "PNG dimensions exceed the preview limit.";
  } else {
    std::vector<uint8_t> full(size_t(image.width) * image.height * 4);
    if (!png_image_finish_read(&image, nullptr, full.data(), 0, nullptr)) {
      out.error = "PNG decoding failed; the file may be damaged.";
    } else if (!out.cancelled) {
      const uint32_t divisor = std::max(
          (image.width + max_width - 1) / max_width,
          (image.height + max_height - 1) / max_height);
      const uint32_t width = std::max(1u, image.width / divisor);
      const uint32_t height = std::max(1u, image.height / divisor);
      if (Allocate(out, width, height))
        ScaleBgra(full.data(), image.width, image.height, out.pixels,
                  width, height);
    }
  }
  png_image_free(&image);
}

struct JpegInput { FILE *file; PictureData *out; uint32_t step = 1; };
size_t ReadJpeg(JDEC *decoder, uint8_t *buffer, size_t count) {
  auto *input = static_cast<JpegInput *>(decoder->device);
  if (input->out->cancelled) return 0;
  if (buffer) return fread(buffer, 1, count, input->file);
  // Read discarded bytes too, so truncated skips cannot succeed past EOF.
  std::array<uint8_t, 512> discard{};
  size_t read = 0;
  while (read < count) {
    const size_t chunk = std::min(discard.size(), count - read);
    const size_t got = fread(discard.data(), 1, chunk, input->file);
    read += got;
    if (got != chunk) break;
  }
  return read;
}
int WriteJpeg(JDEC *decoder, void *bitmap, JRECT *rect) {
  auto *input = static_cast<JpegInput *>(decoder->device);
  auto *out = input->out;
  if (out->cancelled || rect->right >= decoder->width || rect->bottom >= decoder->height ||
      rect->left > rect->right || rect->top > rect->bottom) return 0;
  const auto *src = static_cast<const uint8_t *>(bitmap);
  const uint32_t step = input->step;
  const uint32_t first_x = (rect->left + step - 1) / step * step;
  const uint32_t first_y = (rect->top + step - 1) / step * step;
  for (uint32_t y = first_y; y <= rect->bottom; y += step) {
    auto *dest = out->pixels + (size_t(y / step) * out->width + first_x / step) * 4;
    for (uint32_t x = first_x; x <= rect->right; x += step) {
      const auto *pixel = src + ((y - rect->top) * (rect->right - rect->left + 1) + x - rect->left) * 3;
      // LVGL's TJpgDec fork emits BGR, not upstream RGB.
      dest[0] = pixel[0]; dest[1] = pixel[1]; dest[2] = pixel[2]; dest[3] = 255;
      dest += 4;
    }
  }
  return 1;
}

struct LibJpegError {
  jpeg_error_mgr base{};
  jmp_buf jump{};
  JSAMPLE *row = nullptr;
};

void LibJpegErrorExit(j_common_ptr decoder) {
  auto *error = reinterpret_cast<LibJpegError *>(decoder->err);
  longjmp(error->jump, 1);
}

void ProgressiveJpeg(FILE *file, PictureData &out, uint32_t max_width,
                     uint32_t max_height) {
  rewind(file);
  jpeg_decompress_struct decoder{};
  LibJpegError error{};
  decoder.err = jpeg_std_error(&error.base);
  error.base.error_exit = LibJpegErrorExit;
  if (setjmp(error.jump)) {
    free(error.row);
    jpeg_destroy_decompress(&decoder);
    free(out.pixels);
    out.pixels = nullptr;
    out.width = out.height = 0;
    out.error = "JPEG decoding failed; the file may be damaged or unsupported.";
    return;
  }

  jpeg_create_decompress(&decoder);
  decoder.mem->max_memory_to_use = 96 * 1024 * 1024;
  jpeg_stdio_src(&decoder, file);
  jpeg_read_header(&decoder, TRUE);
  if (!decoder.image_width || !decoder.image_height ||
      decoder.image_width > 16384 || decoder.image_height > 16384 ||
      uint64_t(decoder.image_width) * decoder.image_height > 64 * 1024 * 1024) {
    jpeg_destroy_decompress(&decoder);
    out.error = "JPEG dimensions exceed the preview limit.";
    return;
  }

  const uint32_t width_limit = max_width ? max_width : 2048;
  const uint32_t height_limit = max_height ? max_height : 2048;
  decoder.scale_num = 1;
  decoder.scale_denom = 1;
  while (decoder.scale_denom < 8 &&
         ((decoder.image_width + decoder.scale_denom - 1) /
                  decoder.scale_denom > width_limit ||
          (decoder.image_height + decoder.scale_denom - 1) /
                  decoder.scale_denom > height_limit))
    decoder.scale_denom *= 2;
  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);
  if (!Allocate(out, decoder.output_width, decoder.output_height)) {
    jpeg_destroy_decompress(&decoder);
    return;
  }

  error.row = static_cast<JSAMPLE *>(malloc(size_t(decoder.output_width) * 3));
  if (!error.row) {
    jpeg_destroy_decompress(&decoder);
    free(out.pixels);
    out.pixels = nullptr;
    out.width = out.height = 0;
    out.error = "Not enough memory to preview this image.";
    return;
  }
  while (decoder.output_scanline < decoder.output_height && !out.cancelled) {
    JSAMPROW rows[] = {error.row};
    if (jpeg_read_scanlines(&decoder, rows, 1) != 1)
      break;
    auto *target = out.pixels +
        size_t(decoder.output_scanline - 1) * decoder.output_width * 4;
    for (uint32_t x = 0; x < decoder.output_width; ++x) {
      target[x * 4] = error.row[x * 3 + 2];
      target[x * 4 + 1] = error.row[x * 3 + 1];
      target[x * 4 + 2] = error.row[x * 3];
      target[x * 4 + 3] = 255;
    }
  }
  free(error.row);
  error.row = nullptr;
  if (out.cancelled) {
    jpeg_abort_decompress(&decoder);
  } else {
    jpeg_finish_decompress(&decoder);
  }
  jpeg_destroy_decompress(&decoder);
}

void Jpeg(FILE *file, PictureData &out, uint32_t max_width,
          uint32_t max_height) {
  static_assert(JD_FORMAT == 0, "Picture viewer requires RGB888 JPEG output");
  static_assert(JD_USE_SCALE == 0, "Use bounded output sampling, not TJpgDec scaling");
  alignas(16) std::array<uint8_t, 32768> work{};
  JDEC decoder{};
  JpegInput input{file, &out};
  if (jd_prepare(&decoder, ReadJpeg, work.data(), work.size(), &input) != JDR_OK) {
    // TJpgDec deliberately stays as the tiny baseline fast path. Telegram and
    // many cameras emit progressive JPEGs, so use bounded libjpeg decoding for
    // formats that TJpgDec cannot prepare.
    ProgressiveJpeg(file, out, max_width, max_height);
    return;
  }
  if (!decoder.width || !decoder.height || decoder.width > 16384 || decoder.height > 16384 ||
      uint64_t(decoder.width) * decoder.height > 64 * 1024 * 1024) {
    out.error = "JPEG dimensions exceed the preview limit.";
    return;
  }
  const uint32_t width_limit = max_width ? max_width : 2048;
  const uint32_t height_limit = max_height ? max_height : 2048;
  while ((decoder.width + input.step - 1) / input.step > width_limit ||
         (decoder.height + input.step - 1) / input.step > height_limit)
    input.step *= 2;
  if (Allocate(out, (decoder.width + input.step - 1) / input.step,
                    (decoder.height + input.step - 1) / input.step) &&
      jd_decomp(&decoder, WriteJpeg, 0) != JDR_OK)
    out.error = "JPEG decoding failed; the file may be damaged or unsupported.";
}
void DecodeFile(const std::string &path, PictureData &out,
                uint32_t max_width, uint32_t max_height) {
  std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
  struct stat info{};
  if (out.cancelled) {
    out.ready.store(true, std::memory_order_release);
    return;
  }
  if (!file || fstat(fileno(file.get()), &info) || !S_ISREG(info.st_mode))
    out.error = "Cannot open this image. Check storage and decryption.";
  else if (info.st_size <= 0 || uint64_t(info.st_size) > kMaxFileBytes)
    out.error = "Image is empty or exceeds the 32 MiB file-size limit.";
  else {
    std::array<uint8_t, 8> signature{};
    const size_t count = fread(signature.data(), 1, signature.size(), file.get());
    rewind(file.get());
    if (count == 8 && png_sig_cmp(signature.data(), 0, 8) == 0)
      Png(file.get(), out, max_width, max_height);
    else if (count >= 2 && signature[0] == 0xff && signature[1] == 0xd8)
      Jpeg(file.get(), out, max_width, max_height);
    else
      out.error = "Not a supported PNG or JPEG image.";
  }
  if (!out.error.empty() || out.cancelled) {
    free(out.pixels);
    out.pixels = nullptr;
  }
  out.ready.store(true, std::memory_order_release);
}
} // namespace

bool IsPicture(const std::string &path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  auto suffix = path.substr(dot);
  for (auto &c : suffix) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return suffix == ".png" || suffix == ".jpg" || suffix == ".jpeg";
}

void DecodePicture(const std::string &path, PictureData &out) {
  // Closing/reopening while a PNG finishes must not accumulate large buffers.
  static std::atomic<bool> busy{false};
  if (busy.exchange(true)) {
    out.error = "The previous preview is still closing. Please try again.";
    out.ready.store(true, std::memory_order_release);
    return;
  }
  struct Release { std::atomic<bool> &flag; ~Release() { flag = false; } } release{busy};
  DecodeFile(path, out, 0, 0);
}

void DecodePictureThumbnail(const std::string &path, uint32_t max_width,
                            uint32_t max_height, PictureData &out) {
  DecodeFile(path, out, std::max(1u, max_width), std::max(1u, max_height));
}
} // namespace aeraui
