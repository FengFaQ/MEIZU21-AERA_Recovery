/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"
#include "../../aera_remote/aera_remote.hpp"

#include "browser/runtime.hpp"
#include "plugin_api/launcher.hpp"
#include "plugin_api/operations.hpp"
#include "plugin_api/protocol.hpp"
#include "plugin_api/session.hpp"
#include "plugins/plugin_manager.hpp"
#include "ui_components.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

std::mutex gSelectionMutex;
std::string gSelectedPlugin;

uint64_t NowMs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

struct ButtonModel {
  uint32_t id = 0;
  uint32_t flags = 0;
  std::string title;
  std::string detail;
};

enum class RowKind { kSection, kMetric };

struct RowModel {
  RowKind kind = RowKind::kMetric;
  uint32_t id = 0;
  uint32_t flags = 0;
  std::string title;
  std::string value;
  lv_obj_t *value_label = nullptr;
};

struct GenericScene {
  lv_obj_t *screen = nullptr;
  lv_obj_t *card = nullptr;
  lv_obj_t *content = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *mirror_tabs[2] = {nullptr, nullptr};
  uint32_t mirror_selection = 0;
  std::string mirror_access_code;
  uint64_t mirror_code_checked_ms = 0;
  plugins::Plugin plugin;
  web::Preparation preparation;
  plugin_api::Process process;
  plugin_api::Session session;
  std::thread worker;
  lv_timer_t *timer = nullptr;
  std::string page_title;
  std::string page_body;
  std::vector<ButtonModel> buttons;
  std::vector<RowModel> rows;
  uint32_t page_back_action = 0;
  uint32_t pending_back_action = 0;
  uint64_t launched_ms = 0;
  uint64_t operation_started_ms = 0;
  uint32_t operation_request = 0;
  bool launched = false;
  bool page_pending = false;
  bool operation_pending = false;
  bool stopped = false;

  ~GenericScene() {
    if (timer) lv_timer_delete(timer);
    preparation.cancel.store(true);
    if (worker.joinable()) worker.join();
    if (session.Connected()) {
      session.Send(plugin_api::Kind::kLifecycle, 0,
                   static_cast<uint32_t>(plugin_api::Lifecycle::kStop));
      session.Send(plugin_api::Kind::kClose);
    }
    session.Close();
    process.Stop();
    web::RemoveRuntime(preparation.directory);
  }
};

void SetStatus(GenericScene *scene, const std::string &status) {
  if (scene->status) i18n::BindLabel(scene->status, status.c_str());
}

void SetBackAction(GenericScene *scene, uint32_t action) {
  scene->page_back_action = action;
  if (scene->card)
    lv_obj_set_user_data(scene->card,
                         action ? &kPersistentModalMarker : nullptr);
}

void RequestPage(GenericScene *scene, uint32_t action) {
  if (!action || !scene->session.Connected()) return;
  // Mirror Host API 2 actions 1/2 select the Wi-Fi/USB guide; they do not
  // start the server. Start actions still use the host approval sheet.
  if (scene->plugin.id == "mirror" && (action == 1 || action == 2))
    scene->mirror_selection = action;
  SetBackAction(scene, 0);
  SetStatus(scene, "Working...");
  if (scene->progress) {
    lv_bar_set_value(scene->progress, 24, LV_ANIM_OFF);
    lv_bar_set_value(scene->progress, 72, LV_ANIM_ON);
  }
  scene->session.Send(plugin_api::Kind::kAction, action);
}

void MirrorModeCard(GenericScene *scene, int index, int x, int y, int width) {
  auto *card = lv_button_create(scene->screen);
  scene->mirror_tabs[index] = card;
  Panel(card, 34, kMainPanel);
  Interactive(card, kMainSelected);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, 220);
  auto *symbol = Label(card, index == 0 ? LV_SYMBOL_WIFI : LV_SYMBOL_USB,
                        &lv_font_montserrat_48, kAccent);
  lv_obj_set_pos(symbol, 34, 30);
  auto *heading = Label(card, index == 0 ? "Wi-Fi Mirror" : "USB Mirror",
                         &lv_font_montserrat_32, kText);
  lv_obj_set_pos(heading, 112, 28);
  SingleLineLabel(heading, width - 176, &lv_font_montserrat_32);
  auto *copy = Label(card, index == 0
      ? "Connect from a browser on your Wi-Fi network."
      : "Connect through ADB and the desktop launcher.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 34, 112);
  FitLabelToLines(copy, width - 68, 2,
      {&lv_font_montserrat_24, &lv_font_montserrat_20, &lv_font_montserrat_18});
  OnClick(card, [scene, index] { RequestPage(scene, index + 1); });
}

bool ModalVisible(lv_obj_t *screen) {
  for (uint32_t index = 0; index < lv_obj_get_child_count(screen); ++index) {
    auto *child = lv_obj_get_child(screen, static_cast<int32_t>(index));
    if (lv_obj_get_user_data(child) == &kModalMarker &&
        !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) return true;
  }
  return false;
}

const char *ActionIcon(const ButtonModel &model) {
  if (model.title.find("Wi-Fi") != std::string::npos)
    return LV_SYMBOL_WIFI;
  if (model.title.find("USB") != std::string::npos)
    return LV_SYMBOL_USB;
  if (model.title.find("Stop") != std::string::npos)
    return LV_SYMBOL_CLOSE;
  if (model.title.find("another") != std::string::npos ||
      model.title.find("Return") != std::string::npos)
    return LV_SYMBOL_LEFT;
  if (model.title.find("Start") != std::string::npos ||
      model.title.find("Restart") != std::string::npos)
    return LV_SYMBOL_PLAY;
  return LV_SYMBOL_RIGHT;
}

lv_obj_t *ActionCard(GenericScene *scene, const ButtonModel &model, int width,
                     int y, size_t index) {
  const bool primary = (model.flags & plugin_api::kPrimary) != 0;
  const bool destructive = (model.flags & plugin_api::kDestructive) != 0;
  const bool disabled = (model.flags & plugin_api::kDisabled) != 0;
  auto *card = lv_button_create(scene->content);
  Panel(card, 34, kMainSheet);
  Interactive(card, primary ? kMainSelected : kMainPanel);
  lv_obj_set_pos(card, 36, y);
  lv_obj_set_size(card, width, model.detail.empty() ? 116 : 148);
  lv_obj_set_style_border_width(card, primary ? 2 : 1, 0);
  lv_obj_set_style_border_color(card,
      destructive ? kRed : primary ? kAccent : kMainLine, 0);
  lv_obj_set_style_border_opa(card,
      primary ? LV_OPA_60 : destructive ? LV_OPA_40 : LV_OPA_30, 0);
  lv_obj_set_style_transform_scale(card, 254, LV_STATE_PRESSED);
  auto *plate = IconPlate(card, ActionIcon(model),
                          destructive ? kRed : primary ? kAccent : kText,
                          primary ? kMainPanel : kMainCanvas, 76);
  lv_obj_align(plate, LV_ALIGN_LEFT_MID, 24, 0);
  auto *title = Label(card, model.title.c_str(), &lv_font_montserrat_32,
                      destructive ? kRed : kText);
  lv_obj_set_pos(title, 124, model.detail.empty() ? 38 : 28);
  lv_obj_set_width(title, width - 210);
  if (!model.detail.empty()) {
    auto *detail = Label(card, model.detail.c_str(),
                         &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail, 124, 78);
    lv_obj_set_width(detail, width - 210);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
  }
  auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_24,
                      primary ? kAccent : kDim);
  lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -28, 0);
  OnClick(card, [scene, id = model.id, disabled] {
    if (!disabled) RequestPage(scene, id);
  });
  if (disabled) lv_obj_add_state(card, LV_STATE_DISABLED);
  AnimateEnter(card, 25 + static_cast<uint32_t>(index) * 26, 10);
  return card;
}

lv_color_t MetricColor(uint32_t flags) {
  if (flags & plugin_api::kMetricCritical) return kRed;
  if (flags & plugin_api::kMetricWarning) return kAmber;
  if (flags & plugin_api::kMetricGood) return kGreen;
  if (flags & plugin_api::kMetricAccent) return kAccent;
  return kText;
}

bool MetricFlagsValid(uint32_t flags) {
  constexpr uint32_t mask = plugin_api::kMetricGood |
      plugin_api::kMetricWarning | plugin_api::kMetricCritical |
      plugin_api::kMetricAccent;
  const uint32_t tone = flags & mask;
  return (flags & ~mask) == 0 && (tone == 0 || (tone & (tone - 1)) == 0);
}

void RenderMetricPage(GenericScene *scene) {
  lv_obj_clean(scene->content);
  lv_obj_update_layout(scene->content);
  const int content_width = lv_obj_get_width(scene->content);
  const size_t metric_count = std::count_if(
      scene->rows.begin(), scene->rows.end(), [](const RowModel &row) {
        return row.kind == RowKind::kMetric;
      });
  const bool animate_rows = metric_count <= 20;
  auto *title = Label(scene->content,
                      scene->page_title.empty() ? scene->plugin.name.c_str()
                                                : scene->page_title.c_str(),
                      &lv_font_montserrat_40, kText);
  lv_obj_set_pos(title, 36, 22);
  lv_obj_set_width(title, std::max(400, content_width - 72));
  int y = 92;
  if (!scene->page_body.empty()) {
    auto *body = Label(scene->content, scene->page_body.c_str(),
                       &lv_font_montserrat_28, kMutedStrong);
    lv_obj_set_pos(body, 36, y);
    lv_obj_set_width(body, std::max(400, content_width - 72));
    lv_obj_set_style_text_line_space(body, 8, 0);
    lv_obj_update_layout(body);
    y += static_cast<int>(lv_obj_get_height(body)) + 30;
  }
  size_t visible_index = 0;
  for (auto &model : scene->rows) {
    model.value_label = nullptr;
    if (model.kind == RowKind::kSection) {
      if (visible_index != 0) y += 18;
      auto *section = Label(scene->content, model.title.c_str(),
                            &lv_font_montserrat_24, kAccent);
      lv_obj_set_pos(section, 36, y + 12);
      lv_obj_set_width(section, std::max(400, content_width - 72));
      lv_obj_set_style_text_letter_space(section, 2, 0);
      y += 56;
      continue;
    }
    const int row_width = std::max(400, content_width - 72);
    const int column_gap = 36;
    const int name_width = std::max(180, (row_width - column_gap) * 2 / 5);
    const int value_width = std::max(180, row_width - name_width - column_gap);
    auto *row = lv_obj_create(scene->content);
    Clear(row);
    lv_obj_set_pos(row, 36, y);
    lv_obj_set_width(row, row_width);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, kMainLine, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_20, 0);
    auto *name = Label(row, model.title.c_str(), &lv_font_montserrat_32,
                       kMutedStrong);
    lv_obj_set_pos(name, 0, 20);
    lv_obj_set_width(name, name_width);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
    model.value_label = Label(row, model.value.c_str(),
                              &lv_font_montserrat_32,
                              MetricColor(model.flags));
    lv_obj_set_pos(model.value_label, name_width + column_gap, 20);
    lv_obj_set_width(model.value_label, value_width);
    lv_obj_set_style_text_align(model.value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(model.value_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_update_layout(row);
    const int row_height = std::max(
        82, std::max(static_cast<int>(lv_obj_get_height(name)),
                     static_cast<int>(lv_obj_get_height(model.value_label))) + 40);
    lv_obj_set_height(row, row_height);
    if (animate_rows)
      AnimateEnter(row, 12 + static_cast<uint32_t>(visible_index) * 10, 6);
    ++visible_index;
    y += row_height;
  }
  if (!scene->buttons.empty()) y += 26;
  for (size_t index = 0; index < scene->buttons.size(); ++index) {
    const auto &model = scene->buttons[index];
    ActionCard(scene, model, std::max(400, content_width - 72), y,
               visible_index + index);
    y += model.detail.empty() ? 136 : 168;
  }
}

void RenderPage(GenericScene *scene) {
  if (!scene->rows.empty()) {
    RenderMetricPage(scene);
    return;
  }
  lv_obj_clean(scene->content);
  // LVGL defers coordinate resolution until a layout pass. Generic plugin
  // pages can commit during the very first timer tick, so resolve the explicit
  // content size before using it for child widths.
  lv_obj_update_layout(scene->content);
  const int content_width = lv_obj_get_width(scene->content);
  const bool mirror = scene->plugin.id == "mirror";
  if (mirror) {
    scene->mirror_access_code = plugin_api::ActiveMirrorMode() ==
        plugin_api::MirrorMode::kOff ? "" : aera::remote::AccessCode();
  }
  if (mirror) {
    for (int index = 0; index < 2; ++index) {
      auto *tab = scene->mirror_tabs[index];
      if (!tab) continue;
      const bool selected = scene->mirror_selection == static_cast<uint32_t>(index + 1);
      lv_obj_set_style_border_width(tab, selected ? 3 :
          RecoverySurfaceStyle() == SurfaceStyle::kOutline ? 2 : 1, 0);
      lv_obj_set_style_border_color(tab, selected ? kAccent : kMainLine, 0);
      lv_obj_set_style_border_opa(tab, selected ? LV_OPA_COVER : LV_OPA_50, 0);
    }
  }
  auto *title = Label(scene->content,
                      scene->page_title.empty() ? scene->plugin.name.c_str()
                                                : scene->page_title.c_str(),
                      &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 36, 28);
  lv_obj_set_width(title, std::max(400, content_width - 72));
  auto *body_panel = lv_obj_create(scene->content);
  Panel(body_panel, 36, kMainSheet);
  lv_obj_set_pos(body_panel, 36, 108);
  lv_obj_set_width(body_panel, std::max(400, content_width - 72));
  lv_obj_set_style_border_width(body_panel, 1, 0);
  lv_obj_set_style_border_color(body_panel, kMainLine, 0);
  lv_obj_set_style_border_opa(body_panel, LV_OPA_30, 0);
  auto *body_icon = IconPlate(body_panel,
      mirror ? LV_SYMBOL_VIDEO : LV_SYMBOL_SETTINGS,
      mirror ? kAccent : kText, kMainPanel, 88);
  lv_obj_set_pos(body_icon, 30, 32);
  auto *eyebrow = Label(body_panel,
      mirror ? "PRIVATE RECOVERY MIRROR" : "PLUGIN SESSION",
      &lv_font_montserrat_18, mirror ? kAccent : kMuted);
  lv_obj_set_pos(eyebrow, 146, 30);
  lv_obj_set_style_text_letter_space(eyebrow, 3, 0);
  if (mirror) {
    const bool live = plugin_api::ActiveMirrorMode() !=
                      plugin_api::MirrorMode::kOff;
    auto *state = Label(body_panel, live ? "●  LIVE" : "READY",
                        &lv_font_montserrat_18, live ? kGreen : kAccent);
    lv_obj_align(state, LV_ALIGN_TOP_RIGHT, -30, 30);
    lv_obj_set_style_text_letter_space(state, 2, 0);
  }
  std::string body_copy = scene->page_body;
  std::string address;
  if (mirror) {
    const size_t url = body_copy.find("http://");
    if (url != std::string::npos) {
      const size_t end = body_copy.find('\n', url);
      address = body_copy.substr(url, end == std::string::npos
                                         ? std::string::npos : end - url);
      const size_t line_start = body_copy.rfind('\n', url);
      body_copy.erase(line_start == std::string::npos ? 0 : line_start + 1,
                      end == std::string::npos
                          ? std::string::npos
                          : end - (line_start == std::string::npos
                                       ? 0 : line_start + 1) + 1);
    }
  }
  const int body_y = address.empty() ? 70 : 154;
  if (!address.empty()) {
    auto *address_plate = lv_obj_create(body_panel);
    Panel(address_plate, 22, kMainPanel);
    lv_obj_set_pos(address_plate, 146, 70);
    lv_obj_set_size(address_plate, std::max(250, content_width - 316), 64);
    auto *address_label = Label(address_plate, address.c_str(),
                                &lv_font_montserrat_32, kAccent);
    lv_obj_align(address_label, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_width(address_label, std::max(200, content_width - 356));
    lv_label_set_long_mode(address_label, LV_LABEL_LONG_DOT);
  }
  auto *body = Label(body_panel, body_copy.c_str(),
                     &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_pos(body, 146, body_y);
  lv_obj_set_width(body, std::max(280, content_width - 270));
  lv_obj_set_style_text_line_space(body, 14, 0);
  lv_obj_update_layout(body);
  const int panel_height = std::max(170,
      body_y + 48 + static_cast<int>(lv_obj_get_height(body)));
  lv_obj_set_height(body_panel, panel_height);
  AnimateEnter(body_panel, 10, 10);
  int y = 134 + panel_height;
  if (mirror && !scene->mirror_access_code.empty()) {
    // Read the actual running session's code for both transports, rather
    // than relying on the plugin's Wi-Fi-only result URL.
    auto *caption = Label(scene->content, "Access code",
                           &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(caption, 36, y + 8);
    lv_obj_set_width(caption, std::max(400, content_width - 72));
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    auto *code = Label(scene->content, scene->mirror_access_code.c_str(),
                        &lv_font_montserrat_48, kText);
    lv_obj_set_pos(code, 36, y + 48);
    lv_obj_set_width(code, std::max(400, content_width - 72));
    lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(code, 8, 0);
    y += 138;
  }
  const ButtonModel *stop_button = nullptr;
  size_t visible_index = 0;
  for (size_t index = 0; index < scene->buttons.size(); ++index) {
    const auto &model = scene->buttons[index];
    // The persistent mode cards replace Mirror's guide/home navigation.
    if (mirror && (model.id == 1 || model.id == 2 || model.id == 6))
      continue;
    if (mirror && model.id == 3) {
      if (plugin_api::ActiveMirrorMode() != plugin_api::MirrorMode::kOff)
        stop_button = &model;
      continue;
    }
    ActionCard(scene, model, std::max(400, content_width - 72), y,
               visible_index++);
    y += model.detail.empty() ? 136 : 168;
  }
  if (stop_button != nullptr) {
    const int stop_y = std::max(y + 24,
        static_cast<int>(lv_obj_get_height(scene->content)) - 184);
    ActionCard(scene, *stop_button, std::max(400, content_width - 72),
               stop_y, visible_index);
  }
}

void ReplyOperation(GenericScene *scene, plugin_api::Operation operation,
                    uint32_t request) {
  std::string result;
  const bool success = plugin_api::RunOperation(scene->plugin, operation, result);
  scene->session.Send(plugin_api::Kind::kOperationResult, request,
                      success ? 1U : 0U, 0, nullptr, result.c_str());
  SetStatus(scene, result);
  scene->operation_pending = false;
  scene->operation_request = 0;
}

void RequestOperation(GenericScene *scene,
                      const plugin_api::Message &message) {
  if (message.request_id == 0 || message.value <
          static_cast<uint32_t>(plugin_api::Operation::kBackupSettings) ||
      message.value >
          static_cast<uint32_t>(
              plugin_api::Operation::kStartWifiMirror)) {
    scene->session.Send(plugin_api::Kind::kOperationResult,
                        message.request_id, 0, 0, nullptr,
                        "Unsupported host operation.");
    return;
  }
  const auto operation = static_cast<plugin_api::Operation>(message.value);
  if (!plugin_api::OperationAllowed(scene->plugin, operation)) {
    scene->session.Send(plugin_api::Kind::kOperationResult,
                        message.request_id, 0, 0, nullptr,
                        "Permission denied by the AERA host.");
    return;
  }
  if (scene->operation_pending) {
    scene->session.Send(plugin_api::Kind::kOperationResult,
                        message.request_id, 0, 0, nullptr,
                        "Another host operation is waiting for approval.");
    return;
  }
  scene->operation_pending = true;
  scene->operation_request = message.request_id;
  scene->operation_started_ms = NowMs();
  SetStatus(scene, "Waiting for your approval");
  Sheet(scene->screen, plugin_api::OperationTitle(operation),
        plugin_api::OperationPrompt(operation),
        [scene, operation, request = message.request_id] {
          ReplyOperation(scene, operation, request);
        });
}

void HandleMessage(GenericScene *scene, const plugin_api::Message &message) {
  switch (message.kind) {
    case plugin_api::Kind::kBeginPage:
      scene->page_title = message.title;
      scene->page_body = message.text;
      scene->buttons.clear();
      scene->rows.clear();
      scene->pending_back_action = 0;
      scene->page_pending = true;
      break;
    case plugin_api::Kind::kAddButton:
      if (!scene->page_pending || message.request_id == 0 ||
          message.title[0] == '\0' ||
          (message.flags & ~(plugin_api::kPrimary |
                             plugin_api::kDestructive |
                             plugin_api::kDisabled)) != 0 ||
          scene->buttons.size() >= plugin_api::kMaxButtons ||
          std::any_of(scene->buttons.begin(), scene->buttons.end(),
                      [&](const ButtonModel &button) {
                        return button.id == message.request_id;
                      })) {
        SetStatus(scene, "Plugin page exceeded the Host API limit");
        scene->session.Close();
        break;
      }
      scene->buttons.push_back({message.request_id, message.flags,
                                message.title, message.text});
      break;
    case plugin_api::Kind::kAddSection:
      if (!scene->page_pending || message.title[0] == '\0' ||
          message.flags != 0 ||
          std::count_if(scene->rows.begin(), scene->rows.end(),
                        [](const RowModel &row) {
                          return row.kind == RowKind::kSection;
                        }) >= plugin_api::kMaxSections) {
        SetStatus(scene, "Plugin page exceeded the Host API limit");
        scene->session.Close();
        break;
      }
      scene->rows.push_back({RowKind::kSection, 0, 0, message.title,
                             message.text, nullptr});
      break;
    case plugin_api::Kind::kAddMetric:
      if (!scene->page_pending || message.request_id == 0 ||
          message.title[0] == '\0' || !MetricFlagsValid(message.flags) ||
          std::count_if(scene->rows.begin(), scene->rows.end(),
                        [](const RowModel &row) {
                          return row.kind == RowKind::kMetric;
                        }) >= plugin_api::kMaxMetrics ||
          std::any_of(scene->rows.begin(), scene->rows.end(),
                      [&](const RowModel &row) {
                        return row.kind == RowKind::kMetric &&
                               row.id == message.request_id;
                      })) {
        SetStatus(scene, "Plugin page exceeded the Host API limit");
        scene->session.Close();
        break;
      }
      scene->rows.push_back({RowKind::kMetric, message.request_id,
                             message.flags, message.title, message.text,
                             nullptr});
      break;
    case plugin_api::Kind::kUpdateMetric: {
      auto row = std::find_if(scene->rows.begin(), scene->rows.end(),
          [&](const RowModel &candidate) {
            return candidate.kind == RowKind::kMetric &&
                   candidate.id == message.request_id;
          });
      if (scene->page_pending || row == scene->rows.end() ||
          !MetricFlagsValid(message.flags)) {
        SetStatus(scene, "Plugin sent an invalid metric update");
        scene->session.Close();
        break;
      }
      row->value = message.text;
      row->flags = message.flags;
      if (row->value_label) {
        lv_label_set_text(row->value_label, row->value.c_str());
        lv_obj_set_style_text_color(row->value_label,
                                    MetricColor(row->flags), 0);
      }
      break;
    }
    case plugin_api::Kind::kSetBackAction:
      if (!scene->page_pending || message.request_id == 0 ||
          message.value != 0 || message.flags != 0 ||
          message.title[0] != '\0' || message.text[0] != '\0') {
        SetStatus(scene, "Plugin sent an invalid back action");
        scene->session.Close();
        break;
      }
      scene->pending_back_action = message.request_id;
      break;
    case plugin_api::Kind::kCommitPage:
      if (!scene->page_pending ||
          (scene->pending_back_action != 0 &&
           std::none_of(scene->buttons.begin(), scene->buttons.end(),
                        [scene](const ButtonModel &button) {
                          return button.id == scene->pending_back_action &&
                              !(button.flags & plugin_api::kDisabled);
                        }))) {
        SetStatus(scene, "Plugin sent an invalid Host API sequence");
        scene->session.Close();
        break;
      }
      scene->page_pending = false;
      if (scene->plugin.id == "mirror" &&
          std::any_of(scene->buttons.begin(), scene->buttons.end(),
                      [](const ButtonModel &button) { return button.id == 1; }) &&
          std::any_of(scene->buttons.begin(), scene->buttons.end(),
                      [](const ButtonModel &button) { return button.id == 2; })) {
        // Open the selected guide directly, including after Stop. Selecting
        // a mode never starts sharing; the guide retains its Start action.
        RequestPage(scene, scene->mirror_selection ? scene->mirror_selection :
            plugin_api::ActiveMirrorMode() == plugin_api::MirrorMode::kUsb ? 2 : 1);
        break;
      }
      SetBackAction(scene, scene->pending_back_action);
      RenderPage(scene);
      if (scene->progress)
        lv_bar_set_value(scene->progress, 100, LV_ANIM_ON);
      SetStatus(scene, "Connected through AERA Host API 2");
      break;
    case plugin_api::Kind::kSetStatus:
      SetStatus(scene, message.text);
      break;
    case plugin_api::Kind::kRequestOperation:
      RequestOperation(scene, message);
      break;
    case plugin_api::Kind::kClose:
      scene->session.Close();
      scene->process.Stop();
      scene->stopped = true;
      SetStatus(scene, "Plugin closed");
      break;
    default:
      SetStatus(scene, "Plugin sent an invalid Host API sequence");
      scene->session.Close();
      break;
  }
}
}  // namespace

void SetSelectedPluginId(const std::string &id) {
  std::lock_guard<std::mutex> lock(gSelectionMutex);
  gSelectedPlugin = id;
}

std::string GetSelectedPluginId() {
  std::lock_guard<std::mutex> lock(gSelectionMutex);
  return gSelectedPlugin;
}

void BuildGenericPluginScene(lv_obj_t *screen, const std::string &id,
                             ActionCallback callback, void *context) {
  auto *scene = new GenericScene;
  scene->screen = screen;
  if (!plugins::FindInstalled(id, scene->plugin) ||
      !plugins::IsGeneric(scene->plugin)) {
    Header(screen, "Plugin unavailable",
           "The selected app is missing or does not support Host API 2.",
           callback, context);
    Navigation(screen, Action::kBackHome, callback, context, true);
    delete scene;
    return;
  }
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<GenericScene *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, scene);
  Header(screen, scene->plugin.name.c_str(),
         scene->plugin.description.c_str(), callback, context);
  const bool landscape = Landscape(screen);
  const bool mirror = scene->plugin.id == "mirror";
  const int card_width = landscape ? lv_obj_get_width(screen) - 128 : 1312;
  const int top = landscape ? 320 : 430;
  if (mirror) {
    const int left_width = landscape ? (card_width - 20) / 2 : 630;
    MirrorModeCard(scene, 0, 64, top, left_width);
    MirrorModeCard(scene, 1, 64 + left_width + 20, top,
                   card_width - left_width - 20);
  }
  const int card_y = top + (mirror ? 260 : 0);
  const int card_height = lv_obj_get_height(screen) - card_y - 36;
  scene->card = lv_obj_create(screen);
  Panel(scene->card, 42, kMainPanel);
  lv_obj_add_event_cb(scene->card, [](lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_CANCEL) return;
    auto *scene = static_cast<GenericScene *>(lv_event_get_user_data(event));
    RequestPage(scene, scene->page_back_action);
  }, LV_EVENT_CANCEL, scene);
  lv_obj_set_pos(scene->card, 64, card_y);
  lv_obj_set_size(scene->card, card_width, card_height);
  scene->content = lv_obj_create(scene->card);
  Clear(scene->content);
  lv_obj_set_pos(scene->content, 24, 24);
  lv_obj_set_size(scene->content, card_width - 48, card_height - 132);
  lv_obj_add_flag(scene->content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->content, LV_DIR_VER);
  scene->page_title = "Preparing " + scene->plugin.name;
  scene->page_body = "Verifying the plugin runtime and expanding it into private RAM.";
  RenderPage(scene);
  scene->progress = lv_bar_create(scene->card);
  lv_obj_set_pos(scene->progress, 36, card_height - 80);
  lv_obj_set_size(scene->progress, card_width - 72, 10);
  lv_obj_set_style_bg_color(scene->progress, kAccent, LV_PART_INDICATOR);
  scene->status = Label(scene->card, "Checking signed runtime",
                        &lv_font_montserrat_24, kMuted);
  lv_obj_align(scene->status, LV_ALIGN_BOTTOM_LEFT, 36, -24);
  Navigation(screen, Action::kBackHome, callback, context, true);

  scene->worker = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, scene->plugin.id.c_str(),
                              scene->plugin.type.c_str(),
                              scene->plugin.entry.c_str());
  });
  scene->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *scene = static_cast<GenericScene *>(lv_timer_get_user_data(timer));
    if (!scene->preparation.done.load(std::memory_order_acquire)) {
      lv_bar_set_value(scene->progress, scene->preparation.progress.load(),
                       LV_ANIM_OFF);
      return;
    }
    if (!scene->preparation.verified) {
      SetStatus(scene, scene->preparation.error);
      return;
    }
    if (!scene->launched) {
      scene->launched = true;
      scene->launched_ms = NowMs();
      int control = -1;
      std::string error;
      if (!scene->process.Start(scene->preparation.directory, control, error) ||
          !scene->session.Adopt(control)) {
        scene->stopped = true;
        SetStatus(scene, error.empty() ? scene->session.Status() : error);
        return;
      }
      lv_bar_set_value(scene->progress, 100, LV_ANIM_ON);
    }
    for (const auto &message : scene->session.Poll())
      HandleMessage(scene, message);
    if (scene->plugin.id == "mirror" && !scene->page_pending &&
        NowMs() - scene->mirror_code_checked_ms >= 500) {
      scene->mirror_code_checked_ms = NowMs();
      const std::string code = plugin_api::ActiveMirrorMode() ==
          plugin_api::MirrorMode::kOff ? "" : aera::remote::AccessCode();
      if (code != scene->mirror_access_code) RenderPage(scene);
    }
    if (scene->session.Connected() && !scene->session.Negotiated() &&
        NowMs() - scene->launched_ms > 5000) {
      scene->session.Close();
      SetStatus(scene, "Plugin handshake timed out");
    }
    if (scene->operation_pending &&
        NowMs() - scene->operation_started_ms > 60000) {
      scene->session.Send(plugin_api::Kind::kOperationResult,
                          scene->operation_request, 0, 0, nullptr,
                          "Host operation approval timed out.");
      scene->operation_pending = false;
      scene->operation_request = 0;
    }
    if (scene->operation_pending && !ModalVisible(scene->screen)) {
      scene->session.Send(plugin_api::Kind::kOperationResult,
                          scene->operation_request, 0, 0, nullptr,
                          "Host operation cancelled by the user.");
      scene->operation_pending = false;
      scene->operation_request = 0;
      SetStatus(scene, "Operation cancelled");
    }
    if (!scene->stopped && !scene->process.Running()) {
      scene->stopped = true;
      scene->session.Close();
      SetStatus(scene, "Plugin process exited; its resources were reclaimed");
    }
  }, 16, scene);
}

}  // namespace aeraui
