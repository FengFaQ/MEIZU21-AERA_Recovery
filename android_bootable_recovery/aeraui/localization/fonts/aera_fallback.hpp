/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <string>

#include <lvgl.h>

namespace aeraui::fonts {

void InitializeFallbackFonts();
void SetFallbackLanguage(const std::string &language);
bool SelectUiFont(const std::string &id, const std::string &path);
const std::string &SelectedUiFont();
const lv_font_t *PreviewFont(const std::string &path, uint32_t size);
const lv_font_t *WithLanguageFallback(const lv_font_t *font);
const lv_font_t *DisplayFont(uint32_t size);

}  // namespace aeraui::fonts
