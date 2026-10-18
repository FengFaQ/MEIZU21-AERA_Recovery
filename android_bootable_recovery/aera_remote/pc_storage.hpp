/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <json/json.h>

namespace aera::pc::storage {
constexpr char kMount[] = "/pc";
constexpr size_t kBlock = 8 * 1024 * 1024;
constexpr size_t kReadBlock = 4 * 1024 * 1024;
struct TransferStatistics {
  uint64_t bytes = 0;
  double bytes_per_second = 0;
  bool finalizing = false;
  uint64_t waiting_seconds = 0;
};
// One bounded request at a time provides backpressure without caching backup data.
class Bridge {
 public:
  explicit Bridge(std::chrono::milliseconds operation_timeout = std::chrono::seconds(45),
                  std::chrono::milliseconds finalize_timeout = std::chrono::minutes(10),
                  std::chrono::milliseconds heartbeat_timeout = std::chrono::seconds(90))
      : operation_timeout_(operation_timeout), finalize_timeout_(finalize_timeout),
        heartbeat_timeout_(heartbeat_timeout) {}
  bool Begin(const std::string &owner, bool writable);
  bool Heartbeat(const std::string &owner);
  void Stop();
  bool Active() const;
  bool Writable() const;
  TransferStatistics Statistics();
  std::string LastError() const;
  Json::Value Poll(const std::string &owner);
  bool Data(const std::string &owner, uint64_t id, std::string &data);
  bool AppendData(const std::string &owner, uint64_t id, std::string &frame);
  bool Reply(const std::string &owner, uint64_t id, const Json::Value &reply,
             const std::string &data = {});
  int Call(Json::Value request, Json::Value &reply, std::string *data = nullptr,
           const std::string &write_data = {});
 private:
  mutable std::mutex lock_;
  std::condition_variable ready_;
  std::string owner_, data_, reply_data_;
  std::string last_error_;
  Json::Value request_, reply_;
  uint64_t sequence_ = 0;
  struct Sample { std::chrono::steady_clock::time_point time; uint64_t bytes; };
  std::deque<Sample> samples_;
  uint64_t transferred_ = 0;
  const std::chrono::milliseconds operation_timeout_, finalize_timeout_, heartbeat_timeout_;
  std::chrono::steady_clock::time_point request_started_, last_seen_;
  bool active_ = false, writable_ = false, waiting_ = false, answered_ = false;
};
Bridge &Channel();
bool Mount(std::string &error);
void Unmount();
int WriteError();
bool SafePath(const std::string &path);
}  // namespace aera::pc::storage
