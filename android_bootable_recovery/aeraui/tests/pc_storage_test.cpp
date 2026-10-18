/* SPDX-License-Identifier: Apache-2.0 */
#include "../../aera_remote/pc_storage.hpp"
#include <cassert>
#include <cerrno>
#include <future>
#include <thread>

using namespace aera::pc::storage;
int main() {
  for (const auto *path : {"/", "/AERA/BACKUPS/test/data.ext4.win000", "/data"}) assert(SafePath(path));
  for (const auto *path : {"", "relative", "/../data", "/a/./b", "/a//b", "/a\\b"}) assert(!SafePath(path));
  assert(!SafePath(std::string("/a\0b", 4)));
  Bridge bridge;
  assert(!bridge.Begin("", true));
  assert(bridge.Begin("browser", true));
  assert(!bridge.Begin("other", true));
  assert(bridge.Poll("other").isNull());
  Json::Value read; read["op"] = "read"; read["path"] = "/data.win"; read["size"] = 8;
  std::string received;
  auto reader = std::async(std::launch::async, [&] { Json::Value reply; return bridge.Call(read, reply, &received); });
  auto request = bridge.Poll("browser");
  assert(request["op"] == "read");
  const auto id = request["id"].asUInt64();
  Json::Value okay(Json::objectValue);
  assert(!bridge.Reply("other", id, okay, "data"));
  assert(!bridge.Reply("browser", id + 1, okay, "data"));
  assert(!bridge.Reply("browser", id, okay, "too large"));
  assert(bridge.Reply("browser", id, okay, "data"));
  assert(reader.get() == 0 && received == "data");
  assert(bridge.Statistics().bytes == 4);
  Json::Value write; write["op"] = "write"; write["path"] = "/data.win";
  const std::string block(kBlock, 'x');
  auto writer = std::async(std::launch::async, [&] { Json::Value reply; return bridge.Call(write, reply, nullptr, block); });
  request = bridge.Poll("browser"); std::string outgoing;
  assert(bridge.Data("browser", request["id"].asUInt64(), outgoing) && outgoing == block);
  assert(!bridge.Data("other", request["id"].asUInt64(), outgoing));
  std::string frame = "metadata";
  assert(!bridge.AppendData("other", request["id"].asUInt64(), frame));
  assert(frame == "metadata");
  assert(bridge.AppendData("browser", request["id"].asUInt64(), frame));
  assert(frame == "metadata" + block);
  Json::Value invalid; invalid["errno"] = "invalid";
  assert(!bridge.Reply("browser", request["id"].asUInt64(), invalid));
  assert(bridge.Reply("browser", request["id"].asUInt64(), okay));
  assert(writer.get() == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(260));
  const auto transferred = bridge.Statistics();
  assert(transferred.bytes == block.size() + 4 && transferred.bytes_per_second > 0);
  auto interrupted = std::async(std::launch::async, [&] { Json::Value reply; return bridge.Call(read, reply); });
  request = bridge.Poll("browser"); assert(request.isMember("id"));
  bridge.Stop();
  assert(interrupted.get() == -EIO);
  assert(bridge.Statistics().bytes == block.size() + 4 && bridge.Statistics().bytes_per_second == 0);
  assert(bridge.Begin("next", false));
  assert(bridge.Statistics().bytes == 0 && bridge.Statistics().bytes_per_second == 0);
  assert(!bridge.Writable());
  Json::Value stat; stat["op"] = "stat"; stat["path"] = "/data.win";
  auto attributes = std::async(std::launch::async, [&] { Json::Value reply; return bridge.Call(stat, reply); });
  request = bridge.Poll("next");
  assert(!bridge.Reply("next", request["id"].asUInt64(), okay));
  Json::Value missing; missing["errno"] = ENOENT;
  assert(bridge.Reply("next", request["id"].asUInt64(), missing));
  assert(attributes.get() == -ENOENT);
  bridge.Stop();
  using namespace std::chrono_literals;
  Json::Value close; close["op"] = "close"; close["path"] = "/large.win";
  Bridge slow(100ms, 1800ms, 300ms);
  assert(slow.Begin("browser", true));
  auto closing = std::async(std::launch::async, [&] { Json::Value reply; return slow.Call(close, reply); });
  const auto closing_id = slow.Poll("browser")["id"].asUInt64();
  assert(!slow.Heartbeat("other"));
  for (int i = 0; i < 11; ++i) {
    std::this_thread::sleep_for(100ms);
    assert(slow.Heartbeat("browser"));
  }
  assert(closing.wait_for(0ms) == std::future_status::timeout);
  assert(slow.Statistics().finalizing && slow.Statistics().waiting_seconds >= 1);
  assert(slow.Reply("browser", closing_id, okay));
  assert(closing.get() == 0 && !slow.Statistics().finalizing);
  slow.Stop();

  Bridge disconnected(100ms, 2s, 150ms);
  assert(disconnected.Begin("browser", true));
  auto lost = std::async(std::launch::async, [&] { Json::Value reply; return disconnected.Call(close, reply); });
  assert(disconnected.Poll("browser").isMember("id"));
  assert(lost.wait_for(500ms) == std::future_status::ready && lost.get() == -EIO);

  Bridge bounded(100ms, 350ms, 150ms);
  assert(bounded.Begin("browser", true));
  auto stuck = std::async(std::launch::async, [&] { Json::Value reply; return bounded.Call(close, reply); });
  assert(bounded.Poll("browser").isMember("id"));
  while (stuck.wait_for(50ms) != std::future_status::ready) bounded.Heartbeat("browser");
  assert(stuck.get() == -EIO);

  Bridge ordinary(100ms, 2s, 300ms);
  assert(ordinary.Begin("browser", true));
  auto timed_read = std::async(std::launch::async, [&] { Json::Value reply; return ordinary.Call(read, reply); });
  assert(ordinary.Poll("browser").isMember("id"));
  while (timed_read.wait_for(25ms) != std::future_status::ready) ordinary.Heartbeat("browser");
  assert(timed_read.get() == -EIO);

  assert(slow.Begin("browser", true));
  auto cancelled_close = std::async(std::launch::async, [&] { Json::Value reply; return slow.Call(close, reply); });
  assert(slow.Poll("browser").isMember("id"));
  slow.Stop();
  assert(cancelled_close.wait_for(100ms) == std::future_status::ready && cancelled_close.get() == -EIO);
  assert(slow.Begin("browser", true));
  auto failed_close = std::async(std::launch::async, [&] { Json::Value reply; return slow.Call(close, reply); });
  request = slow.Poll("browser");
  Json::Value failed; failed["errno"] = EIO; failed["error"] = "SecurityError: file commit denied\nby browser";
  assert(slow.Reply("browser", request["id"].asUInt64(), failed));
  assert(failed_close.get() == -EIO);
  assert(slow.LastError() == "close: SecurityError: file commit denied by browser");
  slow.Stop();
  assert(slow.Begin("browser", true) && slow.LastError().empty());
  slow.Stop();
}
