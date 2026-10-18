/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <tinyxml2.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aeraui::appvault {
constexpr char kMetadataRoot[] = "/tmp/aera-appvault-metadata";

// Kept independent of Android services so package merges can be tested offline.
bool ExportPackage(const tinyxml2::XMLDocument& database, const tinyxml2::XMLDocument& user_state,
                   const std::string& package, int user, int sdk, tinyxml2::XMLDocument& metadata,
                   std::string& error);
bool ImportPackage(tinyxml2::XMLDocument& database,
                   const std::map<int, tinyxml2::XMLDocument*>& users,
                   const tinyxml2::XMLDocument& metadata, int user, int sdk, unsigned& app_id,
                   std::string& error);

bool CaptureRegistration(int user, const std::vector<std::string>& packages,
                         std::vector<std::string>& metadata_paths, std::string& error);
bool RegisteredApkDirectories(std::vector<std::string>& paths, std::string& error);

class RestoreRegistration {
 public:
  RestoreRegistration();
  ~RestoreRegistration();
  bool Prepare(int user, const std::vector<std::string>& paths, const std::string& staging,
               std::string& error);
  bool Finish(std::string& error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace aeraui::appvault
