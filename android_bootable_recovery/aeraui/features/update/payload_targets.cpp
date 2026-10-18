/* SPDX-License-Identifier: Apache-2.0 */
#include "payload_targets.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace aeraui::payload {
namespace {
bool ValidName(const std::string &name) {
  if (name.empty() || name.size() > 128 || name == "super" || name == "userdata" ||
      name == "metadata" || name == "misc" || name == "persist" || name == "cache" ||
      name == "frp" || name == "devinfo" || name == "modemst1" || name == "modemst2" ||
      name == "fsg" || name == "fsc" || name == "config" || name == "keystore") return false;
  if (name.size() > 2 && (name.compare(name.size()-2, 2, "_a") == 0 ||
                         name.compare(name.size()-2, 2, "_b") == 0)) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
bool Geometry(int fd, uint64_t &device, uint64_t &bytes) {
  struct stat st{};
  int readonly = 1;
  if (fstat(fd, &st) || !S_ISBLK(st.st_mode) || ioctl(fd, BLKGETSIZE64, &bytes) ||
      !bytes || ioctl(fd, BLKROGET, &readonly) || readonly) return false;
  device = st.st_rdev;
  return true;
}
}
bool ProbeBlock(const std::string &path, RawNode &node) {
  char resolved[PATH_MAX];
  if (!realpath(path.c_str(), resolved) || strncmp(resolved, "/dev/block/", 11)) return false;
  int fd = open(resolved, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  uint64_t device = 0, bytes = 0;
  const bool valid = Geometry(fd, device, bytes);
  close(fd);
  if (!valid) return false;
  const std::string sys = "/sys/dev/block/" + std::to_string(major(device)) + ":" + std::to_string(minor(device));
  // A real kernel partition, never an entire disk or device-mapper alias.
  std::ifstream part(sys + "/partition");
  unsigned number = 0;
  if (!(part >> number) || !number) return false;
  std::ifstream uevent(sys + "/uevent");
  std::string line, name;
  while (std::getline(uevent, line)) if (line.rfind("PARTNAME=", 0) == 0) name = line.substr(9);
  if (name.empty()) return false;
  node = {resolved, name, bytes, device};
  return true;
}
PayloadFlashTarget DiscoverRawTarget(const std::string &name, const std::string &slot,
                                    const BlockProbe &probe) {
  PayloadFlashTarget target;
  target.name = name;
  target.reason = "No verified A/B firmware target on this device";
  if (!ValidName(name)) { target.reason = "Protected or invalid partition name"; return target; }
  if (slot != "A" && slot != "B") { target.reason = "Current slot is unknown"; return target; }
  for (const char *root : {"/dev/block/by-name/", "/dev/block/bootdevice/by-name/"}) {
    RawNode a, b;
    if (!probe(std::string(root) + name + "_a", a) ||
        !probe(std::string(root) + name + "_b", b)) continue;
    if (a.name != name + "_a" || b.name != name + "_b" || !a.device || !b.device ||
        a.device == b.device || !a.bytes || !b.bytes) continue;
    const auto &chosen = slot == "A" ? a : b;
    if (!target.path.empty() && (target.device != chosen.device || target.bytes != chosen.bytes)) {
      target.path.clear(); target.reason = "Conflicting partition aliases"; return target;
    }
    target.path = chosen.path;
    target.bytes = chosen.bytes;
    target.device = chosen.device;
    target.raw = true;
    target.reason.clear();
  }
  return target;
}
int OpenRawTarget(const PayloadFlashTarget &target, uint64_t image_bytes, std::string &error) {
  if (!target.raw || target.path.empty() || !image_bytes || image_bytes > target.bytes) {
    error = "Image exceeds partition capacity or target is invalid"; return -1;
  }
  int fd = open(target.path.c_str(), O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) { error = "Cannot claim " + target.name + ": " + strerror(errno); return -1; }
  uint64_t device = 0, bytes = 0;
  if (!Geometry(fd, device, bytes) || device != target.device || bytes != target.bytes) {
    close(fd); error = "Partition identity or capacity changed: " + target.name; return -1;
  }
  return fd;
}
bool VerifyImageBytes(int fd, uint64_t image_bytes, const std::string &expected_sha256,
                      std::string &error,
                      const std::function<void(uint64_t, uint64_t)> &progress) {
  if (fd < 0 || !image_bytes || image_bytes > uint64_t(INT64_MAX) ||
      expected_sha256.size() != SHA256_DIGEST_LENGTH) {
    error = "Invalid image size or manifest SHA-256 for readback"; return false;
  }
  SHA256_CTX hash;
  SHA256_Init(&hash);
  // Heap buffer avoids a large stack frame on the recovery worker thread.
  std::vector<unsigned char> buffer(1024 * 1024);
  auto last_update = std::chrono::steady_clock::now();
  if (progress) progress(0, image_bytes);
  for (uint64_t offset = 0; offset < image_bytes;) {
    const size_t count = std::min<uint64_t>(buffer.size(), image_bytes - offset);
    ssize_t n = pread(fd, buffer.data(), count, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      error = "Partition readback failed at byte " + std::to_string(offset); return false;
    }
    SHA256_Update(&hash, buffer.data(), n);
    offset += n;
    const auto now = std::chrono::steady_clock::now();
    if (progress && offset < image_bytes && now - last_update >= std::chrono::milliseconds(100)) {
      progress(offset, image_bytes); last_update = now;
    }
  }
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256_Final(digest, &hash);
  if (memcmp(digest, expected_sha256.data(), sizeof(digest))) {
    error = "Written image SHA-256 does not match the OTA manifest. Do not reboot; reflash a known-good image.";
    return false;
  }
  if (progress) progress(image_bytes, image_bytes);
  return true;
}
int OpenDirectBlock(const std::string &path, uint64_t image_bytes, std::string &error) {
  char resolved[PATH_MAX];
  if (!realpath(path.c_str(), resolved) || strncmp(resolved, "/dev/block/", 11)) {
    error = "Invalid direct block target"; return -1;
  }
  const int fd = open(resolved, O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) { error = "Cannot exclusively claim " + path + ": " + strerror(errno); return -1; }
  uint64_t device = 0, bytes = 0;
  if (!image_bytes || !Geometry(fd, device, bytes) || image_bytes > bytes) {
    close(fd); error = "Direct target is read-only or too small"; return -1;
  }
  return fd;
}
bool VerifyWrittenFd(int fd, uint64_t image_bytes, const std::string &expected_sha256,
                     std::string &error,
                     const std::function<void(uint64_t, uint64_t)> &progress) {
  struct stat st{};
  uint64_t capacity = 0;
  if (fstat(fd, &st) || !S_ISBLK(st.st_mode) || ioctl(fd, BLKGETSIZE64, &capacity) ||
      !image_bytes || image_bytes > capacity || expected_sha256.size() != SHA256_DIGEST_LENGTH) {
    error = "Written partition has invalid type or capacity"; return false;
  }
  if (fsync(fd) || ioctl(fd, BLKFLSBUF)) {
    error = "Cannot flush written partition for verification: " + std::string(strerror(errno)); return false;
  }
  return VerifyImageBytes(fd, image_bytes, expected_sha256, error, progress);
}
bool VerifyWrittenBlock(const std::string &path, uint64_t image_bytes,
                        const std::string &expected_sha256, std::string &error,
                        const std::function<void(uint64_t, uint64_t)> &progress) {
  char resolved[PATH_MAX];
  if (!realpath(path.c_str(), resolved) || strncmp(resolved, "/dev/block/", 11)) {
    error = "Cannot resolve written partition for verification"; return false;
  }
  const int fd = open(resolved, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) { error = "Cannot open written partition for verification"; return false; }
  struct stat st{};
  uint64_t capacity = 0;
  bool ok = false;
  if (fstat(fd, &st) || !S_ISBLK(st.st_mode) || ioctl(fd, BLKGETSIZE64, &capacity) ||
      !image_bytes || image_bytes > capacity || expected_sha256.size() != SHA256_DIGEST_LENGTH) {
    error = "Written partition has invalid type or capacity";
  } else if (fsync(fd) || ioctl(fd, BLKFLSBUF)) {
    error = "Cannot flush written partition for verification: " + std::string(strerror(errno));
  } else {
    ok = VerifyImageBytes(fd, image_bytes, expected_sha256, error, progress);
  }
  close(fd);
  return ok;
}
bool WriteRawImage(int fd, const PayloadFlashTarget &target, const std::string &image,
                   uint64_t image_bytes, const std::string &expected_sha256, std::string &error,
                   const std::function<void(uint64_t, uint64_t)> &progress) {
  uint64_t device = 0, bytes = 0;
  if (!target.raw || !Geometry(fd, device, bytes) || device != target.device || bytes != target.bytes ||
      !image_bytes || image_bytes > bytes || image_bytes > uint64_t(INT64_MAX)/2 ||
      expected_sha256.size() != SHA256_DIGEST_LENGTH) {
    error = "Invalid claimed block target"; return false;
  }
  int source = open(image.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat st{};
  if (source < 0 || fstat(source, &st) || !S_ISREG(st.st_mode) ||
      st.st_size < 0 || uint64_t(st.st_size) != image_bytes) {
    if (source >= 0) close(source);
    error = "Extracted image changed"; return false;
  }
  auto fail = [&](const char *message) { error = message; close(source); return false; };
  std::array<unsigned char, 256 * 1024> buffer;
  auto transfer = [](int descriptor, unsigned char *data, size_t count, uint64_t offset, bool write) {
    size_t done = 0;
    while (done < count) {
      ssize_t n = write ? pwrite(descriptor, data + done, count - done, offset + done)
                        : pread(descriptor, data + done, count - done, offset + done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      done += n;
    }
    return true;
  };
  for (uint64_t offset = 0; offset < image_bytes;) {
    size_t count = std::min<uint64_t>(buffer.size(), image_bytes - offset);
    if (!transfer(source, buffer.data(), count, offset, false) ||
        !transfer(fd, buffer.data(), count, offset, true)) return fail("Firmware write failed");
    offset += count;
    if (progress) progress(offset, image_bytes * 2);
  }
  if (fsync(fd) || ioctl(fd, BLKFLSBUF)) return fail("Firmware flush failed");
  close(source);
  return VerifyImageBytes(fd, image_bytes, expected_sha256, error,
      [&](uint64_t done, uint64_t) { if (progress) progress(image_bytes + done, image_bytes * 2); });
}
}
