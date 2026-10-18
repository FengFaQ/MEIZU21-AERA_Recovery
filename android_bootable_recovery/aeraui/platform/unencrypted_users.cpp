/* SPDX-License-Identifier: Apache-2.0 */
#include "unencrypted_users.hpp"
#include "../features/backup/android_packages_xml.hpp"

#include <algorithm>
#include <climits>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <utility>

namespace aeraui {

// Only call with mounted, unencrypted userdata. FBE users remain governed by
// the keystore's per-user unlock state, not directory visibility.
std::vector<AndroidUser> UnencryptedAndroidUsers(
    const std::string &data_root, bool mounted, bool encrypted) {
  std::vector<AndroidUser> users;
  if (!mounted || encrypted) return users;
  const std::string metadata = data_root + "/system/users/";
  DIR *directory = opendir(metadata.c_str());
  if (!directory) return users;
  while (const auto *entry = readdir(directory)) {
    const std::string filename = entry->d_name;
    if (filename.size() <= 4 ||
        filename.compare(filename.size() - 4, 4, ".xml") != 0)
      continue;
    const std::string number = filename.substr(0, filename.size() - 4);
    int id = 0;
    bool valid = number == "0" || number.front() != '0';
    for (const char c : number) {
      if (c < '0' || c > '9' || id > (INT_MAX - (c - '0')) / 10) {
        valid = false;
        break;
      }
      id = id * 10 + (c - '0');
    }
    if (!valid) continue;
    const std::string path = metadata + filename;
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > 64 * 1024)
      continue;
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(info.st_size), '\0');
    if (!input.read(bytes.data(), bytes.size())) continue;
    tinyxml2::XMLDocument document;
    std::string error;
    if (!appvault::ParsePackagesXml(bytes, document, error)) continue;
    const auto *user = document.FirstChildElement("user");
    int metadata_id = -1;
    if (!user || user->QueryIntAttribute("id", &metadata_id) !=
                     tinyxml2::XML_SUCCESS || metadata_id != id ||
        user->BoolAttribute("partial") || user->BoolAttribute("preCreated"))
      continue;
    std::string name;
    const auto *name_node = user->FirstChildElement("name");
    if (name_node && name_node->GetText()) name = name_node->GetText();
    if (name.empty())
      name = id == 0 ? "Owner" : "Android user " + number;

    const std::string ce = data_root + (id == 0 ? "/data" : "/user/" + number);
    DIR *data = opendir(ce.c_str());
    if (!data && id == 0) data = opendir((data_root + "/user/0").c_str());
    const bool accessible = data != nullptr;
    if (data) closedir(data);
    users.push_back({id, std::move(name), 0, accessible});
  }
  closedir(directory);
  std::sort(users.begin(), users.end(),
            [](const AndroidUser &left, const AndroidUser &right) {
              return left.id < right.id;
            });
  return users;
}

}  // namespace aeraui
