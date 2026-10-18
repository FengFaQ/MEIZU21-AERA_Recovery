/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/backup/app_vault_progress.hpp"
#include <cassert>
#include <iostream>

int main() {
  using namespace aeraui;
  ResticProgress result;
  Json::Value status;
  status["message_type"] = "status";
  assert(ReadResticProgress(status, true, result));
  assert(!result.bytes_total && !result.bytes_done && !result.percent);
  status["bytes_restored"] = 25;
  status["total_bytes"] = 100;
  status["files_restored"] = 1;
  status["total_files"] = 4;
  assert(ReadResticProgress(status, true, result));
  assert(result.bytes_done == 25 && result.bytes_total == 100 && result.percent == 25);
  status["bytes_skipped"] = 100;
  status["files_skipped"] = 2;
  assert(ReadResticProgress(status, true, result));
  assert(result.bytes_done == 125 && result.bytes_total == 200 && result.percent == 62);
  assert(result.files_done == 3 && result.files_total == 6);
  status.removeMember("bytes_restored"); status.removeMember("total_bytes");
  status.removeMember("files_restored"); status.removeMember("total_files");
  assert(ReadResticProgress(status, true, result));
  assert(result.bytes_done == 100 && result.bytes_total == 100 && result.percent == 99);
  status["message_type"] = "summary";
  assert(ReadResticProgress(status, true, result));
  assert(result.bytes_done == 100 && result.files_done == 2);
  status = Json::Value(); status["message_type"] = "status";
  status["total_files"] = 4; status["files_restored"] = 2;
  assert(ReadResticProgress(status, true, result));
  assert(!result.bytes_total && result.percent == 50);
  status["total_bytes"] = 100; status["bytes_done"] = 40; status["files_done"] = 1;
  assert(ReadResticProgress(status, false, result));
  assert(result.bytes_done == 40 && result.percent == 40 && result.files_done == 1);
  status["message_type"] = "summary";
  status["total_bytes_processed"] = 100; status["total_files_processed"] = 4;
  assert(ReadResticProgress(status, false, result));
  assert(result.bytes_done == 100 && result.files_done == 4 && result.percent == 99);
  status["message_type"] = "error";
  assert(!ReadResticProgress(status, true, result));
  status = Json::Value(); status["message_type"] = "status"; status["total_bytes"] = "invalid";
  assert(ReadResticProgress(status, true, result) && !result.bytes_total);
  std::cout << "App backup/restore status, skipped files and summaries passed\n";
}
