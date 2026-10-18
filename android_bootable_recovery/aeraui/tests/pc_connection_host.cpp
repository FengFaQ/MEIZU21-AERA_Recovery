/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_remote/pc_connection.hpp"
#include "../../aera_remote/pc_storage.hpp"
#include "../../aera_flash_log.hpp"
#include "../features/update/payload_inspector.hpp"
#include "../features/update/payload_arb.hpp"
#include "../features/root/root_manager.hpp"

#include <chrono>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <thread>
#include <future>

namespace {
int SimulateBackup(const aeraui::JobRequest &job) {
  using namespace aera::pc;
  if (job.backup_action == "backup") {
    const std::string root = job.backup_source == "computer" ? storage::kMount : job.backup_storage;
    assert(job.path == root + (job.backup_kind == "apps" ? "/AERA/AppBackupVault" : ""));
  }
  Json::Value result(Json::objectValue);
  if (job.backup_action == "apps") {
    Json::Value app; app["package"] = "test.app"; app["name"] = "Test application"; app["system"] = false;
    app["icon"] = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";
    result["apps"].append(app);
  } else if (job.backup_source == "computer") {
    auto call = [&](const char *op, const char *path, const std::string &data = "", uint64_t offset = 0,
                    std::string *read = nullptr) {
      Json::Value request, reply; request["op"] = op; request["path"] = path;
      request["offset"] = Json::UInt64(offset); request["size"] = Json::UInt64(data.empty() ? storage::kBlock : data.size());
      return storage::Channel().Call(request, reply, read, data);
    };
    int error = call("stat", "/");
    if (!error && job.backup_action == "backup") {
      const std::string folder = "/AERA/BACKUPS/test-device/" + job.name;
      for (const auto *path : {"/AERA", "/AERA/BACKUPS", "/AERA/BACKUPS/test-device"}) {
        if (!error) { const int status = call("mkdir", path); if (status != -17) error = status; }
      }
      if (!error) error = call("mkdir", folder.c_str());
      const std::string path = folder + "/data.ext4.win";
      if (!error) error = call("create", path.c_str());
      for (int i = 0; i < 8 && !error; ++i) {
        error = call("write", path.c_str(), std::string(storage::kBlock, static_cast<char>('a' + i)), i * storage::kBlock);
        UpdateInstall((i + 1) * 100 / 8, "Simulated backup block " + std::to_string(i + 1), "");
      }
      if (!error) error = call("close", path.c_str());
      result["folder"] = folder.substr(1);
    } else if (!error) {
      std::string data;
      error = call("read", "/data.ext4.win", "", 0, &data);
      if (!error && (data.size() != storage::kBlock || data != std::string(storage::kBlock, 'a'))) error = -5;
      if (job.backup_action == "inspect") {
        Json::Value part; part["path"] = "/data"; part["name"] = "Data"; part["bytes"] = Json::UInt64(storage::kBlock * 8);
        result["partitions"].append(part);
      }
    }
    storage::Channel().Stop();
    if (error) {
      result["error"] = "Simulated computer storage failed.\nComputer storage: " + storage::Channel().LastError();
      UpdateBackupResult(result); return 1;
    }
  }
  result["detail"] = "Simulated backup operation completed.";
  UpdateBackupResult(result); return 0;
}
}

namespace {
std::string storage;
bool locked = false;
bool current_external = false;
bool app_vault_installed = true;
bool wifi_connected = false;
std::string wifi_address = "127.0.0.2";
}
namespace aeraui {
void RecoveryEnsurePcStorage() {}
bool AppVaultInstalled() { return app_vault_installed; }
std::vector<AndroidUser> RecoveryAndroidUsers() {
  return {{0, "Owner", 1, !locked}, {10, "Second user", 1, !locked}};
}
std::string RecoveryDevice() { return "test-device"; }
std::string RecoveryVersion() { return "PC connection test"; }
std::string RecoverySlot() { return "A"; }
std::string RecoveryBootSlot() { return "a"; }
bool RecoveryDataLocked() { return locked; }
bool RecoveryPreservationSupported() { return true; }
bool RecoveryAblPreservationSupported() { return true; }
bool RecoveryPreference(Preference) { return true; }
SnapshotCowStatus RecoverySnapshotCowStatus() { return {}; }
PayloadFlashProgress RecoveryPayloadFlashProgress() {
  return {true, 2, {{"system", 200000000, 100000000, PayloadPhase::Writing},
                   {"vendor", 100000000, 25000000, PayloadPhase::Verifying},
                   {"boot", 10000000, 10000000, PayloadPhase::Verified},
                   {"dtbo", 10000000, 0, PayloadPhase::Queued},
                   {"recovery", 10000000, 0, PayloadPhase::NotFlashed},
                   {"test_failure", 10000000, 5000000, PayloadPhase::Failed}}};
}
std::vector<PayloadFlashTarget> RecoveryPayloadTargets(const std::vector<std::string> &names) {
  std::vector<PayloadFlashTarget> targets;
  for (const auto &name : names) targets.push_back({name, "/fixture/" + name});
  return targets;
}
std::string RecoveryStorage() { return current_external ? storage + "/usb" : storage; }
std::string RecoveryInternalStorage() { return locked ? "" : storage; }
std::vector<BackupStorage> RecoveryBackupStorages() {
  return {{"Internal storage", storage, true, 10000000000},
          {"USB storage", storage + "/usb", true, 5000000000}};
}
bool RecoveryBackupPathReady(const std::string &) { return true; }
std::string RecoveryAppVaultStorage() { return storage; }
NasStatus RecoveryNasStatus() {
  NasStatus status;
  status.supported = true;
  status.config.host = "test-server";
  status.config.user = "test-user";
  status.config.password = "never-return-this-secret";
  status.config.key_name = "id_ed25519";
  status.key_available = true;
  return status;
}
WifiConnection RecoveryWifiConnection() { return {wifi_connected, "Test network"}; }
NasConfig RecoverySavedNasConfig(const std::string &protocol) {
  auto config = RecoveryNasStatus().config;
  config.type = protocol;
  return config;
}
std::vector<Volume> RecoveryVolumes(const std::string &kind) {
  if (kind == "backup") return {{"Data", "/data", 2147483648}, {"Boot", "/boot", 67108864}, {"Recovery", "/recovery", 104857600}};
  return {{"USB storage", storage + "/usb"}};
}
std::vector<Volume> RecoveryImageVolumes() {
  return {{"Boot", "/boot", 67108864, false, true, false},
          {"System", "/system", 1073741824, false, false, true},
          {"Modem", "/block/modem", 536870912, false, true, false},
          {"Splash", "/block/splash", 16777216, false, false, false},
          {"Vendor", "/vendor", 67108864, false, false, true},
          {"XBL config", "/block/xbl_config", 4096, false, true, false},
          {"ABL", "/block/abl", 4096, false, true, false},
          {"Recovery", "/recovery", 4096, false, true, false}};
}
}
namespace aera::remote {
std::string Address() { return wifi_connected ? wifi_address : ""; }
bool Running() { return false; }
}
// Package parsing is covered by payload_inspector_test. This fixture supplies
// deterministic preflight results and never reads real device firmware.
namespace aeraui::payload {
Info InspectZip(const std::string &path, bool) {
  std::ifstream file(path, std::ios::binary);
  char data[64]{}; file.read(data, sizeof(data));
  const std::string marker(data, static_cast<size_t>(file.gcount()));
  Info info;
  if (marker.rfind("AERA-test-OTA\n", 0) != 0) return info;
  info.is_payload = true; info.valid = true;
  info.target_build = "Infinity-X Test"; info.target_device = "test-device";
  info.device_codename = "infiniti"; info.target_sdk = "Android 16";
  info.security_patch = "2026-10-01"; info.build_id = "TEST.20261009";
  info.system_fingerprint = "test/test-device/infiniti:16/TEST.20261009/1:user/release-keys";
  info.payload_bytes = 128ULL * 1024 * 1024; info.expanded_bytes = 256ULL * 1024 * 1024;
  info.format_version = 2; info.dynamic_partitions = true;
  info.operations = 3; info.operation_types = "raw, zero";
  info.partitions = {{"system", 192ULL * 1024 * 1024, 1}, {"vendor", 64ULL * 1024 * 1024, 1}, {"xbl_config", 4096, 1}};
  info.manifest_hash = std::string(32, 'm');
  if (marker.find("protected") != std::string::npos) {
    info.partitions.push_back({"abl", 4096, 1});
    info.partitions.push_back({"recovery", 4096, 1});
  }
  for (auto &partition : info.partitions) {
    partition.extractable = true;
    partition.sha256 = std::string(32, 'h');
  }
  info.incremental = marker.find("incremental") != std::string::npos;
  if (marker.find("unsupported") != std::string::npos) info.partitions.back().extractable = false;
  info.arb_available = true; info.arb_index = marker.find("upgrade") != std::string::npos ? 3 :
      marker.find("downgrade") != std::string::npos ? 1 : 2;
  info.arb_detail = "Simulated firmware metadata";
  if (marker.find("malformed") != std::string::npos) {
    info.valid = false; info.error = "Simulated malformed payload manifest.";
  }
  return info;
}
std::string Summary(const Info &info) { return info.is_payload ? "Simulated full OTA" : "Installer ZIP"; }
DeviceArb ReadDeviceArb() { return {{true, 2, "Fixture slot A"}, {true, 2, "Fixture slot B"}}; }
ArbDecision CompareArb(bool includes_firmware, const Arb &package, const DeviceArb &) {
  return !includes_firmware ? ArbDecision::NotApplicable : package.index < 2 ? ArbDecision::Downgrade :
      package.index > 2 ? ArbDecision::Upgrade : ArbDecision::Same;
}
}

namespace aera::flashlog {
Options PlatformOptions(const std::string &, const std::string &, const std::string &) { return {}; }
std::vector<std::string> PlatformDirectories() {
  return {storage + "/AERA/logs/FlashLogs"};
}
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  // Only availability is checked; this fixture never executes a flash engine.
  setenv("AERA_TEST_OTARIPPER", "/bin/true", 1);
  storage = argv[1];
  std::filesystem::create_directories(storage + "/usb");
  const int port = std::stoi(argv[2]);
  const bool demo = argc > 3 && std::string(argv[3]) == "--demo";
  if (!aera::pc::SetEnabled(true, port)) return 3;
  std::cout << "READY" << std::endl;
  bool run = demo;
  aera::pc::InstallRequest install;
  std::future<int> backup;
  auto started = std::chrono::steady_clock::now();
  for (;;) {
    pollfd input{0, POLLIN, 0};
    if (poll(&input, 1, 50) > 0 && (input.revents & POLLIN)) {
      std::string command;
      if (!std::getline(std::cin, command) || command == "quit") break;
      aera::pc::ConnectionRequest request;
      if (command == "approve" || command == "remember" || command == "deny") {
        if (aera::pc::TakeConnectionRequest(&request))
          aera::pc::ResolveConnection(request.id, command != "deny", command == "remember");
      } else if (command == "run") run = true;
      else if (command == "hold") run = false;
      else if (command == "cancel-upload") {
        const auto progress = aera::pc::GetUploadProgress();
        if (!progress.active || !aera::pc::CancelUpload(progress.id)) return 6;
      }
      else if (command == "cleanup-transfers") aera::pc::CleanupOldTransfers();
      else if (command == "busy" || command == "idle") aera::pc::SetRecoveryBusy(command == "busy");
      else if (command == "wifi-on" || command == "wifi-off" || command == "wifi-change") {
        wifi_connected = command != "wifi-off";
        if (command == "wifi-change") wifi_address = "127.0.0.3";
        aera::pc::RefreshNetwork();
      }
      else if (command == "reboot") {
        std::string target;
        if (aera::pc::TakeRebootRequest(&target)) std::cout << "REBOOT " << target << std::endl;
      }
      else if (command == "vault-off" || command == "vault-on") {
        app_vault_installed = command == "vault-on"; aera::pc::RefreshPlatformInfo();
      }
      else if (command == "off") aera::pc::SetEnabled(false, port, false);
      else if (command == "on") {
        if (!aera::pc::SetEnabled(true, port, false)) return 5;
      }
      else if (command == "current-usb" || command == "current-internal") {
        current_external = command == "current-usb"; aera::pc::RefreshPlatformInfo();
      }
      else if (command == "lock" || command == "unlock") {
        locked = command == "lock"; aera::pc::RefreshPlatformInfo();
      } else if (command == "restart") {
        aera::pc::SetEnabled(false, port);
        if (!aera::pc::SetEnabled(true, port)) return 4;
      } else if (command == "forget") aera::pc::ForgetComputers();
      else if (command == "prompt") {
        aera::pc::UpdateInstallerPrompt({true, "test-question", "Installer question",
                                      "Continue the simulated installation?", "Continue", "Cancel"});
      } else if (command == "presentation") {
        aeraui::InstallerPresentation presentation;
        presentation.active = true;
        presentation.package_name = "Simulated payload ZIP";
        presentation.device = "test-device";
        presentation.author = "AERA test fixture";
        presentation.stage = 2;
        presentation.stage_count = 5;
        presentation.stage_title = "Reading payload";
        presentation.stage_detail = "No device partitions are accessed.";
        aera::pc::UpdateInstallerPresentation(presentation);
      }
      std::cout << "OK " << command << std::endl;
    }
    if (demo) {
      aera::pc::ConnectionRequest request;
      if (aera::pc::TakeConnectionRequest(&request))
        aera::pc::ResolveConnection(request.id, true, true);
    }
    if (run && install.id.empty() && aera::pc::TakeInstallRequest(&install)) {
      if (!aera::pc::BeginInstall(install.id)) install = {};
      else {
        // Deliberately no RecoveryRunJob: this harness never flashes anything.
        started = std::chrono::steady_clock::now();
        if (install.job.job == aeraui::Job::kPcBackupOperation)
          backup = std::async(std::launch::async, SimulateBackup, install.job);
        std::cout << "INSTALL " << static_cast<int>(install.job.job) << ' '
                  << install.job.both_slots << ' ' << install.job.path << std::endl;
        std::cout << "VISIBLE " << install.job.show_on_device << std::endl;
      }
    }
    if (!install.id.empty()) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count();
      aera::pc::UpdateInstall(std::min(99L, ms / 30), "Simulated installation",
                            "Test installer: no device partitions are accessed.\n");
      std::string id; bool accepted;
      if (aera::pc::TakePromptAnswer(&id, &accepted)) {
        std::cout << "ANSWER " << id << ' ' << accepted << std::endl;
        aera::pc::UpdateInstallerPrompt({});
      }
      if (install.job.job == aeraui::Job::kPcBackupOperation) {
        if (backup.valid() && backup.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
          aera::pc::CompleteInstall(install.id, backup.get()); install = {};
        }
      } else if (ms >= (demo ? 10000 : 3000)) {
        if (install.job.root_action == "inspect") {
          aeraui::root::Progress result;
          result.has_inspection = true;
          result.inspected_target.slot = install.job.root_slot;
          result.inspected_target.kmi = "android16-6.12";
          result.inspected_patch.inspected = true;
          result.inspected_patch.patched = true;
          result.inspected_patch.aera_verified = true;
          result.inspected_patch.provider = "KernelSU Next";
          result.inspected_patch.version = "v3.4.0";
          result.inspected_offline.available = true;
          result.inspected_offline.version = "v3.3.0";
          aera::pc::UpdateRootInspection(result);
        }
        aera::pc::CompleteInstall(install.id, 0); install = {};
      }
    }
  }
  aera::pc::SetEnabled(false, port);
}
