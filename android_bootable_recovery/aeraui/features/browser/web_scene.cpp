// SPDX-License-Identifier: Apache-2.0
#include "scene.hpp"
#include "../../core/operation_affinity.hpp"
#include "ui_components.hpp"
#include "protocol.hpp"
#include "launcher.hpp"
#include "phone_keyboard.hpp"
#include "aeraui/status_bar.hpp"
#include "runtime.hpp"
#include "session.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <cmath>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <unistd.h>

namespace aeraui {
namespace {
using namespace widgets;
constexpr int kBrowserViewportWidth = 1392;
constexpr int kBrowserViewportHeight = 2708;
constexpr int kBrowserImageScale =
    (kBrowserViewportWidth * 256 + web::kWidth / 2) / web::kWidth;
struct WebScene;
void BrowserRefreshReady(lv_event_t *event);
void HideKeyboard(WebScene *s);
void ResetBrowserTouches(WebScene *s);
void ShowBrowserSettings(WebScene *s);
WebScene *gWebScene = nullptr;
WebScene *gPersistentWebScene = nullptr;

struct WebScene {
  lv_obj_t *screen = nullptr, *address = nullptr, *keyboard = nullptr;
  lv_display_t *display = nullptr;
  lv_obj_t *navigation = nullptr;
  lv_obj_t *title = nullptr, *detail = nullptr, *progress = nullptr, *prepare = nullptr;
  lv_obj_t *area = nullptr, *viewport = nullptr, *image = nullptr;
  lv_obj_t *web_progress = nullptr;
  lv_obj_t *controls[6]{};
  lv_obj_t *manager = nullptr, *download_list = nullptr;
  lv_image_dsc_t descriptor{};
  web::Session session;
  web::BrowserProcess process;
  bool showing_web = false, web_keyboard = false;
  std::string last_status;
  unsigned last_progress = 101;
  lv_timer_t *timer = nullptr;
  web::Preparation state;
  std::thread worker;
  bool started = false, finished = false, auto_launch = true;
  bool persistent = false;
  bool manager_open = false;
  bool settings_open = false;
  bool frame_waiting_for_refresh = false;
  bool touch_pressed[2]{};
  bool touch_ignored[2]{};
  bool pinch_active = false;
  double pinch_start_distance = 0.0;
  unsigned pinch_start_zoom = 100;
  uint32_t last_download_revision = UINT32_MAX;
  uint32_t last_settings_revision = UINT32_MAX;
  lv_obj_t *settings_status = nullptr;
  std::unordered_set<uint32_t> announced_downloads;
  int view_left = 24, view_top = 460;
  int view_width = kBrowserViewportWidth;
  int view_height = kBrowserViewportHeight;
  ~WebScene() {
    if (timer) lv_timer_delete(timer);
    state.cancel.store(true);
    if (worker.joinable()) worker.join();
    if (session.Connected()) session.Send(web::Kind::kClose);
    process.Stop();
    session.Close();
    web::RemoveRuntime(state.directory);
  }
};

void DetachWebScene(WebScene *s) {
  if (gWebScene == s) gWebScene = nullptr;
  if (s->display)
    lv_display_remove_event_cb_with_user_data(
        s->display, BrowserRefreshReady, s);
  if (s->frame_waiting_for_refresh) s->session.AcknowledgeFrame();
  s->frame_waiting_for_refresh = false;
  if (s->image) {
    lv_image_set_src(s->image, nullptr);
    lv_image_cache_drop(&s->descriptor);
  }
  s->screen = nullptr;
  s->display = nullptr;
  s->address = nullptr;
  s->keyboard = nullptr;
  s->navigation = nullptr;
  s->title = nullptr;
  s->detail = nullptr;
  s->progress = nullptr;
  s->prepare = nullptr;
  s->area = nullptr;
  s->viewport = nullptr;
  s->image = nullptr;
  s->web_progress = nullptr;
  for (auto &control : s->controls) control = nullptr;
  s->manager = nullptr;
  s->download_list = nullptr;
  s->settings_status = nullptr;
  s->descriptor = {};
  s->showing_web = false;
  s->web_keyboard = false;
  s->manager_open = false;
  s->settings_open = false;
  ResetBrowserTouches(s);
}

std::string FormatBytes(uint64_t bytes) {
  char text[48];
  if (bytes >= 1024ULL * 1024 * 1024)
    snprintf(text, sizeof(text), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
  else if (bytes >= 1024ULL * 1024)
    snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024));
  else if (bytes >= 1024)
    snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
  else
    snprintf(text, sizeof(text), "%llu B",
             static_cast<unsigned long long>(bytes));
  return text;
}

void ShowDownloadStarted(WebScene *s, const web::DownloadItem &download) {
  auto *toast = lv_obj_create(lv_layer_top());
  NoScroll(toast);
  Panel(toast, 48, kMainSheet);
  const int width = std::clamp(lv_obj_get_width(s->screen) - 96, 680, 980);
  const int resting_y = StatusBarHeight() + 24;
  lv_obj_set_size(toast, width, 142);
  lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, -166);
  lv_obj_set_style_border_width(toast, 1, 0);
  lv_obj_set_style_border_color(toast, kAccent, 0);
  lv_obj_set_style_border_opa(toast, LV_OPA_50, 0);
  auto *icon = Label(toast, LV_SYMBOL_DOWNLOAD,
                     &lv_font_montserrat_36, kAccent);
  lv_obj_align(icon, LV_ALIGN_LEFT_MID, 42, 0);
  auto *title = Label(toast, "Download started",
                      &lv_font_montserrat_28, kText);
  lv_obj_set_pos(title, 108, 28);
  auto *name = Label(toast, download.name.c_str(),
                     &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(name, 108, 78);
  lv_obj_set_width(name, width - 150);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_anim_t enter;
  lv_anim_init(&enter);
  lv_anim_set_var(&enter, toast);
  lv_anim_set_values(&enter, -166, resting_y);
  lv_anim_set_duration(&enter, 210);
  lv_anim_set_path_cb(&enter, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&enter, [](void *target, int32_t y) {
    lv_obj_set_y(static_cast<lv_obj_t *>(target), y);
  });
  lv_anim_start(&enter);
  lv_obj_delete_delayed(toast, 1850);
}

void ResetBrowserTouches(WebScene *s) {
  for (int slot = 0; slot < 2; ++slot) {
    s->touch_pressed[slot] = false;
    s->touch_ignored[slot] = false;
  }
  s->pinch_active = false;
}

void RefreshDownloads(WebScene *s) {
  if (!s->manager || !s->download_list) return;
  s->last_download_revision = s->session.DownloadRevision();
  lv_obj_clean(s->download_list);
  const auto &downloads = s->session.Downloads();
  if (downloads.empty()) {
    auto *icon = Label(s->download_list, LV_SYMBOL_DOWNLOAD,
                       &lv_font_montserrat_48, kMutedStrong);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 130);
    auto *empty = Label(s->download_list, "No downloads yet",
                        &lv_font_montserrat_36, kText);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 220);
    auto *hint = Label(s->download_list,
        "Files downloaded from websites appear here.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 282);
    return;
  }
  const int width = lv_obj_get_width(s->download_list) - 12;
  int y = 0;
  for (const auto &download : downloads) {
    auto *row = lv_obj_create(s->download_list);
    NoScroll(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, width, 186);
    lv_obj_set_style_radius(row, 34, 0);
    lv_obj_set_style_bg_color(row, kMainPanel, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, kMainLine, 0);
    auto *icon = Label(row,
        download.status == web::DownloadStatus::kFinished ? LV_SYMBOL_OK :
        download.status == web::DownloadStatus::kFailed ? LV_SYMBOL_WARNING :
        download.status == web::DownloadStatus::kCancelled ? LV_SYMBOL_CLOSE :
        LV_SYMBOL_DOWNLOAD,
        &lv_font_montserrat_36,
        download.status == web::DownloadStatus::kFinished ? kGreen :
        download.status == web::DownloadStatus::kFailed ? kRed : kAccent);
    lv_obj_set_pos(icon, 28, 30);
    auto *name = Label(row, download.name.c_str(),
                       &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 88, 24);
    lv_obj_set_width(name, width -
        (download.status == web::DownloadStatus::kActive ? 265 : 130));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    std::string detail;
    if (download.status == web::DownloadStatus::kActive) {
      detail = FormatBytes(download.received_bytes);
      if (download.total_bytes)
        detail = i18n::Format("%s of %s", detail.c_str(),
                              FormatBytes(download.total_bytes).c_str());
      detail = i18n::Format("%s  •  %d%%", detail.c_str(),
                            download.progress);
    } else if (download.status == web::DownloadStatus::kFinished) {
      detail = i18n::Format("Saved to AERA/Downloads  •  %s",
                            FormatBytes(download.received_bytes).c_str());
    } else if (download.status == web::DownloadStatus::kCancelled) {
      detail = "Download cancelled";
    } else {
      detail = download.error.empty() ? "Download failed" : download.error;
    }
    auto *status = Label(row, detail.c_str(), &lv_font_montserrat_24,
                         kMutedStrong);
    lv_obj_set_pos(status, 88, 78);
    lv_obj_set_width(status, width - 125);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    auto *progress = lv_bar_create(row);
    lv_obj_set_pos(progress, 88, 137);
    lv_obj_set_size(progress, width - 126, 10);
    lv_obj_set_style_bg_color(progress, kMainLine, 0);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(progress,
        download.status == web::DownloadStatus::kFinished ? kGreen : kAccent,
        LV_PART_INDICATOR);
    lv_bar_set_value(progress,
        download.status == web::DownloadStatus::kFinished ? 100 :
        static_cast<int>(download.progress), LV_ANIM_OFF);
    if (download.status == web::DownloadStatus::kActive) {
      auto *cancel = Button(row, LV_SYMBOL_CLOSE,
          [s, id = download.id] { s->session.CancelDownload(id); });
      lv_obj_set_pos(cancel, width - 146, 20);
      lv_obj_set_size(cancel, 112, 82);
      lv_obj_set_style_radius(cancel, 41, 0);
    }
    y += 204;
  }
}

void SetDownloadManager(WebScene *s, bool visible) {
  HideKeyboard(s);
  ResetBrowserTouches(s);
  if (!s->manager) {
    s->manager = lv_obj_create(s->screen);
    lv_obj_set_user_data(s->manager, &kPersistentModalMarker);
    lv_obj_add_event_cb(s->manager, [](lv_event_t *event) {
      SetDownloadManager(
          static_cast<WebScene *>(lv_event_get_user_data(event)), false);
    }, LV_EVENT_CANCEL, s);
    NoScroll(s->manager);
    lv_obj_set_pos(s->manager, s->view_left, s->view_top);
    lv_obj_set_size(s->manager, s->view_width, s->view_height);
    lv_obj_set_style_radius(s->manager, 36, 0);
    lv_obj_set_style_bg_color(s->manager, kMainCanvas, 0);
    lv_obj_set_style_bg_opa(s->manager, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s->manager, 1, 0);
    lv_obj_set_style_border_color(s->manager, kMainLine, 0);
    auto *title = Label(s->manager, "Downloads",
                        &lv_font_montserrat_48, kText);
    lv_obj_set_pos(title, 38, 30);
    auto *folder = Label(s->manager,
        "Saved to /sdcard/AERA/Downloads",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(folder, 40, 96);
    auto *close = Button(s->manager, LV_SYMBOL_CLOSE,
                         [s] { SetDownloadManager(s, false); });
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -26, 22);
    lv_obj_set_size(close, 108, 86);
    lv_obj_set_style_radius(close, 43, 0);
    s->download_list = lv_obj_create(s->manager);
    lv_obj_set_pos(s->download_list, 24, 150);
    lv_obj_set_size(s->download_list, s->view_width - 48,
                    s->view_height - 174);
    lv_obj_set_style_bg_opa(s->download_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->download_list, 0, 0);
    lv_obj_set_style_pad_all(s->download_list, 0, 0);
    lv_obj_set_scroll_dir(s->download_list, LV_DIR_VER);
    RefreshDownloads(s);
  }
  s->manager_open = visible;
  if (visible) {
    lv_obj_add_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s->manager, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s->manager);
  } else {
    lv_obj_add_flag(s->manager, LV_OBJ_FLAG_HIDDEN);
    if (s->showing_web) lv_obj_remove_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
  }
}

void BrowserRefreshReady(lv_event_t *event) {
  auto *s = static_cast<WebScene *>(lv_event_get_user_data(event));
  if (!s->frame_waiting_for_refresh) return;
  s->frame_waiting_for_refresh = false;
  if (s->session.Connected()) s->session.AcknowledgeFrame();
}

void HideKeyboard(WebScene *s) {
  lv_obj_add_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(s->keyboard, nullptr);
  lv_obj_remove_state(s->address, LV_STATE_FOCUSED);
  // Plugin navigation remains hidden; the global bottom-edge gesture owns
  // switching away from the browser.
  s->web_keyboard = false;
}

void OpenAddressBar(WebScene *s) {
  auto address = web::Address(lv_textarea_get_text(s->address));
  if (address.empty()) {
    HideKeyboard(s);
    Sheet(s->screen, "Website address", "Enter an HTTP or HTTPS address, such as example.org. Local files and executable URLs are not supported.");
    return;
  }
  lv_textarea_set_text(s->address, address.c_str());
  HideKeyboard(s);
  if (s->session.Connected()) {
    s->session.Send(web::Kind::kOpen, 0, 0, 0, address.c_str());
    return;
  }
  Sheet(s->screen, "Browsing unavailable", web::LaunchBlockReason());
}

void ShowKeyboard(WebScene *s, bool web_keyboard, uint32_t purpose = 0,
                  lv_obj_t *target = nullptr) {
  ResetBrowserTouches(s);
  s->web_keyboard = web_keyboard;
  lv_keyboard_set_mode(s->keyboard,
      web_keyboard && (purpose == 2 || purpose == 9)
          ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(s->keyboard,
                           web_keyboard ? nullptr : target ? target : s->address);
  if (s->navigation) lv_obj_add_flag(s->navigation, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s->keyboard);
}

bool SupportsPersistentBrowserSettings(const WebScene *s) {
  unsigned major = 0, minor = 0;
  return sscanf(s->state.version.c_str(), "%u.%u", &major, &minor) == 2 &&
      (major > 1 || (major == 1 && minor >= 5));
}

struct BrowserSettingsUi {
  WebScene *scene = nullptr;
  lv_obj_t *overlay = nullptr;
  lv_obj_t *homepage = nullptr;
  lv_obj_t *zoom_value = nullptr;
  lv_obj_t *cookie_buttons[3]{};
  int zoom = 100;
  BrowserCookiePolicy cookies = BrowserCookiePolicy::kFirstParty;
};

void StyleCookieChoice(BrowserSettingsUi *settings) {
  const int selected = static_cast<int>(settings->cookies);
  for (int i = 0; i < 3; ++i) {
    const bool active = i == selected;
    lv_obj_set_style_bg_color(settings->cookie_buttons[i],
                              active ? kAccentSoft : kMainPanel, 0);
    lv_obj_set_style_border_width(settings->cookie_buttons[i], 1, 0);
    lv_obj_set_style_border_color(settings->cookie_buttons[i],
                                  active ? kAccent : kMainLine, 0);
    auto *label = lv_obj_get_child(settings->cookie_buttons[i], 0);
    if (label) lv_obj_set_style_text_color(label, active ? kAccent : kText, 0);
  }
}

void CloseBrowserSettings(BrowserSettingsUi *settings) {
  HideKeyboard(settings->scene);
  settings->scene->settings_open = false;
  settings->scene->settings_status = nullptr;
  lv_obj_delete_async(settings->overlay);
}

void ShowBrowserSettings(WebScene *s) {
  HideKeyboard(s);
  ResetBrowserTouches(s);
  if (s->settings_open) return;
  s->settings_open = true;

  auto *settings = new BrowserSettingsUi;
  settings->scene = s;
  settings->zoom = RecoveryBrowserZoom();
  settings->cookies = RecoveryBrowserCookiePolicy();
  settings->overlay = lv_obj_create(s->screen);
  auto *overlay = settings->overlay;
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_pos(overlay, 0, StatusBarHeight());
  lv_obj_set_size(overlay, lv_obj_get_width(s->screen),
                  lv_obj_get_height(s->screen) - StatusBarHeight());
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    auto *settings = static_cast<BrowserSettingsUi *>(
        lv_event_get_user_data(event));
    settings->scene->settings_open = false;
    settings->scene->settings_status = nullptr;
    delete settings;
  }, LV_EVENT_DELETE, settings);

  auto *close = Button(overlay, LV_SYMBOL_LEFT,
                       [settings] { CloseBrowserSettings(settings); });
  lv_obj_set_pos(close, 36, 30);
  lv_obj_set_size(close, 106, 92);
  lv_obj_set_style_radius(close, 46, 0);
  auto *title = Label(overlay, "Browser settings",
                      &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 174, 46);
  auto *subtitle = Label(overlay,
      "Homepage, page scale and private website data",
      &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(subtitle, 176, 106);

  const int content_width = std::min(1280, lv_obj_get_width(overlay) - 80);
  auto *content = lv_obj_create(overlay);
  Clear(content);
  lv_obj_set_pos(content, (lv_obj_get_width(overlay) - content_width) / 2, 176);
  lv_obj_set_size(content, content_width, lv_obj_get_height(overlay) - 360);
  lv_obj_set_scroll_dir(content, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_ACTIVE);

  auto *home_title = Label(content, "Homepage",
                           &lv_font_montserrat_32, kText);
  lv_obj_set_pos(home_title, 4, 12);
  auto *home_hint = Label(content,
      "Opened at first launch and by the Home button",
      &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(home_hint, 4, 60);
  settings->homepage = TextArea(content);
  text_edit::SetSelectAllOnHold(settings->homepage, true);
  lv_obj_set_pos(settings->homepage, 4, 108);
  lv_obj_set_size(settings->homepage, content_width - 8, 112);
  lv_textarea_set_one_line(settings->homepage, true);
  lv_textarea_set_max_length(settings->homepage, 2040);
  lv_textarea_set_text(settings->homepage,
                       RecoveryBrowserHomepage().c_str());
  lv_obj_set_style_text_font(settings->homepage,
                             UiFont(&lv_font_montserrat_28), 0);
  lv_obj_set_style_text_color(settings->homepage, kText, 0);
  lv_obj_set_style_bg_color(settings->homepage, kMainPanel, 0);
  lv_obj_set_style_border_color(settings->homepage, kMainLine, 0);
  lv_obj_set_style_border_width(settings->homepage, 1, 0);
  lv_obj_set_style_radius(settings->homepage, 42, 0);
  lv_obj_set_style_pad_all(settings->homepage, 25, 0);
  lv_obj_add_event_cb(settings->homepage, [](lv_event_t *event) {
    auto *settings = static_cast<BrowserSettingsUi *>(
        lv_event_get_user_data(event));
    ShowKeyboard(settings->scene, false, 0, settings->homepage);
  }, LV_EVENT_CLICKED, settings);
  auto *use_current = Button(content, "Use current page", [settings] {
    const auto address = web::Address(settings->scene->session.Status());
    if (!address.empty()) lv_textarea_set_text(settings->homepage,
                                                address.c_str());
  });
  lv_obj_set_pos(use_current, 4, 240);
  lv_obj_set_size(use_current, content_width - 8, 104);
  lv_obj_set_style_radius(use_current, 46, 0);

  auto *zoom_title = Label(content, "Default zoom",
                           &lv_font_montserrat_32, kText);
  lv_obj_set_pos(zoom_title, 4, 408);
  settings->zoom_value = Label(content, "", &lv_font_montserrat_28, kAccent);
  lv_obj_align_to(settings->zoom_value, zoom_title,
                  LV_ALIGN_OUT_RIGHT_MID, 24, 0);
  char zoom_text[16];
  snprintf(zoom_text, sizeof(zoom_text), "%d%%", settings->zoom);
  lv_label_set_text(settings->zoom_value, zoom_text);
  auto *zoom = lv_slider_create(content);
  lv_obj_set_pos(zoom, 18, 490);
  lv_obj_set_size(zoom, content_width - 36, 28);
  lv_slider_set_range(zoom, 50, 300);
  lv_slider_set_value(zoom, settings->zoom, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(zoom, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_bg_color(zoom, kAccent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(zoom, kAccent, LV_PART_KNOB);
  lv_obj_set_style_pad_all(zoom, 14, LV_PART_KNOB);
  lv_obj_add_event_cb(zoom, [](lv_event_t *event) {
    auto *settings = static_cast<BrowserSettingsUi *>(
        lv_event_get_user_data(event));
    settings->zoom = lv_slider_get_value(lv_event_get_target_obj(event));
    char text[16];
    snprintf(text, sizeof(text), "%d%%", settings->zoom);
    lv_label_set_text(settings->zoom_value, text);
  }, LV_EVENT_VALUE_CHANGED, settings);

  auto *cookie_title = Label(content, "Cookies & privacy",
                             &lv_font_montserrat_32, kText);
  lv_obj_set_pos(cookie_title, 4, 604);
  auto *cookie_hint = Label(content,
      SupportsPersistentBrowserSettings(s)
          ? "Stored privately in recovery; first-party only is recommended"
          : "Install Browser 1.5 or newer to enable persistent cookies",
      &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(cookie_hint, 4, 652);
  lv_obj_set_width(cookie_hint, content_width - 8);
  const char *cookie_names[] = {"Block all", "First-party", "Allow all"};
  const int choice_gap = 16;
  const int choice_width = (content_width - 8 - choice_gap * 2) / 3;
  for (int i = 0; i < 3; ++i) {
    settings->cookie_buttons[i] = Button(content, cookie_names[i],
        [settings, i] {
          settings->cookies = static_cast<BrowserCookiePolicy>(i);
          StyleCookieChoice(settings);
        });
    lv_obj_set_pos(settings->cookie_buttons[i],
                   4 + i * (choice_width + choice_gap), 724);
    lv_obj_set_size(settings->cookie_buttons[i], choice_width, 108);
    lv_obj_set_style_radius(settings->cookie_buttons[i], 48, 0);
  }
  StyleCookieChoice(settings);

  auto *clear = Button(content, "Clear cookies & site data", [settings] {
    if (!SupportsPersistentBrowserSettings(settings->scene)) {
      lv_label_set_text(settings->scene->settings_status,
                        "Browser 1.5 or newer is required.");
      return;
    }
    if (settings->scene->session.Send(web::Kind::kClearBrowsingData))
      lv_label_set_text(settings->scene->settings_status,
                        "Clearing website data...");
  });
  lv_obj_set_pos(clear, 4, 862);
  lv_obj_set_size(clear, content_width - 8, 110);
  lv_obj_set_style_radius(clear, 48, 0);
  s->settings_status = Label(content, "", &lv_font_montserrat_24,
                             kMutedStrong);
  lv_obj_set_pos(s->settings_status, 12, 992);
  lv_obj_set_width(s->settings_status, content_width - 24);
  lv_obj_set_style_text_align(s->settings_status, LV_TEXT_ALIGN_CENTER, 0);

  auto *save = Button(overlay, "Save browser settings", [settings] {
    auto homepage = web::Address(lv_textarea_get_text(settings->homepage));
    if (homepage.empty()) {
      lv_label_set_text(settings->scene->settings_status,
                        "Enter a valid HTTP or HTTPS homepage.");
      return;
    }
    const bool ok = RecoverySetBrowserHomepage(homepage) &&
        RecoverySetBrowserZoom(settings->zoom) &&
        RecoverySetBrowserCookiePolicy(settings->cookies) &&
        RecoverySavePreferences();
    if (!ok) {
      lv_label_set_text(settings->scene->settings_status,
                        "Could not save browser settings.");
      return;
    }
    settings->scene->session.SetZoom(settings->zoom);
    if (SupportsPersistentBrowserSettings(settings->scene))
      settings->scene->session.Send(web::Kind::kSetCookiePolicy, 0, 0,
                                    static_cast<uint32_t>(settings->cookies));
    auto *screen = settings->scene->screen;
    CloseBrowserSettings(settings);
    Sheet(screen, "Browser settings saved",
          "Homepage, default zoom and cookie policy were updated.");
  }, true);
  lv_obj_set_size(save, content_width, 116);
  lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, -32);
  lv_obj_set_style_radius(save, 50, 0);
  lv_obj_move_foreground(overlay);
}

void Prepare(WebScene *s) {
  HideKeyboard(s);
  if (s->started) return;
  if (!web::RuntimeInstalled()) {
    i18n::BindLabel(s->title, "Browser package unavailable");
    i18n::BindLabel(s->detail, "This image does not contain the expected browser payload.");
    return;
  }
  s->started = true;
  lv_obj_add_state(s->prepare, LV_STATE_DISABLED);
  lv_obj_remove_flag(s->progress, LV_OBJ_FLAG_HIDDEN);
  i18n::BindLabel(s->title, "Preparing WebKit");
  i18n::BindLabel(s->detail,
      "Verifying and expanding the engine in RAM. Preparation continues if you leave this page.");
  s->worker = std::thread([s] {
    operation_affinity::RestoreForWorker();
    web::PrepareRuntime(s->state);
  });
}
}  // namespace

bool BrowserHandlePointer(int slot, int x, int y, bool pressed) {
  auto *s = gWebScene;
  if (!s || slot < 0 || slot > 1 || !s->showing_web ||
      !s->session.Connected() || s->manager_open || s->settings_open ||
      (s->keyboard && !lv_obj_has_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN)))
    return false;
  // Quick Settings is a child overlay of the active browser screen. It must
  // exclusively own every new tap/drag until its closing animation is gone.
  if (StatusBarShadeOpen()) {
    ResetBrowserTouches(s);
    return false;
  }
  const bool inside = x >= s->view_left && y >= s->view_top &&
      x < s->view_left + s->view_width &&
      y < s->view_top + s->view_height;
  const bool was_pressed = s->touch_pressed[slot];
  // Once another UI surface owns a contact (status shade, edge gesture,
  // navigation, etc.), Browser must not capture that same finger when it
  // later crosses into the web viewport. Release is the ownership boundary.
  if (s->touch_ignored[slot]) {
    if (!pressed) s->touch_ignored[slot] = false;
    return false;
  }
  if (pressed && !was_pressed) {
    const int screen_width = lv_obj_get_width(s->screen);
    const int edge = std::max(72, screen_width / 20);
    if (!inside || (slot == 0 && (x <= edge || x >= screen_width - edge))) {
      s->touch_ignored[slot] = true;
      return false;
    }
    s->touch_pressed[slot] = true;
    if (slot == 0) {
      const int browser_x = std::clamp(
          (x - s->view_left) * web::kViewWidth / s->view_width,
          0, web::kViewWidth - 1);
      const int browser_y = std::clamp(
          (y - s->view_top) * web::kViewHeight / s->view_height,
          0, web::kViewHeight - 1);
      s->session.Send(web::Kind::kTouchDown, browser_x, browser_y, 0);
    }
  } else if (!s->touch_pressed[slot]) {
    return false;
  }

  static int touch_x[2]{};
  static int touch_y[2]{};
  touch_x[slot] = x;
  touch_y[slot] = y;

  if (!s->pinch_active && s->touch_pressed[0] && s->touch_pressed[1]) {
    const double dx = touch_x[1] - touch_x[0];
    const double dy = touch_y[1] - touch_y[0];
    s->pinch_start_distance = std::max(24.0, std::hypot(dx, dy));
    s->pinch_start_zoom = s->session.ZoomPercent();
    s->pinch_active = true;
    const int browser_x = std::clamp(
        (touch_x[0] - s->view_left) * web::kViewWidth / s->view_width,
        0, web::kViewWidth - 1);
    const int browser_y = std::clamp(
        (touch_y[0] - s->view_top) * web::kViewHeight / s->view_height,
        0, web::kViewHeight - 1);
    s->session.Send(web::Kind::kTouchUp, browser_x, browser_y, 0);
  }

  if (s->pinch_active) {
    if (pressed && s->touch_pressed[0] && s->touch_pressed[1]) {
      const double dx = touch_x[1] - touch_x[0];
      const double dy = touch_y[1] - touch_y[0];
      const unsigned zoom = static_cast<unsigned>(std::clamp(
          std::lround(s->pinch_start_zoom *
                      std::hypot(dx, dy) / s->pinch_start_distance),
          50L, 300L));
      if (zoom != s->session.ZoomPercent()) s->session.SetZoom(zoom);
    }
    if (!pressed) s->touch_pressed[slot] = false;
    if (!s->touch_pressed[0] && !s->touch_pressed[1]) s->pinch_active = false;
    return true;
  }

  if (slot == 0) {
    const int browser_x = std::clamp(
        (x - s->view_left) * web::kViewWidth / s->view_width,
        0, web::kViewWidth - 1);
    const int browser_y = std::clamp(
        (y - s->view_top) * web::kViewHeight / s->view_height,
        0, web::kViewHeight - 1);
    s->session.Send(pressed ? web::Kind::kTouchMove : web::Kind::kTouchUp,
                    browser_x, browser_y, 0);
  }
  if (!pressed) s->touch_pressed[slot] = false;
  return true;
}

void ShutdownWebRuntime() {
  auto *s = gPersistentWebScene;
  if (!s) return;
  gPersistentWebScene = nullptr;
  s->persistent = false;
  if (s->timer) {
    lv_timer_delete(s->timer);
    s->timer = nullptr;
  }
  s->state.cancel.store(true);
  if (s->worker.joinable()) s->worker.join();
  if (s->session.Connected()) s->session.Send(web::Kind::kClose);
  s->process.Stop();
  s->session.Close();
  web::RemoveRuntime(s->state.directory);
  s->state.directory.clear();
  if (!s->screen) delete s;
}

void BuildWebScene(lv_obj_t *screen, ActionCallback callback, void *context,
                   int frame_fd, int control_fd, bool auto_launch) {
  auto *s = auto_launch ? gPersistentWebScene : nullptr;
  if (!s) {
    s = new WebScene;
    s->persistent = auto_launch;
    if (auto_launch) gPersistentWebScene = s;
  }
  // Recreate only the LVGL presentation. The verified runtime, WebKit worker,
  // navigation state and downloads deliberately survive between scenes.
  gWebScene = s;
  s->auto_launch = auto_launch;
  s->screen = screen;
  s->display = lv_obj_get_display(screen);
  s->view_left = 24;
  s->view_top = 460;
  s->view_width = kBrowserViewportWidth;
  s->view_height = kBrowserViewportHeight;
  s->showing_web = false;
  s->web_keyboard = false;
  s->manager_open = false;
  s->frame_waiting_for_refresh = false;
  s->last_status.clear();
  s->last_progress = 101;
  s->last_download_revision = UINT32_MAX;
  s->last_settings_revision = UINT32_MAX;
  ResetBrowserTouches(s);
  lv_display_add_event_cb(
      s->display, BrowserRefreshReady, LV_EVENT_REFR_READY, s);
  if (frame_fd >= 0 || control_fd >= 0) s->session.Adopt(frame_fd, control_fd);
  lv_obj_add_event_cb(screen, [](lv_event_t *e) {
    auto *s = static_cast<WebScene *>(lv_event_get_user_data(e));
    DetachWebScene(s);
    if (!s->persistent) delete s;
  }, LV_EVENT_DELETE, s);
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  const bool landscape = lv_obj_get_width(screen) > lv_obj_get_height(screen);
  if (landscape) {
    s->view_top = StatusBarHeight();
    s->view_height = lv_obj_get_height(screen) - s->view_top;
    s->view_width = s->view_height * kBrowserViewportWidth /
                    kBrowserViewportHeight;
    s->view_left = 48;
  } else {
    s->view_height = lv_obj_get_height(screen) - s->view_top;
  }
  s->web_progress = lv_bar_create(screen);
  lv_obj_set_pos(s->web_progress, landscape ? 760 : 24, 316);
  lv_obj_set_size(s->web_progress,
                  landscape ? lv_obj_get_width(screen) - 824 : 1392, 6);
  lv_obj_set_style_bg_color(s->web_progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(s->web_progress, LV_OBJ_FLAG_HIDDEN);
  s->address = TextArea(screen);
  text_edit::SetSelectAllOnHold(s->address, true);
  // LVGL's one-line setter derives a compact height. Apply it before the
  // explicit browser-bar geometry so the field and Go button stay aligned.
  lv_textarea_set_one_line(s->address, true);
  lv_obj_set_pos(s->address, landscape ? 760 : 24, 188);
  lv_obj_set_size(s->address, landscape ? 2020 : 1168, 112);
  lv_textarea_set_max_length(s->address, 2040);
  lv_textarea_set_placeholder_text(s->address, "Enter website address");
  lv_obj_set_style_text_font(s->address, UiFont(&lv_font_montserrat_36), 0);
  lv_obj_set_style_text_color(s->address, kText, 0);
  lv_obj_set_style_bg_color(s->address, kMainPanel, 0);
  lv_obj_set_style_bg_opa(s->address, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s->address, kMainLine, 0);
  lv_obj_set_style_border_width(s->address, 1, 0);
  lv_obj_set_style_radius(s->address, 56, 0);
  lv_obj_set_style_pad_all(s->address, 26, 0);
  lv_obj_set_style_pad_top(s->address, 34, 0);
  lv_obj_set_style_pad_bottom(s->address, 18, 0);
  auto *go = Button(screen, "Go", [s] { OpenAddressBar(s); }, true);
  lv_obj_set_pos(go, landscape ? 2800 : 1210, 188);
  lv_obj_set_size(go, landscape ? 304 : 206, 112);
  lv_obj_set_style_radius(go, 56, 0);
  const char *controls[] = {LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT,
                            LV_SYMBOL_REFRESH, LV_SYMBOL_HOME,
                            LV_SYMBOL_DOWNLOAD, LV_SYMBOL_SETTINGS};
  for (int i = 0; i < 6; ++i) {
    auto *button = Button(screen, controls[i], [s, i] {
      if (!s->session.Connected()) return;
      if (i == 3) {
        const auto homepage = web::Address(RecoveryBrowserHomepage());
        if (!homepage.empty())
          s->session.Send(web::Kind::kOpen, 0, 0, 0, homepage.c_str());
      } else if (i == 4) {
        SetDownloadManager(s, !s->manager_open);
      } else if (i == 5) {
        ShowBrowserSettings(s);
      } else {
        const web::Kind kinds[] = {web::Kind::kBack, web::Kind::kForward,
                                   web::Kind::kReload};
        s->session.Send(kinds[i]);
      }
    });
    s->controls[i] = button;
    lv_obj_set_pos(button, (landscape ? 760 : 24) +
                     i * (landscape ? 394 : 236), 334);
    lv_obj_set_size(button, landscape ? 370 : 212, 106);
    lv_obj_set_style_radius(button, 53, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(button, 0),
                               UiFont(&lv_font_montserrat_48), 0);
    // Do not present navigation as functional before the isolated session exists.
    lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
  }
  auto *area = lv_obj_create(screen);
  s->area = area;
  Panel(area, 36, kMainPanel);
  lv_obj_set_pos(area, s->view_left, s->view_top);
  lv_obj_set_size(area, s->view_width, s->view_height);
  lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);
  auto *icon = Label(area, LV_SYMBOL_GPS, &lv_font_montserrat_48, kAccent);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 160);
  s->title = Label(area, "Web engine installed", &lv_font_montserrat_48, kText);
  lv_obj_set_size(s->title, landscape ? s->view_width - 70 : 1140,
                  LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(s->title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s->title, LV_ALIGN_TOP_MID, 0, landscape ? 230 : 288);
  s->detail = Label(area, web::LaunchBlockReason().c_str(), &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(s->detail, landscape ? s->view_width - 70 : 1090);
  lv_obj_set_style_text_line_space(s->detail, 16, 0);
  lv_obj_set_style_text_align(s->detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s->detail, LV_ALIGN_TOP_MID, 0, landscape ? 360 : 418);
  s->progress = lv_bar_create(area);
  lv_obj_set_size(s->progress, landscape ? s->view_width - 90 : 1030, 12);
  lv_obj_align(s->progress, LV_ALIGN_TOP_MID, 0, landscape ? 620 : 870);
  lv_obj_set_style_bg_color(s->progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(s->progress, LV_OBJ_FLAG_HIDDEN);
  s->prepare = Button(area, "Verify and prepare engine", [s] { Prepare(s); }, true);
  lv_obj_set_size(s->prepare, landscape ? s->view_width - 70 : 1050,
                  landscape ? 118 : 144);
  lv_obj_set_style_opa(s->prepare, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_align(s->prepare, LV_ALIGN_TOP_MID, 0, landscape ? 680 : 960);
  auto *note = Label(area, "Wi-Fi setup is separate.\n\nNo browser runs during boot.\nAfter its first launch, Browser stays ready in RAM.\nDownloads continue when you leave this page.\n\nDownloads are saved to AERA/Downloads.",
                     &lv_font_montserrat_32, kMuted);
  lv_obj_set_width(note, landscape ? s->view_width - 70 : 1090);
  lv_obj_set_style_text_line_space(note, 14, 0);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(note, LV_ALIGN_TOP_MID, 0, landscape ? 850 : 1240);
  if (!web::RuntimeInstalled()) {
    i18n::BindLabel(s->title, "Browser package unavailable");
    lv_obj_add_state(s->prepare, LV_STATE_DISABLED);
  }
  s->viewport = lv_obj_create(screen);
  Clear(s->viewport);
  lv_obj_set_pos(s->viewport, s->view_left, s->view_top);
  lv_obj_set_size(s->viewport, s->view_width, s->view_height);
  lv_obj_add_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
  s->image = lv_image_create(s->viewport);
  lv_image_set_pivot(s->image, 0, 0);
  lv_image_set_scale(s->image, landscape
      ? (s->view_width * 256 + web::kWidth / 2) / web::kWidth
      : kBrowserImageScale);
  lv_obj_remove_flag(s->image, LV_OBJ_FLAG_CLICKABLE);
  s->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  s->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  // WebKit reuses this descriptor while replacing its pixel contents. Keep
  // every frame on the OpenGL renderer, but never reuse a stale GPU texture.
  // USER1 selects the OpenGL renderer's persistent streaming-texture path.
  s->descriptor.header.flags = LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;
  s->descriptor.header.w = web::kWidth; s->descriptor.header.h = web::kHeight;
  s->descriptor.header.stride = web::kWidth * 4;
  s->descriptor.data_size = web::kFrameBytes;
  lv_obj_add_event_cb(s->viewport, [](lv_event_t *e) {
    const auto code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    // Screen DELETE frees the scene before its children are deleted. Ignore
    // child lifecycle events before dereferencing their scene user-data.
    auto *s = static_cast<WebScene *>(lv_event_get_user_data(e));
    if (!s->session.Connected()) return;
    auto *input = lv_indev_active();
    if (!input) return;
    lv_point_t point{}; lv_indev_get_point(input, &point);
    lv_area_t bounds{}; lv_obj_get_coords(s->viewport, &bounds);
    const int x = std::clamp<int>((point.x - bounds.x1) * web::kViewWidth /
        s->view_width, 0, web::kViewWidth - 1);
    const int y = std::clamp<int>((point.y - bounds.y1) * web::kViewHeight /
        s->view_height, 0, web::kViewHeight - 1);
    const auto kind = code == LV_EVENT_PRESSED ? web::Kind::kTouchDown :
        code == LV_EVENT_PRESSING ? web::Kind::kTouchMove : web::Kind::kTouchUp;
    s->session.Send(kind, x, y);
  }, LV_EVENT_ALL, s);
  lv_obj_add_event_cb(s->viewport, [](lv_event_t *event) {
    auto *s = static_cast<WebScene *>(lv_event_get_user_data(event));
    if (s->session.Connected() && s->session.CanBack())
      s->session.Send(web::Kind::kBack);
  }, LV_EVENT_CANCEL, s);
  s->navigation = Navigation(screen, Action::kSettings, callback, context, true);
  s->keyboard = lv_keyboard_create(screen);
  lv_obj_set_user_data(s->keyboard, &kPersistentModalMarker);
  lv_obj_add_event_cb(s->keyboard, [](lv_event_t *event) {
    HideKeyboard(static_cast<WebScene *>(lv_event_get_user_data(event)));
  }, LV_EVENT_CANCEL, s);
  phone_keyboard::Apply(s->keyboard);
  lv_obj_set_size(s->keyboard, landscape ? 2408 : 1440,
                  landscape ? 900 : 950);
  if (landscape) lv_obj_align(s->keyboard, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  else lv_obj_align(s->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s->address, [](lv_event_t *e) {
    auto *s = static_cast<WebScene *>(lv_event_get_user_data(e));
    ShowKeyboard(s, false);
  }, LV_EVENT_CLICKED, s);
  lv_obj_add_event_cb(s->keyboard, [](lv_event_t *e) {
    auto *s = static_cast<WebScene *>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_READY && !s->web_keyboard)
      OpenAddressBar(s);
    else if (lv_event_get_code(e) == LV_EVENT_READY ||
             lv_event_get_code(e) == LV_EVENT_CANCEL)
      HideKeyboard(s);
    else if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED && s->web_keyboard && s->session.Connected()) {
      const char *key = phone_keyboard::ActivatedKey(s->keyboard);
      if (!key) return;
      if (!strcmp(key, LV_SYMBOL_BACKSPACE)) s->session.Send(web::Kind::kKey, 0, 0, 8);
      else if (!strcmp(key, LV_SYMBOL_NEW_LINE)) s->session.Send(web::Kind::kKey, 0, 0, 13);
      else if (const uint32_t codepoint = phone_keyboard::KeyCodepoint(key);
               codepoint >= 32)
        s->session.Send(web::Kind::kKey, 0, 0, codepoint);
      // Mode selectors are keyboard UI, never injected as strings into a page.
    }
  }, LV_EVENT_ALL, s);
  if (!s->timer) s->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *s = static_cast<WebScene *>(lv_timer_get_user_data(timer));
    const bool visible = s->screen != nullptr;
    if (!s->process.Running() && s->session.Connected())
      s->session.Close();
    lv_timer_set_period(timer, s->session.Connected() ? 16 : 50);
    if (s->session.Connected()) {
      const bool new_frame = s->session.Poll();
      uint32_t keyboard_purpose = 0;
      const auto keyboard_request = s->session.TakeKeyboardRequest(&keyboard_purpose);
      if (!visible) {
        // WebKit uses a two-buffer handshake. Consume and acknowledge hidden
        // frames so its main loop and network downloads never stall merely
        // because the Browser scene is not currently on screen.
        if (new_frame) s->session.AcknowledgeFrame();
      } else if (keyboard_request == web::KeyboardRequest::kShow) {
        ShowKeyboard(s, true, keyboard_purpose);
      } else if (keyboard_request == web::KeyboardRequest::kHide &&
                 s->web_keyboard) {
        HideKeyboard(s);
      }
      if (visible && new_frame) {
        s->descriptor.data = s->session.Pixels();
        lv_image_cache_drop(&s->descriptor);
        lv_image_set_src(s->image, &s->descriptor);
        s->frame_waiting_for_refresh = true;
        lv_obj_invalidate(s->image);
        lv_obj_add_flag(s->area, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
        s->showing_web = true;
      }
      for (int i = 0; visible && i < 6; ++i) {
        const bool enabled = s->session.Connected() &&
            (i == 0 ? s->session.CanBack() : i == 1 ? s->session.CanForward() : true);
        if (enabled) lv_obj_remove_state(s->controls[i], LV_STATE_DISABLED);
        else lv_obj_add_state(s->controls[i], LV_STATE_DISABLED);
      }
      if (visible && s->viewport) {
        lv_obj_set_user_data(s->viewport,
                             s->session.CanBack()
                                 ? &kPersistentModalMarker : nullptr);
      }
      if (s->last_download_revision != s->session.DownloadRevision()) {
        if (visible) {
          for (const auto &download : s->session.Downloads()) {
            if (s->announced_downloads.insert(download.id).second) {
              ShowDownloadStarted(s, download);
              break;
            }
          }
          if (s->manager_open) RefreshDownloads(s);
        } else {
          for (const auto &download : s->session.Downloads())
            s->announced_downloads.insert(download.id);
        }
        s->last_download_revision = s->session.DownloadRevision();
      }
      if (s->last_settings_revision != s->session.SettingsRevision()) {
        s->last_settings_revision = s->session.SettingsRevision();
        if (visible && !s->session.SettingsNotice().empty()) {
          if (s->settings_status)
            lv_label_set_text(s->settings_status,
                              s->session.SettingsNotice().c_str());
          else
            Sheet(s->screen, "Browser privacy",
                  s->session.SettingsNotice());
        }
      }
      if (visible && s->last_status != s->session.Status()) {
        s->last_status = s->session.Status();
        i18n::BindLabel(s->detail, s->last_status.c_str());
        // The editable field is the only address display. Track redirects,
        // history navigation and same-document URL changes unless the user is
        // actively replacing its contents.
        const auto current_address = web::Address(s->last_status);
        if (!current_address.empty() &&
            !lv_obj_has_state(s->address, LV_STATE_FOCUSED))
          lv_textarea_set_text(s->address, current_address.c_str());
      }
      if (visible && s->session.Progress() != s->last_progress) {
        s->last_progress = s->session.Progress();
        lv_bar_set_value(s->web_progress, s->last_progress, LV_ANIM_ON);
        if (s->last_progress < 100 && s->session.Connected())
          lv_obj_remove_flag(s->web_progress, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s->web_progress, LV_OBJ_FLAG_HIDDEN);
      }
    } else if (visible && s->showing_web) {
      HideKeyboard(s);
      lv_obj_add_flag(s->web_progress, LV_OBJ_FLAG_HIDDEN);
      s->showing_web = false;
      lv_obj_add_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s->area, LV_OBJ_FLAG_HIDDEN);
      i18n::BindLabel(s->title, "Browser stopped");
      i18n::BindLabel(s->detail,
                        "The persistent WebKit session closed unexpectedly.");
    }
    if (!s->started || s->finished) return;
    if (visible)
      lv_bar_set_value(s->progress, s->state.progress.load(), LV_ANIM_ON);
    if (!s->state.done.load(std::memory_order_acquire)) return;
    s->finished = true;
    if (s->worker.joinable()) s->worker.join();
    if (!s->state.verified) {
      if (visible) {
        i18n::BindLabel(s->title, "Preparation failed");
        i18n::BindLabel(s->detail, s->state.error.c_str());
      }
      return;
    }
    if (!s->auto_launch) {
      if (visible) {
        i18n::BindLabel(s->title, "Runtime verified");
        i18n::BindLabel(s->detail, web::LaunchBlockReason().c_str());
      }
      return;
    }
    int frame = -1, control = -1;
    std::string error;
    if (!s->process.Start(s->state.directory, frame, control, error) ||
        !s->session.Adopt(frame, control)) {
      s->process.Stop();
      if (visible) {
        i18n::BindLabel(s->title, "Browser start failed");
        i18n::BindLabel(s->detail,
            error.empty() ? s->session.Status().c_str() : error.c_str());
      }
      return;
    }
    const int zoom = RecoveryBrowserZoom();
    s->session.SetZoom(zoom);
    if (SupportsPersistentBrowserSettings(s))
      s->session.Send(web::Kind::kSetCookiePolicy, 0, 0,
                      static_cast<uint32_t>(RecoveryBrowserCookiePolicy()));
    const auto homepage = web::Address(RecoveryBrowserHomepage());
    if (!homepage.empty())
      s->session.Send(web::Kind::kOpen, 0, 0, 0, homepage.c_str());
    if (visible) {
      i18n::BindLabel(s->title, "Starting private browser");
      i18n::BindLabel(s->detail, "Loading the built-in start page...");
    }
  }, 8, s);

  if (s->started && !s->finished) {
    lv_obj_add_state(s->prepare, LV_STATE_DISABLED);
    lv_obj_remove_flag(s->progress, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s->progress, s->state.progress.load(), LV_ANIM_OFF);
    i18n::BindLabel(s->title, "Preparing WebKit");
    i18n::BindLabel(s->detail,
        "Verifying and expanding the persistent engine in RAM...");
  } else if (s->finished && !s->state.verified) {
    i18n::BindLabel(s->title, "Preparation failed");
    i18n::BindLabel(s->detail, s->state.error.c_str());
  }
  if (s->session.Connected() && s->session.HasFrame()) {
    s->descriptor.data = s->session.Pixels();
    lv_image_set_src(s->image, &s->descriptor);
    lv_obj_add_flag(s->area, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s->viewport, LV_OBJ_FLAG_HIDDEN);
    s->showing_web = true;
    const auto current_address = web::Address(s->session.Status());
    if (!current_address.empty())
      lv_textarea_set_text(s->address, current_address.c_str());
  }

  // Opening Browser is the user's explicit request to use the engine. Begin
  // verification immediately instead of depending on a second tap in the
  // large content panel; manual mode remains available to the host UI tests.
  if (s->auto_launch) Prepare(s);
}
}  // namespace aeraui
