/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <aeraui/i18n.hpp>

#include "fonts/aera_fallback.hpp"
#include "catalog/catalog.hpp"

#include <algorithm>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef AERA_DEFAULT_LANGUAGE
#define AERA_DEFAULT_LANGUAGE "en"
#endif

namespace aeraui::i18n {

namespace {

const std::vector<Language> kLanguages = {
    {"en", "English", "English", false},
#ifdef AERA_EXTRA_LANGUAGES
    {"ar_SA", "Arabic", "العربية", true},
    {"bg_BG", "Bulgarian", "Български", false},
    {"bn_BD", "Bengali", "বাংলা", false},
    {"ca_ES", "Catalan", "Català", false},
    {"cs_CZ", "Czech", "Čeština", false},
    {"de_DE", "German", "Deutsch", false},
    {"el_GR", "Greek", "Ελληνικά", false},
    {"es-ES", "Spanish", "Español", false},
    {"fa_IR", "Persian", "فارسی", true},
    {"fr_FR", "French", "Français", false},
    {"he_IL", "Hebrew", "עברית", true},
    {"hi_IN", "Hindi", "हिन्दी", false},
    {"hu", "Hungarian", "Magyar", false},
    {"id_ID", "Indonesian", "Bahasa Indonesia", false},
    {"it_IT", "Italian", "Italiano", false},
    {"ja_JP", "Japanese", "日本語", false},
    {"ko_KR", "Korean", "한국어", false},
    {"nl_NL", "Dutch", "Nederlands", false},
    {"no_NO", "Norwegian", "Norsk", false},
    {"pl_PL", "Polish", "Polski", false},
    {"pt_BR", "Portuguese (Brazil)", "Português (Brasil)", false},
    {"pt_PT", "Portuguese (Portugal)", "Português (Portugal)", false},
    {"ro_RO", "Romanian", "Română", false},
    {"ru", "Russian", "Русский", false},
    {"sr_Cyrl", "Serbian", "Српски", false},
    {"sv_SE", "Swedish", "Svenska", false},
    {"th_TH", "Thai", "ไทย", false},
    {"tr_TR", "Turkish", "Türkçe", false},
    {"uk_UA", "Ukrainian", "Українська", false},
    {"vi_VN", "Vietnamese", "Tiếng Việt", false},
    {"zh_CN", "Chinese (Simplified)", "简体中文", false},
    {"zh_TW", "Chinese (Traditional)", "繁體中文", false},
#endif
};

bool initialized = false;
const Language *current = &kLanguages.front();

std::string Normalize(std::string code) {
  std::replace(code.begin(), code.end(), '-', '_');
  std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return code;
}

const Language *FindLanguage(const std::string &requested) {
  const std::string normalized = Normalize(requested);
  for (const auto &language : kLanguages) {
    if (Normalize(language.code) == normalized) return &language;
  }
  const auto separator = normalized.find('_');
  const std::string base = normalized.substr(0, separator);
  for (const auto &language : kLanguages) {
    const std::string candidate = Normalize(language.code);
    if (candidate == base || candidate.rfind(base + "_", 0) == 0)
      return &language;
  }
  return &kLanguages.front();
}

}  // namespace

void Initialize(const std::string &requested_language) {
  if (!initialized) {
    if (!RegisterCatalog()) {
      std::fprintf(stderr, "AERA: could not load the built-in translation catalog\n");
    }
    initialized = true;
  }
  SetLanguage(requested_language);
}

bool SetLanguage(const std::string &requested_language) {
  if (!initialized) Initialize(AERA_DEFAULT_LANGUAGE);
  const Language *selected = FindLanguage(requested_language);
  current = selected;
  const std::string locale_environment = std::string(selected->code) + ".UTF-8";
  setenv("AERA_LOCALE", selected->code, 1);
  setenv("LANG", locale_environment.c_str(), 1);
  fonts::SetFallbackLanguage(selected->code);
  lv_translation_set_language(selected->code);
  return Normalize(selected->code) == Normalize(requested_language) ||
         Normalize(selected->code).rfind(
             Normalize(requested_language) + "_", 0) == 0;
}

const char *CurrentLanguage() { return current->code; }

const std::vector<Language> &AvailableLanguages() { return kLanguages; }

bool IsRightToLeft() { return current->rtl; }

void ApplyDirection(lv_obj_t *object) {
  if (object == nullptr) return;
  lv_obj_set_style_base_dir(object,
                            IsRightToLeft() ? LV_BASE_DIR_RTL
                                            : LV_BASE_DIR_LTR,
                            LV_PART_MAIN);
}

void BindLabel(lv_obj_t *label, const char *tag) {
  if (label == nullptr) return;
  if (tag == nullptr || *tag == '\0') {
    lv_label_set_text(label, "");
    return;
  }
  const auto *bytes = reinterpret_cast<const unsigned char *>(tag);
  const bool leading_private_use_icon =
      bytes[0] == 0xef && bytes[1] >= 0x80 && bytes[1] <= 0xa3 &&
      (bytes[2] & 0xc0) == 0x80;
  if (leading_private_use_icon) {
    const char *text = tag + 3;
    const char *content = text;
    while (*content == ' ' || *content == '\n' || *content == '\t') ++content;
    if (*content != '\0') {
      std::string translated(tag, static_cast<size_t>(content - tag));
      translated += Translate(content);
      lv_label_set_text(label, translated.c_str());
      return;
    }
  }
  lv_label_set_translation_tag(label, tag);
}

const char *Translate(const char *tag) {
  return lv_tr(tag == nullptr ? "" : tag);
}

std::string Format(const char *tag, ...) {
  const char *format = Translate(tag);
  va_list arguments;
  va_start(arguments, tag);
  va_list copy;
  va_copy(copy, arguments);
  const int length = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (length < 0) {
    va_end(arguments);
    return format;
  }
  std::string result(static_cast<size_t>(length) + 1, '\0');
  vsnprintf(result.data(), result.size(), format, arguments);
  va_end(arguments);
  result.resize(static_cast<size_t>(length));
  return result;
}

}  // namespace aeraui::i18n
