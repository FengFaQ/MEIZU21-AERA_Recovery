// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../aera_frp_wipe.hpp"
#include <cassert>
#include <vector>

int main() {
#if defined(AERA_ENABLE_FRP_WIPE) && AERA_ENABLE_FRP_WIPE == 1
  assert(aera_frp::Enabled());
#else
  assert(!aera_frp::Enabled());
#endif
  aera_frp::Target target;
  assert(!aera_frp::Discover("/tmp/not-a-partition", &target));
  assert(!aera_frp::Inspect("/dev/null", &target));
  assert(!aera_frp::Inspect("/dev/block/does-not-exist", &target));
  assert(!aera_frp::Wipe(target));
  char name[] = "/tmp/aera-frp-test-XXXXXX";
  const int fd = mkstemp(name);
  assert(fd >= 0);
  assert(!aera_frp::Inspect(name, &target));
  // Preserve bytes past the declared range and exercise the final partial chunk.
  std::vector<unsigned char> original(131089, 0xa5);
  assert(write(fd, original.data(), original.size()) == ssize_t(original.size()));
  const uint64_t range = original.size() - 7;
  assert(aera_frp::ZeroAndVerify(fd, range));
  std::vector<unsigned char> result(original.size());
  assert(pread(fd, result.data(), result.size(), 0) == ssize_t(result.size()));
  for (size_t i = 0; i < result.size(); ++i)
    assert(result[i] == (i < range ? 0 : 0xa5));
  assert(!aera_frp::ZeroAndVerify(fd, 0));
  assert(!aera_frp::ZeroAndVerify(-1, 1));
  const int read_only = open(name, O_RDONLY);
  assert(read_only >= 0);
  assert(!aera_frp::ZeroAndVerify(read_only, range));
  close(read_only);
  close(fd);
  assert(unlink(name) == 0);
}
