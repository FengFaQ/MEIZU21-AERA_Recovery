/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "root_manager.hpp"

namespace aeraui::root {
inline std::vector<std::string> ModuleAssetNames(Provider provider,
                                                const std::string &kmi) {
  switch (provider) {
    case Provider::kKernelSU:
      return {"lkm-aarch64-" + kmi + "_kernelsu.ko"};
    case Provider::kKernelSUNext:
      return {"aarch64_" + kmi + "_kernelsu.ko",
              "aarch64-" + kmi + "_kernelsu.ko",
              kmi + "_kernelsu.ko"};
    case Provider::kSukiSU:
      return {"aarch64-" + kmi + "-lkm.zip"};
  }
  return {};
}
}  // namespace aeraui::root
