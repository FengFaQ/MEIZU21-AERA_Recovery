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

#ifndef _MINUI_H_
#define _MINUI_H_

#include "minuitwrp/placement.h"
#include <stdbool.h>

struct GRSurface {
    int width;
    int height;
    int row_bytes;
    int pixel_bytes;
    unsigned char* data;
    __u32 format;
};

typedef void* gr_surface;
typedef unsigned short gr_pixel;

// Metadata for importing a minui DRM scanout buffer into a GPU API. The
// returned dma-buf descriptor belongs to the caller and must be closed.
struct GRDrmBufferInfo {
    int dma_buf_fd;
    int width;
    int height;
    int pitch;
    __u32 drm_format;
};

#define FONT_TYPE_TWRP 0
#define FONT_TYPE_TTF  1

int gr_init(void);
void gr_exit(void);

int gr_fb_width(void);
int gr_fb_height(void);
gr_pixel *gr_fb_data(void);
void gr_flip(void);
void gr_fb_blank(bool blank);

int gr_drm_get_scanout_buffers(gr_surface* buffer1, gr_surface* buffer2);
#ifdef __cplusplus
extern "C"
#endif
int gr_drm_export_scanout_buffer(gr_surface buffer, GRDrmBufferInfo* info);
#ifdef __cplusplus
extern "C"
#endif
int gr_drm_import_scanout_buffer(const GRDrmBufferInfo* info,
                                 gr_surface* buffer);
#ifdef __cplusplus
extern "C"
#endif
int gr_drm_release_imported_scanout_buffer(gr_surface buffer);
int gr_drm_present(gr_surface buffer);
gr_surface gr_drm_get_presented_surface(void);

void gr_color(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void gr_clip(int x, int y, int w, int h);
// Nested clipping: gr_clip_push intersects (x,y,w,h) with the currently
// active clip and makes it the active region; gr_clip_pop restores the
// previous one. While a region is pushed, plain gr_clip()/gr_noclip() calls
// are intersected with / reset to it, so a child widget that sets its own clip
// (e.g. a scroll list inside a scroll container) stays confined to the parent.
void gr_clip_push(int x, int y, int w, int h);
void gr_clip_pop();
void gr_noclip();
void gr_fill(int x, int y, int w, int h);
void gr_line(int x0, int y0, int x1, int y1, int width);
gr_surface gr_render_circle(int radius, unsigned char r, unsigned char g, unsigned char b, unsigned char a);

int gr_textEx_scaleW(int x, int y, const char *s, void* pFont, int max_width, int placement, int scale);

int gr_getMaxFontHeight(void *font);

void *gr_ttf_loadFont(const char *filename, int size, int dpi);
void *gr_ttf_scaleFont(void *font, int max_width, int measured_width);
void gr_ttf_freeFont(void *font);
int gr_ttf_textExWH(void *context, int x, int y, const char *s, void *pFont,
                    int max_width, int max_height, const gr_surface gr_draw);
int gr_ttf_measureEx(const char *s, void *font);
int gr_ttf_maxExW(const char *s, void *font, int max_width);
int gr_ttf_getMaxFontHeight(void *font);
void gr_ttf_dump_stats(void);

void gr_blit(gr_surface source, int sx, int sy, int w, int h, int dx, int dy);
void gr_blit_rotated(gr_surface source, int sx, int sy, int w, int h, int dx, int dy, int angle);
unsigned int gr_get_width(gr_surface surface);
unsigned int gr_get_height(gr_surface surface);
int gr_get_surface(gr_surface* surface);
int gr_free_surface(gr_surface surface);

// Functions in graphics_utils.c
int gr_save_screenshot(const char *dest);
int gr_save_screenshot_rotated(const char *dest, unsigned int rotation);
int gr_save_screenshot_scaled_fast(const char *dest, unsigned int max_width);
int gr_save_screenshot_scaled_jpeg(const char *dest, unsigned int max_width,
                                   int quality);
int gr_capture_scaled_bgra(unsigned int max_width, unsigned int max_height,
                           unsigned char **pixels, unsigned int *width,
                           unsigned int *height);
void gr_draw_rect(int x, int y, int w, int h, int thickness);

// Transform minuitwrp API coordinates into display coordinates,
// for panels that are hardware-mounted in a rotated manner.
int ROTATION_X_DISP(int x, int y, int w);

int ROTATION_Y_DISP(int x, int y, int h);

void surface_ROTATION_transform(gr_surface dst_ptr, const gr_surface src_ptr, size_t num_bytes_per_pixel);

// input event structure, include <linux/input.h> for the definition.
// see http://www.mjmwired.net/kernel/Documentation/input/ for info.
struct input_event;

int ev_init(void);
void ev_exit(void);
int ev_get(struct input_event *ev, int timeout_ms);
int ev_has_mouse(void);
unsigned ev_input_generation(void);

// Resources

// Returns 0 if no error, else negative.
int res_create_surface(const char* name, gr_surface* pSurface);
int res_get_pixel_format(void);
void res_free_surface(gr_surface surface);
int res_scale_surface(gr_surface source, gr_surface* destination, float scale_w, float scale_h);

int vibrate(int timeout_ms);

#endif
