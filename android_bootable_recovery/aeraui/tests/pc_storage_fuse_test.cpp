/* SPDX-License-Identifier: Apache-2.0 */
// Run only inside a private mount namespace. No device partitions are accessed.
#include "../../aera_remote/pc_storage.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace aera::pc::storage;
int main() {
  assert(geteuid() == 0);
  struct stat existing{};
  assert(lstat(kMount, &existing) != 0 && errno == ENOENT);
  char temporary[] = "/tmp/aera-pc-fuse-XXXXXX";
  const std::string root = mkdtemp(temporary);
  assert(!root.empty());
  assert(Channel().Begin("test", true));
  std::atomic<bool> running{true};
  std::atomic<unsigned> writes{0}, reads{0};
  std::thread computer([&] {
    while (running) {
      const auto request = Channel().Poll("test");
      if (!request.isMember("id")) continue;
      const auto id = request["id"].asUInt64();
      const auto op = request["op"].asString(), path = root + request["path"].asString();
      Json::Value reply(Json::objectValue); std::string data; int error = 0;
      if (op == "stat") {
        struct stat info{};
        if (stat(path.c_str(), &info)) error = errno;
        else { reply["directory"] = S_ISDIR(info.st_mode); reply["size"] = Json::UInt64(info.st_size); reply["mtime"] = Json::Int64(info.st_mtime); }
      } else if (op == "list") {
        reply["entries"] = Json::Value(Json::arrayValue);
        std::vector<Json::Value> entries;
        for (const auto &entry : std::filesystem::directory_iterator(path)) {
          Json::Value item; item["name"] = entry.path().filename().string(); item["directory"] = entry.is_directory(); entries.push_back(item);
        }
        for (size_t i = request["offset"].asUInt64(); i < entries.size(); ++i) reply["entries"].append(entries[i]);
      } else if (op == "mkdir") { if (mkdir(path.c_str(), 0700)) error = errno;
      } else if (op == "create") {
        int fd = open(path.c_str(), O_CREAT | O_WRONLY | (request["exclusive"].asBool() ? O_EXCL : 0), 0600);
        if (fd < 0) error = errno; else close(fd);
      } else if (op == "write" || op == "read") {
        int fd = open(path.c_str(), op == "write" ? O_WRONLY : O_RDONLY);
        if (fd < 0) error = errno;
        else {
          if (op == "write") {
            ++writes;
            assert(Channel().Data("test", id, data));
            if (pwrite(fd, data.data(), data.size(), request["offset"].asInt64()) != static_cast<ssize_t>(data.size())) error = EIO;
            data.clear();
          } else {
            ++reads;
            data.resize(request["size"].asUInt64());
            ssize_t count = pread(fd, data.data(), data.size(), request["offset"].asInt64());
            if (count < 0) { error = errno; data.clear(); } else data.resize(count);
          }
          close(fd);
        }
      } else if (op == "truncate") { if (truncate(path.c_str(), request["size"].asInt64())) error = errno;
      } else if (op == "rename") { if (rename(path.c_str(), (root + request["target"].asString()).c_str())) error = errno;
      } else if (op == "unlink") { if (unlink(path.c_str())) error = errno;
      } else if (op == "rmdir") { if (rmdir(path.c_str())) error = errno; }
      if (op == "close" && request["path"] == "/bad-tail.win") error = EIO;
      reply["errno"] = error;
      assert(Channel().Reply("test", id, reply, data));
    }
  });
  std::string error;
  assert(Mount(error));
  const auto folder = std::string(kMount) + "/Backup";
  assert(mkdir(folder.c_str(), 0700) == 0);
  const auto path = folder + "/data.ext4.win";
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
  const std::string block(1024 * 1024, 'x');
  assert(write(fd, block.data(), block.size()) == static_cast<ssize_t>(block.size()));
  assert(fsync(fd) == 0); assert(close(fd) == 0);
  fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd < 0 && errno == EEXIST);
  struct stat info{}; assert(stat(path.c_str(), &info) == 0 && info.st_size == static_cast<off_t>(block.size()));
  std::ifstream input(path, std::ios::binary); std::string read((std::istreambuf_iterator<char>(input)), {});
  assert(read == block); input.close();
  assert(std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator()) == 1);
  assert(truncate(path.c_str(), 17) == 0);
  const auto buffered_path = folder + "/buffered.win";
  fd = open(buffered_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
  const auto before_writes = writes.load();
  for (unsigned i = 0; i < 32; ++i)
    assert(write(fd, block.data(), 32768) == 32768);
  assert(writes.load() - before_writes == 1024 * 1024 / kBlock);
  assert(write(fd, "tail", 4) == 4);
  assert(stat(buffered_path.c_str(), &info) == 0 && info.st_size == 1024 * 1024 + 4);
  assert(fsync(fd) == 0); assert(close(fd) == 0);
  fd = open(buffered_path.c_str(), O_RDONLY); assert(fd >= 0);
  char digest_block[4096];
  const auto before_reads = reads.load();
  for (unsigned i = 0; i < 256; ++i) {
    assert(::read(fd, digest_block, sizeof(digest_block)) == sizeof(digest_block));
    for (const auto value : digest_block) assert(value == 'x');
  }
  assert(::read(fd, digest_block, sizeof(digest_block)) == 4);
  assert(std::string(digest_block, 4) == "tail");
  assert(::read(fd, digest_block, sizeof(digest_block)) == 0);
  assert(reads.load() - before_reads <= 6);
  close(fd); assert(unlink(buffered_path.c_str()) == 0);
  assert(rename(path.c_str(), (path + "000").c_str()) == 0);
  assert(unlink((path + "000").c_str()) == 0);
  assert(rmdir(folder.c_str()) == 0);
  fd = open((std::string(kMount) + "/bad-tail.win").c_str(), O_WRONLY | O_CREAT, 0600);
  assert(fd >= 0 && write(fd, "tail", 4) == 4);
  assert(fsync(fd) != 0 && errno == EIO);
  assert(WriteError() == -EIO);
  close(fd);
  fd = open((std::string(kMount) + "/interrupted.win").c_str(), O_WRONLY | O_CREAT, 0600);
  assert(fd >= 0);
  running = false; Channel().Stop(); computer.join();
  assert(write(fd, block.data(), 4096) < 0 && errno == EIO);
  close(fd);
  Unmount(); assert(rmdir(kMount) == 0);
  std::filesystem::remove_all(root);
}
