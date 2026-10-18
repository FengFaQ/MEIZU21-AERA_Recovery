// SPDX-License-Identifier: Apache-2.0
// Includes the implementation to exercise its private parser without weakening
// the production requirement for a RAM-backed extraction directory.
#include "runtime.cpp"
#include "protocol.hpp"
#include <cassert>
#include <cstdio>

namespace aeraui::plugins {
bool ResolvePayload(const std::string &, Plugin &, std::string &,
                    std::string &) {
  return false;
}
}  // namespace aeraui::plugins

using namespace aeraui::web;
int main(int argc, char **argv) {
  assert(argc == 2);
  for (const char *bad : {"", "/root", "../data", "usr/../data", "usr//lib", "usr/./lib", "usr/"})
    assert(!PathSafe(bad));
  assert(PathSafe("usr/lib/libWPEWebKit-2.0.so.1"));
  assert(Address(" example.org ") == "https://example.org");
  assert(Address("https://example.org/?a=1") == "https://example.org/?a=1");
  assert(Address("aera://start") == "aera://start");
  assert(Address("aera://test") == "aera://test");
  for (const char *bad : {"", "https://", "https:///oops", "file:///data", "javascript:alert(1)",
                          "https://a@b/", "https://a\\b/", "https://a\nb/", "ftp://example.org"})
    assert(Address(bad).empty());
  Message message; message.kind = Kind::kFrame; message.x = kWidth;
  message.y = kHeight; message.value = kFrameBytes;
  assert(Valid(message, true)); ++message.value; assert(!Valid(message, true));
  message = Message{}; message.kind = Kind::kTouchDown; message.x = kViewWidth;
  assert(!Valid(message, false)); message.x = 0; assert(Valid(message, false));
  message = Message{}; message.kind = Kind::kKeyboardShow; message.value = 10;
  assert(Valid(message, true)); message.value = 11; assert(!Valid(message, true));
  message = Message{}; message.kind = Kind::kKeyboardHide; assert(Valid(message, true));
  message = Message{}; message.kind = Kind::kSetZoom; message.value = 50;
  assert(Valid(message, false)); message.value = 301; assert(!Valid(message, false));
  message = Message{}; message.kind = Kind::kSetCookiePolicy; message.value = 2;
  assert(Valid(message, false)); message.value = 3; assert(!Valid(message, false));
  message = Message{}; message.kind = Kind::kClearBrowsingData;
  assert(Valid(message, false));
  message = Message{}; message.kind = Kind::kBrowsingDataCleared;
  assert(Valid(message, true));
  message = Message{}; message.kind = Kind::kDownloadStarted;
  message.sequence = 1; message.x = 42; message.y = 1024;
  assert(Valid(message, true)); message.sequence = 0; assert(!Valid(message, true));
  memset(message.text, 'x', sizeof(message.text)); assert(!Valid(message, false));
  assert(LaunchBlockReason().find("unprivileged") != std::string::npos);

  FD file(open(argv[1], O_RDONLY | O_CLOEXEC)); struct stat info{};
  assert(file.value >= 0 && fstat(file.value, &info) == 0);
  assert(uint64_t(info.st_size) == kCompressedBytes);
  void *data = mmap(nullptr, kCompressedBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, file.value, 0);
  assert(data != MAP_FAILED);
  Hash hash; hash.Add(data, kCompressedBytes); assert(hash.Matches(kCompressedHash));
  char directory[] = "/tmp/aera-web-XXXXXX"; assert(mkdtemp(directory));
  FD root(open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  Preparation state;
  const RuntimeSpec spec{kCompressedBytes, kExpandedBytes, kMemberCount,
                         kCompressedHash, kExpandedHash};
  {
    Reader reader(static_cast<const uint8_t *>(data), kCompressedBytes, state, spec);
    assert(Extract(reader, root.value, spec)); assert(reader.total == kExpandedBytes);
    assert(state.progress.load() == 100);
  }
  struct stat loader{}, alias{};
  assert(fstatat(root.value, "lib/ld-musl-aarch64.so.1", &loader, 0) == 0);
  assert(fstatat(root.value, "usr/lib/libc.musl-aarch64.so.1", &alias, 0) == 0);
  assert(loader.st_ino == alias.st_ino); // musl must not load twice.
  assert(faccessat(root.value, "usr/bin/aera-browser-worker", X_OK, 0) == 0);
  ClearDirectory(root.value);
  // Truncation must not publish a runtime even if complete files were emitted.
  {
    Reader reader(static_cast<const uint8_t *>(data), kCompressedBytes - 20, state, spec);
    assert(!Extract(reader, root.value, spec));
  }
  ClearDirectory(root.value);
  // Bad CRC/data is rejected; recovery never executes partially extracted files.
  static_cast<uint8_t *>(data)[kCompressedBytes / 2] ^= 0x40;
  {
    Reader reader(static_cast<const uint8_t *>(data), kCompressedBytes, state, spec);
    assert(!Extract(reader, root.value, spec));
  }
  ClearDirectory(root.value);
  static_cast<uint8_t *>(data)[kCompressedBytes / 2] ^= 0x40;
  state.cancel.store(true);
  {
    Reader reader(static_cast<const uint8_t *>(data), kCompressedBytes, state, spec);
    assert(!Extract(reader, root.value, spec));
  }
  RemoveRuntime(directory); assert(access(directory, F_OK) != 0);
  munmap(data, kCompressedBytes);
  puts("PASS: payload hash, extraction, musl alias, truncation, corruption, cancellation, paths, URL policy and IPC");
}
