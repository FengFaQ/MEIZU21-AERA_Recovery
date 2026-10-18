/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <lvgl.h>

namespace aeraui {

// Shared code-stream texture used by the Terminal card and branded surfaces.
void AddMatrixEngraving(lv_obj_t *parent, int width, int height,
                        lv_color_t accent);

}  // namespace aeraui
