/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"

#include "recorder/service.hpp"
#include "ui_components.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace aeraui {
namespace {
using namespace widgets;

struct RecorderScene {
  lv_obj_t *status = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *timer = nullptr;
  lv_obj_t *frames = nullptr;
  lv_obj_t *record = nullptr;
  lv_obj_t *record_label = nullptr;
  lv_obj_t *dot = nullptr;
  lv_obj_t *quality720 = nullptr;
  lv_obj_t *quality1080 = nullptr;
  lv_obj_t *fps30 = nullptr;
  lv_obj_t *fps60 = nullptr;
  lv_timer_t *refresh = nullptr;
};

std::string Duration(uint64_t milliseconds) {
  const uint64_t seconds = milliseconds / 1000;
  char value[32];
  snprintf(value, sizeof(value), "%02llu:%02llu",
           static_cast<unsigned long long>(seconds / 60),
           static_cast<unsigned long long>(seconds % 60));
  return value;
}

void Select(lv_obj_t *button, bool selected) {
  if (!button) return;
  lv_obj_set_style_bg_color(button, selected ? kAccentSoft : kMainPanel, 0);
  lv_obj_set_style_border_color(button, selected ? kAccent : kMainLine, 0);
  lv_obj_set_style_border_opa(button, selected ? LV_OPA_80 : LV_OPA_40, 0);
}

void RefreshRecorder(RecorderScene *scene) {
  if (!scene) return;
  recorder::Poll();
  const auto snapshot = recorder::GetSnapshot();
  const bool active = snapshot.state == recorder::State::kPreparing ||
      snapshot.state == recorder::State::kStarting ||
      snapshot.state == recorder::State::kRecording ||
      snapshot.state == recorder::State::kFinalizing;
  const bool recording = snapshot.state == recorder::State::kRecording;
  i18n::BindLabel(scene->status, snapshot.status.c_str());
  const std::string dimensions = snapshot.width && snapshot.height
      ? i18n::Format("%d × %d  •  %d FPS", snapshot.width, snapshot.height,
                     snapshot.fps)
      : "Screen capture stays outside the 100 MB recovery image.";
  i18n::BindLabel(scene->detail, dimensions.c_str());
  i18n::BindLabel(scene->timer, Duration(snapshot.elapsed_ms).c_str());
  const std::string metrics = i18n::Format(
      "%llu frames  •  %llu dropped",
      static_cast<unsigned long long>(snapshot.frames),
      static_cast<unsigned long long>(snapshot.dropped));
  i18n::BindLabel(scene->frames, metrics.c_str());
  i18n::BindLabel(scene->record_label,
                    active ? (snapshot.state == recorder::State::kFinalizing
                        ? "Finalizing…" : "Stop recording") : "Start recording");
  lv_obj_set_style_bg_color(scene->record,
      recording ? kRedSoft : kAccent, 0);
  lv_obj_set_style_text_color(scene->record_label,
      recording ? kRed : lv_color_black(), 0);
  if (recording) lv_obj_remove_flag(scene->dot, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(scene->dot, LV_OBJ_FLAG_HIDDEN);
  for (auto *choice : {scene->quality720, scene->quality1080,
                       scene->fps30, scene->fps60})
    if (choice) lv_obj_set_style_opa(choice, active ? LV_OPA_40 : LV_OPA_COVER, 0);
  Select(scene->quality720, recorder::ProfileShortEdge() == 720);
  Select(scene->quality1080, recorder::ProfileShortEdge() == 1080);
  Select(scene->fps30, recorder::ProfileFps() == 30);
  Select(scene->fps60, recorder::ProfileFps() == 60);
}

template <typename Handler>
lv_obj_t *Choice(lv_obj_t *parent, int x, int y, int width, const char *title,
                 Handler action) {
  auto *button = lv_button_create(parent);
  Panel(button, 34, kMainPanel);
  Interactive(button, kMainSelected);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, 112);
  lv_obj_set_style_border_width(button, 2, 0);
  lv_obj_set_style_border_color(button, kMainLine, 0);
  auto *label = Label(button, title, &lv_font_montserrat_32, kText);
  lv_obj_center(label);
  OnClick(button, action);
  return button;
}

}  // namespace

void BuildRecorderScene(lv_obj_t *screen, ActionCallback callback,
                        void *context) {
  Header(screen, "AERA Recorder", "Capture recovery without slowing it down.",
         callback, context);
  const bool landscape = Landscape(screen);
  auto *scene = new RecorderScene;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    auto *scene = static_cast<RecorderScene *>(lv_event_get_user_data(event));
    if (scene->refresh) lv_timer_delete(scene->refresh);
    delete scene;
  }, LV_EVENT_DELETE, scene);

  auto *hero = lv_obj_create(screen);
  Panel(hero, 52, kMainSheet);
  lv_obj_set_pos(hero, 64, landscape ? 330 : 470);
  lv_obj_set_size(hero, landscape ? 1510 : 1312,
                  landscape ? 610 : 720);
  lv_obj_set_style_border_width(hero, 1, 0);
  lv_obj_set_style_border_color(hero, kMainLine, 0);
  lv_obj_set_style_border_opa(hero, LV_OPA_50, 0);

  auto *camera = IconPlate(hero, LV_SYMBOL_VIDEO, kAccent, kMainPanel, 126);
  lv_obj_set_pos(camera, 48, 46);
  scene->dot = lv_obj_create(camera);
  Clear(scene->dot);
  lv_obj_set_size(scene->dot, 28, 28);
  lv_obj_align(scene->dot, LV_ALIGN_TOP_RIGHT, 4, -4);
  lv_obj_set_style_radius(scene->dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(scene->dot, kRed, 0);
  lv_obj_set_style_bg_opa(scene->dot, LV_OPA_COVER, 0);
  lv_obj_add_flag(scene->dot, LV_OBJ_FLAG_HIDDEN);

  scene->status = Label(hero, "Ready to record", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(scene->status, 210, 54);
  lv_obj_set_width(scene->status, landscape ? 1180 : 1000);
  scene->detail = Label(hero, "", &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(scene->detail, 212, 126);
  scene->timer = Label(hero, "00:00", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(scene->timer, 50, 250);
  scene->frames = Label(hero, "0 frames  •  0 dropped",
                        &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(scene->frames, 52, 326);

  scene->record = lv_button_create(hero);
  Panel(scene->record, 58, kAccent);
  Interactive(scene->record, kAccentPressed);
  lv_obj_set_pos(scene->record, 48, landscape ? 440 : 500);
  lv_obj_set_size(scene->record, landscape ? 1414 : 1216, 126);
  scene->record_label = Label(scene->record, "Start recording",
                              &lv_font_montserrat_32, lv_color_black());
  lv_obj_center(scene->record_label);
  OnClick(scene->record, [=] {
    if (recorder::Active()) recorder::Stop();
    else recorder::Start(static_cast<uint32_t>(lv_obj_get_width(screen)),
                         static_cast<uint32_t>(lv_obj_get_height(screen)));
    RefreshRecorder(scene);
  });

  auto *settings = lv_obj_create(screen);
  Panel(settings, 46, kMainSheet);
  lv_obj_set_pos(settings, landscape ? 1610 : 64,
                 landscape ? 330 : 1230);
  lv_obj_set_size(settings, landscape ? 1430 : 1312,
                  landscape ? 610 : 780);
  auto *title = Label(settings, "Recording quality", &lv_font_montserrat_36, kText);
  lv_obj_set_pos(title, 42, 38);
  auto *hint = Label(settings,
      "720p is smoother. 1080p keeps fine text sharper but uses more CPU and storage.",
      &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 42, 100);
  lv_obj_set_width(hint, landscape ? 1320 : 1200);
  const int choice_width = landscape ? 650 : 584;
  scene->quality720 = Choice(settings, 42, 190, choice_width, "720p", [scene] {
    recorder::SetProfile(720, recorder::ProfileFps()); RefreshRecorder(scene);
  });
  scene->quality1080 = Choice(settings, landscape ? 738 : 668, 190,
                              choice_width, "1080p", [scene] {
    recorder::SetProfile(1080, recorder::ProfileFps()); RefreshRecorder(scene);
  });
  scene->fps30 = Choice(settings, 42, 340, choice_width, "30 FPS", [scene] {
    recorder::SetProfile(recorder::ProfileShortEdge(), 30); RefreshRecorder(scene);
  });
  scene->fps60 = Choice(settings, landscape ? 738 : 668, 340,
                        choice_width, "60 FPS", [scene] {
    recorder::SetProfile(recorder::ProfileShortEdge(), 60); RefreshRecorder(scene);
  });
  auto *privacy = Label(settings,
      LV_SYMBOL_WARNING "  Recordings may contain private recovery information.",
      &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(privacy, 44, 508);

  auto *location = Label(screen,
      LV_SYMBOL_DIRECTORY "  /sdcard/AERA/Recordings",
      &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(location, 80, landscape ? 1010 : 2070);
  AnimateEnter(hero, 20, 12);
  AnimateEnter(settings, 55, 12);
  Navigation(screen, Action::kBackHome, callback, context, true);

  scene->refresh = lv_timer_create([](lv_timer_t *timer) {
    RefreshRecorder(static_cast<RecorderScene *>(lv_timer_get_user_data(timer)));
  }, 120, scene);
  RefreshRecorder(scene);
}

}  // namespace aeraui
