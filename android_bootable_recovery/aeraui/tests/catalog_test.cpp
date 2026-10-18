/* SPDX-License-Identifier: Apache-2.0 */
#include <aeraui/i18n.hpp>
#include "localization/catalog/catalog.hpp"

#include <json/json.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace aeraui::fonts {
void SetFallbackLanguage(const std::string &) {}
}

int main(int argc, char **argv) {
  assert(argc == 2);
  std::ifstream source(argv[1]);
  Json::Value catalog;
  source >> catalog;
  assert(catalog["schema"].asInt() == 1);
  lv_init();
  lv_display_create(1440, 3168);
  aeraui::i18n::Initialize("en");
  assert(aeraui::i18n::RegisterCatalog());
  const auto &languages = aeraui::i18n::AvailableLanguages();
#ifdef AERA_EXTRA_LANGUAGES
  assert(languages.size() == catalog["languages"].size());
#else
  assert(languages.size() == 1 && std::strcmp(languages[0].code, "en") == 0);
#endif
  size_t checked = 0;
  const char *retained = nullptr;
  std::string retained_copy;
  auto *label = lv_label_create(lv_screen_active());
  auto *reference = lv_label_create(lv_screen_active());
  aeraui::i18n::BindLabel(label, "Paste");
  for (const auto &language : languages) {
    assert(aeraui::i18n::SetLanguage(language.code));
    assert(aeraui::i18n::CurrentLanguage() == std::string(language.code));
    assert(std::strcmp(lv_translation_get_language(), language.code) == 0);
    const auto &strings = catalog["strings"];
    for (const auto &tag : strings.getMemberNames()) {
      std::string expected = std::strcmp(language.code, "en") == 0 ? tag :
          strings[tag].get(language.code, tag).asString();
      if (expected.empty()) expected = tag;
      const auto *actual = aeraui::i18n::Translate(tag.c_str());
      if (expected != actual) {
        std::fprintf(stderr, "Translation mismatch: %s / %s\n", language.code, tag.c_str());
        return 1;
      }
      ++checked;
    }
    // LVGL shapes Arabic/Persian text before storing it in a label.
    lv_label_set_text(reference, aeraui::i18n::Translate("Paste"));
    assert(std::strcmp(lv_label_get_text(label), lv_label_get_text(reference)) == 0);
    if (retained) assert(retained_copy == retained);
    else { retained = aeraui::i18n::Translate("Paste"); retained_copy = retained; }
  }
  assert(std::strcmp(aeraui::i18n::Translate("missing catalog tag"), "missing catalog tag") == 0);
  assert(std::strcmp(aeraui::i18n::Translate(nullptr), "") == 0);
  assert(aeraui::i18n::RegisterCatalog());
  std::printf("Packed catalog: %zu translations and live language switching passed.\n", checked);
  lv_deinit();
}
