/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl.h>

namespace aeraui::text_edit {
void Attach(lv_obj_t *input);
lv_obj_t *FocusedInput();
using ControlHandler = void (*)(lv_obj_t *, unsigned, void *);
void SetControlHandler(lv_obj_t *input, ControlHandler handler, void *context);
void SendControl(lv_obj_t *input, unsigned key);
void SetSelectAllOnHold(lv_obj_t *input, bool enabled);
void SelectWord(lv_obj_t *input);
void SelectAll(lv_obj_t *input);
bool Copy(lv_obj_t *input);
bool Cut(lv_obj_t *input);
bool Paste(lv_obj_t *input);
bool Dismiss(lv_obj_t *screen);
}  // namespace aeraui::text_edit
