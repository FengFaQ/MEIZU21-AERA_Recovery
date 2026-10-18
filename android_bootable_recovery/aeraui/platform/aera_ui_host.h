// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int gui_init(void);
int gui_loadResources(void);
int gui_loadCustomResources(void);
int aeraui_is_active(void);
int gui_start(void);
int gui_startPage(const char* page_name, int allow_commands,
                  int stop_on_page_done);

void gui_print(const char* format, ...);
void gui_print_color(const char* color, const char* format, ...);
void gui_set_FILE(FILE* file);

void gui_aera_progress_overall(int percent);
void gui_aera_progress_item(int percent);
void gui_aera_progress_detail(const char* phase, int percent,
                              const char* label,
                              unsigned long long current_bytes,
                              unsigned long long total_bytes,
                              unsigned long long bytes_per_second,
                              unsigned long long eta_seconds,
                              unsigned long long current_files,
                              unsigned long long total_files,
                              const char* size_text,
                              const char* file_text);

void set_scale_values(float width, float height);
int scale_theme_x(int value);
int scale_theme_y(int value);
int scale_theme_min(int value);
float get_scale_w(void);
float get_scale_h(void);

#ifdef __cplusplus
}
#endif
