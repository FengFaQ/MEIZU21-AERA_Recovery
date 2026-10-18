/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace chromeos_update_engine { class DeltaArchiveManifest; }
namespace aeraui::payload {
struct Arb {
  bool available = false;
  uint32_t index = 0;
  std::string detail;
};
struct DeviceArb { Arb slot_a; Arb slot_b; };
enum class ArbDecision { NotApplicable, Unknown, Same, Upgrade, Downgrade };
DeviceArb ReadDeviceArb();
ArbDecision CompareArb(bool includes_firmware, const Arb &package, const DeviceArb &device);
bool ArbAllowsInstall(ArbDecision decision, bool upgrade_acknowledged);
Arb ReadFirmwareArb(const std::vector<uint8_t> &image);
Arb InspectArb(int fd, uint64_t data_offset, uint64_t data_size,
               const chromeos_update_engine::DeltaArchiveManifest &manifest);
}
