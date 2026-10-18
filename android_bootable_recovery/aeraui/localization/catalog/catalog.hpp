/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstddef>

namespace aeraui::i18n {
struct PackedCatalog {
  const unsigned char *data;
  size_t compressed_size;
  size_t expanded_size;
};

PackedCatalog GetPackedCatalog();
bool RegisterCatalog();
}  // namespace aeraui::i18n
