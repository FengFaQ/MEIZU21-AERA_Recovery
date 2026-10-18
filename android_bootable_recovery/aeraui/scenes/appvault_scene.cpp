/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scene.hpp"
#include "../../aera_remote/backup_cancel.hpp"
#include "../../aera_shared_storage.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <androidfw/Asset.h>
#include <androidfw/AssetManager.h>
#include <androidfw/ResourceTypes.h>
#include <json/json.h>
#include <openssl/evp.h>
#include <png.h>
#include <utils/String8.h>

#include "android_icon.hpp"
#include "browser/runtime.hpp"
#include "backup/app_vault.hpp"
#include "backup/app_vault_progress.hpp"
#include "backup/app_registration.hpp"
#include "backup/incremental_apks.hpp"
#include "ui_components.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#ifdef OF_ENABLE_WLAN
#include "../../nas/NasManager.hpp"
#endif

namespace aeraui {
namespace {
using namespace design;
using namespace widgets;

constexpr char kPluginId[] = "appvault";
constexpr char kPluginType[] = "app-runtime";
constexpr char kPluginEntry[] = "appvault";
constexpr char kDefaultRepository[] = "/sdcard/AERA/AppBackupVault";
constexpr char kInternalPassword[] = "AERA-App-Backup-Vault-Internal-v1";
constexpr char kTag[] = "aera-appvault";
constexpr uint32_t kLabelAttribute = 0x01010001;
constexpr uint32_t kIconAttribute = 0x01010002;
constexpr uint32_t kRoundIconAttribute = 0x0101052c;
constexpr uint32_t kIconPixels = 76;

enum class Work {
  kNone,
  kBackup,
  kRefresh,
  kRestore,
  kDiscover,
  kStorage
};

struct AppIcon {
  std::vector<uint8_t> pixels;
  lv_image_dsc_t descriptor{};
};

struct App {
  std::string package;
  std::string name;
  std::string apk;
  std::vector<std::string> paths;
  std::unique_ptr<AppIcon> icon;
  bool system = true;
  bool selected = false;
};

struct SnapshotApp {
  std::string package;
  std::string name;
  std::string icon_png;
  std::unique_ptr<AppIcon> icon;
  std::vector<std::string> paths;
  bool selected = true;
};

struct Snapshot {
  std::string id;
  std::string time;
  uint64_t bytes = 0;
  std::vector<SnapshotApp> apps;
};

struct State {
  lv_obj_t *screen = nullptr;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *selected = nullptr;
  lv_obj_t *system_filter_label = nullptr;
  lv_obj_t *select_all_label = nullptr;
  lv_obj_t *user_button = nullptr;
  lv_obj_t *user_label = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *backup = nullptr;
  lv_obj_t *refresh_button = nullptr;
  lv_obj_t *restore = nullptr;
  lv_obj_t *storage_button = nullptr;
  lv_obj_t *storage_label = nullptr;
  lv_timer_t *timer = nullptr;
  web::Preparation preparation;
  std::thread prepare_thread;
  std::thread worker;
  std::atomic<bool> preparation_handled{false};
  std::atomic<bool> metadata_done{false};
  std::atomic<bool> busy{false};
  std::atomic<bool> done{false};
  std::atomic<bool> cancel{false};
  std::atomic<pid_t> child{-1};
  std::atomic<unsigned> work_progress{0};
  std::atomic<uint64_t> work_bytes_done{0};
  std::atomic<uint64_t> work_bytes_total{0};
  std::atomic<uint64_t> work_files_done{0};
  std::atomic<uint64_t> work_files_total{0};
  std::atomic<bool> work_success{false};
  std::atomic<bool> waiting_for_nas{false};
  std::mutex result_mutex;
  std::string runtime;
  std::string repository = kDefaultRepository;
  std::string storage_root;
  std::string pending_storage;
  std::string result;
  std::string latest_id;
  std::string latest_time;
  std::string restore_snapshot;
  unsigned snapshots = 0;
  bool browse_after_refresh = false;
  bool show_system = false;
  int user_id = 0;
  Work work = Work::kNone;
  std::vector<AndroidUser> users;
  std::vector<App> apps;
  std::vector<Snapshot> snapshot_items;
  std::function<void(const AppVaultProgress &)> remote_progress;
};

std::string HumanBytes(uint64_t bytes);

bool SafePackage(const char *name) {
  if (!name || !*name || strlen(name) > 255) return false;
  if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
  for (const unsigned char c : std::string(name))
    if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
  return true;
}

bool Directory(const std::string &path) {
  struct stat info{};
  return !lstat(path.c_str(), &info) && S_ISDIR(info.st_mode);
}

std::set<std::string> UserPackages() {
  std::set<std::string> result;
  FILE *file = fopen("/data/system/packages.list", "re");
  if (!file) return result;
  char *line = nullptr;
  size_t capacity = 0;
  while (getline(&line, &capacity, file) >= 0) {
    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (stream >> field) fields.push_back(std::move(field));
    // Android 16's package list carries SELinux package provenance in field
    // five. System packages contain partition=system/product/vendor/etc and
    // end in @system; Play Store, sideloaded and adb-installed apps do not.
    if (fields.size() >= 6 && SafePackage(fields[0].c_str()) &&
        fields[4].find("partition=") == std::string::npos &&
        fields.back() != "@system") result.insert(fields[0]);
  }
  free(line);
  fclose(file);
  return result;
}

void FitCompactButtonText(lv_obj_t *label) {
  if (label == nullptr) return;
  auto *button = lv_obj_get_parent(label);
  const int width =
      std::max(40, static_cast<int>(lv_obj_get_width(button)) - 32);
  FitLabelToLines(label, width, 1,
                  {&lv_font_montserrat_24, &lv_font_montserrat_20,
                   &lv_font_montserrat_18, &lv_font_montserrat_16});
  lv_obj_center(label);
}

void AddCodePath(std::map<std::string, std::string> &result,
                 const std::string &path, const std::string &leaf) {
  const size_t split = leaf.find('-');
  const std::string package = split == std::string::npos
      ? leaf : leaf.substr(0, split);
  if (SafePackage(package.c_str()) && Directory(path) &&
      access((path + "/base.apk").c_str(), R_OK) == 0)
    result[package] = path;
}

std::map<std::string, std::string> DataAppCodePaths() {
  std::map<std::string, std::string> result;
  DIR *root = opendir("/data/app");
  if (!root) return result;
  while (const dirent *outer = readdir(root)) {
    if (!strcmp(outer->d_name, ".") || !strcmp(outer->d_name, "..")) continue;
    const std::string outer_path = std::string("/data/app/") + outer->d_name;
    if (!Directory(outer_path)) continue;
    AddCodePath(result, outer_path, outer->d_name);
    DIR *container = opendir(outer_path.c_str());
    if (!container) continue;
    while (const dirent *inner = readdir(container)) {
      if (!strcmp(inner->d_name, ".") || !strcmp(inner->d_name, "..")) continue;
      AddCodePath(result, outer_path + "/" + inner->d_name, inner->d_name);
    }
    closedir(container);
  }
  closedir(root);
  return result;
}

std::vector<std::string> SiblingApks(const std::string &base) {
  std::vector<std::string> paths;
  const size_t slash = base.find_last_of('/');
  if (slash == std::string::npos) return paths;
  const std::string directory = base.substr(0, slash);
  DIR *handle = opendir(directory.c_str());
  if (!handle) return paths;
  while (const dirent *entry = readdir(handle)) {
    const std::string name = entry->d_name;
    if (name == "." || name == ".." || name == "base.apk" ||
        name.size() < 4 || name.compare(name.size() - 4, 4, ".apk")) continue;
    paths.push_back(directory + "/" + name);
  }
  closedir(handle);
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string FriendlyPackageName(const std::string &package) {
  const size_t dot = package.find_last_of('.');
  std::string name = package.substr(dot == std::string::npos ? 0 : dot + 1);
  for (char &c : name) {
    if (c == '_' || c == '-') c = ' ';
  }
  bool capitalize = true;
  for (char &c : name) {
    if (capitalize && std::isalpha(static_cast<unsigned char>(c)))
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    capitalize = c == ' ';
  }
  return name.empty() ? package : name;
}

android::String8 ResolvedAttribute(const android::ResTable &resources,
                                   const android::ResXMLTree &tree,
                                   uint32_t attribute) {
  ssize_t index = -1;
  for (size_t i = 0; i < tree.getAttributeCount(); ++i) {
    if (tree.getAttributeNameResID(i) == attribute) {
      index = static_cast<ssize_t>(i);
      break;
    }
  }
  if (index < 0) return {};
  android::Res_value value{};
  if (tree.getAttributeValue(index, &value) == android::BAD_TYPE) return {};
  if (value.dataType == android::Res_value::TYPE_STRING) {
    size_t length = 0;
    const char16_t *text = tree.getAttributeStringValue(index, &length);
    return text ? android::String8(text, length) : android::String8();
  }
  const ssize_t block = resources.resolveReference(&value, 0);
  if (block < 0 || value.dataType != android::Res_value::TYPE_STRING) return {};
  size_t length = 0;
  const char16_t *text = resources.valueToString(
      &value, static_cast<size_t>(block), nullptr, &length);
  return text ? android::String8(text, length) : android::String8();
}

std::unique_ptr<AppIcon> LoadIcon(const std::string &apk,
                                  const std::vector<std::string> &resources) {
  auto icon = std::make_unique<AppIcon>();
  if (!DecodeAndroidIcon(apk, resources, kIconPixels, icon->pixels))
    return nullptr;
  auto &descriptor = icon->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = kIconPixels;
  descriptor.header.h = kIconPixels;
  descriptor.header.stride = kIconPixels * 4;
  descriptor.data_size = icon->pixels.size();
  descriptor.data = icon->pixels.data();
  return icon;
}

void ReadAppMetadata(App &app) {
  if (app.apk.empty()) return;
  android::AssetManager assets;
  int32_t cookie = 0;
  if (!assets.addAssetPath(android::String8(app.apk.c_str()), &cookie)) return;
  for (const auto &split : SiblingApks(app.apk)) {
    int32_t split_cookie = 0;
    assets.addAssetPath(android::String8(split.c_str()), &split_cookie);
  }
  android::ResTable_config configuration{};
  configuration.density = 480;
  assets.setConfiguration(configuration, "en-US");
  std::unique_ptr<android::Asset> manifest(assets.openNonAsset(
      cookie, "AndroidManifest.xml", android::Asset::ACCESS_BUFFER));
  if (!manifest) return;
  android::ResXMLTree tree;
  if (tree.setTo(manifest->getBuffer(true), manifest->getLength()) != android::NO_ERROR)
    return;
  while (true) {
    const auto event = tree.next();
    if (event == android::ResXMLTree::END_DOCUMENT ||
        event == android::ResXMLTree::BAD_DOCUMENT) break;
    if (event != android::ResXMLTree::START_TAG) continue;
    size_t length = 0;
    const char16_t *tag_text = tree.getElementName(&length);
    if (!tag_text || android::String8(tag_text, length) != "application") continue;
    const android::ResTable &resources = assets.getResources();
    const android::String8 label = ResolvedAttribute(resources, tree, kLabelAttribute);
    if (!label.empty()) app.name = label.c_str();
    std::vector<std::string> icons;
    const android::String8 icon = ResolvedAttribute(resources, tree, kIconAttribute);
    const android::String8 round = ResolvedAttribute(resources, tree, kRoundIconAttribute);
    if (!icon.empty()) icons.emplace_back(icon.c_str());
    if (!round.empty() && round != icon) icons.emplace_back(round.c_str());
    app.icon = LoadIcon(app.apk, icons);
    break;
  }
}

std::string RemoteIcon(const AppIcon *icon) {
  if (!icon) return {};
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = kIconPixels;
  image.height = kIconPixels;
  image.format = PNG_FORMAT_BGRA;
  png_alloc_size_t length = 0;
  if (!png_image_write_to_memory(&image, nullptr, &length, 0,
                                icon->pixels.data(), 0, nullptr) ||
      length > 32768) return {};
  std::vector<unsigned char> png(length);
  if (!png_image_write_to_memory(&image, png.data(), &length, 0,
                                icon->pixels.data(), 0, nullptr)) return {};
  std::string encoded(4 * ((length + 2) / 3) + 1, '\0');
  const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                 png.data(), static_cast<int>(length));
  if (size <= 0) return {};
  encoded.resize(size);
  return "data:image/png;base64," + encoded;
}

std::string RemoteAppIcon(const App *app) {
  return RemoteIcon(app ? app->icon.get() : nullptr);
}

bool SavedIconEncoding(const std::string &uri) {
  constexpr char prefix[] = "data:image/png;base64,";
  if (uri.compare(0, sizeof(prefix) - 1, prefix)) return false;
  const std::string encoded = uri.substr(sizeof(prefix) - 1);
  return !encoded.empty() && encoded.size() <= 44000 && encoded.size() % 4 == 0 &&
      encoded.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") ==
          std::string::npos;
}

std::unique_ptr<AppIcon> SavedAppIcon(const std::string &uri) {
  if (!SavedIconEncoding(uri)) return nullptr;
  const std::string encoded = uri.substr(strlen("data:image/png;base64,"));
  std::vector<unsigned char> bytes(encoded.size() / 4 * 3);
  int size = EVP_DecodeBlock(bytes.data(),
      reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size());
  if (size <= 0) return nullptr;
  if (encoded.back() == '=') --size;
  if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') --size;
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&image, bytes.data(), size)) return nullptr;
  auto icon = std::make_unique<AppIcon>();
  bool okay = image.width == kIconPixels && image.height == kIconPixels;
  if (okay) {
    image.format = PNG_FORMAT_BGRA;
    icon->pixels.resize(kIconPixels * kIconPixels * 4);
    okay = png_image_finish_read(&image, nullptr, icon->pixels.data(), 0, nullptr);
  }
  png_image_free(&image);
  if (!okay) return nullptr;
  auto &descriptor = icon->descriptor;
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = kIconPixels;
  descriptor.header.h = kIconPixels;
  descriptor.header.stride = kIconPixels * 4;
  descriptor.data_size = icon->pixels.size();
  descriptor.data = icon->pixels.data();
  return icon;
}

std::vector<App> DiscoverApps(const std::atomic<bool> &cancel, int user_id,
                              bool metadata = true) {
  appvault::IncrementalApkViews incremental;
  std::vector<std::string> registered;
  std::string incremental_error;
  if (!appvault::RegisteredApkDirectories(registered, incremental_error) ||
      !incremental.Prepare(registered, incremental_error))
    fprintf(stderr, "App Backup Vault: %s\n", incremental_error.c_str());
  std::vector<App> apps;
  const std::string id = std::to_string(user_id);
  const std::vector<std::string> roots = {
      "/data/user/" + id,
      "/data/user_de/" + id,
      "/data/media/" + id + "/Android/data",
      "/data/media/" + id + "/Android/obb",
      "/data/media/" + id + "/Android/media"};
  std::set<std::string> packages;
  for (const auto &root : roots) {
    DIR *directory = opendir(root.c_str());
    if (!directory) continue;
    while (const dirent *entry = readdir(directory)) {
      if (SafePackage(entry->d_name) &&
          Directory(root + "/" + entry->d_name))
        packages.insert(entry->d_name);
    }
    closedir(directory);
  }
  const auto user_packages = UserPackages();
  const auto code_paths = DataAppCodePaths();
  for (const auto &package : packages) {
    App app;
    app.package = package;
    app.name = FriendlyPackageName(app.package);
    const auto code = code_paths.find(app.package);
    app.system = user_packages.count(app.package) == 0;
    app.selected = !app.system;
    const std::vector<std::string> candidates = {
        roots[0] + "/" + app.package,
        roots[1] + "/" + app.package,
        roots[2] + "/" + app.package,
        roots[3] + "/" + app.package,
        roots[4] + "/" + app.package};
    for (const auto &path : candidates)
      if (Directory(path)) app.paths.push_back(path);
    if (code != code_paths.end()) {
      app.paths.push_back(code->second);
      app.apk = code->second + "/base.apk";
    }
    if (!app.paths.empty()) apps.push_back(std::move(app));
  }
  for (auto &app : apps) {
    if (cancel.load(std::memory_order_relaxed)) break;
    if (metadata) ReadAppMetadata(app);
  }
  std::sort(apps.begin(), apps.end(), [](const App &left, const App &right) {
    std::string left_name = left.name;
    std::string right_name = right.name;
    std::transform(left_name.begin(), left_name.end(), left_name.begin(), ::tolower);
    std::transform(right_name.begin(), right_name.end(), right_name.begin(), ::tolower);
    return left_name == right_name ? left.package < right.package : left_name < right_name;
  });
  return apps;
}

bool ValidRepository(const std::string &path) {
  if (path.empty() || path.size() > 1024 || path.find("..") != std::string::npos)
    return false;
  const auto storages = RecoveryVolumes("storage");
  return path.compare(0, 8, "/sdcard/") == 0 ||
      path.compare(0, 14, "/data/media/0/") == 0 ||
      path.compare(0, 9, "/mnt/nas/") == 0 || path == "/pc" || path.compare(0, 4, "/pc/") == 0 ||
      std::any_of(storages.begin(), storages.end(),
                  [&](const Volume &volume) { return path.compare(0, volume.path.size() + 1,
                                                        volume.path + "/") == 0; });
}

bool MakeDirectories(const std::string &path) {
  if (!ValidRepository(path) || !RecoveryBackupPathReady(path)) return false;
  std::string current;
  size_t start = 1;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    current = path.substr(0, slash == std::string::npos ? path.size() : slash);
    if (!current.empty() && mkdir(current.c_str(), 0700) && errno != EEXIST)
      return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return Directory(path);
}

std::string LastUsefulLine(const std::string &text) {
  size_t end = text.find_last_not_of("\r\n ");
  if (end == std::string::npos) return "The backup engine did not return details.";
  size_t begin = text.rfind('\n', end);
  std::string line = text.substr(begin == std::string::npos ? 0 : begin + 1,
                                 end - (begin == std::string::npos ? 0 : begin + 1) + 1);
  Json::Value message;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (reader->parse(line.data(), line.data() + line.size(), &message, &errors) &&
      message.isObject() && message["message"].isString()) {
    line = message["message"].asString();
    if (line.compare(0, 7, "Fatal: ") == 0) line.erase(0, 7);
  }
  if (line.size() > 220) line.resize(220);
  return line;
}

void ParseProgressLine(State *state, const std::string &line) {
  Json::Value message;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (state->work != Work::kBackup && state->work != Work::kRestore) return;
  ResticProgress counters;
  if (!reader->parse(line.data(), line.data() + line.size(), &message, &errors) ||
      !ReadResticProgress(message, state->work == Work::kRestore, counters))
    return;

  state->work_progress.store(counters.percent);
  state->work_bytes_total.store(counters.bytes_total);
  state->work_files_total.store(counters.files_total);
  state->work_bytes_done.store(counters.bytes_done);
  state->work_files_done.store(counters.files_done);
  if (state->remote_progress) {
    AppVaultProgress progress;
    progress.percent = state->work_progress.load();
    progress.stage = state->work == Work::kRestore ? "Restoring apps" : "Backing up selected apps\u2026";
    if (counters.bytes_total)
      progress.amount = HumanBytes(counters.bytes_done) + " / " + HumanBytes(counters.bytes_total);
    else if (counters.bytes_done)
      progress.amount = HumanBytes(counters.bytes_done);
    progress.files_done = state->work_files_done.load();
    progress.files_total = state->work_files_total.load();
    if (message["current_files"].isArray()) {
      for (const auto &file : message["current_files"])
        if (file.isString() && !file.asString().empty()) { progress.current_file = file.asString(); break; }
    } else if (message["current_file"].isString()) progress.current_file = message["current_file"].asString();
    state->remote_progress(progress);
  }
}

bool WriteAll(int fd, const std::string &value) {
  size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t count = write(fd, value.data() + offset, value.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

int RunRestic(State *state, const std::vector<std::string> &arguments,
              std::string &output) {
  if (aera::pc::BackupCancelRequested()) { output = "Backup operation cancelled."; return -1; }
  const std::string program = state->runtime + "/usr/bin/restic";
  struct stat info{};
  if (lstat(program.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_uid ||
      (info.st_mode & 0022) || !(info.st_mode & 0100)) {
    output = "The verified backup engine is unavailable.";
    return -1;
  }
  char password_path[] = "/tmp/aera-vault-password-XXXXXX";
  const int password_fd = mkstemp(password_path);
  if (password_fd < 0 || fchmod(password_fd, 0600) ||
      !WriteAll(password_fd, std::string(kInternalPassword) + "\n")) {
    if (password_fd >= 0) close(password_fd);
    unlink(password_path);
    output = "Could not create a private password channel.";
    return -1;
  }
  fsync(password_fd);
  close(password_fd);
  mkdir("/tmp/aera-restic-cache", 0700);

  int channel[2] = {-1, -1};
  if (pipe2(channel, O_CLOEXEC)) {
    unlink(password_path);
    output = "Could not create the backup engine channel.";
    return -1;
  }
  std::vector<std::string> values = {program, "--repo", state->repository,
                                     "--password-file", password_path};
  if (state->remote_progress) values.push_back("--no-cache");
  if (state->remote_progress && state->repository.compare(0, 3, "/pc") == 0 && state->work != Work::kBackup)
    values.push_back("--no-lock");
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  const pid_t child = fork();
  if (!child) {
    if (aera::pc::BackupOperationActive()) setpgid(0, 0);
    dup2(channel[1], STDOUT_FILENO);
    dup2(channel[1], STDERR_FILENO);
    close(channel[0]);
    close(channel[1]);
    setenv("TMPDIR", "/tmp", 1);
    setenv("RESTIC_CACHE_DIR", "/tmp/aera-restic-cache", 1);
    setenv("RESTIC_PROGRESS_FPS", "2", 1);
    setenv("GOMAXPROCS", "4", 1);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  close(channel[1]);
  if (child < 0) {
    close(channel[0]);
    unlink(password_path);
    output = "Could not start the backup engine.";
    return -1;
  }
  state->child.store(child);
  aera::pc::BackupChildCancellation cancellation(child);
  char buffer[8192];
  std::string pending;
  while (true) {
    const ssize_t count = read(channel[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    output.append(buffer, static_cast<size_t>(count));
    pending.append(buffer, static_cast<size_t>(count));
    size_t newline = 0;
    while ((newline = pending.find('\n')) != std::string::npos) {
      ParseProgressLine(state, pending.substr(0, newline));
      pending.erase(0, newline + 1);
    }
    if (output.size() > (1U << 20)) output.erase(0, output.size() - (1U << 19));
    if (state->cancel.load()) kill(child, SIGTERM);
  }
  if (!pending.empty()) ParseProgressLine(state, pending);
  close(channel[0]);
  int status = 0;
  cancellation.Stop();
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  state->child.store(-1);
  unlink(password_path);
  return !aera::pc::BackupCancelRequested() && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string PackageFromBackupPath(std::string path, int user_id) {
  if (path.empty()) return {};
  if (path[0] != '/') path.insert(path.begin(), '/');
  const std::string id = std::to_string(user_id);
  const std::vector<std::string> prefixes = {
      std::string(appvault::kMetadataRoot) + "/" + id + "/",
      "/data/user/" + id + "/", "/data/user_de/" + id + "/",
      "/data/media/" + id + "/Android/data/",
      "/data/media/" + id + "/Android/obb/",
      "/data/media/" + id + "/Android/media/"};
  for (const auto &prefix : prefixes) {
    if (path.compare(0, prefix.size(), prefix)) continue;
    std::string package = path.substr(prefix.size());
    const size_t slash = package.find('/');
    if (slash != std::string::npos) package.resize(slash);
    return SafePackage(package.c_str()) ? package : std::string();
  }
  if (path.compare(0, 10, "/data/app/") == 0) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
      std::string package = path.substr(slash + 1);
      const size_t suffix = package.find('-');
      if (suffix != std::string::npos) package.resize(suffix);
      if (SafePackage(package.c_str())) return package;
    }
  }
  return {};
}

const App *InstalledApp(const State *state, const std::string &package) {
  const auto match = std::find_if(state->apps.begin(), state->apps.end(),
      [&](const App &app) { return app.package == package; });
  return match == state->apps.end() ? nullptr : &*match;
}

bool SaveAppDisplay(State *state, const std::vector<std::string> &folders, std::string &error) {
  for (const auto &folder : folders) {
    const std::string package = folder.substr(folder.find_last_of('/') + 1);
    App discovered;
    const App *app = InstalledApp(state, package);
    if (!app) {
      discovered.package = package;
      discovered.name = FriendlyPackageName(package);
      tinyxml2::XMLDocument registration;
      if (registration.LoadFile((folder + "/registration.xml").c_str()) == tinyxml2::XML_SUCCESS) {
        const auto *root = registration.RootElement();
        const auto *node = root ? root->FirstChildElement("package") : nullptr;
        if (node && node->Attribute("codePath")) {
          discovered.apk = std::string(node->Attribute("codePath")) + "/base.apk";
          ReadAppMetadata(discovered);
        }
      }
      app = &discovered;
    }
    Json::Value display;
    display["schema"] = 1;
    display["package"] = package;
    display["name"] = app->name;
    display["icon"] = RemoteAppIcon(app);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string path = folder + "/display.json";
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { error = "Cannot save app display metadata."; return false; }
    const bool okay = WriteAll(fd, Json::writeString(writer, display));
    const int closed = close(fd);
    if (!okay || closed) { error = "Cannot save app display metadata."; return false; }
  }
  return true;
}

void LoadAppDisplay(const std::string &path, SnapshotApp &app) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return;
  struct stat info{};
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 65536) {
    close(fd); return;
  }
  std::string bytes(info.st_size, '\0');
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += count;
  }
  close(fd);
  if (offset != bytes.size()) return;
  Json::CharReaderBuilder builder;
  Json::Value display;
  std::string error;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &display, &error) ||
      !display.isObject() || display["schema"] != 1 ||
      !display["package"].isString() || display["package"].asString() != app.package) return;
  if (display["name"].isString()) {
    const auto name = display["name"].asString();
    if (!name.empty() && name.size() <= 1024 && name.find('\0') == std::string::npos)
      app.name = name;
  }
  if (display["icon"].isString() && SavedIconEncoding(display["icon"].asString()))
    app.icon_png = display["icon"].asString();
}

void DropSnapshotIcons(State *state) {
  for (const auto &snapshot : state->snapshot_items)
    for (const auto &app : snapshot.apps)
      if (app.icon) lv_image_cache_drop(&app.icon->descriptor);
}

std::string SnapshotDate(const std::string &time) {
  if (time.size() >= 16) {
    std::string date = time.substr(0, 16);
    if (date[10] == 'T') date[10] = ' ';
    return date;
  }
  return time.empty() ? "Unknown date" : time;
}

std::string HumanBytes(uint64_t bytes) {
  static constexpr const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit;
  }
  char text[48];
  snprintf(text, sizeof(text), unit > 1 ? "%.1f %s" : "%.0f %s", value,
           units[unit]);
  return text;
}

void ParseSnapshots(State *state, const std::string &output) {
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(output.data(), output.data() + output.size(), &root,
                     &errors) || !root.isArray()) return;
  std::lock_guard<std::mutex> lock(state->result_mutex);
  state->snapshots = root.size();
  state->latest_id.clear();
  state->latest_time.clear();
  state->snapshot_items.clear();
  for (const auto &snapshot : root) {
    Snapshot item;
    item.id = snapshot["short_id"].asString();
    if (item.id.empty()) item.id = snapshot["id"].asString();
    item.time = snapshot["time"].asString();
    if (snapshot["summary"].isObject())
      item.bytes = snapshot["summary"]["total_bytes_processed"].asUInt64();
    std::map<std::string, std::vector<std::string>> app_paths;
    for (const auto &entry : snapshot["paths"]) {
      const std::string path = entry.asString();
      const std::string package = PackageFromBackupPath(path, state->user_id);
      if (!package.empty()) app_paths[package].push_back(path);
    }
    for (auto &[package, paths] : app_paths) {
      SnapshotApp app;
      app.package = package;
      app.paths = std::move(paths);
      const App *installed = InstalledApp(state, package);
      app.name = installed ? installed->name : FriendlyPackageName(package);
      item.apps.push_back(std::move(app));
    }
    std::sort(item.apps.begin(), item.apps.end(),
        [](const SnapshotApp &left, const SnapshotApp &right) {
          return left.name < right.name;
        });
    state->snapshot_items.push_back(std::move(item));
    const std::string time = snapshot["time"].asString();
    if (time >= state->latest_time) {
      state->latest_time = time;
      state->latest_id = snapshot["short_id"].asString();
      if (state->latest_id.empty()) state->latest_id = snapshot["id"].asString();
    }
  }
  std::sort(state->snapshot_items.begin(), state->snapshot_items.end(),
      [](const Snapshot &left, const Snapshot &right) {
        return left.time > right.time;
      });
}

std::string UserTag(int user_id) {
  return "aera-user-" + std::to_string(user_id);
}

void LoadSnapshotDisplays(State *state) {
  const std::string prefix = std::string(appvault::kMetadataRoot) + "/" +
      std::to_string(state->user_id) + "/";
  for (auto &snapshot : state->snapshot_items) {
    if (state->cancel.load()) return;
    std::vector<std::string> files;
    for (const auto &app : snapshot.apps) {
      const std::string folder = prefix + app.package;
      if (std::find(app.paths.begin(), app.paths.end(), folder) != app.paths.end())
        files.push_back(folder + "/display.json");
    }
    if (files.empty()) continue;
    char staging[] = "/tmp/aera-appvault-display-XXXXXX";
    if (!mkdtemp(staging)) continue;
    std::vector<std::string> arguments = {"restore", snapshot.id,
        "--tag", std::string(kTag) + "," + UserTag(state->user_id),
        "--target", staging, "--exclude-xattr", "security.selinux"};
    for (const auto &file : files) {
      arguments.push_back("--include"); arguments.push_back(file);
    }
    std::string output;
    if (!RunRestic(state, arguments, output)) {
      for (auto &app : snapshot.apps)
        LoadAppDisplay(std::string(staging) + prefix + app.package + "/display.json", app);
      std::sort(snapshot.apps.begin(), snapshot.apps.end(),
          [](const SnapshotApp &left, const SnapshotApp &right) { return left.name < right.name; });
    }
    std::error_code error;
    std::filesystem::remove_all(staging, error);
  }
}

void Worker(State *state, Work work, std::vector<std::string> paths,
            int user_id) {
  state->work = work;
  if (work == Work::kStorage) {
    std::string error;
    const bool okay = RecoveryPrepareBackupStorage(state->pending_storage, &error);
    {
      std::lock_guard<std::mutex> lock(state->result_mutex);
      state->result = okay ? "Ready" : error;
    }
    state->work_success.store(okay);
    state->done.store(true, std::memory_order_release);
    return;
  }
  if (work == Work::kDiscover) {
    auto apps = DiscoverApps(state->cancel, user_id);
    {
      std::lock_guard<std::mutex> lock(state->result_mutex);
      state->apps = std::move(apps);
      state->result = "Apps loaded.";
    }
    state->work_success.store(!state->cancel.load());
    state->work_progress.store(100);
    state->done.store(true, std::memory_order_release);
    return;
  }
  aera::shared_storage::RepairOnExit shared_permissions(state->repository);
  std::string output;
  std::vector<std::string> arguments;
  appvault::RestoreRegistration registration;
  std::string registration_staging;
  struct StagingCleanup {
    std::string &path;
    ~StagingCleanup() {
      if (!path.empty()) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
      }
    }
  } cleanup{registration_staging};
  const auto fail = [&](const std::string &message) {
    fprintf(stderr, "App Backup Vault operation failed: %s\n", message.c_str());
    std::lock_guard<std::mutex> lock(state->result_mutex);
    state->result = message;
    state->work_success.store(false);
    state->work_progress.store(0);
    state->done.store(true, std::memory_order_release);
  };
  if ((!state->storage_root.empty() &&
       !RecoveryPrepareBackupStorage(state->storage_root, &output)) ||
      !RecoveryBackupPathReady(state->repository)) {
    fail("Storage unavailable"); return;
  }
  static std::mutex app_transfer_mutex;
  std::unique_lock<std::mutex> app_transfer_lock(app_transfer_mutex, std::defer_lock);
  if ((work == Work::kBackup || work == Work::kRestore) && !app_transfer_lock.try_lock()) {
    fail("Another app backup or restore is already running.");
    return;
  }
#ifdef OF_ENABLE_WLAN
  if (work == Work::kBackup && state->repository.compare(0, 9, "/mnt/nas/") == 0 &&
      !NasManager::ResetTransferStats()) {
    fail(NasManager::GetLastError());
    return;
  }
#endif
  appvault::IncrementalApkViews incremental;
  if (work == Work::kBackup || work == Work::kRestore) {
    std::vector<std::string> registered;
    if (!appvault::RegisteredApkDirectories(registered, output)) {
      fail(output); return;
    }
    registered.erase(std::remove_if(registered.begin(), registered.end(),
        [&](const std::string &path) {
          return std::find(paths.begin(), paths.end(), path) == paths.end();
        }), registered.end());
    if (!incremental.Prepare(registered, output)) {
      fail(output); return;
    }
    output.clear();
  }
  const std::string metadata_prefix = std::string(appvault::kMetadataRoot) +
      "/" + std::to_string(user_id) + "/";
  if (work == Work::kBackup) {
    std::set<std::string> packages;
    for (const auto &path : paths) {
      const std::string package = PackageFromBackupPath(path, user_id);
      if (!package.empty()) packages.insert(package);
    }
    std::vector<std::string> metadata_paths;
    if (state->remote_progress)
      state->remote_progress({0, "Saving Android app registration", {}, 0, 0, {}});
    if (state->cancel.load() || !appvault::CaptureRegistration(
            user_id, {packages.begin(), packages.end()}, metadata_paths, output)) {
      fail(state->cancel.load() ? "App backup cancelled." : output);
      return;
    }
    if (!SaveAppDisplay(state, metadata_paths, output)) { fail(output); return; }
    output.clear();
    paths.insert(paths.end(), metadata_paths.begin(), metadata_paths.end());
    if (access((state->repository + "/config").c_str(), R_OK) != 0) {
      if (!MakeDirectories(state->repository)) {
        std::lock_guard<std::mutex> lock(state->result_mutex);
        state->result = "Could not create the backup folder.";
        state->work_success.store(false);
        state->done.store(true);
        return;
      }
      if (state->remote_progress) state->remote_progress({0, "Creating app backup repository", {}, 0, 0, {}});
      const int initialize = RunRestic(state, {"init"}, output);
      if (initialize) {
        std::lock_guard<std::mutex> lock(state->result_mutex);
        state->result = LastUsefulLine(output);
        state->work_success.store(false);
        state->done.store(true);
        return;
      }
      output.clear();
      state->work_progress.store(8);
    }
    arguments = {"backup", "--json", "--tag", kTag, "--tag",
                 UserTag(user_id), "--host", "aera-recovery"};
    for (const auto &path : paths) {
      if (path.compare(0, 10, "/data/app/") == 0) {
        arguments.push_back("--exclude");
        arguments.push_back(path + "/oat");
      }
    }
    arguments.insert(arguments.end(), paths.begin(), paths.end());
  } else if (work == Work::kRestore) {
    std::vector<std::string> metadata_paths;
    for (const auto &path : paths)
      if (path.compare(0, metadata_prefix.size(), metadata_prefix) == 0)
        metadata_paths.push_back(path);
    if (metadata_paths.empty()) {
      fail("This backup has no package registration; create a new backup first.");
      return;
    }
    char temporary[] = "/tmp/aera-appvault-restore-XXXXXX";
    if (!mkdtemp(temporary)) { fail("Could not create app restore staging folder."); return; }
    registration_staging = temporary;
    std::vector<std::string> inspect = {"restore", state->restore_snapshot,
        "--tag", std::string(kTag) + "," + UserTag(user_id),
        "--target", registration_staging, "--exclude-xattr", "security.selinux"};
    for (const auto &path : metadata_paths) {
      inspect.push_back("--include"); inspect.push_back(path);
    }
    if (state->remote_progress)
      state->remote_progress({0, "Checking Android app registration", {}, 0, 0, {}});
    if (RunRestic(state, inspect, output)) { fail(LastUsefulLine(output)); return; }
    output.clear();
    if (!registration.Prepare(user_id, paths, registration_staging, output)) {
      fail(output); return;
    }
    output.clear();
    arguments = {"restore", state->restore_snapshot, "--json", "--tag",
                 std::string(kTag) + "," + UserTag(user_id), "--target", "/",
                 "--exclude-xattr", "security.selinux", "--exclude-xattr", "user.incfs.*"};
    bool includes_data = false;
    for (const auto &path : paths) {
      if (path.compare(0, metadata_prefix.size(), metadata_prefix) == 0) continue;
      if (path.compare(0, 10, "/data/app/") == 0 && appvault::IsIncrementalApk(path)) continue;
      arguments.push_back("--include");
      arguments.push_back(path);
      includes_data = true;
    }
    if (!includes_data) {
      fail("No writable app data was selected for restore.");
      return;
    }
  } else {
    if (access((state->repository + "/config").c_str(), R_OK) != 0) {
      std::lock_guard<std::mutex> lock(state->result_mutex);
      state->snapshots = 0;
      state->latest_id.clear();
      state->latest_time.clear();
      state->snapshot_items.clear();
      state->result = "No backups yet.";
      state->work_success.store(true);
      state->work_progress.store(100);
      state->done.store(true, std::memory_order_release);
      return;
    }
    arguments = {"snapshots", "--json", "--tag",
                 std::string(kTag) + "," + UserTag(user_id)};
  }
  int result = RunRestic(state, arguments, output);
#ifdef OF_ENABLE_WLAN
  if (!result && work == Work::kBackup &&
      state->repository.compare(0, 9, "/mnt/nas/") == 0) {
    state->waiting_for_nas.store(true);
    state->work_progress.store(99);
    if (state->remote_progress)
      state->remote_progress({99, "Waiting for NAS upload to finish...", {}, 0, 0, {}});
    if (!NasManager::WaitForPendingUploads(1800, 0, &state->cancel) ||
        !RecoveryBackupPathReady(state->repository)) {
      state->waiting_for_nas.store(false);
      fail("NAS upload did not complete. Check the connection and retry the backup.");
      return;
    }
    state->waiting_for_nas.store(false);
  }
#endif
  if (!result && work == Work::kRestore) {
    if (state->cancel.load()) { result = -1; output = "App restore cancelled."; }
    else {
      state->work_progress.store(99);
      if (state->remote_progress)
        state->remote_progress({99, "Registering restored apps in Android", {}, 0, 0, {}});
      if (!registration.Finish(output)) result = -1;
    }
  }
  if (result) fprintf(stderr, "App Backup Vault: %s\n", output.c_str());
  if (!result && work == Work::kRefresh) {
    ParseSnapshots(state, output);
    LoadSnapshotDisplays(state);
  }
  {
    std::lock_guard<std::mutex> lock(state->result_mutex);
    if (!result) {
      state->result = work == Work::kBackup ? "App backup completed successfully." :
          work == Work::kRestore ? "App restore completed successfully." :
          "Backups refreshed.";
    } else {
      state->result = LastUsefulLine(output);
    }
  }
  state->work_success.store(!result);
  state->work_progress.store(result ? 0 : 100);
  state->done.store(true, std::memory_order_release);
}

void UpdateSelected(State *state) {
  const auto visible = [state](const App &app) {
    return state->show_system || !app.system;
  };
  const size_t total = std::count_if(state->apps.begin(), state->apps.end(), visible);
  const size_t count = std::count_if(state->apps.begin(), state->apps.end(),
      [&](const App &app) { return visible(app) && app.selected; });
  const std::string label =
      i18n::Format("%zu of %zu selected", count, total);
  i18n::BindLabel(state->selected, label.c_str());
  if (state->system_filter_label)
    i18n::BindLabel(state->system_filter_label,
                      state->show_system ? "System apps  On" : "System apps  Off");
  if (state->select_all_label)
    i18n::BindLabel(state->select_all_label,
                      total && count == total ? "Clear all" : "Select all");
  FitCompactButtonText(state->system_filter_label);
  FitCompactButtonText(state->select_all_label);
}

std::string UserName(const State *state, int user_id) {
  const auto found = std::find_if(
      state->users.begin(), state->users.end(),
      [user_id](const AndroidUser &user) { return user.id == user_id; });
  return found == state->users.end()
             ? i18n::Format("Android user %d", user_id)
             : found->name;
}

void UpdateAppSummary(State *state) {
  const size_t user_apps = std::count_if(
      state->apps.begin(), state->apps.end(),
      [](const App &app) { return !app.system; });
  const size_t system_apps = state->apps.size() - user_apps;
  const std::string detail = i18n::Format(
      "%s / %zu user apps / %zu system apps hidden",
      UserName(state, state->user_id).c_str(), user_apps, system_apps);
  i18n::BindLabel(state->detail, detail.c_str());
}

void RenderApps(State *state) {
  lv_obj_clean(state->list);
  int y = 0;
  for (size_t index = 0; index < state->apps.size(); ++index) {
    auto &app = state->apps[index];
    if (app.system && !state->show_system) continue;
    auto *row = lv_button_create(state->list);
    Clear(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, std::max(300, static_cast<int>(lv_obj_get_width(state->list))), 128);
    lv_obj_set_style_radius(row, 24, 0);
    lv_obj_set_style_bg_color(row, kMainSelected, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);
    auto *icon_plate = lv_obj_create(row);
    Panel(icon_plate, 22, kMainPanel);
    lv_obj_set_pos(icon_plate, 18, 26);
    lv_obj_set_size(icon_plate, 76, 76);
    lv_obj_remove_flag(icon_plate, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(icon_plate, LV_OBJ_FLAG_SCROLLABLE);
    if (app.icon) {
      auto *image = lv_image_create(icon_plate);
      lv_image_set_src(image, &app.icon->descriptor);
      lv_image_set_antialias(image, true);
      lv_obj_center(image);
      lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    } else {
      char initial[2] = {'?', 0};
      for (const unsigned char c : app.name) {
        if (std::isalnum(c)) {
          initial[0] = static_cast<char>(std::toupper(c));
          break;
        }
      }
      auto *letter = Label(icon_plate, initial, &lv_font_montserrat_32, kAccent);
      lv_obj_center(letter);
    }
    auto *name = Label(row, app.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 118, 18);
    lv_obj_set_width(name, std::max(200, static_cast<int>(lv_obj_get_width(state->list)) - 210));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    auto *package = Label(row, app.package.c_str(), &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(package, 118, 68);
    lv_obj_set_width(package,
        std::max(200, static_cast<int>(lv_obj_get_width(state->list)) - 300));
    lv_label_set_long_mode(package, LV_LABEL_LONG_DOT);
    if (app.system) {
      auto *badge = Kicker(row, "SYSTEM", kMutedStrong);
      lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -98, 0);
    }
    auto *indicator = lv_obj_create(row);
    Clear(indicator);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(indicator, 58, 58);
    lv_obj_align(indicator, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_radius(indicator, 18, 0);
    lv_obj_set_style_border_width(indicator, 2, 0);
    lv_obj_set_style_border_color(indicator, app.selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_bg_color(indicator, app.selected ? kAccent : kMainPanel, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    auto *mark = Label(indicator, app.selected ? LV_SYMBOL_OK : "",
                       &lv_font_montserrat_24,
                       app.selected ? kOnAccent : kMuted);
    lv_obj_center(mark);
    OnClick(row, [state, index, indicator, mark] {
      auto &selected_app = state->apps[index];
      selected_app.selected = !selected_app.selected;
      lv_obj_set_style_border_color(indicator,
          selected_app.selected ? kAccent : kMainLine, 0);
      lv_obj_set_style_bg_color(indicator,
          selected_app.selected ? kAccent : kMainPanel, 0);
      i18n::BindLabel(mark, selected_app.selected ? LV_SYMBOL_OK : "");
      lv_obj_set_style_text_color(mark,
          selected_app.selected ? kOnAccent : kMuted, 0);
      UpdateSelected(state);
    });
    y += 136;
  }
  const bool any_visible = std::any_of(state->apps.begin(), state->apps.end(),
      [state](const App &app) { return state->show_system || !app.system; });
  if (!any_visible) {
    auto *empty = Label(state->list,
        RecoveryDataLocked() ? "Unlock data to discover installed apps." :
        "No user app data was found.", &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 100);
  }
  UpdateSelected(state);
}

void SetActions(State *state, bool enabled) {
  for (auto *button : {state->backup, state->refresh_button, state->restore,
                       state->storage_button}) {
    if (!button) continue;
    if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
  }
  if (state->user_button) {
    if (enabled && state->users.size() > 1)
      lv_obj_remove_state(state->user_button, LV_STATE_DISABLED);
    else
      lv_obj_add_state(state->user_button, LV_STATE_DISABLED);
  }
}

void ChooseStorage(State *state) {
  if (state->busy.load()) return;
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  MainBackground(overlay);
  auto *title = Label(overlay, "Choose storage", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 80, 180);
  const int height = lv_obj_get_height(state->screen);
  auto *list = lv_obj_create(overlay);
  Clear(list);
  lv_obj_set_pos(list, 64, 300);
  lv_obj_set_size(list, lv_obj_get_width(state->screen) - 128, height - 520);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  int y = 0;
  for (const auto &storage : RecoveryBackupStorages()) {
    Row(list, y, LV_SYMBOL_DRIVE, storage.name, storage.path,
        [state, overlay, storage] {
          if (state->worker.joinable()) state->worker.join();
          state->pending_storage = storage.path;
          state->work = Work::kStorage;
          state->done.store(false);
          state->cancel.store(false);
          state->busy.store(true);
          SetActions(state, false);
          i18n::BindLabel(state->status, "Mounting storage");
          state->worker = std::thread(Worker, state, Work::kStorage,
                                      std::vector<std::string>{}, state->user_id);
          lv_obj_delete_async(overlay);
        });
    y += 190;
  }
  auto *close = Button(overlay, "Cancel", [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_set_size(close, lv_obj_get_width(state->screen) - 128, 112);
  lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -100);
}

void StartDiscovery(State *state) {
  if (!state || state->busy.load()) return;
  if (state->worker.joinable()) state->worker.join();
  lv_obj_clean(state->list);
  for (const auto &app : state->apps)
    if (app.icon) lv_image_cache_drop(&app.icon->descriptor);
  state->apps.clear();
  DropSnapshotIcons(state);
  state->snapshot_items.clear();
  state->snapshots = 0;
  state->latest_id.clear();
  state->latest_time.clear();
  state->work = Work::kDiscover;
  state->done.store(false);
  state->work_success.store(false);
  state->cancel.store(false);
  state->busy.store(true);
  state->work_progress.store(8);
  SetActions(state, false);
  lv_obj_remove_flag(state->progress, LV_OBJ_FLAG_HIDDEN);
  lv_bar_set_value(state->progress, 8, LV_ANIM_OFF);
  const std::string title = i18n::Format(
      "Loading %s apps...", UserName(state, state->user_id).c_str());
  i18n::BindLabel(state->status, title.c_str());
  i18n::BindLabel(state->selected, "Discovering apps...");
  state->worker =
      std::thread(Worker, state, Work::kDiscover,
                  std::vector<std::string>{}, state->user_id);
}

void StartWork(State *state, Work work) {
  if (!state || state->busy.load() || state->runtime.empty()) return;
  std::vector<std::string> paths;
  if (work == Work::kBackup) {
    for (const auto &app : state->apps)
      if (app.selected) paths.insert(paths.end(), app.paths.begin(), app.paths.end());
    if (paths.empty()) {
      Sheet(state->screen, "Nothing selected",
            "Select at least one app before starting the backup.");
      return;
    }
  } else if (work == Work::kRestore) {
    const auto snapshot = std::find_if(
        state->snapshot_items.begin(), state->snapshot_items.end(),
        [state](const Snapshot &item) { return item.id == state->restore_snapshot; });
    if (snapshot == state->snapshot_items.end()) {
      Sheet(state->screen, "Choose a backup",
            "Open Restore apps and select the backup you want to use.");
      return;
    }
    for (const auto &app : snapshot->apps)
      if (app.selected) paths.insert(paths.end(), app.paths.begin(), app.paths.end());
    if (paths.empty()) {
      Sheet(state->screen, "Nothing selected",
            "Select at least one app from this backup before restoring.");
      return;
    }
  }
  if (state->worker.joinable()) state->worker.join();
  state->work = work;
  if (work == Work::kRefresh) DropSnapshotIcons(state);
  state->done.store(false);
  state->work_success.store(false);
  state->cancel.store(false);
  state->busy.store(true);
  state->work_progress.store(work == Work::kBackup || work == Work::kRestore ? 0 : 4);
  state->work_bytes_done.store(0);
  state->work_bytes_total.store(0);
  state->work_files_done.store(0);
  state->work_files_total.store(0);
  SetActions(state, false);
  lv_obj_remove_flag(state->progress, LV_OBJ_FLAG_HIDDEN);
  lv_bar_set_value(state->progress, state->work_progress.load(), LV_ANIM_OFF);
  const char *title = work == Work::kBackup ? "Backing up selected apps…" :
      work == Work::kRestore ? "Restoring latest snapshot…" :
      "Reading backups…";
  i18n::BindLabel(state->status, title);
  i18n::BindLabel(state->detail, "Preparing backup engine…");
  state->worker =
      std::thread(Worker, state, work, std::move(paths), state->user_id);
}

void ShowSnapshotChooser(State *state);

void ShowUserChooser(State *state) {
  if (!state || state->busy.load() || state->users.size() < 2) return;
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  const bool landscape = Landscape(state->screen);
  const int sheet_width = landscape ? 1600 : 1312;
  const int sheet_height =
      std::min(2200, 280 + static_cast<int>(state->users.size()) * 150);
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, LV_ALIGN_CENTER, 0, 40);
  auto *title =
      Label(sheet, "Choose Android user", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 40, 34);
  auto *close = Button(sheet, LV_SYMBOL_CLOSE,
                       [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -36, 30);
  lv_obj_set_size(close, 112, 92);
  auto *list = lv_obj_create(sheet);
  Clear(list);
  lv_obj_set_pos(list, 28, 150);
  lv_obj_set_size(list, sheet_width - 56, sheet_height - 178);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  for (size_t index = 0; index < state->users.size(); ++index) {
    const auto user = state->users[index];
    auto *row = Button(list, "", [state, user, overlay] {
      lv_obj_delete_async(overlay);
      if (state->user_id == user.id) return;
      state->user_id = user.id;
      const std::string label =
          i18n::Format("Android user: %s", user.name.c_str());
      i18n::BindLabel(state->user_label, label.c_str());
      FitCompactButtonText(state->user_label);
      StartDiscovery(state);
    }, user.id == state->user_id);
    lv_obj_set_pos(row, 0, static_cast<int>(index) * 142);
    lv_obj_set_size(row, sheet_width - 80, 126);
    lv_obj_set_style_radius(row, 24, 0);
    auto *name = Label(row, user.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 28, 18);
    lv_obj_set_width(name, sheet_width - 310);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const std::string detail =
        i18n::Format("User %d / Unlocked", user.id);
    auto *status = Label(row, detail.c_str(), &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(status, 30, 72);
    if (user.id == state->user_id) {
      auto *mark =
          Label(row, LV_SYMBOL_OK, &lv_font_montserrat_32, kOnAccent);
      lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -32, 0);
    }
  }
  AnimateEnter(sheet, 0, 28);
}

void UpdateSnapshotCount(const Snapshot &snapshot, lv_obj_t *label) {
  const size_t selected = std::count_if(
      snapshot.apps.begin(), snapshot.apps.end(),
      [](const SnapshotApp &app) { return app.selected; });
  const std::string text = i18n::Format(
      "%zu of %zu apps selected", selected, snapshot.apps.size());
  i18n::BindLabel(label, text.c_str());
}

void ShowSnapshotApps(State *state, size_t snapshot_index) {
  if (!state || snapshot_index >= state->snapshot_items.size()) return;
  auto &snapshot = state->snapshot_items[snapshot_index];
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  const bool landscape = Landscape(state->screen);
  const int sheet_width = landscape ? 2920 : 1312;
  const int sheet_height = landscape ? 1280 : 2520;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, LV_ALIGN_CENTER, 0, landscape ? 70 : 80);

  auto *back = Button(sheet, LV_SYMBOL_LEFT, [state, overlay] {
    lv_obj_delete_async(overlay);
    ShowSnapshotChooser(state);
  });
  lv_obj_set_pos(back, 36, 30);
  lv_obj_set_size(back, 112, 92);
  auto *title = Label(sheet, "Restore apps", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 176, 30);
  auto *subtitle = Label(sheet, SnapshotDate(snapshot.time).c_str(),
                         &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(subtitle, 178, 88);
  auto *selected = Label(sheet, "", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(selected, 38, 150);
  UpdateSnapshotCount(snapshot, selected);
  auto *select_all = Button(sheet, "Select all", [state, snapshot_index, overlay] {
    auto &item = state->snapshot_items[snapshot_index];
    const bool all = !item.apps.empty() && std::all_of(
        item.apps.begin(), item.apps.end(),
        [](const SnapshotApp &app) { return app.selected; });
    for (auto &app : item.apps) app.selected = !all;
    lv_obj_delete_async(overlay);
    ShowSnapshotApps(state, snapshot_index);
  });
  lv_obj_align(select_all, LV_ALIGN_TOP_RIGHT, -36, 32);
  lv_obj_set_size(select_all, 300, 92);

  auto *list = lv_obj_create(sheet);
  Clear(list);
  lv_obj_set_pos(list, 28, 205);
  lv_obj_set_size(list, sheet_width - 56, sheet_height - 390);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, kAccent, LV_PART_SCROLLBAR);
  int y = 0;
  for (size_t index = 0; index < snapshot.apps.size(); ++index) {
    auto &saved = snapshot.apps[index];
    auto *row = lv_button_create(list);
    Clear(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, sheet_width - 80, 128);
    lv_obj_set_style_radius(row, 24, 0);
    lv_obj_set_style_bg_color(row, kMainSelected, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);
    auto *icon_plate = lv_obj_create(row);
    Panel(icon_plate, 22, kMainPanel);
    lv_obj_set_pos(icon_plate, 18, 26);
    lv_obj_set_size(icon_plate, 76, 76);
    lv_obj_remove_flag(icon_plate, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(icon_plate, LV_OBJ_FLAG_SCROLLABLE);
    const App *installed = InstalledApp(state, saved.package);
    if (!saved.icon && !saved.icon_png.empty()) saved.icon = SavedAppIcon(saved.icon_png);
    const auto *icon = saved.icon ? saved.icon.get() : installed ? installed->icon.get() : nullptr;
    if (icon) {
      auto *image = lv_image_create(icon_plate);
      lv_image_set_src(image, &icon->descriptor);
      lv_image_set_antialias(image, true);
      lv_obj_center(image);
      lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    } else {
      char initial[2] = {'?', 0};
      for (const unsigned char c : saved.name) {
        if (std::isalnum(c)) {
          initial[0] = static_cast<char>(std::toupper(c));
          break;
        }
      }
      auto *letter = Label(icon_plate, initial, &lv_font_montserrat_32, kAccent);
      lv_obj_center(letter);
    }
    auto *name = Label(row, saved.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 118, 18);
    lv_obj_set_width(name, sheet_width - 350);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    auto *package = Label(row, saved.package.c_str(),
                          &lv_font_montserrat_20, kMuted);
    lv_obj_set_pos(package, 118, 68);
    lv_obj_set_width(package, sheet_width - 410);
    lv_label_set_long_mode(package, LV_LABEL_LONG_DOT);
    auto *indicator = lv_obj_create(row);
    Clear(indicator);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(indicator, 58, 58);
    lv_obj_align(indicator, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_radius(indicator, 18, 0);
    lv_obj_set_style_border_width(indicator, 2, 0);
    lv_obj_set_style_border_color(indicator, saved.selected ? kAccent : kMainLine, 0);
    lv_obj_set_style_bg_color(indicator, saved.selected ? kAccent : kMainPanel, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    auto *mark = Label(indicator, saved.selected ? LV_SYMBOL_OK : "",
                       &lv_font_montserrat_24,
                       saved.selected ? kOnAccent : kMuted);
    lv_obj_center(mark);
    OnClick(row, [state, snapshot_index, index, indicator, mark, selected] {
      auto &app = state->snapshot_items[snapshot_index].apps[index];
      app.selected = !app.selected;
      lv_obj_set_style_border_color(indicator,
          app.selected ? kAccent : kMainLine, 0);
      lv_obj_set_style_bg_color(indicator,
          app.selected ? kAccent : kMainPanel, 0);
      i18n::BindLabel(mark, app.selected ? LV_SYMBOL_OK : "");
      lv_obj_set_style_text_color(mark, app.selected ? kOnAccent : kMuted, 0);
      UpdateSnapshotCount(state->snapshot_items[snapshot_index], selected);
    });
    y += 136;
  }
  if (snapshot.apps.empty()) {
    auto *empty = Label(list, "This backup does not contain recognizable app data.",
                        &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 120);
  }
  auto *restore = Button(sheet, "Restore selected apps",
      [state, snapshot_index, overlay] {
        auto &item = state->snapshot_items[snapshot_index];
        const size_t count = std::count_if(
            item.apps.begin(), item.apps.end(),
            [](const SnapshotApp &app) { return app.selected; });
        if (!count) return;
        state->restore_snapshot = item.id;
        lv_obj_delete_async(overlay);
        const std::string message = i18n::Format(
            "Restore %zu selected apps from %s?", count,
            SnapshotDate(item.time).c_str());
        Sheet(state->screen, "Restore selected apps?", message.c_str(),
              [state] { StartWork(state, Work::kRestore); });
      }, true);
  lv_obj_set_size(restore, sheet_width - 72, 116);
  lv_obj_align(restore, LV_ALIGN_BOTTOM_MID, 0, -28);
  AnimateEnter(sheet, 0, 28);
}

void ShowSnapshotChooser(State *state) {
  if (!state) return;
  auto *overlay = lv_obj_create(state->screen);
  lv_obj_set_user_data(overlay, &kModalMarker);
  Clear(overlay);
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  const bool landscape = Landscape(state->screen);
  const int sheet_width = landscape ? 2920 : 1312;
  const int sheet_height = landscape ? 1280 : 2200;
  auto *sheet = lv_obj_create(overlay);
  Panel(sheet, 48, kMainSheet);
  lv_obj_set_size(sheet, sheet_width, sheet_height);
  lv_obj_align(sheet, LV_ALIGN_CENTER, 0, landscape ? 70 : 40);
  auto *title = Label(sheet, "Restore apps", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 40, 34);
  auto *hint = Label(sheet, "Choose a backup to see exactly which apps it contains.",
                     &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(hint, 42, 98);
  auto *close = Button(sheet, LV_SYMBOL_CLOSE,
                       [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -36, 30);
  lv_obj_set_size(close, 112, 92);
  auto *list = lv_obj_create(sheet);
  Clear(list);
  lv_obj_set_pos(list, 28, 170);
  lv_obj_set_size(list, sheet_width - 56, sheet_height - 210);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  int y = 0;
  for (size_t index = 0; index < state->snapshot_items.size(); ++index) {
    const auto &snapshot = state->snapshot_items[index];
    auto *card = Button(list, "", [state, index, overlay] {
      lv_obj_delete_async(overlay);
      ShowSnapshotApps(state, index);
    });
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, sheet_width - 80, 174);
    lv_obj_set_style_radius(card, 30, 0);
    auto *date = Label(card, SnapshotDate(snapshot.time).c_str(),
                       &lv_font_montserrat_36, kText);
    lv_obj_set_pos(date, 30, 24);
    std::string detail =
        i18n::Format("%zu apps", snapshot.apps.size());
    if (snapshot.bytes) detail += " • " + HumanBytes(snapshot.bytes);
    auto *details = Label(card, detail.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(details, 32, 86);
    auto *arrow = Label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, kAccent);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -34, 0);
    y += 190;
  }
  if (state->snapshot_items.empty()) {
    auto *empty = Label(list, "No app backups found.",
                        &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 140);
  }
  AnimateEnter(sheet, 0, 28);
}

void Poll(State *state) {
  if (!state) return;
  if (state->preparation.done.load(std::memory_order_acquire) &&
      state->metadata_done.load(std::memory_order_acquire) &&
      !state->preparation_handled.exchange(true)) {
    if (state->prepare_thread.joinable()) state->prepare_thread.join();
    if (state->preparation.verified) {
      state->runtime = state->preparation.directory;
      i18n::BindLabel(state->status, "Ready");
      const size_t user_apps = std::count_if(state->apps.begin(), state->apps.end(),
          [](const App &app) { return !app.system; });
      const size_t system_apps = state->apps.size() - user_apps;
      const std::string detail = i18n::Format(
          "%zu user apps • %zu system apps hidden", user_apps, system_apps);
      i18n::BindLabel(state->detail, detail.c_str());
      UpdateAppSummary(state);
      RenderApps(state);
      SetActions(state, !RecoveryDataLocked());
    } else {
      i18n::BindLabel(state->status, "Engine unavailable");
      i18n::BindLabel(state->detail, state->preparation.error.c_str());
      SetActions(state, false);
    }
  }
  if (state->busy.load()) {
    const unsigned progress = state->work_progress.load();
    lv_bar_set_value(state->progress, progress, LV_ANIM_ON);
    if (state->work == Work::kBackup || state->work == Work::kRestore) {
      const char *base = state->waiting_for_nas.load() ? "Waiting for NAS upload to finish..." : state->work == Work::kBackup
          ? "Backing up selected apps…" : "Restoring latest snapshot…";
      const std::string status = i18n::Format(
          "%s %u%%", i18n::Translate(base), progress);
      i18n::BindLabel(state->status, status.c_str());
      const uint64_t bytes_done = state->work_bytes_done.load();
      const uint64_t bytes_total = state->work_bytes_total.load();
      const uint64_t files_done = state->work_files_done.load();
      const uint64_t files_total = state->work_files_total.load();
      std::string detail;
      if (bytes_total)
        detail = HumanBytes(bytes_done) + " / " + HumanBytes(bytes_total);
      if (files_total) {
        if (!detail.empty()) detail += "  •  ";
        detail += std::to_string(files_done) + " / " +
                  std::to_string(files_total);
      }
      if (!detail.empty()) i18n::BindLabel(state->detail, detail.c_str());
    }
  }
  if (state->busy.load() && state->done.exchange(false)) {
    if (state->worker.joinable()) state->worker.join();
    std::string result;
    {
      std::lock_guard<std::mutex> lock(state->result_mutex);
      result = state->result;
    }
    state->busy.store(false);
    SetActions(state, true);
    if (state->work == Work::kStorage && state->work_success.load()) {
      state->storage_root = state->pending_storage;
      state->repository = state->storage_root + "/AERA/AppBackupVault";
      lv_label_set_text(state->storage_label, state->repository.c_str());
      DropSnapshotIcons(state);
      state->snapshot_items.clear();
      state->snapshots = 0;
      state->latest_id.clear();
      state->latest_time.clear();
      state->restore_snapshot.clear();
      RecoveryRememberAppVaultStorage(state->storage_root);
    }
    lv_bar_set_value(state->progress, state->work_progress.load(), LV_ANIM_ON);
    if (state->work_success.load()) {
      i18n::BindLabel(state->status, result.c_str());
    } else {
      const char *failure = state->work == Work::kStorage ? "Storage unavailable" :
          state->work == Work::kBackup ? "App backup failed" :
          state->work == Work::kRestore ? "App restore failed" :
          state->work == Work::kDiscover ? "Could not read user apps" :
          "Could not read snapshots";
      i18n::BindLabel(state->status, failure);
      i18n::BindLabel(state->detail, result.c_str());
    }
    if (state->work_success.load() && state->work == Work::kDiscover) {
      i18n::BindLabel(state->status, "Ready");
      UpdateAppSummary(state);
      RenderApps(state);
      lv_obj_add_flag(state->progress, LV_OBJ_FLAG_HIDDEN);
    }
    if (state->work_success.load() && state->work == Work::kRefresh) {
      std::string detail =
          i18n::Format("%u app backups", state->snapshots);
      if (!state->latest_time.empty())
        detail = i18n::Format("%s • latest %s", detail.c_str(),
                              SnapshotDate(state->latest_time).c_str());
      i18n::BindLabel(state->detail, detail.c_str());
      if (state->browse_after_refresh) ShowSnapshotChooser(state);
    }
    state->browse_after_refresh = false;
  }
}

void Destroy(State *state) {
  if (!state) return;
  if (state->timer) lv_timer_delete(state->timer);
  state->preparation.cancel.store(true);
  state->cancel.store(true);
  const pid_t child = state->child.load();
  if (child > 0) kill(child, SIGTERM);
  if (state->prepare_thread.joinable()) state->prepare_thread.join();
  if (state->worker.joinable()) state->worker.join();
  if (state->list) lv_obj_clean(state->list);
  for (const auto &app : state->apps)
    if (app.icon) lv_image_cache_drop(&app.icon->descriptor);
  DropSnapshotIcons(state);
  web::RemoveRuntime(state->runtime.empty() ? state->preparation.directory : state->runtime);
  delete state;
}

}  // namespace

bool AppVaultInstalled() {
  return web::PluginRuntimeInstalled(kPluginId, kPluginType, kPluginEntry);
}

bool RunAppVaultRemote(const JobRequest &request, Json::Value &result,
                      const std::function<void(const AppVaultProgress &)> &progress) {
  State state;
  state.user_id = request.backup_user; state.repository = request.path;
  if (request.backup_source == "device") state.storage_root = request.backup_storage;
  state.remote_progress = progress;
  const auto users = RecoveryAndroidUsers();
  if (RecoveryDataLocked() || std::none_of(users.begin(), users.end(), [&](const AndroidUser &user) {
        return user.id == state.user_id && user.decrypted;
      }) || !AppVaultInstalled() || !ValidRepository(state.repository)) {
    result["error"] = "Install App Backup Vault and unlock the selected Android user first.";
    return false;
  }
  if (request.backup_action == "apps") {
    progress({0, "Preparing your backup", {}, 0, 0, {}});
    for (const auto &app : DiscoverApps(state.cancel, state.user_id)) {
      Json::Value item; item["package"] = app.package; item["name"] = app.name;
      item["icon"] = RemoteAppIcon(&app);
      item["system"] = app.system; result["apps"].append(item);
    }
    return true;
  }
  progress({0, "Preparing backup engine\u2026", {}, 0, 0, {}});
  struct Cleanup {
    State &state;
    ~Cleanup() { web::RemoveRuntime(state.runtime.empty() ? state.preparation.directory : state.runtime); }
  } cleanup{state};
  web::PreparePluginRuntime(state.preparation, kPluginId, kPluginType, kPluginEntry);
  if (!state.preparation.verified) { result["error"] = state.preparation.error; return false; }
  state.runtime = state.preparation.directory;
  if (request.backup_action == "snapshots")
    state.apps = DiscoverApps(state.cancel, state.user_id);
  progress({0, "Reading app backups", {}, 0, 0, {}});
  Worker(&state, Work::kRefresh, {}, state.user_id);
  if (!state.work_success.load()) { result["error"] = state.result; return false; }
  if (request.backup_action == "snapshots") {
    for (const auto &snapshot : state.snapshot_items) {
      Json::Value item; item["id"] = snapshot.id; item["time"] = snapshot.time;
      item["bytes"] = Json::UInt64(snapshot.bytes);
      for (const auto &app : snapshot.apps) {
        Json::Value value; value["package"] = app.package; value["name"] = app.name;
        value["icon"] = !app.icon_png.empty() ? app.icon_png :
            RemoteAppIcon(InstalledApp(&state, app.package));
        item["apps"].append(value);
      }
      result["snapshots"].append(item);
    }
    return true;
  }
  std::vector<std::string> paths;
  std::set<std::string> selected(request.partitions.begin(), request.partitions.end()), found;
  if (request.backup_action == "backup") {
    for (const auto &app : DiscoverApps(state.cancel, state.user_id, false)) {
      if (!selected.count(app.package)) continue;
      found.insert(app.package); paths.insert(paths.end(), app.paths.begin(), app.paths.end());
    }
  } else if (request.backup_action == "restore") {
    const auto snapshot = std::find_if(state.snapshot_items.begin(), state.snapshot_items.end(),
        [&](const Snapshot &item) { return item.id == request.backup_snapshot; });
    if (snapshot == state.snapshot_items.end()) { result["error"] = "Select a snapshot for this Android user."; return false; }
    state.restore_snapshot = snapshot->id;
    for (const auto &app : snapshot->apps) {
      if (!selected.count(app.package)) continue;
      found.insert(app.package); paths.insert(paths.end(), app.paths.begin(), app.paths.end());
    }
  } else { result["error"] = "Unknown app backup operation."; return false; }
  if (paths.empty() || found != selected) { result["error"] = "Select available apps for this Android user."; return false; }
  progress({0, request.backup_action == "backup" ? "Backing up selected apps\u2026" : "Restoring apps", {}, 0, 0, {}});
  state.work_progress.store(0);
  state.work_bytes_done.store(0); state.work_bytes_total.store(0);
  state.work_files_done.store(0); state.work_files_total.store(0);
  Worker(&state, request.backup_action == "backup" ? Work::kBackup : Work::kRestore,
         paths, state.user_id);
  result["detail"] = state.result;
  if (!state.work_success.load()) result["error"] = state.result;
  return state.work_success.load();
}

void BuildAppVaultScene(lv_obj_t *screen, ActionCallback callback,
                        void *context) {
  Header(screen, "App Backup",
         "Per-app backups stored in AERA storage.",
         callback, context);
  auto *state = new State;
  state->screen = screen;
  state->callback = callback;
  state->context = context;
  state->storage_root = RecoveryAppVaultStorage();
  state->repository = state->storage_root + "/AERA/AppBackupVault";
  for (const auto &user : RecoveryAndroidUsers())
    if (user.decrypted) state->users.push_back(user);
  const auto owner = std::find_if(
      state->users.begin(), state->users.end(),
      [](const AndroidUser &user) { return user.id == 0; });
  if (owner != state->users.end())
    state->user_id = owner->id;
  else if (!state->users.empty())
    state->user_id = state->users.front().id;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    Destroy(static_cast<State *>(lv_event_get_user_data(event)));
  }, LV_EVENT_DELETE, state);

  const bool landscape = Landscape(screen);
  const int actions_height = landscape ? 118 : 132;
  const int actions_y = lv_obj_get_height(screen) - actions_height - 34;
  const int apps_y = landscape ? 320 : 1080;
  const int apps_height = actions_y - apps_y - 24;
  auto *hero = lv_obj_create(screen);
  Panel(hero, 46, kMainSheet);
  lv_obj_set_pos(hero, 64, landscape ? 320 : 450);
  lv_obj_set_size(hero, landscape ? 900 : 1312, landscape ? 780 : 600);
  auto *plate = IconPlate(hero, LV_SYMBOL_SAVE, kCyan, kMainPanel, 116);
  lv_obj_set_pos(plate, 38, 38);
  state->status = Label(hero, "Preparing backup engine…",
                        &lv_font_montserrat_48, kText);
  lv_obj_set_pos(state->status, 184, 44);
  lv_obj_set_width(state->status, landscape ? 650 : 1040);
  lv_label_set_long_mode(state->status, LV_LABEL_LONG_DOT);
  state->detail = Label(hero, "Loading installed apps.",
                        &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->detail, 184, 104);
  lv_obj_set_width(state->detail, landscape ? 650 : 1040);
  lv_label_set_long_mode(state->detail, LV_LABEL_LONG_WRAP);

  auto *location_title =
      Label(hero, "Backup location", &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(location_title, 40, 218);
  auto *location =
      Label(hero, state->repository.c_str(), &lv_font_montserrat_32, kText);
  state->storage_label = location;
  lv_obj_set_pos(location, 40, 266);
  lv_obj_set_width(location, landscape ? 650 : 1030);
  lv_label_set_long_mode(location, LV_LABEL_LONG_DOT);
  state->storage_button = Button(hero, LV_SYMBOL_DIRECTORY,
                                [state] { ChooseStorage(state); });
  lv_obj_set_pos(state->storage_button, landscape ? 720 : 1100, 222);
  lv_obj_set_size(state->storage_button, 132, 110);
  auto *protection =
      Label(hero, "Encrypted recovery snapshots", &lv_font_montserrat_24, kGreen);
  lv_obj_set_pos(protection, 40, 334);

  state->progress = lv_bar_create(hero);
  lv_obj_set_pos(state->progress, 38, landscape ? 430 : 430);
  lv_obj_set_size(state->progress, landscape ? 824 : 1236, 12);
  lv_bar_set_range(state->progress, 0, 100);
  lv_obj_set_style_bg_color(state->progress, kMainPanel, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->progress, kAccent, LV_PART_INDICATOR);
  lv_obj_add_flag(state->progress, LV_OBJ_FLAG_HIDDEN);

  auto *apps_panel = lv_obj_create(screen);
  Panel(apps_panel, 46, kMainSheet);
  lv_obj_set_pos(apps_panel, landscape ? 994 : 64, apps_y);
  lv_obj_set_size(apps_panel, landscape ? 2110 : 1312,
                  apps_height);
  auto *apps_title = Label(apps_panel, "Apps", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(apps_title, 34, 24);
  state->selected = Label(apps_panel, "Discovering apps…",
                          &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->selected, 36, 82);
  const int filter_width = landscape ? 410 : 330;
  auto *select_all = Button(apps_panel, "", [state] {
    const bool all_selected = std::all_of(state->apps.begin(), state->apps.end(),
        [state](const App &app) {
          return (!state->show_system && app.system) || app.selected;
        });
    for (auto &app : state->apps)
      if (state->show_system || !app.system) app.selected = !all_selected;
    RenderApps(state);
  });
  lv_obj_set_size(select_all, filter_width, 82);
  lv_obj_align(select_all, LV_ALIGN_TOP_RIGHT, -(filter_width + 44), 24);
  state->select_all_label = lv_obj_get_child(select_all, 0);
  lv_obj_set_style_text_font(state->select_all_label,
                             UiFont(&lv_font_montserrat_24), 0);
  auto *system_filter = Button(apps_panel, "", [state] {
    state->show_system = !state->show_system;
    RenderApps(state);
  });
  lv_obj_set_size(system_filter, filter_width, 82);
  lv_obj_align(system_filter, LV_ALIGN_TOP_RIGHT, -24, 24);
  state->system_filter_label = lv_obj_get_child(system_filter, 0);
  lv_obj_set_style_text_font(state->system_filter_label,
                             UiFont(&lv_font_montserrat_24), 0);
  state->user_button = Button(apps_panel, "", [state] {
    ShowUserChooser(state);
  });
  lv_obj_set_pos(state->user_button, 24, 130);
  lv_obj_set_size(state->user_button, landscape ? 680 : 600, 82);
  lv_obj_set_style_radius(state->user_button, 24, 0);
  state->user_label = lv_obj_get_child(state->user_button, 0);
  const std::string user_label = i18n::Format(
      "Android user: %s", UserName(state, state->user_id).c_str());
  i18n::BindLabel(state->user_label, user_label.c_str());
  FitCompactButtonText(state->user_label);
  state->list = lv_obj_create(apps_panel);
  Clear(state->list);
  lv_obj_set_pos(state->list, 24, 226);
  lv_obj_set_size(state->list, landscape ? 2062 : 1264,
                  apps_height - 266);
  lv_obj_add_flag(state->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(state->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(state->list, kAccent, LV_PART_SCROLLBAR);

  auto *actions = lv_obj_create(screen);
  Clear(actions);
  lv_obj_set_pos(actions, 64, actions_y);
  lv_obj_set_size(actions, landscape ? 3040 : 1312, actions_height);
  const int gap = 20;
  const int columns = 3;
  const int width = ((landscape ? 3040 : 1312) - gap * (columns - 1)) / columns;
  auto add = [&](lv_obj_t **target, const char *text, int index,
                 Handler action, bool primary = false) {
    *target = Button(actions, text, std::move(action), primary);
    const int row = index / columns;
    const int column = index % columns;
    lv_obj_set_pos(*target, column * (width + gap), row * 146);
    lv_obj_set_size(*target, width, 116);
    auto *label = lv_obj_get_child(*target, 0);
    FitLabelToLines(label, width - 48, 2,
                    {&lv_font_montserrat_32, &lv_font_montserrat_28,
                     &lv_font_montserrat_24, &lv_font_montserrat_20,
                     &lv_font_montserrat_18, &lv_font_montserrat_16});
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
  };
  add(&state->backup, "Back up selected", 0,
      [state] { StartWork(state, Work::kBackup); }, true);
  add(&state->refresh_button, "Refresh backups", 1, [state] {
    state->browse_after_refresh = false;
    StartWork(state, Work::kRefresh);
  });
  add(&state->restore, "Restore apps", 2, [state] {
    state->browse_after_refresh = true;
    StartWork(state, Work::kRefresh);
  });
  SetActions(state, false);
  AnimateEnter(hero, 20, 12);
  AnimateEnter(apps_panel, 50, 12);
  Navigation(screen, Action::kBackHome, callback, context, true);

  state->prepare_thread = std::thread([state] {
    web::PreparePluginRuntime(state->preparation, kPluginId, kPluginType,
                              kPluginEntry);
    if (state->preparation.verified)
      state->apps = DiscoverApps(state->preparation.cancel, state->user_id);
    state->metadata_done.store(true, std::memory_order_release);
  });
  state->timer = lv_timer_create([](lv_timer_t *timer) {
    Poll(static_cast<State *>(lv_timer_get_user_data(timer)));
  }, 100, state);
}

}  // namespace aeraui
