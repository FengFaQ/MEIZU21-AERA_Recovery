/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aeraui::payload {

struct Partition {
  std::string name;
  uint64_t bytes = 0;
  uint64_t operations = 0;
  bool extractable = false;
  std::string extraction_error;
  std::string sha256; // Raw 32-byte final image digest from the payload manifest.
};

struct Info {
  bool is_payload = false;
  bool valid = false;
  bool incremental = false;
  bool partial = false;
  bool dynamic_partitions = false;
  bool snapshots = false;
  bool virtual_ab_compression = false;
  uint64_t payload_bytes = 0;
  uint64_t expanded_bytes = 0;
  uint64_t operations = 0;
  uint64_t max_timestamp = 0;
  uint32_t format_version = 0;
  uint32_t minor_version = 0;
  std::string target_device;
  std::string target_build;
  std::string build_id;
  std::string device_codename;
  bool name_from_filename = false;
  std::string system_fingerprint;
  bool arb_available = false;
  uint32_t arb_index = 0;
  std::string arb_detail;
  std::string target_sdk;
  std::string security_patch;
  std::string operation_types;
  std::string error;
  std::string manifest_hash;
  std::vector<Partition> partitions;
};

// Reads the ZIP directory, OTA metadata and payload manifest. Optional ARB
// inspection also reconstructs bounded xbl_config data in RAM. No device writes.
Info InspectZip(const std::string &path, bool read_arb = false);

// Writes only new regular files into an existing directory. Never opens a
// device partition. All selected images must pass SHA-256 or they are removed.
bool ExtractImages(const std::string &zip, const std::string &manifest_hash,
                   const std::vector<std::string> &names, const std::string &directory,
                   std::string &error,
                   const std::function<void(const std::string &, uint64_t, uint64_t)> &progress = {});

// Compact, display-ready preflight summary for the install confirmation.
// Runs only after complete selected-image preflight; callback borrows the exact ZIP fd.
bool WithValidatedPayload(const std::string &zip, const std::string &manifest_hash,
                          const std::vector<std::string> &names, std::string &error,
                          const std::function<bool(int, std::string &)> &write);

std::string Summary(const Info &info);

}  // namespace aeraui::payload
