/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "aera_engine.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <aeraui/backend.hpp>
#include <aeraui/runner.hpp>

#include "../aera_remote/aera_remote.hpp"
#include "../data.hpp"
#include "../partitions.hpp"
#include "../twrp-functions.hpp"
#include "../variables.h"

namespace aera::rpc {
namespace {

using aeraui::Job;
using aeraui::JobRequest;
using aeraui::Volume;

std::string Text(const Json::Value &args, const char *name,
                 const std::string &fallback = {}) {
  return args[name].isString() ? args[name].asString() : fallback;
}

bool Flag(const Json::Value &args, const char *name, bool fallback = false) {
  return args[name].isBool() ? args[name].asBool() : fallback;
}

int Number(const Json::Value &args, const char *name, int fallback = 0) {
  return args[name].isInt() ? args[name].asInt() : fallback;
}

std::vector<std::string> Strings(const Json::Value &args, const char *name) {
  std::vector<std::string> result;
  const Json::Value &array = args[name];
  if (!array.isArray()) return result;
  for (const auto &entry : array)
    if (entry.isString() && !entry.asString().empty()) result.push_back(entry.asString());
  return result;
}

Json::Value Volumes(const std::vector<Volume> &volumes, bool mounted_state) {
  Json::Value result(Json::objectValue);
  Json::Value items(Json::arrayValue);
  for (const auto &volume : volumes) {
    Json::Value item(Json::objectValue);
    item["name"] = volume.name;
    item["path"] = volume.path;
    item["bytes"] = Json::UInt64(volume.bytes);
    item[mounted_state ? "mounted" : "selected"] = volume.selected;
    items.append(std::move(item));
  }
  result["items"] = std::move(items);
  return result;
}

int Fail(EventSink &events, const std::string &code,
         const std::string &message, int result = 1) {
  events.Error(code, message);
  return result;
}

int Status(EventSink &events) {
  Json::Value status(Json::objectValue);
  status["release"] = aeraui::RecoveryVersion();
  status["build_type"] = aeraui::RecoveryBuildType();
  status["build_date"] = aeraui::RecoveryBuildDate();
  status["device"] = aeraui::RecoveryDevice();
  status["maintainer"] = aeraui::RecoveryMaintainer();
  status["active_slot"] = aeraui::RecoverySlot();
  status["storage"] = aeraui::RecoveryStorage();
  status["storage_locked"] = aeraui::RecoveryDataLocked();
  status["mtp"] = aeraui::RecoveryMtpEnabled();
  status["brightness"] = aeraui::RecoveryBrightness();
  const auto wifi = aeraui::RecoveryWifiStatus();
  status["wifi_supported"] = wifi.supported;
  status["wifi_enabled"] = wifi.enabled;
  status["wifi_connected"] = wifi.connected;
  status["wifi_ssid"] = wifi.ssid;
  status["wifi_address"] = wifi.ip_address;
  events.Data("status", status);
  return 0;
}

int Mount(const Request &request, EventSink &events, bool unmount) {
  const auto paths = Strings(request.arguments, "paths");
  if (paths.empty()) {
    events.Data("mounts", Volumes(aeraui::RecoveryVolumes("mount"), true));
    return 0;
  }
  for (const auto &path : paths) {
    JobRequest job;
    job.job = unmount ? Job::kUnmount : Job::kMount;
    job.title = unmount ? "Unmount" : "Mount";
    job.path = path;
    if (aeraui::RecoveryRunJob(job) != 0)
      return Fail(events, unmount ? "unmount_failed" : "mount_failed",
                  "Could not " + std::string(unmount ? "unmount " : "mount ") + path);
  }
  return 0;
}

int Install(const Request &request, EventSink &events) {
  const auto packages = Strings(request.arguments, "zips");
  if (packages.empty()) return Fail(events, "missing_package", "No package path was supplied", 2);
  for (const auto &path : packages) {
    JobRequest job;
    job.job = Job::kInstall;
    job.title = "Install package";
    job.path = path;
    events.Log("Installing " + path + "\n");
    if (aeraui::RecoveryRunJob(job) != 0)
      return Fail(events, "install_failed", "Package installation failed: " + path);
  }
  return 0;
}

int Backup(const Request &request, EventSink &events) {
  const auto partitions = Strings(request.arguments, "parts");
  if (partitions.empty()) return Fail(events, "missing_partitions", "Backup requires at least one partition", 2);
  JobRequest job;
  job.job = Job::kBackup;
  job.title = "Create backup";
  job.path = Text(request.arguments, "storage", aeraui::RecoveryStorage());
  job.partitions = partitions;
  job.compression = Flag(request.arguments, "compress", true);
  job.name = Text(request.arguments, "name");
  job.digest = Flag(request.arguments, "digest", true);
  return aeraui::RecoveryRunJob(job) == 0
      ? 0 : Fail(events, "backup_failed", "AERA could not create the backup");
}

int Restore(const Request &request, EventSink &events) {
  std::string folder = Text(request.arguments, "path");
  if (folder.empty()) {
    const std::string name = Text(request.arguments, "name");
    if (!name.empty()) folder = aeraui::RecoveryBackupRoot() + "/" + name;
  }
  if (folder.empty()) return Fail(events, "missing_backup", "Restore requires a backup name or path", 2);
  auto partitions = Strings(request.arguments, "parts");
  if (partitions.empty()) {
    for (const auto &volume : aeraui::RecoveryRestoreVolumes(folder))
      if (volume.path != "ADB Backup") partitions.push_back(volume.path);
  }
  JobRequest job;
  job.job = Job::kRestore;
  job.title = "Restore backup";
  job.path = folder;
  job.partitions = std::move(partitions);
  job.digest = Flag(request.arguments, "digest_check", true);
  return aeraui::RecoveryRunJob(job) == 0
      ? 0 : Fail(events, "restore_failed", "AERA could not restore the backup");
}

int Wipe(const Request &request, EventSink &events) {
  const auto partitions = Strings(request.arguments, "parts");
  if (partitions.empty()) {
    events.Data("wipe_targets", Volumes(aeraui::RecoveryVolumes("wipe"), false));
    return 0;
  }
  JobRequest job;
  job.job = Job::kWipe;
  job.title = "Wipe partitions";
  job.partitions = partitions;
  return aeraui::RecoveryRunJob(job) == 0
      ? 0 : Fail(events, "wipe_failed", "AERA could not wipe every selected partition");
}

int FormatData(const Request &request, EventSink &events) {
  if (!Flag(request.arguments, "confirm"))
    return Fail(events, "confirmation_required", "Format Data requires explicit confirmation", 2);
  JobRequest job;
  job.job = Job::kFormatData;
  job.title = "Format Data";
  job.path = "/data";
  job.confirmation = "yes";
  return aeraui::RecoveryRunJob(job) == 0
      ? 0 : Fail(events, "format_failed", "AERA could not format /data");
}

int Wifi(const Request &request, EventSink &events) {
  const std::string action = Text(request.arguments, "action", "status");
  if (action == "status" || action == "list") {
    const auto status = aeraui::RecoveryWifiStatus();
    Json::Value value(Json::objectValue);
    value["supported"] = status.supported;
    value["enabled"] = status.enabled;
    value["connected"] = status.connected;
    value["busy"] = status.busy;
    value["state"] = status.state;
    value["ssid"] = status.ssid;
    value["ip_address"] = status.ip_address;
    Json::Value networks(Json::arrayValue);
    for (const auto &network : status.networks) {
      Json::Value item(Json::objectValue);
      item["ssid"] = network.ssid;
      item["security"] = network.security;
      item["saved"] = network.saved;
      item["connected"] = network.connected;
      networks.append(std::move(item));
    }
    value["networks"] = std::move(networks);
    events.Data("wifi", value);
    return 0;
  }
  aeraui::WifiRequest wifi;
  if (action == "enable" || action == "start") wifi.operation = aeraui::WifiOperation::kEnable;
  else if (action == "disable" || action == "stop") wifi.operation = aeraui::WifiOperation::kDisable;
  else if (action == "disconnect") wifi.operation = aeraui::WifiOperation::kDisconnect;
  else if (action == "scan") wifi.operation = aeraui::WifiOperation::kScan;
  else if (action == "connect") wifi.operation = aeraui::WifiOperation::kConnect;
  else if (action == "forget") wifi.operation = aeraui::WifiOperation::kForget;
  else if (action == "test") wifi.operation = aeraui::WifiOperation::kTest;
  else return Fail(events, "invalid_wifi_action", "Unknown Wi-Fi action: " + action, 2);
  wifi.ssid = Text(request.arguments, "ssid");
  wifi.password = Text(request.arguments, "password");
  wifi.use_saved_credentials = wifi.password.empty();
  return aeraui::RecoveryRunWifi(wifi) == 0
      ? 0 : Fail(events, "wifi_failed", "The Wi-Fi operation failed");
}

int Partition(const Request &request, EventSink &events) {
  const std::string path = Text(request.arguments, "path");
  const std::string action = Text(request.arguments, "action", "list");
  if (path.empty() || action == "list") {
    events.Data("partitions", Volumes(aeraui::RecoveryVolumes("part_option"), false));
    return 0;
  }
  int success = 0;
  if (action == "repair") success = PartitionManager.Repair_By_Path(path, true);
  else if (action == "resize") success = PartitionManager.Resize_By_Path(path, true);
  else if (action == "wipe") {
    const std::string filesystem = Text(request.arguments, "fs");
    success = filesystem.empty() ? PartitionManager.Wipe_By_Path(path)
                                 : PartitionManager.Wipe_By_Path(path, filesystem);
  } else {
    return Fail(events, "invalid_partition_action", "Unknown partition action: " + action, 2);
  }
  PartitionManager.Update_System_Details();
  return success ? 0 : Fail(events, "partition_operation_failed", action + " failed for " + path);
}

int Reboot(const Request &request, EventSink &events) {
  const std::string target = Text(request.arguments, "target", "system");
  RebootCommand command;
  if (target == "system" || target == "android") command = rb_system;
  else if (target == "recovery") command = rb_recovery;
  else if (target == "bootloader") command = rb_bootloader;
  else if (target == "fastbootd") command = rb_fastboot;
  else if (target == "poweroff" || target == "shutdown") command = rb_poweroff;
  else if (target == "download") command = rb_download;
  else if (target == "edl") command = rb_edl;
  else return Fail(events, "invalid_reboot_target", "Unknown reboot target: " + target, 2);
  events.Log("Rebooting to " + target + "\n");
  sync();
  TWFunc::tw_reboot(command);
  return 0;
}

int Transition(const Request &request, EventSink &events) {
  const std::string target = Text(request.arguments, "target");
  if (target != "fastboot" && target != "recovery")
    return Fail(events, "invalid_transition_target",
                "Transition target must be fastboot or recovery", 2);

  const bool toward_fastboot = target == "fastboot";
  switch (aeraui::RequestModeTransition(toward_fastboot)) {
    case aeraui::ModeTransitionRequestResult::kAccepted:
      events.Log("Transitioning to " + target + "\n");
      return 0;
    case aeraui::ModeTransitionRequestResult::kAlreadyActive:
      events.Log("Already in " + target + "\n");
      return 0;
    case aeraui::ModeTransitionRequestResult::kBusy:
      return Fail(events, "transition_busy",
                  "Another mode transition is already running");
    case aeraui::ModeTransitionRequestResult::kUnavailable:
      return Fail(events, "transition_unavailable",
                  "The native AERA interface is not available");
  }
  return Fail(events, "transition_failed", "Could not queue the transition");
}

int Log(EventSink &events) {
  std::ifstream input("/tmp/recovery.log");
  if (!input) return Fail(events, "log_unavailable", "The recovery log is not available");
  std::string line;
  while (std::getline(input, line)) events.Log(line + "\n");
  return 0;
}

int Mirror(const Request &request, EventSink &events) {
  const std::string action = Text(request.arguments, "action", "status");
  if (action == "start") {
    const int port = Number(request.arguments, "port", 80);
    if (!aera::remote::Start(port))
      return Fail(events, "mirror_start_failed", "AERA Remote could not start");
  } else if (action == "stop") {
    aera::remote::Stop();
  } else if (action != "status") {
    return Fail(events, "invalid_mirror_action", "Unknown mirror action: " + action, 2);
  }

  Json::Value value(Json::objectValue);
  value["running"] = aera::remote::Running();
  value["port"] = aera::remote::Port();
  value["address"] = aera::remote::Address();
  value["access_code"] = aera::remote::AccessCode();
  if (aera::remote::Running()) {
    const std::string address = value["address"].asString();
    value["url"] = "http://" + (address.empty() ? std::string("127.0.0.1") : address) +
                   ":" + std::to_string(aera::remote::Port()) +
                   "/?code=" + aera::remote::AccessCode();
  }
  events.Data("mirror", value);
  return 0;
}

}  // namespace

int Execute(const Request &request, EventSink &events) {
  const std::string &op = request.operation;
  if (op == "status") return Status(events);
  if (op == "storages") {
    events.Data("storages", Volumes(aeraui::RecoveryVolumes("storage"), false));
    return 0;
  }
  if (op == "mount") return Mount(request, events, false);
  if (op == "unmount" || op == "umount") return Mount(request, events, true);
  if (op == "flash" || op == "install") return Install(request, events);
  if (op == "backup") return Backup(request, events);
  if (op == "restore") return Restore(request, events);
  if (op == "wipe") return Wipe(request, events);
  if (op == "format_data") return FormatData(request, events);
  if (op == "decrypt") {
    const std::string password = Text(request.arguments, "password");
    if (password.empty()) return Fail(events, "missing_password", "Decrypt requires a credential", 2);
    return aeraui::RecoveryDecrypt(password, Number(request.arguments, "user")) == 0
        ? 0 : Fail(events, "decrypt_failed", "The supplied credential did not unlock storage");
  }
  if (op == "sideload") {
    JobRequest job;
    job.job = Job::kSideload;
    job.title = "ADB sideload";
    return aeraui::RecoveryRunJob(job) == 0
        ? 0 : Fail(events, "sideload_failed", "ADB sideload failed or was cancelled");
  }
  if (op == "wlan" || op == "wifi") return Wifi(request, events);
  if (op == "partition") return Partition(request, events);
  if (op == "mtp") {
    const std::string action = Text(request.arguments, "action", "status");
    if (action == "status") {
      Json::Value value(Json::objectValue);
      value["enabled"] = aeraui::RecoveryMtpEnabled();
      events.Data("mtp", value);
      return 0;
    }
    const bool enabled = action == "enable" || action == "start";
    if (!enabled && action != "disable" && action != "stop")
      return Fail(events, "invalid_mtp_action", "Unknown MTP action: " + action, 2);
    return aeraui::RecoverySetMtp(enabled)
        ? 0 : Fail(events, "mtp_failed", "Could not change MTP state");
  }
  if (op == "reboot") return Reboot(request, events);
  if (op == "transition") return Transition(request, events);
  if (op == "log") return Log(events);
  if (op == "mirror") return Mirror(request, events);
  return Fail(events, "unsupported_operation",
              "This AERA RPC build does not support operation: " + op, 2);
}

}  // namespace aera::rpc
