/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/backup/app_registration.hpp"
#include "../features/backup/android_packages_xml.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace aeraui::appvault;
using tinyxml2::XMLDocument;

static void Read(const std::string& path, XMLDocument& document) {
  std::ifstream file(path, std::ios::binary);
  assert(file.good());
  std::string bytes((std::istreambuf_iterator<char>(file)), {}), error;
  assert(ParsePackagesXml(bytes, document, error));
}

static tinyxml2::XMLElement* Package(XMLDocument& document, const std::string& name,
                                     const char* tag = "package") {
  for (auto* node = document.RootElement()->FirstChildElement(tag); node;
       node = node->NextSiblingElement(tag)) {
    const char* value = node->Attribute("name");
    if (value && name == value) return node;
  }
  return nullptr;
}

int main(int argc, char** argv) {
  const std::string name = "com.mcdonalds.mobileapp";
  XMLDocument source, user0, user10, user11, metadata;
  if (argc == 2) {
    const std::string directory = argv[1];
    Read(directory + "/packages.abx", source);
    Read(directory + "/restrictions-0.abx", user0);
    Read(directory + "/restrictions-10.abx", user10);
    Read(directory + "/restrictions-11.abx", user11);
  } else {
    assert(source.Parse(R"(<packages>
      <version sdkVersion="37"/>
      <package name="com.mcdonalds.mobileapp" userId="10480" version="81829"
        codePath="/data/app/~~example/com.mcdonalds.mobileapp-example"
        installer="com.android.vending" installerUid="10050"
        installInitiator="com.android.vending" packageSource="2">
        <sigs count="1" schemeVersion="3"><cert index="0" key="30820100"/></sigs>
        <install-initiator-sigs count="1" schemeVersion="3"><cert index="1"/></install-initiator-sigs>
        <proper-signing-keyset identifier="1"/>
      </package>
      <package name="com.android.vending" userId="10050">
        <sigs count="1" schemeVersion="3"><cert index="1" key="30820101"/></sigs>
      </package>
      <keyset-settings version="1"><keys><public-key identifier="1" value="AQID"/></keys>
        <keysets><keyset identifier="1"><key-id identifier="1"/></keyset></keysets>
        <lastIssuedKeyId value="1"/><lastIssuedKeySetId value="1"/>
      </keyset-settings>
    </packages>)") == tinyxml2::XML_SUCCESS);
    assert(user0.Parse(R"(<package-restrictions><pkg name="com.mcdonalds.mobileapp"
      ceDataInode="123" deDataInode="456"/></package-restrictions>)") == tinyxml2::XML_SUCCESS);
    assert(user10.Parse("<package-restrictions/>") == tinyxml2::XML_SUCCESS);
    assert(user11.Parse("<package-restrictions/>") == tinyxml2::XML_SUCCESS);
  }
  std::string error;
  assert(ExportPackage(source, user0, name, 0, 37, metadata, error));
  assert(!metadata.RootElement()->FirstChildElement("pkg")->Attribute("ceDataInode"));
  const auto* saved_package = metadata.RootElement()->FirstChildElement("package");
  const auto* original_package = Package(source, name);
  for (const char* field : { "installer", "installInitiator", "packageSource" }) {
    const char* before = original_package->Attribute(field);
    const char* after = saved_package->Attribute(field);
    assert(std::string(before ? before : "") == std::string(after ? after : ""));
  }
  if (original_package->FirstChildElement("install-initiator-sigs"))
    assert(saved_package->FirstChildElement("install-initiator-sigs"));
  XMLDocument absent;
  user0.DeepCopy(&absent);
  Package(absent, name, "pkg")->SetAttribute("inst", "false");
  XMLDocument rejected;
  assert(!ExportPackage(source, absent, name, 0, 37, rejected, error));
  XMLDocument target, target0, target10, target11;
  source.DeepCopy(&target);
  user0.DeepCopy(&target0);
  user10.DeepCopy(&target10);
  user11.DeepCopy(&target11);
  target.RootElement()->DeleteChild(Package(target, name));
  for (auto* document : { &target0, &target10, &target11 }) {
    if (auto* node = Package(*document, name, "pkg")) document->RootElement()->DeleteChild(node);
  }
  // Force an appId conflict; restoring must not take another app's UID.
  auto* collision = target.NewElement("package");
  collision->SetAttribute("name", "example.uid.collision");
  collision->SetAttribute("userId", 10480);
  target.RootElement()->InsertEndChild(collision);
  unsigned uid = 0;
  const std::map<int, XMLDocument*> users = { { 0, &target0 },
                                              { 10, &target10 },
                                              { 11, &target11 } };
  assert(!ImportPackage(target, users, metadata, 0, 36, uid, error));
  assert(!Package(target, name));
  if (!ImportPackage(target, users, metadata, 0, 37, uid, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  assert(uid >= 10000 && uid < 20000 && uid != 10480);
  assert(Package(target, "example.uid.collision")->UnsignedAttribute("userId") == 10480);
  assert(Package(target, name)->UnsignedAttribute("userId") == uid);
  bool found_package = false;
  for (auto* node = target.RootElement()->FirstChildElement(); node;
       node = node->NextSiblingElement()) {
    if (node == Package(target, name)) found_package = true;
    if (std::string(node->Name()) == "keyset-settings") assert(found_package);
  }
  assert(Package(target0, name, "pkg")->BoolAttribute("inst", false));
  assert(!Package(target10, name, "pkg")->BoolAttribute("inst", true));
  assert(!Package(target11, name, "pkg")->BoolAttribute("inst", true));
  for (auto* document : { &target0, &target10, &target11 })
    assert(!Package(*document, name, "pkg")->Attribute("installed"));
  const auto* installer = Package(target, "com.android.vending");
  if (installer) {
    assert(std::string(Package(target, name)->Attribute("installer")) == "com.android.vending");
    assert(Package(target, name)->UnsignedAttribute("installerUid") ==
           installer->UnsignedAttribute("userId"));
  }
  for (auto* node = source.RootElement()->FirstChildElement("package"); node;
       node = node->NextSiblingElement("package")) {
    const std::string other = node->Attribute("name");
    if (other == name) continue;
    tinyxml2::XMLPrinter before, after;
    node->Accept(&before);
    assert(Package(target, other));
    Package(target, other)->Accept(&after);
    assert(std::string(before.CStr()) == after.CStr());
  }
  XMLDocument exported;
  assert(ExportPackage(target, target0, name, 0, 37, exported, error));
  unsigned same_uid = 0;
  assert(ImportPackage(target, users, exported, 0, 37, same_uid, error));
  assert(same_uid == uid);
  if (Package(source, "com.android.vending")) {
    XMLDocument relocated;
    source.DeepCopy(&relocated);
    relocated.RootElement()->DeleteChild(Package(relocated, name));
    Package(relocated, "com.android.vending")->SetAttribute("userId", 19000);
    assert(ImportPackage(relocated, users, metadata, 0, 37, same_uid, error));
    assert(Package(relocated, name)->UnsignedAttribute("installerUid") == 19000);
    assert(std::string(Package(relocated, name)->Attribute("installer")) == "com.android.vending");
  }
  metadata.RootElement()->FirstChildElement("package")->SetAttribute("codePath",
                                                                     "/data/app/../system");
  assert(!ImportPackage(target, users, metadata, 0, 37, same_uid, error));
  // A system/shared-UID app must not acquire a new UID or be installed from backup.
  XMLDocument system_metadata;
  source.DeepCopy(&system_metadata);
  auto* system_app = Package(system_metadata, name);
  system_app->DeleteAttribute("userId");
  system_app->SetAttribute("sharedUserId", "1000");
  system_app->SetAttribute("codePath", "/system/app/Example");
  XMLDocument data_backup;
  assert(ExportPackage(system_metadata, user0, name, 0, 37, data_backup, error));
  assert(!ImportPackage(target, users, data_backup, 0, 37, same_uid, error));
  assert(ImportPackage(system_metadata, users, data_backup, 0, 37, same_uid, error));
  assert(same_uid == 1000);
  std::cout << "McDonald's registration: export, uninstall/restore, UID collision,\n"
               "signing keyset remapping and multi-user isolation passed\n";
}
