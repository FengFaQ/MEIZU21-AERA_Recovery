/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace aeraui::payload {
// A decoder can retain 64 MiB output plus an 80 MiB XZ dictionary.
// Leave headroom for the UI, filesystem cache and active extractor processes.
inline unsigned WorkerBudget(uint64_t ram, uint64_t available, unsigned cpus) {
  constexpr uint64_t reserve = 512ULL << 20, per_worker = 256ULL << 20;
  const uint64_t spare = available > reserve ? available - reserve : 0;
  const auto memory_limit = std::max<uint64_t>(1, std::min(ram / 2, spare) / per_worker);
  // Two software workers per online CPU, shared across the whole batch.
  // Keep the existing 16-worker ceiling and clamp before multiplying.
  const unsigned cpu_limit = cpus ? 2 * std::min(cpus, 8u) : 1;
  return std::max(1u, std::min(cpu_limit, unsigned(std::min<uint64_t>(16, memory_limit))));
}
inline unsigned AvailableWorkers() {
  const long pages = sysconf(_SC_PHYS_PAGES), page_size = sysconf(_SC_PAGESIZE);
  const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  const uint64_t ram = pages > 0 && page_size > 0 ? uint64_t(pages) * page_size : 0;
  uint64_t available = ram / 4;
  std::ifstream memory("/proc/meminfo");
  std::string line;
  while (std::getline(memory, line)) {
    std::istringstream fields(line);
    std::string key;
    uint64_t kib = 0;
    if (fields >> key >> kib && key == "MemAvailable:") { available = kib * 1024; break; }
  }
  return WorkerBudget(ram, available, cpus > 0 ? cpus : 1);
}
}  // namespace aeraui::payload
