/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "aera_fallback.hpp"
#include "font_staging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#ifndef AERA_FONT_ROOT
#define AERA_FONT_ROOT "/twres/fonts"
#endif
#ifndef AERA_COMMON_FONT_ROOT
#define AERA_COMMON_FONT_ROOT AERA_FONT_ROOT
#endif

namespace aeraui::fonts {
namespace {

constexpr std::array<int, 9> kSizes{16, 18, 20, 24, 28, 32, 36, 40, 48};
constexpr std::array<const lv_font_t *, 9> kBaseFonts{
    &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20,
    &lv_font_montserrat_24, &lv_font_montserrat_28, &lv_font_montserrat_32,
    &lv_font_montserrat_36, &lv_font_montserrat_40, &lv_font_montserrat_48};

enum class Script : size_t {
  kGeneral,
  kArabic,
  kHebrew,
  kDevanagari,
  kBengali,
  kThai,
  kJapanese,
  kCjk,
  kCount,
};

constexpr std::array<const char *, static_cast<size_t>(Script::kCount)>
    kFontPaths{
        AERA_COMMON_FONT_ROOT "/Roboto-Regular.ttf",
        AERA_FONT_ROOT "/NotoNaskhArabicUI-Regular.ttf",
        AERA_FONT_ROOT "/NotoSansHebrew-Regular.ttf",
        AERA_FONT_ROOT "/NotoSansDevanagariUI-VF.ttf",
        AERA_FONT_ROOT "/NotoSansBengaliUI-VF.ttf",
        AERA_FONT_ROOT "/NotoSansThaiUI-Regular.ttf",
        AERA_FONT_ROOT "/NotoSansCJKjp-Regular.ttf",
        AERA_FONT_ROOT "/wqy-microhei.ttf",
    };

struct FontSize {
  lv_font_t wrapper{};
  lv_font_t symbols{};
  std::array<lv_font_t *, static_cast<size_t>(Script::kCount)> fallback{};
  bool external_primary = false;
  bool loaded = false;
};

std::array<FontSize, kSizes.size()> fonts;
struct DisplaySize {
  lv_font_t wrapper{};
  bool loaded = false;
};
std::map<uint32_t, DisplaySize> display_fonts;
std::map<std::pair<std::string, uint32_t>, lv_font_t *> external_fonts;
FontStaging staged_fonts;
bool initialized = false;
std::string language = "en";
std::string selected_font_id;
std::string selected_font_path;

std::string BaseLanguage(std::string code) {
  std::replace(code.begin(), code.end(), '-', '_');
  const auto separator = code.find('_');
  if (separator != std::string::npos) code.resize(separator);
  std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return code;
}

Script PrimaryScript(const std::string &code) {
  const std::string base = BaseLanguage(code);
  if (base == "ar" || base == "fa") return Script::kArabic;
  if (base == "he") return Script::kHebrew;
  if (base == "hi") return Script::kDevanagari;
  if (base == "bn") return Script::kBengali;
  if (base == "th") return Script::kThai;
  if (base == "ja") return Script::kJapanese;
  if (base == "zh" || base == "ko") return Script::kCjk;
  return Script::kGeneral;
}

void LinkFallbacks(FontSize &set, size_t index) {
  std::vector<Script> order{
      Script::kGeneral, Script::kArabic, Script::kHebrew,
      Script::kDevanagari, Script::kBengali, Script::kThai,
      Script::kJapanese, Script::kCjk};
  const Script primary = PrimaryScript(language);
  if (primary != Script::kGeneral) {
    order.erase(std::remove(order.begin(), order.end(), primary), order.end());
    order.insert(order.begin() + 1, primary);
  }
  lv_font_t *previous = nullptr;
  if (set.external_primary) {
    // Plugin fonts intentionally contain text only. Keep AERA/LVGL symbols
    // available before falling through to the language-specific typefaces.
    set.symbols = *kBaseFonts[index];
    set.wrapper.fallback = &set.symbols;
    previous = &set.symbols;
  }
  for (const Script script : order) {
    lv_font_t *font = set.fallback[static_cast<size_t>(script)];
    if (font == nullptr) continue;
    if (previous == nullptr)
      set.wrapper.fallback = font;
    else
      previous->fallback = font;
    previous = font;
  }
  if (previous != nullptr) previous->fallback = nullptr;
}

lv_font_t *LoadExternalFont(const std::string &path, uint32_t size) {
  if (path.empty() || size < 8 || size > 512) return nullptr;
  const auto key = std::make_pair(path, size);
  const auto existing = external_fonts.find(key);
  if (existing != external_fonts.end()) return existing->second;
  // Do not let FreeType pin /data (or removable storage), even for previews.
  const std::string staged_path = staged_fonts.Stage(path);
  if (staged_path.empty()) return nullptr;
  lv_font_t *font = lv_freetype_font_create(
      staged_path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size,
      LV_FREETYPE_FONT_STYLE_NORMAL);
  if (font != nullptr) external_fonts.emplace(key, font);
  return font;
}

const lv_font_t *PrimaryFont(size_t index) {
  if (!selected_font_path.empty()) {
    if (auto *font = LoadExternalFont(selected_font_path, kSizes[index]))
      return font;
  }
  return kBaseFonts[index];
}

void RefreshSize(size_t index) {
  auto &set = fonts[index];
  const lv_font_t *primary = PrimaryFont(index);
  set.wrapper = *primary;
  set.external_primary = primary != kBaseFonts[index];
  if (set.loaded) LinkFallbacks(set, index);
}

void LoadSize(size_t index) {
  auto &set = fonts[index];
  if (set.loaded) return;
  set.loaded = true;
  for (size_t script = 0; script < kFontPaths.size(); ++script) {
#ifndef AERA_EXTRA_LANGUAGES
    if (script != static_cast<size_t>(Script::kGeneral)) continue;
#endif
    set.fallback[script] = lv_freetype_font_create(
        kFontPaths[script], LV_FREETYPE_FONT_RENDER_MODE_BITMAP, kSizes[index],
        LV_FREETYPE_FONT_STYLE_NORMAL);
  }
  RefreshSize(index);
}

}  // namespace

void InitializeFallbackFonts() {
  if (initialized) return;
  initialized = true;
  for (size_t i = 0; i < kSizes.size(); ++i) RefreshSize(i);
}

void SetFallbackLanguage(const std::string &requested) {
  language = requested;
  InitializeFallbackFonts();
  for (size_t index = 0; index < fonts.size(); ++index)
    if (fonts[index].loaded) LinkFallbacks(fonts[index], index);
}

bool SelectUiFont(const std::string &id, const std::string &path) {
  InitializeFallbackFonts();
  if (id.empty() || path.empty()) {
    selected_font_id.clear();
    selected_font_path.clear();
  } else {
    if (LoadExternalFont(path, 24) == nullptr) return false;
    selected_font_id = id;
    selected_font_path = path;
  }
  for (size_t i = 0; i < fonts.size(); ++i) RefreshSize(i);
  for (auto &[size, display] : display_fonts) {
    const char *path = selected_font_path.empty()
        ? kFontPaths[static_cast<size_t>(Script::kGeneral)]
        : selected_font_path.c_str();
    if (auto *font = LoadExternalFont(path, size)) {
      display.wrapper = *font;
      display.loaded = true;
    }
  }
  return true;
}

const std::string &SelectedUiFont() { return selected_font_id; }

const lv_font_t *PreviewFont(const std::string &path, uint32_t size) {
  return LoadExternalFont(path, size);
}

const lv_font_t *WithLanguageFallback(const lv_font_t *font) {
  if (font == nullptr) return nullptr;
  InitializeFallbackFonts();
  for (size_t i = 0; i < kBaseFonts.size(); ++i) {
    if (font == kBaseFonts[i]) {
      LoadSize(i);
      return &fonts[i].wrapper;
    }
  }
  return font;
}

const lv_font_t *DisplayFont(uint32_t size) {
  InitializeFallbackFonts();
  auto [iterator, inserted] = display_fonts.try_emplace(size);
  (void)inserted;
  auto &display = iterator->second;
  if (!display.loaded) {
    const char *path = selected_font_path.empty()
        ? kFontPaths[static_cast<size_t>(Script::kGeneral)]
        : selected_font_path.c_str();
    if (auto *font = LoadExternalFont(path, size)) {
      display.wrapper = *font;
      display.loaded = true;
    }
  }
  return display.loaded ? &display.wrapper
                        : WithLanguageFallback(&lv_font_montserrat_48);
}

}  // namespace aeraui::fonts
