/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <aeraui/backend.hpp>
#include <functional>

namespace aeraui::payload {
struct RawNode {
  std::string path;
  std::string name;
  uint64_t bytes = 0;
  uint64_t device = 0;
};
using BlockProbe = std::function<bool(const std::string &, RawNode &)>;
bool ProbeBlock(const std::string &path, RawNode &node);
PayloadFlashTarget DiscoverRawTarget(const std::string &name, const std::string &slot,
                                    const BlockProbe &probe = ProbeBlock);
// Exclusive block-device claim; caller owns the returned descriptor.
int OpenRawTarget(const PayloadFlashTarget &target, uint64_t image_bytes, std::string &error);
bool WriteRawImage(int fd, const PayloadFlashTarget &target, const std::string &image,
                   uint64_t image_bytes, const std::string &expected_sha256, std::string &error,
                   const std::function<void(uint64_t, uint64_t)> &progress);
// Hash exactly image_bytes, never the unused tail of a larger partition.
// Separate descriptor helper permits host tests without accessing block devices.
bool VerifyImageBytes(int fd, uint64_t image_bytes, const std::string &expected_sha256,
                      std::string &error,
                      const std::function<void(uint64_t, uint64_t)> &progress = {});
int OpenDirectBlock(const std::string &path, uint64_t image_bytes, std::string &error);
bool VerifyWrittenFd(int fd, uint64_t image_bytes, const std::string &expected_sha256,
                     std::string &error,
                     const std::function<void(uint64_t, uint64_t)> &progress = {});
// Called before remounting a freshly written partition. Flushes and invalidates
// the block cache before reading back, and requires a real block-device target.
bool VerifyWrittenBlock(const std::string &path, uint64_t image_bytes,
                        const std::string &expected_sha256, std::string &error,
                        const std::function<void(uint64_t, uint64_t)> &progress = {});
}
