/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "incremental_apks.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>
#include <linux/incrementalfs.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>

namespace aeraui::appvault {
namespace {
std::mutex mount_mutex;

bool Directory(const std::string& path) {
  struct stat st{};
  return !lstat(path.c_str(), &st) && S_ISDIR(st.st_mode);
}

bool Fail(std::string& error, const std::string& message, const std::string& path) {
  error = message + ": " + path;
  return false;
}

bool SafeCode(const std::string& code) {
  const auto leaf = std::filesystem::path(code).filename().string();
  return code.compare(0, 10, "/data/app/") == 0 && code.size() > 10 && leaf != "." &&
         leaf != ".." && code.find("/../") == std::string::npos &&
         code.find("/./") == std::string::npos && code.find("//") == std::string::npos &&
         code.back() != '/';
}

bool CommittedBinding(const std::string& view, const IncrementalMapping& mapping) {
  // AOSP IncrementalService Metadata.proto: BindPoint source_subdir=2, dest_path=3.
  // Metadata is obtained through the mounted IncFS view, never by copying raw backing files.
  std::error_code error;
  auto iterator = std::filesystem::directory_iterator(view, error);
  const auto end = std::filesystem::directory_iterator();
  for (; !error && iterator != end; iterator.increment(error)) {
    const std::string name = iterator->path().filename().string();
    if (name.compare(0, 12, ".mountpoint.") != 0) continue;
    char bytes[4096];
    const ssize_t count =
        lgetxattr(iterator->path().c_str(), INCFS_XATTR_METADATA_NAME, bytes, sizeof(bytes));
    if (count <= 0) continue;
    if (MatchesIncrementalBinding(std::string(bytes, count), mapping)) return true;
  }
  return false;
}
}  // namespace

bool MatchesIncrementalBinding(const std::string& metadata, const IncrementalMapping& mapping) {
  using google::protobuf::internal::WireFormatLite;
  if (metadata.empty() || metadata.size() > 4096) return false;
  google::protobuf::io::CodedInputStream input(reinterpret_cast<const uint8_t*>(metadata.data()),
                                               static_cast<int>(metadata.size()));
  std::string source, target;
  uint32_t tag;
  while ((tag = input.ReadTag()) != 0) {
    bool valid;
    if (tag == WireFormatLite::MakeTag(2, WireFormatLite::WIRETYPE_LENGTH_DELIMITED))
      valid = WireFormatLite::ReadString(&input, &source);
    else if (tag == WireFormatLite::MakeTag(3, WireFormatLite::WIRETYPE_LENGTH_DELIMITED))
      valid = WireFormatLite::ReadString(&input, &target);
    else
      valid = WireFormatLite::SkipField(&input, tag);
    if (!valid) return false;
  }
  const std::string expected = std::filesystem::path(mapping.backing).parent_path().string() +
                               "/mount/" + mapping.subdirectory;
  return input.ConsumedEntireMessage() && source == expected && target == mapping.target;
}

bool FindIncrementalMapping(const std::string& code, const std::string& stores,
                            IncrementalMapping& mapping, std::string& error) {
  mapping = {};
  if (!SafeCode(code)) return Fail(error, "Invalid registered APK path", code);
  const size_t slash = code.find_last_of('/');
  const std::string package_directory = code.substr(slash + 1);
  const std::string target = code.substr(0, slash);
  if (target == "/data/app") return Fail(error, "Unsupported incremental APK layout", code);
  std::error_code ec;
  if (!Directory(stores)) return true;
  auto store_iterator = std::filesystem::directory_iterator(stores, ec);
  const auto end = std::filesystem::directory_iterator();
  for (; !ec && store_iterator != end; store_iterator.increment(ec)) {
    const auto& store = *store_iterator;
    const std::string backing = store.path().string() + "/backing_store";
    if (!Directory(store.path().string()) || !Directory(backing)) continue;
    auto file_iterator = std::filesystem::directory_iterator(backing, ec);
    for (; !ec && file_iterator != end; file_iterator.increment(ec)) {
      const auto& entry = *file_iterator;
      const std::string subdirectory = entry.path().filename().string();
      if (subdirectory.compare(0, 3, "st_") != 0 || !Directory(entry.path().string()) ||
          !Directory(entry.path().string() + "/" + package_directory))
        continue;
      struct stat st{};
      const std::string apk = entry.path().string() + "/" + package_directory + "/base.apk";
      if (lstat(apk.c_str(), &st) || !S_ISREG(st.st_mode)) continue;
      if (!mapping.backing.empty()) return Fail(error, "Ambiguous incremental APK location", code);
      mapping = { backing, subdirectory, target };
    }
    if (ec) return Fail(error, "Cannot read incremental backing store", backing);
  }
  if (ec) return Fail(error, "Cannot read incremental app stores", stores);
  return true;
}

bool IsIncrementalApk(const std::string& path) {
  struct statfs fs{};
  return !statfs(path.c_str(), &fs) && static_cast<unsigned long>(fs.f_type) == INCFS_MAGIC_NUMBER;
}

struct IncrementalApkViews::Impl {
  std::unique_lock<std::mutex> lock{ mount_mutex, std::defer_lock };
  std::string temporary;
  std::vector<std::string> bindings, filesystems;
};

IncrementalApkViews::IncrementalApkViews() : impl_(std::make_unique<Impl>()) {}

IncrementalApkViews::~IncrementalApkViews() {
  for (auto it = impl_->bindings.rbegin(); it != impl_->bindings.rend(); ++it) {
    if (umount2(it->c_str(), 0)) umount2(it->c_str(), MNT_DETACH);
  }
  for (auto it = impl_->filesystems.rbegin(); it != impl_->filesystems.rend(); ++it) {
    if (umount2(it->c_str(), 0)) umount2(it->c_str(), MNT_DETACH);
    rmdir(it->c_str());
  }
  if (!impl_->temporary.empty()) rmdir(impl_->temporary.c_str());
}

bool IncrementalApkViews::Prepare(const std::vector<std::string>& code_paths, std::string& error) {
  if (impl_->lock.owns_lock())
    return Fail(error, "Incremental view already prepared", "App Backup Vault");
  impl_->lock.lock();
  std::set<std::string> bound;
  for (const auto& code : code_paths) {
    if (access((code + "/base.apk").c_str(), R_OK) == 0) continue;
    IncrementalMapping mapping;
    if (!FindIncrementalMapping(code, "/data/incremental", mapping, error)) return false;
    if (mapping.backing.empty()) continue;
    if (bound.count(mapping.target)) continue;
    // Do not let IncFS create control directories in Android's backing store.
    if (!Directory(mapping.backing + "/.index") || !Directory(mapping.backing + "/.incomplete"))
      return Fail(error, "Incremental APK control directories are missing", mapping.backing);
    std::error_code ec;
    if (!Directory(mapping.target) || !std::filesystem::is_empty(mapping.target, ec) || ec)
      return Fail(error, "Refusing to cover a nonempty APK folder", mapping.target);
    if (impl_->temporary.empty()) {
      char temporary[] = "/tmp/aera-appvault-incfs-XXXXXX";
      if (!mkdtemp(temporary))
        return Fail(error, "Cannot create incremental APK view", strerror(errno));
      impl_->temporary = temporary;
    }
    const std::string view = impl_->temporary + "/" + std::to_string(impl_->filesystems.size());
    if (mkdir(view.c_str(), 0700)) return Fail(error, "Cannot prepare incremental APK view", view);
    constexpr unsigned long flags = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOATIME;
    if (mount(mapping.backing.c_str(), view.c_str(), INCFS_NAME, flags,
              "read_timeout_ms=1000,rlog_pages=0,readahead=0")) {
      const int saved_errno = errno;
      rmdir(view.c_str());
      return Fail(error, "Cannot read incremental APKs in this kernel", strerror(saved_errno));
    }
    impl_->filesystems.push_back(view);
    if (!CommittedBinding(view, mapping))
      return Fail(error, "Incremental APK has no matching active Android mount record", code);
    const std::string source = view + "/" + mapping.subdirectory;
    if (!Directory(source) ||
        mount(source.c_str(), mapping.target.c_str(), nullptr, MS_BIND, nullptr))
      return Fail(error, "Cannot expose incremental APK directory", mapping.target);
    impl_->bindings.push_back(mapping.target);
    if (mount(nullptr, mapping.target.c_str(), nullptr,
              MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, nullptr))
      return Fail(error, "Cannot protect incremental APK view as read-only", mapping.target);
    bound.insert(mapping.target);
  }
  return true;
}
}  // namespace aeraui::appvault
