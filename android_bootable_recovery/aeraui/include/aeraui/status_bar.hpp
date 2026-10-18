/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

#include "aeraui/engine.hpp"

struct _lv_obj_t;

namespace aeraui {

enum class StatusBarAction {
  kNone,
  kPower,
  kBack,
};

// Set once before the first AERA UI scene is constructed. Values are expressed in
// AERA UI's 1440-wide logical coordinate space.
void ConfigureStatusBar(int32_t height, int32_t left_indent,
                        int32_t right_indent);
int32_t StatusBarHeight();
// True while the pull-down shade (including its closing animation) owns
// pointer input above the current scene.
bool StatusBarShadeOpen();
// UI-thread refresh for immediate status display preference previews.
void RefreshStatusBars();

// Adds the persistent clock and battery chrome used by native recovery pages.
void AttachStatusBar(_lv_obj_t *screen, void (*callback)(Action, void *),
                     void *context, StatusBarAction action, bool soft_surface = false);

}  // namespace aeraui
