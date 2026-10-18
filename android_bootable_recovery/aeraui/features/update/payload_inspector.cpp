/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "payload_inspector.hpp"
#include "payload_arb.hpp"
#include "payload_extract.hpp"
#include <openssl/sha.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <limits>
#include <utility>

#include <ziparchive/zip_archive.h>

#include "update_engine/update_metadata.pb.h"
#include "ota_metadata.pb.h"

namespace aeraui::payload {
namespace {

using chromeos_update_engine::DeltaArchiveManifest;
using chromeos_update_engine::InstallOperation;

constexpr uint64_t kMaximumManifestBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumMetadataBytes = 512ULL * 1024ULL;

uint32_t ReadBigEndian32(const uint8_t *value) {
  return (static_cast<uint32_t>(value[0]) << 24U) |
         (static_cast<uint32_t>(value[1]) << 16U) |
         (static_cast<uint32_t>(value[2]) << 8U) |
         static_cast<uint32_t>(value[3]);
}

uint64_t ReadBigEndian64(const uint8_t *value) {
  uint64_t result = 0;
  for (size_t index = 0; index < 8; ++index)
    result = (result << 8U) | static_cast<uint64_t>(value[index]);
  return result;
}

bool ReadExactly(int fd, uint64_t offset, void *buffer, size_t bytes) {
  auto *next = static_cast<uint8_t *>(buffer);
  size_t remaining = bytes;
  while (remaining != 0) {
    const ssize_t result = pread64(fd, next, remaining,
                                   static_cast<off64_t>(offset));
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) return false;
    next += result;
    remaining -= static_cast<size_t>(result);
    offset += static_cast<uint64_t>(result);
  }
  return true;
}

bool ExtractSmallText(ZipArchiveHandle archive, const char *name,
                      std::string &output) {
  ZipEntry64 entry;
  if (FindEntry(archive, name, &entry) != 0 ||
      entry.uncompressed_length > kMaximumMetadataBytes ||
      entry.uncompressed_length > SIZE_MAX) {
    return false;
  }
  output.assign(static_cast<size_t>(entry.uncompressed_length), '\0');
  return ExtractToMemory(archive, &entry,
                         reinterpret_cast<uint8_t *>(output.data()),
                         output.size()) == 0;
}

std::map<std::string, std::string> Properties(const std::string &text) {
  std::map<std::string, std::string> result;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t equal = line.find('=');
    if (equal == std::string::npos || equal == 0) continue;
    std::string value = line.substr(equal + 1, 512);
    for (char &ch : value)
      if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = ' ';
    result[line.substr(0, equal)] = value;
  }
  return result;
}

std::string Property(const std::map<std::string, std::string> &properties,
                     const char *name) {
  const auto found = properties.find(name);
  return found == properties.end() ? std::string{} : found->second;
}

std::string AndroidVersion(const std::string &sdk) {
  if (sdk.empty()) return {};
  const std::map<std::string, std::string> versions = {
      {"29", "Android 10"}, {"30", "Android 11"},
      {"31", "Android 12"}, {"32", "Android 12L"},
      {"33", "Android 13"}, {"34", "Android 14"},
      {"35", "Android 15"}, {"36", "Android 16"},
  };
  const auto found = versions.find(sdk);
  return found == versions.end() ? "SDK " + sdk : found->second;
}

const char *OperationName(InstallOperation::Type type) {
  switch (type) {
    case InstallOperation::REPLACE: return "raw";
    case InstallOperation::REPLACE_BZ: return "bzip2";
    case InstallOperation::MOVE: return "move";
    case InstallOperation::BSDIFF: return "bsdiff";
    case InstallOperation::SOURCE_COPY: return "source copy";
    case InstallOperation::SOURCE_BSDIFF: return "source bsdiff";
    case InstallOperation::ZERO: return "zero";
    case InstallOperation::DISCARD: return "discard";
    case InstallOperation::REPLACE_XZ: return "xz";
    case InstallOperation::REPLACE_ZSTD: return "zstd";
    case InstallOperation::PUFFDIFF: return "puffdiff";
    case InstallOperation::BROTLI_BSDIFF: return "brotli diff";
    case InstallOperation::ZUCCHINI: return "zucchini";
    case InstallOperation::LZ4DIFF_BSDIFF: return "lz4 bsdiff";
    case InstallOperation::LZ4DIFF_PUFFDIFF: return "lz4 puffdiff";
  }
  return "unknown";
}

std::string Join(const std::vector<std::string> &items,
                 const char *separator) {
  std::string result;
  for (const auto &item : items) {
    if (!result.empty()) result += separator;
    result += item;
  }
  return result;
}

std::string Size(uint64_t bytes) {
  char value[48];
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    snprintf(value, sizeof(value), "%.2f GiB",
             static_cast<double>(bytes) /
                 static_cast<double>(1024ULL * 1024ULL * 1024ULL));
  } else if (bytes >= 1024ULL * 1024ULL) {
    snprintf(value, sizeof(value), "%.1f MiB",
             static_cast<double>(bytes) /
                 static_cast<double>(1024ULL * 1024ULL));
  } else {
    snprintf(value, sizeof(value), "%.1f KiB",
             static_cast<double>(bytes) / 1024.0);
  }
  return value;
}

std::string PartitionNames(const std::vector<Partition> &partitions) {
  std::vector<std::string> names;
  const size_t count = partitions.size();
  names.reserve(count);
  for (size_t index = 0; index < count; ++index)
    names.push_back(partitions[index].name);
  std::string result = Join(names, ", ");
  if (partitions.size() > count)
    result += "  +" + std::to_string(partitions.size() - count) + " more";
  return result;
}

}  // namespace

Info InspectZip(const std::string &path, bool read_arb) {
  Info info;
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat file_info {};
  if (fd < 0 || fstat(fd, &file_info) != 0 || !S_ISREG(file_info.st_mode)) {
    if (fd >= 0) close(fd);
    info.error = "The selected package could not be opened.";
    return info;
  }

  ZipArchiveHandle archive = nullptr;
  if (OpenArchiveFd(fd, path.c_str(), &archive, true) != 0) {
    if (archive) CloseArchive(archive);
    else close(fd);
    info.error = "The selected file is not a valid ZIP package.";
    return info;
  }

  ZipEntry64 payload_entry;
  if (FindEntry(archive, "payload.bin", &payload_entry) != 0) {
    CloseArchive(archive);
    return info;
  }
  info.is_payload = true;
  info.payload_bytes = payload_entry.uncompressed_length;

  if (payload_entry.method != kCompressStored) {
    info.error = "payload.bin is compressed inside the ZIP and cannot be "
                 "streamed by the recovery installer.";
    CloseArchive(archive);
    return info;
  }

  uint8_t header[24]{};
  if (payload_entry.uncompressed_length < sizeof(header) ||
      !ReadExactly(fd, static_cast<uint64_t>(payload_entry.offset), header,
                   sizeof(header)) ||
      memcmp(header, "CrAU", 4) != 0) {
    info.error = "payload.bin has an invalid update payload header.";
    CloseArchive(archive);
    return info;
  }

  const uint64_t format_version = ReadBigEndian64(header + 4);
  const uint64_t manifest_bytes = ReadBigEndian64(header + 12);
  const uint64_t signature_bytes = format_version >= 2
                                       ? ReadBigEndian32(header + 20)
                                       : 0;
  const uint64_t header_bytes = format_version >= 2 ? 24 : 20;
  if (format_version != 2 ||
      manifest_bytes == 0 || manifest_bytes > kMaximumManifestBytes ||
      manifest_bytes > INT_MAX ||
      header_bytes + manifest_bytes + signature_bytes >
          payload_entry.uncompressed_length) {
    info.error = "The payload manifest header is unsupported or truncated.";
    CloseArchive(archive);
    return info;
  }

  std::string manifest_data(static_cast<size_t>(manifest_bytes), '\0');
  if (!ReadExactly(fd,
                   static_cast<uint64_t>(payload_entry.offset) + header_bytes,
                   manifest_data.data(), manifest_data.size())) {
    info.error = "The payload manifest could not be read.";
    CloseArchive(archive);
    return info;
  }

  DeltaArchiveManifest manifest;
  unsigned char manifest_digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t *>(manifest_data.data()), manifest_data.size(), manifest_digest);
  info.manifest_hash.assign(reinterpret_cast<const char *>(manifest_digest), sizeof(manifest_digest));
  if (!manifest.ParseFromArray(manifest_data.data(),
                               static_cast<int>(manifest_data.size()))) {
    info.error = "The payload manifest is malformed or uses an unsupported "
                 "schema.";
    CloseArchive(archive);
    return info;
  }
  if (manifest.partitions_size() > 512) {
    info.error = "The payload contains too many partition entries.";
    CloseArchive(archive);
    return info;
  }

  std::string metadata_text;
  const auto metadata =
      ExtractSmallText(archive, "META-INF/com/android/metadata", metadata_text)
          ? Properties(metadata_text)
          : std::map<std::string, std::string>{};

  build::tools::releasetools::OtaMetadata ota;
  std::string ota_bytes;
  const bool has_ota = ExtractSmallText(archive, "META-INF/com/android/metadata.pb", ota_bytes) &&
      ota.ParseFromArray(ota_bytes.data(), static_cast<int>(ota_bytes.size()));

  info.format_version = static_cast<uint32_t>(format_version);
  info.minor_version = manifest.minor_version();
  info.incremental = false;
  info.partial = manifest.partial_update();
  info.max_timestamp = manifest.max_timestamp() > 0
                           ? static_cast<uint64_t>(manifest.max_timestamp())
                           : 0;
  info.device_codename = Property(metadata, "pre-device");
  info.target_device = Property(metadata, "product_name");
  if (info.target_device.empty()) info.target_device = info.device_codename;
  info.build_id = Property(metadata, "post-build-incremental");
  info.target_build = Property(metadata, "version_name");
  if (info.target_build.empty()) info.target_build = Property(metadata, "version_name_show");
  if (info.target_build.empty()) {
    info.target_build = path.substr(path.find_last_of('/') + 1);
    if (info.target_build.size() > 4 &&
        (info.target_build.compare(info.target_build.size() - 4, 4, ".zip") == 0 ||
         info.target_build.compare(info.target_build.size() - 4, 4, ".ZIP") == 0))
      info.target_build.resize(info.target_build.size() - 4);
    info.name_from_filename = true;
  }
  info.target_sdk = AndroidVersion(Property(metadata, "post-sdk-level"));
  info.security_patch = Property(metadata, "post-security-patch-level");
  if (info.security_patch.empty())
    info.security_patch = manifest.security_patch_level();

  if (has_ota) {
    const auto &post = ota.postcondition();
    if (info.build_id.empty()) info.build_id = post.build_incremental();
    if (info.target_sdk.empty()) info.target_sdk = AndroidVersion(post.sdk_level());
    if (info.security_patch.empty()) info.security_patch = post.security_patch_level();
    for (const auto &partition : post.partition_state()) {
      if (partition.partition_name() != "system" || partition.build_size() == 0) continue;
      const auto &fingerprint = partition.build(0);
      info.system_fingerprint = fingerprint;
      const auto colon = fingerprint.find(':');
      const auto release_end = fingerprint.find('/', colon == std::string::npos ? 0 : colon + 1);
      const auto device_start = fingerprint.rfind('/', colon);
      if (colon != std::string::npos && release_end != std::string::npos && device_start != std::string::npos) {
        const auto release = fingerprint.substr(colon + 1, release_end - colon - 1);
        // A custom ROM can retain a stock global fingerprint. Its system
        // partition fingerprint describes the OS being installed instead.
        if (!release.empty() && release.size() < 24)
          info.target_sdk = "Android " + release;
        if (Property(metadata, "product_name").empty())
          info.target_device = fingerprint.substr(device_start + 1, colon - device_start - 1);
      }
    }
  }

  if (manifest.has_dynamic_partition_metadata()) {
    const auto &dynamic = manifest.dynamic_partition_metadata();
    info.dynamic_partitions = dynamic.groups_size() != 0;
    info.snapshots = dynamic.snapshot_enabled();
    info.virtual_ab_compression = dynamic.vabc_enabled();
  }

  std::set<std::string> operation_types;
  info.partitions.reserve(static_cast<size_t>(manifest.partitions_size()));
  for (const auto &partition : manifest.partitions()) {
    Partition entry;
    entry.name = partition.partition_name();
    if (entry.name.empty() || entry.name.size() > 128 ||
        entry.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
      info.error = "The manifest contains an invalid partition name.";
      CloseArchive(archive);
      return info;
    }
    entry.bytes = partition.has_new_partition_info()
                      ? partition.new_partition_info().size()
                      : 0;
    entry.operations = static_cast<uint64_t>(partition.operations_size());
    entry.sha256 = partition.new_partition_info().hash();
    entry.extraction_error = ExtractionError(partition, manifest.block_size(),
        payload_entry.uncompressed_length - header_bytes - manifest_bytes - signature_bytes);
    entry.extractable = entry.extraction_error.empty();
    if (entry.bytes > std::numeric_limits<uint64_t>::max() - info.expanded_bytes) {
      info.error = "The manifest partition sizes overflow.";
      CloseArchive(archive);
      return info;
    }
    info.expanded_bytes += entry.bytes;
    info.operations += entry.operations;
    if (partition.has_old_partition_info()) info.incremental = true;
    for (const auto &operation : partition.operations()) {
      const int type = static_cast<int>(operation.type());
      if (operation.src_extents_size() || operation.has_src_sha256_hash() ||
          (type != 0 && type != 1 && type != 6 && type != 7 && type != 8 && type != 14))
        info.incremental = true;
      operation_types.insert(OperationName(operation.type()));
    }
    info.partitions.push_back(std::move(entry));
  }

  std::vector<std::string> operations(operation_types.begin(),
                                      operation_types.end());
  info.operation_types = Join(operations, ", ");
  info.valid = !info.partitions.empty();
  if (info.valid && read_arb) {
    const auto data_offset = header_bytes + manifest_bytes + signature_bytes;
    const auto arb = InspectArb(fd, payload_entry.offset + data_offset,
                                payload_entry.uncompressed_length - data_offset, manifest);
    info.arb_available = arb.available;
    info.arb_index = arb.index;
    info.arb_detail = arb.detail;
  }
  if (!info.valid) info.error = "The payload manifest contains no partitions.";
  CloseArchive(archive);
  return info;
}

std::string Summary(const Info &info) {
  if (!info.is_payload) {
    return "Installer ZIP\nThe package's own installer controls which "
           "partitions are changed.";
  }
  if (!info.valid) {
    return "PAYLOAD OTA / PREFLIGHT FAILED\n" +
           (info.error.empty() ? "The payload manifest could not be read."
                               : info.error);
  }

  std::string mode = info.incremental ? "Incremental OTA" : "Full OTA";
  if (info.partial) mode += " / partial";
  std::string layout;
  if (info.dynamic_partitions) layout = "Dynamic partitions";
  if (info.snapshots) layout += layout.empty() ? "Snapshots" : " / snapshots";
  if (info.virtual_ab_compression)
    layout += layout.empty() ? "Virtual A/B compression" : " / Virtual A/B compression";

  std::string result = mode;
  if (!info.target_device.empty()) result += "\nDevice: " + info.target_device;
  if (!info.target_sdk.empty()) result += "  /  " + info.target_sdk;
  if (!info.target_build.empty()) result += "\nBuild: " + info.target_build;
  if (!info.security_patch.empty())
    result += "\nSecurity patch: " + info.security_patch;
  result += "\nPayload: " + Size(info.payload_bytes) +
            "  /  Expanded: " + Size(info.expanded_bytes);
  if (!layout.empty()) result += "\nLayout: " + layout;
  result += "\nPartitions (" + std::to_string(info.partitions.size()) +
            "): " + PartitionNames(info.partitions);
  result += "\nOperations: " + std::to_string(info.operations);
  if (!info.operation_types.empty()) result += "  /  " + info.operation_types;
  return result;
}

}  // namespace aeraui::payload
