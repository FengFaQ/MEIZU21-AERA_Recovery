/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <string>
#include <vector>

#include <lvgl.h>

namespace aeraui::i18n {

struct Language {
  const char *code;
  const char *name;
  const char *native_name;
  bool rtl;
};

void Initialize(const std::string &requested_language);
bool SetLanguage(const std::string &requested_language);
const char *CurrentLanguage();
const std::vector<Language> &AvailableLanguages();
bool IsRightToLeft();
void ApplyDirection(lv_obj_t *object);
void BindLabel(lv_obj_t *label, const char *tag);
const char *Translate(const char *tag);
std::string Format(const char *tag, ...);

}  // namespace aeraui::i18n
