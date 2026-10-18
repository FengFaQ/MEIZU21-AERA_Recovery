/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <lvgl.h>
#include "design.hpp"
#include "phone_keyboard.hpp"
#include "picture_viewer.hpp"
#include "plugins/plugin_manager.hpp"
#include "plugins/store_metadata.hpp"
#include "retroarch_icon.hpp"
#include "ui_components.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

enum class View { kStore, kInstalled, kUpdates };
const char *const kCategories[] = {"", "tools", "backup", "multimedia", "network", "games", "themes"};

struct State {
  lv_obj_t *screen = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  PluginScene scene{};
  lv_obj_t *tabs[3]{};
  lv_obj_t *filters = nullptr;
  lv_obj_t *search = nullptr;
  lv_obj_t *clear_search = nullptr;
  lv_obj_t *category = nullptr;
  lv_obj_t *automatic = nullptr;
  lv_obj_t *toggle = nullptr;
  lv_obj_t *update_all = nullptr;
  lv_obj_t *back = nullptr;
  View view = View::kStore;
  unsigned category_index = 0;
  std::string query;
  std::string detail_id;
  int scroll_position = 0;
  plugins::Job active_job = plugins::Job::kRefresh;
  bool busy = false;
};

std::mutex gRequestMutex;
plugins::Request gRequest;
void Render(State *state);

lv_obj_t *StoreButton(lv_obj_t *parent, const char *text, Handler action,
                      bool primary = false) {
  auto *button = Button(parent, text, std::move(action), primary);
  lv_obj_set_style_text_align(lv_obj_get_child(button, 0), LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(button, kMainPanel, LV_STATE_DISABLED);
  lv_obj_set_style_text_color(lv_obj_get_child(button, 0), kMuted, LV_STATE_DISABLED);
  return button;
}

void Select(const plugins::Request &request) {
  std::lock_guard<std::mutex> lock(gRequestMutex);
  gRequest = request;
}

const plugins::Plugin *Find(const std::vector<plugins::Plugin> &items, const std::string &id) {
  const auto found = std::find_if(items.begin(), items.end(),
      [&](const auto &plugin) { return plugin.id == id; });
  return found == items.end() ? nullptr : &*found;
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

Action LaunchAction(const plugins::Plugin &plugin) {
  if (plugins::IsThemeExtension(plugin)) return Action::kTheme;
  if (plugins::IsGeneric(plugin)) return Action::kPluginApp;
  if (plugin.entry == "browser") return Action::kWeb;
  if (plugin.entry == "retroarch") return Action::kRetroArch;
  if (plugin.entry == "doom") return Action::kDoom;
  if (plugin.entry == "telegram") return Action::kTelegram;
  if (plugin.entry == "gallery") return Action::kGallery;
  if (plugin.entry == "media") return Action::kMedia;
  if (plugin.entry == "streams") return Action::kStreams;
  if (plugin.entry == "recorder") return Action::kRecorder;
  if (plugin.entry == "appvault") return Action::kAppVault;
  return Action::kNone;
}

const char *Icon(const plugins::Plugin &plugin) {
  if (plugin.category == "themes" || plugins::IsThemeExtension(plugin)) return LV_SYMBOL_EDIT;
  if (plugin.category == "backup") return LV_SYMBOL_SAVE;
  if (plugin.id == "browser") return LV_SYMBOL_GPS;
  if (plugin.id == "gallery") return LV_SYMBOL_IMAGE;
  if (plugin.category == "multimedia" || plugin.category == "games") return LV_SYMBOL_PLAY;
  if (plugin.category == "network") return LV_SYMBOL_WIFI;
  return LV_SYMBOL_SETTINGS;
}

void Request(State *state, plugins::Job job, const std::string &id = "", size_t screenshot = 0) {
  if (!state || state->busy) return;
  const bool online_job = job != plugins::Job::kRemove &&
      job != plugins::Job::kInstallLocalMemory && job != plugins::Job::kInstallLocalStorage;
  if (online_job && !RecoveryWifiConnection().connected && job != plugins::Job::kScreenshot) {
    i18n::BindLabel(state->scene.status, "Offline");
    Sheet(state->screen, "Plugin Store is offline",
          "Connect AERA to Wi-Fi, then try Refresh store again.");
    return;
  }
  plugins::Request request;
  request.job = job;
  request.id = id;
  request.screenshot_index = screenshot;
  Select(request);
  state->callback(Action::kRunPluginOperation, state->context);
}

void ResizeList(State *state, bool detail) {
  const int toolbar = Landscape(state->screen) ? 310 : 430;
  const int top = toolbar + (detail ? 248 : 390);
  lv_obj_set_pos(state->scene.list, 64, top);
  lv_obj_set_size(state->scene.list, lv_obj_get_width(state->screen) - 128,
      std::max(180, static_cast<int>(lv_obj_get_height(state->screen)) - top -
          NavigationHeight(state->screen) - 48));
  lv_obj_update_layout(state->scene.list);
}

void ShowDetail(State *state, const std::string &id) {
  if (state->busy) return;
  state->scroll_position = lv_obj_get_scroll_y(state->scene.list);
  state->detail_id = id;
  Render(state);
}

lv_obj_t *BodyLabel(lv_obj_t *parent, const std::string &text,
                    const lv_font_t *font, lv_color_t color, int width) {
  auto *label = Label(parent, text.c_str(), font, color);
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(label, width);
  return label;
}

int PlaceText(lv_obj_t *parent, int y, const std::string &text,
              const lv_font_t *font, lv_color_t color, int width) {
  auto *label = BodyLabel(parent, text, font, color, width);
  lv_obj_set_pos(label, 28, y);
  lv_obj_update_layout(label);
  return y + lv_obj_get_height(label) + 24;
}

void Detail(State *state, const std::vector<plugins::Plugin> &catalog,
            const std::vector<plugins::Plugin> &installed) {
  const auto *local = Find(installed, state->detail_id);
  const auto *release = Find(catalog, state->detail_id);
  if (local && local->trust == plugins::Trust::kUnofficial && state->view != View::kStore)
    release = nullptr;
  const auto *plugin = release ? release : local;
  if (!plugin) { state->detail_id.clear(); Render(state); return; }
  const int width = lv_obj_get_width(state->scene.list) - 56;
  int y = 28;
  auto *plate = plugin->id == "retroarch"
      ? RetroArchIconPlate(state->scene.list, kText, 112)
      : IconPlate(state->scene.list, Icon(*plugin), kAccent, IconBackground(), 112);
  lv_obj_set_pos(plate, 28, y);
  auto *title = BodyLabel(state->scene.list, plugin->name, &lv_font_montserrat_48,
                          kText, width - 156);
  lv_obj_set_pos(title, 184, y + 8);
  lv_obj_update_layout(title);
  y += std::max(112, static_cast<int>(lv_obj_get_height(title)) + 16) + 36;
  y = PlaceText(state->scene.list, y, i18n::Translate(plugins::CategoryLabel(plugin->category)),
                &lv_font_montserrat_24, kAccent, width);
  if (!plugin->author.empty())
    y = PlaceText(state->scene.list, y, plugin->author, &lv_font_montserrat_24, kMuted, width);
  const std::string details = plugin->details.empty() ? plugin->description : plugin->details;
  y = PlaceText(state->scene.list, y + 8, details, &lv_font_montserrat_32, kText, width);
  y += 20;
  if (release) {
    y = PlaceText(state->scene.list, y,
        i18n::Format("Available version: %s", release->version.c_str()),
        &lv_font_montserrat_24, kMuted, width);
    y = PlaceText(state->scene.list, y,
        i18n::Format("Download size: %s", Size(release->package_size).c_str()),
        &lv_font_montserrat_24, kMuted, width);
  }
  if (local) {
    y = PlaceText(state->scene.list, y,
        i18n::Format("Installed version: %s", local->version.c_str()),
        &lv_font_montserrat_24, kMuted, width);
    y = PlaceText(state->scene.list, y,
        i18n::Format("Installed in: %s", i18n::Translate(plugins::LocationLabel(local->location))),
        &lv_font_montserrat_24, kMuted, width);
    if (local->trust == plugins::Trust::kUnofficial)
      y = PlaceText(state->scene.list, y, i18n::Translate("Installed from a local package"),
                    &lv_font_montserrat_24, kAmber, width);
  }
  y += 20;
  auto *actions = lv_obj_create(state->scene.list);
  Clear(actions);
  lv_obj_set_pos(actions, 28, y);
  lv_obj_set_size(actions, width, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(actions, 24, 0);
  lv_obj_set_style_pad_row(actions, 24, 0);
  const std::string id = plugin->id;
  auto add = [&](const char *text, Handler handler, bool primary = false) {
    auto *button = StoreButton(actions, text, std::move(handler), primary);
    lv_obj_set_size(button, (width - 24) / 2, kInterfaceSize >= 2 ? 140 : 120);
    lv_obj_set_style_radius(button, 28, 0);
    if (state->busy) lv_obj_add_state(button, LV_STATE_DISABLED);
  };
  if (local && (local->trust == plugins::Trust::kOfficial || !release) &&
      LaunchAction(*local) != Action::kNone) {
    const Action action = LaunchAction(*local);
    add(action == Action::kTheme ? "Theme Engine" : "Open", [state, id, action] {
      if (action == Action::kPluginApp) SetSelectedPluginId(id);
      state->callback(action, state->context);
    }, true);
  }
  if (release) {
    const bool update = local && plugins::IsUpdateAvailable(*local, *release);
    add(!local ? "Install on storage" : update ? "Update on storage" : "Reinstall",
        [state, id] { Request(state, plugins::Job::kInstallStorage, id); }, !local);
    add(local && local->location == plugins::Location::kMemory && update
            ? "Update in RAM" : "Load into RAM",
        [state, id] { Request(state, plugins::Job::kInstallMemory, id); });
  }
  if (local) add("Remove", [state, id] {
    Sheet(state->screen, "Remove plugin?", "Remove this plugin from AERA?",
          [state, id] { Request(state, plugins::Job::kRemove, id); });
  });
  lv_obj_update_layout(actions);
  y += lv_obj_get_height(actions) + 40;
  if (!plugin->screenshots.empty()) {
    y = PlaceText(state->scene.list, y, i18n::Translate("Screenshots"),
                  &lv_font_montserrat_32, kText, width);
    for (size_t i = 0; i < plugin->screenshots.size(); ++i) {
      auto *button = StoreButton(state->scene.list,
          i18n::Format("Screenshot %u", static_cast<unsigned>(i + 1)).c_str(),
          [state, id, i] { Request(state, plugins::Job::kScreenshot, id, i); });
      lv_obj_set_pos(button, 28, y);
      lv_obj_set_size(button, width, 116);
      y += 140;
    }
  }
}

void OpenSearch(State *state) {
  if (state->busy) return;
  auto *overlay = lv_obj_create(state->screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
  const bool landscape = Landscape(state->screen);
  const int width = std::min(landscape ? 1900 : 1312,
      static_cast<int>(lv_obj_get_width(state->screen)) - 80);
  const int margin = 40;
  const int input_y = 40 + lv_font_get_line_height(UiFont(&lv_font_montserrat_48)) + 28;
  const int input_height = std::max(160,
      static_cast<int>(lv_font_get_line_height(UiFont(&lv_font_montserrat_40))) + 64);
  const int button_y = input_y + input_height + 24;
  const int button_height = 128;
  const int keyboard_y = button_y + button_height + 32;
  const int keyboard_height = std::min(landscape ? 680 : 900,
      static_cast<int>(lv_obj_get_height(state->screen)) - 120 - keyboard_y);
  const int height = keyboard_y + keyboard_height + margin;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 44, kMainSheet);
  lv_obj_set_style_pad_all(sheet, 0, 0);
  lv_obj_set_size(sheet, width, height);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -40);
  auto *title = Label(sheet, "Search plugins", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, margin, 40);
  SingleLineLabel(title, width - 2 * margin, &lv_font_montserrat_48);
  auto *input = TextArea(sheet);
  lv_obj_set_pos(input, margin, input_y);
  lv_obj_set_size(input, width - 2 * margin, input_height);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_max_length(input, 100);
  lv_textarea_set_text(input, state->query.c_str());
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_40), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_bg_color(input, kMainPanel, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(input, 24, 0);
  lv_obj_set_style_pad_all(input, 28, 0);
  lv_obj_set_style_min_height(input, input_height, 0);
  const int vertical_padding =
      (input_height - lv_font_get_line_height(UiFont(&lv_font_montserrat_40))) / 2;
  lv_obj_set_style_pad_top(input, vertical_padding, 0);
  lv_obj_set_style_pad_bottom(input, vertical_padding, 0);
  lv_obj_set_style_border_width(input, 2, 0);
  lv_obj_set_style_border_color(input, kAccent, 0);
  auto *keyboard = lv_keyboard_create(sheet);
  phone_keyboard::Apply(keyboard);
  lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, margin, keyboard_y);
  lv_obj_set_size(keyboard, width - 2 * margin, keyboard_height);
  lv_keyboard_set_textarea(keyboard, input);
  auto save = [state, input, overlay] {
    state->query = lv_textarea_get_text(input);
    lv_obj_delete_async(overlay);
    Render(state);
  };
  const int button_width = (width - 2 * margin - 24) / 2;
  auto *cancel = StoreButton(sheet, "Cancel", [overlay] { lv_obj_delete_async(overlay); });
  auto *accept = StoreButton(sheet, "Search", save, true);
  lv_obj_set_pos(cancel, margin, button_y);
  lv_obj_set_pos(accept, margin + button_width + 24, button_y);
  lv_obj_set_size(cancel, button_width, button_height);
  lv_obj_set_size(accept, button_width, button_height);
  lv_obj_add_event_cb(keyboard, [](lv_event_t *event) {
    lv_obj_send_event(static_cast<lv_obj_t *>(lv_event_get_user_data(event)), LV_EVENT_CLICKED, nullptr);
  }, LV_EVENT_READY, accept);
  auto *handler = new Handler(save);
  lv_obj_add_event_cb(input, [](lv_event_t *event) {
    auto *handler = static_cast<Handler *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) delete handler;
    else if (lv_event_get_code(event) == LV_EVENT_READY) (*handler)();
  }, LV_EVENT_ALL, handler);
  lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
}

void OpenCategories(State *state) {
  if (state->busy) return;
  const int row_height = std::max(136,
      static_cast<int>(lv_font_get_line_height(UiFont(&lv_font_montserrat_36))) + 72);
  Sheet(state->screen, state->view == View::kStore ? "Store" : "Installed", "", {},
        390 + 7 * (row_height + 16), true, SheetPresentation::kStandard,
        "Swipe to confirm", [state, row_height](lv_obj_t *area) {
    lv_obj_update_layout(area);
    const int width = lv_obj_get_width(area) - 16;
    auto *overlay = lv_obj_get_parent(lv_obj_get_parent(area));
    for (unsigned i = 0; i < 7; ++i) {
      const char *text = i ? plugins::CategoryLabel(kCategories[i]) : "All";
      auto *choice = StoreButton(area, "", [state, overlay, i] {
        state->category_index = i;
        lv_obj_delete_async(overlay);
        Render(state);
      });
      lv_obj_set_pos(choice, 0, i * (row_height + 16));
      lv_obj_set_size(choice, width, row_height);
      lv_obj_set_style_radius(choice, 26, 0);
      const bool selected = i == state->category_index;
      if (selected) {
        lv_obj_set_style_border_width(choice, 2, 0);
        lv_obj_set_style_border_color(choice, kAccent, 0);
      }
      auto *label = Label(choice, text, &lv_font_montserrat_36,
                          selected ? kAccent : kText);
      FitLabelToLines(label, width - 140, 1,
                      {&lv_font_montserrat_36, &lv_font_montserrat_32});
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 32, 0);
      if (selected) {
        auto *check = Label(choice, LV_SYMBOL_OK, &lv_font_montserrat_32, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -32, 0);
      }
    }
  });
}

void Render(State *state) {
  const auto catalog = plugins::Catalog();
  const auto installed = plugins::Installed();
  const bool detail = !state->detail_id.empty();
  const bool updates_view = !detail && state->view == View::kUpdates;
  const auto hidden = [](lv_obj_t *object, bool value) {
    if (value) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
  };
  for (int i = 0; i < 3; ++i) {
    hidden(state->tabs[i], detail);
    lv_obj_set_style_bg_color(state->tabs[i], kMainPanel, 0);
    lv_obj_set_style_bg_color(state->tabs[i], kMainPanel, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(state->tabs[i], static_cast<int>(state->view) == i ? 2 : 0, 0);
    lv_obj_set_style_border_color(state->tabs[i], kAccent, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(state->tabs[i], 0),
        static_cast<int>(state->view) == i ? kAccent : kMuted, 0);
  }
  hidden(state->filters, detail || updates_view);
  hidden(state->automatic, !updates_view);
  hidden(state->update_all, !updates_view);
  hidden(state->back, !detail);
  hidden(state->scene.refresh, detail);
  hidden(state->clear_search, state->query.empty());
  hidden(state->search, detail || updates_view);
  lv_obj_set_style_border_width(state->search, state->query.empty() ? 0 : 2, 0);
  lv_obj_set_style_border_color(state->search, kAccent, 0);
  const int toolbar = Landscape(state->screen) ? 310 : 430;
  lv_obj_set_y(state->scene.status, toolbar + (detail ? 142 : 282));
  lv_obj_set_y(state->scene.progress, toolbar + (detail ? 210 : 350));
  ResizeList(state, detail);
  lv_obj_clean(state->scene.list);
  lv_obj_scroll_to_y(state->scene.list, 0, LV_ANIM_OFF);
  if (detail) { Detail(state, catalog, installed); return; }
  const int half_width = (lv_obj_get_width(state->filters) - 24) / 2;
  const int category_width = state->query.empty() ? lv_obj_get_width(state->filters) : half_width;
  lv_obj_set_width(state->category, category_width);
  lv_obj_set_width(lv_obj_get_child(state->category, 0), category_width - 128);
  const std::string clear_query = state->query + "  " LV_SYMBOL_CLOSE;
  i18n::BindLabel(lv_obj_get_child(state->clear_search, 0), clear_query.c_str());
  FitButtonLabel(state->clear_search);
  i18n::BindLabel(lv_obj_get_child(state->category, 0),
      state->category_index ? plugins::CategoryLabel(kCategories[state->category_index]) : "All");
  std::vector<plugins::Plugin> items;
  unsigned update_count = 0;
  for (const auto &release : catalog) {
    const auto *local = Find(installed, release.id);
    if (local && plugins::IsUpdateAvailable(*local, release)) ++update_count;
  }
  if (state->busy || !update_count || !RecoveryWifiConnection().connected)
    lv_obj_add_state(state->update_all, LV_STATE_DISABLED);
  else lv_obj_remove_state(state->update_all, LV_STATE_DISABLED);
  if (state->view == View::kStore) items = catalog;
  else for (const auto &local : installed) {
    const auto *release = local.trust == plugins::Trust::kOfficial ? Find(catalog, local.id) : nullptr;
    if (state->view == View::kUpdates && (!release || !plugins::IsUpdateAvailable(local, *release))) continue;
    items.push_back(release ? *release : local);
  }
  const auto query = Lower(state->query);
  items.erase(std::remove_if(items.begin(), items.end(), [&](const auto &plugin) {
    return state->view != View::kUpdates &&
        ((state->category_index && plugin.category != kCategories[state->category_index]) ||
         (!query.empty() && Lower(plugin.name + " " + plugin.id + " " + plugin.description).find(query) == std::string::npos));
  }), items.end());
  const auto category_rank = [](const auto &plugin) {
    for (unsigned i = 1; i < 7; ++i) if (plugin.category == kCategories[i]) return i;
    return 1U; // Unspecified categories belong to Tools.
  };
  std::sort(items.begin(), items.end(), [&](const auto &a, const auto &b) {
    const auto first = category_rank(a), second = category_rank(b);
    return first != second ? first < second : Lower(a.name) < Lower(b.name);
  });
  const int width = lv_obj_get_width(state->scene.list);
  const int name_height = lv_font_get_line_height(UiFont(&lv_font_montserrat_36));
  const int description_height = 2 * lv_font_get_line_height(UiFont(&lv_font_montserrat_24));
  const int meta_height = lv_font_get_line_height(UiFont(&lv_font_montserrat_20));
  const int row_height = 64 + name_height + description_height + meta_height + 24;
  int y = 0;
  unsigned last_category = 0, category_row = 0;
  for (const auto &plugin : items) {
    const auto category = category_rank(plugin);
    if (category != last_category) {
      if (last_category) y += 36;
      y = PlaceText(state->scene.list, y,
          i18n::Translate(plugins::CategoryLabel(kCategories[category])),
          &lv_font_montserrat_24, kMuted, width - 56);
      last_category = category;
      category_row = 0;
    }
    const auto *local = Find(installed, plugin.id);
    const bool update = local && plugins::IsUpdateAvailable(*local, plugin);
    auto *row = StoreButton(state->scene.list, "", [state, id = plugin.id] { ShowDetail(state, id); });
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, width, row_height);
    lv_obj_set_style_radius(row, 24, 0);
    lv_obj_set_style_bg_opa(row, category_row++ % 2 == 0 ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);
    const bool unofficial = local && local->trust == plugins::Trust::kUnofficial;
    auto *plate = plugin.id == "retroarch" ? RetroArchIconPlate(row, kText, 84)
        : IconPlate(row, Icon(plugin), update || unofficial ? kAmber : kAccent, IconBackground(), 84);
    lv_obj_set_pos(plate, 28, 32);
    lv_obj_remove_flag(plate, LV_OBJ_FLAG_CLICKABLE);
    auto *name = Label(row, plugin.name.c_str(), &lv_font_montserrat_36, kText);
    lv_obj_set_pos(name, 144, 28);
    SingleLineLabel(name, width - 224, &lv_font_montserrat_36);
    auto *description = Label(row, plugin.description.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(description, 144, 36 + name_height);
    lv_label_set_long_mode(description, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(description, width - 224, description_height);
    const std::string version = local ? local->version : plugin.version;
    const char *status = update ? "Update available" : local ? "Installed" : "Available";
    const std::string metadata = version + "  /  " +
        (local && local->trust == plugins::Trust::kUnofficial
            ? i18n::Translate("Unofficial") : i18n::Translate(status));
    auto *meta = Label(row, metadata.c_str(), &lv_font_montserrat_20,
        update || unofficial ? kAmber : local ? kGreen : kDim);
    lv_obj_set_pos(meta, 144, 48 + name_height + description_height);
    SingleLineLabel(meta, width - 224, &lv_font_montserrat_20);
    auto *arrow = Label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_24, kMuted);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -28, 0);
    if (state->busy) lv_obj_add_state(row, LV_STATE_DISABLED);
    y += row_height + 12;
  }
  if (items.empty()) {
    const char *text = !query.empty() || state->category_index ? "No matching plugins." :
        state->view == View::kUpdates ? "All plugins are up to date." :
        state->view == View::kInstalled ? "No plugins installed." :
        "Connect to Wi-Fi and refresh the store.";
    PlaceText(state->scene.list, 64, i18n::Translate(text), &lv_font_montserrat_32, kMuted, width - 56);
  }
}
}  // namespace

plugins::Request GetPluginRequest() { std::lock_guard<std::mutex> lock(gRequestMutex); return gRequest; }
void SetPluginRequest(const plugins::Request &request) { Select(request); }

PluginScene BuildPluginScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *state = new State;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  state->scene.state = state;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) { delete static_cast<State *>(lv_event_get_user_data(event)); }, LV_EVENT_DELETE, state);
  Header(screen, "Plugin Manager", "Signed apps and extensions for AERA.", callback, context);
  lv_obj_update_layout(screen);
  const int width = lv_obj_get_width(screen) - 160;
  const int top = Landscape(screen) ? 310 : 430;
  const int tab_width = (width - 144) / 3;
  for (int i = 0; i < 3; ++i) {
    state->tabs[i] = StoreButton(screen, i == 0 ? "Store" : i == 1 ? "Installed" : "Updates", [state, i] {
      if (state->busy) return;
      state->view = static_cast<View>(i);
      state->category_index = 0;
      state->query.clear();
      Render(state);
    });
    lv_obj_set_pos(state->tabs[i], 80 + i * (tab_width + 16), top);
    lv_obj_set_size(state->tabs[i], tab_width, 110);
    lv_obj_set_style_radius(state->tabs[i], 28, 0);
  }
  state->scene.refresh = StoreButton(screen, LV_SYMBOL_REFRESH, [state] { Request(state, plugins::Job::kRefresh); });
  lv_obj_set_pos(state->scene.refresh, 80 + width - 96, top);
  lv_obj_set_size(state->scene.refresh, 96, 110);
  state->back = StoreButton(screen, LV_SYMBOL_LEFT, [state] { NavigatePluginBack(state->scene); });
  lv_obj_set_pos(state->back, 80, top);
  lv_obj_set_size(state->back, 80, 110);
  state->filters = lv_obj_create(screen);
  Clear(state->filters);
  lv_obj_set_pos(state->filters, 80, top + 138);
  lv_obj_set_size(state->filters, width, 116);
  const int half_width = (width - 24) / 2;
  state->search = StoreButton(screen, "Search", [state] { OpenSearch(state); });
  lv_obj_set_size(state->search, 96, 96);
  lv_obj_set_style_radius(state->search, 28, 0);
  lv_obj_align(state->search, LV_ALIGN_TOP_RIGHT, -80, Landscape(screen) ? 184 : 232);
  lv_obj_add_flag(lv_obj_get_child(state->search, 0), LV_OBJ_FLAG_HIDDEN);
  // Draw the magnifier independently of the selected font's glyph coverage.
  auto *search_icon = lv_obj_create(state->search);
  Clear(search_icon);
  lv_obj_set_size(search_icon, 44, 44);
  lv_obj_center(search_icon);
  auto *lens = lv_obj_create(search_icon);
  Clear(lens);
  lv_obj_set_size(lens, 30, 30);
  lv_obj_set_style_radius(lens, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(lens, 4, 0);
  lv_obj_set_style_border_color(lens, kMuted, 0);
  auto *handle = lv_line_create(search_icon);
  static const lv_point_precise_t handle_points[] = {{26, 26}, {41, 41}};
  lv_line_set_points(handle, handle_points, 2);
  lv_obj_set_style_line_width(handle, 4, 0);
  lv_obj_set_style_line_color(handle, kMuted, 0);
  lv_obj_set_style_line_rounded(handle, true, 0);
  for (auto *object : {search_icon, lens, handle}) lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  state->clear_search = StoreButton(state->filters, LV_SYMBOL_CLOSE, [state] {
    if (!state->busy) { state->query.clear(); Render(state); }
  });
  lv_obj_set_pos(state->clear_search, half_width + 24, 0);
  lv_obj_set_size(state->clear_search, half_width, 116);
  state->category = StoreButton(state->filters, "All", [state] { OpenCategories(state); });
  lv_obj_set_pos(state->category, 0, 0);
  lv_obj_set_size(state->category, half_width, 116);
  lv_obj_set_style_radius(state->category, 28, 0);
  auto *category_label = lv_obj_get_child(state->category, 0);
  lv_obj_set_width(category_label, half_width - 128);
  lv_obj_set_style_text_align(category_label, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(category_label, LV_ALIGN_LEFT_MID, 32, 0);
  auto *category_arrow = Label(state->category, LV_SYMBOL_DOWN, &lv_font_montserrat_32, kMuted);
  lv_obj_align(category_arrow, LV_ALIGN_RIGHT_MID, -32, 0);
  state->automatic = lv_obj_create(screen);
  Clear(state->automatic);
  lv_obj_set_pos(state->automatic, 80, top + 138);
  lv_obj_set_size(state->automatic, (width - 24) / 2, 116);
  auto *automatic_label = Label(state->automatic, "Auto-update", &lv_font_montserrat_28, kText);
  lv_obj_set_pos(automatic_label, 0, 30);
  FitLabelToLines(automatic_label, (width - 24) / 2 - 140, 1, {&lv_font_montserrat_28, &lv_font_montserrat_24});
  state->toggle = lv_switch_create(state->automatic);
  lv_obj_set_size(state->toggle, 110, 62);
  lv_obj_align(state->toggle, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_opa(state->toggle, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->toggle, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_radius(state->toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(state->toggle, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(state->toggle, kAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_radius(state->toggle, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(state->toggle, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_bg_color(state->toggle, kText, LV_PART_KNOB);
  lv_obj_set_style_radius(state->toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(state->toggle, -8, LV_PART_KNOB);
  if (RecoveryPreference(Preference::kPluginAutoUpdate)) lv_obj_add_state(state->toggle, LV_STATE_CHECKED);
  lv_obj_add_event_cb(state->toggle, [](lv_event_t *event) {
    auto *state = static_cast<State *>(lv_event_get_user_data(event));
    const bool enabled = lv_obj_has_state(state->toggle, LV_STATE_CHECKED);
    const bool previous = RecoveryPreference(Preference::kPluginAutoUpdate);
    if (!RecoverySetPreference(Preference::kPluginAutoUpdate, enabled) || !RecoverySavePreferences()) {
      RecoverySetPreference(Preference::kPluginAutoUpdate, previous);
      if (previous) lv_obj_add_state(state->toggle, LV_STATE_CHECKED);
      else lv_obj_remove_state(state->toggle, LV_STATE_CHECKED);
      Sheet(state->screen, "Setting unavailable", "This setting could not be changed.");
    } else if (enabled) state->callback(Action::kApplyPluginAutoUpdates, state->context);
  }, LV_EVENT_VALUE_CHANGED, state);
  state->update_all = StoreButton(screen, "Update all", [state] { Request(state, plugins::Job::kUpdateAll); }, true);
  lv_obj_set_pos(state->update_all, 80 + (width + 24) / 2, top + 138);
  lv_obj_set_size(state->update_all, (width - 24) / 2, 116);
  state->scene.status = Label(screen, RecoveryWifiConnection().connected ? "Ready" : "Offline", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->scene.status, 80, top + 282);
  lv_obj_set_width(state->scene.status, width);
  lv_label_set_long_mode(state->scene.status, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_height(state->scene.status, 2 * lv_font_get_line_height(UiFont(&lv_font_montserrat_24)));
  state->scene.progress = lv_bar_create(screen);
  lv_obj_set_pos(state->scene.progress, 80, top + 350);
  lv_obj_set_size(state->scene.progress, width, 12);
  lv_bar_set_range(state->scene.progress, 0, 100);
  lv_obj_set_style_bg_color(state->scene.progress, kMainPanel, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->scene.progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(state->scene.progress, LV_OBJ_FLAG_HIDDEN);
  state->scene.list = Scroll(screen, 820, 1600);
  lv_obj_set_style_pad_bottom(state->scene.list, 28, 0);
  lv_obj_set_scrollbar_mode(state->scene.list, LV_SCROLLBAR_MODE_AUTO);
  Navigation(screen, Action::kNone, callback, context);
  Render(state);
  return state->scene;
}

void RefreshPluginScene(const PluginScene &scene) {
  auto *state = static_cast<State *>(scene.state);
  if (state && !state->busy) Render(state);
}

bool NavigatePluginBack(const PluginScene &scene) {
  auto *state = static_cast<State *>(scene.state);
  if (!state || state->busy || state->detail_id.empty()) return false;
  state->detail_id.clear();
  Render(state);
  lv_obj_scroll_to_y(state->scene.list, state->scroll_position, LV_ANIM_OFF);
  return true;
}

void SetPluginBusy(const PluginScene &scene, const plugins::Request &request) {
  auto *state = static_cast<State *>(scene.state);
  if (!state) return;
  state->busy = true; state->active_job = request.job;
  lv_obj_add_state(scene.refresh, LV_STATE_DISABLED);
  lv_obj_add_state(state->toggle, LV_STATE_DISABLED);
  lv_obj_add_state(state->category, LV_STATE_DISABLED);
  lv_obj_remove_flag(scene.progress, LV_OBJ_FLAG_HIDDEN);
  const char *text = request.job == plugins::Job::kRefresh ? "Refreshing signed store..." :
      request.job == plugins::Job::kRemove ? "Removing plugin..." :
      request.job == plugins::Job::kScreenshot ? "Loading screenshot..." :
      request.job == plugins::Job::kUpdateAll ? "Updating plugins..." :
      request.job == plugins::Job::kInstallMemory || request.job == plugins::Job::kInstallLocalMemory
          ? "Loading plugin into RAM..." : "Installing plugin on storage...";
  i18n::BindLabel(scene.status, text);
  Render(state);
}

void UpdatePluginProgress(const PluginScene &scene, unsigned value,
                          uint64_t downloaded_bytes, uint64_t total_bytes,
                          unsigned completed_plugins, unsigned total_plugins) {
  auto *state = static_cast<State *>(scene.state);
  if (!state || !state->busy) return;
  value = std::min(value, 100U);
  const unsigned overall = total_plugins ?
      std::min(100U, (completed_plugins * 100 + value) / total_plugins) : value;
  lv_bar_set_value(scene.progress, overall, LV_ANIM_OFF);
  std::string message;
  if (total_bytes && value <= 78) {
    message = i18n::Format("Downloading plugin  %.1f / %.1f MB  -  %u%%",
        downloaded_bytes / 1048576.0, total_bytes / 1048576.0,
        static_cast<unsigned>(std::min(downloaded_bytes, total_bytes) * 100 / total_bytes));
  } else message = i18n::Format("Verifying and installing  -  %u%%", value);
  if (state->active_job == plugins::Job::kRefresh)
    message = i18n::Format("Refreshing signed store  -  %u%%", value);
  if (state->active_job == plugins::Job::kRemove) message = i18n::Translate("Removing plugin...");
  if (state->active_job == plugins::Job::kScreenshot) message = i18n::Translate("Loading screenshot...");
  if (total_plugins) message = i18n::Format("Plugin %u of %u", std::min(completed_plugins + 1, total_plugins), total_plugins) + "  /  " + message;
  i18n::BindLabel(scene.status, message.c_str());
}

void CompletePluginOperation(const PluginScene &scene, bool success, const char *message) {
  auto *state = static_cast<State *>(scene.state);
  if (!state) return;
  state->busy = false;
  lv_obj_remove_state(scene.refresh, LV_STATE_DISABLED);
  lv_obj_remove_state(state->toggle, LV_STATE_DISABLED);
  lv_obj_remove_state(state->category, LV_STATE_DISABLED);
  lv_obj_add_flag(scene.progress, LV_OBJ_FLAG_HIDDEN);
  const bool screenshot = state->active_job == plugins::Job::kScreenshot;
  i18n::BindLabel(scene.status, screenshot && success ? "Ready" :
      message && *message ? message : success ? "Plugin operation completed." : "Plugin operation failed.");
  Render(state);
  if (screenshot && success && message) OpenPicture(state->screen, message);
}
}  // namespace aeraui
