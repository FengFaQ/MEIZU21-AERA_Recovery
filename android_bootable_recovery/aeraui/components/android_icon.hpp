/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aeraui {

// Resolves and rasterizes Android APK icon resources into a square BGRA image.
// Supports bitmap, WebP, vector, adaptive, layer-list and simple shape icons.
bool DecodeAndroidIcon(const std::string &apk,
                       const std::vector<std::string> &resources,
                       uint32_t size, std::vector<uint8_t> &pixels);

// Reads the package identifier from an APK's binary AndroidManifest.xml.
// This uses Android's resource parser, so no aapt executable is required in
// the recovery ramdisk.
bool ReadAndroidPackageName(const std::string &apk, std::string &package_name);

}  // namespace aeraui
