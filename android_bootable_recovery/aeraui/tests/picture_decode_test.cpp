/* SPDX-License-Identifier: Apache-2.0 */
// Host regression test: libpng + libjpeg are needed to generate tiny fixtures.
#include "picture_decode.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <png.h>
#include <jpeglib.h>
#include <unistd.h>
using namespace aeraui;

void WritePng(const std::string &path, unsigned width, unsigned height) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = width; image.height = height; image.format = PNG_FORMAT_BGRA;
  std::vector<unsigned char> pixels(size_t(width) * height * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = 23; pixels[i + 1] = 71; pixels[i + 2] = 191; pixels[i + 3] = 255;
  }
  assert(png_image_write_to_file(&image, path.c_str(), 0, pixels.data(), 0, nullptr));
}
void WriteJpeg(const std::string &path, int width, int height, bool progressive) {
  FILE *file = fopen(path.c_str(), "wb"); assert(file);
  jpeg_compress_struct encoder{};
  jpeg_error_mgr error{};
  encoder.err = jpeg_std_error(&error);
  jpeg_create_compress(&encoder);
  jpeg_stdio_dest(&encoder, file);
  encoder.image_width = width; encoder.image_height = height;
  encoder.input_components = 3; encoder.in_color_space = JCS_RGB;
  jpeg_set_defaults(&encoder);
  if (progressive) jpeg_simple_progression(&encoder);
  jpeg_start_compress(&encoder, TRUE);
  std::vector<unsigned char> row(width * 3);
  for (int x = 0; x < width; ++x) { row[x * 3] = 191; row[x * 3 + 1] = 71; row[x * 3 + 2] = 23; }
  while (encoder.next_scanline < encoder.image_height) {
    auto *data = row.data(); jpeg_write_scanlines(&encoder, &data, 1);
  }
  jpeg_finish_compress(&encoder); jpeg_destroy_compress(&encoder); fclose(file);
}
int main() {
  char folder[] = "/tmp/aera-picture-test-XXXXXX";
  assert(mkdtemp(folder));
  const std::string root(folder);
  assert(IsPicture("image.PNG") && IsPicture("image.JpEg") && !IsPicture("image.zip"));
  WritePng(root + "/valid.png", 2, 3);
  { PictureData data; DecodePicture(root + "/valid.png", data);
    assert(data.ready && data.error.empty() && data.pixels && data.width == 2 && data.height == 3);
    assert(data.pixels[0] == 23 && data.pixels[1] == 71 && data.pixels[2] == 191 && data.pixels[3] == 255); }
  WritePng(root + "/wide.png", 8193, 1);
  { PictureData data; DecodePicture(root + "/wide.png", data); assert(data.ready && !data.pixels && !data.error.empty()); }
  WriteJpeg(root + "/valid.jpg", 37, 29, false);
  { PictureData data; DecodePicture(root + "/valid.jpg", data);
    assert(data.ready && data.error.empty() && data.pixels && data.width == 37 && data.height == 29);
    assert(data.pixels[0] < data.pixels[1] && data.pixels[1] < data.pixels[2] && data.pixels[3] == 255); }
  WriteJpeg(root + "/large.jpg", 4097, 33, false);
  { PictureData data; DecodePicture(root + "/large.jpg", data);
    assert(data.ready && data.error.empty() && data.pixels && data.width == 1025 && data.height == 9);
    for (unsigned i = 0; i < data.width * data.height; ++i)
      assert(data.pixels[i * 4] < data.pixels[i * 4 + 1] && data.pixels[i * 4 + 1] < data.pixels[i * 4 + 2] && data.pixels[i * 4 + 3] == 255); }
  WriteJpeg(root + "/progressive.jpg", 37, 29, true);
  { PictureData data; DecodePicture(root + "/progressive.jpg", data);
    assert(data.ready && data.error.empty() && data.pixels && data.width == 37 && data.height == 29); }
  { PictureData data; data.cancelled = true; DecodePicture(root + "/valid.png", data); assert(data.ready && !data.pixels); }
  { PictureData data; DecodePicture(root + "/missing.png", data); assert(data.ready && !data.pixels && !data.error.empty()); }
  { FILE *file = fopen((root + "/broken.png").c_str(), "wb"); assert(file);
    const unsigned char partial[]{137,80,78,71,13,10,26,10};
    assert(fwrite(partial, 1, sizeof(partial), file) == sizeof(partial)); fclose(file); }
  { PictureData data; DecodePicture(root + "/broken.png", data); assert(data.ready && !data.pixels && !data.error.empty()); }
  { FILE *file = fopen((root + "/empty.jpg").c_str(), "wb"); assert(file); fclose(file); }
  { PictureData data; DecodePicture(root + "/empty.jpg", data); assert(data.ready && !data.pixels && !data.error.empty()); }
  printf("Picture decoder tests passed. Fixtures: %s\n", folder);
}
