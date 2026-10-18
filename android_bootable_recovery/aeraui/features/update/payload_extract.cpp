/* Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "payload_inspector.hpp"
#include "payload_extract.hpp"
#include "payload_otaripper.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <bzlib.h>
#include <xz.h>
#include "update_engine/common/zstd_decode.h"
#include <openssl/sha.h>
#include <ziparchive/zip_archive.h>

namespace aeraui::payload {
namespace {
using chromeos_update_engine::DeltaArchiveManifest;
using chromeos_update_engine::PartitionUpdate;
constexpr uint64_t kOperationLimit = 64ULL * 1024 * 1024;
struct Fd {
  int value;
  ~Fd() { if (value >= 0) close(value); }
};
struct Archive {
  ZipArchiveHandle value = nullptr;
  ~Archive() { if (value) CloseArchive(value); }
};
bool Read(int fd, uint64_t offset, void *data, size_t length) {
  auto *p = static_cast<uint8_t *>(data);
  while (length) {
    const auto n = pread64(fd, p, length, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n; length -= n; offset += n;
  }
  return true;
}
bool Write(int fd, uint64_t offset, const uint8_t *data, size_t length) {
  while (length) {
    const auto n = pwrite64(fd, data, length, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data += n; length -= n; offset += n;
  }
  return true;
}
bool Hash(const void *data, size_t length, const std::string &expected) {
  uint8_t hash[SHA256_DIGEST_LENGTH];
  return expected.size() == sizeof(hash) && SHA256(static_cast<const uint8_t *>(data), length, hash) &&
      memcmp(hash, expected.data(), sizeof(hash)) == 0;
}
uint64_t Big(const uint8_t *p, unsigned bytes) {
  uint64_t value = 0;
  while (bytes--) value = (value << 8) | *p++;
  return value;
}
bool Decode(int type, std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
  if (type == 0) {
    if (in.size() != out.size()) return false;
    out.swap(in); return true;
  }
  if (type == 1) {
    unsigned length = out.size();
    return BZ2_bzBuffToBuffDecompress(reinterpret_cast<char *>(out.data()), &length,
        reinterpret_cast<char *>(in.data()), in.size(), 0, 0) == BZ_OK && length == out.size();
  }
  if (type == 14) {
    size_t offset = 0;
    return chromeos_update_engine::DecodeZstd(in.data(), in.size(), out.size(),
        [&](const void* bytes, size_t count) {
          memcpy(out.data() + offset, bytes, count);
          offset += count;
          return true;
        });
  }
  if (type != 8) return false;
  static std::once_flag crc;
  std::call_once(crc, xz_crc32_init);
  auto *decoder = xz_dec_init(XZ_DYNALLOC, kOperationLimit);
  if (!decoder) return false;
  xz_buf b{};
  b.in = in.data(); b.in_size = in.size(); b.out = out.data(); b.out_size = out.size();
  xz_ret result;
  do {
    const auto old_in = b.in_pos, old_out = b.out_pos;
    result = xz_dec_run(decoder, &b);
    if (result == XZ_OK && old_in == b.in_pos && old_out == b.out_pos) break;
  } while (result == XZ_OK);
  xz_dec_end(decoder);
  return result == XZ_STREAM_END && b.in_pos == b.in_size && b.out_pos == b.out_size;
}
}

std::string ExtractionError(const PartitionUpdate &p, uint64_t block, uint64_t data_size) {
  const auto &name = p.partition_name();
  if (name.empty() || name.size() > 128 ||
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
    return "Invalid partition name";
  if (!block || block > 1024 * 1024 || (block & (block - 1))) return "Unsupported block size";
  if (p.has_old_partition_info()) return "Requires an existing source image";
  const auto size = p.new_partition_info().size();
  if (!size || size > INT64_MAX || size % block || p.new_partition_info().hash().size() != SHA256_DIGEST_LENGTH)
    return "Missing or unsupported image size / SHA-256";
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  for (const auto &op : p.operations()) {
    const int type = op.type();
    if ((type != 0 && type != 1 && type != 6 && type != 7 && type != 8 && type != 14) ||
        op.src_extents_size() || op.has_src_sha256_hash()) return "Requires an incremental update operation";
    uint64_t bytes = 0;
    for (const auto &e : op.dst_extents()) {
      if (!e.num_blocks() || e.start_block() >= size / block || e.num_blocks() > size / block - e.start_block())
        return "Invalid destination extent";
      const auto length = e.num_blocks() * block;
      if (length > size - bytes) return "Operation size overflow";
      bytes += length;
      ranges.emplace_back(e.start_block() * block, (e.start_block() + e.num_blocks()) * block);
      if (ranges.size() > 1000000) return "Too many image extents";
    }
    if (!bytes) return "Empty destination operation";
    if (type == 6 || type == 7) {
      if (op.data_length()) return "Unexpected data in a zero operation";
    } else {
      if (bytes > kOperationLimit || !op.data_length() || op.data_length() > kOperationLimit)
        return "Operation exceeds the bounded extraction buffer";
      if (op.data_offset() > data_size || op.data_length() > data_size - op.data_offset())
        return "Operation data is outside the payload";
      if (op.data_sha256_hash().size() != SHA256_DIGEST_LENGTH) return "Missing operation SHA-256";
    }
  }
  std::sort(ranges.begin(), ranges.end());
  uint64_t end = 0;
  for (const auto &range : ranges) {
    if (range.first != end) return "Image has overlapping or incomplete extents";
    end = range.second;
  }
  if (end != size) return "Image requires additional generated data";
  return {};
}

static bool ProcessImages(const std::string &path, const std::string &manifest_hash,
                   const std::vector<std::string> &names, const std::string &directory,
                   std::string &error,
                   const std::function<void(const std::string &, uint64_t, uint64_t)> &progress,
                   const std::function<bool(int, std::string &)> &direct) {
  const auto started = std::chrono::steady_clock::now();
  auto fail = [&](const std::string &message) { error = message; return false; };
  if (names.empty() || names.size() > 512) return fail("No valid partition selection");
  const std::set<std::string> selected(names.begin(), names.end());
  if (selected.size() != names.size()) return fail("Duplicate partition selection");
  Fd fd{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  struct stat st{};
  if (fd.value < 0 || fstat(fd.value, &st) || !S_ISREG(st.st_mode)) return fail("Cannot open package");
  Archive archive;
  if (OpenArchiveFd(fd.value, path.c_str(), &archive.value, false)) return fail("Invalid ZIP package");
  ZipEntry64 entry{};
  std::array<uint8_t, 24> header{};
  if (FindEntry(archive.value, "payload.bin", &entry) || entry.method != kCompressStored ||
      entry.offset < 0 || entry.uncompressed_length < header.size() ||
      uint64_t(entry.offset) > uint64_t(st.st_size) || entry.uncompressed_length > uint64_t(st.st_size) - entry.offset ||
      !Read(fd.value, entry.offset, header.data(), header.size()) || memcmp(header.data(), "CrAU", 4) ||
      Big(header.data() + 4, 8) != 2) return fail("Unsupported or truncated payload");
  const auto manifest_size = Big(header.data() + 12, 8);
  const auto data_start = header.size() + manifest_size + Big(header.data() + 20, 4);
  if (!manifest_size || manifest_size > kOperationLimit || data_start > entry.uncompressed_length)
    return fail("Invalid payload header");
  std::string bytes(manifest_size, '\0');
  if (!Read(fd.value, entry.offset + header.size(), bytes.data(), bytes.size()) ||
      !Hash(bytes.data(), bytes.size(), manifest_hash)) return fail("Package changed since review; open it again");
  DeltaArchiveManifest manifest;
  if (!manifest.ParseFromString(bytes) || manifest.partitions_size() > 512) return fail("Invalid payload manifest");
  std::vector<const PartitionUpdate *> partitions;
  std::set<std::string> unique;
  uint64_t total = 0;
  for (const auto &p : manifest.partitions()) {
    if (!unique.insert(p.partition_name()).second) return fail("Duplicate partition in manifest");
    for (const auto &op : p.operations())
      if (p.has_old_partition_info() || op.src_extents_size() || op.has_src_sha256_hash() ||
          (op.type() != 0 && op.type() != 1 && op.type() != 6 && op.type() != 7 && op.type() != 8 && op.type() != 14))
        return fail("Incremental payloads are not supported by Advanced extraction");
    if (!selected.count(p.partition_name())) continue;
    auto reason = ExtractionError(p, manifest.block_size(), entry.uncompressed_length - data_start);
    if (!reason.empty()) return fail(p.partition_name() + ": " + reason);
    if (p.new_partition_info().size() > UINT64_MAX / 2 - total) return fail("Selected sizes overflow");
    total += p.new_partition_info().size(); partitions.push_back(&p);
  }
  if (partitions.size() != names.size()) return fail("Selected partition is missing from payload");
  // Keep the validated ZIP descriptor open for the entire direct transaction.
  // No extraction directory, temporary image, or free-space requirement.
  if (direct) return direct(fd.value, error);
  Fd output{open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
  struct statvfs space{};
  if (output.value < 0 || fstatvfs(output.value, &space)) return fail("Cannot access extraction storage");
  const auto available = static_cast<long double>(space.f_bavail) * space.f_frsize;
  if (available < static_cast<long double>(total) + 16 * 1024 * 1024) return fail("Not enough free storage for selected images");
  std::vector<std::string> created;
  auto cleanup = [&](const std::string &message) {
    for (const auto &name : created) unlinkat(output.value, name.c_str(), 0);
    return fail(message);
  };
  uint64_t done = 0;
  bool otaripper = access(OtaripperExecutable(), X_OK) == 0;
  if (!otaripper && errno != ENOENT)
    return fail("Otaripper exists but cannot be executed");
  if (otaripper) {
    // The subprocess may only populate a fresh, empty directory. This also
    // makes failure cleanup safe: pre-existing files are never removed.
    DIR *listing = fdopendir(dup(output.value));
    if (!listing) return fail("Cannot inspect extraction directory");
    bool empty = true;
    errno = 0;
    while (auto *entry = readdir(listing)) {
      if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) { empty = false; break; }
    }
    const int list_error = errno;
    closedir(listing);
    if (!empty || list_error) return fail("Otaripper requires an empty extraction directory");
    for (const auto *p : partitions) created.push_back(p->partition_name() + ".img");
    if (!RunOtaripper(fd.value, names, directory, total, progress, error)) return cleanup(error);
    fprintf(stderr, "AERA payload: otaripper extraction %.3f seconds, %llu bytes\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
        static_cast<unsigned long long>(total));
    done = total;
  }
  for (const auto *p : partitions) {
    const auto filename = p->partition_name() + ".img";
    Fd image{openat(output.value, filename.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC |
        (otaripper ? 0 : O_CREAT | O_EXCL), 0600)};
    if (image.value < 0) return cleanup("Cannot create " + filename + " (existing files are never overwritten)");
    if (!otaripper) created.push_back(filename);
    const auto size = p->new_partition_info().size();
    if (otaripper) {
      struct stat image_stat{};
      if (fstat(image.value, &image_stat) || !S_ISREG(image_stat.st_mode) ||
          uint64_t(image_stat.st_size) != size || image_stat.st_nlink != 1)
        return cleanup("Otaripper output has invalid type or size: " + filename);
    } else {
    if (ftruncate64(image.value, size)) return cleanup("Cannot allocate " + filename);
    for (const auto &op : p->operations()) {
      uint64_t count = 0;
      for (const auto &e : op.dst_extents()) count += e.num_blocks() * manifest.block_size();
      if (op.type() != 6 && op.type() != 7) {
        std::vector<uint8_t> input(op.data_length()), decoded(count);
        if (!Read(fd.value, entry.offset + data_start + op.data_offset(), input.data(), input.size()) ||
            !Hash(input.data(), input.size(), op.data_sha256_hash()) || !Decode(op.type(), input, decoded))
          return cleanup("Cannot decode or verify " + filename);
        size_t offset = 0;
        for (const auto &e : op.dst_extents()) {
          const auto length = e.num_blocks() * manifest.block_size();
          if (!Write(image.value, e.start_block() * manifest.block_size(), decoded.data() + offset, length))
            return cleanup("Cannot write " + filename);
          offset += length;
        }
      }
      done += count;
      if (progress) progress("Extracting " + filename, done, total * 2);
    }
    }
    SHA256_CTX hash;
    SHA256_Init(&hash);
    std::array<uint8_t, 256 * 1024> buffer;
    for (uint64_t offset = 0; offset < size;) {
      const auto length = std::min<uint64_t>(buffer.size(), size - offset);
      if (!Read(image.value, offset, buffer.data(), length)) return cleanup("Cannot verify " + filename);
      SHA256_Update(&hash, buffer.data(), length); offset += length;
      if (progress) progress("Verifying " + filename, done + offset, total * 2);
    }
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &hash);
    if (memcmp(digest, p->new_partition_info().hash().data(), sizeof(digest)) || fsync(image.value))
      return cleanup("Image hash or storage verification failed: " + filename);
    done += size;
  }
  fprintf(stderr, "AERA payload: %s extraction and verification %.3f seconds, %llu bytes\n",
      otaripper ? "otaripper" : "native",
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
      static_cast<unsigned long long>(total));
  return true;
}
bool ExtractImages(const std::string &path, const std::string &hash,
                   const std::vector<std::string> &names, const std::string &directory,
                   std::string &error,
                   const std::function<void(const std::string &, uint64_t, uint64_t)> &progress) {
  return ProcessImages(path, hash, names, directory, error, progress, {});
}
bool WithValidatedPayload(const std::string &path, const std::string &hash,
                          const std::vector<std::string> &names, std::string &error,
                          const std::function<bool(int, std::string &)> &write) {
  if (!write) { error = "Missing direct writer"; return false; }
  return ProcessImages(path, hash, names, {}, error, {}, write);
}
} // namespace aeraui::payload
