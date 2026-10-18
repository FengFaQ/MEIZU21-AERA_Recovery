/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>
#include <vector>

class AeraSecrets {
 public:
  struct WlanSession {
    bool auto_enable = false;
    bool auto_connect = false;
    std::string last_ssid;
  };

  struct AdbDevice {
    std::string fingerprint;
    std::string name;
    std::string key;
    long long last_seen = 0;
  };

  static bool SetWlanNetwork(const std::string& ssid,
                             const std::string& password,
                             const std::string& security);
  static bool GetWlanNetwork(const std::string& ssid,
                             std::string& password,
                             std::string& security);
  static bool DeleteWlanNetwork(const std::string& ssid);
  static bool ListWlanNetworks(std::vector<std::string>& ssids);
  // The early cache contains WLAN only, never NAS or trusted-computer keys.
  static bool LoadEarlyWlan(const std::string& path, WlanSession& session);
  static bool SaveEarlyWlan(const std::string& path, const WlanSession& session);
  // Returns whether the connected network's cached credentials changed.
  static bool UsePrimaryWlan(const std::string& connected_ssid = "");

  static bool SetNasPassword(const std::string& password, const std::string& protocol = "");
  static bool ClearNasPassword(const std::string& protocol = "");
  static bool GetNasPassword(std::string& password, const std::string& protocol = "");
  static bool MigrateNasPassword(const std::string& protocol);
  static bool HasNasPassword();
  static bool SetNasPrivateKey(const std::string& key);
  static bool GetNasPrivateKey(std::string& key);

  static bool AddAdbDevice(const std::string& key,
                           const std::string& name = "");
  static bool ListAdbDevices(std::vector<AdbDevice>& devices);
  static bool DeleteAdbDevice(const std::string& id,
                              std::string& removed_key);
};
