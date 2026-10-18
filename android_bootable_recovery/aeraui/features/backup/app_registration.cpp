/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_registration.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <functional>
#include <set>

namespace aeraui::appvault {
namespace {
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

std::string Attr(const XMLElement* element, const char* name) {
  const char* value = element ? element->Attribute(name) : nullptr;
  return value ? value : "";
}

bool Number(const std::string& text, uint64_t& value) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return false;
  errno = 0;
  char* end = nullptr;
  value = strtoull(text.c_str(), &end, 10);
  return !errno && end && !*end;
}

const XMLElement* Find(const XMLElement* parent, const char* tag, const char* attribute,
                       const std::string& value) {
  for (auto* node = parent ? parent->FirstChildElement(tag) : nullptr; node;
       node = node->NextSiblingElement(tag))
    if (Attr(node, attribute) == value) return node;
  return nullptr;
}

XMLElement* Find(XMLElement* parent, const char* tag, const char* attribute,
                 const std::string& value) {
  return const_cast<XMLElement*>(
      Find(static_cast<const XMLElement*>(parent), tag, attribute, value));
}

bool SafePackage(const std::string& package) {
  return !package.empty() && package.size() < 256 && package.find('.') != std::string::npos &&
         package.front() != '.' && package.find("..") == std::string::npos &&
         package.find_first_not_of(
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") ==
             std::string::npos;
}

bool SafeCodePath(const std::string& path) {
  return path.compare(0, 10, "/data/app/") == 0 && path.size() > 10 &&
         path.find("/../") == std::string::npos && path.find("/./") == std::string::npos &&
         path.find("//") == std::string::npos && path.back() != '/' &&
         path.substr(path.size() >= 3 ? path.size() - 3 : 0) != "/.." &&
         path.substr(path.size() >= 2 ? path.size() - 2 : 0) != "/.";
}

bool CodeMatchesPackage(const std::string& path, const std::string& package) {
  const std::string leaf = path.substr(path.find_last_of('/') + 1);
  return leaf == package || leaf.compare(0, package.size() + 1, package + "-") == 0;
}

std::string AppId(const XMLElement* package) {
  return package && package->Attribute("sharedUserId") ? Attr(package, "sharedUserId")
                                                       : Attr(package, "userId");
}

void Walk(const XMLElement* node, const std::function<void(const XMLElement*)>& visit) {
  if (!node) return;
  visit(node);
  for (auto* child = node->FirstChildElement(); child; child = child->NextSiblingElement())
    Walk(child, visit);
}

bool Certificates(const XMLElement* root, std::map<std::string, std::string>& pool) {
  bool valid = true;
  Walk(root, [&](const XMLElement* node) {
    if (std::string(node->Name()) != "cert") return;
    const std::string index = Attr(node, "index"), key = Attr(node, "key");
    uint64_t numeric;
    if (!Number(index, numeric) || numeric > INT_MAX) {
      valid = false;
      return;
    }
    if (!key.empty()) {
      if (key.size() % 2 || key.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
          (pool.count(index) && pool[index] != key)) {
        valid = false;
        return;
      }
      pool[index] = key;
    }
  });
  return valid;
}

bool NormalizeCertificates(XMLElement* node, const std::map<std::string, std::string>& pool) {
  bool valid = true;
  Walk(node, [&](const XMLElement* entry) {
    if (std::string(entry->Name()) != "cert") return;
    const auto found = pool.find(Attr(entry, "index"));
    if (found == pool.end())
      valid = false;
    else
      const_cast<XMLElement*>(entry)->SetAttribute("key", found->second.c_str());
  });
  return valid;
}

std::set<std::string> SigningKeys(const XMLElement* package,
                                  const std::map<std::string, std::string>& pool) {
  std::set<std::string> keys;
  const auto* sigs = package ? package->FirstChildElement("sigs") : nullptr;
  for (auto* cert = sigs ? sigs->FirstChildElement("cert") : nullptr; cert;
       cert = cert->NextSiblingElement("cert")) {
    const auto found = pool.find(Attr(cert, "index"));
    if (found == pool.end()) return {};
    keys.insert(found->second);
  }
  return keys;
}

bool CopyKeySets(const XMLElement* package, const XMLElement* source, XMLDocument& metadata,
                 XMLElement* root) {
  std::set<std::string> sets;
  for (auto* node = package->FirstChildElement(); node; node = node->NextSiblingElement()) {
    const std::string tag = node->Name();
    if (tag == "proper-signing-keyset" || tag == "upgrade-keyset" || tag == "defined-keyset")
      sets.insert(Attr(node, "identifier"));
  }
  auto* settings = metadata.NewElement("keyset-settings");
  settings->SetAttribute("version", "1");
  auto* keys = metadata.NewElement("keys");
  auto* keysets = metadata.NewElement("keysets");
  settings->InsertEndChild(keys);
  settings->InsertEndChild(keysets);
  std::set<std::string> key_ids;
  for (const auto& id : sets) {
    const auto* set =
        Find(source ? source->FirstChildElement("keysets") : nullptr, "keyset", "identifier", id);
    if (!set) return false;
    keysets->InsertEndChild(set->DeepClone(&metadata));
    for (auto* key = set->FirstChildElement("key-id"); key; key = key->NextSiblingElement("key-id"))
      key_ids.insert(Attr(key, "identifier"));
  }
  for (const auto& id : key_ids) {
    const auto* key =
        Find(source ? source->FirstChildElement("keys") : nullptr, "public-key", "identifier", id);
    if (!key || Attr(key, "value").empty()) return false;
    keys->InsertEndChild(key->DeepClone(&metadata));
  }
  root->InsertEndChild(settings);
  return true;
}

bool ImportKeySets(XMLDocument& database, XMLElement* package, const XMLElement* saved) {
  auto* destination = database.RootElement()->FirstChildElement("keyset-settings");
  if (!saved || !destination || Attr(destination, "version") != "1") return false;
  auto* keys = destination->FirstChildElement("keys");
  auto* sets = destination->FirstChildElement("keysets");
  auto* last_key = destination->FirstChildElement("lastIssuedKeyId");
  auto* last_set = destination->FirstChildElement("lastIssuedKeySetId");
  uint64_t key_counter, set_counter;
  if (!keys || !sets || !last_key || !last_set || !Number(Attr(last_key, "value"), key_counter) ||
      !Number(Attr(last_set, "value"), set_counter))
    return false;
  Walk(keys, [&](const XMLElement* node) {
    uint64_t id;
    if (Number(Attr(node, "identifier"), id)) key_counter = std::max(key_counter, id);
  });
  Walk(sets, [&](const XMLElement* node) {
    uint64_t id;
    if (Number(Attr(node, "identifier"), id)) set_counter = std::max(set_counter, id);
  });
  if (key_counter > INT_MAX || set_counter > INT_MAX) return false;
  std::map<std::string, std::string> key_map, set_map;
  for (auto* node = saved->FirstChildElement("keys")
                        ? saved->FirstChildElement("keys")->FirstChildElement("public-key")
                        : nullptr;
       node; node = node->NextSiblingElement("public-key")) {
    const std::string id = Attr(node, "identifier");
    if (id.empty() || key_map.count(id) || Attr(node, "value").empty()) return false;
    auto* copy = node->DeepClone(&database)->ToElement();
    key_map[id] = std::to_string(++key_counter);
    copy->SetAttribute("identifier", key_map[id].c_str());
    keys->InsertEndChild(copy);
  }
  for (auto* node = saved->FirstChildElement("keysets")
                        ? saved->FirstChildElement("keysets")->FirstChildElement("keyset")
                        : nullptr;
       node; node = node->NextSiblingElement("keyset")) {
    const std::string id = Attr(node, "identifier");
    if (id.empty() || set_map.count(id)) return false;
    auto* copy = node->DeepClone(&database)->ToElement();
    set_map[id] = std::to_string(++set_counter);
    copy->SetAttribute("identifier", set_map[id].c_str());
    for (auto* key = copy->FirstChildElement("key-id"); key;
         key = key->NextSiblingElement("key-id")) {
      const auto found = key_map.find(Attr(key, "identifier"));
      if (found == key_map.end()) return false;
      key->SetAttribute("identifier", found->second.c_str());
    }
    sets->InsertEndChild(copy);
  }
  for (auto* node = package->FirstChildElement(); node; node = node->NextSiblingElement()) {
    const std::string tag = node->Name();
    if (tag == "proper-signing-keyset" || tag == "upgrade-keyset" || tag == "defined-keyset") {
      const auto found = set_map.find(Attr(node, "identifier"));
      if (found == set_map.end()) return false;
      node->SetAttribute("identifier", found->second.c_str());
    }
  }
  last_key->SetAttribute("value", std::to_string(key_counter).c_str());
  last_set->SetAttribute("value", std::to_string(set_counter).c_str());
  return true;
}
}  // namespace

bool ExportPackage(const XMLDocument& database, const XMLDocument& user_state,
                   const std::string& package, int user, int sdk, XMLDocument& metadata,
                   std::string& error) {
  metadata.Clear();
  const auto* source = Find(database.RootElement(), "package", "name", package);
  uint64_t uid;
  if (!SafePackage(package) || !source || !Number(AppId(source), uid) || !uid || uid > 19999) {
    error = package + ": cannot resolve Android package registration.";
    return false;
  }
  std::map<std::string, std::string> certificates;
  if (!Certificates(database.RootElement(), certificates) ||
      SigningKeys(source, certificates).empty()) {
    error = package + ": cannot resolve Android signing certificates.";
    return false;
  }
  auto* root = metadata.NewElement("appvault-registration");
  root->SetAttribute("schema", "1");
  root->SetAttribute("user", user);
  root->SetAttribute("sdk", sdk);
  const bool data_only =
      source->Attribute("sharedUserId") || uid < 10000 || !SafeCodePath(Attr(source, "codePath"));
  if (!data_only && !CodeMatchesPackage(Attr(source, "codePath"), package)) {
    error = package + ": APK folder does not match its registered package.";
    return false;
  }
  root->SetAttribute("data-only", data_only ? "true" : "false");
  metadata.InsertEndChild(root);
  auto* copy = source->DeepClone(&metadata)->ToElement();
  // Preserve the real install source. Its UID is resolved against the destination database.
  copy->DeleteAttribute("originalInstallerUid");
  if (!NormalizeCertificates(copy, certificates)) {
    error = package + ": incomplete signing metadata.";
    return false;
  }
  root->InsertEndChild(copy);
  if (!CopyKeySets(copy, database.RootElement()->FirstChildElement("keyset-settings"), metadata,
                   root)) {
    error = package + ": incomplete Android signing keysets.";
    return false;
  }
  const auto* state = Find(user_state.RootElement(), "pkg", "name", package);
  if (state && (Attr(state, "inst") == "false" || state->Attribute("archiveState"))) {
    error = package + ": app is not installed for the selected Android user.";
    return false;
  }
  auto* restriction = state ? state->DeepClone(&metadata)->ToElement() : metadata.NewElement("pkg");
  restriction->SetAttribute("name", package.c_str());
  restriction->DeleteAttribute("installed");
  restriction->SetAttribute("inst", "true");
  restriction->SetAttribute("stopped", "true");
  restriction->DeleteAttribute("ceDataInode");
  restriction->DeleteAttribute("deDataInode");
  root->InsertEndChild(restriction);
  return true;
}

bool ImportPackage(XMLDocument& database, const std::map<int, XMLDocument*>& users,
                   const XMLDocument& metadata, int user, int sdk, unsigned& app_id,
                   std::string& error) {
  const auto* root = metadata.RootElement();
  const auto* source = root ? root->FirstChildElement("package") : nullptr;
  const std::string name = Attr(source, "name"), code = Attr(source, "codePath");
  const bool data_only = Attr(root, "data-only") == "true";
  uint64_t saved_id;
  if (!root || std::string(root->Name()) != "appvault-registration" ||
      Attr(root, "schema") != "1" || root->IntAttribute("user", -1) != user ||
      root->IntAttribute("sdk", -1) != sdk || !SafePackage(name) ||
      (!data_only && (!SafeCodePath(code) || !CodeMatchesPackage(code, name) ||
                      source->Attribute("sharedUserId"))) ||
      !Number(AppId(source), saved_id) || !saved_id || saved_id > 19999 ||
      (!data_only && saved_id < 10000) || !users.count(user) || !database.RootElement() ||
      std::string(database.RootElement()->Name()) != "packages") {
    error = "App registration does not match this Android version or user.";
    return false;
  }
  std::map<std::string, std::string> saved_certificates, current_certificates;
  if (!Certificates(root, saved_certificates) ||
      !Certificates(database.RootElement(), current_certificates) ||
      SigningKeys(source, saved_certificates).empty()) {
    error = name + ": invalid signing metadata.";
    return false;
  }
  const auto* existing = Find(database.RootElement(), "package", "name", name);
  const auto* saved_state = root->FirstChildElement("pkg");
  if (!saved_state || Attr(saved_state, "name") != name) {
    error = name + ": missing Android user state.";
    return false;
  }
  for (const auto& [id, state] : users) {
    if (!state || !state->RootElement() ||
        std::string(state->RootElement()->Name()) != "package-restrictions") {
      error = "Invalid Android user package database.";
      return false;
    }
  }
  if (existing) {
    uint64_t id;
    if (!Number(AppId(existing), id) || !id || id > 19999 || code != Attr(existing, "codePath") ||
        Attr(source, "version") != Attr(existing, "version") || (data_only && saved_id != id) ||
        SigningKeys(source, saved_certificates) != SigningKeys(existing, current_certificates)) {
      error = name + ": installed app differs from the backup; uninstall it in Android first.";
      return false;
    }
    app_id = id;
  } else {
    if (data_only) {
      error = name + ": this system or shared-UID app must already be installed.";
      return false;
    }
    std::set<unsigned> occupied;
    for (auto* node = database.RootElement()->FirstChildElement(); node;
         node = node->NextSiblingElement()) {
      uint64_t id;
      if (Number(Attr(node, "userId"), id) && id <= UINT_MAX) occupied.insert(id);
      if (Number(Attr(node, "sharedUserId"), id) && id <= UINT_MAX) occupied.insert(id);
    }
    app_id = saved_id;
    if (occupied.count(app_id)) {
      app_id = 10000;
      while (app_id <= 19999 && occupied.count(app_id)) ++app_id;
      if (app_id > 19999) {
        error = "No free Android app UID.";
        return false;
      }
    }
    auto* copy = source->DeepClone(&database)->ToElement();
    copy->SetAttribute("userId", app_id);
    copy->DeleteAttribute("installerUid");
    const auto* installer =
        Find(database.RootElement(), "package", "name", Attr(source, "installer"));
    uint64_t installer_id, saved_installer_uid;
    if (installer && Number(AppId(installer), installer_id) && installer_id > 0 &&
        installer_id < 100000) {
      const uint64_t installer_user = Number(Attr(source, "installerUid"), saved_installer_uid)
                                          ? saved_installer_uid / 100000
                                          : static_cast<uint64_t>(user);
      const uint64_t installer_uid = installer_user <= static_cast<uint64_t>(INT_MAX / 100000)
                                         ? installer_user * 100000 + installer_id
                                         : UINT64_MAX;
      const auto found_user =
          installer_uid <= INT_MAX ? users.find(static_cast<int>(installer_user)) : users.end();
      if (found_user != users.end()) {
        const auto* installer_state =
            Find(found_user->second->RootElement(), "pkg", "name", Attr(source, "installer"));
        if (Attr(installer_state, "inst") != "false")
          copy->SetAttribute("installerUid", static_cast<unsigned>(installer_uid));
      }
    }
    if (!NormalizeCertificates(copy, saved_certificates)) {
      error = name + ": incomplete signing certificates.";
      return false;
    }
    uint64_t next_certificate = 0;
    for (const auto& [index, key] : current_certificates) {
      uint64_t id;
      if (Number(index, id)) next_certificate = std::max(next_certificate, id + 1);
    }
    std::map<std::string, std::string> assigned;
    Walk(copy, [&](const XMLElement* entry) {
      if (std::string(entry->Name()) != "cert") return;
      auto* cert = const_cast<XMLElement*>(entry);
      const std::string key = saved_certificates.at(Attr(cert, "index"));
      if (!assigned.count(key)) assigned[key] = std::to_string(next_certificate++);
      cert->SetAttribute("index", assigned[key].c_str());
      cert->SetAttribute("key", key.c_str());
    });
    if (!ImportKeySets(database, copy, root->FirstChildElement("keyset-settings"))) {
      error = name + ": cannot import signing keysets.";
      return false;
    }
    // AOSP consumes keyset references while reading packages, before keyset-settings.
    auto* keysets = database.RootElement()->FirstChildElement("keyset-settings");
    if (keysets->PreviousSibling())
      database.RootElement()->InsertAfterChild(keysets->PreviousSibling(), copy);
    else
      database.RootElement()->InsertFirstChild(copy);
    // Android otherwise defaults a newly known package to installed for every user.
    for (const auto& [id, document] : users) {
      if (id == user) continue;
      auto* old = Find(document->RootElement(), "pkg", "name", name);
      if (old) document->RootElement()->DeleteChild(old);
      auto* entry = document->NewElement("pkg");
      entry->SetAttribute("name", name.c_str());
      entry->SetAttribute("inst", "false");
      document->RootElement()->InsertEndChild(entry);
    }
  }
  auto* state = users.at(user);
  if (!state->RootElement() ||
      std::string(state->RootElement()->Name()) != "package-restrictions") {
    error = "Invalid Android user package database.";
    return false;
  }
  if (auto* old = Find(state->RootElement(), "pkg", "name", name))
    state->RootElement()->DeleteChild(old);
  auto* copy = saved_state->DeepClone(state)->ToElement();
  copy->DeleteAttribute("ceDataInode");
  copy->DeleteAttribute("deDataInode");
  copy->DeleteAttribute("installed");
  copy->SetAttribute("inst", "true");
  state->RootElement()->InsertEndChild(copy);
  return true;
}
}  // namespace aeraui::appvault
