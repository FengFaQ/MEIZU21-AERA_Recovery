/*
 * Copyright 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aera_secrets.hpp"
#include "../aera_shared_storage.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <cutils/properties.h>
#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace {

constexpr char kCredentialPath[] = "/data/media/0/AERA/secrets.json";
constexpr char kWifiPath[] = "/data/media/0/AERA/wifi.json";
constexpr int kSchema = 2;
constexpr size_t kMaximumDocument = 1024 * 1024;

std::mutex g_lock;
Json::Value g_early_wlan;
std::string g_early_wlan_path;

std::string Property(const char* name) {
  char value[PROPERTY_VALUE_MAX] = {};
  property_get(name, value, "");
  return value;
}

std::string Hex(const unsigned char* bytes, size_t size) {
  constexpr char alphabet[] = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (size_t index = 0; index < size; ++index) {
    result[index * 2] = alphabet[bytes[index] >> 4];
    result[index * 2 + 1] = alphabet[bytes[index] & 0x0f];
  }
  return result;
}

bool Unhex(const std::string& text, std::vector<unsigned char>* bytes) {
  if (text.size() % 2 != 0) return false;
  const auto digit = [](unsigned char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  bytes->clear();
  bytes->reserve(text.size() / 2);
  for (size_t index = 0; index < text.size(); index += 2) {
    const int high = digit(text[index]);
    const int low = digit(text[index + 1]);
    if (high < 0 || low < 0) {
      bytes->clear();
      return false;
    }
    bytes->push_back(static_cast<unsigned char>((high << 4) | low));
  }
  return true;
}

bool StableDeviceKeyEnabled() {
  return Property("ro.aera.stable_secret_key") == "1";
}

std::string DeviceSerial() {
  std::string serial = Property("ro.serialno");
  if (serial.empty()) serial = Property("ro.boot.serialno");
  return serial;
}

std::array<unsigned char, SHA256_DIGEST_LENGTH> DeviceKey(int version) {
  const std::string serial = Property("ro.serialno");
  std::string stable_serial = serial;
  if (stable_serial.empty()) stable_serial = Property("ro.boot.serialno");
  // Version 4 intentionally excludes firmware and product/build identities.
  // A serial is public: this is device-bound obfuscation, not a secret key.
  const std::string material = version == 4
      ? "AERA-Credential-Store-v4\n" + DeviceSerial()
      : version == 3
      ? "AERA-Credential-Store-v3\n" + stable_serial + "\n" +
          Property("ro.product.device") + "\n" + Property("ro.product.name")
      : "AERA-Credential-Store-v2\n" + serial + "\n" +
          Property("ro.boot.vbmeta.digest") + "\n" +
          Property("ro.product.device") + "\n" + Property("ro.product.name");
  std::array<unsigned char, SHA256_DIGEST_LENGTH> key {};
  SHA256(reinterpret_cast<const unsigned char*>(material.data()),
         material.size(), key.data());
  return key;
}

bool Seal(const std::string& plain, Json::Value* value, bool serial_only = false) {
  if (serial_only && DeviceSerial().empty()) return false;
  std::array<unsigned char, 12> nonce {};
  std::array<unsigned char, 16> tag {};
  if (RAND_bytes(nonce.data(), nonce.size()) != 1) return false;

  std::array<unsigned char, SHA256_DIGEST_LENGTH> key =
      DeviceKey(serial_only ? 4 : StableDeviceKeyEnabled() ? 3 : 2);
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  std::vector<unsigned char> cipher(plain.size() + 16);
  int amount = 0;
  int total = 0;
  bool okay = context != nullptr &&
      EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1 &&
      EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1;
  if (okay && !plain.empty()) {
    okay = EVP_EncryptUpdate(
        context, cipher.data(), &amount,
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size()) == 1;
    total = amount;
  }
  if (okay) {
    okay = EVP_EncryptFinal_ex(context, cipher.data() + total, &amount) == 1;
    total += amount;
  }
  if (okay)
    okay = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG,
                               tag.size(), tag.data()) == 1;
  EVP_CIPHER_CTX_free(context);
  OPENSSL_cleanse(key.data(), key.size());
  if (!okay) return false;

  cipher.resize(static_cast<size_t>(total));
  *value = Json::Value(Json::objectValue);
  if (serial_only) (*value)["key_version"] = 4;
  (*value)["nonce"] = Hex(nonce.data(), nonce.size());
  (*value)["data"] = Hex(cipher.data(), cipher.size());
  (*value)["tag"] = Hex(tag.data(), tag.size());
  return true;
}

bool OpenWithKey(const Json::Value& value, std::string* plain, int version) {
  if (version == 4 && DeviceSerial().empty()) return false;
  if (!value.isObject() || !value["nonce"].isString() ||
      !value["data"].isString() || !value["tag"].isString())
    return false;
  std::vector<unsigned char> nonce;
  std::vector<unsigned char> cipher;
  std::vector<unsigned char> tag;
  if (!Unhex(value["nonce"].asString(), &nonce) || nonce.size() != 12 ||
      !Unhex(value["data"].asString(), &cipher) ||
      !Unhex(value["tag"].asString(), &tag) || tag.size() != 16)
    return false;

  std::array<unsigned char, SHA256_DIGEST_LENGTH> key = DeviceKey(version);
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  std::vector<unsigned char> output(cipher.size() + 16);
  int amount = 0;
  int total = 0;
  bool okay = context != nullptr &&
      EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1 &&
      EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1;
  if (okay && !cipher.empty()) {
    okay = EVP_DecryptUpdate(context, output.data(), &amount,
                             cipher.data(), cipher.size()) == 1;
    total = amount;
  }
  if (okay)
    okay = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG,
                               tag.size(), tag.data()) == 1;
  if (okay) {
    okay = EVP_DecryptFinal_ex(context, output.data() + total, &amount) == 1;
    total += amount;
  }
  EVP_CIPHER_CTX_free(context);
  OPENSSL_cleanse(key.data(), key.size());
  if (!okay) return false;
  plain->assign(reinterpret_cast<const char*>(output.data()),
                static_cast<size_t>(total));
  OPENSSL_cleanse(output.data(), output.size());
  return true;
}

bool Open(const Json::Value& value, std::string* plain,
          bool* used_legacy = nullptr, bool serial_only = false) {
  if (used_legacy) *used_legacy = false;
  if (serial_only) {
    if (!value.isObject()) return false;
    if (value.isMember("key_version")) {
      if (!value["key_version"].isInt() || value["key_version"].asInt() != 4)
        return false;
      return OpenWithKey(value, plain, 4);
    }
    if (!OpenWithKey(value, plain, 3) && !OpenWithKey(value, plain, 2))
      return false;
    if (used_legacy) *used_legacy = true;
    return true;
  }
  if (!StableDeviceKeyEnabled()) return OpenWithKey(value, plain, 2);
  if (OpenWithKey(value, plain, 3)) return true;
  if (!OpenWithKey(value, plain, 2)) return false;
  if (used_legacy) *used_legacy = true;
  return true;
}

Json::Value EmptyDocument() {
  Json::Value root(Json::objectValue);
  root["schema"] = kSchema;
  return root;
}

Json::Value ReadDocument(const char* path) {
  int descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) return EmptyDocument();
  struct stat info {};
  if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size <= 0 || static_cast<size_t>(info.st_size) > kMaximumDocument) {
    close(descriptor);
    return EmptyDocument();
  }
  std::string text(static_cast<size_t>(info.st_size), '\0');
  size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t amount = read(descriptor, text.data() + offset,
                                text.size() - offset);
    if (amount < 0 && errno == EINTR) continue;
    if (amount <= 0) {
      close(descriptor);
      return EmptyDocument();
    }
    offset += static_cast<size_t>(amount);
  }
  close(descriptor);
  Json::Value root;
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::string errors;
  std::istringstream input(text);
  if (!Json::parseFromStream(builder, input, &root, &errors) ||
      !root.isObject() || !root["schema"].isInt() || root["schema"].asInt() != kSchema)
    return EmptyDocument();
  return root;
}

bool WriteAll(int descriptor, const std::string& text) {
  size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t amount = write(descriptor, text.data() + offset,
                                 text.size() - offset);
    if (amount < 0 && errno == EINTR) continue;
    if (amount <= 0) return false;
    offset += static_cast<size_t>(amount);
  }
  return true;
}

bool WriteDocument(const char* path, Json::Value root) {
  const std::string directory = std::string(path).substr(0, std::string(path).find_last_of('/'));
  if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) return false;
  root["schema"] = kSchema;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  const std::string text = Json::writeString(writer, root);
  if (text.size() > kMaximumDocument) return false;

  const std::string temporary = std::string(path) + ".new";
  unlink(temporary.c_str());
  const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                              O_NOFOLLOW | O_CLOEXEC, 0600);
  if (descriptor < 0) return false;
  const bool okay = WriteAll(descriptor, text) && fsync(descriptor) == 0;
  close(descriptor);
  if (!okay || rename(temporary.c_str(), path) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  if (!aera::shared_storage::detail::MediaPath(path).empty()) {
    aera::shared_storage::Normalize(path);
    return true;
  }
  return chmod(path, 0600) == 0;
}

void RefreshEarlyWlanNetworks(const Json::Value& primary) {
  if (g_early_wlan_path.empty()) return;
  Json::Value cache = ReadDocument(g_early_wlan_path.c_str());
  if (!cache["auto_enable"].isBool() || !cache["auto_connect"].isBool() ||
      !cache["last_ssid"].isString()) return;
  cache["networks"] = primary["networks"].isObject()
      ? primary["networks"] : Json::Value(Json::objectValue);
  WriteDocument(g_early_wlan_path.c_str(), std::move(cache));
}

std::string KeyToken(const std::string& key) {
  const size_t space = key.find_first_of(" \t");
  return key.substr(0, space);
}

std::string KeyName(const std::string& key) {
  const size_t space = key.find_first_of(" \t");
  if (space == std::string::npos) return {};
  const size_t begin = key.find_first_not_of(" \t", space);
  if (begin == std::string::npos) return {};
  const size_t end = key.find_last_not_of(" \t\r\n");
  return key.substr(begin, end - begin + 1);
}

std::string Fingerprint(const std::string& key) {
  const std::string token = KeyToken(key);
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest {};
  SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(),
         digest.data());
  return Hex(digest.data(), digest.size()).substr(0, 16);
}

}  // namespace

bool AeraSecrets::SetWlanNetwork(const std::string& ssid,
                                 const std::string& password,
                                 const std::string& security) {
  if (ssid.empty()) return false;
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kWifiPath);
  Json::Value network(Json::objectValue);
  network["security"] = security.empty() ? "WPA2" : security;
  if (!Seal(password, &network["password"], true)) return false;
  root["networks"][ssid] = std::move(network);
  if (!WriteDocument(kWifiPath, root)) return false;
  RefreshEarlyWlanNetworks(root);
  return true;
}

bool AeraSecrets::GetWlanNetwork(const std::string& ssid,
                                 std::string& password,
                                 std::string& security) {
  password.clear();
  security.clear();
  if (ssid.empty()) return false;
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = g_early_wlan.isObject() ? g_early_wlan : ReadDocument(kWifiPath);
  Json::Value& network = root["networks"][ssid];
  if (!network.isObject() ||
      (network.isMember("security") && !network["security"].isString())) return false;
  security = network.get("security", "WPA2").asString();
  bool used_legacy = false;
  if (!Open(network["password"], &password, &used_legacy, true)) return false;
  if (!g_early_wlan.isObject() && used_legacy && Seal(password, &network["password"], true))
    WriteDocument(kWifiPath, std::move(root));
  return true;
}

bool AeraSecrets::DeleteWlanNetwork(const std::string& ssid) {
  if (ssid.empty()) return false;
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kWifiPath);
  if (!root["networks"].isObject() || !root["networks"].isMember(ssid)) {
    RefreshEarlyWlanNetworks(root);
    return true;
  }
  root["networks"].removeMember(ssid);
  if (!WriteDocument(kWifiPath, root)) return false;
  RefreshEarlyWlanNetworks(root);
  return true;
}

bool AeraSecrets::ListWlanNetworks(std::vector<std::string>& ssids) {
  ssids.clear();
  std::lock_guard<std::mutex> guard(g_lock);
  const Json::Value root = g_early_wlan.isObject() ? g_early_wlan : ReadDocument(kWifiPath);
  if (!root["networks"].isObject()) return false;
  ssids = root["networks"].getMemberNames();
  ssids.erase(std::remove(ssids.begin(), ssids.end(), ""), ssids.end());
  std::sort(ssids.begin(), ssids.end());
  return !ssids.empty();
}

bool AeraSecrets::LoadEarlyWlan(const std::string& path, WlanSession& session) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_early_wlan = Json::Value();
  session = WlanSession{};
  const Json::Value cache = ReadDocument(path.c_str());
  if (!cache["auto_enable"].isBool() || !cache["auto_connect"].isBool() ||
      !cache["last_ssid"].isString() || !cache["networks"].isObject())
    return false;
  session.auto_enable = cache["auto_enable"].asBool();
  session.auto_connect = cache["auto_connect"].asBool();
  session.last_ssid = cache["last_ssid"].asString();
  if (session.last_ssid.size() > 32) return false;
  for (const auto& ssid : cache["networks"].getMemberNames()) {
    const auto& network = cache["networks"][ssid];
    if (ssid.empty() || ssid.size() > 32 || !network.isObject() ||
        !network["security"].isString() || !network["password"].isObject()) return false;
  }
  g_early_wlan = cache;
  g_early_wlan_path = path;
  return true;
}

bool AeraSecrets::SaveEarlyWlan(const std::string& path, const WlanSession& session) {
  std::lock_guard<std::mutex> guard(g_lock);
  // Called only after the normal settings become available. Missing/empty
  // primary credentials deliberately clear old cached networks.
  Json::Value cache = EmptyDocument();
  const Json::Value primary = ReadDocument(kWifiPath);
  cache["networks"] = primary["networks"].isObject()
      ? primary["networks"] : Json::Value(Json::objectValue);
  cache["auto_enable"] = session.auto_enable;
  cache["auto_connect"] = session.auto_connect;
  cache["last_ssid"] = session.last_ssid;
  if (!WriteDocument(path.c_str(), std::move(cache))) return false;
  g_early_wlan_path = path;
  return true;
}

bool AeraSecrets::UsePrimaryWlan(const std::string& connected_ssid) {
  std::lock_guard<std::mutex> guard(g_lock);
  bool changed = false;
  if (g_early_wlan.isObject() && !connected_ssid.empty()) {
    const Json::Value primary = ReadDocument(kWifiPath);
    const Json::Value& before = g_early_wlan["networks"][connected_ssid];
    const Json::Value after = primary["networks"].isObject()
        ? primary["networks"][connected_ssid] : Json::Value();
    std::string old_password, new_password;
    changed = !before.isObject() || !after.isObject() ||
        before["security"] != after["security"] ||
        !Open(before["password"], &old_password, nullptr, true) ||
        !Open(after["password"], &new_password, nullptr, true) ||
        old_password != new_password;
    OPENSSL_cleanse(old_password.data(), old_password.size());
    OPENSSL_cleanse(new_password.data(), new_password.size());
  }
  g_early_wlan = Json::Value();
  return changed;
}

bool AeraSecrets::SetNasPassword(const std::string& password, const std::string& protocol) {
  if (!protocol.empty() && protocol != "smb" && protocol != "sftp") return false;
  if (password.empty()) return ClearNasPassword(protocol);
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kCredentialPath);
  const auto slot = protocol.empty() ? "nas_password" : "nas_password_" + protocol;
  if (!Seal(password, &root[slot])) return false;
  return WriteDocument(kCredentialPath, std::move(root));
}

bool AeraSecrets::ClearNasPassword(const std::string& protocol) {
  if (!protocol.empty() && protocol != "smb" && protocol != "sftp") return false;
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kCredentialPath);
  root.removeMember(protocol.empty() ? "nas_password" : "nas_password_" + protocol);
  return WriteDocument(kCredentialPath, std::move(root));
}

bool AeraSecrets::GetNasPassword(std::string& password, const std::string& protocol) {
  password.clear();
  if (!protocol.empty() && protocol != "smb" && protocol != "sftp") return false;
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kCredentialPath);
  bool used_legacy = false;
  const auto slot = protocol.empty() ? "nas_password" : "nas_password_" + protocol;
  if (!Open(root[slot], &password, &used_legacy) || password.empty())
    return false;
  if (used_legacy && Seal(password, &root[slot]))
    WriteDocument(kCredentialPath, std::move(root));
  return true;
}

bool AeraSecrets::HasNasPassword() {
  std::string password;
  const bool found = GetNasPassword(password) || GetNasPassword(password, "smb") ||
      GetNasPassword(password, "sftp");
  OPENSSL_cleanse(password.data(), password.size());
  return found;
}

bool AeraSecrets::MigrateNasPassword(const std::string& protocol) {
  if (protocol != "smb" && protocol != "sftp") return false;
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kCredentialPath);
  if (!root.isMember("nas_password")) return true;
  const auto slot = "nas_password_" + protocol;
  if (!root.isMember(slot)) {
    std::string password;
    const bool sealed = Open(root["nas_password"], &password) && Seal(password, &root[slot]);
    OPENSSL_cleanse(password.data(), password.size());
    if (!sealed) return false;
  }
  root.removeMember("nas_password");
  return WriteDocument(kCredentialPath, std::move(root));
}

bool AeraSecrets::SetNasPrivateKey(const std::string& key) {
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kCredentialPath);
  if (key.empty()) root.removeMember("nas_private_key");
  else if (!Seal(key, &root["nas_private_key"])) return false;
  return WriteDocument(kCredentialPath, std::move(root));
}

bool AeraSecrets::GetNasPrivateKey(std::string& key) {
  key.clear();
  std::lock_guard<std::mutex> guard(g_lock);
  const auto root = ReadDocument(kCredentialPath);
  return Open(root["nas_private_key"], &key) && !key.empty();
}

bool AeraSecrets::AddAdbDevice(const std::string& key,
                               const std::string& name) {
  if (KeyToken(key).empty()) return false;
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kCredentialPath);
  const std::string fingerprint = Fingerprint(key);
  Json::Value device(Json::objectValue);
  device["name"] = name.empty() ? KeyName(key) : name;
  if (device["name"].asString().empty())
    device["name"] = "device-" + fingerprint.substr(0, 6);
  device["key"] = key;
  device["last_seen"] = static_cast<Json::Int64>(time(nullptr));
  root["adb_devices"][fingerprint] = std::move(device);
  return WriteDocument(kCredentialPath, std::move(root));
}

bool AeraSecrets::ListAdbDevices(std::vector<AdbDevice>& devices) {
  devices.clear();
  std::lock_guard<std::mutex> guard(g_lock);
  const Json::Value root = ReadDocument(kCredentialPath);
  const Json::Value& stored = root["adb_devices"];
  if (!stored.isObject()) return false;
  for (const std::string& fingerprint : stored.getMemberNames()) {
    const Json::Value& entry = stored[fingerprint];
    if (!entry.isObject()) continue;
    AdbDevice device;
    device.fingerprint = fingerprint;
    device.name = entry.get("name", "").asString();
    device.key = entry.get("key", "").asString();
    device.last_seen = entry.get("last_seen", 0).asInt64();
    if (!device.key.empty()) devices.push_back(std::move(device));
  }
  std::sort(devices.begin(), devices.end(),
            [](const AdbDevice& left, const AdbDevice& right) {
              return left.last_seen > right.last_seen;
            });
  return !devices.empty();
}

bool AeraSecrets::DeleteAdbDevice(const std::string& id,
                                  std::string& removed_key) {
  removed_key.clear();
  if (id.empty()) return false;
  std::lock_guard<std::mutex> guard(g_lock);
  Json::Value root = ReadDocument(kCredentialPath);
  Json::Value& devices = root["adb_devices"];
  if (!devices.isObject()) return false;

  std::string match;
  for (const std::string& fingerprint : devices.getMemberNames()) {
    if (fingerprint == id || devices[fingerprint].get("name", "").asString() == id) {
      match = fingerprint;
      break;
    }
  }
  if (match.empty() && id.size() >= 4) {
    for (const std::string& fingerprint : devices.getMemberNames()) {
      if (fingerprint.compare(0, id.size(), id) != 0) continue;
      if (!match.empty()) return false;
      match = fingerprint;
    }
  }
  if (match.empty()) return false;
  removed_key = devices[match].get("key", "").asString();
  devices.removeMember(match);
  return WriteDocument(kCredentialPath, std::move(root));
}
