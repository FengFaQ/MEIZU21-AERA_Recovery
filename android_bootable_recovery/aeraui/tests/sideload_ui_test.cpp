#include "scene.hpp"
#include "ui_components.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace aeraui;

namespace {
SideloadStatus g_status;
std::string g_installer_status;

void Tick(int count = 40) {
  for (int i = 0; i < count; ++i) {
    lv_tick_inc(16);
    lv_timer_handler();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

lv_obj_t *Find(lv_obj_t *root, const char *text) {
  if (lv_obj_check_type(root, &lv_label_class) &&
      !strcmp(lv_label_get_text(root), text)) {
    return lv_obj_get_parent(root);
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
    if (auto *found = Find(lv_obj_get_child(root, i), text)) return found;
  }
  return nullptr;
}
}  // namespace

namespace aeraui {
void AttachStatusBar(lv_obj_t *, void (*)(Action, void *), void *,
                     StatusBarAction, bool) {}
int32_t StatusBarHeight() { return 165; }
DockLayout RecoveryDockLayout() { return DockLayout::kGlass; }
bool RecoveryTintedIconBackgrounds() { return false; }
int RecoveryDockTransparency() { return 60; }
int RecoveryDockBlur() { return 24; }
bool RecoveryDockHideInApps() { return false; }
int RecoveryProgress() { return 0; }
std::string RecoveryOperationDetail() { return {}; }
std::string RecoveryInstallerStatus() { return g_installer_status; }
InstallerPresentation RecoveryInstallerPresentation() { return {}; }
InstallerPrompt RecoveryInstallerPrompt() { return {}; }
bool RecoveryAnswerInstallerPrompt(bool) { return true; }
SideloadStatus RecoverySideloadStatus() { return g_status; }
void RecoveryVibrate(Haptic) {}
}  // namespace aeraui

int main() {
  lv_init();
  std::vector<uint8_t> frame(1440 * 3168 * 4);
  auto *display = lv_display_create(1440, 3168);
  lv_display_set_buffers(display, frame.data(), nullptr, frame.size(),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display,
      [](lv_display_t *value, const lv_area_t *, uint8_t *) {
        lv_display_flush_ready(value);
      });

  auto *home = lv_screen_active();
  auto *sideload = lv_obj_create(nullptr);
  BuildSideloadScene(sideload, [](Action, void *) {}, nullptr);
  lv_screen_load(sideload);
  Tick();
  assert(Find(sideload, "Start ADB sideload"));
  assert(Find(sideload, "adb sideload package.zip"));
  lv_screen_load(home);
  lv_obj_delete(sideload);

  g_status = {true, false, 32ULL * 1024 * 1024, 64ULL * 1024 * 1024};
  g_installer_status = "Reading test package";
  JobRequest request;
  request.job = Job::kSideload;
  request.title = "ADB Sideload";
  auto *operation = lv_obj_create(nullptr);
  auto scene = BuildJobScene(operation, request, [](Action, void *) {}, nullptr);
  lv_screen_load(operation);
  RefreshOperationScene(scene);
  Tick();
  assert(Find(operation, "50%"));
  assert(Find(operation, "32.0 MB / 64.0 MB"));
  assert(Find(operation, "Reading test package"));

  g_status.cancel_requested = true;
  RefreshOperationScene(scene);
  Tick();
  assert(Find(operation, "Cancelling sideload"));
  assert(Find(operation, "Cancelling..."));

  lv_screen_load(home);
  lv_obj_delete(operation);
  lv_deinit();
  return 0;
}
