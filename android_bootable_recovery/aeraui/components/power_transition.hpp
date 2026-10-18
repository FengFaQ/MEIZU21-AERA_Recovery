/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <functional>
#include <string>

#include <lvgl.h>

namespace aeraui::widgets {

// Covers the current scene with a brief branded terminal animation and calls
// |complete| only after the animation has been painted.  This prevents the
// completed confirmation slider from becoming the last frame left on screen
// while init performs a reboot or shutdown.
void PowerTransition(lv_obj_t *screen, const std::string &title,
                     std::function<void()> complete);

// Live recovery <-> Fastbootd switches keep the process and renderer alive.
// Give that handoff a spatial transition instead of presenting it as a reboot.
void ModeTransition(lv_obj_t *screen, bool toward_fastbootd,
                    std::function<void()> complete,
                    uint32_t duration_ms = 720);

}  // namespace aeraui::widgets
