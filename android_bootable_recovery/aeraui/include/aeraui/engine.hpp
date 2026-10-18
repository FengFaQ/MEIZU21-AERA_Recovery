/*
 * Copyright (C) 2026 Recovery AERA UI contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <memory>

namespace aeraui {

enum class Action {
    kNone = 0,
    kInstall,
    kBackup,
    kRestore,
    kWipe,
    kFiles,
    kTerminal,
    kSettings,
    kOpenReboot,
    kBack,
    kBackHome,
    kRebootSystem,
    kRebootRecovery,
    kRebootBootloader,
    kRebootFastbootd,
    kPowerOff,
    kBootComplete,
    kBrowsePackages,
    kInstallPackage,
    kSideload,
    kStartSideload,
    kCancelSideload,
    kDecryptSubmit,
    kDecryptSkip,
    kRunOperation,
    kMounts,
    kLogs,
    kPreferences,
    kLanguage,
    kTheme,
    kFormatData,
    kWeb,
    kRetroArch,
    kDoom,
    kTelegram,
    kGallery,
    kMedia,
    kStreams,
    kRecorder,
    kAppVault,
    kRootManager,
    kWifi,
    kRunWifiOperation,
    kPlugins,
    kRunPluginOperation,
    kApplyPluginAutoUpdates,
    kInstallLocalPlugin,
    kPluginApp,
    kUnlock,
    kQuickWifiToggle,
    kToggleRotation,
    kToggleVideoRotation,
    kNas,
    kRunNasOperation,
    kToggleRecording,
    kUsers,
    kDecryptUser,
    kUpdates,
    kCheckUpdates,
    kDownloadUpdate,
    kAbout,
    kPcConnection,
    kCancelPcBackup,
    kStatusBar,
    kTemperatures,
    kLockScreenshot,
};

enum class DecryptionCompletion {
    kNone = 0,
    kSuccess,
    kSkipped,
};

struct PointerEvent {
    int32_t x = 0;
    int32_t y = 0;
    int32_t slot = 0;
    bool pressed = false;
};

// LVGL owns global process state, so a recovery process must create one Engine.
// minui's gr_init() must succeed before Initialize() is called.
class Engine final {
  public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool Initialize(bool fastboot_mode = false,
                    bool adaptive_resolution = false,
                    int32_t logical_height = 3168,
                    bool resume_recovery = false);
    void Shutdown();

    // Terminal actions hand control to init, which will replace this process.
    // Keep EGL/LVGL alive until then; destroying the active Adreno scanout
    // context during handoff aborts the recovery process before sys.powerctl.
    void AbandonForTerminalAction();

    // Runs animations, input processing and rendering once. Returns the maximum
    // number of milliseconds the event loop should wait before calling again.
    uint32_t RunFrame();

    // Returns and clears the next action requested by the scene.
    Action TakeAction();
    bool TakeScreenshotRequest();

    // Explicit Home clears navigation history. Hardware Back and the edge
    // gesture use NavigateBack() to return to the previous native surface.
    void NavigateHome();
    void NavigateBack();

    // Unlocks workflows after recovery has finished fstab, mount and decrypt
    // initialization. The boot renderer can run before this point.
    void SetBackendReady();

    // Rebuild only the active AERA scene while retaining the initialized
    // LVGL/Adreno/DRM stack across recovery <-> fastbootd transitions.
    void SetFastbootMode(bool enabled, bool show_recovery_home = true);

    // Shows the credential UI while the recovery startup thread waits at the
    // same point as OrangeFox's stock decrypt page.
    void BeginDecryption(int credential_type, bool file_based, int user_id,
                         int pattern_grid_size);

    // Returns and clears a completed native decryption decision.
    DecryptionCompletion TakeDecryptionCompletion();

    // Feed already-normalized minui touch coordinates into LVGL.
    void SetPointer(const PointerEvent& event);
    void SetMouseConnected(bool connected);
    void MouseMove(int dx, int dy);
    void MouseButton(bool pressed);
    void MouseWheel(int steps);
    bool KeyboardKey(unsigned code, int value);
    void ResetKeyboard();

    // Hardware-key surfaces managed by the recovery event loop.
    void SetSuspended(bool suspended);
    void ShowLockScreen(bool ambient = false);
    void ShowPowerMenu();
    void ShowVolume(int percent);
    void ShowScreenshotResult(bool success);

    int32_t Width() const;
    int32_t Height() const;
    bool IsLandscape() const;
    bool IsInitialized() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aeraui
