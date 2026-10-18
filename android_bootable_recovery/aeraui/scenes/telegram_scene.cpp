// SPDX-License-Identifier: Apache-2.0
#include "scene.hpp"
#include "browser/runtime.hpp"
#include "phone_keyboard.hpp"
#include "picture_decode.hpp"
#include "telegram/launcher.hpp"
#include "telegram/protocol.hpp"
#include "ui_components.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <map>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

// The encrypted Telegram database remains persistent, while its unlock secret
// lives only for the current recovery process. Reopening the app in the same
// boot can therefore restore the TDLib session without persisting a password.
std::string g_boot_vault_password;

void SecureClear(std::string &value) {
  std::fill(value.begin(), value.end(), '\0');
  value.clear();
  value.shrink_to_fit();
}

struct AvatarImage {
  std::shared_ptr<PictureData> data;
  lv_image_dsc_t descriptor{};
};

struct PhotoPreviewView {
  lv_obj_t *container = nullptr;
  std::string runtime_path;
  bool loaded = false;
  uint32_t retry_after = 0;
};

struct PhotoMessageContext {
  std::string sender;
  std::string avatar_path;
  std::string caption;
  bool outgoing = false;
};

struct AvatarView {
  lv_obj_t *plate = nullptr;
  std::string name;
  int size = 0;
};

struct MessageStatusView {
  lv_obj_t *label = nullptr;
};

struct TelegramScene {
  lv_obj_t *screen = nullptr;
  lv_obj_t *surface = nullptr;
  lv_obj_t *title = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *navigation = nullptr;
  lv_obj_t *keyboard = nullptr;
  lv_obj_t *composer = nullptr;
  lv_obj_t *send_button = nullptr;
  lv_obj_t *attach_button = nullptr;
  lv_timer_t *timer = nullptr;
  telegram::Process process;
  web::Preparation preparation;
  std::thread worker;
  int control = -1;
  telegram::AuthState auth = telegram::AuthState::kStarting;
  int64_t chat_id = 0;
  int item_y = 0;
  bool launched = false;
  bool existing_vault = false;
  bool keyboard_visible = false;
  bool history_loading = false;
  bool using_cached_vault = false;
  bool landscape = false;
  std::string pending_vault_password;
  std::vector<std::unique_ptr<AvatarImage>> avatars;
  std::map<std::string, std::vector<AvatarView>> avatar_views;
  std::map<int32_t, PhotoPreviewView> photo_previews;
  std::map<int64_t, PhotoMessageContext> photo_message_contexts;
  std::map<int64_t, uint32_t> message_statuses;
  std::map<int64_t, std::vector<MessageStatusView>> message_status_views;
  lv_obj_t *photo_viewer_preview = nullptr;
  int32_t photo_viewer_file_id = 0;
  int64_t reply_to_message_id = 0;
  int64_t editing_message_id = 0;
  lv_obj_t *composer_context = nullptr;
  lv_obj_t *composer_context_label = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;

  ~TelegramScene() {
    if (timer) lv_timer_delete(timer);
    preparation.cancel.store(true);
    if (worker.joinable()) worker.join();
    Send(telegram::Kind::kClose);
    if (control >= 0) close(control);
    process.Stop();
    web::RemoveRuntime(preparation.directory);
    SecureClear(pending_vault_password);
  }

  bool Send(telegram::Kind kind, const std::string &text = {},
            int64_t primary = 0, int64_t secondary = 0) {
    if (control < 0 || text.size() >= telegram::kTextBytes) return false;
    telegram::Message message;
    message.kind = kind;
    message.primary = primary;
    message.secondary = secondary;
    memcpy(message.text, text.data(), text.size());
    const ssize_t count = send(control, &message, sizeof(message),
                               MSG_DONTWAIT | MSG_NOSIGNAL);
    return count == static_cast<ssize_t>(sizeof(message));
  }
};

void ClearComposerContext(TelegramScene *scene);

lv_obj_t *Input(lv_obj_t *parent, int y, const char *placeholder,
                bool password = false) {
  auto *input = TextArea(parent);
  lv_obj_set_align(input, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(input, 42, y);
  lv_textarea_set_one_line(input, true);
  // set_one_line() selects a content-height widget in LVGL. Apply the intended
  // touch height afterwards so the field cannot collapse into a thin line.
  lv_obj_set_size(input, 1228, 132);
  lv_textarea_set_placeholder_text(input, placeholder);
  lv_textarea_set_password_mode(input, password);
  lv_textarea_set_password_show_time(input, 0);
  lv_textarea_set_max_length(input, 512);
  lv_obj_set_style_text_font(input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_text_color(input, kMutedStrong,
                              LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(input, kMainBottom, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, kLineBright, 0);
  lv_obj_set_style_border_color(input, kCyan, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(input, 2, 0);
  lv_obj_set_style_border_width(input, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(input, 28, 0);
  lv_obj_set_style_pad_all(input, 34, 0);
  return input;
}

void StyleKeyboard(lv_obj_t *keyboard) {
  phone_keyboard::Apply(keyboard);
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_size(keyboard, 1228, 850);
}

std::string StateImageHostPath(const std::string &runtime_path) {
  constexpr char state[] = "/state/";
  if (runtime_path.compare(0, sizeof(state) - 1, state) != 0 ||
      runtime_path.find("..") != std::string::npos ||
      runtime_path.find("//") != std::string::npos) return {};
  const std::string relative = runtime_path.substr(sizeof(state) - 1);
  if (relative.compare(0, 8, "avatars/") != 0 &&
      relative.compare(0, 9, "previews/") != 0 &&
      relative.compare(0, 6, "files/") != 0) return {};
  return "/data/recovery/AERA/telegram/" + relative;
}

std::string Initial(const std::string &name) {
  if (name.empty()) return "?";
  size_t bytes = 1;
  const unsigned char first = static_cast<unsigned char>(name[0]);
  if ((first & 0xe0) == 0xc0) bytes = 2;
  else if ((first & 0xf0) == 0xe0) bytes = 3;
  else if ((first & 0xf8) == 0xf0) bytes = 4;
  bytes = std::min(bytes, name.size());
  std::string result = name.substr(0, bytes);
  if (bytes == 1)
    result[0] = static_cast<char>(toupper(static_cast<unsigned char>(result[0])));
  return result;
}

bool RenderAvatar(TelegramScene *scene, lv_obj_t *plate,
                  const std::string &name, const std::string &runtime_path,
                  int size) {
  lv_obj_clean(plate);
  auto *fallback = Label(plate, Initial(name).c_str(), &lv_font_montserrat_32,
                         kCyan);
  lv_obj_center(fallback);

  const std::string path = StateImageHostPath(runtime_path);
  if (path.empty()) return false;
  auto avatar = std::make_unique<AvatarImage>();
  avatar->data = std::make_shared<PictureData>();
  const uint32_t decode_size = static_cast<uint32_t>(size) * 3U;
  DecodePictureThumbnail(path, decode_size, decode_size, *avatar->data);
  if (!avatar->data->pixels) return false;
  // LVGL does not reliably clip a transformed image to its widget radius.
  // Round the decoded alpha itself so real photos match the fallback tiles.
  const uint32_t radius =
      std::max(1u, std::min(avatar->data->width, avatar->data->height) / 4);
  for (uint32_t y = 0; y < radius; ++y) {
    for (uint32_t x = 0; x < radius; ++x) {
      const float dx = static_cast<float>(radius) - x - 0.5f;
      const float dy = static_cast<float>(radius) - y - 0.5f;
      const float distance = std::sqrt(dx * dx + dy * dy);
      const float coverage = std::clamp(
          static_cast<float>(radius) + 0.5f - distance, 0.0f, 1.0f);
      if (coverage >= 1.0f) continue;
      const uint8_t alpha = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
      const uint32_t right = avatar->data->width - 1 - x;
      const uint32_t bottom = avatar->data->height - 1 - y;
      avatar->data->pixels[(size_t(y) * avatar->data->width + x) * 4 + 3] = alpha;
      avatar->data->pixels[(size_t(y) * avatar->data->width + right) * 4 + 3] = alpha;
      avatar->data->pixels[(size_t(bottom) * avatar->data->width + x) * 4 + 3] = alpha;
      avatar->data->pixels[(size_t(bottom) * avatar->data->width + right) * 4 + 3] = alpha;
    }
  }
  auto &descriptor = avatar->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = avatar->data->width;
  descriptor.header.h = avatar->data->height;
  descriptor.header.stride = avatar->data->width * 4;
  descriptor.data_size = avatar->data->width * avatar->data->height * 4;
  descriptor.data = avatar->data->pixels;
  auto *image = lv_image_create(plate);
  lv_image_set_src(image, &descriptor);
  lv_image_set_antialias(image, true);
  lv_obj_set_style_radius(image, std::max(10, size / 4), 0);
  lv_obj_set_style_clip_corner(image, true, 0);
  const uint32_t scale = std::max(
      (static_cast<uint32_t>(size) * 256 + avatar->data->width - 1) /
          avatar->data->width,
      (static_cast<uint32_t>(size) * 256 + avatar->data->height - 1) /
          avatar->data->height);
  lv_image_set_scale(image, scale);
  lv_obj_center(image);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(fallback, LV_OBJ_FLAG_HIDDEN);
  scene->avatars.push_back(std::move(avatar));
  return true;
}

lv_obj_t *Avatar(TelegramScene *scene, lv_obj_t *parent,
                 const std::string &name, const std::string &runtime_path,
                 int size) {
  auto *plate = lv_obj_create(parent);
  Clear(plate);
  lv_obj_set_size(plate, size, size);
  lv_obj_set_style_radius(plate, std::max(10, size / 4), 0);
  lv_obj_set_style_bg_color(plate, kMainSelected, 0);
  lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(plate, 0, 0);
  lv_obj_set_style_pad_all(plate, 0, 0);
  lv_obj_set_style_clip_corner(plate, true, 0);
  lv_obj_remove_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
  RenderAvatar(scene, plate, name, runtime_path, size);
  if (!runtime_path.empty())
    scene->avatar_views[runtime_path].push_back({plate, name, size});
  return plate;
}

bool RenderPhotoPreview(TelegramScene *scene, lv_obj_t *container,
                        const std::string &runtime_path) {
  if (!container) return false;
  const std::string path = StateImageHostPath(runtime_path);
  if (path.empty()) return false;
  lv_obj_update_layout(container);
  const uint32_t available_width =
      std::max<int32_t>(1, lv_obj_get_width(container));
  const uint32_t available_height =
      std::max<int32_t>(1, lv_obj_get_height(container));
  auto image_data = std::make_unique<AvatarImage>();
  image_data->data = std::make_shared<PictureData>();
  DecodePictureThumbnail(path, available_width, available_height,
                         *image_data->data);
  if (!image_data->data->pixels || !image_data->data->width ||
      !image_data->data->height) return false;

  // Some recovery GPU stacks render dynamically-backed LVGL images as a
  // black rectangle when an image transform is active. Scale the decoded
  // pixels into their fitted display size once and hand LVGL a native-scale
  // buffer. This also makes the card and full viewer use the same path.
  const uint64_t width_scale =
      uint64_t(available_width) * 1000000 / image_data->data->width;
  const uint64_t height_scale =
      uint64_t(available_height) * 1000000 / image_data->data->height;
  const uint64_t fit_scale = std::min(width_scale, height_scale);
  const uint32_t fitted_width = std::max<uint32_t>(1,
      uint64_t(image_data->data->width) * fit_scale / 1000000);
  const uint32_t fitted_height = std::max<uint32_t>(1,
      uint64_t(image_data->data->height) * fit_scale / 1000000);
  if (fitted_width != image_data->data->width ||
      fitted_height != image_data->data->height) {
    const size_t fitted_bytes = size_t(fitted_width) * fitted_height * 4;
    auto *fitted = static_cast<uint8_t *>(malloc(fitted_bytes));
    if (!fitted) return false;
    const uint32_t source_width = image_data->data->width;
    const uint32_t source_height = image_data->data->height;
    for (uint32_t y = 0; y < fitted_height; ++y) {
      const uint32_t source_y = std::min(source_height - 1,
          static_cast<uint32_t>(uint64_t(y) * source_height / fitted_height));
      for (uint32_t x = 0; x < fitted_width; ++x) {
        const uint32_t source_x = std::min(source_width - 1,
            static_cast<uint32_t>(uint64_t(x) * source_width / fitted_width));
        memcpy(fitted + (size_t(y) * fitted_width + x) * 4,
               image_data->data->pixels +
                   (size_t(source_y) * source_width + source_x) * 4,
               4);
      }
    }
    free(image_data->data->pixels);
    image_data->data->pixels = fitted;
    image_data->data->width = fitted_width;
    image_data->data->height = fitted_height;
  }
  // Keep the placeholder intact until a complete, decodable preview exists.
  // Telegram downloads the stable file asynchronously and may notify the UI
  // just before or just after the attachment card is constructed.
  lv_obj_clean(container);
  auto &descriptor = image_data->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = image_data->data->width;
  descriptor.header.h = image_data->data->height;
  descriptor.header.stride = image_data->data->width * 4;
  descriptor.data_size = image_data->data->width * image_data->data->height * 4;
  descriptor.data = image_data->data->pixels;
  auto *image = lv_image_create(container);
  lv_image_set_src(image, &descriptor);
  lv_obj_center(image);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
  scene->avatars.push_back(std::move(image_data));
  return true;
}

void ConfirmDownload(TelegramScene *scene, int32_t file_id,
                     const std::string &name, uint64_t bytes,
                     lv_obj_t *viewer_status = nullptr) {
  const std::string detail = i18n::Format(
      "%s\n\nSize: %s\n\nSave this file to AERA/Telegram?",
      name.c_str(), Size(bytes).c_str());
  Sheet(scene->screen, "Download attachment?", detail.c_str(),
      [scene, file_id, name, viewer_status] {
        if (!scene->Send(telegram::Kind::kDownloadFile, name, file_id)) return;
        if (scene->detail) {
          const std::string status =
              i18n::Format("Downloading %s", name.c_str());
          i18n::BindLabel(scene->detail, status.c_str());
        }
        if (viewer_status && lv_obj_is_valid(viewer_status))
          i18n::BindLabel(viewer_status, "Downloading original photo...");
      });
}

void ShowPhotoViewer(TelegramScene *scene, int32_t file_id,
                     const std::string &name, uint64_t bytes) {
  const auto preview = scene->photo_previews.find(file_id);
  if (preview == scene->photo_previews.end()) return;
  auto *overlay = lv_obj_create(scene->screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_align(overlay, LV_ALIGN_TOP_LEFT);
  const int status_height = StatusBarHeight();
  const int screen_width = lv_obj_get_width(scene->screen);
  const int screen_height = lv_obj_get_height(scene->screen);
  lv_obj_set_pos(overlay, 0, status_height);
  lv_obj_set_size(overlay, screen_width, screen_height - status_height);
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

  auto *close = Button(overlay, LV_SYMBOL_CLOSE, [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_pos(close, 28, 24);
  lv_obj_set_size(close, 112, 112);
  CenterButtonContent(close);
  auto *title = Label(overlay, name.c_str(), &lv_font_montserrat_36, kText);
  lv_obj_set_pos(title, 170, 38);
  lv_obj_set_width(title, screen_width - 220);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

  auto *frame = lv_obj_create(overlay);
  Clear(frame);
  lv_obj_set_pos(frame, 28, 166);
  lv_obj_set_size(frame, screen_width - 56, screen_height - status_height - 410);
  lv_obj_set_style_radius(frame, 34, 0);
  lv_obj_set_style_bg_color(frame, kCanvas, 0);
  lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(frame, 1, 0);
  lv_obj_set_style_border_color(frame, kMainLine, 0);
  if (!RenderPhotoPreview(scene, frame, preview->second.runtime_path)) {
    auto *empty = Label(frame, LV_SYMBOL_IMAGE "  Preview unavailable",
                        &lv_font_montserrat_32, kMutedStrong);
    lv_obj_center(empty);
  }
  scene->photo_viewer_preview = frame;
  scene->photo_viewer_file_id = file_id;
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    auto *state = static_cast<TelegramScene *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
      state->photo_viewer_preview = nullptr;
      state->photo_viewer_file_id = 0;
    }
  }, LV_EVENT_DELETE, scene);

  auto *viewer_status = Label(overlay, "Tap Download to save the original",
                              &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(viewer_status, 40,
                 screen_height - status_height - 272);
  auto *download = Button(overlay, LV_SYMBOL_DOWNLOAD "  Download photo",
      [scene, file_id, name, bytes, viewer_status] {
        ConfirmDownload(scene, file_id, name, bytes, viewer_status);
      }, true);
  lv_obj_set_pos(download, 28, screen_height - status_height - 212);
  lv_obj_set_size(download, screen_width - 56, 132);
}

void ShowStatus(TelegramScene *scene, const char *title, const char *detail) {
  lv_obj_clean(scene->surface);
  scene->avatars.clear();
  scene->avatar_views.clear();
  scene->photo_previews.clear();
  scene->photo_message_contexts.clear();
  scene->message_statuses.clear();
  scene->message_status_views.clear();
  scene->title = Label(scene->surface, title, &lv_font_montserrat_48, kText);
  lv_obj_set_width(scene->title, 1180);
  lv_obj_set_style_text_align(scene->title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(scene->title, LV_ALIGN_TOP_MID, 0, 170);
  scene->detail = Label(scene->surface, detail, &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(scene->detail, 1080);
  lv_obj_set_style_text_align(scene->detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(scene->detail, 14, 0);
  lv_obj_align(scene->detail, LV_ALIGN_TOP_MID, 0, 280);
  scene->progress = lv_bar_create(scene->surface);
  lv_obj_set_size(scene->progress, 920, 16);
  lv_obj_align(scene->progress, LV_ALIGN_TOP_MID, 0, 470);
  lv_bar_set_value(scene->progress, scene->preparation.progress.load(), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(scene->progress, kCyan, LV_PART_INDICATOR);
}

void BindKeyboard(lv_obj_t *input, lv_obj_t *keyboard) {
  lv_obj_add_event_cb(input, [](lv_event_t *event) {
    auto *keyboard = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED)
      lv_keyboard_set_textarea(keyboard, lv_event_get_target_obj(event));
  }, LV_EVENT_FOCUSED, keyboard);
}

lv_obj_t *PasswordToggle(lv_obj_t *parent, lv_obj_t *input, int y) {
  auto *toggle = lv_button_create(parent);
  Clear(toggle);
  lv_obj_set_align(toggle, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(toggle, 1102, y);
  lv_obj_set_size(toggle, 168, 132);
  lv_obj_set_style_radius(toggle, 28, 0);
  lv_obj_set_style_bg_color(toggle, kMainBottom, 0);
  lv_obj_set_style_bg_color(toggle, kMainSelected, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(toggle, 2, 0);
  lv_obj_set_style_border_color(toggle, kLineBright, 0);
  auto *icon = Label(toggle, LV_SYMBOL_EYE_OPEN,
                     &lv_font_montserrat_32, kMutedStrong);
  lv_obj_center(icon);
  OnClick(toggle, [input, icon] {
    const bool hidden = lv_textarea_get_password_mode(input);
    lv_textarea_set_password_mode(input, !hidden);
    i18n::BindLabel(icon, hidden ? LV_SYMBOL_EYE_CLOSE
                                   : LV_SYMBOL_EYE_OPEN);
  });
  return toggle;
}

void ShowAuth(TelegramScene *scene, telegram::AuthState state,
              const std::string &detail) {
  scene->auth = state;
  lv_obj_clean(scene->surface);
  scene->avatars.clear();
  scene->avatar_views.clear();
  scene->photo_previews.clear();
  scene->photo_message_contexts.clear();
  scene->title = nullptr;
  scene->detail = nullptr;
  scene->progress = nullptr;
  scene->list = nullptr;
  const bool configure = state == telegram::AuthState::kNeedConfiguration;
  const bool vault = state == telegram::AuthState::kNeedVault;
  const bool new_vault = vault && !scene->existing_vault;
  const char *title = configure ? "Set up AERA Telegram" :
      new_vault ? "Create AERA Vault" :
      vault ? "Unlock AERA Telegram" :
      state == telegram::AuthState::kNeedPhone ? "Your phone number" :
      state == telegram::AuthState::kNeedCode ? "Telegram code" :
      state == telegram::AuthState::kNeedPassword ? "Two-step verification" :
      state == telegram::AuthState::kNeedEmail ? "Recovery email" :
      state == telegram::AuthState::kNeedEmailCode ? "Email code" :
      state == telegram::AuthState::kNeedRegistration ? "Create account" :
      state == telegram::AuthState::kConfirmElsewhere ? "Confirm sign-in" :
      "Connecting";
  auto *logo = lv_obj_create(scene->surface);
  Panel(logo, LV_RADIUS_CIRCLE, kAccentSoft);
  lv_obj_set_size(logo, 132, 132);
  lv_obj_set_style_border_width(logo, 3, 0);
  lv_obj_set_style_border_color(logo, kAccent, 0);
  lv_obj_set_style_border_opa(logo, LV_OPA_70, 0);
  auto *logo_mark = Label(logo, LV_SYMBOL_ENVELOPE,
                          &lv_font_montserrat_48, kAccent);
  lv_obj_center(logo_mark);
  if (scene->landscape) lv_obj_set_pos(logo, 662, 48);
  else lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 64);
  auto *badge = Kicker(scene->surface,
      configure ? "ONE-TIME CLIENT SETUP" :
      new_vault ? "FIRST-TIME SECURITY" :
      vault ? "ENCRYPTED LOCAL SESSION" : "TELEGRAM SIGN IN",
      configure ? kAccent : kCyan);
  if (scene->landscape) {
    lv_obj_set_width(badge, 1450);
    lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(badge, 44, 208);
  } else {
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 218);
  }
  scene->title = Label(scene->surface, title, &lv_font_montserrat_48, kText);
  lv_obj_set_width(scene->title, 1180);
  lv_obj_set_style_text_align(scene->title, LV_TEXT_ALIGN_CENTER, 0);
  if (scene->landscape) lv_obj_set_pos(scene->title, 180, 276);
  else lv_obj_align(scene->title, LV_ALIGN_TOP_MID, 0, 294);
  const std::string shown_detail = new_vault
      ? "Choose a password to protect Telegram data stored by this recovery."
      : vault
          ? "Enter the vault password you created on this device."
          : detail;
  scene->detail = Label(scene->surface, shown_detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_width(scene->detail, 1120);
  lv_obj_set_style_text_align(scene->detail, LV_TEXT_ALIGN_CENTER, 0);
  if (scene->landscape) lv_obj_set_pos(scene->detail, 220, 366);
  else lv_obj_align(scene->detail, LV_ALIGN_TOP_MID, 0, 382);

  if (state == telegram::AuthState::kConfirmElsewhere) {
    auto *note = Label(scene->surface,
        "Approve this login from an existing Telegram device, then wait here.",
        &lv_font_montserrat_32, kMutedStrong);
    lv_obj_set_pos(note, 92, 560); lv_obj_set_width(note, 1128);
    return;
  }
  if (state == telegram::AuthState::kStarting ||
      state == telegram::AuthState::kClosing) return;

  if (vault) {
    const int card_width = scene->landscape ? 1408 : 1312;
    const int card_height = new_vault ? 708 : 500;
    auto *vault_card = lv_obj_create(scene->surface);
    Panel(vault_card, 40, kMainSheet);
    lv_obj_set_pos(vault_card, scene->landscape ? 64 : 64,
                   scene->landscape ? 438 : 500);
    lv_obj_set_size(vault_card, card_width, card_height);
    lv_obj_set_style_border_width(vault_card, 1, 0);
    lv_obj_set_style_border_color(vault_card, kMainLine, 0);
    lv_obj_set_style_border_opa(vault_card, LV_OPA_40, 0);

    auto *hint = Label(vault_card,
        new_vault
            ? "This is not your Telegram account password. Use at least 8 characters and keep it safe."
            : "Encrypted locally. Your password never leaves this device.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(hint, 42, 32);
    lv_obj_set_width(hint, card_width - 84);

    auto *password_label = Label(vault_card, "Vault password",
                                 &lv_font_montserrat_24, kText);
    lv_obj_set_pos(password_label, 42, 100);
    auto *first = Input(vault_card, 142,
                        new_vault ? "Create a password" : "Enter your password",
                        true);
    lv_obj_set_width(first, 1044);
    PasswordToggle(vault_card, first, 142);

    lv_obj_t *confirmation = nullptr;
    if (new_vault) {
      auto *confirm_label = Label(vault_card, "Confirm password",
                                  &lv_font_montserrat_24, kText);
      lv_obj_set_pos(confirm_label, 42, 306);
      confirmation = Input(vault_card, 348, "Type it again", true);
      lv_obj_set_width(confirmation, 1044);
      PasswordToggle(vault_card, confirmation, 348);
    }

    auto *submit = Button(vault_card,
        new_vault ? "Create vault and continue" : "Unlock and continue",
        [scene, first, confirmation, new_vault] {
          const std::string value = lv_textarea_get_text(first);
          if (value.size() < 8) {
            i18n::BindLabel(scene->detail,
                              "Use at least 8 characters for the vault password.");
            return;
          }
          if (new_vault &&
              value != std::string(lv_textarea_get_text(confirmation))) {
            i18n::BindLabel(scene->detail,
                              "The two passwords do not match. Please try again.");
            return;
          }
          scene->pending_vault_password = value;
          scene->using_cached_vault = false;
          if (!scene->Send(telegram::Kind::kConfigure, value, 0)) {
            SecureClear(scene->pending_vault_password);
            i18n::BindLabel(scene->detail,
                              "Could not unlock the Telegram vault.");
            return;
          }
          lv_textarea_set_text(first, "");
          if (confirmation) lv_textarea_set_text(confirmation, "");
          ShowStatus(scene, "Opening encrypted session",
                     new_vault
                         ? "Creating your private AERA Telegram database."
                         : "Unlocking your local messages and account state.");
        }, true);
    lv_obj_set_pos(submit, 42, new_vault ? 536 : 326);
    lv_obj_set_size(submit, card_width - 84, 132);

    auto *keyboard = lv_keyboard_create(scene->surface);
    StyleKeyboard(keyboard);
    if (scene->landscape) {
      lv_obj_set_pos(keyboard, 1540,
                     (lv_obj_get_height(scene->surface) - 900) / 2);
      lv_obj_set_size(keyboard, 1564, 900);
    } else {
      constexpr int keyboard_height = 790;
      lv_obj_set_pos(keyboard, 0,
                     lv_obj_get_height(scene->surface) - keyboard_height);
      lv_obj_set_size(keyboard, lv_obj_get_width(scene->surface),
                      keyboard_height);
    }
    BindKeyboard(first, keyboard);
    if (confirmation) BindKeyboard(confirmation, keyboard);
    lv_keyboard_set_textarea(keyboard, first);
    lv_obj_add_state(first, LV_STATE_FOCUSED);
    AnimateEnter(vault_card, 0, 18);
    AnimateEnter(keyboard, 45, 22);
    return;
  }

  if (configure) {
    auto *explanation = lv_obj_create(scene->surface);
    Panel(explanation, 28, kMainSheet);
    lv_obj_set_pos(explanation, 42, 500);
    lv_obj_set_size(explanation, 1228, 150);
    auto *info = Label(explanation,
        "This is not your account login. Telegram requires every independent client to have its own API ID and hash from my.telegram.org. You enter these only once.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(info, 28, 22); lv_obj_set_width(info, 1172);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
  }

  const int first_y = configure ? 690 : 550;
  auto *first = Input(scene->surface, first_y,
      configure ? "Telegram API ID" :
      state == telegram::AuthState::kNeedPhone ? "+46 70 123 45 67" :
      state == telegram::AuthState::kNeedCode ? "Login code" :
      state == telegram::AuthState::kNeedPassword ? "Telegram password" :
      state == telegram::AuthState::kNeedEmail ? "Email address" :
      state == telegram::AuthState::kNeedEmailCode ? "Email code" : "First name",
      state == telegram::AuthState::kNeedPassword);
  lv_obj_t *second = nullptr;
  lv_obj_t *third = nullptr;
  if (configure) {
    lv_textarea_set_accepted_chars(first, "0123456789");
    lv_textarea_set_max_length(first, 10);
    second = Input(scene->surface, first_y + 148, "Telegram API hash");
    third = Input(scene->surface, first_y + 296, "AERA vault password", true);
    lv_textarea_set_max_length(second, 64);
    lv_textarea_set_max_length(third, 128);
  } else if (state == telegram::AuthState::kNeedRegistration) {
    second = Input(scene->surface, first_y + 148, "Last name");
  }

  auto *keyboard = lv_keyboard_create(scene->surface);
  StyleKeyboard(keyboard);
  if (scene->landscape) {
    lv_obj_set_pos(keyboard, 1600, 300);
    lv_obj_set_size(keyboard, 1500, 850);
  } else {
    constexpr int keyboard_height = 850;
    lv_obj_set_pos(keyboard, 0,
                   lv_obj_get_height(scene->surface) - keyboard_height);
    lv_obj_set_size(keyboard, lv_obj_get_width(scene->surface),
                    keyboard_height);
  }
  BindKeyboard(first, keyboard);
  if (second) BindKeyboard(second, keyboard);
  if (third) BindKeyboard(third, keyboard);
  lv_keyboard_set_textarea(keyboard, first);

  auto *submit = Button(scene->surface,
      configure ? "Save and continue" : "Continue",
      [scene, state, first, second, third] {
        std::string value = lv_textarea_get_text(first);
        telegram::Kind kind = telegram::Kind::kPhone;
        if (state == telegram::AuthState::kNeedConfiguration) {
          const long api_id = strtol(value.c_str(), nullptr, 10);
          const std::string hash = second ? lv_textarea_get_text(second) : "";
          const std::string vault = third ? lv_textarea_get_text(third) : "";
          if (api_id <= 0 || hash.size() < 16 || vault.size() < 8) {
            i18n::BindLabel(scene->detail,
                "API ID/hash are available at my.telegram.org. Use at least 8 characters for the vault password.");
            return;
          }
          scene->Send(telegram::Kind::kConfigure, hash + "\n" + vault, api_id);
          lv_textarea_set_text(third, "");
          ShowStatus(scene, "Opening encrypted session",
                     "TDLib is unlocking your local AERA Telegram database.");
          return;
        }
        if (value.empty()) return;
        if (state == telegram::AuthState::kNeedCode) kind = telegram::Kind::kCode;
        else if (state == telegram::AuthState::kNeedPassword) kind = telegram::Kind::kPassword;
        else if (state == telegram::AuthState::kNeedEmail) kind = telegram::Kind::kEmail;
        else if (state == telegram::AuthState::kNeedEmailCode) kind = telegram::Kind::kEmailCode;
        else if (state == telegram::AuthState::kNeedRegistration) {
          kind = telegram::Kind::kRegister;
          value += "\n" + std::string(second ? lv_textarea_get_text(second) : "");
        }
        scene->Send(kind, value);
        lv_textarea_set_text(first, "");
        ShowStatus(scene, "Checking with Telegram", "Keep this recovery connected to Wi-Fi.");
      }, true);
  lv_obj_set_pos(submit, 776, configure ? 1134 : 700);
  lv_obj_set_size(submit, 494, 118);
}

void ShowChats(TelegramScene *scene);

void SetChatKeyboard(TelegramScene *scene, bool visible) {
  if (!scene->keyboard || !scene->composer || !scene->list ||
      !scene->send_button || !scene->attach_button) return;
  scene->keyboard_visible = visible;
  const int composer_y = scene->landscape ? 170 : visible ? 1790 : 2780;
  lv_obj_set_y(scene->attach_button, composer_y);
  lv_obj_set_y(scene->composer, composer_y);
  lv_obj_set_y(scene->send_button, composer_y);
  lv_obj_set_height(scene->list, scene->landscape ? 1080 :
                    visible ? 1560 : 2520);
  if (visible) {
    lv_obj_remove_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(scene->keyboard);
  } else {
    lv_obj_add_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(scene->composer, LV_STATE_FOCUSED);
  }
  lv_obj_scroll_to_y(scene->list, scene->item_y, LV_ANIM_OFF);
}

struct AttachmentEntry {
  std::string name;
  std::string path;
  bool directory = false;
  uint64_t size = 0;
};

struct PickerThumbnail {
  std::string path;
  int y = 0;
  lv_obj_t *preview = nullptr;
  lv_obj_t *placeholder = nullptr;
  lv_obj_t *image = nullptr;
  std::shared_ptr<PictureData> data;
  lv_image_dsc_t descriptor{};
  int state = 0;
};

struct AttachmentPickerState {
  lv_obj_t *list = nullptr;
  lv_timer_t *timer = nullptr;
  std::vector<std::unique_ptr<PickerThumbnail>> thumbnails;
  int loading = -1;
};

bool PickerThumbnailVisible(const AttachmentPickerState *state,
                            const PickerThumbnail *thumbnail) {
  const int scroll = lv_obj_get_scroll_y(state->list);
  const int viewport = lv_obj_get_height(state->list);
  return thumbnail->y + 142 >= scroll - 142 &&
         thumbnail->y <= scroll + viewport + 142;
}

void ShowPickerThumbnail(PickerThumbnail *thumbnail) {
  if (!thumbnail->data || !thumbnail->data->pixels) {
    thumbnail->state = 3;
    thumbnail->data.reset();
    return;
  }
  auto &descriptor = thumbnail->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = thumbnail->data->width;
  descriptor.header.h = thumbnail->data->height;
  descriptor.header.stride = thumbnail->data->width * 4;
  descriptor.data_size = thumbnail->data->width * thumbnail->data->height * 4;
  descriptor.data = thumbnail->data->pixels;
  thumbnail->image = lv_image_create(thumbnail->preview);
  lv_image_set_src(thumbnail->image, &descriptor);
  lv_image_set_antialias(thumbnail->image, true);
  const uint32_t scale = std::max(
      (76U * 256 + thumbnail->data->width - 1) / thumbnail->data->width,
      (76U * 256 + thumbnail->data->height - 1) / thumbnail->data->height);
  lv_image_set_scale(thumbnail->image, scale);
  lv_obj_center(thumbnail->image);
  lv_obj_remove_flag(thumbnail->image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(thumbnail->placeholder, LV_OBJ_FLAG_HIDDEN);
  thumbnail->state = 2;
}

void AttachmentPickerTick(lv_timer_t *timer) {
  auto *state = static_cast<AttachmentPickerState *>(
      lv_timer_get_user_data(timer));
  if (!state || !state->list) return;
  if (state->loading >= 0) {
    auto *thumbnail = state->thumbnails[state->loading].get();
    if (thumbnail->data &&
        thumbnail->data->ready.load(std::memory_order_acquire)) {
      ShowPickerThumbnail(thumbnail);
      state->loading = -1;
    }
  }
  if (state->loading >= 0) return;
  for (size_t index = 0; index < state->thumbnails.size(); ++index) {
    auto *thumbnail = state->thumbnails[index].get();
    if (thumbnail->state != 0 ||
        !PickerThumbnailVisible(state, thumbnail)) continue;
    thumbnail->state = 1;
    thumbnail->data = std::make_shared<PictureData>();
    state->loading = static_cast<int>(index);
    std::thread([data = thumbnail->data, path = thumbnail->path] {
      DecodePictureThumbnail(path, 76, 76, *data);
    }).detach();
    break;
  }
}

void DeleteAttachmentPicker(lv_event_t *event) {
  auto *state = static_cast<AttachmentPickerState *>(
      lv_event_get_user_data(event));
  if (!state) return;
  if (state->timer) lv_timer_delete(state->timer);
  for (auto &thumbnail : state->thumbnails) {
    if (thumbnail->data) thumbnail->data->cancelled = true;
    if (thumbnail->image) {
      lv_obj_delete(thumbnail->image);
      thumbnail->image = nullptr;
      lv_image_cache_drop(&thumbnail->descriptor);
    }
  }
  delete state;
}

bool StoragePath(const std::string &path) {
  for (const char *root : {"/sdcard", "/mnt/nas", "/usb_otg",
                           "/external_sd"}) {
    const size_t length = strlen(root);
    if (path == root || (path.compare(0, length, root) == 0 &&
        path.size() > length && path[length] == '/')) return true;
  }
  return false;
}

std::string StorageRoot(const std::string &path) {
  for (const char *root : {"/sdcard", "/mnt/nas", "/usb_otg",
                           "/external_sd"}) {
    const size_t length = strlen(root);
    if (path == root || (path.compare(0, length, root) == 0 &&
        path.size() > length && path[length] == '/')) return root;
  }
  return {};
}

std::string ParentDirectory(const std::string &path) {
  const std::string root = StorageRoot(path);
  if (root.empty() || path == root) return {};
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos || slash < root.size()) return root;
  return path.substr(0, slash);
}

std::vector<AttachmentEntry> AttachmentRoots() {
  std::vector<AttachmentEntry> roots;
  auto add = [&roots](const std::string &name, const std::string &path) {
    if (!StoragePath(path)) return;
    if (std::any_of(roots.begin(), roots.end(), [&](const auto &entry) {
          return entry.path == path;
        })) return;
    roots.push_back({name, path, true, 0});
  };
  // Recovery exposes internal storage to applications as /sdcard even when
  // the partition backend reports its physical /data/media/0 path.
  add("Internal storage", "/sdcard");
  for (const auto &volume : RecoveryVolumes("storage")) {
    std::string path = volume.path;
    std::string name = volume.name;
    if (path == "/data/media/0" || path == "INTERNAL") {
      path = "/sdcard";
      name = "Internal storage";
    } else if (path == "/mnt/nas") name = "Network storage";
    else if (path == "/usb_otg") name = "USB OTG";
    else if (path == "/external_sd") name = "SD card";
    add(name, path);
  }
  struct stat external{};
  if (stat("/external_sd", &external) == 0 && S_ISDIR(external.st_mode))
    add("SD card", "/external_sd");
  return roots;
}

void ShowAttachmentPicker(TelegramScene *scene, const std::string &requested) {
  const bool root_view = requested.empty() || !StoragePath(requested) ||
      requested.find("/../") != std::string::npos;
  const std::string path = !root_view
      ? requested : std::string{};
  auto *overlay = lv_obj_create(scene->screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_align(overlay, LV_ALIGN_TOP_LEFT);
  const int status_height = StatusBarHeight();
  const int overlay_width = lv_obj_get_width(scene->screen);
  const int overlay_height = lv_obj_get_height(scene->screen) - status_height;
  lv_obj_set_pos(overlay, 0, status_height);
  lv_obj_set_size(overlay, overlay_width, overlay_height);
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

  auto *close = Button(overlay, LV_SYMBOL_LEFT, [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_pos(close, 24, 20);
  lv_obj_set_size(close, 120, 112);
  auto *title = Label(overlay, "Attach a file", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 174, 22);
  auto *where = Label(overlay, root_view ? "Available storage" : path.c_str(),
                      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(where, 174, 84);
  lv_obj_set_width(where, scene->landscape ? 2850 : 1190);
  lv_label_set_long_mode(where, LV_LABEL_LONG_DOT);

  auto *list = lv_obj_create(overlay);
  Clear(list);
  lv_obj_set_pos(list, 24, 160);
  lv_obj_set_size(list, overlay_width - 48, overlay_height - 184);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  auto *picker = new AttachmentPickerState;
  picker->list = list;
  lv_obj_add_event_cb(overlay, DeleteAttachmentPicker, LV_EVENT_DELETE, picker);

  std::vector<AttachmentEntry> entries = root_view
      ? AttachmentRoots() : std::vector<AttachmentEntry>{};
  if (!root_view)
    entries.push_back({"Up one level", ParentDirectory(path), true, 0});
  if (!root_view) {
    if (DIR *directory = opendir(path.c_str())) {
      while (auto *item = readdir(directory)) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        std::string child = path + "/" + item->d_name;
        if (child.size() >= telegram::kTextBytes) continue;
        struct stat info{};
        if (lstat(child.c_str(), &info) != 0 || S_ISLNK(info.st_mode)) continue;
        if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) continue;
        entries.push_back({item->d_name, std::move(child), S_ISDIR(info.st_mode),
                           static_cast<uint64_t>(std::max<off_t>(0, info.st_size))});
        if (entries.size() >= 300) break;
      }
      closedir(directory);
    }
  }
  const size_t parent_rows = root_view ? 0 : 1;
  std::sort(entries.begin() + parent_rows, entries.end(),
            [](const AttachmentEntry &left, const AttachmentEntry &right) {
    if (left.directory != right.directory) return left.directory > right.directory;
    return left.name < right.name;
  });

  int y = 0;
  for (size_t index = 0; index < entries.size(); ++index) {
    const auto &entry = entries[index];
    const bool up = !root_view && index == 0;
    auto *row = Button(list, "", [scene, overlay, entry] {
      if (entry.directory) {
        ShowAttachmentPicker(scene, entry.path);
        lv_obj_delete_async(overlay);
        return;
      }
      if (scene->Send(telegram::Kind::kSendFile, entry.path, scene->chat_id,
                      scene->reply_to_message_id)) {
        ClearComposerContext(scene);
        if (scene->detail) {
          const std::string status =
              i18n::Format("Uploading %s", entry.name.c_str());
          i18n::BindLabel(scene->detail, status.c_str());
        }
      }
      lv_obj_delete_async(overlay);
    });
    lv_obj_set_pos(row, 0, y);
    const int row_width = overlay_width - 48;
    lv_obj_set_size(row, row_width, 142);
    lv_obj_set_style_radius(row, up ? 26 : 0, 0);
    lv_obj_set_style_bg_color(row, up ? kMainPanel : kMainCanvas, 0);
    lv_obj_set_style_bg_opa(row, up ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (!entry.directory && IsPicture(entry.path)) {
      auto thumbnail = std::make_unique<PickerThumbnail>();
      thumbnail->path = entry.path;
      thumbnail->y = y;
      thumbnail->preview = lv_obj_create(row);
      Clear(thumbnail->preview);
      lv_obj_set_pos(thumbnail->preview, 22, 32);
      lv_obj_set_size(thumbnail->preview, 76, 76);
      lv_obj_set_style_radius(thumbnail->preview, 18, 0);
      lv_obj_set_style_clip_corner(thumbnail->preview, true, 0);
      lv_obj_set_style_bg_color(thumbnail->preview, kMainPanel, 0);
      lv_obj_set_style_bg_opa(thumbnail->preview, LV_OPA_COVER, 0);
      lv_obj_remove_flag(thumbnail->preview, LV_OBJ_FLAG_SCROLLABLE);
      thumbnail->placeholder = Label(thumbnail->preview, LV_SYMBOL_IMAGE,
                                     &lv_font_montserrat_28, kAccent);
      lv_obj_center(thumbnail->placeholder);
      picker->thumbnails.push_back(std::move(thumbnail));
    } else {
      auto *icon = IconPlate(row,
          up ? LV_SYMBOL_UP :
              entry.directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
          entry.directory ? kCyan : kAccent, kMainPanel, 76);
      lv_obj_set_pos(icon, 22, 32);
    }
    auto *name = Label(row, entry.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 126, 24);
    lv_obj_set_width(name, row_width - 280);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const std::string meta = up
        ? (path == StorageRoot(path) ? "Storage locations" :
           ParentDirectory(path))
        : entry.directory ? (root_view ? "Storage" : "Folder")
                          : Size(entry.size);
    auto *detail = Label(row, meta.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail, 126, 82);
    auto *arrow = Label(row, entry.directory ? LV_SYMBOL_RIGHT : LV_SYMBOL_PLUS,
                        &lv_font_montserrat_32,
                        entry.directory ? kMutedStrong : kCyan);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -34, 0);
    y += 142;
  }
  if (entries.empty()) {
    auto *empty = Label(list, root_view ? "No readable storage is mounted" :
                        "No attachable files in this folder",
                        &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 100);
  }
  if (!picker->thumbnails.empty())
    picker->timer = lv_timer_create(AttachmentPickerTick, 35, picker);
}

void ApplyMessageStatus(lv_obj_t *label, uint32_t status) {
  if (!label || !lv_obj_is_valid(label)) return;
  const char *text = status == 2 ? LV_SYMBOL_OK " " LV_SYMBOL_OK :
      status == 1 ? LV_SYMBOL_OK :
      status == 3 ? LV_SYMBOL_WARNING : "…";
  i18n::BindLabel(label, text);
  lv_obj_set_style_text_color(label,
      status == 2 ? kCyan : status == 3 ? kRed : kMutedStrong, 0);
}

lv_obj_t *MessageStatus(TelegramScene *scene, lv_obj_t *parent,
                        int64_t message_id) {
  auto *label = Label(parent, "…", &lv_font_montserrat_20, kMutedStrong);
  const auto status = scene->message_statuses.find(message_id);
  ApplyMessageStatus(label,
      status == scene->message_statuses.end() ? 0 : status->second);
  scene->message_status_views[message_id].push_back({label});
  return label;
}

void ClearComposerContext(TelegramScene *scene) {
  scene->reply_to_message_id = 0;
  scene->editing_message_id = 0;
  if (scene->composer_context)
    lv_obj_add_flag(scene->composer_context, LV_OBJ_FLAG_HIDDEN);
  if (scene->composer_context_label)
    i18n::BindLabel(scene->composer_context_label, "");
}

std::string ContextSnippet(std::string text) {
  std::replace(text.begin(), text.end(), '\n', ' ');
  if (text.size() > 72) text = text.substr(0, 69) + "...";
  return text;
}

void SetComposerContext(TelegramScene *scene, int64_t message_id,
                        const std::string &text, bool editing) {
  if (!scene->composer || !scene->composer_context ||
      !scene->composer_context_label) return;
  scene->reply_to_message_id = editing ? 0 : message_id;
  scene->editing_message_id = editing ? message_id : 0;
  const std::string detail = std::string(editing ? "Editing: " : "Replying to: ") +
      ContextSnippet(text);
  i18n::BindLabel(scene->composer_context_label, detail.c_str());
  lv_obj_remove_flag(scene->composer_context, LV_OBJ_FLAG_HIDDEN);
  if (editing) lv_textarea_set_text(scene->composer, text.c_str());
  SetChatKeyboard(scene, true);
}

void ShowMessageActions(TelegramScene *scene, int64_t message_id,
                        const std::string &text, bool outgoing,
                        bool editable) {
  auto *overlay = lv_obj_create(scene->screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event))
      lv_obj_delete_async(lv_event_get_current_target_obj(event));
  }, LV_EVENT_CLICKED, nullptr);

  const int width = scene->landscape ? 1500 : 1312;
  const int action_count = editable ? 3 : 2;
  auto *panel = lv_obj_create(overlay);
  Panel(panel, 42, kMainSheet);
  lv_obj_set_size(panel, width, 250 + action_count * 136);
  lv_obj_align(panel, scene->landscape ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID,
               0, scene->landscape ? 0 : -40);
  lv_obj_set_style_blur_backdrop(panel, true, 0);
  lv_obj_set_style_blur_radius(panel, 18, 0);
  auto *title = Label(panel, "Message actions", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 42, 34);
  auto *subtitle = Label(panel, ContextSnippet(text).c_str(),
                         &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, 42, 98);
  lv_obj_set_width(subtitle, width - 84);
  lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

  int y = 166;
  auto add = [&](const char *icon, const char *label, Handler action,
                 bool destructive = false) {
    auto *button = Button(panel, "", [overlay, action] {
      lv_obj_delete_async(overlay);
      action();
    });
    lv_obj_set_pos(button, 36, y);
    lv_obj_set_size(button, width - 72, 116);
    lv_obj_set_style_radius(button, 24, 0);
    lv_obj_set_style_bg_color(button, kMainPanel, 0);
    auto *symbol = Label(button, icon, &lv_font_montserrat_32,
                         destructive ? kRed : kAccent);
    lv_obj_align(symbol, LV_ALIGN_LEFT_MID, 28, 0);
    auto *copy = Label(button, label, &lv_font_montserrat_28,
                       destructive ? kRed : kText);
    lv_obj_align(copy, LV_ALIGN_LEFT_MID, 94, 0);
    y += 136;
  };
  add(LV_SYMBOL_NEW_LINE, "Reply", [scene, message_id, text] {
    SetComposerContext(scene, message_id, text, false);
  });
  if (editable)
    add(LV_SYMBOL_EDIT, "Edit message", [scene, message_id, text] {
      SetComposerContext(scene, message_id, text, true);
    });
  add(LV_SYMBOL_TRASH, outgoing ? "Delete message" : "Delete for me",
      [scene, message_id] {
        Sheet(scene->screen, "Delete message?",
              "Remove this message from the conversation?",
              [scene, message_id] {
                scene->Send(telegram::Kind::kDeleteMessage, {},
                            scene->chat_id, message_id);
              }, 900, false, SheetPresentation::kCompactGlass,
              "Swipe to delete");
      }, true);
}

void OpenChat(TelegramScene *scene, int64_t id, const std::string &title) {
  scene->chat_id = id;
  lv_obj_set_user_data(scene->surface, &kPersistentModalMarker);
  scene->history_loading = true;
  lv_obj_clean(scene->surface);
  scene->avatars.clear();
  scene->avatar_views.clear();
  scene->photo_previews.clear();
  scene->photo_message_contexts.clear();
  scene->message_statuses.clear();
  scene->message_status_views.clear();
  scene->reply_to_message_id = 0;
  scene->editing_message_id = 0;
  scene->keyboard = nullptr;
  scene->composer = nullptr;
  scene->send_button = nullptr;
  scene->attach_button = nullptr;
  scene->composer_context = nullptr;
  scene->composer_context_label = nullptr;
  auto *back = Button(scene->surface, LV_SYMBOL_LEFT, [scene] {
    scene->chat_id = 0;
    ShowChats(scene);
  });
  lv_obj_set_pos(back, 24, 20);
  lv_obj_set_size(back, 120, 112);
  scene->title = Label(scene->surface, title.c_str(),
                       &lv_font_montserrat_48, kText);
  lv_obj_set_pos(scene->title, 174, 20);
  lv_obj_set_width(scene->title, 1160);
  lv_label_set_long_mode(scene->title, LV_LABEL_LONG_DOT);
  scene->detail = Label(scene->surface, "Loading message history...",
                        &lv_font_montserrat_24, kGreen);
  lv_obj_set_pos(scene->detail, 174, 88);
  scene->progress = nullptr;

  scene->list = lv_obj_create(scene->surface);
  Clear(scene->list);
  lv_obj_set_pos(scene->list, 16, 170);
  lv_obj_set_size(scene->list, scene->landscape ? 1536 : 1408,
                  scene->landscape ? 1080 : 2478);
  lv_obj_add_flag(scene->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scene->list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_opa(scene->list, LV_OPA_TRANSP, 0);
  scene->item_y = 18;

  scene->attach_button = Button(scene->surface, LV_SYMBOL_PLUS, [scene] {
    SetChatKeyboard(scene, false);
    ShowAttachmentPicker(scene, "");
  });
  lv_obj_set_pos(scene->attach_button, scene->landscape ? 1600 : 32,
                 scene->landscape ? 170 : 2780);
  lv_obj_set_size(scene->attach_button, 132, 132);
  lv_obj_set_style_radius(scene->attach_button, 66, 0);

  scene->composer_context = lv_obj_create(scene->surface);
  Clear(scene->composer_context);
  lv_obj_set_pos(scene->composer_context, scene->landscape ? 1750 : 182,
                 scene->landscape ? 96 : 2700);
  lv_obj_set_size(scene->composer_context,
                  scene->landscape ? 1190 : 1088, 70);
  lv_obj_set_style_radius(scene->composer_context, 20, 0);
  lv_obj_set_style_bg_color(scene->composer_context, kMainPanel, 0);
  lv_obj_set_style_bg_opa(scene->composer_context, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scene->composer_context, 1, 0);
  lv_obj_set_style_border_color(scene->composer_context, kAccent, 0);
  scene->composer_context_label = Label(scene->composer_context, "",
                                         &lv_font_montserrat_20, kMutedStrong);
  lv_obj_set_pos(scene->composer_context_label, 22, 20);
  lv_obj_set_width(scene->composer_context_label,
                   (scene->landscape ? 1190 : 1088) - 105);
  lv_label_set_long_mode(scene->composer_context_label, LV_LABEL_LONG_DOT);
  auto *cancel_context = Button(scene->composer_context, LV_SYMBOL_CLOSE,
                                [scene] { ClearComposerContext(scene); });
  lv_obj_align(cancel_context, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_set_size(cancel_context, 62, 54);
  lv_obj_set_style_bg_opa(cancel_context, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(scene->composer_context, LV_OBJ_FLAG_HIDDEN);

  scene->composer = Input(scene->surface, 2780, "Message");
  lv_obj_set_pos(scene->composer, scene->landscape ? 1750 : 182,
                 scene->landscape ? 170 : 2780);
  lv_obj_set_width(scene->composer, scene->landscape ? 1190 : 1088);
  scene->send_button = Button(scene->surface, LV_SYMBOL_RIGHT, [scene] {
    const std::string text = lv_textarea_get_text(scene->composer);
    const telegram::Kind kind = scene->editing_message_id
        ? telegram::Kind::kEditMessage : telegram::Kind::kSendText;
    const int64_t target = scene->editing_message_id
        ? scene->editing_message_id : scene->reply_to_message_id;
    if (!text.empty() && scene->Send(kind, text, scene->chat_id, target)) {
      lv_textarea_set_text(scene->composer, "");
      i18n::BindLabel(scene->detail,
                      kind == telegram::Kind::kEditMessage
                          ? "Saving edit..." : "Sending...");
      ClearComposerContext(scene);
    }
  }, true);
  lv_obj_set_pos(scene->send_button, scene->landscape ? 2960 : 1288,
                 scene->landscape ? 170 : 2780);
  lv_obj_set_size(scene->send_button, 120, 132);
  lv_obj_set_style_radius(scene->send_button, 60, 0);

  scene->keyboard = lv_keyboard_create(scene->surface);
  StyleKeyboard(scene->keyboard);
  if (scene->landscape) {
    lv_obj_set_pos(scene->keyboard, 1600, 330);
    lv_obj_set_size(scene->keyboard, 1552, 900);
  } else {
    constexpr int keyboard_height = 1000;
    lv_obj_set_pos(scene->keyboard, 0,
                   lv_obj_get_height(scene->surface) - keyboard_height);
    lv_obj_set_size(scene->keyboard, lv_obj_get_width(scene->surface),
                    keyboard_height);
  }
  lv_keyboard_set_textarea(scene->keyboard, scene->composer);
  lv_obj_add_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(scene->composer, [](lv_event_t *event) {
    SetChatKeyboard(static_cast<TelegramScene *>(lv_event_get_user_data(event)), true);
  }, LV_EVENT_FOCUSED, scene);
  lv_obj_add_event_cb(scene->keyboard, [](lv_event_t *event) {
    auto *scene = static_cast<TelegramScene *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY) {
      lv_obj_send_event(scene->send_button, LV_EVENT_CLICKED, nullptr);
      SetChatKeyboard(scene, false);
    } else if (lv_event_get_code(event) == LV_EVENT_CANCEL) {
      SetChatKeyboard(scene, false);
    }
  }, LV_EVENT_ALL, scene);
  scene->Send(telegram::Kind::kOpenChat, {}, id);
}

void AddChat(TelegramScene *scene, const telegram::Message &message) {
  if (!scene->list) return;
  const std::string payload = message.text;
  const auto split = payload.find('\n');
  const auto avatar_split = payload.find_last_of('\n');
  const std::string title = payload.substr(0, split);
  const bool has_avatar_field = split != std::string::npos &&
      avatar_split != std::string::npos && avatar_split != split;
  const std::string preview = split == std::string::npos ? "No recent message" :
      payload.substr(split + 1, has_avatar_field
          ? avatar_split - split - 1 : std::string::npos);
  const std::string avatar_path = has_avatar_field
      ? payload.substr(avatar_split + 1) : std::string{};
  auto *row = Button(scene->list, "", [scene, id = message.primary, title] {
    OpenChat(scene, id, title);
  });
  lv_obj_set_pos(row, 0, scene->item_y);
  const int row_width = scene->landscape ? 3136 : 1408;
  lv_obj_set_size(row, row_width, 164);
  lv_obj_set_style_radius(row, 0, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  auto *avatar = Avatar(scene, row, title, avatar_path, 92);
  lv_obj_set_pos(avatar, 26, 35);
  auto *name = Label(row, title.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 148, 28);
  lv_obj_set_width(name, row_width - (message.value ? 350 : 250));
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  auto *copy = Label(row, preview.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 148, 88);
  lv_obj_set_width(copy, row_width - 300);
  lv_label_set_long_mode(copy, LV_LABEL_LONG_DOT);
  if (message.value) {
    char count[16];
    snprintf(count, sizeof(count), "%u", message.value);
    auto *badge = lv_obj_create(row);
    Panel(badge, 32, kCyan);
    lv_obj_set_size(badge, 76, 58);
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -34, 0);
    auto *number = Label(badge, count, &lv_font_montserrat_20, kCanvas);
    lv_obj_center(number);
  }
  auto *line = lv_obj_create(row);
  Clear(line);
  lv_obj_set_pos(line, 148, 162);
  lv_obj_set_size(line, row_width - 148, 1);
  lv_obj_set_style_bg_color(line, kMainLine, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  scene->item_y += 164;
}

void ShowChats(TelegramScene *scene) {
  lv_obj_clean(scene->surface);
  scene->avatars.clear();
  scene->avatar_views.clear();
  scene->photo_previews.clear();
  scene->photo_message_contexts.clear();
  scene->chat_id = 0;
  lv_obj_set_user_data(scene->surface, nullptr);
  scene->keyboard = nullptr;
  scene->composer = nullptr;
  scene->send_button = nullptr;
  scene->attach_button = nullptr;
  auto *home = Button(scene->surface, LV_SYMBOL_LEFT, [scene] {
    if (scene->callback) scene->callback(Action::kBack, scene->context);
  });
  lv_obj_set_pos(home, 24, 20);
  lv_obj_set_size(home, 120, 112);
  scene->title = Label(scene->surface, "Chats", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(scene->title, 174, 20);
  scene->detail = Label(scene->surface, "Connected securely",
                        &lv_font_montserrat_24, kGreen);
  lv_obj_set_pos(scene->detail, 174, 88);
  scene->progress = nullptr;
  auto *status = Kicker(scene->surface, "AERA TELEGRAM", kCyan);
  lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -32, 48);
  scene->list = lv_obj_create(scene->surface);
  Clear(scene->list);
  lv_obj_set_pos(scene->list, 16, 170);
  lv_obj_set_size(scene->list, scene->landscape ? 3136 : 1408,
                  scene->landscape ? 1080 : 2818);
  lv_obj_add_flag(scene->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scene->list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_opa(scene->list, LV_OPA_TRANSP, 0);
  scene->item_y = 0;
  scene->Send(telegram::Kind::kLoadChats);
}

void AddMessage(TelegramScene *scene, const telegram::Message &message) {
  if (!scene->list || scene->chat_id != message.primary) return;
  std::string payload = message.text;
  const auto split = payload.find('\n');
  const std::string sender = payload.substr(0, split);
  const auto avatar_split = split == std::string::npos
      ? std::string::npos : payload.find('\n', split + 1);
  const std::string avatar_path = avatar_split == std::string::npos
      ? std::string{} : payload.substr(split + 1, avatar_split - split - 1);
  const std::string text = split == std::string::npos ? "" :
      payload.substr(avatar_split == std::string::npos
          ? split + 1 : avatar_split + 1);
  const bool outgoing = (message.value & 1U) != 0;
  const bool editable = (message.value & (1U << 3)) != 0;
  const bool photo = (message.value & (1U << 4)) != 0;
  const uint32_t delivery_status = (message.value >> 1) & 3U;
  if (outgoing) scene->message_statuses[message.secondary] = delivery_status;
  if (photo) {
    scene->photo_message_contexts[message.secondary] =
        {sender, avatar_path, text == "Photo" ? std::string{} : text, outgoing};
    return;
  }
  auto *bubble = lv_obj_create(scene->list);
  Panel(bubble, 34, outgoing ? Color(0x16495a) : kMainPanel);
  const int width = scene->landscape ? 1010 : 900;
  const int bubble_x = outgoing
      ? std::max(20, static_cast<int>(lv_obj_get_width(scene->list)) - width - 20)
      : 20;
  lv_obj_set_pos(bubble, bubble_x, scene->item_y);
  lv_obj_set_size(bubble, width, 180);
  auto *avatar = Avatar(scene, bubble, sender, avatar_path, 64);
  lv_obj_set_pos(avatar, 22, 18);
  auto *name = Label(bubble, sender.c_str(), &lv_font_montserrat_20,
                     outgoing ? kCyan : kGreen);
  lv_obj_set_pos(name, 108, 26);
  lv_obj_set_width(name, width - 140);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  auto *body = Label(bubble, text.c_str(), &lv_font_montserrat_24, kText);
  lv_obj_set_pos(body, 30, 94);
  lv_obj_set_width(body, width - 60);
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_obj_update_layout(body);
  const int bubble_height = std::clamp(lv_obj_get_height(body) + 132, 180, 650);
  lv_obj_set_height(bubble, bubble_height);
  if (outgoing) {
    auto *status = MessageStatus(scene, bubble, message.secondary);
    lv_obj_align(status, LV_ALIGN_BOTTOM_RIGHT, -24, -16);
  }
  OnPressOrHold(bubble, [] {},
      [scene, message_id = message.secondary, text, outgoing, editable] {
        ShowMessageActions(scene, message_id, text, outgoing,
                           outgoing && editable);
      });
  scene->item_y += bubble_height + 20;
  if (!scene->history_loading)
    lv_obj_scroll_to_y(scene->list, scene->item_y, LV_ANIM_ON);
}

void AddAttachment(TelegramScene *scene, const telegram::Message &message) {
  if (!scene->list || scene->chat_id != message.primary || !message.value) return;
  const std::string payload = message.text;
  const auto first = payload.find('\n');
  const auto second = first == std::string::npos ? std::string::npos :
      payload.find('\n', first + 1);
  const auto third = second == std::string::npos ? std::string::npos :
      payload.find('\n', second + 1);
  const std::string name = payload.substr(0, first);
  const std::string size_text = first == std::string::npos ? std::string{} :
      payload.substr(first + 1, second == std::string::npos
          ? std::string::npos : second - first - 1);
  const uint64_t bytes = strtoull(size_text.c_str(), nullptr, 10);
  const std::string kind = second == std::string::npos ? "file" :
      payload.substr(second + 1, third == std::string::npos
          ? std::string::npos : third - second - 1);
  const std::string preview_path = third == std::string::npos
      ? std::string{} : payload.substr(third + 1);
  const bool photo = kind == "photo";
  const int32_t file_id = static_cast<int32_t>(message.value);
  const auto context_it = scene->photo_message_contexts.find(message.secondary);
  const bool has_context = photo &&
      context_it != scene->photo_message_contexts.end();
  const std::string sender = has_context ? context_it->second.sender : "";
  const std::string avatar_path = has_context
      ? context_it->second.avatar_path : "";
  const std::string caption = has_context ? context_it->second.caption : "";
  const bool outgoing = has_context ? context_it->second.outgoing :
      scene->message_statuses.find(message.secondary) !=
          scene->message_statuses.end();
  const int width = scene->landscape ? 1010 : 900;
  const int card_x = outgoing
      ? std::max(20, static_cast<int>(lv_obj_get_width(scene->list)) - width - 20)
      : 20;
  auto *card = Button(scene->list, "", [] {});
  OnPressOrHold(card, [scene, file_id, name, bytes, photo] {
      if (photo) ShowPhotoViewer(scene, file_id, name, bytes);
      else ConfirmDownload(scene, file_id, name, bytes);
    }, [scene, message_id = message.secondary,
        action_text = caption.empty() ? name : caption, outgoing] {
      ShowMessageActions(scene, message_id, action_text, outgoing, false);
    });
  lv_obj_set_pos(card, card_x, scene->item_y);
  lv_obj_set_size(card, width, photo ? 548 : 124);
  lv_obj_set_style_radius(card, 30, 0);
  if (photo) {
    const int header_height = has_context ? 92 : 18;
    if (has_context) {
      auto *avatar = Avatar(scene, card, sender, avatar_path, 58);
      lv_obj_set_pos(avatar, 22, 17);
      auto *sender_name = Label(card, sender.c_str(),
          &lv_font_montserrat_20, outgoing ? kCyan : kGreen);
      lv_obj_set_pos(sender_name, 96, 30);
      lv_obj_set_width(sender_name, width - 130);
      lv_label_set_long_mode(sender_name, LV_LABEL_LONG_DOT);
    }
    auto *preview = lv_obj_create(card);
    Clear(preview);
    lv_obj_set_pos(preview, 18, header_height);
    lv_obj_set_size(preview, width - 36, 402);
    lv_obj_set_style_radius(preview, 24, 0);
    lv_obj_set_style_bg_color(preview, kCanvas, 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    const bool preview_loaded =
        RenderPhotoPreview(scene, preview, preview_path);
    if (!preview_loaded) {
      auto *placeholder = Label(preview, LV_SYMBOL_IMAGE,
                                &lv_font_montserrat_48, kMutedStrong);
      lv_obj_center(placeholder);
    }
    scene->photo_previews[file_id] =
        {preview, preview_path, preview_loaded, lv_tick_get() + 350};
    int footer_y = header_height + 420;
    if (!caption.empty()) {
      auto *body = Label(card, caption.c_str(), &lv_font_montserrat_24, kText);
      lv_obj_set_pos(body, 28, footer_y);
      lv_obj_set_width(body, width - 56);
      lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
      lv_obj_update_layout(body);
      footer_y += std::clamp(static_cast<int>(lv_obj_get_height(body)), 34, 280) + 18;
    }
    const std::string detail = Size(bytes) + "  •  Tap to view";
    auto *meta = Label(card, detail.c_str(), &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(meta, 28, footer_y);
    const int card_height = footer_y + 58;
    lv_obj_set_height(card, card_height);
    auto *open = Label(card, LV_SYMBOL_EYE_OPEN,
                       &lv_font_montserrat_28, kCyan);
    lv_obj_align(open, LV_ALIGN_BOTTOM_RIGHT, -30, -22);
    if (outgoing) {
      auto *status = MessageStatus(scene, card, message.secondary);
      lv_obj_align(status, LV_ALIGN_BOTTOM_RIGHT, -102, -22);
    }
    scene->photo_message_contexts.erase(message.secondary);
    scene->item_y += card_height + 20;
    if (!scene->history_loading)
      lv_obj_scroll_to_y(scene->list, scene->item_y, LV_ANIM_ON);
    return;
  }
  auto *icon = IconPlate(card, LV_SYMBOL_DOWNLOAD, kCyan, kMainSelected, 72);
  lv_obj_set_pos(icon, 24, 26);
  auto *title = Label(card, name.c_str(), &lv_font_montserrat_28, kText);
  lv_obj_set_pos(title, 120, 20);
  lv_obj_set_width(title, width - 220);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  const std::string detail = Size(bytes) + "  •  Tap to download";
  auto *meta = Label(card, detail.c_str(), &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(meta, 120, 72);
  auto *arrow = Label(card, LV_SYMBOL_DOWNLOAD, &lv_font_montserrat_28, kCyan);
  lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -30, 0);
  if (outgoing) {
    auto *status = MessageStatus(scene, card, message.secondary);
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, -96, 0);
  }
  scene->item_y += 144;
  if (!scene->history_loading)
    lv_obj_scroll_to_y(scene->list, scene->item_y, LV_ANIM_ON);
}

void Poll(TelegramScene *scene) {
  for (int i = 0; i < 24 && scene->control >= 0; ++i) {
    telegram::Message message;
    const ssize_t count = recv(scene->control, &message, sizeof(message),
                               MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count < 0 && errno == EINTR) continue;
    if (count != static_cast<ssize_t>(sizeof(message)) ||
        !telegram::Valid(message, true)) {
      close(scene->control); scene->control = -1;
      ShowStatus(scene, "Telegram stopped", "The isolated runtime closed unexpectedly.");
      return;
    }
    if (message.kind == telegram::Kind::kState) {
      const auto state = static_cast<telegram::AuthState>(message.value);
      if (state == telegram::AuthState::kReady) {
        if (!scene->pending_vault_password.empty()) {
          SecureClear(g_boot_vault_password);
          g_boot_vault_password = scene->pending_vault_password;
          SecureClear(scene->pending_vault_password);
        }
        scene->using_cached_vault = false;
        ShowChats(scene);
      } else if (state == telegram::AuthState::kNeedVault &&
                 !g_boot_vault_password.empty() &&
                 !scene->using_cached_vault) {
        scene->pending_vault_password = g_boot_vault_password;
        scene->using_cached_vault = true;
        if (scene->Send(telegram::Kind::kConfigure,
                        scene->pending_vault_password, 0)) {
          ShowStatus(scene, "Restoring Telegram session",
                     "Unlocking the encrypted session from this recovery boot.");
        } else {
          SecureClear(scene->pending_vault_password);
          scene->using_cached_vault = false;
          ShowAuth(scene, state, message.text);
        }
      } else {
        if (state == telegram::AuthState::kNeedVault &&
            scene->using_cached_vault) {
          SecureClear(g_boot_vault_password);
          SecureClear(scene->pending_vault_password);
          scene->using_cached_vault = false;
        }
        ShowAuth(scene, state, message.text);
      }
    } else if (message.kind == telegram::Kind::kError) {
      if (!scene->pending_vault_password.empty()) {
        if (scene->using_cached_vault) SecureClear(g_boot_vault_password);
        SecureClear(scene->pending_vault_password);
        scene->using_cached_vault = false;
        ShowAuth(scene, telegram::AuthState::kNeedVault, message.text);
      } else if (scene->detail) {
        i18n::BindLabel(scene->detail, message.text);
      }
    } else if (message.kind == telegram::Kind::kChat) {
      if (!scene->chat_id) AddChat(scene, message);
    } else if (message.kind == telegram::Kind::kMessage) {
      AddMessage(scene, message);
    } else if (message.kind == telegram::Kind::kAttachment) {
      AddAttachment(scene, message);
    } else if (message.kind == telegram::Kind::kFileProgress) {
      if (scene->detail && scene->chat_id) {
        const std::string status = i18n::Format(
            "Downloading %s  •  %u%%", message.text, message.value);
        i18n::BindLabel(scene->detail, status.c_str());
      }
    } else if (message.kind == telegram::Kind::kFileReady) {
      if (scene->detail && scene->chat_id) {
        const std::string status = i18n::Format("Saved to %s", message.text);
        i18n::BindLabel(scene->detail, status.c_str());
      }
    } else if (message.kind == telegram::Kind::kPhotoReady) {
      if (message.primary > 0 && message.primary <= INT32_MAX) {
        const int32_t file_id = static_cast<int32_t>(message.primary);
        const auto preview = scene->photo_previews.find(file_id);
        if (preview != scene->photo_previews.end()) {
          preview->second.runtime_path = message.text;
          if (lv_obj_is_valid(preview->second.container))
            preview->second.loaded = RenderPhotoPreview(
                scene, preview->second.container, message.text);
          if (scene->photo_viewer_file_id == file_id &&
              scene->photo_viewer_preview &&
              lv_obj_is_valid(scene->photo_viewer_preview))
            RenderPhotoPreview(scene, scene->photo_viewer_preview,
                               message.text);
        }
      }
    } else if (message.kind == telegram::Kind::kAvatarReady) {
      const auto avatars = scene->avatar_views.find(message.text);
      if (avatars != scene->avatar_views.end()) {
        for (const auto &view : avatars->second)
          if (view.plate && lv_obj_is_valid(view.plate))
            RenderAvatar(scene, view.plate, view.name, message.text, view.size);
      }
    } else if (message.kind == telegram::Kind::kMessageStatus) {
      if (message.primary == scene->chat_id) {
        scene->message_statuses[message.secondary] = message.value;
        const auto views = scene->message_status_views.find(message.secondary);
        if (views != scene->message_status_views.end())
          for (const auto &view : views->second)
            ApplyMessageStatus(view.label, message.value);
      }
    } else if (message.kind == telegram::Kind::kMessageDeleted) {
      if (message.primary == scene->chat_id && scene->title) {
        const std::string title = lv_label_get_text(scene->title);
        OpenChat(scene, scene->chat_id, title);
      }
    } else if (message.kind == telegram::Kind::kStatus) {
      if (scene->detail && scene->chat_id)
        i18n::BindLabel(scene->detail, message.text);
    } else if (message.kind == telegram::Kind::kChatsDone && scene->detail) {
      i18n::BindLabel(scene->detail, scene->item_y ? "Connected securely" :
          "No chats were returned by Telegram.");
    } else if (message.kind == telegram::Kind::kMessagesDone && scene->detail) {
      scene->history_loading = false;
      const std::string status = i18n::Format(
          "%u messages loaded securely", message.value);
      i18n::BindLabel(scene->detail, status.c_str());
      if (scene->list)
        lv_obj_scroll_to_y(scene->list, scene->item_y, LV_ANIM_OFF);
    }
  }

  // A completed TDLib download and its IPC notification can straddle card
  // creation. Retry one pending stable path per timer tick, throttled so a
  // history containing many photos never stalls touch or scrolling.
  const uint32_t now = lv_tick_get();
  for (auto &[file_id, preview] : scene->photo_previews) {
    if (preview.loaded || preview.runtime_path.empty() ||
        !preview.container || !lv_obj_is_valid(preview.container) ||
        static_cast<int32_t>(now - preview.retry_after) < 0)
      continue;
    preview.loaded = RenderPhotoPreview(
        scene, preview.container, preview.runtime_path);
    if (preview.loaded && scene->photo_viewer_file_id == file_id &&
        scene->photo_viewer_preview &&
        lv_obj_is_valid(scene->photo_viewer_preview))
      RenderPhotoPreview(scene, scene->photo_viewer_preview,
                         preview.runtime_path);
    preview.retry_after = now + (preview.loaded ? 0 : 750);
    break;
  }
}

void Launch(TelegramScene *scene) {
  if (scene->launched || !scene->preparation.verified) return;
  scene->launched = true;
  std::string error;
  if (!scene->process.Start(scene->preparation.directory, scene->control, error)) {
    ShowStatus(scene, "AERA Telegram could not start", error.c_str());
    return;
  }
  fcntl(scene->control, F_SETFL, fcntl(scene->control, F_GETFL) | O_NONBLOCK);
  ShowStatus(scene, "Starting AERA Telegram",
             "Launching TDLib inside its private network sandbox.");
}
}  // namespace

void BuildTelegramScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *scene = new TelegramScene;
  scene->existing_vault =
      access("/data/recovery/AERA/telegram/database", F_OK) == 0;
  scene->screen = screen;
  scene->landscape = lv_obj_get_width(screen) > lv_obj_get_height(screen);
  scene->callback = callback;
  scene->context = context;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<TelegramScene *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, scene);
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  scene->surface = lv_obj_create(screen);
  Clear(scene->surface);
  lv_obj_add_event_cb(scene->surface, [](lv_event_t *event) {
    auto *scene = static_cast<TelegramScene *>(lv_event_get_user_data(event));
    if (scene->keyboard_visible)
      SetChatKeyboard(scene, false);
    else if (scene->chat_id != 0)
      ShowChats(scene);
  }, LV_EVENT_CANCEL, scene);
  lv_obj_set_align(scene->surface, LV_ALIGN_TOP_LEFT);
  const int status_height = StatusBarHeight();
  lv_obj_set_pos(scene->surface, 0, status_height);
  lv_obj_set_size(scene->surface, lv_obj_get_width(screen),
                  lv_obj_get_height(screen) - status_height);
  lv_obj_set_style_bg_color(scene->surface, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(scene->surface, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scene->surface, 0, 0);
  scene->navigation = nullptr;
  ShowStatus(scene, "Preparing AERA Telegram",
             "Verifying the signed TDLib runtime and expanding it into private RAM.");
  scene->worker = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, "telegram",
                              "app-runtime", "telegram");
  });
  scene->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *scene = static_cast<TelegramScene *>(lv_timer_get_user_data(timer));
    if (!scene->preparation.done.load(std::memory_order_acquire)) {
      if (scene->progress)
        lv_bar_set_value(scene->progress, scene->preparation.progress.load(), LV_ANIM_OFF);
      return;
    }
    if (!scene->preparation.verified) {
      if (scene->title) i18n::BindLabel(scene->title, "AERA Telegram unavailable");
      if (scene->detail) i18n::BindLabel(scene->detail, scene->preparation.error.c_str());
      return;
    }
    Launch(scene);
    Poll(scene);
    if (scene->launched && !scene->process.Running() && scene->control >= 0) {
      close(scene->control); scene->control = -1;
      ShowStatus(scene, "AERA Telegram closed", "Return Home to reopen it.");
    }
  }, 16, scene);
}
}  // namespace aeraui
