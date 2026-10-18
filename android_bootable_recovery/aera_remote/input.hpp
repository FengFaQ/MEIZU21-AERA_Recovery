/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>

namespace aera::remote::input {

bool Touch(const std::string& action, int x, int y);
bool Key(const std::string& name);
void Shutdown();

}  // namespace aera::remote::input
