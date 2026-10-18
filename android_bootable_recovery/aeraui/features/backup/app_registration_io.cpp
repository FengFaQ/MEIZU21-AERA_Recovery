/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "android_packages_xml.hpp"
#include "incremental_apks.hpp"
#include "app_registration.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <selinux/selinux.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>

namespace aeraui::appvault {
namespace {
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

std::string Attr(const XMLElement* node, const char* name) {
  const char* value = node ? node->Attribute(name) : nullptr;
  return value ? value : "";
}

bool Fail(std::string& error, const std::string& message, const std::string& path = {}) {
  error = message + (path.empty() ? "" : ": " + path);
  return false;
}

bool Read(const std::string& path, std::string& bytes, std::string& error) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat st{};
  if (fd < 0) return Fail(error, "Cannot read app registration", path);
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (32 << 20)) {
    close(fd);
    return Fail(error, "Invalid app registration file", path);
  }
  bytes.clear();
  char buffer[16384];
  ssize_t count;
  while ((count = read(fd, buffer, sizeof(buffer))) != 0) {
    if (count < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return Fail(error, "Cannot read app registration", path);
    }
    bytes.append(buffer, count);
    if (bytes.size() > (32U << 20)) {
      close(fd);
      return Fail(error, "App registration is too large");
    }
  }
  close(fd);
  return true;
}

bool Load(const std::string& path, XMLDocument& document, std::string& error) {
  std::string bytes;
  return Read(path, bytes, error) && ParsePackagesXml(bytes, document, error);
}

std::string Xml(const XMLDocument& document) {
  tinyxml2::XMLPrinter printer(nullptr, true);
  document.Print(&printer);
  return std::string(printer.CStr(), printer.CStrSize() - 1);
}

std::string Context(const std::string& path) {
  char* context = nullptr;
  if (lgetfilecon(path.c_str(), &context) < 0) return {};
  const std::string value = context;
  freecon(context);
  return value;
}

bool PrivateDirectory(const std::string& path, std::string& error) {
  if (mkdir(path.c_str(), 0700) && errno != EEXIST)
    return Fail(error, "Cannot create private app metadata folder", path);
  struct stat st{};
  return !lstat(path.c_str(), &st) && S_ISDIR(st.st_mode) && st.st_uid == 0 &&
                 (st.st_mode & 0777) == 0700
             ? true
             : Fail(error, "Unsafe app metadata folder", path);
}

bool Write(const std::string& path, const std::string& bytes, mode_t mode, uid_t uid, gid_t gid,
           const std::string& context, std::string& error) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
  if (fd < 0) return Fail(error, "Cannot stage app registration", path);
  size_t offset = 0;
  bool okay = true;
  while (offset < bytes.size()) {
    const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      okay = false;
      break;
    }
    offset += count;
  }
  if (okay)
    okay = fchown(fd, uid, gid) == 0 && fchmod(fd, mode) == 0 &&
           (context.empty() || fsetfilecon(fd, context.c_str()) == 0) && fsync(fd) == 0;
  close(fd);
  if (!okay) {
    unlink(path.c_str());
    return Fail(error, "Cannot save app registration", path);
  }
  return true;
}

int Sdk(const XMLDocument& database) {
  for (auto* node = database.RootElement() ? database.RootElement()->FirstChildElement("version")
                                           : nullptr;
       node; node = node->NextSiblingElement("version")) {
    if (Attr(node, "volumeUuid").empty()) return node->IntAttribute("sdkVersion", 0);
  }
  return 0;
}

bool PackageDatabase(XMLDocument& database, std::string& error) {
  if (access("/data/system/packages-backup.xml", F_OK) == 0)
    return Fail(error, "Android has an unfinished package transaction; boot Android first");
  if (!Load("/data/system/packages.xml", database, error)) return false;
  if (Sdk(database) < 23)
    return Fail(error, "Android package database is not initialized; boot Android first");
  return true;
}

bool Users(std::map<int, std::unique_ptr<XMLDocument>>& users, std::string& error) {
  DIR* directory = opendir("/data/system/users");
  if (!directory) return Fail(error, "Cannot read Android users");
  bool okay = true;
  while (const auto* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
    char* end = nullptr;
    const long id = strtol(name.c_str(), &end, 10);
    if (!end || *end || id < 0 || id > 21474) {
      okay = false;
      break;
    }
    const std::string path = "/data/system/users/" + name + "/package-restrictions.xml";
    const std::string backup = "/data/system/users/" + name + "/package-restrictions-backup.xml";
    if (access(backup.c_str(), F_OK) == 0) {
      okay = Fail(error, "Android has unfinished user package state; boot Android first");
      break;
    }
    auto document = std::make_unique<XMLDocument>();
    if (!Load(path, *document, error)) {
      okay = false;
      break;
    }
    users.emplace(static_cast<int>(id), std::move(document));
  }
  closedir(directory);
  return okay && !users.empty();
}

bool Digest(const std::string& path, std::string& digest, std::string& error) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st{};
  if (fd < 0) return Fail(error, "Cannot verify restored APK", path);
  if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    close(fd);
    return Fail(error, "Invalid APK", path);
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool okay = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
  char buffer[65536];
  while (okay) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) okay = false;
    if (count <= 0) break;
    okay = EVP_DigestUpdate(ctx, buffer, count) == 1;
  }
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  if (okay) okay = EVP_DigestFinal_ex(ctx, hash, &size) == 1;
  EVP_MD_CTX_free(ctx);
  close(fd);
  if (!okay) return Fail(error, "Cannot hash restored APK", path);
  static constexpr char hex[] = "0123456789abcdef";
  digest.clear();
  for (unsigned i = 0; i < size; ++i) {
    digest += hex[hash[i] >> 4];
    digest += hex[hash[i] & 15];
  }
  return true;
}

std::string Categories(unsigned app_id, int user) {
  const unsigned app = app_id - 10000;
  return ":c" + std::to_string(app % 256) + ",c" + std::to_string(256 + app / 256) + ",c" +
         std::to_string(512 + user % 256) + ",c" + std::to_string(768 + user / 256);
}

bool DataContext(const std::string& saved, unsigned old_id, unsigned new_id, int user,
                 std::string& context) {
  if (saved.empty()) {
    context.clear();
    return true;
  }
  if (old_id == new_id && saved.compare(0, 11, "u:object_r:") == 0 &&
      saved.find(":s0") != std::string::npos &&
      saved.find_first_of(" \t\r\n") == std::string::npos) {
    context = saved;
    return true;
  }
  const std::string base = "u:object_r:app_data_file:s0";
  if (saved == base) {
    context = saved;
    return true;
  }
  if (saved != base + Categories(old_id, user)) return false;
  context = base + Categories(new_id, user);
  return true;
}

bool LabelTree(const std::string& path, uid_t old_uid, uid_t new_uid, gid_t old_cache,
               gid_t new_cache, const std::string& context, bool apk, std::string& error) {
  struct stat st{};
  if (lstat(path.c_str(), &st))
    return errno == ENOENT ? true : Fail(error, "Cannot inspect restored app data", path);
  const uid_t uid = apk ? 1000 : st.st_uid == old_uid ? new_uid : st.st_uid;
  const gid_t gid = apk                      ? 1000
                    : st.st_gid == old_cache ? new_cache
                    : st.st_gid == old_uid   ? new_uid
                                             : st.st_gid;
  if (lchown(path.c_str(), uid, gid) ||
      (!context.empty() && lsetfilecon(path.c_str(), context.c_str())))
    return Fail(error, "Cannot repair restored app ownership or SELinux context", path);
  // chown may clear setgid on cache directories; preserve backed-up access modes.
  if (!S_ISLNK(st.st_mode) && chmod(path.c_str(), st.st_mode & 07777))
    return Fail(error, "Cannot repair restored app access modes", path);
  if (!S_ISDIR(st.st_mode)) return true;
  DIR* directory = opendir(path.c_str());
  if (!directory) return Fail(error, "Cannot read restored app directory", path);
  bool okay = true;
  while (const auto* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (apk && name == "oat") continue;
    if (!LabelTree(path + "/" + name, old_uid, new_uid, old_cache, new_cache, context, apk,
                   error)) {
      okay = false;
      break;
    }
  }
  closedir(directory);
  return okay;
}

struct DatabaseFile {
  std::string path, original, replacement, context;
  mode_t mode = 0600;
  uid_t uid = 1000;
  gid_t gid = 1000;
};

bool PrepareFile(const std::string& path, const XMLDocument& document, DatabaseFile& file,
                 std::string& error) {
  file.path = path;
  struct stat st{};
  if (lstat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_uid != 1000)
    return Fail(error, "Unsafe Android package database", path);
  if (!Read(path, file.original, error)) return false;
  file.mode = st.st_mode & 0777;
  file.uid = st.st_uid;
  file.gid = st.st_gid;
  file.context = Context(path);
  file.replacement = Xml(document);
  return !file.context.empty() || Fail(error, "Cannot read package database SELinux context", path);
}

bool SyncParent(const std::string& path) {
  const int fd =
      open(path.substr(0, path.find_last_of('/')).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool okay = fsync(fd) == 0;
  close(fd);
  return okay;
}

bool Commit(std::vector<DatabaseFile>& files, std::string& error) {
  // Stage every main file and its Android reserve copy before changing anything.
  // User restrictions are committed first; the global package registry is last.
  std::vector<std::pair<DatabaseFile*, std::string>> staged;
  for (auto& file : files) {
    std::string current;
    if (!Read(file.path, current, error) || current != file.original)
      return Fail(error, "Android package database changed during restore; retry");
  }
  for (auto& file : files) {
    for (const auto& target : { file.path + ".reservecopy", file.path }) {
      const std::string temporary = target + ".aera-new";
      if (!Write(temporary, file.replacement, file.mode, file.uid, file.gid, file.context, error)) {
        for (const auto& entry : staged) unlink((entry.second + ".aera-new").c_str());
        return false;
      }
      staged.emplace_back(&file, target);
    }
  }
  std::vector<std::pair<DatabaseFile*, std::string>> committed;
  for (const auto& entry : staged) {
    if (rename((entry.second + ".aera-new").c_str(), entry.second.c_str()) ||
        !SyncParent(entry.second)) {
      bool rollback = true;
      // Reserve copies receive the old valid XML too, rather than remaining stale.
      committed.push_back(entry);
      for (const auto& old : committed) {
        const std::string temporary = old.second + ".aera-rollback";
        std::string ignored;
        rollback = Write(temporary, old.first->original, old.first->mode, old.first->uid,
                         old.first->gid, old.first->context, ignored) &&
                   rename(temporary.c_str(), old.second.c_str()) == 0 && SyncParent(old.second) &&
                   rollback;
      }
      for (const auto& pending : staged) unlink((pending.second + ".aera-new").c_str());
      return Fail(
          error,
          rollback
              ? "Could not commit app registration; original database restored"
              : "App registration rollback failed; do not boot Android and keep the recovery log");
    }
    committed.push_back(entry);
  }
  return true;
}
}  // namespace

bool CaptureRegistration(int user, const std::vector<std::string>& packages,
                         std::vector<std::string>& metadata_paths, std::string& error) {
  XMLDocument database, restrictions;
  if (!PackageDatabase(database, error) ||
      !Load("/data/system/users/" + std::to_string(user) + "/package-restrictions.xml",
            restrictions, error) ||
      !PrivateDirectory(kMetadataRoot, error) ||
      !PrivateDirectory(std::string(kMetadataRoot) + "/" + std::to_string(user), error))
    return false;
  for (const auto& name : packages) {
    XMLDocument metadata;
    if (!ExportPackage(database, restrictions, name, user, Sdk(database), metadata, error))
      return false;
    auto* root = metadata.RootElement();
    const std::string code = Attr(root->FirstChildElement("package"), "codePath");
    const auto* package = root->FirstChildElement("package");
    const unsigned id = package->Attribute("sharedUserId")
                            ? package->UnsignedAttribute("sharedUserId")
                            : package->UnsignedAttribute("userId");
    for (const char* type : { "user", "user_de" }) {
      const std::string path =
          "/data/" + std::string(type) + "/" + std::to_string(user) + "/" + name;
      const std::string context = Context(path);
      std::string checked;
      struct stat st{};
      if (!lstat(path.c_str(), &st) &&
          (context.empty() || !DataContext(context, id, id, user, checked)))
        return Fail(error, "Unsupported app data security context", path);
      root->SetAttribute(std::string(type) == "user" ? "ce-context" : "de-context",
                         context.c_str());
    }
    for (const char* type : { "data", "obb", "media" }) {
      const std::string path =
          "/data/media/" + std::to_string(user) + "/Android/" + type + "/" + name;
      const std::string context = Context(path);
      struct stat st{};
      if (!lstat(path.c_str(), &st) && context.empty())
        return Fail(error, "Cannot read external app data context", path);
      root->SetAttribute((std::string("external-") + type).c_str(), context.c_str());
    }
    const bool has_apk = code.compare(0, 10, "/data/app/") == 0;
    DIR* directory = has_apk ? opendir(code.c_str()) : nullptr;
    if (has_apk && !directory) return Fail(error, "Cannot read installed APK folder", code);
    bool okay = true, base = !has_apk;
    while (directory) {
      const auto* entry = readdir(directory);
      if (!entry) break;
      const std::string file = entry->d_name;
      if (file.size() <= 4 || file.substr(file.size() - 4) != ".apk") continue;
      std::string hash;
      if (!Digest(code + "/" + file, hash, error)) {
        okay = false;
        break;
      }
      auto* apk = metadata.NewElement("apk");
      apk->SetAttribute("name", file.c_str());
      apk->SetAttribute("sha256", hash.c_str());
      root->InsertEndChild(apk);
      base |= file == "base.apk";
    }
    if (directory) closedir(directory);
    if (!okay || !base) return Fail(error, "Could not capture installed APKs", code);
    const std::string folder = std::string(kMetadataRoot) + "/" + std::to_string(user) + "/" + name;
    if (!PrivateDirectory(folder, error)) return false;
    const std::string path = folder + "/registration.xml";
    const std::string temporary = path + ".new";
    if (!Write(temporary, Xml(metadata), 0600, 0, 0, {}, error)) return false;
    if (rename(temporary.c_str(), path.c_str())) {
      unlink(temporary.c_str());
      return Fail(error, "Cannot save app metadata", path);
    }
    metadata_paths.push_back(folder);
  }
  return true;
}

bool RegisteredApkDirectories(std::vector<std::string>& paths, std::string& error) {
  XMLDocument database;
  paths.clear();
  if (!Load("/data/system/packages.xml", database, error)) return false;
  if (!database.RootElement() || std::string(database.RootElement()->Name()) != "packages")
    return Fail(error, "Invalid Android package database");
  for (auto* node = database.RootElement()->FirstChildElement("package"); node;
       node = node->NextSiblingElement("package")) {
    const std::string path = Attr(node, "codePath");
    if (path.compare(0, 10, "/data/app/") == 0) paths.push_back(path);
  }
  return true;
}

struct RestoreRegistration::Impl {
  struct App {
    std::string name, code, ce_context, de_context;
    unsigned old_id = 0, new_id = 0;
    bool data_only = false;
    bool has_apk = false;
    std::map<std::string, std::string> apks;
    std::map<std::string, std::string> external_contexts;
    std::set<std::string> restored_paths;
  };
  int user = 0;
  std::vector<App> apps;
  std::vector<DatabaseFile> files;
};

RestoreRegistration::RestoreRegistration() : impl_(std::make_unique<Impl>()) {}
RestoreRegistration::~RestoreRegistration() = default;

bool RestoreRegistration::Prepare(int user, const std::vector<std::string>& paths,
                                  const std::string& staging, std::string& error) {
  impl_->user = user;
  impl_->apps.clear();
  impl_->files.clear();
  XMLDocument database;
  std::map<int, std::unique_ptr<XMLDocument>> documents;
  if (!PackageDatabase(database, error) || !Users(documents, error)) return false;
  std::map<int, XMLDocument*> users;
  for (const auto& [id, document] : documents) users[id] = document.get();
  const std::string prefix = std::string(kMetadataRoot) + "/" + std::to_string(user) + "/";
  std::set<std::string> names, allowed;
  for (const auto& path : paths) {
    if (path.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string name = path.substr(prefix.size());
    if (name.find('/') != std::string::npos || name.empty())
      return Fail(error, "Invalid app metadata path");
    names.insert(name);
  }
  if (names.empty())
    return Fail(error, "This backup has no package registration; create a new backup first");
  for (const auto& name : names) {
    XMLDocument metadata;
    if (!Load(staging + prefix + name + "/registration.xml", metadata, error)) return false;
    auto* root = metadata.RootElement();
    Impl::App app;
    app.name = name;
    auto* package = root ? root->FirstChildElement("package") : nullptr;
    if (Attr(package, "name") != name)
      return Fail(error, "App backup metadata does not match its package");
    app.code = Attr(package, "codePath");
    app.old_id = package->Attribute("sharedUserId") ? package->UnsignedAttribute("sharedUserId")
                                                    : package->UnsignedAttribute("userId");
    app.data_only = Attr(root, "data-only") == "true";
    app.has_apk = app.code.compare(0, 10, "/data/app/") == 0;
    if (!ImportPackage(database, users, metadata, user, Sdk(database), app.new_id, error))
      return false;
    if (!DataContext(Attr(root, "ce-context"), app.old_id, app.new_id, user, app.ce_context) ||
        !DataContext(Attr(root, "de-context"), app.old_id, app.new_id, user, app.de_context))
      return Fail(error, "Unsupported restored app security context", name);
    for (const char* type : { "data", "obb", "media" }) {
      const std::string saved = Attr(root, (std::string("external-") + type).c_str());
      std::string context;
      if (!saved.empty() && saved != "u:object_r:media_rw_data_file:s0" &&
          !DataContext(saved, app.old_id, app.new_id, user, context))
        return Fail(error, "Unsupported external app data context", name);
      app.external_contexts[type] = saved == "u:object_r:media_rw_data_file:s0" ? saved : context;
    }
    for (auto* apk = root->FirstChildElement("apk"); apk; apk = apk->NextSiblingElement("apk")) {
      const std::string file = Attr(apk, "name"), hash = Attr(apk, "sha256");
      if (file.empty() || file == "." || file == ".." || file.find('/') != std::string::npos ||
          hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos ||
          app.apks.count(file))
        return Fail(error, "Invalid APK verification metadata", name);
      app.apks[file] = hash;
    }
    if (app.has_apk && !app.apks.count("base.apk"))
      return Fail(error, "Backup has no base APK verification", name);
    allowed.insert(prefix + name);
    if (app.has_apk) allowed.insert(app.code);
    const std::string id = std::to_string(user);
    for (const char* kind : { "user", "user_de" })
      allowed.insert("/data/" + std::string(kind) + "/" + id + "/" + name);
    for (const char* kind : { "data", "obb", "media" })
      allowed.insert("/data/media/" + id + "/Android/" + kind + "/" + name);
    for (const auto& path : paths) {
      const bool internal =
          path == "/data/user/" + id + "/" + name || path == "/data/user_de/" + id + "/" + name;
      const bool external = path == "/data/media/" + id + "/Android/data/" + name ||
                            path == "/data/media/" + id + "/Android/obb/" + name ||
                            path == "/data/media/" + id + "/Android/media/" + name;
      if (internal || external || path == app.code) app.restored_paths.insert(path);
    }
    if (app.has_apk && std::find(paths.begin(), paths.end(), app.code) == paths.end())
      return Fail(error, "Backup does not include the app's APK folder", name);
    impl_->apps.push_back(std::move(app));
  }
  for (const auto& path : paths)
    if (!allowed.count(path)) return Fail(error, "Backup contains an unexpected app path", path);
  for (const auto& [id, document] : documents) {
    DatabaseFile file;
    if (!PrepareFile("/data/system/users/" + std::to_string(id) + "/package-restrictions.xml",
                     *document, file, error))
      return false;
    impl_->files.push_back(std::move(file));
  }
  DatabaseFile file;
  if (!PrepareFile("/data/system/packages.xml", database, file, error)) return false;
  impl_->files.push_back(std::move(file));
  return true;
}

bool RestoreRegistration::Finish(std::string& error) {
  if (impl_->apps.empty()) return Fail(error, "App registration was not prepared");
  for (const auto& app : impl_->apps) {
    for (const auto& [file, expected] : app.apks) {
      std::string hash;
      if (!Digest(app.code + "/" + file, hash, error)) return false;
      if (hash != expected) return Fail(error, "Restored APK does not match its backup", app.name);
    }
    const unsigned old_uid = impl_->user * 100000 + app.old_id;
    const unsigned new_uid = impl_->user * 100000 + app.new_id;
    const unsigned old_cache = impl_->user * 100000 + app.old_id + 10000;
    const unsigned new_cache = impl_->user * 100000 + app.new_id + 10000;
    const bool incremental = app.has_apk && IsIncrementalApk(app.code);
    if (app.has_apk && !incremental) {
      const std::string parent = app.code.substr(0, app.code.find_last_of('/'));
      if (parent != "/data/app") {
        struct stat st{};
        if (lstat(parent.c_str(), &st) || !S_ISDIR(st.st_mode) ||
            lchown(parent.c_str(), 1000, 1000) ||
            lsetfilecon(parent.c_str(), "u:object_r:apk_data_file:s0") ||
            chmod(parent.c_str(), 0755))
          return Fail(error, "Cannot repair restored APK parent folder", parent);
      }
    }
    if (app.has_apk && !incremental &&
        !LabelTree(app.code, 1000, 1000, 1000, 1000, "u:object_r:apk_data_file:s0", true, error))
      return false;
    for (const auto& [type, context] : std::map<std::string, std::string>{
             { "user", app.ce_context }, { "user_de", app.de_context } }) {
      const std::string path = "/data/" + type + "/" + std::to_string(impl_->user) + "/" + app.name;
      if (!app.restored_paths.count(path)) continue;
      if (!LabelTree(path, old_uid, new_uid, old_cache, new_cache, context, false, error))
        return false;
      struct stat st{};
      if (!lstat(path.c_str(), &st) && (context.empty() || lchown(path.c_str(), new_uid, new_uid)))
        return Fail(error, "Missing app data security context", path);
    }
    for (const auto& [type, context] : app.external_contexts) {
      const std::string path =
          "/data/media/" + std::to_string(impl_->user) + "/Android/" + type + "/" + app.name;
      if (!app.restored_paths.count(path)) continue;
      struct stat st{};
      if (!lstat(path.c_str(), &st) && context.empty())
        return Fail(error, "Missing external app data context", path);
      if (!LabelTree(path, old_uid, new_uid, old_cache, new_cache, context, false, error))
        return false;
    }
  }
  return Commit(impl_->files, error);
}
}  // namespace aeraui::appvault
