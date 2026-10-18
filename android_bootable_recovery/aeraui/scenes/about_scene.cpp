/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"

#include <algorithm>
#include <string>

#include "aera_logo.hpp"
#include "matrix_engraving.hpp"
#include "ui_components.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

std::string OrUnknown(std::string value) {
  return value.empty() ? "Unknown" : value;
}

lv_obj_t *BrandLayer(lv_obj_t *parent, const lv_image_dsc_t *source,
                     int scale, lv_color_t color) {
  auto *logo = lv_image_create(parent);
  lv_image_set_src(logo, source);
  lv_image_set_antialias(logo, true);
  lv_image_set_scale(logo, scale);
  lv_obj_set_style_image_recolor(logo, color, 0);
  lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
  lv_obj_set_style_image_opa(logo, LV_OPA_COVER, 0);
  lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE);
  return logo;
}

void Contributor(lv_obj_t *parent, int x, int y, int width,
                 const char *initial, const char *name, const char *role,
                 uint32_t delay) {
  auto *card = lv_obj_create(parent);
  Panel(card, 34, kMainSheet);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, 232);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);

  auto *avatar = lv_obj_create(card);
  Panel(avatar, LV_RADIUS_CIRCLE, kAccentSoft);
  lv_obj_set_pos(avatar, 34, 42);
  lv_obj_set_size(avatar, 146, 146);
  lv_obj_set_style_border_width(avatar, 2, 0);
  lv_obj_set_style_border_color(avatar, kAccent, 0);
  lv_obj_set_style_border_opa(avatar, LV_OPA_50, 0);
  auto *letter = Label(avatar, initial, &lv_font_montserrat_48, kAccent);
  lv_obj_set_style_transform_scale(letter, 310, 0);
  lv_obj_center(letter);

  auto *person = Label(card, name, &lv_font_montserrat_40, kText);
  lv_obj_set_pos(person, 214, 50);
  lv_obj_set_width(person, width - 250);
  lv_label_set_long_mode(person, LV_LABEL_LONG_DOT);
  auto *work = Label(card, role, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(work, 216, 124);
  lv_obj_set_width(work, width - 250);
  lv_label_set_long_mode(work, LV_LABEL_LONG_WRAP);

  auto *accent = lv_obj_create(card);
  Clear(accent);
  lv_obj_set_pos(accent, 216, 179);
  lv_obj_set_size(accent, 112, 5);
  lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(accent, kAccent, 0);
  lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
  AnimateEnter(card, delay, 12);
}

void IdentityRow(lv_obj_t *parent, int x, int y, int width,
                 const char *label, const std::string &value) {
  auto *caption = Kicker(parent, label, kMuted);
  lv_obj_set_pos(caption, x, y);
  auto *text = Label(parent, value.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(text, x, y + 42);
  lv_obj_set_width(text, width);
  lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
}

void BrandHero(lv_obj_t *parent, int x, int y, int width, int height,
               bool landscape) {
  auto *hero = lv_obj_create(parent);
  Panel(hero, 46, kMainSheet);
  lv_obj_set_pos(hero, x, y);
  lv_obj_set_size(hero, width, height);
  lv_obj_set_style_border_width(hero, 1, 0);
  lv_obj_set_style_border_color(hero, kMainLine, 0);
  lv_obj_set_style_border_opa(hero, LV_OPA_30, 0);
  lv_obj_set_style_clip_corner(hero, true, 0);
  lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_NONE, 0);

  AddMatrixEngraving(hero, width, height, kCyan);

  // Crop the approved boot composition just below the A. The white layer has
  // no orbit; from the cyan layer we reveal only the one colored A segment.
  // The boot spinner, wordmark and subtitle deliberately stay out of About.
  const int logo_scale = landscape ? 250 : 230;
  const int crop_width = assets::kAeraBrandWidth * logo_scale / LV_SCALE_NONE;
  const int crop_height = 500 * logo_scale / LV_SCALE_NONE;
  auto *logo_crop = lv_obj_create(hero);
  Clear(logo_crop);
  lv_obj_set_size(logo_crop, crop_width, crop_height);
  lv_obj_align(logo_crop, LV_ALIGN_CENTER, 0, landscape ? -104 : -92);
  lv_obj_set_style_clip_corner(logo_crop, true, 0);

  auto *white = BrandLayer(logo_crop, &assets::kAeraBrandWhite,
                           logo_scale, kText);
  lv_image_set_pivot(white, 0, 0);
  lv_obj_set_pos(white, 0, 0);

  constexpr int kSegmentX = 413;
  constexpr int kSegmentY = 230;
  constexpr int kSegmentWidth = 183;
  constexpr int kSegmentHeight = 121;
  auto *segment_crop = lv_obj_create(logo_crop);
  Clear(segment_crop);
  lv_obj_set_pos(segment_crop, kSegmentX * logo_scale / LV_SCALE_NONE,
                 kSegmentY * logo_scale / LV_SCALE_NONE);
  lv_obj_set_size(segment_crop,
                  kSegmentWidth * logo_scale / LV_SCALE_NONE,
                  kSegmentHeight * logo_scale / LV_SCALE_NONE);
  lv_obj_set_style_clip_corner(segment_crop, true, 0);
  auto *segment = BrandLayer(segment_crop, &assets::kAeraBrandAccent,
                             logo_scale, kCyan);
  lv_image_set_pivot(segment, 0, 0);
  lv_obj_set_pos(segment, -kSegmentX * logo_scale / LV_SCALE_NONE,
                 -kSegmentY * logo_scale / LV_SCALE_NONE);

  // These two labels are part of the logo composition rather than interface
  // copy. Keep their metrics fixed so larger Density modes cannot distort the
  // crop or overlap the mark.
  auto *word = FixedLabel(hero, "AERA", &lv_font_montserrat_48, kText);
  lv_obj_set_style_transform_scale(word, landscape ? 330 : 310, 0);
  lv_obj_set_style_text_letter_space(word, 8, 0);
  lv_obj_align(word, LV_ALIGN_BOTTOM_MID, 0, landscape ? -132 : -118);
  auto *project = FixedLabel(hero, "RECOVERY PROJECT",
                             &lv_font_montserrat_18, kCyan);
  lv_obj_set_style_text_letter_space(project, 3, 0);
  lv_obj_set_style_text_letter_space(project, 7, 0);
  lv_obj_align(project, LV_ALIGN_BOTTOM_MID, 0, landscape ? -66 : -54);
  AnimateEnter(hero, 20, 16);
}

}  // namespace

void BuildAboutScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  Header(screen, "About AERA", "Independent. Open. Built for recovery.",
         callback, context);
  const bool landscape = Landscape(screen);
  auto *content = Scroll(screen, landscape ? 330 : 446,
                         landscape ? 900 : 2440);
  lv_obj_set_style_pad_bottom(content, 40, 0);

  const std::string version = OrUnknown(RecoveryVersion());
  const std::string build_type = OrUnknown(RecoveryBuildType());
  const std::string build_status = OrUnknown(RecoveryBuildStatus());
  const std::string build_identity = build_type + " · " + build_status;
  const std::string device = OrUnknown(RecoveryDevice());
  const std::string slot = OrUnknown(RecoverySlot());
  const std::string maintainer = OrUnknown(RecoveryMaintainer());

  if (landscape) {
    BrandHero(content, 0, 0, 980, 842, true);

    auto *credits = Kicker(content, "THE PEOPLE BEHIND AERA", kAccent);
    lv_obj_set_pos(credits, 1040, 4);
    Contributor(content, 1032, 54, 2008, "J", "Jonas Salo · koaaN",
                "Founder, lead developer & interface direction", 45);
    Contributor(content, 1032, 306, 2008, "D", "Daniel Springer · Daniel210191",
                "Core developer & recovery engineering", 70);

    auto *identity = lv_obj_create(content);
    Panel(identity, 34, kMainPanel);
    lv_obj_set_pos(identity, 1032, 574);
    lv_obj_set_size(identity, 2008, 268);
    lv_obj_set_style_border_width(identity, 1, 0);
    lv_obj_set_style_border_color(identity, kMainLine, 0);
    lv_obj_set_style_border_opa(identity, LV_OPA_30, 0);
    IdentityRow(identity, 38, 38, 520, "MAINTAINER", maintainer);
    IdentityRow(identity, 650, 38, 240, "VERSION", version);
    IdentityRow(identity, 982, 38, 300, "BUILD", build_identity);
    IdentityRow(identity, 1374, 38, 360, "DEVICE", device);
    IdentityRow(identity, 1810, 38, 150, "SLOT", slot);
    auto *foundation = Label(identity,
        "Open-source foundations · Android Recovery · TWRP · OrangeFox · LVGL",
        &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(foundation, 38, 188);
    lv_obj_set_width(foundation, 1920);
    AnimateEnter(identity, 95, 10);
  } else {
    BrandHero(content, 0, 0, 1312, 716, false);

    auto *credits = Kicker(content, "THE PEOPLE BEHIND AERA", kAccent);
    lv_obj_set_pos(credits, 18, 782);
    Contributor(content, 0, 838, 1312, "J", "Jonas Salo · koaaN",
                "Founder, lead developer & interface direction", 48);
    Contributor(content, 0, 1090, 1312, "D", "Daniel Springer · Daniel210191",
                "Core developer & recovery engineering", 76);

    auto *identity = lv_obj_create(content);
    Panel(identity, 38, kMainPanel);
    lv_obj_set_pos(identity, 0, 1388);
    lv_obj_set_size(identity, 1312, 550);
    lv_obj_set_style_border_width(identity, 1, 0);
    lv_obj_set_style_border_color(identity, kMainLine, 0);
    lv_obj_set_style_border_opa(identity, LV_OPA_30, 0);
    auto *identity_title = Kicker(identity, "THIS BUILD", kAccent);
    lv_obj_set_pos(identity_title, 38, 30);
    IdentityRow(identity, 38, 98, 1236, "MAINTAINER", maintainer);
    IdentityRow(identity, 38, 224, 530, "VERSION", version);
    IdentityRow(identity, 696, 224, 520, "BUILD", build_identity);
    IdentityRow(identity, 38, 362, 530, "DEVICE", device);
    IdentityRow(identity, 696, 362, 520, "ACTIVE SLOT", slot);
    AnimateEnter(identity, 102, 10);

    auto *foundation = lv_obj_create(content);
    Panel(foundation, 34, kMainSheet);
    lv_obj_set_pos(foundation, 0, 1972);
    lv_obj_set_size(foundation, 1312, 318);
    // Match the identity and contributor cards above. The global Outline
    // material is intentionally stronger, but this final card belongs to the
    // same About-page group and should not look independently selected.
    lv_obj_set_style_border_width(foundation, 1, 0);
    lv_obj_set_style_border_color(foundation, kMainLine, 0);
    lv_obj_set_style_border_opa(foundation, LV_OPA_30, 0);
    auto *open = Kicker(foundation, "OPEN-SOURCE FOUNDATION", kAccent);
    lv_obj_set_pos(open, 38, 34);
    auto *copy = Label(foundation,
        "AERA stands on years of open recovery work from Android Recovery, "
        "TWRP, OrangeFox and LVGL contributors.",
        &lv_font_montserrat_28, kText);
    lv_obj_set_pos(copy, 38, 98);
    lv_obj_set_width(copy, 1236);
    lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);
    auto *thank_you = Label(foundation, "Made with care for the Android community.",
                            &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(thank_you, 38, 238);
    AnimateEnter(foundation, 128, 10);
  }

  Navigation(screen, Action::kSettings, callback, context);
}

}  // namespace aeraui
