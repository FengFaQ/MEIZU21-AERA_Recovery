/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <string>

#include <lvgl.h>

#include "aeraui/engine.hpp"
#include "aeraui/backend.hpp"
#include "plugins/plugin_manager.hpp"

namespace aeraui {

using ActionCallback = void (*)(Action action, void *context);

struct OperationScene {
  lv_obj_t *status = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *progress_pulse = nullptr;
  lv_obj_t *percent = nullptr;
  lv_obj_t *elapsed = nullptr;
  lv_obj_t *files = nullptr;
  lv_obj_t *activity_title = nullptr;
  lv_obj_t *activity_summary = nullptr;
  lv_obj_t *notice = nullptr;
  lv_obj_t *payload_list = nullptr;
  std::vector<lv_obj_t *> payload_cards, payload_names, payload_states, payload_bars;
  bool payload_parallel = false;
  lv_obj_t *steps[4] = {};
  lv_obj_t *step_icons[4] = {};
  lv_obj_t *step_titles[4] = {};
  lv_obj_t *step_details[4] = {};
  lv_obj_t *step_states[4] = {};
  lv_obj_t *log_overlay = nullptr;
  lv_obj_t *details = nullptr;
  lv_obj_t *done = nullptr;
  lv_obj_t *done_label = nullptr;
  lv_obj_t *log = nullptr;
  lv_obj_t *activity = nullptr;
  lv_obj_t *metrics = nullptr;
  lv_obj_t *metrics_panel = nullptr;
  lv_obj_t *activity_panel = nullptr;
  lv_obj_t *transfer_speed = nullptr;
  lv_obj_t *destination = nullptr;
  lv_obj_t *cancel = nullptr;
  lv_obj_t *installer_prompt = nullptr;
  lv_obj_t *installer_prompt_title = nullptr;
  lv_obj_t *installer_prompt_message = nullptr;
  lv_obj_t *installer_prompt_accept = nullptr;
  lv_obj_t *installer_prompt_decline = nullptr;
  uint32_t started = 0;
  unsigned installer_lines = 0;
  Job job = Job::kInstall;
  bool pc_restore = false;
  bool pc_app_backup = false;
  bool format_data = false;
  bool indeterminate_progress = false;
};

struct DecryptScene {
  lv_obj_t *status = nullptr;
  lv_obj_t *input = nullptr;
  lv_obj_t *submit = nullptr;
  void *state = nullptr;
};

struct UserDecryptRequest {
  AndroidUser user;
};

struct WifiScene {
  lv_obj_t *status = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *toggle = nullptr;
  lv_obj_t *refresh = nullptr;
  lv_obj_t *test = nullptr;
  lv_obj_t *activity = nullptr;
  void *state = nullptr;
};

struct PluginScene {
  lv_obj_t *status = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *refresh = nullptr;
  lv_obj_t *list = nullptr;
  void *state = nullptr;
};

struct NasScene {
  lv_obj_t *status = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *primary = nullptr;
  lv_obj_t *secondary = nullptr;
  void *state = nullptr;
};

struct UpdateScene {
  lv_obj_t *screen = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *installed = nullptr;
  lv_obj_t *release = nullptr;
  lv_obj_t *changelog = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *progress_value = nullptr;
  lv_obj_t *progress_amount = nullptr;
  lv_obj_t *stable = nullptr;
  lv_obj_t *nightly = nullptr;
  lv_obj_t *check = nullptr;
  lv_obj_t *install = nullptr;
};

void BuildBootScene(lv_obj_t *screen, ActionCallback callback, void *context);
void CompleteBootScene(lv_obj_t *screen);
void BuildHomeScene(lv_obj_t *screen, ActionCallback callback, void *context);
void BuildFilesScene(lv_obj_t *screen, ActionCallback callback, void *context);
void BuildToolScene(lv_obj_t *screen, Action tool, ActionCallback callback,
                    void *context);
void SetUserDecryptRequest(const UserDecryptRequest &request);
UserDecryptRequest GetUserDecryptRequest();
void BuildBrowserScene(lv_obj_t *screen, ActionCallback callback, void *context);
// Optional FDs are owned channels supplied only by a trusted isolated launcher.
// Engine currently uses the defaults: it cannot launch a browser.
void BuildWebScene(lv_obj_t *screen, ActionCallback callback, void *context,
                   int frame_fd = -1, int control_fd = -1,
                   bool auto_launch = true);
// Consumes both native touch slots while the browser viewport owns input,
// enabling pinch zoom without exposing recovery input devices to WebKit.
bool BrowserHandlePointer(int slot, int x, int y, bool pressed);
// Stops the persistent browser worker and releases its expanded RAM runtime.
// The engine calls this before LVGL destroys the active display.
void ShutdownWebRuntime();
void BuildRetroArchScene(lv_obj_t *screen, ActionCallback callback,
                         void *context);
void BuildDoomScene(lv_obj_t *screen, ActionCallback callback, void *context);
void BuildTelegramScene(lv_obj_t *screen, ActionCallback callback,
                        void *context);
void BuildGalleryScene(lv_obj_t *screen, ActionCallback callback,
                       void *context);
void BuildMediaScene(lv_obj_t *screen, ActionCallback callback,
                     void *context);
void BuildStreamsScene(lv_obj_t *screen, ActionCallback callback,
                       void *context);
void BuildRecorderScene(lv_obj_t *screen, ActionCallback callback,
                        void *context);
void BuildAppVaultScene(lv_obj_t *screen, ActionCallback callback,
                        void *context);
void SetSelectedPluginId(const std::string &id);
std::string GetSelectedPluginId();
void BuildGenericPluginScene(lv_obj_t *screen, const std::string &id,
                             ActionCallback callback, void *context);
void BuildRootManagerScene(lv_obj_t *screen, ActionCallback callback,
                           void *context);
void BuildAboutScene(lv_obj_t *screen, ActionCallback callback, void *context);
void BuildTerminalScene(lv_obj_t *screen, ActionCallback callback,
                        void *context);
void BuildSideloadScene(lv_obj_t *screen, ActionCallback callback,
                        void *context);
void BuildRebootScene(lv_obj_t *screen, ActionCallback callback, void *context);
void BuildFastbootScene(lv_obj_t *screen, ActionCallback callback,
                        void *context);
void PollFastbootTelemetry();
void BuildFastbootFormatScene(lv_obj_t *screen, ActionCallback callback,
                              void *context);
const char *GetSelectedPackagePath();
void SetSelectedPackagePath(const char *path);
OperationScene BuildOperationScene(lv_obj_t *screen, const char *path,
                                   ActionCallback callback, void *context);
void CompleteOperationScene(const OperationScene &scene, bool success,
                            const char *detail);
DecryptScene BuildDecryptScene(lv_obj_t *screen, int credential_type,
                               bool file_based, int user_id,
                               int pattern_grid_size,
                               ActionCallback callback, void *context,
                               const std::string &user_name = {});
std::string GetDecryptCredential(const DecryptScene &scene);
void SetDecryptBusy(const DecryptScene &scene);
void CompleteDecryptAttempt(const DecryptScene &scene, bool success);
void BuildPreparingScene(lv_obj_t *screen, bool decrypted);
lv_obj_t *BuildLockScene(lv_obj_t *parent, ActionCallback callback,
                         void *context, bool preview = false,
                         const LockTheme *theme = nullptr, bool ambient = false);
void SetJobRequest(const JobRequest &request);
JobRequest GetJobRequest();
OperationScene BuildJobScene(lv_obj_t *screen, const JobRequest &request,
                             ActionCallback callback, void *context);
void RefreshOperationScene(const OperationScene &scene);
bool NavigateFileBack();
WifiScene BuildWifiScene(lv_obj_t *screen, ActionCallback callback,
                         void *context);
void SetWifiRequest(const WifiRequest &request);
WifiRequest GetWifiRequest();
void SetWifiBusy(const WifiScene &scene, const WifiRequest &request);
void CompleteWifiOperation(const WifiScene &scene, bool success);
NasScene BuildNasScene(lv_obj_t *screen, ActionCallback callback,
                       void *context);
NasRequest GetNasRequest();
void SetNasBusy(const NasScene &scene, const NasRequest &request);
void CompleteNasOperation(const NasScene &scene, bool success);
PluginScene BuildPluginScene(lv_obj_t *screen, ActionCallback callback,
                             void *context);
plugins::Request GetPluginRequest();
void SetPluginRequest(const plugins::Request &request);
void RefreshPluginScene(const PluginScene &scene);
bool NavigatePluginBack(const PluginScene &scene);
void SetPluginBusy(const PluginScene &scene, const plugins::Request &request);
void UpdatePluginProgress(const PluginScene &scene, unsigned value,
                          uint64_t downloaded_bytes, uint64_t total_bytes,
                          unsigned completed_plugins = 0, unsigned total_plugins = 0);
void CompletePluginOperation(const PluginScene &scene, bool success,
                             const char *message);
UpdateScene BuildUpdateScene(lv_obj_t *screen, ActionCallback callback,
                             void *context);
void RefreshUpdateScene(const UpdateScene &scene);

}  // namespace aeraui
