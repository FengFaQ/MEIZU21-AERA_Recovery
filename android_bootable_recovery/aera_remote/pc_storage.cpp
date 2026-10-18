/* SPDX-License-Identifier: Apache-2.0 */
#include "pc_storage.hpp"
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <new>
#include <set>
#include <atomic>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef OF_ENABLE_WLAN
#if !defined(AERA_PC_HOST_TEST) || defined(AERA_PC_FUSE_TEST)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define FUSE_USE_VERSION 26
#include "fuse/include/fuse.h"
#include "fuse/include/fuse_lowlevel.h"
#include <sys/mount.h>
#endif
#endif

namespace aera::pc::storage {
namespace { std::atomic<int> write_error{0}; }
int WriteError() { return write_error.load(); }
bool SafePath(const std::string &path) {
  if (path.empty() || path[0] != '/' || path.size() > 2048 ||
      path.find('\0') != std::string::npos || path.find('\\') != std::string::npos) return false;
  size_t begin = 1;
  while (begin < path.size()) {
    const auto end = path.find('/', begin);
    const auto component = path.substr(begin, end - begin);
    if (component.empty() || component == "." || component == "..") return false;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return true;
}
bool Bridge::Begin(const std::string &owner, bool writable) {
  std::lock_guard<std::mutex> guard(lock_);
  if (active_ || waiting_ || owner.empty()) return false;
  active_ = true; writable_ = writable; owner_ = owner;
  transferred_ = 0; samples_.clear();
  last_error_.clear();
  samples_.push_back({std::chrono::steady_clock::now(), 0});
  waiting_ = answered_ = false; return true;
}
void Bridge::Stop() {
  std::lock_guard<std::mutex> guard(lock_);
  active_ = false; ready_.notify_all();
}
bool Bridge::Heartbeat(const std::string &owner) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!active_ || owner != owner_) return false;
  last_seen_ = std::chrono::steady_clock::now();
  ready_.notify_all();
  return true;
}
bool Bridge::Active() const { std::lock_guard<std::mutex> guard(lock_); return active_; }
bool Bridge::Writable() const { std::lock_guard<std::mutex> guard(lock_); return writable_; }
std::string Bridge::LastError() const { std::lock_guard<std::mutex> guard(lock_); return last_error_; }
TransferStatistics Bridge::Statistics() {
  std::lock_guard<std::mutex> guard(lock_);
  const auto now = std::chrono::steady_clock::now();
  const auto cutoff = now - std::chrono::seconds(4);
  if (active_ && !samples_.empty() && now - samples_.back().time >= std::chrono::milliseconds(250))
    samples_.push_back({now, transferred_});
  while (samples_.size() > 1 && samples_[1].time <= cutoff) samples_.pop_front();
  TransferStatistics result; result.bytes = transferred_;
  if (active_ && writable_ && waiting_ && !answered_ &&
      (request_["op"] == "close" || request_["op"] == "rename")) {
    result.waiting_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - request_started_).count();
    result.finalizing = result.waiting_seconds >= 1;
  }
  if (active_ && !samples_.empty()) {
    const auto start = std::max(cutoff, samples_.front().time);
    const double seconds = std::chrono::duration<double>(now - start).count();
    if (seconds >= .25) result.bytes_per_second = (transferred_ - samples_.front().bytes) / seconds;
  }
  return result;
}
Json::Value Bridge::Poll(const std::string &owner) {
  std::unique_lock<std::mutex> guard(lock_);
  if (active_ && owner == owner_)
    ready_.wait_for(guard, std::chrono::seconds(1), [&] { return !active_ || (waiting_ && !answered_); });
  if (!active_ || owner != owner_) return Json::Value();
  if (!waiting_ || answered_) return Json::Value(Json::objectValue);
  return request_;
}
bool Bridge::Data(const std::string &owner, uint64_t id, std::string &data) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!active_ || !waiting_ || answered_ || owner != owner_ || id != sequence_) return false;
  data = data_; return true;
}
bool Bridge::AppendData(const std::string &owner, uint64_t id, std::string &frame) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!active_ || !waiting_ || answered_ || owner != owner_ || id != sequence_) return false;
  frame.append(data_); return true;
}
bool Bridge::Reply(const std::string &owner, uint64_t id,
                   const Json::Value &reply, const std::string &data) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!active_ || !waiting_ || answered_ || owner != owner_ || id != sequence_ ||
      data.size() > kBlock) return false;
  if (!reply.isObject() || (reply.isMember("errno") &&
      (!reply["errno"].isInt() || reply["errno"].asInt() < 0 || reply["errno"].asInt() > 4095))) return false;
  if (!reply.get("errno", 0).asInt()) {
    const auto op = request_["op"].asString();
    if (op == "stat" && (!reply["directory"].isBool() || !reply["size"].isUInt64() ||
        reply["size"].asUInt64() > INT64_MAX || !reply["mtime"].isInt64())) return false;
    if (op == "read" && data.size() > request_["size"].asUInt64()) return false;
    if (op != "read" && !data.empty()) return false;
    if (op == "list") {
      if (!reply["entries"].isArray() || reply["entries"].size() > 4096) return false;
      for (const auto &entry : reply["entries"]) {
        if (!entry["name"].isString() || !entry["directory"].isBool()) return false;
        const auto name = entry["name"].asString();
        if (name.empty() || name.size() > 255 || name == "." || name == ".." ||
            name.find('/') != std::string::npos || name.find('\0') != std::string::npos) return false;
      }
    }
    const size_t bytes = op == "write" ? data_.size() : op == "read" ? data.size() : 0;
    if (bytes) {
      transferred_ += bytes;
      const auto now = std::chrono::steady_clock::now();
      const auto cutoff = now - std::chrono::seconds(4);
      while (samples_.size() > 1 && samples_[1].time <= cutoff) samples_.pop_front();
      // Sample at most four times per second, including idle samples from
      // Statistics(), so stalled streams decay to zero without unbounded RAM.
      if (now - samples_.back().time >= std::chrono::milliseconds(250))
        samples_.push_back({now, transferred_});
    }
  }
  const int error = reply.get("errno", 0).asInt();
  if (error && error != ENOENT && error != EEXIST && last_error_.empty()) {
    std::string detail = reply["error"].isString() ? reply["error"].asString() : strerror(error);
    detail.resize(std::min<size_t>(detail.size(), 1024));
    for (auto &character : detail)
      if (static_cast<unsigned char>(character) < 32 || character == 127) character = ' ';
    last_error_ = request_["op"].asString() + ": " + detail;
    fprintf(stderr, "PC storage: browser reported %s\n", last_error_.c_str());
  }
  reply_ = reply; reply_data_ = data; answered_ = true; ready_.notify_all(); return true;
}
int Bridge::Call(Json::Value request, Json::Value &reply, std::string *data,
                 const std::string &write_data) {
  if (!SafePath(request.get("path", "/").asString()) || write_data.size() > kBlock) return -EINVAL;
  std::unique_lock<std::mutex> guard(lock_);
  ready_.wait(guard, [&] { return !active_ || !waiting_; });
  if (!active_) return -EIO;
  request["id"] = Json::UInt64(++sequence_);
  request_ = std::move(request); data_ = write_data; waiting_ = true; answered_ = false;
  request_started_ = last_seen_ = std::chrono::steady_clock::now();
  const bool finalizing = writable_ && (request_["op"] == "close" || request_["op"] == "rename");
  const auto deadline = request_started_ + (finalizing ? finalize_timeout_ : operation_timeout_);
  ready_.notify_all();
  // File System Access commits its temporary file on close. Large files can
  // take longer than data writes; only an active storage pump may extend this wait.
  while (active_ && !answered_) {
    const auto until = finalizing ? std::min(deadline, last_seen_ + heartbeat_timeout_) : deadline;
    if (std::chrono::steady_clock::now() >= until) break;
    ready_.wait_until(guard, until);
  }
  int result = -EIO;
  if (active_ && answered_) {
    result = -std::clamp(reply_.get("errno", 0).asInt(), 0, 4095);
    reply = std::move(reply_);
    if (data) *data = std::move(reply_data_);
  } else {
    if (active_) fprintf(stderr, "PC storage: timed out waiting for %s %s\n",
        request_["op"].asCString(), request_["path"].asCString());
    active_ = false;
  }
  if (result && result != -ENOENT && result != -EEXIST)
    fprintf(stderr, "PC storage: %s %s failed: %s\n", request_["op"].asCString(),
        request_["path"].asCString(), strerror(-result));
  waiting_ = false; data_.clear(); reply_data_.clear(); ready_.notify_all();
  return result;
}
Bridge &Channel() { static Bridge bridge; return bridge; }

#if defined(OF_ENABLE_WLAN) && (!defined(AERA_PC_HOST_TEST) || defined(AERA_PC_FUSE_TEST))
namespace {
struct fuse *filesystem = nullptr;
struct fuse_chan *channel = nullptr;
std::thread loop;
struct OpenFile {
  std::string path, pending, read_data;
  off_t offset = 0, read_offset = 0;
  int error = 0;
};
// Bound outstanding file data to one block per open writer. Only the FUSE loop
// accesses these handles; fsync/close must deliver every byte before succeeding.
std::set<OpenFile *> writers;
OpenFile *File(fuse_file_info *info) {
  return reinterpret_cast<OpenFile *>(info->fh);
}
Json::Value Command(const char *op, const char *path) {
  Json::Value result; result["op"] = op; result["path"] = path; return result;
}
int Simple(const char *op, const char *path) {
  if (!Channel().Writable()) return -EROFS;
  Json::Value reply; return Channel().Call(Command(op, path), reply);
}
int Flush(OpenFile *file) {
  if (!file || file->pending.empty()) return file ? file->error : 0;
  Json::Value request = Command("write", file->path.c_str()), reply;
  request["offset"] = Json::Int64(file->offset);
  request["size"] = Json::UInt64(file->pending.size());
  file->error = Channel().Call(request, reply, nullptr, file->pending);
  if (file->error) write_error.store(file->error);
  file->pending.clear();
  return file->error;
}
int Attributes(const char *path, struct stat *info) {
  Json::Value reply; const int result = Channel().Call(Command("stat", path), reply);
  if (result) return result;
  *info = {}; info->st_mode = reply["directory"].asBool() ? S_IFDIR | 0755 : S_IFREG | 0644;
  info->st_nlink = reply["directory"].asBool() ? 2 : 1;
  info->st_size = reply["size"].asUInt64(); info->st_blksize = kBlock;
  for (const auto *file : writers)
    if (file->path == path && !file->pending.empty())
      info->st_size = std::max(info->st_size, file->offset + static_cast<off_t>(file->pending.size()));
  info->st_blocks = (info->st_size + 511) / 512; info->st_mtime = reply["mtime"].asInt64();
  return 0;
}
int List(const char *path, void *buffer, fuse_fill_dir_t fill, off_t offset, fuse_file_info *) {
  Json::Value request = Command("list", path), reply;
  request["offset"] = Json::Int64(offset);
  const int result = Channel().Call(request, reply);
  if (result) return result;
  off_t position = offset;
  for (const auto &entry : reply["entries"]) {
    const auto name = entry["name"].asString();
    if (name.empty() || name.find('/') != std::string::npos || name == "..") return -EIO;
    struct stat info{}; info.st_mode = entry["directory"].asBool() ? S_IFDIR : S_IFREG;
    if (fill(buffer, name.c_str(), &info, ++position)) break;
  }
  return 0;
}
int Open(const char *path, fuse_file_info *info) {
  if ((info->flags & O_ACCMODE) != O_RDONLY && !Channel().Writable()) return -EROFS;
  auto *file = new (std::nothrow) OpenFile;
  if (!file) return -ENOMEM;
  file->path = path; writers.insert(file);
  info->fh = reinterpret_cast<uintptr_t>(file);
  info->direct_io = 1;
  return 0;
}
int Create(const char *path, mode_t, fuse_file_info *info) {
  if (!Channel().Writable()) return -EROFS;
  Json::Value request = Command("create", path), reply;
  request["exclusive"] = (info->flags & O_EXCL) != 0;
  const int result = Channel().Call(request, reply);
  return result ? result : Open(path, info);
}
int Read(const char *path, char *buffer, size_t size, off_t offset, fuse_file_info *info) {
  for (auto *file : writers) if (file->path == path) {
    const int result = Flush(file); if (result) return result;
  }
  auto *file = File(info);
  if (!file || !Channel().Active()) return -EIO;
  if (offset >= file->read_offset && offset < file->read_offset + static_cast<off_t>(file->read_data.size())) {
    const size_t start = offset - file->read_offset;
    const size_t count = std::min(size, file->read_data.size() - start);
    memcpy(buffer, file->read_data.data() + start, count); return count;
  }
  Json::Value request = Command("read", path), reply;
  // Restic can keep many pack files open. Keep just one native read-ahead
  // block, rather than retaining a large cache for every open descriptor.
  for (auto *reader : writers)
    if (reader != file) std::string().swap(reader->read_data);
  request["offset"] = Json::Int64(offset); request["size"] = Json::UInt64(kReadBlock);
  const int result = Channel().Call(request, reply, &file->read_data);
  if (result) return result;
  file->read_offset = offset;
  const size_t count = std::min(size, file->read_data.size());
  memcpy(buffer, file->read_data.data(), count); return count;
}
int Write(const char *, const char *buffer, size_t size, off_t offset, fuse_file_info *info) {
  if (!Channel().Writable()) return -EROFS;
  auto *file = File(info);
  if (!file || size > kBlock || !Channel().Active()) return -EIO;
  if (file->error) return file->error;
  size_t retained = 0;
  for (auto *writer : writers) retained += writer->pending.capacity();
  if (retained + size > 2 * kBlock) {
    for (auto *writer : writers) if (writer != file && writer->pending.capacity() > 1024) {
      const int result = Flush(writer); if (result) return result;
      std::string().swap(writer->pending);
    }
  }
  for (auto *reader : writers) if (reader->path == file->path) reader->read_data.clear();
  if (!file->pending.empty() && (offset != file->offset + static_cast<off_t>(file->pending.size()) ||
      size > kBlock - file->pending.size())) {
    const int result = Flush(file); if (result) return result;
  }
  if (file->pending.empty()) file->offset = offset;
  file->pending.append(buffer, size);
  if (file->pending.size() == kBlock) {
    const int result = Flush(file); if (result) return result;
  }
  return static_cast<int>(size);
}
int Close(const char *path, fuse_file_info *info) {
  if (!Channel().Writable()) return 0;
  const int result = Flush(File(info)); if (result) return result;
  Json::Value reply;
  const int closed = Channel().Call(Command("close", path), reply);
  if (closed) write_error.store(closed);
  return closed;
}
int Release(const char *path, fuse_file_info *info) {
  const int result = Close(path, info);
  auto *file = File(info); writers.erase(file); delete file; info->fh = 0;
  return result;
}
int Sync(const char *path, int, fuse_file_info *info) { return Close(path, info); }
int Truncate(const char *path, off_t size) {
  if (!Channel().Writable()) return -EROFS;
  for (auto *file : writers) if (file->path == path) {
    const int result = Flush(file); if (result) return result;
    file->read_data.clear();
  }
  Json::Value request = Command("truncate", path), reply; request["size"] = Json::Int64(size);
  return Channel().Call(request, reply);
}
int Rename(const char *source, const char *target) {
  if (!Channel().Writable() || !SafePath(target)) return -EROFS;
  for (auto *file : writers) if (file->path == source || file->path == target) {
    const int result = Flush(file); if (result) return result;
  }
  Json::Value request = Command("rename", source), reply; request["target"] = target;
  const int result = Channel().Call(request, reply);
  if (!result) for (auto *file : writers) if (file->path == source) file->path = target;
  return result;
}
int Mkdir(const char *path, mode_t) { return Simple("mkdir", path); }
int Chmod(const char *, mode_t) { return Channel().Writable() ? 0 : -EROFS; }
int Chown(const char *, uid_t, gid_t) { return Channel().Writable() ? 0 : -EROFS; }
int Utime(const char *, struct utimbuf *) { return Channel().Writable() ? 0 : -EROFS; }
int Unlink(const char *path) { return Simple("unlink", path); }
int Rmdir(const char *path) { return Simple("rmdir", path); }
int Statfs(const char *, struct statvfs *info) {
  *info = {}; info->f_bsize = info->f_frsize = 4096; info->f_namemax = 255;
  // Folder picker APIs do not expose the actual free space on the PC disk.
  return 0;
}
}
bool Mount(std::string &error) {
  if (filesystem || !Channel().Active()) { error = "Computer storage is already in use."; return false; }
  write_error.store(0);
  mkdir(kMount, 0700);
  struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
  fuse_opt_add_arg(&args, "aera-pc"); fuse_opt_add_arg(&args, "-o");
  const std::string options = "fsname=aera-pc,max_read=" + std::to_string(kBlock) +
      ",max_write=" + std::to_string(kBlock) +
      ",big_writes,attr_timeout=0,entry_timeout=1,negative_timeout=0";
  fuse_opt_add_arg(&args, options.c_str());
  channel = fuse_mount(kMount, &args);
  if (channel) fcntl(fuse_chan_fd(channel), F_SETFD, FD_CLOEXEC);
  struct fuse_operations operations{};
  operations.getattr = Attributes; operations.readdir = List; operations.open = Open;
  operations.create = Create; operations.read = Read; operations.write = Write;
  operations.flush = Close; operations.release = Release; operations.truncate = Truncate;
  operations.fsync = Sync;
  operations.mkdir = Mkdir; operations.rename = Rename; operations.unlink = Unlink;
  operations.rmdir = Rmdir; operations.chmod = Chmod; operations.chown = Chown;
  operations.utime = Utime; operations.statfs = Statfs;
  if (channel) filesystem = fuse_new(channel, &args, &operations, sizeof(operations), nullptr);
  fuse_opt_free_args(&args);
  if (!filesystem) {
    if (channel) fuse_unmount(kMount, channel);
    channel = nullptr; error = "Computer streaming requires working FUSE support."; return false;
  }
  loop = std::thread([] { fuse_loop(filesystem); }); return true;
}
void Unmount() {
  Channel().Stop();
  if (filesystem) {
    fuse_exit(filesystem);
    umount2(kMount, MNT_DETACH);
    if (loop.joinable()) loop.join();
    fuse_unmount(kMount, channel); fuse_destroy(filesystem);
    for (auto *file : writers) delete file;
    writers.clear();
    filesystem = nullptr; channel = nullptr;
  }
}
#else
bool Mount(std::string &error) { error = "Computer filesystem is unavailable."; return false; }
void Unmount() { Channel().Stop(); }
#endif
}  // namespace aera::pc::storage
