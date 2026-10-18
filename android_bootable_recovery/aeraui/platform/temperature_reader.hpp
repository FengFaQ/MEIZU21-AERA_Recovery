// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "aeraui/backend.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aeraui {
class TemperatureReader {
 public:
  explicit TemperatureReader(std::string root = "/sys") : root_(std::move(root)) {}

  static bool CpuSensor(const std::string &name) {
    if (name.compare(0, 12, "cpu-hw-trip-") == 0) return false;
    return name.compare(0, 4, "cpu-") == 0 || name.compare(0, 7, "cpullc-") == 0 ||
        name.compare(0, 4, "qmx-") == 0 || name == "cpu" || name == "cpu_thermal" ||
        name == "soc_thermal" || name == "soc-thermal" || name == "soc-therm";
  }

  Temperatures Read() {
    const auto now = std::chrono::steady_clock::now();
    if (now >= discover_after_) {
      cpu_.clear(); battery_.clear();
      Discover(root_ + "/class/thermal", "thermal_zone", [this](const std::string &path) {
        std::ifstream type(path + "/type"); std::string name; type >> name;
        if (CpuSensor(name)) cpu_.push_back(path + "/temp");
      });
      Discover(root_ + "/class/power_supply", "", [this](const std::string &path) {
        std::ifstream type(path + "/type"); std::string name; type >> name;
        if (name == "Battery") battery_.push_back(path + "/temp");
      });
      discover_after_ = now + std::chrono::seconds(30);
    }
    Temperatures result;
    for (const auto &path : cpu_) {
      long value = 0; std::ifstream input(path);
      if (!(input >> value)) continue;
      if (value > 0 && value <= 150) value *= 1000;
      if (value > 0 && value <= 150000)
        result.cpu = std::max(result.cpu, static_cast<int>(value));
    }
    for (const auto &path : battery_) {
      long value = 0; std::ifstream input(path);
      // Linux power_supply temperatures are in tenths of a degree Celsius.
      if ((input >> value) && value >= -200 && value <= 900)
        result.battery = std::max(result.battery, static_cast<int>(value * 100));
    }
    return result;
  }

 private:
  template <typename Callback>
  static void Discover(const std::string &directory, const std::string &prefix, Callback callback) {
    DIR *dir = opendir(directory.c_str());
    if (!dir) return;
    while (auto *entry = readdir(dir)) {
      const std::string name = entry->d_name;
      if (name == "." || name == ".." || name.compare(0, prefix.size(), prefix) != 0) continue;
      callback(directory + "/" + name);
    }
    closedir(dir);
  }
  std::string root_;
  std::vector<std::string> cpu_, battery_;
  std::chrono::steady_clock::time_point discover_after_{};
};

class SystemMetricsReader {
 public:
  explicit SystemMetricsReader(std::string root = "/proc") : root_(std::move(root)) {}
  SystemMetrics Read() {
    SystemMetrics result;
    std::ifstream stat(root_ + "/stat");
    std::string name; uint64_t ticks[8]{};
    if (stat >> name && name == "cpu" &&
        stat >> ticks[0] >> ticks[1] >> ticks[2] >> ticks[3] >> ticks[4] >> ticks[5] >> ticks[6] >> ticks[7]) {
      uint64_t total = 0;
      for (auto tick : ticks) total += tick;
      const uint64_t idle = ticks[3] + ticks[4];
      if (previous_total_ && total > previous_total_ && idle >= previous_idle_) {
        const uint64_t elapsed = total - previous_total_;
        const uint64_t waiting = std::min(elapsed, idle - previous_idle_);
        result.cpu_percent = static_cast<int>((elapsed - waiting) * 100 / elapsed);
      }
      previous_total_ = total; previous_idle_ = idle;
    }
    std::ifstream memory(root_ + "/meminfo");
    uint64_t available = 0, free = 0, buffers = 0, cached = 0, reclaimable = 0, shared = 0;
    bool have_available = false;
    std::string line;
    while (std::getline(memory, line)) {
      std::istringstream fields(line);
      std::string key, unit; uint64_t value = 0;
      if (!(fields >> key >> value >> unit) || unit != "kB" || value > UINT64_MAX / 1024) continue;
      value *= 1024;
      if (key == "MemTotal:") result.memory_total = value;
      else if (key == "MemAvailable:") { available = value; have_available = true; }
      else if (key == "MemFree:") free = value;
      else if (key == "Buffers:") buffers = value;
      else if (key == "Cached:") cached = value;
      else if (key == "SReclaimable:") reclaimable = value;
      else if (key == "Shmem:") shared = value;
    }
    if (!have_available) {
      available = free + buffers + cached + reclaimable;
      available -= std::min(available, shared);
    }
    result.memory_used = result.memory_total - std::min(result.memory_total, available);
    return result;
  }
 private:
  std::string root_;
  uint64_t previous_total_ = 0, previous_idle_ = 0;
};
struct StatusReadings {
  Temperatures temperature;
  SystemMetrics system;
};

class TemperatureSampler {
 public:
  ~TemperatureSampler() {
    { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
  StatusReadings Snapshot(bool enabled, bool temperatures) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled && !worker_.joinable()) worker_ = std::thread([this] { Run(); });
    temperatures_ = temperatures;
    if (enabled_ != enabled) {
      enabled_ = enabled;
      if (!enabled) cached_ = {};
      changed_.notify_all();
    }
    return cached_;
  }
 private:
  void Run() {
    TemperatureReader reader;
    SystemMetricsReader system;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
      changed_.wait(lock, [this] { return stop_ || enabled_; });
      if (stop_) break;
      const bool temperatures = temperatures_;
      lock.unlock();
      const StatusReadings values{temperatures ? reader.Read() : Temperatures{}, system.Read()};
      lock.lock();
      if (enabled_) cached_ = values;
      changed_.wait_for(lock, std::chrono::seconds(2), [this] { return stop_ || !enabled_; });
    }
  }
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread worker_;
  StatusReadings cached_;
  bool temperatures_ = false;
  bool enabled_ = false, stop_ = false;
};
}  // namespace aeraui
