/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>

#include <lvgl.h>

namespace aeraui::wallpaper {

// Decode and activate a local PNG or JPEG. The decoded image is bounded to the
// active display, while each attached LVGL object owns a reference to its
// pixels so live theme changes cannot invalidate an outgoing screen.
bool Load(const std::string &path, std::string *error = nullptr);
void Clear();
bool Active();
const std::string &Path();

// Adds a non-interactive, centre-cropped wallpaper behind the target's other
// children. Returns false when the normal appearance background should be used.
bool Attach(lv_obj_t *target, bool contain = false);
// Independent lock-screen image/cache; never changes the main wallpaper.
bool AttachLock(lv_obj_t *target, const std::string &path, int blur);
// Height of an uncropped preview at the requested width, or fallback if unset.
int PreviewHeight(int width, int fallback);

}  // namespace aeraui::wallpaper
