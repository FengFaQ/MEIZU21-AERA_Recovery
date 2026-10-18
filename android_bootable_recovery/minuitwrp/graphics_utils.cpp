/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>
#include <pixelflinger/pixelflinger.h>
#include <linux/fb.h>
#include <stdint.h>
#include <string.h>

#include "minuitwrp/minui.h"

struct fb_var_screeninfo vi;
extern GGLSurface gr_mem_surface;
extern GRSurface* gr_draw;
extern unsigned int gr_rotation;

struct aera_jpeg_error {
    struct jpeg_error_mgr base;
    jmp_buf jump;
};

static void aera_jpeg_error_exit(j_common_ptr info)
{
    aera_jpeg_error *error = (aera_jpeg_error *)info->err;
    longjmp(error->jump, 1);
}

static bool gr_capture_source(GGLSurface *source)
{
    *source = gr_mem_surface;
    const gr_surface presented = gr_drm_get_presented_surface();
    if (presented != NULL) {
        const GRSurface *drm_surface = (const GRSurface *)presented;
        source->version = sizeof(*source);
        source->width = drm_surface->width;
        source->height = drm_surface->height;
        source->stride = drm_surface->row_bytes / drm_surface->pixel_bytes;
        source->data = drm_surface->data;
        source->format = drm_surface->format;
    }
    return source->data != NULL && source->width != 0 &&
           source->height != 0 && source->stride != 0;
}

static bool gr_rgb_pixel(const GGLSurface *source, uint32_t source_x,
                         uint32_t source_y, uint8_t *destination)
{
    const uint32_t pixel_bytes =
        source->format == GGL_PIXEL_FORMAT_RGB_565 ? 2 : 4;
    const uint8_t *src = source->data +
        ((size_t)source_y * source->stride + source_x) * pixel_bytes;
    if (source->format == GGL_PIXEL_FORMAT_BGRA_8888) {
        destination[0] = src[2]; destination[1] = src[1]; destination[2] = src[0];
    } else if (source->format == GGL_PIXEL_FORMAT_RGBA_8888 ||
               source->format == GGL_PIXEL_FORMAT_RGBX_8888) {
        destination[0] = src[0]; destination[1] = src[1]; destination[2] = src[2];
    } else if (source->format == GGL_PIXEL_FORMAT_RGB_565) {
        const uint16_t pixel = src[0] | ((uint16_t)src[1] << 8);
        destination[0] = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
        destination[1] = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
        destination[2] = (uint8_t)((pixel & 0x1f) * 255 / 31);
    } else {
        return false;
    }
    return true;
}

static bool gr_rgb_row(const GGLSurface *source, uint32_t source_y,
                       uint32_t output_width, uint8_t *destination)
{
    for (uint32_t x = 0; x < output_width; ++x) {
        const uint32_t source_x = (uint32_t)((uint64_t)x * source->width /
                                              output_width);
        if (!gr_rgb_pixel(source, source_x, source_y, destination + x * 3))
            return false;
    }
    return true;
}

int gr_capture_scaled_bgra(unsigned int max_width, unsigned int max_height,
                           unsigned char **pixels, unsigned int *width,
                           unsigned int *height)
{
    if (!pixels || !width || !height)
        return -1;
    *pixels = NULL;
    *width = 0;
    *height = 0;

    GGLSurface source;
    if (!gr_capture_source(&source))
        return -1;

    uint32_t output_width = source.width;
    uint32_t output_height = source.height;
    if (max_width > 0 && output_width > max_width) {
        output_width = max_width;
        output_height = (uint32_t)(((uint64_t)source.height * output_width +
                                    source.width / 2) / source.width);
    }
    if (max_height > 0 && output_height > max_height) {
        output_height = max_height;
        output_width = (uint32_t)(((uint64_t)source.width * output_height +
                                   source.height / 2) / source.height);
    }
    if (output_width == 0 || output_height == 0 ||
        output_width > SIZE_MAX / 4 / output_height)
        return -1;

    uint8_t *output = (uint8_t *)malloc((size_t)output_width *
                                         output_height * 4);
    if (!output)
        return -1;

    for (uint32_t y = 0; y < output_height; ++y) {
        const uint32_t source_y = (uint32_t)(
            (uint64_t)y * source.height / output_height);
        for (uint32_t x = 0; x < output_width; ++x) {
            const uint32_t source_x = (uint32_t)(
                (uint64_t)x * source.width / output_width);
            uint8_t rgb[3];
            if (!gr_rgb_pixel(&source, source_x, source_y, rgb)) {
                free(output);
                return -1;
            }
            uint8_t *destination = output +
                ((size_t)y * output_width + x) * 4;
            destination[0] = rgb[2];
            destination[1] = rgb[1];
            destination[2] = rgb[0];
            destination[3] = 0xff;
        }
    }

    *pixels = output;
    *width = output_width;
    *height = output_height;
    return 0;
}

static bool gr_rgb_row_rotated(const GGLSurface *source, uint32_t output_y,
                               uint32_t output_width, uint32_t output_height,
                               unsigned int rotation, uint8_t *destination)
{
    const uint32_t rotated_width =
        rotation == 90 || rotation == 270 ? source->height : source->width;
    const uint32_t rotated_height =
        rotation == 90 || rotation == 270 ? source->width : source->height;
    const uint32_t ry = (uint32_t)((uint64_t)output_y * rotated_height /
                                    output_height);
    for (uint32_t x = 0; x < output_width; ++x) {
        const uint32_t rx = (uint32_t)((uint64_t)x * rotated_width /
                                       output_width);
        uint32_t sx = rx;
        uint32_t sy = ry;
        if (rotation == 90) {
            sx = ry;
            sy = source->height - rx - 1;
        } else if (rotation == 180) {
            sx = source->width - rx - 1;
            sy = source->height - ry - 1;
        } else if (rotation == 270) {
            sx = source->width - ry - 1;
            sy = rx;
        }
        if (!gr_rgb_pixel(source, sx, sy, destination + x * 3))
            return false;
    }
    return true;
}

static int gr_save_screenshot_internal(const char *dest,
                                       unsigned int max_width,
                                       bool fast,
                                       unsigned int rotation)
{
    uint32_t y;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t oriented_width = 0;
    uint32_t oriented_height = 0;
    volatile int res = -1;
    uint8_t * volatile png_row = NULL;
    FILE * volatile fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    GGLSurface capture_source;

    // DRM direct scanout does not use minui's legacy gr_draw surface. Use
    // the dimensions and mapped pixels of the actually presented buffer;
    // dereferencing gr_draw here caused a null-pointer crash in AERA's GPU
    // UI and left a zero-byte PNG behind.
    if (!gr_capture_source(&capture_source))
        goto exit;

    if (!(rotation == 0 || rotation == 90 || rotation == 180 ||
          rotation == 270))
        goto exit;
    oriented_width = rotation == 90 || rotation == 270
        ? capture_source.height : capture_source.width;
    oriented_height = rotation == 90 || rotation == 270
        ? capture_source.width : capture_source.height;
    output_width = max_width > 0 && oriented_width > max_width
        ? max_width : oriented_width;
    output_height = output_width == oriented_width
        ? oriented_height
        : (uint32_t)(((uint64_t)oriented_height * output_width +
                      oriented_width / 2) / oriented_width);

    fp = fopen(dest, "wb");
    if(!fp)
        goto exit;

    png_row = (uint8_t *)malloc((size_t)output_width * 3);
    if (!png_row) {
        printf("gr_save_screenshot failed to allocate PNG row\n");
        goto exit;
    }

    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr)
        goto exit;

    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL)
        goto exit;

    if (setjmp(png_jmpbuf(png_ptr)))
        goto exit;

    png_init_io(png_ptr, fp);
    if (fast) {
        png_set_compression_level(png_ptr, 1);
        png_set_filter(png_ptr, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
    }
    png_set_IHDR(png_ptr, info_ptr, output_width, output_height,
         8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
         PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png_ptr, info_ptr);

    // AERA presents a mapped DRM scanout buffer. Converting that buffer
    // directly avoids Pixelflinger, whose GGL context is not valid while the
    // LVGL/Adreno UI owns direct scanout. The old GGL path either returned an
    // empty image or dereferenced a null legacy draw surface.
    for (y = 0; y < output_height; ++y) {
        if (!gr_rgb_row_rotated(&capture_source, y, output_width,
                                output_height, rotation,
                                (uint8_t *)png_row))
            goto exit;
        png_write_row(png_ptr, (png_bytep)png_row);
    }

    png_write_end(png_ptr, NULL);

    res = 0;
exit:
    if(info_ptr)
        png_free_data(png_ptr, info_ptr, PNG_FREE_ALL, -1);
    if(png_ptr)
        png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
    if(png_row)
        free((void *)png_row);
    if(fp)
        fclose(fp);
    if (res != 0)
        remove(dest);
    return res;
}

int gr_save_screenshot(const char *dest)
{
    return gr_save_screenshot_internal(dest, 0, false, 0);
}

int gr_save_screenshot_rotated(const char *dest, unsigned int rotation)
{
    return gr_save_screenshot_internal(dest, 0, false, rotation);
}

int gr_save_screenshot_scaled_fast(const char *dest, unsigned int max_width)
{
    return gr_save_screenshot_internal(dest, max_width, true, 0);
}

int gr_save_screenshot_scaled_jpeg(const char *dest, unsigned int max_width,
                                   int quality)
{
    GGLSurface source;
    if (!gr_capture_source(&source))
        return -1;
    const uint32_t width = max_width > 0 && source.width > max_width
        ? max_width : source.width;
    const uint32_t height = width == source.width ? source.height :
        (uint32_t)(((uint64_t)source.height * width + source.width / 2) /
                   source.width);
    FILE *file = fopen(dest, "wb");
    if (!file)
        return -1;
    uint8_t *row = (uint8_t *)malloc((size_t)width * 3);
    if (!row) {
        fclose(file);
        remove(dest);
        return -1;
    }

    jpeg_compress_struct encoder = {};
    aera_jpeg_error error = {};
    encoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = aera_jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&encoder);
        free(row);
        fclose(file);
        remove(dest);
        return -1;
    }
    jpeg_create_compress(&encoder);
    jpeg_stdio_dest(&encoder, file);
    encoder.image_width = width;
    encoder.image_height = height;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    if (quality < 35) quality = 35;
    if (quality > 95) quality = 95;
    jpeg_set_quality(&encoder, quality, TRUE);
    encoder.dct_method = JDCT_IFAST;
    jpeg_start_compress(&encoder, TRUE);
    while (encoder.next_scanline < encoder.image_height) {
        const uint32_t source_y = (uint32_t)(
            (uint64_t)encoder.next_scanline * source.height / height);
        if (!gr_rgb_row(&source, source_y, width, row)) {
            jpeg_abort_compress(&encoder);
            jpeg_destroy_compress(&encoder);
            free(row);
            fclose(file);
            remove(dest);
            return -1;
        }
        JSAMPROW scanline = row;
        jpeg_write_scanlines(&encoder, &scanline, 1);
    }
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    free(row);
    fclose(file);
    return 0;
}

int ROTATION_X_DISP(int x, int y, int w) {
    return ((gr_rotation ==   0) ? (x) :
            (gr_rotation ==  90) ? (w - (y) - 1) :
            (gr_rotation == 180) ? (w - (x) - 1) :
            (gr_rotation == 270) ? (y) : -1);
}

int ROTATION_Y_DISP(int x, int y, int h) {
    return ((gr_rotation ==   0) ? (y) :
            (gr_rotation ==  90) ? (x) :
            (gr_rotation == 180) ? (h - (y) - 1) :
            (gr_rotation == 270) ? (h - (x) - 1) : -1);
}

#define MATRIX_ELEMENT(matrix, row, col, row_size, elem_size) \
    (((uint8_t*) (matrix)) + (((row) * (elem_size)) * (row_size)) + ((col) * (elem_size)))

#define DO_MATRIX_ROTATION(bits_per_pixel, bytes_per_pixel)                   \
{                                                                             \
    for (size_t y = 0; y < src->height; y++) {                                \
        for (size_t x = 0; x < src->width; x++) {                             \
            /* output pointer in dst->data */                                 \
            uint##bits_per_pixel##_t       *op;                               \
            /* input pointer from src->data */                                \
            const uint##bits_per_pixel##_t *ip;                               \
            /* Display coordinates (in dst) corresponding to (x, y) in src */ \
            size_t x_disp = ROTATION_X_DISP(x, y, dst->width);                \
            size_t y_disp = ROTATION_Y_DISP(x, y, dst->height);               \
                                                                              \
            ip = (const uint##bits_per_pixel##_t*)                            \
                 MATRIX_ELEMENT(src->data, y, x,                              \
                                src->stride, bytes_per_pixel);                \
            op = (uint##bits_per_pixel##_t*)                                  \
                 MATRIX_ELEMENT(dst->data, y_disp, x_disp,                    \
                                dst->stride, bytes_per_pixel);                \
            *op = *ip;                                                        \
        }                                                                     \
    }                                                                         \
}

void surface_ROTATION_transform(gr_surface dst_ptr, const gr_surface src_ptr,
                                  size_t num_bytes_per_pixel)
{
    GGLSurface *dst = (GGLSurface*) dst_ptr;
    const GGLSurface *src = (GGLSurface*) src_ptr;

    /* Handle duplicated code via a macro.
     * This is currently used for rotating surfaces of graphical resources
     * (32-bit pixel format) and of font glyphs (8-bit pixel format).
     * If you need to add handling of other pixel formats feel free to do so.
     */
    if (num_bytes_per_pixel == 4) {
        DO_MATRIX_ROTATION(32, 4);
    } else if (num_bytes_per_pixel == 1) {
        DO_MATRIX_ROTATION(8, 1);
    }
}

void gr_draw_rect(int x, int y, int w, int h, int thickness) {
    // Top border
    gr_line(x, y, x + w - 1, y, thickness);
    // Left border
    gr_line(x, y, x, y + h - 1, thickness);
    // Right border
    gr_line(x + w - 1, y, x + w - 1, y + h - 1, thickness);
    // Bottom border
    gr_line(x, y + h - 1, x + w - 1, y + h - 1, thickness);
}
