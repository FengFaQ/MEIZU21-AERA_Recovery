/* SPDX-License-Identifier: Apache-2.0 */
#include "catalog.hpp"

#include <lvgl.h>
#include <zlib.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace aeraui::i18n {
namespace {
uint32_t Read32(const unsigned char *data) {
  return static_cast<uint32_t>(data[0]) |
         static_cast<uint32_t>(data[1]) << 8 |
         static_cast<uint32_t>(data[2]) << 16 |
         static_cast<uint32_t>(data[3]) << 24;
}

struct Catalog {
  // LVGL keeps these pointers. Storage must outlive labels and language changes.
  std::vector<unsigned char> storage;
  std::vector<const char *> languages;
  std::vector<const char *> tags;
  std::vector<const char *> translations;
  bool registered = false;

  bool Load() {
    const auto packed = GetPackedCatalog();
    if (packed.expanded_size < 20) return false;
    storage.resize(packed.expanded_size);
    uLongf size = storage.size();
    if (uncompress(storage.data(), &size, packed.data, packed.compressed_size) != Z_OK ||
        size != storage.size() || std::memcmp(storage.data(), "AERACAT1", 8)) return false;
    const uint32_t language_count = Read32(storage.data() + 8);
    const uint32_t tag_count = Read32(storage.data() + 12);
    const uint32_t pool_size = Read32(storage.data() + 16);
    const uint64_t translation_count = static_cast<uint64_t>(language_count) * tag_count;
    const uint64_t entries = language_count + static_cast<uint64_t>(tag_count) + translation_count;
    if (entries > (storage.size() - 20) / 4) return false;
    const uint64_t pool_offset = 20 + entries * 4;
    if (!language_count || !tag_count || !pool_size ||
        pool_offset + pool_size != storage.size()) return false;
    const auto *table = storage.data() + 20;
    const auto *pool = reinterpret_cast<const char *>(storage.data() + pool_offset);
    for (uint64_t i = 0; i < entries; ++i) {
      const uint32_t offset = Read32(table + i * 4);
      if (offset >= pool_size || !std::memchr(pool + offset, 0, pool_size - offset)) return false;
    }
    languages.resize(language_count + 1, nullptr);
    tags.resize(tag_count + 1, nullptr);
    translations.resize(translation_count);
    size_t index = 0;
    for (uint32_t i = 0; i < language_count; ++i)
      languages[i] = pool + Read32(table + index++ * 4);
    for (uint32_t i = 0; i < tag_count; ++i)
      tags[i] = pool + Read32(table + index++ * 4);
    for (size_t i = 0; i < translations.size(); ++i)
      translations[i] = pool + Read32(table + index++ * 4);
    return true;
  }
};
}  // namespace

bool RegisterCatalog() {
  static Catalog catalog;
  if (catalog.registered) return true;
  if (!catalog.Load()) return false;
  catalog.registered = lv_translation_add_static(catalog.languages.data(),
      catalog.tags.data(), catalog.translations.data()) != nullptr;
  return catalog.registered;
}
}  // namespace aeraui::i18n
