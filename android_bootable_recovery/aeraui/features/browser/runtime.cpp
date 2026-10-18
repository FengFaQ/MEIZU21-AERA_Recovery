// SPDX-License-Identifier: Apache-2.0
#include "runtime.hpp"
#include <aera_browser_payload.hpp>
#include "plugins/plugin_manager.hpp"
#include <aeraui/i18n.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <mutex>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <vector>
extern "C" {
#include <xz.h>
}

namespace aeraui::web {
namespace {
constexpr unsigned long kTmpfs = 0x01021994, kRamfs = 0x858458f6;
struct RuntimeSpec {
  uint64_t compressed_bytes = 0;
  uint64_t expanded_bytes = 0;
  uint32_t member_count = 0;
  std::string compressed_hash;
  std::string expanded_hash;
};
struct FD {
  int value;
  explicit FD(int v = -1) : value(v) {}
  ~FD() { if (value >= 0) close(value); }
  FD(const FD &) = delete;
  FD &operator=(const FD &) = delete;
};
struct Hash {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
  ~Hash() { EVP_MD_CTX_free(ctx); }
  void Add(const void *data, size_t bytes) {
    ok = ok && EVP_DigestUpdate(ctx, data, bytes) == 1;
  }
  bool Matches(const char *expected) {
    unsigned char result[EVP_MAX_MD_SIZE]; unsigned length = 0;
    if (!ok || EVP_DigestFinal_ex(ctx, result, &length) != 1 || length != 32) return false;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < length; ++i)
      if (expected[i * 2] != hex[result[i] >> 4] ||
          expected[i * 2 + 1] != hex[result[i] & 15]) return false;
    return expected[64] == '\0';
  }
};
bool PathSafe(const std::string &path) {
  if (path.empty() || path.size() >= 240 || path.front() == '/' ||
      path.back() == '/' || path.find('\0') != std::string::npos) return false;
  size_t pos = 0;
  while (pos < path.size()) {
    auto end = path.find('/', pos);
    const auto part = path.substr(pos, end - pos);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return true;
}
// Traversal is relative to our private root. No existing symlink is followed.
int Parent(int root, const std::string &path) {
  int directory = dup(root);
  size_t pos = 0;
  while (directory >= 0) {
    auto end = path.find('/', pos);
    if (end == std::string::npos) return directory;
    const auto part = path.substr(pos, end - pos);
    if (mkdirat(directory, part.c_str(), 0755) != 0 && errno != EEXIST) {
      close(directory); return -1;
    }
    const int next = openat(directory, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(directory); directory = next; pos = end + 1;
  }
  return -1;
}
void ClearDirectory(int root) {
  DIR *directory = fdopendir(dup(root));
  if (!directory) return;
  while (auto *entry = readdir(directory)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    struct stat info{};
    if (fstatat(root, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (S_ISDIR(info.st_mode)) {
      FD sub(openat(root, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (sub.value >= 0) ClearDirectory(sub.value);
      unlinkat(root, entry->d_name, AT_REMOVEDIR);
    } else unlinkat(root, entry->d_name, 0);
  }
  closedir(directory);
}
struct Reader {
  struct xz_dec *decoder = nullptr;
  struct xz_buf buffer{};
  Preparation &state;
  Hash hash;
  const RuntimeSpec &spec;
  uint64_t total = 0;
  bool ended = false;
  Reader(const uint8_t *data, size_t size, Preparation &s,
         const RuntimeSpec &runtime) : state(s), spec(runtime) {
    static std::once_flag crc_initialized;
    std::call_once(crc_initialized, xz_crc32_init);
    decoder = xz_dec_init(XZ_DYNALLOC, 64U * 1024 * 1024);
    buffer.in = data; buffer.in_size = size;
  }
  ~Reader() { if (decoder) xz_dec_end(decoder); }
  bool Read(void *output, size_t size) {
    if (!decoder || ended || total + size > spec.expanded_bytes) return false;
    buffer.out = static_cast<uint8_t *>(output); buffer.out_pos = 0; buffer.out_size = size;
    while (buffer.out_pos < size) {
      if (state.cancel.load()) return false;
      const size_t old_in = buffer.in_pos, old_out = buffer.out_pos;
      auto result = xz_dec_run(decoder, &buffer);
      if (result == XZ_STREAM_END) { ended = true; break; }
      if (result != XZ_OK || (old_in == buffer.in_pos && old_out == buffer.out_pos)) return false;
    }
    if (buffer.out_pos != size) return false;
    total += size; hash.Add(output, size);
    state.progress.store(static_cast<unsigned>(total * 100 / spec.expanded_bytes));
    return true;
  }
  bool Finish() {
    if (total != spec.expanded_bytes) return false;
    uint8_t extra;
    buffer.out = &extra; buffer.out_pos = 0; buffer.out_size = 1;
    if (!ended) ended = xz_dec_run(decoder, &buffer) == XZ_STREAM_END;
    return ended && !buffer.out_pos && buffer.in_pos == buffer.in_size &&
           hash.Matches(spec.expanded_hash.c_str());
  }
};
uint64_t Little(const uint8_t *p, unsigned count) {
  uint64_t result = 0;
  for (unsigned i = 0; i < count; ++i) result |= uint64_t(p[i]) << (8 * i);
  return result;
}
bool Extract(Reader &reader, int root, const RuntimeSpec &spec) {
  std::array<uint8_t, 12> header{};
  if (!reader.Read(header.data(), header.size()) || memcmp(header.data(), "AERAWEB1", 8) ||
      Little(header.data() + 8, 4) != spec.member_count || spec.member_count > 4096) return false;
  std::set<std::string> names;
  std::vector<std::pair<std::string, std::string>> aliases;
  for (uint32_t i = 0; i < spec.member_count; ++i) {
    if (!reader.Read(header.data(), header.size())) return false;
    size_t length = Little(header.data(), 2);
    unsigned mode = Little(header.data() + 2, 2);
    uint64_t size = Little(header.data() + 4, 8);
    if (!length || length >= 240 || size > 100U * 1024 * 1024 ||
        (mode != 0 && mode != 0644 && mode != 0755)) return false;
    std::string name(length, '\0');
    if (!reader.Read(name.data(), length) || !PathSafe(name) || !names.insert(name).second) return false;
    const size_t padding = (4 - reader.total % 4) % 4;
    uint8_t zero[3]{};
    if (padding && (!reader.Read(zero, padding) || zero[0] || zero[1] || zero[2])) return false;
    FD parent(Parent(root, name));
    if (parent.value < 0) return false;
    const auto leaf = name.substr(name.find_last_of('/') + 1);
    if (!mode) {
      if (!size || size >= 240) return false;
      std::string target(size, '\0');
      if (!reader.Read(target.data(), size) || !PathSafe(target)) return false;
      aliases.emplace_back(name, target);
      continue;
    }
    FD file(openat(parent.value, leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    if (file.value < 0) return false;
    std::array<uint8_t, 65536> chunk{};
    while (size) {
      const size_t bytes = std::min<uint64_t>(chunk.size(), size);
      if (!reader.Read(chunk.data(), bytes)) return false;
      size_t offset = 0;
      while (offset < bytes) {
        const auto written = write(file.value, chunk.data() + offset, bytes - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += written;
      }
      size -= bytes;
    }
    if (fchmod(file.value, mode) != 0) return false;
  }
  if (!reader.Finish()) return false;
  for (const auto &alias : aliases) {
    if (!names.count(alias.second)) return false;
    struct stat info{};
    if (fstatat(root, alias.second.c_str(), &info, AT_SYMLINK_NOFOLLOW) || !S_ISREG(info.st_mode)) return false;
    if (linkat(root, alias.second.c_str(), root, alias.first.c_str(), 0)) return false;
  }
  return true;
}
}  // namespace

bool RuntimeInstalled() {
  plugins::Plugin plugin; std::string path, error;
  return plugins::ResolvePayload("browser", plugin, path, error) &&
         plugin.type == "browser-runtime" && plugin.entry == "browser";
}

bool PluginRuntimeInstalled(const char *id, const char *type,
                            const char *entry) {
  plugins::Plugin plugin; std::string path, error;
  return id && type && entry &&
         plugins::ResolvePayload(id, plugin, path, error) &&
         plugin.type == type && plugin.entry == entry;
}

void RemoveRuntime(const std::string &directory) {
  // Only private directories generated by this module, never a broad root.
  const auto leaf = directory.substr(directory.find_last_of('/') + 1);
  const bool browser = leaf.size() == 15 && leaf.compare(0, 9, "aera-web-") == 0;
  const bool retroarch = leaf.size() == 14 && leaf.compare(0, 8, "aera-ra-") == 0;
  const bool doom = leaf.size() == 16 && leaf.compare(0, 10, "aera-doom-") == 0;
  const bool telegram = leaf.size() == 14 && leaf.compare(0, 8, "aera-tg-") == 0;
  const bool media = leaf.size() == 17 && leaf.compare(0, 11, "aera-media-") == 0;
  const bool recorder = leaf.size() == 15 && leaf.compare(0, 9, "aera-rec-") == 0;
  const bool appvault = leaf.size() == 17 && leaf.compare(0, 11, "aera-vault-") == 0;
  const bool streams = leaf.size() == 24 &&
      leaf.compare(0, 18, "aera-streams-") == 0;
  const bool plugin_v2 = leaf.size() == 14 && leaf.compare(0, 8, "aera-p2-") == 0;
  if (!browser && !retroarch && !doom && !telegram && !media && !recorder && !appvault &&
      !streams &&
      !plugin_v2) return;
  FD root(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (root.value < 0) return;
  ClearDirectory(root.value);
  rmdir(directory.c_str());
}

void PreparePluginRuntime(Preparation &state, const char *id,
                          const char *type, const char *entry,
                          const char *parent) {
  struct Completion {
    Preparation &s;
    ~Completion() { s.done.store(true, std::memory_order_release); }
  } done{state};
  state.error = "Plugin runtime could not be prepared.";
  const bool retroarch = id && type && entry && !strcmp(id, "retroarch") &&
      !strcmp(type, "app-runtime") && !strcmp(entry, "retroarch");
  const bool doom = id && type && entry && !strcmp(id, "doom") &&
      !strcmp(type, "app-runtime") && !strcmp(entry, "doom");
  const bool telegram = id && type && entry && !strcmp(id, "telegram") &&
      !strcmp(type, "app-runtime") && !strcmp(entry, "telegram");
  const bool media = id && type && entry && !strcmp(id, "media") &&
      !strcmp(type, "app-runtime") && !strcmp(entry, "media");
  const bool recorder = id && type && entry && !strcmp(id, "recorder") &&
      !strcmp(type, "app-runtime") && !strcmp(entry, "recorder");
  const bool appvault = id && type && entry && !strcmp(id, "appvault") &&
      !strcmp(type, "app-runtime") && !strcmp(entry, "appvault");
  const bool streams = id && type && entry && !strcmp(id, "streams") &&
      !strcmp(type, "app-runtime") && !strcmp(entry, "streams");
  const bool plugin_v2 = id && type && entry &&
      !strcmp(type, "ui-runtime") && !strcmp(entry, "main");
  if (!retroarch && !doom && !telegram && !media && !recorder && !appvault &&
      !streams &&
      !plugin_v2) {
    state.error = "AERA rejected an unsupported plugin entry point.";
    return;
  }
  plugins::Plugin plugin;
  std::string selected, error;
  if (!plugins::ResolvePayload(id, plugin, selected, error) ||
      plugin.type != type || plugin.entry != entry ||
      (plugin_v2 && !plugins::IsGeneric(plugin))) {
    state.error = error.empty()
        ? "Install the plugin from Plugin Manager first." : error;
    return;
  }
  const std::string name = plugin.name;
  const RuntimeSpec spec = {plugin.payload_size, plugin.expanded_size,
      plugin.member_count, plugin.payload_sha256, plugin.expanded_sha256};
  FD file(open(selected.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  if (file.value < 0 || fstat(file.value, &info) || !S_ISREG(info.st_mode) ||
      static_cast<uint64_t>(info.st_size) != spec.compressed_bytes) {
    state.error = i18n::Format(
        "%s payload is missing or has an unexpected size.", name.c_str());
    return;
  }
  FD ram(open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct statfs filesystem{}; struct statvfs capacity{}; struct sysinfo memory{};
  if (ram.value < 0 || fstatfs(ram.value, &filesystem) ||
      (static_cast<unsigned long>(filesystem.f_type) != kTmpfs &&
       static_cast<unsigned long>(filesystem.f_type) != kRamfs)) {
    state.error = i18n::Format(
        "%s expansion requires RAM-backed temporary storage.", name.c_str());
    return;
  }
  const uint64_t reserve = spec.expanded_bytes + 96ULL * 1024 * 1024;
  if (sysinfo(&memory) || uint64_t(memory.freeram) * memory.mem_unit < reserve ||
      (static_cast<unsigned long>(filesystem.f_type) == kTmpfs &&
       (fstatvfs(ram.value, &capacity) ||
        uint64_t(capacity.f_bavail) * capacity.f_frsize < spec.expanded_bytes))) {
    state.error =
        i18n::Format("Not enough free RAM to prepare %s.", name.c_str());
    return;
  }
  void *mapped = mmap(nullptr, spec.compressed_bytes, PROT_READ, MAP_PRIVATE,
                      file.value, 0);
  if (mapped == MAP_FAILED) return;
  struct Mapping {
    void *data; size_t size;
    ~Mapping() { munmap(data, size); }
  } mapping{mapped, static_cast<size_t>(spec.compressed_bytes)};
  Hash compressed;
  for (uint64_t pos = 0; pos < spec.compressed_bytes; pos += 65536) {
    if (state.cancel.load()) {
      state.error =
          i18n::Format("%s preparation cancelled.", name.c_str());
      return;
    }
    compressed.Add(static_cast<const uint8_t *>(mapped) + pos,
        std::min<uint64_t>(65536, spec.compressed_bytes - pos));
  }
  if (!compressed.Matches(spec.compressed_hash.c_str())) {
    state.error =
        i18n::Format("%s payload integrity check failed.", name.c_str());
    return;
  }
  std::string temporary = std::string(parent) +
      (doom ? "/aera-doom-XXXXXX" :
       telegram ? "/aera-tg-XXXXXX" : media ? "/aera-media-XXXXXX" :
       recorder ? "/aera-rec-XXXXXX" : appvault ? "/aera-vault-XXXXXX" :
       streams ? "/aera-streams-XXXXXX" :
       plugin_v2 ? "/aera-p2-XXXXXX" :
       "/aera-ra-XXXXXX");
  if (!mkdtemp(temporary.data())) return;
  FD root(open(temporary.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  Reader reader(static_cast<const uint8_t *>(mapped), spec.compressed_bytes,
                state, spec);
  if (root.value < 0 || !Extract(reader, root.value, spec) ||
      state.cancel.load()) {
    RemoveRuntime(temporary);
    state.error = state.cancel.load()
        ? i18n::Format("%s preparation cancelled.", name.c_str())
        : i18n::Format(
              "%s runtime validation or extraction failed.", name.c_str());
    return;
  }
  state.directory = temporary;
  state.verified = true;
  state.error.clear();
}

void PrepareRuntime(Preparation &state, const char *payload, const char *parent) {
  // RAII publishes completion even on a normal early return. No worker touches LVGL.
  struct Completion { Preparation &s; ~Completion() { s.done.store(true, std::memory_order_release); } } done{state};
  state.error = "Browser runtime could not be prepared.";
  RuntimeSpec spec;
  std::string selected;
  if (payload && *payload) {
    selected = payload;
    spec = {kCompressedBytes, kExpandedBytes, kMemberCount,
            kCompressedHash, kExpandedHash};
  } else {
    plugins::Plugin plugin; std::string error;
    if (!plugins::ResolvePayload("browser", plugin, selected, error) ||
        plugin.type != "browser-runtime" || plugin.entry != "browser") {
      state.error = error.empty() ? "Install AERA Browser from Plugin Manager first." : error;
      return;
    }
    state.version = plugin.version;
    spec = {plugin.payload_size, plugin.expanded_size, plugin.member_count,
            plugin.payload_sha256, plugin.expanded_sha256};
  }
  FD file(open(selected.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  if (file.value < 0 || fstat(file.value, &info) || !S_ISREG(info.st_mode) ||
      static_cast<uint64_t>(info.st_size) != spec.compressed_bytes) {
    state.error = "Browser payload is missing or has an unexpected size."; return;
  }
  FD ram(open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct statfs filesystem{}; struct statvfs capacity{}; struct sysinfo memory{};
  if (ram.value < 0 || fstatfs(ram.value, &filesystem) ||
      (static_cast<unsigned long>(filesystem.f_type) != kTmpfs &&
       static_cast<unsigned long>(filesystem.f_type) != kRamfs)) {
    state.error = "Browser expansion requires a RAM-backed temporary directory."; return;
  }
  const uint64_t reserve = spec.expanded_bytes + 128ULL * 1024 * 1024;
  if (sysinfo(&memory) || uint64_t(memory.freeram) * memory.mem_unit < reserve ||
      (static_cast<unsigned long>(filesystem.f_type) == kTmpfs &&
       (fstatvfs(ram.value, &capacity) ||
        uint64_t(capacity.f_bavail) * capacity.f_frsize < spec.expanded_bytes))) {
    state.error = "Not enough free RAM to prepare the browser."; return;
  }
  void *mapped = mmap(nullptr, spec.compressed_bytes, PROT_READ, MAP_PRIVATE, file.value, 0);
  if (mapped == MAP_FAILED) return;
  struct Mapping {
    void *data;
    size_t size;
    ~Mapping() { munmap(data, size); }
  } mapping{mapped, static_cast<size_t>(spec.compressed_bytes)};
  Hash compressed;
  for (uint64_t pos = 0; pos < spec.compressed_bytes; pos += 65536) {
    if (state.cancel.load()) { state.error = "Browser preparation cancelled."; return; }
    compressed.Add(static_cast<const uint8_t *>(mapped) + pos,
                   std::min<uint64_t>(65536, spec.compressed_bytes - pos));
  }
  if (!compressed.Matches(spec.compressed_hash.c_str())) {
    state.error = "Browser payload integrity check failed."; return;
  }
  std::string temporary = std::string(parent) + "/aera-web-XXXXXX";
  if (!mkdtemp(temporary.data())) return;
  FD root(open(temporary.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  Reader reader(static_cast<const uint8_t *>(mapped), spec.compressed_bytes, state, spec);
  if (root.value < 0 || !Extract(reader, root.value, spec) || state.cancel.load()) {
    RemoveRuntime(temporary);
    state.error = state.cancel.load() ? "Browser preparation cancelled." : "Browser runtime validation or extraction failed.";
    return;
  }
  // Publish only the complete verified tree. Ownership is still recovery root;
  // only the fixed userspace jail may execute programs from it.
  state.directory = temporary; state.verified = true; state.error.clear();
}

std::string LaunchBlockReason() {
  return "The engine runs on demand as an unprivileged, memory-limited process. "
         "Recovery data and raw partitions are hidden from webpages. Wi-Fi setup is separate.";
}
}  // namespace aeraui::web
