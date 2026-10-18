/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>

namespace aera::remote {

bool Start(int port = 80);
void Stop();
bool Running();
int Port();
std::string Address();
std::string AccessCode();

bool ShouldRenderFrame();
void CaptureAfterRender();

}  // namespace aera::remote
