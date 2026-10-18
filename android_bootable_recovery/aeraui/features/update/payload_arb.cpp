/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 * Hash-segment selection also follows Jonas Salo's supplied arbextract.c.
 * Qualcomm OEM metadata validation follows otaripper's arbscan implementation:
 * https://github.com/syedinsaf/otaripper/blob/49289575260a5c7a4d2af0cb4be949cd2dcf138a/src/cmd/arbscan.rs
 * Copyright Syed Insaf, Apache-2.0.
 */
#include "payload_arb.hpp"
#include "update_engine/update_metadata.pb.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <bzlib.h>
#include <openssl/sha.h>
#include <xz.h>
#include "update_engine/common/zstd_decode.h"
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

namespace aeraui::payload {
namespace {
constexpr uint64_t kLimit = 16 * 1024 * 1024;
uint64_t Little(const uint8_t *p, unsigned n) {
  uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
  return v;
}
bool Hash(const std::vector<uint8_t> &data, const std::string &expected) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  return expected.size() == sizeof(digest) &&
      SHA256(data.data(), data.size(), digest) &&
      memcmp(digest, expected.data(), sizeof(digest)) == 0;
}
bool Read(int fd, uint64_t offset, std::vector<uint8_t> &data) {
  size_t pos = 0;
  while (pos < data.size()) {
    const auto n = pread64(fd, data.data() + pos, data.size() - pos, offset + pos);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    pos += n;
  }
  return true;
}
bool Decode(int type, std::vector<uint8_t> &input, std::vector<uint8_t> &out) {
  if (type == 0) {
    if (input.size() != out.size()) return false;
    out = input;
    return true;
  }
  if (type == 1) {
    unsigned size = out.size();
    return BZ2_bzBuffToBuffDecompress(reinterpret_cast<char *>(out.data()), &size,
        reinterpret_cast<char *>(input.data()), input.size(), 0, 0) == BZ_OK && size == out.size();
  }
  if (type == 14) {
    size_t offset = 0;
    return chromeos_update_engine::DecodeZstd(input.data(), input.size(), out.size(),
        [&](const void* bytes, size_t count) {
          memcpy(out.data() + offset, bytes, count);
          offset += count;
          return true;
        });
  }
  if (type != 8) return false;
  static std::once_flag crc;
  std::call_once(crc, xz_crc32_init);
  auto *decoder = xz_dec_init(XZ_DYNALLOC, 64 * 1024 * 1024);
  if (!decoder) return false;
  xz_buf b{};
  b.in = input.data(); b.in_size = input.size();
  b.out = out.data(); b.out_size = out.size();
  xz_ret result;
  do {
    const auto in = b.in_pos, written = b.out_pos;
    result = xz_dec_run(decoder, &b);
    if (result == XZ_OK && in == b.in_pos && written == b.out_pos) break;
  } while (result == XZ_OK);
  xz_dec_end(decoder);
  return result == XZ_STREAM_END && b.in_pos == b.in_size && b.out_pos == b.out_size;
}
}
DeviceArb ReadDeviceArb() {
  auto read_slot = [](const char *name) -> Arb {
    int fd = -1;
    for (const char *root : {"/dev/block/by-name/", "/dev/block/bootdevice/by-name/"}) {
      const auto path = std::string(root) + name;
      fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
      if (fd >= 0) break;
      if (errno != ENOENT && errno != ENOTDIR)
        return {false, 0, "Cannot open " + std::string(name)};
    }
    if (fd < 0) return {false, 0, "Partition not found: " + std::string(name)};
    struct stat st{};
    uint64_t bytes = 0;
    if (fstat(fd, &st) || !S_ISBLK(st.st_mode) || ioctl(fd, BLKGETSIZE64, &bytes) ||
        bytes < 64 || bytes > kLimit) {
      close(fd);
      return {false, 0, "Cannot determine bounded partition size: " + std::string(name)};
    }
    std::vector<uint8_t> image(bytes);
    const bool ok = Read(fd, 0, image);
    close(fd);
    if (!ok) return {false, 0, "Cannot read " + std::string(name)};
    auto result = ReadFirmwareArb(image);
    result.detail = std::string(name) + ": " + (result.available ?
        "installed firmware metadata; not the hardware fuse value" : result.detail);
    return result;
  };
  return {read_slot("xbl_config_a"), read_slot("xbl_config_b")};
}
ArbDecision CompareArb(bool includes_firmware, const Arb &package, const DeviceArb &device) {
  if (!includes_firmware) return ArbDecision::NotApplicable;
  if (!package.available) return ArbDecision::Unknown;
  // A known downgrade must never be hidden by an unreadable second slot.
  if ((device.slot_a.available && package.index < device.slot_a.index) ||
      (device.slot_b.available && package.index < device.slot_b.index))
    return ArbDecision::Downgrade;
  if (!device.slot_a.available || !device.slot_b.available) return ArbDecision::Unknown;
  if (package.index > device.slot_a.index || package.index > device.slot_b.index)
    return ArbDecision::Upgrade;
  return ArbDecision::Same;
}
bool ArbAllowsInstall(ArbDecision decision, bool upgrade_acknowledged) {
  return decision == ArbDecision::NotApplicable || decision == ArbDecision::Unknown ||
      decision == ArbDecision::Same ||
      (decision == ArbDecision::Upgrade && upgrade_acknowledged);
}
Arb ReadFirmwareArb(const std::vector<uint8_t> &image) {
  Arb result{false, 0, "Unsupported Qualcomm OEM metadata"};
  if (image.size() < 64 || memcmp(image.data(), "\177ELF", 4) || image[4] != 2 || image[5] != 1)
    return result;
  const auto table = Little(image.data() + 32, 8);
  const auto stride = Little(image.data() + 54, 2), count = Little(image.data() + 56, 2);
  if (stride < 56 || !count || count * stride > 65536 || table > image.size() || count * stride > image.size() - table)
    return result;
  for (uint64_t i = 0; i < count; ++i) {
    const auto *ph = image.data() + table + i * stride;
    const auto flags = Little(ph + 4, 4), offset = Little(ph + 8, 8), size = Little(ph + 32, 8);
    if (Little(ph, 4) != 0 || (flags & 1) || offset > image.size() || size > image.size() - offset || size < 36) continue;
    const auto *segment = image.data() + offset;
    for (uint64_t j = 0; j < std::min<uint64_t>(4096, size); j += 4) {
      if (size - j < 36) break;
      const auto *h = segment + j;
      const auto version = Little(h, 4), common = Little(h + 4, 4), qti = Little(h + 8, 4);
      const auto oem = Little(h + 12, 4), hashes = Little(h + 16, 4);
      if (version < 1 || version > 10 || common > 4096 || qti > 4096 ||
          oem < 12 || oem > 16384 || !hashes || hashes % 16 ||
          36 + common + qti + oem > size - j ||
          hashes > size - j - (36 + common + qti + oem)) continue;
      const auto *md = h + 36 + common + qti;
      const auto major = Little(md, 4), minor = Little(md + 4, 4), arb = Little(md + 8, 4);
      if (major >= 1000 || minor >= 1000 || arb >= 128) continue;
      if (result.available && result.index != arb)
        return {false, 0, "Conflicting OEM indices in xbl_config"};
      result = {true, static_cast<uint32_t>(arb), "Qualcomm OEM metadata in xbl_config; package value only"};
      break;
    }
  }
  return result;
}
Arb InspectArb(int fd, uint64_t base, uint64_t length,
               const chromeos_update_engine::DeltaArchiveManifest &manifest) {
  for (const auto &p : manifest.partitions()) {
    if (p.partition_name() != "xbl_config") continue;
    const auto size = p.new_partition_info().size();
    const auto block = manifest.block_size();
    if (!size || size > kLimit || !block || block > 65536 || p.operations_size() > 4096)
      return {false, 0, "xbl_config exceeds scan limits"};
    if (p.new_partition_info().hash().size() != SHA256_DIGEST_LENGTH)
      return {false, 0, "xbl_config has no SHA-256 integrity hash"};
    std::vector<uint8_t> image(size, 0);
    uint64_t work = 0;
    for (const auto &op : p.operations()) {
      const auto type = static_cast<int>(op.type());
      if (type != 0 && type != 1 && type != 8 && type != 6 && type != 14)
        return {false, 0, "xbl_config requires an unsupported or source-dependent operation"};
      uint64_t expanded = 0;
      for (const auto &extent : op.dst_extents()) {
        if (extent.start_block() > size / block || extent.num_blocks() > size / block ||
            extent.num_blocks() * block > size - extent.start_block() * block ||
            extent.num_blocks() * block > kLimit - expanded)
          return {false, 0, "Invalid xbl_config extents"};
        expanded += extent.num_blocks() * block;
      }
      if (op.data_length() > kLimit || expanded + op.data_length() > 4 * kLimit - work ||
          op.data_offset() > length || op.data_length() > length - op.data_offset())
        return {false, 0, "xbl_config operation exceeds scan limits"};
      work += expanded + op.data_length();
      std::vector<uint8_t> output(expanded, 0);
      if (type != 6) {
        std::vector<uint8_t> data(op.data_length());
        if (!Read(fd, base + op.data_offset(), data) ||
            (op.has_data_sha256_hash() && !Hash(data, op.data_sha256_hash())) ||
            !Decode(type, data, output))
          return {false, 0, "Cannot decode or verify xbl_config"};
      }
      size_t pos = 0;
      for (const auto &extent : op.dst_extents()) {
        const size_t bytes = extent.num_blocks() * block;
        if (bytes) std::copy_n(output.data() + pos, bytes, image.data() + extent.start_block() * block);
        pos += bytes;
      }
    }
    if (!Hash(image, p.new_partition_info().hash()))
      return {false, 0, "xbl_config image hash mismatch"};
    return ReadFirmwareArb(image);
  }
  return {false, 0, "No xbl_config image in this payload"};
}
}
