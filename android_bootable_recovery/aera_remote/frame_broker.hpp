/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <string>

namespace aera::remote::frames {

int Width();
int Height();
void Request();
bool ShouldRender();
void CaptureAfterRender();
bool Latest(std::string* jpeg, uint64_t* generation);
void Reset();

}  // namespace aera::remote::frames
