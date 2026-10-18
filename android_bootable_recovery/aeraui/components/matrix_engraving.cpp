/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "matrix_engraving.hpp"

#include <algorithm>
#include <string>

#include "design.hpp"

namespace aeraui {

void AddMatrixEngraving(lv_obj_t *parent, int width, int height,
                        lv_color_t accent) {
  using namespace design;
  static constexpr const char *kStreams[] = {
      "0\n1\nA\n7\n/\nR\n0\n1\n{\n9\nE\n3\n}",
      "R\n4\n>\n0\n1\nA\n8\n/\n2\nE\n1\n;\n6",
      "1\n0\nF\n5\nA\n{\n3\nR\n9\n}\n0\n1\nE",
      "A\nE\n2\n/\n7\nR\n1\n0\n>\n4\n;\n8\nA",
      "7\n{\n0\n1\nE\n5\nR\n/\nA\n3\n}\n9\n0",
      "0\nA\n1\nR\n6\n>\nE\n2\n/\n8\n1\n0\n;",
      "E\n3\n/\nA\n0\n1\nR\n7\n{\n4\n9\n}\n1",
      "1\nR\n8\n0\n>\nA\n5\nE\n/\n2\n0\n1\n6",
      "A\n4\n{\nE\n1\n0\n7\nR\n}\n/\n3\n9\n0",
      "R\n0\n1\n6\nA\n/\n>\nE\n8\n2\n;\n1\n0",
      "0\nE\n5\n1\nR\n{\nA\n3\n/\n}\n7\n0\n1",
      "A\n1\n/\n9\n0\nE\n4\nR\n>\n1\n8\n;\n2",
      "E\n7\nR\n0\n1\nA\n{\n5\n}\n/\n3\n0\n9",
      "1\n0\nA\n2\n/\nR\n8\nE\n>\n6\n1\n;\n4",
      "R\n5\nE\n{\n0\n1\nA\n9\n/\n3\n}\n7\n0",
      "0\nA\n6\n1\nE\n>\nR\n4\n/\n8\n0\n1\n;",
      "A\nE\n0\n7\nR\n/\n1\n{\n5\n}\n9\n0\n3",
      "1\nR\n4\n0\nA\nE\n>\n8\n/\n2\n1\n;\n6",
  };
  constexpr int kStreamRows = 13;
  constexpr int kStreamCount =
      static_cast<int>(sizeof(kStreams) / sizeof(kStreams[0]));

  const int column_count = std::clamp(width / 46, 14, 30);
  // The engraving is generated against an exact 18 px cell grid. Applying
  // interface Density to it changes line metrics without changing the fixed
  // artwork bounds, which folds and clips the streams.
  const int font_height = lv_font_get_line_height(&lv_font_montserrat_18);
  const int row_count = std::max(
      kStreamRows, (height + font_height + 7) / (font_height + 8) + 1);
  std::string fields[3];
  for (int row = 0; row < row_count; ++row) {
    for (int group = 0; group < 3; ++group) {
      for (int column = 0; column < column_count; ++column) {
        const char *stream = kStreams[column % kStreamCount];
        fields[group].push_back(column % 3 == group
            ? stream[(row % kStreamRows) * 2] : ' ');
      }
      if (row + 1 != row_count) fields[group].push_back('\n');
    }
  }

  const int letter_space =
      std::max(10, (width - 36) / (column_count - 1) - 7);
  auto add_field = [&](int group, lv_color_t color, lv_opa_t opacity,
                       int x_offset, int y_offset) {
    auto *field = FixedLabel(parent, fields[group].c_str(),
                             &lv_font_montserrat_18, color);
    lv_obj_set_pos(field, 10 + x_offset, 4 + group * 3 + y_offset);
    lv_obj_set_size(field, width - 20, height - 4);
    lv_obj_set_style_text_align(field, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(field, letter_space, 0);
    lv_obj_set_style_text_line_space(field, 8, 0);
    lv_obj_set_style_text_opa(field, opacity, 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_CLICKABLE);
  };

  for (int group = 0; group < 3; ++group) {
    const lv_opa_t depth = static_cast<lv_opa_t>(108 + group * 18);
    add_field(group, kMainCanvas, depth, 0, 0);
  }
  for (int group = 0; group < 3; ++group) {
    add_field(group, group == 2 ? accent : kText,
              static_cast<lv_opa_t>(group == 2 ? 78 : 54), 1, 2);
  }
}

}  // namespace aeraui
