/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <tinyxml2.h>
#include <string>

namespace aeraui::appvault {
// Android Settings uses either ordinary XML or the ABX v0 typed XML format.
bool ParsePackagesXml(const std::string& bytes, tinyxml2::XMLDocument& document,
                      std::string& error);
}  // namespace aeraui::appvault
