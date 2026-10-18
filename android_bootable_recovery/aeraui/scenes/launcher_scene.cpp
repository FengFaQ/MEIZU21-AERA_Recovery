/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"
#include "home_plugin_grid.hpp"

#include <algorithm>
#include <string>

#include <lvgl.h>

#include "design.hpp"
#include "matrix_engraving.hpp"
#include "themed_app_art.hpp"
#include "plugins/plugin_manager.hpp"
#include "retroarch_icon.hpp"
#include "ui_components.hpp"
#include "webkit_icon.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

// Decorative objects created with lv_obj_create() are clickable by default.
// When an icon is placed inside a button that makes the artwork win hit
// testing, leaving a dead spot in the middle of the card.  Keep the complete
// artwork tree transparent to input so every point on a Home card reaches the
// card itself.
void MakeDecorationPassThrough(lv_obj_t *object) {
  if (object == nullptr) return;
  lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  const uint32_t child_count = lv_obj_get_child_count(object);
  for (uint32_t index = 0; index < child_count; ++index)
    MakeDecorationPassThrough(lv_obj_get_child(object, index));
}

lv_obj_t *MaterialTerminalIcon(lv_obj_t *parent, lv_color_t color, int size) {
  auto *art = lv_obj_create(parent);
  Clear(art);
  lv_obj_set_size(art, size, size);

  auto *frame = lv_obj_create(art);
  Clear(frame);
  lv_obj_set_size(frame, 104, 82);
  lv_obj_center(frame);
  lv_obj_set_style_radius(frame, 12, 0);
  lv_obj_set_style_border_width(frame, 8, 0);
  lv_obj_set_style_border_color(frame, color, 0);
  lv_obj_set_style_border_opa(frame, LV_OPA_COVER, 0);

  static constexpr lv_point_precise_t kChevron[] = {
      {44, 57}, {61, 72}, {44, 87},
  };
  auto *chevron = lv_line_create(art);
  lv_line_set_points(chevron, kChevron, 3);
  lv_obj_set_style_line_width(chevron, 8, 0);
  lv_obj_set_style_line_color(chevron, color, 0);
  lv_obj_set_style_line_rounded(chevron, true, 0);

  auto *cursor = lv_obj_create(art);
  Clear(cursor);
  lv_obj_set_pos(cursor, 74, 82);
  lv_obj_set_size(cursor, 34, 8);
  lv_obj_set_style_radius(cursor, 4, 0);
  lv_obj_set_style_bg_color(cursor, color, 0);
  lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
  return art;
}

lv_obj_t *AppIconPlate(lv_obj_t *parent, const char *symbol,
                       lv_color_t accent, int size,
                       bool retroarch_icon = false,
                       bool terminal_icon = false,
                       bool webkit_icon = false) {
  auto *plate = lv_obj_create(parent);
  Clear(plate);
  lv_obj_set_size(plate, size, size);
  if (terminal_icon) {
    auto *mark = MaterialTerminalIcon(plate, accent, size);
    lv_obj_center(mark);
  } else if (webkit_icon) {
    auto *mark = WebKitIconPlate(plate, accent, std::max(112, size * 112 / 144));
    lv_obj_center(mark);
  } else if (retroarch_icon) {
    auto *mark = RetroArchIconPlate(plate, accent, size - 14);
    lv_obj_center(mark);
  } else {
    auto *mark = Label(plate, symbol, &lv_font_montserrat_48, accent);
    lv_obj_set_style_transform_scale(mark, 352 * std::max(144, size) / 144, 0);
    lv_obj_center(mark);
  }
  return plate;
}

const char *GenericPluginIcon(const plugins::Plugin &plugin) {
  if (plugin.id == "mirror") return LV_SYMBOL_VIDEO;
  if (plugin.id.find("settings") != std::string::npos)
    return LV_SYMBOL_SAVE;
  return LV_SYMBOL_SETTINGS;
}

const char *PluginSummary(const plugins::Plugin &plugin) {
  if (plugin.entry == "browser") return "Browse the web with mobile WebKit.";
  if (plugin.entry == "retroarch") return "Play classic games with RetroArch.";
  if (plugin.entry == "doom") return "Play native Doom with touch controls.";
  if (plugin.entry == "telegram") return "Secure messaging and file sharing.";
  if (plugin.entry == "gallery") return "Browse, preview and zoom your photos.";
  if (plugin.entry == "media") return "Play music and video from storage.";
  if (plugin.entry == "streams") return "Stream videos and music without ads.";
  if (plugin.entry == "recorder") return "Record the recovery screen to video.";
  if (plugin.entry == "appvault") return "Back up and restore installed apps.";
  if (plugin.id == "mirror") return "View and control AERA from another screen.";
  return plugin.description.c_str();
}

void MarkUpdateAvailable(lv_obj_t *card, bool compact = false) {
  if (card == nullptr) return;
  lv_obj_set_style_border_width(card, 3, 0);
  lv_obj_set_style_border_color(card, kAccent, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
  auto *badge = Kicker(card, "UPDATE AVAILABLE", kAccent);
  if (compact)
    lv_obj_align(badge, LV_ALIGN_BOTTOM_MID, 0, -18);
  else
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -36, 28);
  MakeDecorationPassThrough(badge);

  // Draw attention once when Home opens without leaving a permanently
  // blinking control behind.
  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, card);
  lv_anim_set_values(&pulse, LV_OPA_60, LV_OPA_COVER);
  lv_anim_set_duration(&pulse, 520);
  lv_anim_set_delay(&pulse, 360);
  lv_anim_set_playback_duration(&pulse, 620);
  lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&pulse, [](void *target, int32_t opacity) {
    lv_obj_set_style_border_opa(static_cast<lv_obj_t *>(target),
                                static_cast<lv_opa_t>(opacity), 0);
  });
  lv_anim_start(&pulse);
}

lv_obj_t *AppCard(lv_obj_t *screen, int x, int y, int width,
                  const char *icon, const char *name, const char *description,
                  lv_color_t accent, Handler action,
                  bool retroarch_icon = false,
                  bool terminal_engraving = false,
                  bool webkit_icon = false,
                  themed_art::Kind artwork = themed_art::Kind::Tools,
                  int height = 360) {
  auto *card = lv_button_create(screen);
  Panel(card, 44, kMainPanel);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
  OnClick(card, std::move(action));
  if (terminal_engraving) {
    lv_obj_set_style_clip_corner(card, true, 0);
    AddMatrixEngraving(card, width, height, kAccent);
  }
  if (RecoveryThemedHomeIcons()) {
    if (!terminal_engraving) themed_art::Attach(card, artwork);
  } else {
    auto *plate = AppIconPlate(card, icon, accent, 144, retroarch_icon,
                               terminal_engraving, webkit_icon);
    lv_obj_set_pos(plate, 28, 22 + std::max(0, height - 360) / 3);
    MakeDecorationPassThrough(plate);
  }
  // 40 px is the Normal Home identity size. The shared scale maps it down for
  // Small and up to 48 px for Large, keeping all three modes visibly distinct.
  auto *title = Label(card, name, &lv_font_montserrat_40, kText);
  lv_obj_set_pos(title, 36, std::max(188, height - 172));
  FitLabelToLines(title, width - 72, 1,
                  {&lv_font_montserrat_40, &lv_font_montserrat_36,
                   &lv_font_montserrat_32, &lv_font_montserrat_28});
  auto *copy = Label(card, description, &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 36, std::max(250, height - 110));
  FitLabelToLines(copy, width - 110, 2,
                  {&lv_font_montserrat_24, &lv_font_montserrat_20,
                   &lv_font_montserrat_18, &lv_font_montserrat_16});
  auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kDim);
  lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, -38, 74);
  return card;
}

lv_obj_t *PluginTile(lv_obj_t *parent, int x, int y, int width, int height,
                     const char *icon, const plugins::Plugin &plugin,
                     lv_color_t accent, Handler action,
                     bool retroarch_icon = false) {
  if (width >= 600) {
    auto *card = AppCard(parent, x, y, width, icon, plugin.name.c_str(),
                         PluginSummary(plugin), accent, std::move(action),
                         retroarch_icon, false, plugin.entry == "browser",
                         themed_art::ForPlugin(plugin.id, plugin.entry, plugin.category), height);
    return card;
  }
  auto *card = lv_button_create(parent);
  Panel(card, 34, kMainSheet);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kMainLine, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
  const bool webkit_icon = plugin.entry == "browser";
  if (RecoveryThemedHomeIcons()) {
    themed_art::Attach(card, themed_art::ForPlugin(plugin.id, plugin.entry, plugin.category));
  } else {
    auto *plate = AppIconPlate(card, icon, accent, 154, retroarch_icon, false,
                             webkit_icon);
    lv_obj_align(plate, LV_ALIGN_TOP_MID, 0, 18);
    if (!retroarch_icon && !webkit_icon) {
      auto *mark = lv_obj_get_child(plate, 0);
      lv_obj_set_style_transform_scale(mark, 384, 0);
    }
    MakeDecorationPassThrough(plate);
  }
  auto *title = Label(card, plugin.name.c_str(), &lv_font_montserrat_32, kText);
  FitLabelToLines(title, width - 40, 1,
                  {&lv_font_montserrat_32, &lv_font_montserrat_28,
                   &lv_font_montserrat_24, &lv_font_montserrat_20,
                   &lv_font_montserrat_18, &lv_font_montserrat_16});
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 214);
  OnClick(card, std::move(action));
  return card;
}

lv_obj_t *PluginIconTile(lv_obj_t *parent, int x, int y, int width,
                         int height, const char *icon,
                         const plugins::Plugin &plugin, lv_color_t accent,
                         Handler action, bool dense,
                         bool retroarch_icon = false) {
  auto *card = lv_button_create(parent);
  Clear(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, 36, 0);
  lv_obj_set_style_bg_color(card, kMainSelected, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_PRESSED);
  lv_obj_set_style_transform_scale(card, 248, LV_STATE_PRESSED);
  OnClick(card, std::move(action));

  auto *plate = lv_obj_create(card);
  Panel(plate, 38, IconBackground());
  const int plate_size = std::min({width, height - (dense ? 42 : 70), 384});
  const int caption_gap = dense ? 28 : 20;
  const int plate_y = std::max(0, (height - plate_size - (dense ? 70 : 80)) / 2);
  lv_obj_set_size(plate, plate_size, plate_size);
  lv_obj_align(plate, LV_ALIGN_TOP_MID, 0, plate_y);
  lv_obj_set_style_border_width(plate, 1, 0);
  lv_obj_set_style_border_color(plate, kMainLine, 0);
  lv_obj_set_style_border_opa(plate, LV_OPA_40, 0);
  if (RecoveryThemedHomeIcons()) {
    themed_art::Attach(plate, themed_art::ForPlugin(plugin.id, plugin.entry, plugin.category));
  } else {
    auto *art = AppIconPlate(plate, icon, accent, 122 * plate_size / 170, retroarch_icon, false,
                           plugin.entry == "browser");
    lv_obj_center(art);
  }
  MakeDecorationPassThrough(plate);

  auto *title = Label(card, plugin.name.c_str(),
                      &lv_font_montserrat_24,
                      kText);
  FitLabelToLines(title, width, dense ? 1 : 2,
                  dense
                      ? std::initializer_list<const lv_font_t *>{
                            &lv_font_montserrat_24, &lv_font_montserrat_20,
                            &lv_font_montserrat_18, &lv_font_montserrat_16}
                      : std::initializer_list<const lv_font_t *>{
                            &lv_font_montserrat_24, &lv_font_montserrat_20,
                            &lv_font_montserrat_18, &lv_font_montserrat_16});
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, plate_y + plate_size + caption_gap);
  MakeDecorationPassThrough(title);
  return card;
}

void MarkIconUpdateAvailable(lv_obj_t *card) {
  if (card == nullptr) return;
  const int width = lv_obj_get_width(card);
  const int badge_size = width < 160 ? 24 : 30;
  auto *badge = lv_obj_create(card);
  Clear(badge);
  lv_obj_set_pos(badge, width - badge_size, 4);
  lv_obj_set_size(badge, badge_size, badge_size);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(badge, kAccent, 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(badge, 4, 0);
  lv_obj_set_style_border_color(badge, kMainCanvas, 0);
  lv_obj_set_style_border_opa(badge, LV_OPA_COVER, 0);
  MakeDecorationPassThrough(badge);
}

Action PluginAction(const plugins::Plugin &plugin) {
  if (plugin.entry == "browser") return Action::kWeb;
  if (plugin.entry == "retroarch") return Action::kRetroArch;
  if (plugin.entry == "doom") return Action::kDoom;
  if (plugin.entry == "telegram") return Action::kTelegram;
  if (plugin.entry == "gallery") return Action::kGallery;
  if (plugin.entry == "media") return Action::kMedia;
  if (plugin.entry == "streams") return Action::kStreams;
  if (plugin.entry == "recorder") return Action::kRecorder;
  if (plugin.entry == "appvault") return Action::kAppVault;
  return Action::kPluginApp;
}

const char *PluginIcon(const plugins::Plugin &plugin) {
  if (plugin.entry == "browser" || plugin.entry == "telegram") return LV_SYMBOL_GPS;
  if (plugin.entry == "gallery") return LV_SYMBOL_IMAGE;
  if (plugin.entry == "recorder" || plugin.entry == "streams") return LV_SYMBOL_VIDEO;
  if (plugin.entry == "appvault") return LV_SYMBOL_SAVE;
  if (plugin.entry == "retroarch" || plugin.entry == "doom" || plugin.entry == "media")
    return LV_SYMBOL_PLAY;
  return GenericPluginIcon(plugin);
}

}  // namespace

void BuildHomeScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  Header(screen, "Home", "Your recovery apps, files and extensions.", callback, context);
  const auto installed = plugins::Installed();
  const auto updates = plugins::AvailableUpdates(installed);
  const bool landscape = lv_obj_get_width(screen) > lv_obj_get_height(screen);
  const int grid_columns = RecoveryHomeGridColumns();
  auto factory = [updates, grid_columns](lv_obj_t *parent, const plugins::Plugin &plugin,
      int x, int y, int width, int height, bool icons, Handler handler) {
    const char *icon = PluginIcon(plugin);
    auto *tile = icons
        ? PluginIconTile(parent, x, y, width, height, icon, plugin, kAccent,
                         std::move(handler), grid_columns == 5, plugin.entry == "retroarch")
        : PluginTile(parent, x, y, width, height, icon, plugin, kAccent,
                     std::move(handler), plugin.entry == "retroarch");
    if (std::any_of(updates.begin(), updates.end(), [&](const auto &update) {
          return update.installed.id == plugin.id;
        })) {
      if (icons) MarkIconUpdateAvailable(tile);
      else MarkUpdateAvailable(tile, width < 600);
    }
    return tile;
  };
  auto launch = [callback, context](const plugins::Plugin &plugin) {
    SetSelectedPluginId(plugin.id);
    callback(PluginAction(plugin), context);
  };
  if (landscape) {
    auto *apps = Label(screen, "SYSTEM APPS", &lv_font_montserrat_18, kMuted);
    lv_obj_set_pos(apps, 80, 306);
    AppCard(screen, 64, 356, 976, LV_SYMBOL_DIRECTORY, "Files",
            "Browse storage, preview images and install ZIPs.", kAccent,
            [=] { callback(Action::kFiles, context); }, false, false, false, themed_art::Kind::Files);
    auto *store = AppCard(screen, 1072, 356, 976, LV_SYMBOL_DOWNLOAD, "Plugin Manager",
            "Discover and install signed AERA extensions.", kAccent,
            [=] { callback(Action::kPlugins, context); }, false, false, false, themed_art::Kind::Packages);
    if (!updates.empty()) MarkUpdateAvailable(store);
    AppCard(screen, 2080, 356, 976, LV_SYMBOL_EDIT, "Terminal",
            "A real recovery shell built into AERA.", kAccent,
            [=] { callback(Action::kTerminal, context); }, false, true);
    auto *extensions = Label(screen, "INSTALLED PLUGINS", &lv_font_montserrat_18, kMuted);
    lv_obj_set_pos(extensions, 80, 760);
    const bool icons = grid_columns >= 4;
    BuildHomePluginGrid(screen, installed,
                        {64, 810, 2992, 360, icons ? grid_columns : 4, 1, icons ? 250 : 360, icons},
                        factory, launch, callback, context);
    Navigation(screen, Action::kBackHome, callback, context);
    return;
  }
  auto *apps = Label(screen, "SYSTEM APPS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(apps, 3, 0);
  lv_obj_set_pos(apps, 80, 426);
  auto *files = AppCard(screen, 64, 484, 636, LV_SYMBOL_DIRECTORY, "Files",
                        "Browse storage, preview images and install ZIPs.",
                        kAccent, [=] { callback(Action::kFiles, context); },
                        false, false, false, themed_art::Kind::Files);
  const std::string store_summary = updates.empty()
      ? "Discover signed apps and install them to storage or RAM."
      : i18n::Format("%zu updates available", updates.size());
  auto *store = AppCard(screen, 740, 484, 636, LV_SYMBOL_DOWNLOAD,
                        "Plugin Manager", store_summary.c_str(),
                        kAccent, [=] { callback(Action::kPlugins, context); },
                        false, false, false, themed_art::Kind::Packages);
  auto *terminal = AppCard(screen, 64, 884, 1312, LV_SYMBOL_EDIT, "Terminal",
                           "Run recovery commands in the built-in AERA shell.",
                           kAccent, [=] { callback(Action::kTerminal, context); },
                           false, true);
  AnimateEnter(files, 20, 14);
  AnimateEnter(store, 55, 14);
  AnimateEnter(terminal, 80, 14);
  if (!updates.empty()) MarkUpdateAvailable(store);

  auto *extensions = Label(screen, "INSTALLED PLUGINS", &lv_font_montserrat_18, kMuted);
  lv_obj_set_style_text_letter_space(extensions, 3, 0);
  lv_obj_set_pos(extensions, 80, 1320);
  const bool icon_grid = grid_columns >= 4;
  const int grid_height = grid_columns == 5 ? 1320 : 1050;
  const int tile_height = grid_columns == 4 ? 250 : grid_columns == 5 ? 240 : 318;
  if (!installed.empty()) {
    BuildHomePluginGrid(screen, installed,
        {64, 1378, 1312, grid_height, grid_columns, icon_grid ? grid_columns : 3,
         tile_height, icon_grid}, factory, launch, callback, context);
  }
  if (installed.empty()) {
    auto *empty = lv_obj_create(screen);
    Panel(empty, 40, kMainSheet);
    lv_obj_set_pos(empty, 64, 1378);
    lv_obj_set_size(empty, 1312, 310);
    auto *title = Label(empty, "Build AERA your way", &lv_font_montserrat_48, kText);
    lv_obj_set_pos(title, 42, 48);
    auto *copy = Label(empty,
        "Install the browser now. Audio and GPU extensions can follow without growing recovery.img.",
        &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(copy, 42, 120);
    lv_obj_set_width(copy, 1180);
    auto *open = Button(empty, "Open Plugin Manager", [=] {
      callback(Action::kPlugins, context);
    }, true);
    lv_obj_set_pos(open, 42, 196);
    lv_obj_set_size(open, 540, 92);
    lv_obj_set_style_radius(open, 28, 0);
    AnimateEnter(empty, 90, 14);
  }
  Navigation(screen, Action::kBackHome, callback, context);
}

}  // namespace aeraui
