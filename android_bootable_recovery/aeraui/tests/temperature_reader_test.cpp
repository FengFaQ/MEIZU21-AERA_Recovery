// SPDX-License-Identifier: Apache-2.0
#include "../platform/temperature_reader.hpp"
#include <cassert>
#include <filesystem>
#include <unistd.h>

int main() {
  using namespace aeraui;
  char pattern[] = "/tmp/aera-temperature-test-XXXXXX";
  const char *directory = mkdtemp(pattern);
  assert(directory);
  const std::filesystem::path root(directory);
  auto node = [&](const char *group, const char *name, const char *type, const char *temperature) {
    const auto path = root / "class" / group / name;
    std::filesystem::create_directories(path);
    std::ofstream(path / "type") << type;
    std::ofstream(path / "temp") << temperature;
  };
  node("thermal", "thermal_zone0", "cpu-hw-trip-0", "105000");
  node("thermal", "thermal_zone1", "shell_front", "95000");
  node("thermal", "thermal_zone2", "cpu-0-0-0", "65000");
  node("thermal", "thermal_zone3", "cpu-1-0-0", "75");
  node("thermal", "thermal_zone4", "gpuss-0", "120000");
  node("thermal", "thermal_zone5", "qmx-1-1", "2147483647");
  node("power_supply", "battery", "Battery", "287");
  node("power_supply", "usb", "USB", "800");
  TemperatureReader reader(root.string());
  auto values = reader.Read();
  assert(values.cpu == 75000 && values.battery == 28700);
  std::ofstream(root / "class/power_supply/battery/temp") << "0";
  assert(reader.Read().battery == 0);
  std::ofstream(root / "class/power_supply/battery/temp") << "-50";
  assert(reader.Read().battery == -5000);
  std::ofstream(root / "class/power_supply/battery/temp") << "2147483647";
  assert(reader.Read().battery == kTemperatureUnavailable);
  std::ofstream(root / "class/thermal/thermal_zone2/temp") << "0";
  std::ofstream(root / "class/thermal/thermal_zone3/temp") << "garbage";
  assert(reader.Read().cpu == kTemperatureUnavailable);
  std::filesystem::remove_all(root);
  assert(reader.Read().cpu == kTemperatureUnavailable);
  assert(!TemperatureReader::CpuSensor("cpu-hw-trip-0"));
  assert(!TemperatureReader::CpuSensor("gpu"));
  assert(TemperatureReader::CpuSensor("cpu-thermal"));
  std::filesystem::create_directories(root / "proc");
  std::ofstream(root / "proc/stat") << "cpu 100 0 0 100 0 0 0 0 20 0\n";
  std::ofstream(root / "proc/meminfo") << "MemTotal: 4096 kB\nMemAvailable: 1024 kB\n";
  SystemMetricsReader metrics((root / "proc").string());
  auto system = metrics.Read();
  assert(system.cpu_percent == -1 && system.memory_used == 3072 * 1024);
  std::ofstream(root / "proc/stat") << "cpu 200 0 0 200 0 0 0 0 40 0\n";
  assert(metrics.Read().cpu_percent == 50);
  std::ofstream(root / "proc/meminfo") << "MemTotal: 4096 kB\nMemFree: 1024 kB\nCached: 1024 kB\n";
  assert(metrics.Read().memory_used == 2048 * 1024);
  std::filesystem::remove_all(root);
  puts("Temperature sensor selection, units, missing nodes and invalid readings passed");
}
