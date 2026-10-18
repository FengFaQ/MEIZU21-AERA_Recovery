/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace aeraui::appvault {
struct IncrementalMapping {
  std::string backing, subdirectory, target;
};

// Locate the actual package directory, not Android's temporary installer staging APK.
bool FindIncrementalMapping(const std::string& code, const std::string& stores,
                            IncrementalMapping& mapping, std::string& error);
bool MatchesIncrementalBinding(const std::string& metadata, const IncrementalMapping& mapping);
bool IsIncrementalApk(const std::string& path);

class IncrementalApkViews {
 public:
  IncrementalApkViews();
  ~IncrementalApkViews();
  bool Prepare(const std::vector<std::string>& code_paths, std::string& error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace aeraui::appvault
