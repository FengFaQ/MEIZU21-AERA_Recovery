/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <aeraui/backend.hpp>

namespace aeraui {
std::vector<AndroidUser> UnencryptedAndroidUsers(
    const std::string &data_root, bool mounted, bool encrypted);
}  // namespace aeraui
