/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <aeraui/backend.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aeraui::images {
struct Block {
  std::string path;
  uint64_t bytes = 0;
};
inline bool ValidName(const std::string &name) {
  return !name.empty() && name.size() <= 128 &&
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
}
inline std::string BaseName(const std::string &name) {
  if (name.size() > 2 && (name.compare(name.size()-2, 2, "_a") == 0 ||
                         name.compare(name.size()-2, 2, "_b") == 0)) return name.substr(0, name.size()-2);
  return name;
}
inline std::vector<Volume> PhysicalTargets(const std::map<std::string, Block> &blocks,
                                         const std::set<std::string> &represented) {
  std::vector<Volume> result;
  std::set<std::string> included;
  const std::set<std::string> configuration = {"userdata", "data", "metadata", "cache", "persist",
      "misc", "frp", "modemst1", "modemst2", "fsc", "fsg", "keystore", "devinfo", "config"};
  for (const auto &[name, block] : blocks) {
    const auto base = BaseName(name);
    // Super needs the dynamic-partition unmap/resize workflow, never a raw
    // fallback. Snapshot COW devices and mapper nodes are not physical targets.
    if (!ValidName(name) || base == "super" || base == "super_empty" ||
        name.find("-cow") != std::string::npos || block.path.find("/dm-") != std::string::npos ||
        configuration.count(base) || base.rfind("oplusreserve", 0) == 0 ||
        represented.count(base) || !block.bytes) continue;
    const bool slotted = blocks.count(base+"_a") && blocks.count(base+"_b");
    const auto target = slotted ? base : name;
    if (!included.insert(target).second) continue;
    const auto bytes = slotted ? std::min(blocks.at(base+"_a").bytes, blocks.at(base+"_b").bytes) : block.bytes;
    result.push_back({target, "/block/"+target, bytes, false, slotted, false});
  }
  return result;
}
}
