/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_flash_log.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
std::string root;
aera::flashlog::Options options;
std::vector<std::map<std::string, std::string>> captured;
bool fail_archive = false;
std::string Read(const fs::path &file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
bool Archive(const std::string &folder, const std::string &destination) {
  if (fail_archive) return false;
  std::map<std::string, std::string> files;
  for (const auto &entry : fs::directory_iterator(folder))
    if (entry.is_regular_file()) files[entry.path().filename()] = Read(entry.path());
  captured.push_back(std::move(files));
  std::ofstream(destination) << "archive fixture\n";
  return true;
}
void Append(const std::string &text) { std::ofstream(options.recovery_log, std::ios::app) << text; }
void Pause() { std::this_thread::sleep_for(std::chrono::milliseconds(120)); }
}
namespace aera::flashlog {
Options PlatformOptions(const std::string &, const std::string &, const std::string &) { return options; }
std::vector<std::string> PlatformDirectories() { return {root + "/saved", root + "/work"}; }
}
int main() {
  using namespace aera::flashlog;
  char path[] = "/tmp/aera-flash-log-test-XXXXXX";
  root = mkdtemp(path); assert(!root.empty());
  fs::create_directory(root + "/work"); fs::create_directory(root + "/saved");
  options.directory = root + "/work"; options.archive_directory = root + "/saved";
  options.recovery_log = root + "/recovery.log";
  options.summary = "Keep AERA installed: 1\nKeep current ABL: 1\n";
  options.final_summary = [] { return "Slot after: b\n"; };
  options.archive = Archive;
  options.logcat_limit = options.recovery_limit = 2048;
  options.logcat = {"/usr/bin/printf", "logcat fixture\n"};
  options.kernel = {"/usr/bin/printf", "kernel fixture\n"};
  Append("before flash\n");
  {
    Recording recording("fixture.zip", "ZIP");
    { Recording nested("nested.zip", "ZIP"); assert(nested.Finish(9) == 9); }
    Append("installer output\n"); Pause();
    assert(recording.Finish(0) == 0);
  }
  assert(captured.size() == 1);
  assert(captured.back()["summary.txt"].find("Result: SUCCESS") != std::string::npos);
  assert(captured.back()["summary.txt"].find("unfinished") == std::string::npos);
  assert(captured.back()["summary.txt"].find("Keep current ABL: 1") != std::string::npos);
  assert(captured.back()["summary.txt"].find("Slot after: b") != std::string::npos);
  assert(captured.back()["recovery.log"].find("installer output") != std::string::npos);
  assert(captured.back()["logcat.txt"].find("logcat fixture") != std::string::npos);
  assert(captured.back()["kernel.log"].find("kernel fixture") != std::string::npos);
  auto reports = Reports(); assert(reports.size() == 1);
  assert(reports[0].path.find("/saved/") != std::string::npos);
  uint64_t size = 0; int fd = OpenReport(reports[0].name, &size);
  assert(fd >= 0 && size > 0); close(fd);
  assert(OpenReport("../recovery.log", &size) < 0);
  fs::create_symlink(root + "/recovery.log", root + "/saved/flash_20261015_100000_ABCDEF_FAILED.zip");
  assert(OpenReport("flash_20261015_100000_ABCDEF_FAILED.zip", &size) < 0);
  std::ofstream(root + "/saved/unrelated.zip") << "keep";
  options.logcat = {"/usr/bin/python3", "-c", "import sys;sys.stdout.write('x'*20000+'TAIL\\n')"};
  {
    Recording recording("failed.zip", "ZIP");
    Append(std::string(20000, 'y') + "RECOVERY TAIL\n"); Pause();
    assert(recording.Finish(7) == 7);
  }
  assert(captured.size() == 2);
  const auto &bounded = captured.back();
  assert(bounded.at("logcat.txt").size() + bounded.at("logcat.previous.txt").size() <= 2048);
  assert(bounded.at("logcat.txt").find("TAIL") != std::string::npos);
  assert(bounded.at("summary.txt").find("Result: FAILED\nReturn code: 7") != std::string::npos);
  assert(bounded.at("summary.txt").find("unfinished") == std::string::npos);
  assert(fs::exists(root + "/saved/unrelated.zip"));
  {
    Recording abandoned("interrupted.zip", "ZIP"); Pause();
  }
  for (const auto &entry : fs::directory_iterator(root + "/work"))
    if (entry.is_directory()) std::ofstream(root + "/saved/" + entry.path().filename().string() + "_SUCCESS.zip.part") << "partial";
  reports = Reports(); assert(reports.size() == 2);
  for (const auto &entry : fs::directory_iterator(root + "/saved")) assert(entry.path().extension() != ".part");
  assert(reports[0].name.find("_INCOMPLETE.zip") != std::string::npos);
  assert(captured.back()["summary.txt"].find("Recording recovered") != std::string::npos);
  assert(captured.back()["summary.txt"].find("unfinished") != std::string::npos);
  options.logcat = {"/does/not/exist"};
  for (int i = 0; i < 3; ++i) {
    Recording recording("next.zip", "ZIP"); assert(recording.Finish(0) == 0);
    assert(Reports().size() == 2);
  }
  fail_archive = true;
  { Recording recording("archive-failed.zip", "ZIP"); assert(recording.Finish(0) == 0); }
  fail_archive = false;
  reports = Reports(); assert(reports.size() == 2);
  assert(captured.back()["summary.txt"].find("Result: SUCCESS") != std::string::npos);
  assert(SensitiveSetting("tw_crypto_password"));
  assert(SensitiveSetting("aera_pc_token"));
  assert(SensitiveSetting("nas_passphrase"));
  assert(!SensitiveSetting("aera_preserve_abl"));
  assert(!SensitiveSetting("aera_browser_cookies"));
  assert(!SensitiveSetting("aera_installer_author"));
  assert(SensitiveSetting("aera_browser_cookie_data"));
  assert(SensitiveSetting("aera_pc_auth_token"));
  assert(SensitiveSetting("aera_installer_author_password"));
  assert(SingleLine("one\ntwo\r\t") == "one\\ntwo\\r\\t");
  options.directory = root + "/missing/path";
  { Recording disabled("no-storage.zip", "ZIP"); assert(disabled.Finish(42) == 42); }
  fs::remove_all(root);
}
