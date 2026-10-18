/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "root_manager.hpp"
#include "ui_components.hpp"

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

struct RootUi {
  lv_obj_t *screen = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *hero_status = nullptr;
  lv_obj_t *hero_detail = nullptr;
  lv_obj_t *patch_status = nullptr;
  lv_obj_t *patch_detail = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *patch_button = nullptr;
  lv_obj_t *refresh_release_button = nullptr;
  lv_obj_t *rollback_button = nullptr;
  lv_obj_t *manager_status = nullptr;
  lv_obj_t *manager_detail = nullptr;
  lv_obj_t *manager_progress = nullptr;
  lv_obj_t *manager_button = nullptr;
  lv_obj_t *module_list = nullptr;
  lv_obj_t *module_summary = nullptr;
  lv_obj_t *provider_buttons[3] = {};
  lv_obj_t *slot_buttons[2] = {};
  lv_timer_t *timer = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  root::Provider provider = root::Provider::kKernelSU;
  root::Job active_job = root::Job::kRefreshRelease;
  std::string slot = "a";
  root::Status device;
  root::PatchInfo patch;
  std::vector<root::Module> modules;
  root::Progress work;
  std::thread worker;
  std::atomic<bool> preparing{true};
  std::atomic<bool> busy{false};
  std::atomic<bool> done{false};
  std::atomic<bool> success{false};
  std::mutex data_mutex;
};

void RefreshUi(RootUi *state);

void StyleRootButton(lv_obj_t *button, bool danger = false) {
  if (!button) return;
  lv_obj_set_style_radius(button, 24, 0);
  lv_obj_set_style_bg_color(button, kMainPanel, 0);
  lv_obj_set_style_bg_color(button, kMainSelected, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, kMainLine, 0);
  lv_obj_set_style_border_opa(button, LV_OPA_30, 0);
  if (lv_obj_get_child_count(button))
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                danger ? kRed : kText, 0);
}

void SetSelected(lv_obj_t *button, bool selected) {
  if (!button) return;
  lv_obj_set_style_bg_color(button, kMainPanel, 0);
  lv_obj_set_style_border_width(button, selected ? 3 : 1, 0);
  lv_obj_set_style_border_color(button, selected ? kAccent : kMainLine, 0);
  lv_obj_set_style_border_opa(button, selected ? LV_OPA_COVER : LV_OPA_30, 0);
  if (lv_obj_get_child_count(button)) {
    auto *label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_color(label, kText, 0);
  }
}

std::string ProgressText(root::Progress &progress, bool detail) {
  std::lock_guard<std::mutex> lock(progress.text_mutex);
  return detail ? progress.detail : progress.status;
}

void SetBusy(RootUi *state, bool busy) {
  state->busy.store(busy);
  for (auto *button : state->provider_buttons)
    if (button) busy ? lv_obj_add_state(button, LV_STATE_DISABLED)
                     : lv_obj_remove_state(button, LV_STATE_DISABLED);
  for (auto *button : state->slot_buttons)
    if (button) busy ? lv_obj_add_state(button, LV_STATE_DISABLED)
                     : lv_obj_remove_state(button, LV_STATE_DISABLED);
  if (state->patch_button)
    busy ? lv_obj_add_state(state->patch_button, LV_STATE_DISABLED)
         : lv_obj_remove_state(state->patch_button, LV_STATE_DISABLED);
  if (state->refresh_release_button)
    busy ? lv_obj_add_state(state->refresh_release_button, LV_STATE_DISABLED)
         : lv_obj_remove_state(state->refresh_release_button, LV_STATE_DISABLED);
  if (state->rollback_button)
    busy ? lv_obj_add_state(state->rollback_button, LV_STATE_DISABLED)
         : lv_obj_remove_state(state->rollback_button, LV_STATE_DISABLED);
  if (state->manager_button)
    busy ? lv_obj_add_state(state->manager_button, LV_STATE_DISABLED)
         : lv_obj_remove_state(state->manager_button, LV_STATE_DISABLED);
}

void LoadSnapshot(RootUi *state) {
  root::Status device = root::Probe();
  const root::Provider providers[] = {root::Provider::kKernelSU,
      root::Provider::kKernelSUNext, root::Provider::kSukiSU};
  for (root::Provider provider : providers)
    root::BundledRelease(provider, device.kmi);
  const std::string slot = state->slot.empty() ? device.slot : state->slot;
  root::PatchInfo patch = root::InspectSlot(slot);
  auto modules = root::InstalledModules();
  std::lock_guard<std::mutex> lock(state->data_mutex);
  state->device = std::move(device);
  state->patch = std::move(patch);
  state->modules = std::move(modules);
  if (state->slot.empty()) state->slot = slot;
}

void Start(RootUi *state, const root::Request &request) {
  if (!state || state->busy.load() || state->preparing.load()) return;
  if (state->worker.joinable()) state->worker.join();
  state->work.value.store(0);
  state->work.downloaded.store(0);
  state->work.total.store(0);
  state->work.cancel.store(false);
  state->done.store(false);
  SetBusy(state, true);
  state->active_job = request.job;
  const bool manager = request.job == root::Job::kInstallManager ||
                       request.job == root::Job::kUninstallManager;
  lv_obj_t *bar = manager ? state->manager_progress : state->progress;
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);
  i18n::BindLabel(manager ? state->manager_status : state->patch_status,
                  "Starting…");
  i18n::BindLabel(manager ? state->manager_detail : state->patch_detail,
                  "AERA is preparing the root operation.");
  state->worker = std::thread([state, request] {
    const bool success = root::Run(request, state->work);
    LoadSnapshot(state);
    state->success.store(success);
    state->done.store(true, std::memory_order_release);
  });
}

void ConfirmPatch(RootUi *state) {
  if (!state->device.kmi_supported || !state->device.init_boot_available ||
      !state->device.storage_ready) {
    Sheet(state->screen, "Cannot patch this kernel",
          state->device.error.empty() ? "The kernel compatibility check failed."
                                      : state->device.error);
    return;
  }
  const auto release = root::CachedRelease(state->provider);
  std::string text = i18n::Format(
      "Provider\n%s\n\nExact kernel interface\n%s\n\nTarget\n"
      "init_boot_%s\n\nAERA will verify the exact matching module, create "
      "a full rollback image, let ksud patch init_boot, verify it, and only "
      "then write the selected slot.",
      root::ProviderName(state->provider), state->device.kmi.c_str(),
      state->slot.c_str());
  if (release.available) {
    text += i18n::Format("\n\nSelected release\n%s • %s",
                         release.version.c_str(), release.asset_name.c_str());
    text += "\n";
    text += i18n::Translate(
        release.bundled ? "Bundled in recovery — no download required."
                        : "Online release — the verified asset will be downloaded.");
  }
  Sheet(state->screen,
        i18n::Format("Patch init_boot_%s?", state->slot.c_str()), text,
        [state] {
          root::Request request;
          request.job = root::Job::kPatch;
          request.provider = state->provider;
          request.slot = state->slot;
          Start(state, request);
        });
}

void ConfirmRollback(RootUi *state) {
  Sheet(state->screen,
        i18n::Format("Restore init_boot_%s?", state->slot.c_str()),
        "AERA will restore the newest verified full partition backup for this "
        "slot and verify the partition readback before reporting success.",
        [state] {
          root::Request request;
          request.job = root::Job::kRollback;
          request.slot = state->slot;
          Start(state, request);
        });
}

void ConfirmManagerAction(RootUi *state) {
  const root::ManagerStatus manager = root::InspectManager(state->provider);
  if (manager.removal_pending || manager.staged) return;
  if (manager.installed) {
    Sheet(state->screen, "Uninstall manager app?",
          std::string("AERA will schedule ") +
              root::ProviderName(state->provider) +
              " for removal the next time Android boots.",
          [state] {
            root::Request request;
            request.job = root::Job::kUninstallManager;
            request.provider = state->provider;
            Start(state, request);
          });
    return;
  }
  Sheet(state->screen, "Install manager app?",
        std::string("AERA will download and verify the latest official ") +
            root::ProviderName(state->provider) +
            " manager APK, then make it available when Android boots.",
        [state] {
          root::Request request;
          request.job = root::Job::kInstallManager;
          request.provider = state->provider;
          Start(state, request);
        });
}

void ModuleAction(RootUi *state, const root::Module &module, root::Job job) {
  std::string title;
  std::string detail;
  if (job == root::Job::kUpdateModule) {
    title = i18n::Format("Update %s?", module.name.c_str());
    detail = i18n::Format(
        "%s → %s\n\nThe module's own updateJson release URL will be "
        "downloaded, its archive layout checked, then installed by ksud.",
        module.version.c_str(), module.latest_version.c_str());
  } else if (job == root::Job::kRemoveModule) {
    title = i18n::Format("Remove %s?", module.name.c_str());
    detail = "ksud will mark this module for removal. The change completes on reboot.";
  } else {
    root::Request request;
    request.job = job; request.module_id = module.id;
    Start(state, request); return;
  }
  Sheet(state->screen, title, detail, [state, module, job] {
    root::Request request;
    request.job = job; request.module_id = module.id;
    Start(state, request);
  });
}

void RenderModules(RootUi *state) {
  if (!state->module_list) return;
  lv_obj_clean(state->module_list);
  const auto modules = state->modules;
  const size_t updates = std::count_if(modules.begin(), modules.end(),
      [](const root::Module &module) { return module.update_available; });
  std::string summary = modules.size() == 1
      ? i18n::Format("%zu installed module", modules.size())
      : i18n::Format("%zu installed modules", modules.size());
  if (updates)
    summary = i18n::Format("%s • %zu updates", summary.c_str(), updates);
  i18n::BindLabel(state->module_summary, summary.c_str());
  int y = 0;
  for (const auto &module : modules) {
    auto *card = lv_obj_create(state->module_list);
    Panel(card, 30, kMainSheet);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, 1270, module.update_available ? 244 : 196);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, module.update_available ? kAccent : kMainLine, 0);
    lv_obj_set_style_border_opa(card, module.update_available ? LV_OPA_60 : LV_OPA_30, 0);

    auto *icon_plate = lv_obj_create(card);
    Panel(icon_plate, 20, module.enabled ? kAccentSoft : kMainPanel);
    lv_obj_set_pos(icon_plate, 24, 24);
    lv_obj_set_size(icon_plate, 72, 72);
    auto *icon = Label(icon_plate, module.enabled ? LV_SYMBOL_OK : LV_SYMBOL_MINUS,
                       &lv_font_montserrat_24, module.enabled ? kAccent : kMuted);
    lv_obj_center(icon);
    auto *name = Label(card, module.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 120, 22);
    SingleLineLabel(name, 720, &lv_font_montserrat_32);
    std::string version = module.version;
    if (!module.author.empty()) version += " • " + module.author;
    auto *meta = Label(card, version.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(meta, 120, 76);
    SingleLineLabel(meta, 720, &lv_font_montserrat_24);

    auto *toggle = Button(card, module.enabled ? "Disable" : "Enable",
        [state, module] {
          ModuleAction(state, module, module.enabled ? root::Job::kDisableModule
                                                     : root::Job::kEnableModule);
        });
    StyleRootButton(toggle);
    lv_obj_set_pos(toggle, 860, 24);
    lv_obj_set_size(toggle, 190, 82);
    auto *remove = Button(card, LV_SYMBOL_TRASH,
        [state, module] { ModuleAction(state, module, root::Job::kRemoveModule); });
    StyleRootButton(remove, true);
    lv_obj_set_pos(remove, 1070, 24);
    lv_obj_set_size(remove, 150, 82);
    if (module.update_available) {
      const std::string update_text = std::string(LV_SYMBOL_DOWNLOAD) + "  " +
          i18n::Format("Update to %s", module.latest_version.c_str());
      auto *update = Button(card, update_text.c_str(),
          [state, module] { ModuleAction(state, module, root::Job::kUpdateModule); });
      StyleRootButton(update);
      lv_obj_set_pos(update, 24, 126);
      lv_obj_set_size(update, 1196, 90);
    } else {
      auto *description = Label(card,
          module.description.empty() ? module.id.c_str() : module.description.c_str(),
          &lv_font_montserrat_24, kMutedStrong);
      lv_obj_set_pos(description, 24, 126);
      SingleLineLabel(description, 1196, &lv_font_montserrat_24);
    }
    y += module.update_available ? 262 : 214;
  }
  if (modules.empty()) {
    auto *empty = Label(state->module_list,
        state->device.storage_ready
            ? "No KernelSU modules are installed."
            : "Unlock data to inspect installed modules.",
        &lv_font_montserrat_32, kMuted);
    lv_obj_set_pos(empty, 24, 50);
  }
  lv_obj_set_height(state->module_list, std::max(y, 196));
}

void RefreshUi(RootUi *state) {
  const bool supported = state->device.ksud_available && state->device.kmi_supported &&
                         state->device.init_boot_available;
  const std::string status = supported ? "Kernel compatible" : "Patching unavailable";
  i18n::BindLabel(state->hero_status, status.c_str());
  std::string detail = state->device.kernel_release;
  if (!state->device.kmi.empty())
    detail += i18n::Format("\nExact KMI: %s", state->device.kmi.c_str());
  detail += i18n::Format(" • active slot _%s", state->device.slot.c_str());
  if (!state->device.storage_ready) {
    detail += "\n";
    detail += i18n::Translate("Internal storage is locked");
  }
  i18n::BindLabel(state->hero_detail, detail.c_str());

  for (size_t i = 0; i < 3; ++i)
    SetSelected(state->provider_buttons[i], static_cast<size_t>(state->provider) == i);
  SetSelected(state->slot_buttons[0], state->slot == "a");
  SetSelected(state->slot_buttons[1], state->slot == "b");

  std::string patch_status = i18n::Format(
      state->patch.patched ? "init_boot_%s is patched"
                           : "init_boot_%s is stock",
      state->slot.c_str());
  i18n::BindLabel(state->patch_status, patch_status.c_str());
  std::string patch_detail = state->patch.detail;
  const auto release = root::CachedRelease(state->provider);
  if (release.available && release.kmi == state->device.kmi) {
    if (!patch_detail.empty()) patch_detail += "\n";
    patch_detail += i18n::Format(
        release.bundled ? "%s %s • bundled and ready offline"
                        : "%s %s • online release selected",
        root::ProviderName(state->provider), release.version.c_str());
  }
  i18n::BindLabel(state->patch_detail, patch_detail.c_str());
  const bool update_available =
      root::PatchUpdateAvailable(state->patch, state->provider, release);
  const char *patch_action = !state->patch.patched ? "Verify & patch" :
      update_available ? "Update" :
      state->patch.aera_verified &&
              state->patch.provider == root::ProviderName(state->provider)
          ? "Up to date" : "Already patched";
  i18n::BindLabel(lv_obj_get_child(state->patch_button, 0), patch_action);
  FitButtonLabel(state->patch_button);
  if (!supported || !state->device.storage_ready ||
      (state->patch.patched && !update_available))
    lv_obj_add_state(state->patch_button, LV_STATE_DISABLED);
  else if (!state->busy.load())
    lv_obj_remove_state(state->patch_button, LV_STATE_DISABLED);
  if ((!state->patch.patched && !state->patch.aera_verified) || !state->device.storage_ready)
    lv_obj_add_state(state->rollback_button, LV_STATE_DISABLED);
  else if (!state->busy.load())
    lv_obj_remove_state(state->rollback_button, LV_STATE_DISABLED);

  const root::ManagerStatus manager = root::InspectManager(state->provider);
  i18n::BindLabel(state->manager_status,
      manager.removal_pending ? "Manager removal ready" :
      manager.installed ? "Manager installed" :
      manager.staged ? "Manager ready for Android" : "Manager app missing");
  i18n::BindLabel(state->manager_detail, manager.detail.c_str());
  i18n::BindLabel(lv_obj_get_child(state->manager_button, 0),
      manager.removal_pending ? "Pending reboot" :
      manager.installed ? "Uninstall" :
      manager.staged ? "Ready on next boot" : "Download & install");
  StyleRootButton(state->manager_button,
                  manager.installed && !manager.removal_pending);
  FitButtonLabel(state->manager_button);
  if (manager.removal_pending || manager.staged ||
      !state->device.storage_ready)
    lv_obj_add_state(state->manager_button, LV_STATE_DISABLED);
  else if (!state->busy.load())
    lv_obj_remove_state(state->manager_button, LV_STATE_DISABLED);
  RenderModules(state);
}

void Poll(RootUi *state) {
  if (!state) return;
  if (state->preparing.load(std::memory_order_acquire) &&
      state->done.exchange(false, std::memory_order_acq_rel)) {
    if (state->worker.joinable()) state->worker.join();
    state->preparing.store(false);
    lv_obj_add_flag(state->progress, LV_OBJ_FLAG_HIDDEN);
    RefreshUi(state);
    return;
  }
  if (state->busy.load()) {
    const bool manager = state->active_job == root::Job::kInstallManager ||
                         state->active_job == root::Job::kUninstallManager;
    lv_obj_t *bar = manager ? state->manager_progress : state->progress;
    lv_bar_set_value(bar, static_cast<int>(state->work.value.load()), LV_ANIM_ON);
    const std::string status = ProgressText(state->work, false);
    const std::string detail = ProgressText(state->work, true);
    if (!status.empty())
      i18n::BindLabel(manager ? state->manager_status : state->patch_status,
                      status.c_str());
    if (!detail.empty())
      i18n::BindLabel(manager ? state->manager_detail : state->patch_detail,
                      detail.c_str());
  }
  if (state->busy.load() && state->done.exchange(false, std::memory_order_acq_rel)) {
    if (state->worker.joinable()) state->worker.join();
    SetBusy(state, false);
    lv_bar_set_value((state->active_job == root::Job::kInstallManager ||
                      state->active_job == root::Job::kUninstallManager)
                         ? state->manager_progress : state->progress,
                     static_cast<int>(state->work.value.load()), LV_ANIM_ON);
    RefreshUi(state);
    const std::string status = ProgressText(state->work, false);
    const std::string detail = ProgressText(state->work, true);
    Sheet(state->screen, state->success.load() ? status : "Root operation failed",
          detail.empty() ? status : detail);
  }
}

void Destroy(RootUi *state) {
  if (!state) return;
  if (state->timer) lv_timer_delete(state->timer);
  state->work.cancel.store(true);
  if (state->worker.joinable()) state->worker.join();
  delete state;
}

}  // namespace

void BuildRootManagerScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *state = new RootUi;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  Clear(screen);
  MainBackground(screen);
  Header(screen, "Root Manager", "Kernel patching and module control, powered by ksud.",
         callback, context);
  const bool landscape = Landscape(screen);
  state->list = Scroll(screen, landscape ? 340 : 452, landscape ? 1040 : 2320);

  auto *hero = lv_obj_create(state->list);
  Panel(hero, 38, kMainSheet);
  lv_obj_set_pos(hero, 0, 0);
  lv_obj_set_size(hero, 1312, 236);
  lv_obj_set_style_border_width(hero, 1, 0);
  lv_obj_set_style_border_color(hero, kAccent, 0);
  lv_obj_set_style_border_opa(hero, LV_OPA_40, 0);
  auto *plate = lv_obj_create(hero);
  Panel(plate, 26, IconBackground());
  lv_obj_set_pos(plate, 32, 34);
  lv_obj_set_size(plate, 104, 104);
  auto *shield = Label(plate, LV_SYMBOL_SETTINGS, &lv_font_montserrat_48, kAccent);
  lv_obj_center(shield);
  state->hero_status = Label(hero, "Inspecting kernel…", &lv_font_montserrat_36, kText);
  lv_obj_set_pos(state->hero_status, 168, 32);
  state->hero_detail = Label(hero, "Reading KMI and init_boot safely.",
                             &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(state->hero_detail, 168, 96);
  lv_obj_set_width(state->hero_detail, 1080);

  auto *provider_title = Label(state->list, "ROOT PROVIDER", &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(provider_title, 18, 278);
  const root::Provider providers[] = {root::Provider::kKernelSU,
      root::Provider::kKernelSUNext, root::Provider::kSukiSU};
  for (size_t i = 0; i < 3; ++i) {
    state->provider_buttons[i] = Button(state->list, root::ProviderName(providers[i]),
        [state, provider = providers[i]] {
          if (state->busy.load()) return;
          state->provider = provider;
          root::BundledRelease(provider, state->device.kmi);
          RefreshUi(state);
        });
    lv_obj_set_pos(state->provider_buttons[i], static_cast<int>(i) * 432, 320);
    lv_obj_set_size(state->provider_buttons[i], 412, 104);
    StyleRootButton(state->provider_buttons[i]);
  }

  auto *slot_title = Label(state->list, "TARGET SLOT", &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(slot_title, 18, 462);
  for (size_t i = 0; i < 2; ++i) {
    const std::string slot(1, i ? 'b' : 'a');
    state->slot_buttons[i] = Button(state->list, ("init_boot_" + slot).c_str(),
        [state, slot] {
          if (state->busy.load()) return;
          state->slot = slot;
          state->patch = root::InspectSlot(slot);
          RefreshUi(state);
        });
    lv_obj_set_pos(state->slot_buttons[i], static_cast<int>(i) * 652, 504);
    lv_obj_set_size(state->slot_buttons[i], 632, 104);
    StyleRootButton(state->slot_buttons[i]);
  }

  auto *patch_card = lv_obj_create(state->list);
  Panel(patch_card, 34, kMainSheet);
  lv_obj_set_pos(patch_card, 0, 646);
  lv_obj_set_size(patch_card, 1312, 358);
  state->patch_status = Label(patch_card, "Inspecting init_boot…", &lv_font_montserrat_36, kText);
  lv_obj_set_pos(state->patch_status, 32, 28);
  state->patch_detail = Label(patch_card, "Checking for an existing root patch.",
                              &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(state->patch_detail, 32, 88);
  lv_obj_set_width(state->patch_detail, 1240);
  state->progress = lv_bar_create(patch_card);
  lv_obj_set_pos(state->progress, 32, 176);
  lv_obj_set_size(state->progress, 1248, 14);
  lv_bar_set_range(state->progress, 0, 100);
  lv_obj_set_style_bg_color(state->progress, kMainPanel, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->progress, kAccent, LV_PART_INDICATOR);
  lv_obj_set_style_radius(state->progress, 7, LV_PART_MAIN);
  lv_obj_set_style_radius(state->progress, 7, LV_PART_INDICATOR);
  state->patch_button = Button(patch_card, "Verify & patch", [state] {
    ConfirmPatch(state);
  });
  StyleRootButton(state->patch_button);
  lv_obj_set_pos(state->patch_button, 32, 220);
  lv_obj_set_size(state->patch_button, 570, 106);
  state->refresh_release_button = Button(patch_card, "Check online", [state] {
    root::Request request;
    request.job = root::Job::kRefreshRelease;
    request.provider = state->provider;
    Start(state, request);
  });
  StyleRootButton(state->refresh_release_button);
  lv_obj_set_pos(state->refresh_release_button, 622, 220);
  lv_obj_set_size(state->refresh_release_button, 300, 106);
  state->rollback_button = Button(patch_card, "Restore backup", [state] {
    ConfirmRollback(state);
  });
  StyleRootButton(state->rollback_button);
  lv_obj_set_pos(state->rollback_button, 942, 220);
  lv_obj_set_size(state->rollback_button, 338, 106);

  auto *manager_card = lv_obj_create(state->list);
  Panel(manager_card, 34, kMainSheet);
  lv_obj_set_pos(manager_card, 0, 1040);
  lv_obj_set_size(manager_card, 1312, 258);
  auto *manager_plate = lv_obj_create(manager_card);
  Panel(manager_plate, 22, IconBackground());
  lv_obj_set_pos(manager_plate, 28, 30);
  lv_obj_set_size(manager_plate, 82, 82);
  auto *manager_icon = Label(manager_plate, LV_SYMBOL_DOWNLOAD,
                             &lv_font_montserrat_32, kAccent);
  lv_obj_center(manager_icon);
  state->manager_status = Label(manager_card, "Inspecting manager app…",
                                &lv_font_montserrat_32, kText);
  lv_obj_set_pos(state->manager_status, 136, 28);
  state->manager_detail = Label(manager_card,
      "Checking Android's installed packages.", &lv_font_montserrat_24,
      kMutedStrong);
  lv_obj_set_pos(state->manager_detail, 136, 80);
  lv_obj_set_width(state->manager_detail, 688);
  state->manager_progress = lv_bar_create(manager_card);
  lv_obj_set_pos(state->manager_progress, 28, 142);
  lv_obj_set_size(state->manager_progress, 1256, 12);
  lv_bar_set_range(state->manager_progress, 0, 100);
  lv_obj_set_style_bg_color(state->manager_progress, kMainPanel, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->manager_progress, kAccent, LV_PART_INDICATOR);
  lv_obj_set_style_radius(state->manager_progress, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(state->manager_progress, 6, LV_PART_INDICATOR);
  lv_obj_add_flag(state->manager_progress, LV_OBJ_FLAG_HIDDEN);
  state->manager_button = Button(manager_card, "Download & install",
      [state] { ConfirmManagerAction(state); });
  StyleRootButton(state->manager_button);
  lv_obj_set_pos(state->manager_button, 864, 52);
  lv_obj_set_size(state->manager_button, 420, 106);

  auto *modules_title = Label(state->list, "MODULES", &lv_font_montserrat_20, kMuted);
  lv_obj_set_pos(modules_title, 18, 1340);
  state->module_summary = Label(state->list, "Reading installed modules…",
                                &lv_font_montserrat_24, kMutedStrong);
  lv_obj_set_pos(state->module_summary, 18, 1380);
  auto *refresh = Button(state->list, "Check updates", [state] {
    root::Request request; request.job = root::Job::kRefreshModules;
    Start(state, request);
  });
  StyleRootButton(refresh);
  lv_obj_set_pos(refresh, 962, 1330);
  lv_obj_set_size(refresh, 350, 96);
  state->module_list = lv_obj_create(state->list);
  Clear(state->module_list);
  lv_obj_set_pos(state->module_list, 0, 1450);
  lv_obj_set_size(state->module_list, 1312, 1200);
  lv_obj_remove_flag(state->module_list, LV_OBJ_FLAG_SCROLLABLE);

  Navigation(screen, Action::kSettings, callback, context);
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    Destroy(static_cast<RootUi *>(lv_event_get_user_data(event)));
  }, LV_EVENT_DELETE, state);
  state->timer = lv_timer_create([](lv_timer_t *timer) {
    Poll(static_cast<RootUi *>(lv_timer_get_user_data(timer)));
  }, 80, state);
  state->worker = std::thread([state] {
    root::Status device = root::Probe();
    const root::Provider providers[] = {root::Provider::kKernelSU,
        root::Provider::kKernelSUNext, root::Provider::kSukiSU};
    for (root::Provider provider : providers)
      root::BundledRelease(provider, device.kmi);
    const std::string slot = device.slot;
    root::PatchInfo patch = root::InspectSlot(slot);
    auto modules = root::InstalledModules();
    {
      std::lock_guard<std::mutex> lock(state->data_mutex);
      state->device = std::move(device);
      state->slot = slot;
      state->patch = std::move(patch);
      state->modules = std::move(modules);
    }
    state->done.store(true, std::memory_order_release);
  });
  AnimateEnter(hero, 0, 18);
}

}  // namespace aeraui
