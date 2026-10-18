/* SPDX-License-Identifier: Apache-2.0 */
#include "features/update/payload_inspector.hpp"
#include "features/update/payload_arb.hpp"
#include "features/update/payload_extract.hpp"
#include "features/update/payload_targets.hpp"
#include "features/update/payload_otaripper.hpp"
#include "features/update/payload_full_flash.hpp"
#include "features/update/payload_parallel.hpp"
#include "features/update/payload_otaripper_batch.hpp"
#include <atomic>
#include <thread>
#include <chrono>
#include "../../aera_image_flash_batch.hpp"
#include "../../aera_image_flash_mounts.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>
#include <bzlib.h>
#include "update_engine/common/zstd_decode.h"
#include "update_engine/update_metadata.pb.h"
#include "ota_metadata.pb.h"
#include <cstdlib>
#include <android-base/file.h>
#include <gtest/gtest.h>
#include <ziparchive/zip_writer.h>
#include <cstdio>
#include <limits>
#include <sys/stat.h>

using namespace aeraui::payload;
using chromeos_update_engine::DeltaArchiveManifest;
TEST(PayloadSharedPool, ProgressIsBoundToTargetsAndMonotonic) {
  std::vector<DirectTarget> targets = {{"system", 3, 100}, {"vendor", 4, 200}};
  std::vector<uint64_t> previous(2);
  std::vector<aeraui::PayloadPhase> phases(2, aeraui::PayloadPhase::Queued);
  unsigned calls = 0;
  DirectProgress callback = [&](size_t, aeraui::PayloadPhase, uint64_t) { ++calls; };
  EXPECT_TRUE(ParseDirectProgress("AERA_PARTITION system writing 50 100", targets, previous, phases, callback));
  for (const auto* bad : {"AERA_PARTITION other writing 50 100", "AERA_PARTITION system writing 49 100",
      "AERA_PARTITION system written 50 100", "AERA_PARTITION vendor writing 500 200",
      "AERA_PARTITION vendor writing 50 100", "AERA_PARTITION vendor writing 50 200 extra"})
    EXPECT_FALSE(ParseDirectProgress(bad, targets, previous, phases, callback));
  EXPECT_EQ(calls, 1u);
  EXPECT_TRUE(ParseDirectProgress("AERA_PARTITION system written 100 100", targets, previous, phases, callback));
  EXPECT_FALSE(ParseDirectProgress("AERA_PARTITION system writing 100 100", targets, previous, phases, callback));
  EXPECT_TRUE(ParseDirectProgress("AERA_PARTITION vendor failed 50 200", targets, previous, phases, callback));
  EXPECT_FALSE(ParseDirectProgress("AERA_PARTITION vendor writing 100 200", targets, previous, phases, callback));
}
TEST(PayloadSharedPool, InvalidMappingDoesNotLaunchWriter) {
  bool launched = true;
  std::string error;
  EXPECT_FALSE(RunOtaripperBatch(-1, {}, std::string(32, 'h'), 16, {}, error, launched));
  EXPECT_FALSE(launched);
  std::vector<DirectTarget> duplicate = {{"system", 3, 100}, {"vendor", 3, 100}};
  EXPECT_FALSE(RunOtaripperBatch(-1, duplicate, std::string(32, 'h'), 16, {}, error, launched));
  EXPECT_FALSE(launched);
}
TEST(PayloadSharedPool, OneChildReceivesAllDescriptorsAndProgress) {
  TemporaryFile executable, package, a, b;
  const std::string script = R"SH(#!/bin/sh
fds=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --threads) shift; [ "$1" = 16 ] || exit 10 ;;
    --aera-active) exit 11 ;;
    --partitions) shift; [ "$1" = system,vendor ] || exit 12 ;;
    --aera-block-fd) shift; [ -r "/proc/self/fd/$1" ] || exit 13; fds=$((fds+1)) ;;
    /proc/self/fd/*) [ -r "$1" ] || exit 14 ;;
  esac
  shift
done
[ "$fds" = 2 ] || exit 15
printf 'AERA_PARTITION system writing 50 100\n'
printf 'AERA_PARTITION vendor written 200 200\n'
printf 'AERA_PARTITION system written 100 100\n'
)SH";
  ASSERT_TRUE(android::base::WriteStringToFile(script, executable.path));
  ASSERT_EQ(chmod(executable.path, 0700), 0);
  // Close the script's writable fd before exec to avoid ETXTBSY.
  close(executable.fd); executable.fd = -1;
  const char* setting = getenv("AERA_TEST_OTARIPPER");
  const bool had_setting = setting != nullptr;
  const std::string saved = setting ? setting : "";
  setenv("AERA_TEST_OTARIPPER", executable.path, 1);
  bool launched = false;
  std::string error;
  std::vector<aeraui::PayloadPhase> states(2, aeraui::PayloadPhase::Queued);
  const bool ok = RunOtaripperBatch(package.fd, {{"system", a.fd, 100}, {"vendor", b.fd, 200}},
      std::string(32, 'h'), 16, [&](size_t i, aeraui::PayloadPhase phase, uint64_t) { states[i] = phase; }, error, launched);
  if (had_setting) setenv("AERA_TEST_OTARIPPER", saved.c_str(), 1);
  else unsetenv("AERA_TEST_OTARIPPER");
  EXPECT_TRUE(ok) << error;
  EXPECT_TRUE(launched);
  EXPECT_EQ(states[0], aeraui::PayloadPhase::Written);
  EXPECT_EQ(states[1], aeraui::PayloadPhase::Written);
  EXPECT_GE(fcntl(a.fd, F_GETFD), 0);
  EXPECT_GE(fcntl(b.fd, F_GETFD), 0);
}
TEST(PayloadParallel, ResourceLimits) {
  EXPECT_EQ(WorkerBudget(12ULL << 30, 8ULL << 30, 8), 16u);
  EXPECT_EQ(WorkerBudget(12ULL << 30, 8ULL << 30, 4), 8u);
  EXPECT_EQ(WorkerBudget(12ULL << 30, 8ULL << 30, 2), 4u);
  EXPECT_EQ(WorkerBudget(12ULL << 30, 8ULL << 30, 1), 2u);
  EXPECT_EQ(WorkerBudget(16ULL << 30, 12ULL << 30, 16), 16u);
  EXPECT_EQ(WorkerBudget(16ULL << 30, 12ULL << 30, 32), 16u);
  EXPECT_EQ(WorkerBudget(8ULL << 30, 1ULL << 30, 8), 2u);
  EXPECT_EQ(WorkerBudget(0, 0, 0), 1u);
}
TEST(PayloadParallel, BoundedWorkersJoinAndExecuteOnce) {
  std::atomic<int> active{0}, peak{0}, completed{0};
  std::atomic<int> seen[8]{};
  EXPECT_TRUE(RunParallelImages(8, 2, [&](size_t i) {
    int n = ++active;
    int old = peak.load();
    while (old < n && !peak.compare_exchange_weak(old, n)) {}
    ++seen[i];
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    --active; ++completed;
    return true;
  }));
  EXPECT_EQ(active.load(), 0);
  EXPECT_EQ(completed.load(), 8);
  EXPECT_LE(peak.load(), 2);
  for (auto& n : seen) EXPECT_EQ(n.load(), 1);
}
TEST(PayloadParallel, FailureStopsQueueAndJoins) {
  std::atomic<int> calls{0};
  EXPECT_FALSE(RunParallelImages(8, 1, [&](size_t) { ++calls; return false; }));
  EXPECT_EQ(calls.load(), 1);
  EXPECT_FALSE(RunParallelImages(0, 2, [](size_t) { return true; }));
}
TEST(PayloadParallel, EightWorkersOverlapAndFailureJoinsAll) {
  std::mutex mutex;
  std::condition_variable ready;
  unsigned entered = 0;
  std::atomic<unsigned> active{0};
  EXPECT_FALSE(RunParallelImages(12, 8, [&](size_t) {
    ++active;
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++entered;
      ready.notify_all();
      EXPECT_TRUE(ready.wait_for(lock, std::chrono::seconds(2), [&] { return entered == 8; }));
    }
    --active;
    return false;
  }));
  EXPECT_EQ(entered, 8u);
  EXPECT_EQ(active.load(), 0u);
}
TEST(PayloadParallel, IndependentWeightedProgressRequiresVerification) {
  FlashProgressStore store;
  store.Begin({"system", "vendor"}, {300, 100}, 2);
  store.Set(0, aeraui::PayloadPhase::Writing, 300);
  store.Set(1, aeraui::PayloadPhase::Verifying, 50);
  EXPECT_EQ(OverallProgress(store.Read()), 82);
  store.Set(0, aeraui::PayloadPhase::Verified, 300);
  store.Set(1, aeraui::PayloadPhase::Verifying, 100);
  EXPECT_EQ(OverallProgress(store.Read()), 99);
  store.Set(1, aeraui::PayloadPhase::Verified, 100);
  EXPECT_EQ(OverallProgress(store.Read()), 100);
  store.Reset(); EXPECT_FALSE(store.Read().available);
  store.Begin({"boot", "vendor"}, {10, 10}, 2);
  store.Set(0, aeraui::PayloadPhase::Failed);
  store.Finish();
  EXPECT_EQ(store.Read().partitions[1].phase, aeraui::PayloadPhase::NotFlashed);
}
TEST(PayloadFullFlash, RequiresEveryImageAndPreservesManifestOrder) {
  Info info;
  info.valid = info.is_payload = true;
  info.partitions = {{"system", 4096, 1, true, {}, std::string(32, 's')},
                     {"modem", 4096, 1, true, {}, std::string(32, 'm')}};
  std::vector<aeraui::PayloadFlashTarget> targets = {
      {"system", "/system", 1024, false, 0, {}},
      {"modem", "/dev/block/test", 8192, true, 7, {}}};
  std::vector<std::string> names;
  std::string error;
  ASSERT_TRUE(PlanFullFlash(info, targets, names, error)) << error;
  EXPECT_EQ(names, (std::vector<std::string>{"system", "modem"}));
  targets.pop_back();
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error));
  EXPECT_TRUE(names.empty()); // never silently fall back to a partial flash
}
TEST(PayloadFullFlash, RejectsIncrementalInvalidUnsupportedAndUnverifiedImages) {
  Info base;
  base.valid = base.is_payload = true;
  base.partitions = {{"boot", 4096, 1, true, {}, std::string(32, 'b')}};
  std::vector<aeraui::PayloadFlashTarget> targets = {{"boot", "/boot", 8192, false, 0, {}}};
  for (int failure = 0; failure < 6; ++failure) {
    auto info = base;
    if (failure == 0) info.incremental = true;
    if (failure == 1) info.valid = false;
    if (failure == 2) info.partitions[0].extractable = false;
    if (failure == 3) info.partitions[0].sha256.clear();
    if (failure == 4) info.partitions.clear();
    if (failure == 5) info.partitions.push_back(info.partitions[0]);
    std::vector<std::string> names;
    std::string error;
    EXPECT_FALSE(PlanFullFlash(info, targets, names, error));
    EXPECT_TRUE(names.empty());
  }
}
TEST(PayloadFullFlash, AcceptsPartialCoverageOnlyWhenEveryListedImageIsStandalone) {
  Info info;
  info.valid = info.is_payload = info.partial = true;
  info.partitions = {{"boot", 4096, 1, true, {}, std::string(32, 'b')}};
  std::vector<aeraui::PayloadFlashTarget> targets = {{"boot", "/boot", 8192, false, 0, {}}};
  std::vector<std::string> names;
  std::string error;
  ASSERT_TRUE(PlanFullFlash(info, targets, names, error)) << error;
  EXPECT_EQ(names, (std::vector<std::string>{"boot"}));
  info.incremental = true;
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error));
  info.incremental = false;
  info.partitions[0].extractable = false;
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error));
}
TEST(PayloadFullFlash, RejectsTooSmallPhysicalTargetAndAliasedDevices) {
  Info info;
  info.valid = info.is_payload = true;
  info.partitions = {{"dsp", 4096, 1, true, {}, std::string(32, 'd')},
                     {"modem", 4096, 1, true, {}, std::string(32, 'm')}};
  std::vector<aeraui::PayloadFlashTarget> targets = {
      {"dsp", "/dev/block/dsp", 2048, true, 7, {}},
      {"modem", "/dev/block/modem", 8192, true, 8, {}}};
  std::vector<std::string> names;
  std::string error;
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error));
  targets[0].bytes = 8192;
  targets[1].device = 7;
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error));
  targets[1].device = 8;
  targets[1].path = targets[0].path;
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error));
}
TEST(PayloadFullFlash, SkipsOnlyExplicitlyProtectedImages) {
  Info info;
  info.valid = info.is_payload = true;
  info.partitions = {{"boot", 4096, 1, true, {}, std::string(32, 'b')},
                     {"abl", 4096, 1, true, {}, std::string(32, 'a')},
                     {"recovery", 4096, 1, true, {}, std::string(32, 'r')}};
  std::vector<aeraui::PayloadFlashTarget> targets = {{"boot", "/boot", 8192, false, 0, {}}};
  std::vector<std::string> names;
  std::string error;
  ASSERT_TRUE(PlanFullFlash(info, targets, names, error, {"abl", "recovery"})) << error;
  EXPECT_EQ(names, (std::vector<std::string>{"boot"}));
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error));
  EXPECT_TRUE(names.empty());
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error, {"abl"}));
  EXPECT_TRUE(names.empty());
  info.partitions.erase(info.partitions.begin());
  EXPECT_FALSE(PlanFullFlash(info, targets, names, error, {"abl", "recovery"}));
  EXPECT_TRUE(names.empty());
}
TEST(PayloadMounts, FindsPhysicalAliasesAndChildrenWithoutTouchingOtherSlots) {
  std::vector<aera::FlashMount> all, selected;
  ASSERT_TRUE(aera::ParseFlashMounts(
      "44 1 8:70 / /firmware ro,relatime shared:25 - vfat /dev/block/by-name/modem_a ro,fmask=0000\n"
      "48 1 8:73 / /vendor/dsp ro,relatime shared:29 - ext4 /dev/block/by-name/dsp_a ro,seclabel,norecovery\n"
      "53 1 8:70 / /vendor/firmware_mnt ro,relatime shared:25 - vfat /dev/block/sde6 ro,fmask=0000\n"
      "54 1 8:74 / /other-slot ro - ext4 /dev/block/by-name/dsp_b ro\n"
      "55 48 254:15 /etc/file /vendor/dsp/child ro - erofs /dev/block/dm-15 ro\n", all));
  std::string error;
  ASSERT_TRUE(aera::PlanFlashMounts(all, {makedev(8, 70), makedev(8, 73)}, selected, error)) << error;
  ASSERT_EQ(selected.size(), 4u);
  EXPECT_EQ(selected.front().target, "/vendor/firmware_mnt");
  auto child = std::find_if(selected.begin(), selected.end(), [](const auto& m) { return m.target == "/vendor/dsp/child"; });
  auto parent = std::find_if(selected.begin(), selected.end(), [](const auto& m) { return m.target == "/vendor/dsp"; });
  EXPECT_LT(child, parent); // unmount children first; restore in reverse order
  EXPECT_EQ(child->root, "/etc/file");
  EXPECT_EQ(selected.back().target, "/firmware");
  EXPECT_FALSE(selected.front().removed);
  EXPECT_TRUE(aera::PlanFlashMounts(all, {makedev(8, 99)}, selected, error));
  EXPECT_TRUE(selected.empty());
}
TEST(PayloadMounts, DecodesPathsAndPreservesFilesystemAndReadOnlyOptions) {
  std::vector<aera::FlashMount> mounts;
  ASSERT_TRUE(aera::ParseFlashMounts("1 0 8:3 /file\\040name /firmware\\040alias ro,nosuid,nodev,noexec,relatime - ext4 /dev/block/test ro,seclabel,norecovery,errors=remount-ro\n", mounts));
  ASSERT_EQ(mounts.size(), 1u);
  EXPECT_EQ(mounts[0].root, "/file name");
  EXPECT_EQ(mounts[0].target, "/firmware alias");
  EXPECT_EQ(aera::FlashMountFlags(mounts[0].options), static_cast<unsigned long>(MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME));
  EXPECT_EQ(aera::FlashFilesystemOptions(mounts[0].super_options), "norecovery,errors=remount-ro");
}
TEST(PayloadMounts, RejectsMalformedStackedRootAndSlaveMounts) {
  std::vector<aera::FlashMount> all, selected;
  std::string error;
  EXPECT_FALSE(aera::ParseFlashMounts("1 0 invalid / /firmware ro - ext4 /dev/block/test ro\n", all));
  for (const char* rows : {
      "1 0 8:3 / / ro - ext4 /dev/block/test ro\n",
      "1 0 8:3 / /firmware ro master:5 - ext4 /dev/block/test ro\n",
      "1 0 8:3 / /firmware ro - ext4 /dev/block/test ro\n2 1 8:4 / /firmware ro - ext4 /dev/block/other ro\n"}) {
    ASSERT_TRUE(aera::ParseFlashMounts(rows, all));
    EXPECT_FALSE(aera::PlanFlashMounts(all, {makedev(8, 3)}, selected, error));
  }
}
TEST(PayloadBatch, ReleasesAllExclusiveClaimsBeforeMountRestorationOnFailure) {
  bool active = false, prepared = false;
  int claims = 0;
  struct Claim { int& count; explicit Claim(int& c) : count(c) { ++count; } ~Claim() { --count; } };
  EXPECT_FALSE(aera::RunImageFlashBatch(active, [&] { prepared = true; return true; }, [&] {
    EXPECT_TRUE(prepared);
    Claim first(claims), second(claims);
    EXPECT_EQ(claims, 2);
    return false;
  }, [&] { EXPECT_EQ(claims, 0); EXPECT_FALSE(active); }));
}
TEST(PayloadBatch, OneStopRestoreCycleAcrossSuccessOrAnyFailedImage) {
  for (int fail_at : {-1, 0, 1, 4}) {
    bool active = false;
    int stops = 0, restores = 0, images = 0;
    const bool ok = aera::RunImageFlashBatch(active, [&] {
      EXPECT_TRUE(active); ++stops; return true;
    }, [&] {
      for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(active); EXPECT_EQ(restores, 0);
        ++images;
        if (i == fail_at) return false;
      }
      return true;
    }, [&] { EXPECT_FALSE(active); ++restores; });
    EXPECT_EQ(ok, fail_at < 0);
    EXPECT_EQ(stops, 1); EXPECT_EQ(restores, 1);
    EXPECT_EQ(images, fail_at < 0 ? 5 : fail_at + 1);
    EXPECT_FALSE(active);
  }
}
TEST(PayloadBatch, PreparationFailureRestoresWithoutWritingAndNestedBatchCannotInterfere) {
  bool active = false;
  int writes = 0, restores = 0;
  EXPECT_FALSE(aera::RunImageFlashBatch(active, [] { return false; }, [&] {
    ++writes; return true;
  }, [&] { ++restores; }));
  EXPECT_EQ(writes, 0); EXPECT_EQ(restores, 1); EXPECT_FALSE(active);
  EXPECT_TRUE(aera::RunImageFlashBatch(active, [] { return true; }, [&] {
    EXPECT_FALSE(aera::RunImageFlashBatch(active, [&] { ++writes; return true; },
        [] { return true; }, [&] { ++restores; }));
    EXPECT_TRUE(active); EXPECT_EQ(restores, 1); return true;
  }, [&] { ++restores; }));
  EXPECT_EQ(writes, 0); EXPECT_EQ(restores, 2); EXPECT_FALSE(active);
}
TEST(PayloadTargets, DiscoversCurrentSlotOnly) {
  auto probe = [](const std::string &path, RawNode &node) {
    const auto name = path.substr(path.find_last_of('/') + 1);
    if (name != "modem_a" && name != "modem_b") return false;
    const bool a = name == "modem_a";
    node = {a ? "/dev/block/sde1" : "/dev/block/sde2", name, 4096, a ? 1U : 2U};
    return true;
  };
  auto a = DiscoverRawTarget("modem", "A", probe);
  auto b = DiscoverRawTarget("modem", "B", probe);
  EXPECT_EQ(a.path, "/dev/block/sde1"); EXPECT_EQ(b.path, "/dev/block/sde2");
  EXPECT_TRUE(a.raw); EXPECT_EQ(a.bytes, 4096U);
  EXPECT_TRUE(DiscoverRawTarget("modem", "unknown", probe).path.empty());
  EXPECT_TRUE(DiscoverRawTarget("modem_a", "A", probe).path.empty());
}
TEST(PayloadTargets, RejectsProtectedNamesBeforeProbing) {
  int probes = 0;
  auto probe = [&](const std::string &, RawNode &) { ++probes; return true; };
  for (const char *name : {"", "../modem", "/modem", "super", "userdata", "metadata",
                          "persist", "misc", "frp", "modemst1", "modemst2", "fsg", "fsc"})
    EXPECT_TRUE(DiscoverRawTarget(name, "A", probe).path.empty());
  EXPECT_EQ(probes, 0);
}
TEST(PayloadTargets, RequiresDistinctPairAndCorrectKernelNames) {
  for (int kind : {0, 1, 2}) {
    auto probe = [kind](const std::string &path, RawNode &node) {
      auto name = path.substr(path.find_last_of('/') + 1);
      const bool a = name == "dsp_a";
      if (kind == 0 && !a) return false;
      node = {"/dev/block/test", kind == 2 ? "userdata" : name, 4096,
              kind == 1 ? 1U : (a ? 1U : 2U)};
      return true;
    };
    EXPECT_TRUE(DiscoverRawTarget("dsp", "A", probe).path.empty());
  }
}
TEST(PayloadTargets, RejectsConflictingRoots) {
  auto probe = [](const std::string &path, RawNode &node) {
    auto name = path.substr(path.find_last_of('/') + 1);
    unsigned base = path.find("bootdevice") == std::string::npos ? 1 : 3;
    node = {"/dev/block/test", name, 4096, base + (name == "xbl_b" ? 1U : 0U)};
    return true;
  };
  EXPECT_TRUE(DiscoverRawTarget("xbl", "A", probe).path.empty());
}
TEST(PayloadTargets, NeverWritesRegularFilesAsPartitions) {
  TemporaryFile file;
  ASSERT_GE(file.fd, 0);
  ASSERT_EQ(write(file.fd, "untouched", 9), 9);
  aeraui::PayloadFlashTarget target{"modem", file.path, 4096, true, 1, {}};
  std::string error;
  EXPECT_EQ(OpenRawTarget(target, 8192, error), -1);
  EXPECT_EQ(OpenRawTarget(target, 9, error), -1);
  EXPECT_FALSE(WriteRawImage(file.fd, target, file.path, 9, std::string(32, 'x'), error, {}));
  EXPECT_EQ(OpenDirectBlock(file.path, 9, error), -1);
  EXPECT_FALSE(VerifyWrittenFd(file.fd, 9, std::string(32, 'x'), error));
  RawNode node;
  EXPECT_FALSE(ProbeBlock(file.path, node));
  char contents[9];
  ASSERT_EQ(pread(file.fd, contents, 9, 0), 9);
  EXPECT_EQ(std::string(contents, 9), "untouched");
}
namespace {
void Put(std::vector<uint8_t> &v, size_t offset, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) v.at(offset + i) = value >> (8 * i);
}
std::vector<uint8_t> Firmware(uint32_t index) {
  std::vector<uint8_t> v(4096);
  memcpy(v.data(), "\177ELF", 4); v[4] = 2; v[5] = 1;
  Put(v, 32, 64, 8); Put(v, 54, 56, 2); Put(v, 56, 1, 2);
  Put(v, 72, 256, 8); Put(v, 96, 128, 8);
  Put(v, 256, 7, 4); Put(v, 268, 12, 4); Put(v, 272, 32, 4);
  Put(v, 292, 3, 4); Put(v, 300, index, 4);
  return v;
}
std::string Digest(const std::vector<uint8_t> &v) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(v.data(), v.size(), hash);
  return std::string(reinterpret_cast<char *>(hash), sizeof(hash));
}
TEST(PayloadReadback, ExactPrefixAndCorruptionDetection) {
  TemporaryFile file;
  std::vector<uint8_t> image(3 * 1024 * 1024 + 17, 0x6b);
  ASSERT_EQ(write(file.fd, image.data(), image.size()), ssize_t(image.size()));
  ASSERT_EQ(write(file.fd, "tail", 4), 4);
  std::string error;
  uint64_t last = 0;
  ASSERT_TRUE(VerifyImageBytes(file.fd, image.size(), Digest(image), error,
      [&](uint64_t done, uint64_t total) { EXPECT_GE(done, last); EXPECT_EQ(total, image.size()); last = done; }));
  EXPECT_EQ(last, image.size());
  ASSERT_EQ(pwrite(file.fd, "X", 1, 1048577), 1);
  last = 0;
  EXPECT_FALSE(VerifyImageBytes(file.fd, image.size(), Digest(image), error,
      [&](uint64_t done, uint64_t) { last = done; }));
  EXPECT_LT(last, image.size());
  EXPECT_NE(error.find("SHA-256"), std::string::npos);
  EXPECT_FALSE(VerifyImageBytes(file.fd, image.size() + 5, Digest(image), error));
  EXPECT_FALSE(VerifyImageBytes(file.fd, image.size(), "", error));
  EXPECT_FALSE(VerifyImageBytes(file.fd, 0, Digest(image), error));
  EXPECT_FALSE(VerifyImageBytes(-1, image.size(), Digest(image), error));
}
TEST(PayloadArb, ZeroAndMalformedFirmware) {
  auto v = Firmware(0);
  ASSERT_TRUE(ReadFirmwareArb(v).available);
  EXPECT_EQ(ReadFirmwareArb(v).index, 0u);
  v = Firmware(5); EXPECT_EQ(ReadFirmwareArb(v).index, 5u);
  v[5] = 2; EXPECT_FALSE(ReadFirmwareArb(v).available);
  v = Firmware(5); Put(v, 32, UINT64_MAX, 8);
  EXPECT_FALSE(ReadFirmwareArb(v).available);
  v = Firmware(5); Put(v, 268, 8, 4);
  EXPECT_FALSE(ReadFirmwareArb(v).available);
  v = Firmware(5); v.resize(300);
  EXPECT_FALSE(ReadFirmwareArb(v).available);
}
TEST(PayloadArb, BothSlotsMustBeCheckedBeforeInstall) {
  using D = ArbDecision;
  const Arb zero{true, 0, ""}, one{true, 1, ""}, two{true, 2, ""}, unknown{};
  EXPECT_EQ(CompareArb(true, one, {zero, zero}), D::Upgrade);
  EXPECT_EQ(CompareArb(true, zero, {one, one}), D::Downgrade);
  EXPECT_EQ(CompareArb(true, one, {one, one}), D::Same);
  EXPECT_EQ(CompareArb(true, zero, {zero, zero}), D::Same);
  EXPECT_EQ(CompareArb(true, one, {zero, two}), D::Downgrade);
  EXPECT_EQ(CompareArb(true, one, {two, zero}), D::Downgrade);
  EXPECT_EQ(CompareArb(true, one, {zero, one}), D::Upgrade);
  EXPECT_EQ(CompareArb(true, one, {one, zero}), D::Upgrade);
  EXPECT_EQ(CompareArb(true, one, {unknown, two}), D::Downgrade);
  EXPECT_EQ(CompareArb(true, one, {zero, unknown}), D::Unknown);
  EXPECT_EQ(CompareArb(true, unknown, {zero, zero}), D::Unknown);
  EXPECT_EQ(CompareArb(false, unknown, {unknown, unknown}), D::NotApplicable);
  EXPECT_FALSE(ArbAllowsInstall(D::Upgrade, false));
  EXPECT_TRUE(ArbAllowsInstall(D::Upgrade, true));
  for (bool acknowledged : {false, true}) {
    EXPECT_FALSE(ArbAllowsInstall(D::Downgrade, acknowledged));
    EXPECT_TRUE(ArbAllowsInstall(D::Unknown, acknowledged));
    EXPECT_TRUE(ArbAllowsInstall(D::Same, acknowledged));
    EXPECT_TRUE(ArbAllowsInstall(D::NotApplicable, acknowledged));
  }
}
TEST(PayloadArb, RawBzipZstdAndIntegrity) {
  for (int compression : {0, 1, 14}) {
    auto image = Firmware(7), data = image;
    if (compression == 1) {
      data.resize(8192); unsigned length = data.size();
      ASSERT_EQ(BZ2_bzBuffToBuffCompress(reinterpret_cast<char *>(data.data()), &length,
          reinterpret_cast<char *>(image.data()), image.size(), 9, 0, 30), BZ_OK);
      data.resize(length);
    }
    if (compression == 14) {
      data.resize(ZSTD_compressBound(image.size()));
      auto length = ZSTD_compress(data.data(), data.size(), image.data(), image.size(), 3);
      ASSERT_FALSE(ZSTD_isError(length));
      data.resize(length);
    }
    TemporaryFile file;
    ASSERT_EQ(write(file.fd, data.data(), data.size()), static_cast<ssize_t>(data.size()));
    DeltaArchiveManifest manifest;
    manifest.set_block_size(4096);
    auto *p = manifest.add_partitions(); p->set_partition_name("xbl_config");
    p->mutable_new_partition_info()->set_size(image.size());
    p->mutable_new_partition_info()->set_hash(Digest(image));
    auto *op = p->add_operations();
    op->set_type(static_cast<chromeos_update_engine::InstallOperation::Type>(compression));
    op->set_data_length(data.size()); op->set_data_sha256_hash(Digest(data));
    op->set_data_offset(0);
    auto *extent = op->add_dst_extents();
    extent->set_start_block(0); extent->set_num_blocks(1);
    const auto result = InspectArb(file.fd, 0, data.size(), manifest);
    ASSERT_TRUE(result.available) << result.detail;
    EXPECT_EQ(result.index, 7u);
    p->mutable_new_partition_info()->set_hash(std::string(32, 'x'));
    EXPECT_FALSE(InspectArb(file.fd, 0, data.size(), manifest).available);
    op->set_type(chromeos_update_engine::InstallOperation::SOURCE_COPY);
    EXPECT_FALSE(InspectArb(file.fd, 0, data.size(), manifest).available);
  }
}
std::string Payload(const DeltaArchiveManifest &manifest) {
  const auto proto = manifest.SerializeAsString();
  std::string header = "CrAU";
  for (int shift = 56; shift >= 0; shift -= 8)
    header += static_cast<char>(uint64_t{2} >> shift);
  for (int shift = 56; shift >= 0; shift -= 8)
    header += static_cast<char>(static_cast<uint64_t>(proto.size()) >> shift);
  header.append(4, '\0');
  return header + proto;
}
DeltaArchiveManifest Manifest() {
  DeltaArchiveManifest manifest;
  auto *partition = manifest.add_partitions();
  partition->set_partition_name("system");
  partition->mutable_new_partition_info()->set_size(4096);
  partition->add_operations()->set_type(chromeos_update_engine::InstallOperation::ZERO);
  manifest.set_security_patch_level("2026-10-01");
  return manifest;
}
Info Inspect(const std::string &payload, bool compressed = false,
             const char *entry_name = "payload.bin", bool oem = false) {
  TemporaryFile file;
  FILE *stream = fdopen(dup(file.fd), "wb");
  EXPECT_NE(stream, nullptr);
  {
    ZipWriter zip(stream);
    EXPECT_EQ(zip.StartEntry(entry_name, compressed ? ZipWriter::kCompress : 0), 0);
    EXPECT_EQ(zip.WriteBytes(payload.data(), payload.size()), 0);
    EXPECT_EQ(zip.FinishEntry(), 0);
    std::string metadata = "pre-device=lighthouse\npost-build-incremental=TEST123\npost-sdk-level=36\n";
    if (oem) metadata += "product_name=CPH2653\nversion_name=CPH2653_16.0.2.402(EX01)\n";
    EXPECT_EQ(zip.StartEntry("META-INF/com/android/metadata", ZipWriter::kCompress), 0);
    EXPECT_EQ(zip.WriteBytes(metadata.data(), metadata.size()), 0);
    EXPECT_EQ(zip.FinishEntry(), 0);
    EXPECT_EQ(zip.Finish(), 0);
  }
  fclose(stream);
  return InspectZip(file.path);
}
struct ExtractFixture {
  TemporaryFile zip;
  TemporaryDir destination;
  DeltaArchiveManifest manifest;
  std::vector<uint8_t> image = std::vector<uint8_t>(4096, 0x5a);
  std::vector<uint8_t> data;
  explicit ExtractFixture(bool compressed = false) {
    manifest.set_block_size(4096);
    auto *p = manifest.add_partitions(); p->set_partition_name("boot");
    p->mutable_new_partition_info()->set_size(image.size());
    p->mutable_new_partition_info()->set_hash(Digest(image));
    data = image;
    if (compressed) {
      data.resize(8192); unsigned length = data.size();
      EXPECT_EQ(BZ2_bzBuffToBuffCompress(reinterpret_cast<char *>(data.data()), &length,
          reinterpret_cast<char *>(image.data()), image.size(), 9, 0, 30), BZ_OK);
      data.resize(length);
    }
    auto *op = p->add_operations();
    op->set_type(compressed ? chromeos_update_engine::InstallOperation::REPLACE_BZ :
                             chromeos_update_engine::InstallOperation::REPLACE);
    op->set_data_length(data.size()); op->set_data_sha256_hash(Digest(data));
    op->set_data_offset(0);
    auto *extent = op->add_dst_extents();
    extent->set_start_block(0); extent->set_num_blocks(1);
  }
  Info Pack() {
    EXPECT_EQ(ftruncate(zip.fd, 0), 0); EXPECT_EQ(lseek(zip.fd, 0, SEEK_SET), 0);
    FILE *stream = fdopen(dup(zip.fd), "wb");
    EXPECT_NE(stream, nullptr);
    ZipWriter writer(stream);
    auto payload = Payload(manifest);
    payload.append(reinterpret_cast<const char *>(data.data()), data.size());
    EXPECT_EQ(writer.StartEntry("payload.bin", 0), 0);
    EXPECT_EQ(writer.WriteBytes(payload.data(), payload.size()), 0);
    EXPECT_EQ(writer.FinishEntry(), 0); EXPECT_EQ(writer.Finish(), 0);
    fclose(stream);
    return InspectZip(zip.path);
  }
  bool Extract(const Info &info, std::string &error, const std::vector<std::string> &names = {"boot"}) {
    return ExtractImages(zip.path, info.manifest_hash, names, destination.path, error);
  }
  bool Exists() { return access((std::string(destination.path) + "/boot.img").c_str(), F_OK) == 0; }
};
TEST(PayloadDirect, PreflightHoldsPackageAndRejectsChangesBeforeCallback) {
  ExtractFixture f;
  auto info = f.Pack();
  ASSERT_TRUE(info.valid);
  ASSERT_EQ(info.partitions.at(0).sha256, Digest(f.image));
  std::string error;
  int calls = 0;
  auto callback = [&](int fd, std::string &) {
    struct stat st{}; EXPECT_EQ(fstat(fd, &st), 0); EXPECT_TRUE(S_ISREG(st.st_mode));
    ++calls; return true;
  };
  EXPECT_TRUE(WithValidatedPayload(f.zip.path, info.manifest_hash, {"boot"}, error, callback));
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(f.Exists());
  EXPECT_FALSE(WithValidatedPayload(f.zip.path, std::string(32, 'x'), {"boot"}, error, callback));
  EXPECT_FALSE(WithValidatedPayload(f.zip.path, info.manifest_hash, {"boot", "boot"}, error, callback));
  EXPECT_FALSE(WithValidatedPayload(f.zip.path, info.manifest_hash, {"missing"}, error, callback));
  f.manifest.mutable_partitions(0)->mutable_operations(0)->mutable_dst_extents(0)->set_num_blocks(2);
  info = f.Pack();
  EXPECT_FALSE(WithValidatedPayload(f.zip.path, info.manifest_hash, {"boot"}, error, callback));
  EXPECT_EQ(calls, 1);
}
TEST(PayloadDirect, SubprocessReceivesClaimedFdAndRejectsRegularTarget) {
  if (!getenv("AERA_TEST_OTARIPPER")) GTEST_SKIP() << "No host otaripper provided";
  ExtractFixture f;
  const auto info = f.Pack();
  TemporaryFile target;
  ASSERT_EQ(write(target.fd, f.image.data(), f.image.size()), ssize_t(f.image.size()));
  std::string error;
  ASSERT_FALSE(WithValidatedPayload(f.zip.path, info.manifest_hash, {"boot"}, error,
      [&](int fd, std::string &message) {
        return RunOtaripper(fd, {"boot"}, "/tmp", f.image.size(), {}, message, target.fd, info.manifest_hash);
      }));
  EXPECT_NE(error.find("not a block device"), std::string::npos) << error;
  EXPECT_TRUE(VerifyImageBytes(target.fd, f.image.size(), Digest(f.image), error));
}
TEST(PayloadExtract, RawAndBzipVerifiedImages) {
  for (bool bz : {false, true}) {
    ExtractFixture f(bz); auto info = f.Pack();
    ASSERT_TRUE(info.valid); ASSERT_TRUE(info.partitions[0].extractable);
    std::string error;
    ASSERT_TRUE(f.Extract(info, error)) << error;
    std::string bytes;
    ASSERT_TRUE(android::base::ReadFileToString(std::string(f.destination.path) + "/boot.img", &bytes));
    EXPECT_EQ(bytes, std::string(4096, 0x5a));
    EXPECT_FALSE(f.Extract(info, error)); // Never overwrite a previous extraction.
    EXPECT_TRUE(f.Exists());
  }
}
TEST(PayloadExtract, HashFailureRemovesOutput) {
  ExtractFixture f;
  f.manifest.mutable_partitions(0)->mutable_new_partition_info()->set_hash(std::string(32, 'x'));
  auto info = f.Pack(); std::string error;
  EXPECT_FALSE(f.Extract(info, error)); EXPECT_FALSE(f.Exists());
}
TEST(PayloadExtract, ZstdVerifiedImageAndMalformedStreams) {
  for (int fault = 0; fault < 5; ++fault) {
    ExtractFixture f;
    f.data.resize(ZSTD_compressBound(f.image.size()));
    const size_t length = ZSTD_compress(f.data.data(), f.data.size(), f.image.data(), f.image.size(), 3);
    ASSERT_FALSE(ZSTD_isError(length));
    f.data.resize(length);
    if (fault == 1) f.data.pop_back();
    if (fault == 2) f.data.push_back(0);
    if (fault == 3) f.data[0] ^= 0xff;
    if (fault == 4) f.manifest.mutable_partitions(0)->mutable_new_partition_info()->set_hash(std::string(32, 'x'));
    auto* op = f.manifest.mutable_partitions(0)->mutable_operations(0);
    op->set_type(chromeos_update_engine::InstallOperation::REPLACE_ZSTD);
    op->set_data_length(f.data.size());
    op->set_data_sha256_hash(Digest(f.data));
    auto info = f.Pack();
    ASSERT_TRUE(info.valid) << info.error;
    EXPECT_FALSE(info.incremental);
    ASSERT_TRUE(info.partitions[0].extractable);
    std::string error;
    EXPECT_EQ(f.Extract(info, error), fault == 0) << error;
    EXPECT_EQ(f.Exists(), fault == 0);
  }
}
TEST(PayloadZstd, ExactOutputSizeAndSinkFailure) {
  std::string plain(200000, 'z');
  std::vector<char> compressed(ZSTD_compressBound(plain.size()));
  auto size = ZSTD_compress(compressed.data(), compressed.size(), plain.data(), plain.size(), 3);
  ASSERT_FALSE(ZSTD_isError(size));
  auto accept = [](const void*, size_t) { return true; };
  EXPECT_TRUE(chromeos_update_engine::DecodeZstd(compressed.data(), size, plain.size(), accept));
  EXPECT_FALSE(chromeos_update_engine::DecodeZstd(compressed.data(), size, plain.size() - 1, accept));
  EXPECT_FALSE(chromeos_update_engine::DecodeZstd(compressed.data(), size, plain.size() + 1, accept));
  EXPECT_FALSE(chromeos_update_engine::DecodeZstd(compressed.data(), size, plain.size(),
      [](const void*, size_t) { return false; }));
}
TEST(PayloadExtract, ParallelOperationsAndMultiImageProgress) {
  ExtractFixture f(true);
  const auto compressed = f.data;
  f.image.assign(12 * 4096, 0x5a);
  auto *p = f.manifest.mutable_partitions(0);
  p->mutable_new_partition_info()->set_size(f.image.size());
  p->mutable_new_partition_info()->set_hash(Digest(f.image));
  p->clear_operations(); f.data.clear();
  for (unsigned i = 0; i < 12; ++i) {
    auto *op = p->add_operations();
    op->set_type(chromeos_update_engine::InstallOperation::REPLACE_BZ);
    op->set_data_offset(f.data.size()); op->set_data_length(compressed.size());
    op->set_data_sha256_hash(Digest(compressed));
    auto *e = op->add_dst_extents(); e->set_start_block(i); e->set_num_blocks(1);
    f.data.insert(f.data.end(), compressed.begin(), compressed.end());
  }
  auto *second = f.manifest.add_partitions(); *second = *p;
  second->set_partition_name("vendor_boot");
  const auto info = f.Pack(); std::string error;
  uint64_t previous = 0, last_total = 0;
  ASSERT_TRUE(ExtractImages(f.zip.path, info.manifest_hash, {"boot", "vendor_boot"},
      f.destination.path, error, [&](const std::string &, uint64_t done, uint64_t total) {
        EXPECT_GE(done, previous); EXPECT_LE(done, total); previous = done; last_total = total;
      })) << error;
  EXPECT_EQ(previous, last_total);
  for (const auto *name : {"boot", "vendor_boot"}) {
    std::string bytes;
    ASSERT_TRUE(android::base::ReadFileToString(std::string(f.destination.path) + "/" + name + ".img", &bytes));
    EXPECT_EQ(bytes, std::string(f.image.begin(), f.image.end()));
  }
}
TEST(PayloadExtract, OtaripperFailureNeverFallsBackOrAcceptsMissingOutput) {
  const char *setting = getenv("AERA_TEST_OTARIPPER");
  const bool had_setting = setting != nullptr;
  const std::string saved = setting ? setting : "";
  for (const auto *binary : {"/bin/false", "/bin/true"}) {
    setenv("AERA_TEST_OTARIPPER", binary, 1);
    ExtractFixture f; const auto info = f.Pack(); std::string error;
    EXPECT_FALSE(f.Extract(info, error)); EXPECT_FALSE(f.Exists());
  }
  if (had_setting) setenv("AERA_TEST_OTARIPPER", saved.c_str(), 1);
  else unsetenv("AERA_TEST_OTARIPPER");
}
TEST(PayloadExtract, XzVerifiedImage) {
  ExtractFixture f;
  f.data = {0xfd,0x37,0x7a,0x58,0x5a,0x0,0x0,0x1,0x69,0x22,0xde,0x36,0x2,0x0,0x21,0x1,0x16,0x0,0x0,0x0,0x74,0x2f,0xe5,0xa3,0xe0,0xf,0xff,0x0,0x19,0x5d,0x0,0x2d,0x6f,0xfb,0xbf,0xfe,0xa3,0xb1,0x5e,0xe5,0xf8,0x3f,0xb2,0xaa,0x26,0x55,0xf8,0x68,0x70,0x41,0x70,0x15,0xe,0x24,0x18,0xcf,0x0,0x0,0x0,0x0,0xdd,0x51,0xd5,0x7c,0x0,0x1,0x31,0x80,0x20,0x0,0x0,0x0,0x79,0x1f,0xa7,0x1d,0x3e,0x30,0xd,0x8b,0x2,0x0,0x0,0x0,0x0,0x1,0x59,0x5a};
  auto *op = f.manifest.mutable_partitions(0)->mutable_operations(0);
  op->set_type(chromeos_update_engine::InstallOperation::REPLACE_XZ);
  op->set_data_length(f.data.size()); op->set_data_sha256_hash(Digest(f.data));
  auto info = f.Pack(); std::string error;
  ASSERT_TRUE(f.Extract(info, error)) << error;
}
TEST(PayloadExtract, CorruptOperationAndChangedManifest) {
  ExtractFixture f; f.data[5] ^= 1;
  auto info = f.Pack(); std::string error;
  EXPECT_FALSE(f.Extract(info, error)); EXPECT_FALSE(f.Exists());
  info.manifest_hash.assign(32, 'x');
  EXPECT_FALSE(f.Extract(info, error)); EXPECT_FALSE(f.Exists());
}
TEST(PayloadExtract, SelectionAndSourceRequirements) {
  ExtractFixture f; auto info = f.Pack(); std::string error;
  EXPECT_FALSE(f.Extract(info, error, {}));
  EXPECT_FALSE(f.Extract(info, error, {"boot", "boot"}));
  EXPECT_FALSE(f.Extract(info, error, {"missing"}));
  EXPECT_FALSE(f.Extract(info, error, {"../boot"}));
  f.manifest.mutable_partitions(0)->mutable_old_partition_info()->set_size(4096);
  info = f.Pack(); EXPECT_TRUE(info.incremental);
  EXPECT_FALSE(info.partitions[0].extractable);
  EXPECT_FALSE(f.Extract(info, error)); EXPECT_FALSE(f.Exists());
}
TEST(PayloadExtract, RejectInvalidExtentsAndUnboundedBuffers) {
  ExtractFixture f;
  auto *p = f.manifest.mutable_partitions(0);
  auto *op = p->mutable_operations(0);
  op->mutable_dst_extents(0)->set_start_block(UINT64_MAX);
  EXPECT_FALSE(f.Pack().partitions[0].extractable);
  op->mutable_dst_extents(0)->set_start_block(0);
  *p->add_operations() = *op;
  EXPECT_FALSE(f.Pack().partitions[0].extractable);
  p->mutable_operations()->RemoveLast();
  p->mutable_operations(0)->set_data_length(UINT64_MAX);
  EXPECT_FALSE(f.Pack().partitions[0].extractable);
}
TEST(PayloadExtract, ZeroImageAndProgress) {
  ExtractFixture f;
  auto *p = f.manifest.mutable_partitions(0);
  p->mutable_new_partition_info()->set_hash(Digest(std::vector<uint8_t>(4096, 0)));
  auto *op = p->mutable_operations(0); op->set_type(chromeos_update_engine::InstallOperation::ZERO);
  op->clear_data_length(); op->clear_data_sha256_hash(); f.data.clear();
  auto info = f.Pack(); std::string error;
  uint64_t previous = 0, last_total = 0;
  ASSERT_TRUE(ExtractImages(f.zip.path, info.manifest_hash, {"boot"}, f.destination.path, error,
      [&](const std::string &, uint64_t done, uint64_t total) {
        EXPECT_GE(done, previous); EXPECT_LE(done, total); previous = done; last_total = total;
      })) << error;
  EXPECT_EQ(previous, last_total);
}
TEST(PayloadExtract, FailureCleansEarlierSelectedImages) {
  ExtractFixture f;
  auto *p = f.manifest.add_partitions(); *p = f.manifest.partitions(0);
  p->set_partition_name("init_boot");
  p->mutable_new_partition_info()->set_hash(std::string(32, 'x'));
  auto info = f.Pack(); std::string error;
  EXPECT_FALSE(f.Extract(info, error, {"boot", "init_boot"}));
  EXPECT_FALSE(f.Exists());
  EXPECT_NE(access((std::string(f.destination.path) + "/init_boot.img").c_str(), F_OK), 0);
}
TEST(PayloadExtract, LocalFullOtaWhenProvided) {
  const char *path = std::getenv("AERA_TEST_FULL_OTA");
  if (!path) GTEST_SKIP() << "No local full OTA fixture provided";
  const auto info = InspectZip(path);
  ASSERT_TRUE(info.valid) << info.error;
  ASSERT_FALSE(info.incremental);
  std::vector<std::string> names;
  const char *selected_partition = std::getenv("AERA_TEST_PARTITION");
  for (const auto &p : info.partitions) {
    printf("%s: %s\n", p.name.c_str(), p.extractable ? "extractable" : p.extraction_error.c_str());
    if (selected_partition ? p.name == selected_partition : (p.name == "boot" || p.name == "vendor")) {
      ASSERT_TRUE(p.extractable) << p.extraction_error;
      names.push_back(p.name);
    }
  }
  ASSERT_FALSE(names.empty());
  TemporaryDir directory;
  std::string error;
  ASSERT_TRUE(ExtractImages(path, info.manifest_hash, names, directory.path, error)) << error;
}
TEST(PayloadReview, FullAndIncremental) {
  auto manifest = Manifest();
  auto info = Inspect(Payload(manifest));
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_FALSE(info.incremental);
  EXPECT_EQ(info.expanded_bytes, 4096u);
  EXPECT_EQ(info.target_device, "lighthouse");
  EXPECT_EQ(info.build_id, "TEST123");
  EXPECT_TRUE(info.name_from_filename);
  EXPECT_EQ(info.security_patch, "2026-10-01");
  EXPECT_EQ(info.target_sdk, "Android 16");
  manifest.mutable_partitions(0)->mutable_old_partition_info()->set_size(4096);
  EXPECT_TRUE(Inspect(Payload(manifest)).incremental);
}
TEST(PayloadReview, NormalInstaller) {
  auto info = Inspect("#!/sbin/sh\n", false, "META-INF/com/google/android/update-binary");
  EXPECT_FALSE(info.is_payload);
  EXPECT_TRUE(info.error.empty());
}
TEST(PayloadReview, OemDisplayFieldsAndFullRevision) {
  auto manifest = Manifest();
  manifest.set_minor_version(6);
  auto info = Inspect(Payload(manifest), false, "payload.bin", true);
  ASSERT_TRUE(info.valid);
  EXPECT_EQ(info.target_device, "CPH2653");
  EXPECT_EQ(info.target_build, "CPH2653_16.0.2.402(EX01)");
  EXPECT_EQ(info.build_id, "TEST123");
  EXPECT_FALSE(info.incremental);
  manifest.mutable_partitions(0)->mutable_operations(0)->set_type(chromeos_update_engine::InstallOperation::SOURCE_COPY);
  EXPECT_TRUE(Inspect(Payload(manifest)).incremental);
}
TEST(PayloadReview, CustomRomFileWhenProvided) {
  const char *path = std::getenv("AERA_TEST_CUSTOM_OTA");
  if (!path) GTEST_SKIP() << "No local custom OTA fixture provided";
  const auto info = InspectZip(path);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_TRUE(info.name_from_filename);
  EXPECT_EQ(info.target_device, "lighthouse");
  EXPECT_EQ(info.target_sdk, "Android 17");
  EXPECT_EQ(info.target_build, "Project_Infinity-X-4.0-BETA-lighthouse-08.10.2026-GAPPS-UNOFFICIAL");
  EXPECT_EQ(info.build_id, "1791456637");
  const auto arb = InspectZip(path, true);
  ASSERT_TRUE(arb.arb_available) << arb.arb_detail;
  EXPECT_EQ(arb.arb_index, 0u);
}
TEST(PayloadReview, RejectBrokenHeaderAndCompression) {
  EXPECT_FALSE(Inspect("CrAU").valid);
  auto data = Payload(Manifest());
  data[12] = 127;
  EXPECT_FALSE(Inspect(data).valid);
  EXPECT_FALSE(Inspect(Payload(Manifest()), true).valid);
}
TEST(PayloadReview, RejectInvalidNamesAndSizeOverflow) {
  auto manifest = Manifest();
  manifest.mutable_partitions(0)->set_partition_name("../system");
  EXPECT_FALSE(Inspect(Payload(manifest)).valid);
  manifest = Manifest();
  auto *other = manifest.add_partitions();
  other->set_partition_name("vendor");
  other->mutable_new_partition_info()->set_size(std::numeric_limits<uint64_t>::max());
  EXPECT_FALSE(Inspect(Payload(manifest)).valid);
}
TEST(PayloadReview, SnapshotAndPartialMetadata) {
  auto manifest = Manifest();
  manifest.set_partial_update(true);
  auto *dynamic = manifest.mutable_dynamic_partition_metadata();
  dynamic->add_groups()->set_name("main");
  dynamic->set_snapshot_enabled(true);
  dynamic->set_vabc_enabled(true);
  const auto info = Inspect(Payload(manifest));
  ASSERT_TRUE(info.valid);
  EXPECT_TRUE(info.partial);
  EXPECT_TRUE(info.dynamic_partitions);
  EXPECT_TRUE(info.snapshots);
  EXPECT_TRUE(info.virtual_ab_compression);
}
}
