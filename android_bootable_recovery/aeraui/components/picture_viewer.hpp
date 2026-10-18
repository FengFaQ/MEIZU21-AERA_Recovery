/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string>
#include <lvgl.h>
namespace aeraui {
void OpenPicture(lv_obj_t *screen, const std::string &path);
bool PictureViewerHandlePointer(int slot, int x, int y, bool pressed);
}
