/*
 * Copyright (C) 2026 Recovery AERA UI contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace aeraui {

enum class RunResult {
    kEngineFailure = -1,
    kUnexpectedExit = 1,
    kRebootSystem,
    kRebootRecovery,
    kRebootBootloader,
    kRebootFastbootd,
    kPowerOff,
};

enum class DecryptionResult {
    kUnavailable = -1,
    kSuccess = 0,
    kSkipped = 1,
    // The early UI exited for a reboot or power-off request. Recovery must
    // not launch a fallback decrypt renderer; RunAeraUi() preserves and
    // returns the terminal action at the normal handoff.
    kTerminalAction = 2,
};

enum class ModeTransitionRequestResult {
    kAccepted = 0,
    kAlreadyActive,
    kBusy,
    kUnavailable,
};

// AERA_SCREEN_H uses the stock theme's 1080-wide reference space, while
// the status-bar dimensions use AERA UI's 1440-wide logical coordinate space.
// Convert only the screen height at the renderer boundary.
struct DisplayMetrics {
    bool adaptive_resolution = false;
    int32_t logical_height = 3168;
    int32_t status_bar_height = 165;
    int32_t status_indent_left = 54;
    int32_t status_indent_right = 54;

    static constexpr DisplayMetrics FromThemeMetrics(
            bool adaptive, int32_t screen_height, int32_t status_height,
            int32_t status_left = 54, int32_t status_right = 54) {
        return {adaptive, screen_height * 4 / 3, status_height,
                status_left, status_right};
    }
};

// Starts the native renderer while the recovery core continues partition and
// decryption setup. Must be called after gr_init() and ev_init().
void StartAeraUiEarly(const DisplayMetrics& metrics = {});

// Displays AERA's native credential page and blocks recovery startup until
// data is unlocked or the user explicitly chooses to continue encrypted.
// credential_type uses the stock TW_CRYPTO_PWTYPE values (2 pattern, 3 PIN,
// all other non-zero values password).
DecryptionResult RunAeraUiDecryption(int credential_type,
                                          bool file_based, int user_id,
                                          int pattern_grid_size);

// Marks the recovery backend ready, then joins the already-running native UI.
// Hardware Back and ordinary workflows stay inside the native engine.
RunResult RunAeraUi(const DisplayMetrics& metrics = {});

// Restores the normal recovery surface after a userspace-only fastbootd
// handoff. The backend is already ready, so no boot/decryption animation is
// replayed.
RunResult RunAeraUiResume(const DisplayMetrics& metrics = {});

// Runs a dedicated userspace-fastboot surface. This deliberately bypasses
// boot animation, decryption, recovery navigation and plugin initialization.
RunResult RunAeraUiFastboot(const DisplayMetrics& metrics = {});

// Queues a live recovery <-> userspace-fastboot transition on the native UI
// thread. RPC workers must use this entry point instead of touching LVGL or
// the transition backend directly.
ModeTransitionRequestResult RequestModeTransition(bool toward_fastboot);

}  // namespace aeraui
