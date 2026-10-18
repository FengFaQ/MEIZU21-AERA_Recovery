/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "ui_components.hpp"
#include "plugins/store_metadata.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <png.h>
#include <vector>

using namespace aeraui;
static bool online = true, automatic = false;
static Action last_action = Action::kNone;
static std::vector<plugins::Plugin> catalog, installed;

namespace aeraui::wallpaper { bool Attach(lv_obj_t *) { return false; } }
namespace aeraui {
void AttachStatusBar(lv_obj_t *, void (*)(Action, void *), void *, StatusBarAction, bool) {}
int32_t StatusBarHeight() { return 165; }
DockLayout RecoveryDockLayout() { return DockLayout::kGlass; }
bool RecoveryTintedIconBackgrounds() { return false; }
int RecoveryDockTransparency() { return 60; }
int RecoveryDockBlur() { return 24; }
bool RecoveryDockHideInApps() { return false; }
KeyboardLayout RecoveryKeyboardLayout() { return KeyboardLayout::kQwerty; }
std::string RecoveryUiFont() { return {}; }
void RecoveryVibrate(Haptic) {}
WifiConnection RecoveryWifiConnection() { WifiConnection result; result.connected = online; return result; }
bool RecoveryPreference(Preference) { return automatic; }
bool RecoverySetPreference(Preference, bool value) { automatic = value; return true; }
bool RecoverySavePreferences() { return true; }
void SetSelectedPluginId(const std::string &) {}
void OpenPicture(lv_obj_t *, const std::string &) {}
}
namespace aeraui::plugins {
std::vector<Plugin> Catalog() { return catalog; }
std::vector<Plugin> Installed() { return installed; }
bool IsGeneric(const Plugin &p) { return p.entry == "main"; }
bool IsThemeExtension(const Plugin &p) { return p.type == "theme-extension"; }
bool IsUpdateAvailable(const Plugin &local, const Plugin &release) {
  return local.trust == Trust::kOfficial && local.id == release.id && local.version != release.version;
}
const char *LocationLabel(Location value) { return value == Location::kMemory ? "RAM only" : "Storage"; }
}

static void Tick() { for (int i = 0; i < 8; ++i) { lv_tick_inc(32); lv_timer_handler(); } }
static lv_obj_t *LabelWith(lv_obj_t *root, const char *text) {
  if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
  for (int i = static_cast<int>(lv_obj_get_child_count(root)) - 1; i >= 0; --i)
    if (auto *value = LabelWith(lv_obj_get_child(root, i), text)) return value;
  return nullptr;
}
static lv_obj_t *Type(lv_obj_t *root, const lv_obj_class_t *type) {
  if (lv_obj_check_type(root, type)) return root;
  for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
    if (auto *value = Type(lv_obj_get_child(root, i), type)) return value;
  return nullptr;
}
static lv_obj_t *SearchButton(lv_obj_t *root) {
  auto *handle = Type(root, &lv_line_class); assert(handle);
  auto *button = lv_obj_get_parent(lv_obj_get_parent(handle));
  assert(lv_obj_check_type(button, &lv_button_class));
  return button;
}
static void ClickSearch(lv_obj_t *root) {
  lv_obj_send_event(SearchButton(root), LV_EVENT_CLICKED, nullptr); Tick();
}
static void Click(lv_obj_t *root, const char *text, bool translate = true) {
  const char *wanted = translate ? i18n::Translate(text) : text;
  // Category headings are not buttons. Prefer the topmost matching action.
  std::function<lv_obj_t *(lv_obj_t *)> find_action = [&](lv_obj_t *object) -> lv_obj_t * {
    for (int i = static_cast<int>(lv_obj_get_child_count(object)) - 1; i >= 0; --i)
      if (auto *match = find_action(lv_obj_get_child(object, i))) return match;
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), wanted)) {
      for (auto *parent = lv_obj_get_parent(object); parent; parent = lv_obj_get_parent(parent))
        if (lv_obj_check_type(parent, &lv_button_class)) return object;
    }
    return nullptr;
  };
  auto *label = find_action(root);
  assert(label);
  auto *object = lv_obj_get_parent(label);
  while (!lv_obj_check_type(object, &lv_button_class) && lv_obj_get_parent(object)) object = lv_obj_get_parent(object);
  assert(!lv_obj_has_state(object, LV_STATE_DISABLED));
  lv_obj_send_event(object, LV_EVENT_CLICKED, nullptr); Tick();
}
static void Inside(lv_obj_t *label) {
  assert(label);
  lv_obj_update_layout(label);
  lv_area_t inner{}, outer{};
  lv_obj_get_coords(label, &inner); lv_obj_get_coords(lv_obj_get_parent(label), &outer);
  if (inner.y1 < outer.y1 || inner.y2 > outer.y2)
    fprintf(stderr, "Outside parent: %s child=(%d,%d) parent=(%d,%d)\n",
        lv_obj_check_type(label, &lv_label_class) ? lv_label_get_text(label) : "widget",
        inner.y1, inner.y2, outer.y1, outer.y2);
  assert(inner.x1 >= outer.x1 && inner.x2 <= outer.x2);
  assert(inner.y1 >= outer.y1 && inner.y2 <= outer.y2);
}
static void Seed() {
  catalog.clear(); installed.clear();
  for (const auto &entry : std::vector<std::pair<std::string, std::string>>{
      {"browser", "network"}, {"sysinfo", "tools"}, {"appvault", "backup"}, {"font-inter", "themes"}}) {
    plugins::Plugin p; p.id = entry.first; p.name = "AERA " + entry.first;
    p.category = entry.second; p.version = "1.1.0"; p.package_size = 12345678;
    p.description = "A localized short description that can wrap into two lines without touching the version or status below.";
    p.details = "A longer plugin description. It remains readable without being squeezed into a fixed-height box.\n\n"
        "The detail view has room for features, download information, installation location, and clearly separated actions.";
    p.entry = p.id == "browser" ? "browser" : "main";
    if (p.id == "font-inter") { p.type = "theme-extension"; p.entry = "font"; }
    catalog.push_back(p);
    if (p.id != "appvault") { p.version = "1.0.0"; p.location = plugins::Location::kStorage; installed.push_back(p); }
  }
  plugins::Plugin p; p.id = "local-test"; p.name = "Local custom plugin"; p.version = "2.0.0";
  p.description = "A manually installed plugin."; p.entry = "main";
  p.location = plugins::Location::kMemory; p.trust = plugins::Trust::kUnofficial; installed.push_back(p);
}

int main(int argc, char **argv) {
  Json::Value root; root["store"]["category"] = "backup";
  root["store"]["description"] = "English details";
  root["store"]["localizations"]["de_DE"]["description"] = "Deutsche Beschreibung";
  root["store"]["screenshots"].append("https://raw.githubusercontent.com/AERA-Plugins/registry/main/screenshots/example.png");
  root["store"]["screenshots"].append("https://example.invalid/image.png");
  plugins::Plugin metadata;
  plugins::ApplyStoreMetadata(root, metadata, "de-DE");
  assert(metadata.category == "backup" && metadata.details == "Deutsche Beschreibung");
  assert(metadata.screenshots.size() == 1);
  metadata = {}; plugins::ApplyStoreMetadata(root, metadata, "sv_SE");
  assert(metadata.details == "English details");
  metadata = {}; plugins::ApplyStoreMetadata(Json::Value{}, metadata, "en");
  assert(metadata.category == "tools" && metadata.details.empty());
  root["store"]["localizations"]["de_DE"] = 42;
  metadata = {}; plugins::ApplyStoreMetadata(root, metadata, "de_DE");
  assert(metadata.details == "English details");

  lv_init(); i18n::Initialize("en");
  auto *display = lv_display_create(1440, 3168);
  std::vector<uint8_t> frame(3168 * 3168 * 4);
  lv_display_set_buffers(display, frame.data(), nullptr, frame.size(), LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); });
  auto *home = lv_screen_active();
  const bool preview = argc > 1;
  if (argc > 2) {
    const std::string language = argc > 3 ? argv[3] : "de_DE";
    i18n::SetLanguage(language); design::ApplyInterfaceSize(2);
    Json::Value document;
    std::ifstream stream(argv[2]); stream >> document;
    for (const auto &entry : document["plugins"]) {
      plugins::Plugin p;
      p.id = entry["id"].asString(); p.name = entry["name"].asString();
      p.version = entry["version"].asString(); p.description = entry["description"].asString();
      p.package_size = entry["package_size"].asUInt64();
      plugins::ApplyStoreMetadata(entry, p, language);
      p.entry = p.id == "browser" ? "browser" : "main";
      if (p.category == "themes") { p.type = "theme-extension"; p.entry = "font"; }
      catalog.push_back(p);
      if (p.id == "browser" || p.id == "sysinfo" || p.id == "appvault") {
        p.version = "0.0.0"; p.location = plugins::Location::kStorage; installed.push_back(p);
      }
    }
    auto *screen = lv_obj_create(nullptr);
    const auto scene = BuildPluginScene(screen, [](Action, void *) {}, nullptr);
    lv_screen_load(screen); Tick();
    auto save = [&](const char *name) {
      png_image image{}; image.version = PNG_IMAGE_VERSION; image.width = 1440; image.height = 3168; image.format = PNG_FORMAT_BGRA;
      assert(png_image_write_to_file(&image,
          (std::string("/tmp/plugin-") + name + "-" + language + ".png").c_str(), 0, frame.data(), 0, nullptr));
    };
    save("store");
    Click(screen, "All"); save("categories");
    assert(widgets::DismissModal(screen)); Tick();
    ClickSearch(screen); save("search");
    assert(widgets::DismissModal(screen)); Tick();
    const auto browser = std::find_if(catalog.begin(), catalog.end(), [](const auto &p) { return p.id == "browser"; });
    assert(browser != catalog.end()); Click(screen, browser->name.c_str(), false); save("detail");
    assert(NavigatePluginBack(scene)); Click(screen, "Updates"); save("updates");
    lv_screen_load(home); lv_obj_delete(screen); lv_display_delete(display); lv_deinit();
    return 0;
  }
  for (const char *language : {"en", "de_DE"}) for (bool landscape : {false, true}) for (int size : {0, 1, 2, 3}) {
    if (preview && (strcmp(language, "de_DE") || landscape || size != 2)) continue;
    lv_display_set_resolution(display, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
    i18n::SetLanguage(language); design::ApplyInterfaceSize(size); Seed(); online = true;
    auto *screen = lv_obj_create(nullptr);
    auto scene = BuildPluginScene(screen, [](Action action, void *) { last_action = action; }, nullptr);
    lv_screen_load(screen); Tick();
    assert(!LabelWith(screen, i18n::Translate("Search plugins")));
    auto *search_button = SearchButton(screen);
    assert(lv_obj_has_flag(lv_obj_get_child(search_button, 0), LV_OBJ_FLAG_HIDDEN)); Inside(search_button);
    auto *store_tab = lv_obj_get_parent(LabelWith(screen, i18n::Translate("Store")));
    assert(lv_obj_get_style_border_width(store_tab, LV_PART_MAIN) == 2);
    assert(lv_color_eq(lv_obj_get_style_bg_color(store_tab, LV_PART_MAIN), design::kMainPanel));
    int last_bottom = -1;
    for (const char *category : {"Tools", "Backup", "Network", "Themes"}) {
      auto *heading = LabelWith(scene.list, i18n::Translate(category)); assert(heading);
      assert(lv_obj_get_parent(heading) == scene.list);
      lv_area_t bounds{}; lv_obj_get_coords(heading, &bounds);
      assert(bounds.y1 > last_bottom); last_bottom = bounds.y2;
    }
    assert(!LabelWith(scene.list, i18n::Translate("Install on storage")));
    Click(screen, "All");
    auto *choice = LabelWith(screen, i18n::Translate("Themes")); assert(choice); Inside(choice);
    Click(screen, "Themes");
    assert(LabelWith(scene.list, "AERA font-inter"));
    assert(!LabelWith(scene.list, "AERA browser"));
    Click(screen, "Themes"); Click(screen, "All");
    auto save = [&](const char *name) {
      if (!preview) return;
      png_image image{}; image.version = PNG_IMAGE_VERSION; image.width = 1440; image.height = 3168; image.format = PNG_FORMAT_BGRA;
      assert(png_image_write_to_file(&image, (std::string("/tmp/plugin-") + name + ".png").c_str(), 0, frame.data(), 0, nullptr));
    };
    auto *name = LabelWith(scene.list, "AERA browser"); assert(name); Inside(name);
    auto *description = LabelWith(scene.list, catalog[0].description.c_str()); assert(description); Inside(description);
    save("store");
    Click(screen, "AERA browser", false);
    assert(LabelWith(scene.list, catalog[0].details.c_str()));
    auto *update = LabelWith(scene.list, i18n::Translate("Update on storage")); assert(update); Inside(update);
    save("detail");
    assert(NavigatePluginBack(scene)); assert(!NavigatePluginBack(scene)); Tick();
    assert(!LabelWith(screen, LV_SYMBOL_DIRECTORY));
    ClickSearch(screen);
    auto *input = Type(screen, &lv_textarea_class); assert(input); Inside(input);
    auto *keyboard = Type(screen, &lv_keyboard_class); assert(keyboard); Inside(keyboard);
    lv_area_t keyboard_bounds{}, input_bounds{}, action_bounds{};
    lv_obj_get_coords(keyboard, &keyboard_bounds);
    lv_obj_get_coords(input, &input_bounds);
    auto *accept = lv_obj_get_parent(LabelWith(screen, i18n::Translate("Search")));
    lv_obj_get_coords(accept, &action_bounds);
    assert(input_bounds.y2 < action_bounds.y1 && action_bounds.y2 < keyboard_bounds.y1);
    assert(lv_obj_get_height(input) >= 160);
    lv_textarea_set_text(input, "appvault");
    lv_obj_send_event(keyboard, LV_EVENT_READY, nullptr); Tick();
    assert(LabelWith(scene.list, "AERA appvault")); assert(!LabelWith(scene.list, "AERA browser"));
    Click(screen, "Installed"); assert(LabelWith(scene.list, "Local custom plugin"));
    Click(screen, "Local custom plugin", false);
    assert(!LabelWith(scene.list, i18n::Translate("Reinstall")));
    Click(screen, "Open"); assert(last_action == Action::kPluginApp);
    assert(NavigatePluginBack(scene)); Tick();
    Click(screen, "Updates"); assert(!LabelWith(scene.list, "Local custom plugin"));
    auto *toggle = Type(screen, &lv_switch_class); assert(toggle); Inside(toggle);
    assert(lv_obj_get_style_bg_opa(toggle, LV_PART_KNOB) == LV_OPA_COVER);
    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr); Tick();
    assert(automatic && last_action == Action::kApplyPluginAutoUpdates);
    Click(screen, "Update all"); assert(GetPluginRequest().job == plugins::Job::kUpdateAll);
    SetPluginBusy(scene, GetPluginRequest()); UpdatePluginProgress(scene, 50, 512, 1024, 1, 3);
    assert(!NavigatePluginBack(scene));
    CompletePluginOperation(scene, true, "All plugins are up to date.");
    online = false; Click(screen, "Store"); assert(LabelWith(scene.list, "AERA browser"));
    Click(screen, "Installed"); Click(screen, "AERA browser", false); Click(screen, "Open"); assert(last_action == Action::kWeb);
    lv_screen_load(home); lv_obj_delete(screen); Tick();
  }
  lv_display_delete(display); lv_deinit();
}
