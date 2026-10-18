/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "scene.hpp"
namespace aeraui {
void ReviewPackage(lv_obj_t *screen, const std::string &path,
                   ActionCallback callback, void *context);
}
