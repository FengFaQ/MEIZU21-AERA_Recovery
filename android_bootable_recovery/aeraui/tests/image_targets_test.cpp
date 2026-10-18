/* SPDX-License-Identifier: Apache-2.0 */
#include "../platform/image_targets.hpp"
#include <cassert>
#include <algorithm>
using namespace aeraui::images;
int main() {
  std::map<std::string, Block> blocks;
  for (const char *name : {"abl", "aop", "aop_config", "bluetooth", "boot", "cpucp", "cpucp_dtb",
      "devcfg", "dsp", "dtbo", "engineering_cdt", "featenabler", "hyp", "hyp_ac_config", "imagefv",
      "init_boot", "keymaster", "multiimgoem", "multiimgqti", "modem", "oplus_sec", "pdp", "pdp_cdb",
      "pvmfw", "qupfw", "recovery", "secretkeeper", "shrm", "soccp", "soccp_dcd", "soccp_debug",
      "splash", "spuservice", "tme_config", "tme_fw", "tme_seq_patch", "tz", "tz_ac_config", "tz_qti_config",
      "uefi", "uefisecapp", "vbmeta", "vbmeta_system", "vbmeta_vendor", "vendor_boot", "xbl",
      "xbl_ac_config", "xbl_config", "xbl_ramdump"}) {
    blocks[std::string(name)+"_a"] = {"/dev/block/"+std::string(name)+"_a", 4096};
    blocks[std::string(name)+"_b"] = {"/dev/block/"+std::string(name)+"_b", 8192};
  }
  const auto firmware = PhysicalTargets(blocks, {});
  assert(firmware.size() == 49);
  for (const auto &volume : firmware) { assert(volume.slot_select); assert(volume.bytes == 4096); }
  assert(PhysicalTargets(blocks, {"boot", "init_boot", "vendor_boot", "recovery", "dtbo", "abl"}).size() == 43);
  blocks["splash"] = {"/dev/block/splash_a", 4096};
  assert(PhysicalTargets(blocks, {}).size() == firmware.size());
  for (const char *name : {"super", "userdata", "metadata", "persist", "modemst1", "misc", "oplusreserve1"})
    blocks[name] = {"/dev/block/"+std::string(name), 4096};
  blocks["system_a"] = {"/dev/block/dm-1", 4096};
  blocks["bad/name"] = {"/dev/block/bad", 4096};
  assert(PhysicalTargets(blocks, {}).size() == firmware.size());
  std::map<std::string, Block> older{{"modem", {"/dev/block/modem", 8192}}};
  const auto single = PhysicalTargets(older, {});
  assert(single.size() == 1 && !single[0].slot_select && single[0].path == "/block/modem");
  assert(!ValidName("../modem") && !ValidName("modem;reboot"));
}
