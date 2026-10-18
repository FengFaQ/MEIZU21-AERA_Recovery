/* SPDX-License-Identifier: Apache-2.0 */
#include "pc_connection.hpp"
#include "../aera_shared_storage.hpp"
#include "pc_storage.hpp"
#include "backup_cancel.hpp"
#include "pc_terminal.hpp"
#include "../aera_flash_log.hpp"
#include "../aeraui/features/backup/app_vault.hpp"

#ifdef OF_ENABLE_WLAN
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/vfs.h>
#include <unistd.h>
#ifndef AERA_PC_HOST_TEST
#include <selinux/selinux.h>
#include "../mtp/ffs/MtpUtils.h"
#endif
#include <json/json.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include "aera_remote.hpp"
#include "../aeraui/features/update/payload_inspector.hpp"
#include "../aeraui/features/update/payload_arb.hpp"
#include "../aeraui/features/update/payload_full_flash.hpp"
#include "../aeraui/features/update/payload_otaripper.hpp"
#include "../aeraui/features/root/root_manager.hpp"
#include "third_party/mdns/mdns.h"

#ifndef AERA_PC_PRIVATE_DIRECTORY
#define AERA_PC_PRIVATE_DIRECTORY "/data/misc/aera/pc-connection"
#endif
#ifndef AERA_PC_CLIENT_PATH
#define AERA_PC_CLIENT_PATH "/system/etc/aera/remote/pc/index.html"
#endif

extern char **environ;
namespace aera::pc {
namespace {
using Clock = std::chrono::steady_clock;
using TlsContext = std::shared_ptr<SSL_CTX>;
constexpr size_t kMaximumHeaders = 8192;
constexpr size_t kMaximumJson = 8192;
constexpr uint64_t kStorageReserve = 16ULL * 1024 * 1024;

struct Pair {
  std::string id, name, fingerprint, token;
  Clock::time_point expires;
  bool shown = false, allowed = false, denied = false;
};
struct Transfer {
  std::string id, owner, computer, name, root, directory, path, checksum;
  std::string phase = "idle", detail, log;
  uint64_t bytes = 0, received = 0;
  int progress = 0, result = -1;
  bool shown = false;
  bool cancel_requested = false;
  int upload_socket = -1;
  bool upload_finalizing = false;
  bool from_url = false;
  Clock::time_point upload_started{}, upload_sample{}, upload_last_data{};
  uint64_t upload_sample_bytes = 0;
  double upload_speed = 0;
  std::string save_path;
  bool existing_file = false;
  struct stat source{};
  Json::Value package_info{Json::objectValue};
  Json::Value root_inspection{Json::objectValue};
  Json::Value backup_result{Json::objectValue};
  aeraui::JobRequest request;
  aeraui::JobRequest fast_request;
  aeraui::JobRequest payload_request;
};
std::mutex g_lock, g_lifecycle;
std::mutex g_diagnostic_lock;
std::mutex g_download_lock;
std::thread g_download_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_backup_active{false}, g_backup_cancel{false};
std::atomic<unsigned> g_generation{0};
std::atomic<unsigned> g_network_generation{0};
int g_port = 443;
std::string g_network_ip;
TlsContext g_tls_context;
std::shared_ptr<EVP_PKEY> g_authority_key;
std::shared_ptr<X509> g_authority_certificate;
int g_listener = -1, g_http_listener = -1;
std::thread g_accept_thread, g_http_thread, g_mdns_thread;
std::set<int> g_connections;
Status g_status;
Json::Value g_platform{Json::objectValue};
Json::Value g_trusted{Json::objectValue};
std::map<std::string, std::string> g_sessions;
Pair g_pair;
Transfer g_transfer;
std::string g_hostname = "aera.local";
Json::Value g_prompt{Json::objectValue};
Json::Value g_presentation{Json::objectValue};
std::string g_prompt_answer_id;
bool g_prompt_answer = false;
bool g_recovery_busy = false, g_file_busy = false;
std::string g_reboot_target, g_certificate_pem;
Clock::time_point g_reboot_at;
struct Download { std::string path, owner; Clock::time_point expires; };
std::map<std::string, Download> g_downloads;
std::set<std::string> g_cleaned_transfer_roots;

std::string Hex(const unsigned char *data, size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    text[i * 2] = digits[data[i] >> 4];
    text[i * 2 + 1] = digits[data[i] & 15];
  }
  return text;
}
std::string RandomToken() {
  std::array<unsigned char, 32> bytes{};
  if (RAND_bytes(bytes.data(), bytes.size()) != 1) return {};
  return Hex(bytes.data(), bytes.size());
}
bool IsToken(const std::string &text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(),
      [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string Fingerprint(const std::string &key) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(key.data()), key.size(), digest.data());
  return Hex(digest.data(), digest.size());
}
std::string JsonText(const Json::Value &value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}
bool ParseJson(const std::string &text, Json::Value *value) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string error;
  return reader->parse(text.data(), text.data() + text.size(), value, &error) && value->isObject();
}
std::string ReadFile(const std::string &path, size_t maximum) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::string text(maximum + 1, '\0');
  input.read(text.data(), text.size());
  text.resize(static_cast<size_t>(input.gcount()));
  return text.size() <= maximum ? text : std::string{};
}
std::string ReadLogTail(const std::string &path, size_t maximum) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return {};
  const std::streamoff size = input.tellg();
  if (size < 0) return {};
  const auto offset = size > static_cast<std::streamoff>(maximum) ? size - static_cast<std::streamoff>(maximum) : std::streamoff(0);
  input.seekg(offset);
  std::string text(maximum, '\0');
  input.read(text.data(), text.size());
  text.resize(static_cast<size_t>(input.gcount()));
  if (offset > 0) text.insert(0, "[AERA: last 4 MiB of recovery log]\n");
  return text.empty() ? "Recovery log is empty.\n" : text;
}
std::string MdnsSuffix(const std::string &device, const std::string &ip) {
  std::string suffix = device;
  for (char &c : suffix) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') c = '-';
  if (suffix.empty()) suffix = "recovery";
  return suffix + "-" + ip.substr(ip.find_last_of('.') + 1);
}
bool MakeDirectories(const std::string &path) {
  if (path.empty() || path.front() != '/') return false;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/') continue;
    const std::string part = path.substr(0, i);
    struct stat info{};
    if (lstat(part.c_str(), &info) == 0) {
      if (!S_ISDIR(info.st_mode)) return false;
    } else if (errno != ENOENT || mkdir(part.c_str(), 0700) != 0) {
      return false;
    }
  }
  aera::shared_storage::Normalize(path);
  return true;
}
bool WriteAll(int fd, const char *bytes, size_t size) {
  while (size) {
    const ssize_t count = write(fd, bytes, size);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    bytes += count; size -= static_cast<size_t>(count);
  }
  return true;
}
bool AtomicFile(const std::string &path, const std::string &text) {
  const std::string temporary = path + ".tmp";
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  bool ok = fchmod(fd, 0600) == 0 && WriteAll(fd, text.data(), text.size()) && fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  if (!ok || rename(temporary.c_str(), path.c_str()) != 0) { unlink(temporary.c_str()); return false; }
  aera::shared_storage::Normalize(path);
  return true;
}
void RemoveTransferFiles(const Transfer &transfer) {
  if (transfer.existing_file || transfer.directory.empty()) return;
  unlink((transfer.path + ".part").c_str());
  unlink(transfer.path.c_str());
  unlink((transfer.directory + "/.transfer.json").c_str());
  rmdir(transfer.directory.c_str());
}
bool SaveTrusted() {
  return g_status.remember_available &&
      AtomicFile(std::string(AERA_PC_PRIVATE_DIRECTORY) + "/computers.json", JsonText(g_trusted));
}

TlsContext MakeTlsContext(bool persistent, const std::string &ip,
    std::string *fingerprint, std::string *public_certificate) {
  TlsContext context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
  if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1) return {};
  const std::string path = std::string(AERA_PC_PRIVATE_DIRECTORY) + "/authority.pem";
  auto key = g_authority_key;
  auto certificate = g_authority_certificate;
  if (!key && !certificate && persistent) {
    FILE *file = fopen(path.c_str(), "r");
    if (file) {
      key.reset(PEM_read_PrivateKey(file, nullptr, nullptr, nullptr), EVP_PKEY_free);
      certificate.reset(PEM_read_X509(file, nullptr, nullptr, nullptr), X509_free);
      fclose(file);
    }
  }
  if (!key || !certificate || X509_check_private_key(certificate.get(), key.get()) != 1 ||
      X509_get_ext_by_NID(certificate.get(), NID_subject_key_identifier, -1) < 0) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *generated = nullptr;
    if (!generator || EVP_PKEY_keygen_init(generator.get()) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(generator.get(), &generated) != 1) return {};
    key.reset(generated, EVP_PKEY_free);
    certificate.reset(X509_new(), X509_free);
    if (!certificate) return {};
    X509_set_version(certificate.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
    X509_gmtime_adj(X509_get_notBefore(certificate.get()), -86400);
    X509_gmtime_adj(X509_get_notAfter(certificate.get()), 10L * 365 * 86400);
    X509_NAME *name = X509_get_subject_name(certificate.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("AERA Recovery device authority"), -1, -1, 0);
    X509_set_issuer_name(certificate.get(), name);
    if (X509_set_pubkey(certificate.get(), key.get()) != 1) return {};
    X509V3_CTX authority_context;
    X509V3_set_ctx(&authority_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
    for (const auto &extension : {std::make_pair(NID_basic_constraints, "critical,CA:TRUE,pathlen:0"),
                                std::make_pair(NID_key_usage, "critical,keyCertSign,cRLSign"),
                                std::make_pair(NID_subject_key_identifier, "hash"),
                                std::make_pair(NID_authority_key_identifier, "keyid:always")}) {
      std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> value(
          X509V3_EXT_nconf_nid(nullptr, &authority_context, extension.first, extension.second), X509_EXTENSION_free);
      if (!value || X509_add_ext(certificate.get(), value.get(), -1) != 1) return {};
    }
    if (X509_set_pubkey(certificate.get(), key.get()) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) return {};
    if (persistent) {
      std::unique_ptr<BIO, decltype(&BIO_free)> memory(BIO_new(BIO_s_mem()), BIO_free);
      if (!memory || PEM_write_bio_PrivateKey(memory.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
          PEM_write_bio_X509(memory.get(), certificate.get()) != 1) return {};
      char *bytes = nullptr;
      const long size = BIO_get_mem_data(memory.get(), &bytes);
      if (size <= 0 || !AtomicFile(path, std::string(bytes, static_cast<size_t>(size)))) return {};
    }
  }
  g_authority_key = key;
  g_authority_certificate = certificate;
  std::unique_ptr<BIO, decltype(&BIO_free)> public_pem(BIO_new(BIO_s_mem()), BIO_free);
  if (!public_pem || PEM_write_bio_X509(public_pem.get(), certificate.get()) != 1) return {};
  char *public_bytes = nullptr;
  const long public_size = BIO_get_mem_data(public_pem.get(), &public_bytes);
  if (public_size <= 0) return {};
  *public_certificate = std::string(public_bytes, static_cast<size_t>(public_size));
  unsigned char digest[SHA256_DIGEST_LENGTH]; unsigned length = 0;
  if (X509_digest(certificate.get(), EVP_sha256(), digest, &length) != 1) return {};
  *fingerprint = Hex(digest, length);
  // A private device CA stays stable; the short-lived server certificate follows its current IP.
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
      EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
  EVP_PKEY *generated = nullptr;
  if (!generator || EVP_PKEY_keygen_init(generator.get()) != 1 ||
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1) != 1 ||
      EVP_PKEY_keygen(generator.get(), &generated) != 1) return {};
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> server_key(generated, EVP_PKEY_free);
  std::unique_ptr<X509, decltype(&X509_free)> server(X509_new(), X509_free);
  if (!server) return {};
  X509_set_version(server.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(server.get()), time(nullptr));
  X509_gmtime_adj(X509_get_notBefore(server.get()), -86400);
  X509_gmtime_adj(X509_get_notAfter(server.get()), 90L * 86400);
  X509_NAME_add_entry_by_txt(X509_get_subject_name(server.get()), "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char *>("aera.local"), -1, -1, 0);
  X509_set_issuer_name(server.get(), X509_get_subject_name(certificate.get()));
  if (X509_set_pubkey(server.get(), server_key.get()) != 1) return {};
  X509V3_CTX server_context;
  X509V3_set_ctx(&server_context, certificate.get(), server.get(), nullptr, nullptr, 0);
  std::string device;
  { std::lock_guard<std::mutex> guard(g_lock); device = g_platform["device"].asString(); }
  std::string alternatives = "DNS:localhost,DNS:aera.local,IP:127.0.0.1,IP:::1";
  if (!ip.empty() && ip != "127.0.0.1") {
    alternatives += ",IP:" + ip;
    const auto suffix = MdnsSuffix(device, ip);
    for (unsigned conflict = 1; conflict <= 8; ++conflict)
      alternatives += ",DNS:aera-" + suffix + (conflict == 1 ? "" : "-" + std::to_string(conflict)) + ".local";
  }
  const std::pair<int, std::string> extensions[] = {
    {NID_subject_alt_name, alternatives}, {NID_basic_constraints, "critical,CA:FALSE"},
    {NID_key_usage, "critical,digitalSignature"}, {NID_ext_key_usage, "serverAuth"},
    {NID_subject_key_identifier, "hash"}, {NID_authority_key_identifier, "keyid:always"}};
  for (const auto &extension : extensions) {
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> value(
        X509V3_EXT_nconf_nid(nullptr, &server_context, extension.first, extension.second.c_str()), X509_EXTENSION_free);
    if (!value || X509_add_ext(server.get(), value.get(), -1) != 1) return {};
  }
  if (X509_set_pubkey(server.get(), server_key.get()) != 1 ||
      X509_sign(server.get(), key.get(), EVP_sha256()) <= 0 ||
      SSL_CTX_use_certificate(context.get(), server.get()) != 1 ||
      SSL_CTX_use_PrivateKey(context.get(), server_key.get()) != 1) return {};
  return context;
}

struct HttpRequest {
  std::string method, path, host, origin, authorization, prefix;
  std::string cookie, upgrade, websocket_key, websocket_version, websocket_protocol;
  uint64_t length = 0;
};
bool TlsWrite(SSL *tls, const char *bytes, size_t size) {
  while (size) {
    const int count = SSL_write(tls, bytes, static_cast<int>(std::min<size_t>(size, 65536)));
    if (count <= 0) return false;
    bytes += count; size -= static_cast<size_t>(count);
  }
  return true;
}
thread_local bool keep_connection = false;
void Reply(SSL *tls, int status, const std::string &body, const char *type = "application/json",
           const std::string &extra_headers = "") {
  const char *reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 400 ? "Bad Request" :
      status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 409 ? "Conflict" : "Error";
  const std::string header = "HTTP/1.1 " + std::to_string(status) + " " + reason +
      "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
      "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer"
      "\r\nContent-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'"
      "\r\nConnection: " + (keep_connection ? "keep-alive\r\nKeep-Alive: timeout=5" : "close") +
      "\r\n" + extra_headers + "\r\n";
  if (!TlsWrite(tls, header.data(), header.size()) || !TlsWrite(tls, body.data(), body.size()))
    keep_connection = false;
}
void Error(SSL *tls, int status, const std::string &message) {
  (void)tls;
  // Network writes must never occur while an endpoint holds the UI state lock.
  extern thread_local int pending_error_status;
  extern thread_local std::string pending_error;
  pending_error_status = status; pending_error = message;
}
thread_local int pending_error_status = 0;
thread_local std::string pending_error;
std::string Lower(std::string value) {
  for (char &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}
std::string Trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
bool IsBootId(const std::string &id) {
  if (id.size() != 36) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') return false;
    } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  }
  return true;
}
std::string BootId() {
  const auto id = Trim(ReadFile("/proc/sys/kernel/random/boot_id", 64));
  return IsBootId(id) ? id : "";
}
bool MarkTransfer(const Transfer &transfer) {
  const auto boot = BootId();
  if (boot.empty()) return true;  // No trustworthy boot ID means no automatic cleanup.
  Json::Value marker;
  marker["schema"] = 1; marker["owner"] = "AERA-PC-TRANSFER";
  marker["id"] = transfer.id; marker["boot_id"] = boot;
  marker["payload"] = transfer.path.substr(transfer.path.find_last_of('/') + 1);
  return AtomicFile(transfer.directory + "/.transfer.json", JsonText(marker));
}
bool RemoveOldTransfer(int parent, const std::string &id, const std::string &boot) {
  const int directory = openat(parent, id.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0) return true;
  const int marker = openat(directory, ".transfer.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  struct stat info{};
  std::array<char, 513> text{};
  ssize_t count = -1;
  if (marker >= 0 && fstat(marker, &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_nlink == 1 && info.st_size > 0 && info.st_size < static_cast<off_t>(text.size()))
    count = read(marker, text.data(), text.size());
  if (marker >= 0) close(marker);
  Json::Value metadata;
  if (count != info.st_size || count <= 0 || !ParseJson(std::string(text.data(), count), &metadata) ||
      metadata["schema"] != 1 || metadata["owner"] != "AERA-PC-TRANSFER" || metadata["id"] != id ||
      !metadata["boot_id"].isString() || !IsBootId(metadata["boot_id"].asString()) ||
      metadata["boot_id"] == boot || (metadata["payload"] != "payload.zip" && metadata["payload"] != "payload.img")) {
    close(directory); return true;
  }
  const auto payload = metadata["payload"].asString();
  DIR *entries = fdopendir(dup(directory));
  bool known = entries != nullptr;
  if (entries) {
    while (const auto *entry = readdir(entries)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      if ((name != ".transfer.json" && name != payload && name != payload + ".part") ||
          fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(info.st_mode) || info.st_nlink != 1) { known = false; break; }
    }
    closedir(entries);
  }
  if (!known) { close(directory); return true; }
  // Only these exact recovery-owned files are removed, never a recursive tree.
  bool okay = true;
  for (const auto &name : {payload, payload + ".part"})
    if (unlinkat(directory, name.c_str(), 0) != 0 && errno != ENOENT) okay = false;
  if (okay && unlinkat(directory, ".transfer.json", 0) != 0) okay = false;
  close(directory);
  if (okay) okay = unlinkat(parent, id.c_str(), AT_REMOVEDIR) == 0;
  if (okay) fprintf(stderr, "PC connection: removed stale transfer %s from an earlier boot.\n", id.c_str());
  return okay;
}
void CleanupTransferRoot(const std::string &root, const std::string &boot) {
  const int storage = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (storage < 0) return;
  const int aera = openat(storage, "AERA", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(storage);
  if (aera < 0) return;
  const int parent = openat(aera, "PCTransfers", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(aera);
  if (parent < 0) return;
  struct stat info{};
  if (fstat(parent, &info) != 0) { close(parent); return; }
  const auto key = root + ":" + std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
  if (g_cleaned_transfer_roots.count(key)) { close(parent); return; }
  DIR *entries = fdopendir(dup(parent));
  bool okay = entries != nullptr;
  if (entries) {
    while (const auto *entry = readdir(entries)) {
      const std::string id = entry->d_name;
      if (!IsToken(id)) continue;
      if (!RemoveOldTransfer(parent, id, boot)) okay = false;
    }
    closedir(entries);
  }
  close(parent);
  if (okay) g_cleaned_transfer_roots.insert(key);
}
bool ReadHeaders(SSL *tls, HttpRequest *request) {
  std::string headers;
  std::array<char, 4096> buffer{};
  size_t separator;
  while ((separator = headers.find("\r\n\r\n")) == std::string::npos) {
    const int count = SSL_read(tls, buffer.data(), buffer.size());
    if (count <= 0) return false;
    headers.append(buffer.data(), static_cast<size_t>(count));
    if (headers.size() > kMaximumHeaders) return false;
  }
  std::istringstream lines(headers.substr(0, separator));
  std::string line, version;
  if (!std::getline(lines, line)) return false;
  std::istringstream first(line);
  if (!(first >> request->method >> request->path >> version) || version != "HTTP/1.1") return false;
  bool have_length = false;
  while (std::getline(lines, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) return false;
    const auto key = Lower(Trim(line.substr(0, colon)));
    const auto value = Trim(line.substr(colon + 1));
    if (key == "transfer-encoding") return false;
    if (key == "host") request->host = value;
    if (key == "origin") request->origin = value;
    if (key == "authorization") request->authorization = value;
    if (key == "cookie") request->cookie = value;
    if (key == "upgrade") request->upgrade = Lower(value);
    if (key == "sec-websocket-key") request->websocket_key = value;
    if (key == "sec-websocket-version") request->websocket_version = value;
    if (key == "sec-websocket-protocol") request->websocket_protocol = value;
    if (key == "content-length") {
      if (have_length || value.empty() || !std::all_of(value.begin(), value.end(), ::isdigit)) return false;
      errno = 0; char *end = nullptr;
      const unsigned long long length = strtoull(value.c_str(), &end, 10);
      if (errno || !end || *end) return false;
      request->length = length; have_length = true;
    }
  }
  request->prefix = headers.substr(separator + 4);
  if (request->prefix.size() > request->length) return false;
  return !request->host.empty() &&
      (request->origin.empty() || request->origin == "https://" + request->host);
}
bool ReadJsonBody(SSL *tls, const HttpRequest &request, Json::Value *body,
                   size_t maximum = kMaximumJson) {
  if (request.length == 0 || request.length > maximum) return false;
  std::string text = request.prefix;
  std::array<char, 4096> buffer{};
  while (text.size() < request.length) {
    const int count = SSL_read(tls, buffer.data(),
        static_cast<int>(std::min<uint64_t>(buffer.size(), request.length - text.size())));
    if (count <= 0) return false;
    text.append(buffer.data(), static_cast<size_t>(count));
  }
  return ParseJson(text, body);
}
std::string SessionOwner(const HttpRequest &request) {
  if (request.authorization.compare(0, 7, "Bearer ") != 0) return {};
  std::lock_guard<std::mutex> guard(g_lock);
  const auto found = g_sessions.find(request.authorization.substr(7));
  return found == g_sessions.end() ? std::string{} : found->second;
}

Json::Value PairResponse(const Pair &pair) {
  Json::Value result;
  result["request"] = pair.id;
  result["state"] = pair.denied ? "denied" : pair.allowed ? "allowed" : "pending";
  if (pair.allowed) result["token"] = pair.token;
  return result;
}
void Connect(SSL *tls, const Json::Value &body, bool polling) {
  if (!body["key"].isString() || !IsToken(body["key"].asString())) {
    Error(tls, 400, "Invalid computer key."); return;
  }
  const std::string fingerprint = Fingerprint(body["key"].asString());
  Json::Value result;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    const auto existing_session = std::find_if(g_sessions.begin(), g_sessions.end(),
        [&](const auto &session) { return session.second == fingerprint; });
    if (!g_running.load()) { Error(tls, 409, "PC connection is off."); return; }
    if (polling) {
      if (!body["request"].isString() || g_pair.id != body["request"].asString() ||
          g_pair.fingerprint != fingerprint) {
        Error(tls, 403, "Connection request expired."); return;
      }
      if (Clock::now() > g_pair.expires && !g_pair.allowed) g_pair.denied = true;
      result = PairResponse(g_pair);
    } else if (existing_session != g_sessions.end()) {
      // Allow means this recovery session; Remember additionally survives reboot.
      result["state"] = "allowed"; result["token"] = existing_session->first;
    } else if (g_trusted.isMember(fingerprint)) {
      if (g_sessions.size() >= 8) g_sessions.clear();
      const std::string token = RandomToken();
      if (token.empty()) { Error(tls, 500, "Could not create a secure session."); return; }
      g_sessions[token] = fingerprint;
      result["state"] = "allowed"; result["token"] = token;
    } else {
      if (!body["name"].isString()) { Error(tls, 400, "Computer name is required."); return; }
      const std::string name = body["name"].asString();
      if (name.empty() || name.size() > 80 || std::any_of(name.begin(), name.end(),
          [](unsigned char c) { return c < 32 || c == 127; })) {
        Error(tls, 400, "Invalid computer name."); return;
      }
      if (!g_pair.id.empty() && !g_pair.denied && !g_pair.allowed && Clock::now() < g_pair.expires) {
        if (g_pair.fingerprint == fingerprint) result = PairResponse(g_pair);
        else { Error(tls, 409, "Another computer is waiting for approval."); return; }
      } else {
        g_pair = Pair{};
        g_pair.id = RandomToken(); g_pair.fingerprint = fingerprint;
        g_pair.name = name; g_pair.expires = Clock::now() + std::chrono::minutes(3);
        if (g_pair.id.empty()) { Error(tls, 500, "Could not create a secure request."); return; }
        result = PairResponse(g_pair);
      }
    }
  }
  Reply(tls, result["state"] == "pending" ? 202 : 200, JsonText(result));
}
bool TerminalPhase(const std::string &phase);
UploadProgress UploadProgressLocked() {
  UploadProgress progress;
  if (g_transfer.phase != "uploading" || g_transfer.existing_file) return progress;
  progress.active = true;
  progress.id = g_transfer.id; progress.name = g_transfer.name;
  progress.bytes = g_transfer.bytes; progress.received = g_transfer.received;
  progress.finalizing = g_transfer.upload_finalizing;
  progress.cancelling = g_transfer.cancel_requested;
  const auto now = Clock::now();
  if (!progress.finalizing && !progress.cancelling &&
      now - g_transfer.upload_last_data < std::chrono::seconds(2)) {
    progress.bytes_per_second = g_transfer.upload_speed;
    if (progress.bytes > 0 && progress.bytes_per_second > 0 &&
        now - g_transfer.upload_started >= std::chrono::seconds(1))
      progress.remaining_seconds = (progress.bytes - std::min(progress.bytes, progress.received)) /
          progress.bytes_per_second;
  }
  return progress;
}
bool CancelTransferLocked(const std::string &id) {
  if (g_transfer.id != id || TerminalPhase(g_transfer.phase) || g_transfer.phase == "installing") return false;
  if (g_transfer.phase == "uploading") {
    g_transfer.cancel_requested = true;
    g_transfer.detail = "Cancelling transfer...";
    // Wake a stalled TLS reader, but keep the writer alive for its final response.
    if (g_transfer.upload_socket >= 0) shutdown(g_transfer.upload_socket, SHUT_RD);
  } else {
    g_transfer.phase = "cancelled";
    g_transfer.detail = "Transfer cancelled. No installation was started.";
    if (g_transfer.request.job == aeraui::Job::kPcBackupOperation) storage::Channel().Stop();
    RemoveTransferFiles(g_transfer);
  }
  return true;
}
Json::Value TransferStatus() {
  Json::Value state;
  state["id"] = g_transfer.id; state["name"] = g_transfer.name;
  state["phase"] = g_transfer.phase; state["detail"] = g_transfer.detail;
  state["bytes"] = Json::UInt64(g_transfer.bytes);
  state["received"] = Json::UInt64(g_transfer.received);
  state["from_url"] = g_transfer.from_url;
  const auto upload = UploadProgressLocked();
  state["upload_transfer"]["active"] = upload.active;
  state["upload_transfer"]["bytes_per_second"] = upload.bytes_per_second;
  state["upload_transfer"]["remaining_seconds"] = upload.remaining_seconds;
  state["upload_transfer"]["finalizing"] = upload.finalizing;
  state["progress"] = g_transfer.progress; state["result"] = g_transfer.result;
  if (g_transfer.request.job == aeraui::Job::kPcBackupOperation &&
      g_transfer.request.backup_source == "computer" &&
      (g_transfer.request.backup_action == "backup" || g_transfer.request.backup_action == "restore")) {
    const auto statistics = storage::Channel().Statistics();
    state["backup_transfer"]["bytes"] = Json::UInt64(statistics.bytes);
    state["backup_transfer"]["bytes_per_second"] = statistics.bytes_per_second;
    state["backup_transfer"]["finalizing"] = statistics.finalizing;
    state["backup_transfer"]["waiting_seconds"] = Json::UInt64(statistics.waiting_seconds);
  }
  state["log"] = g_transfer.log; state["computer"] = g_transfer.computer;
  state["partition"] = g_transfer.request.job != aeraui::Job::kFlashImage ||
      g_transfer.request.partitions.empty() ? "" : g_transfer.request.partitions.front();
  state["install_method"] = g_transfer.request.payload_full ? "fast" :
      g_transfer.request.job == aeraui::Job::kExtractPayload ? "extract" :
      g_transfer.request.job == aeraui::Job::kFlashPayload ?
          (g_transfer.request.payload_direct ? "direct" : "selected") : "normal";
  state["manual_protection_override"] = g_transfer.request.payload_override_protection;
  if (g_transfer.request.job == aeraui::Job::kFlashPayload || g_transfer.request.job == aeraui::Job::kExtractPayload) {
    state["payload_partitions"] = Json::Value(Json::arrayValue);
    for (const auto &name : g_transfer.request.partitions) state["payload_partitions"].append(name);
  }
  if (g_transfer.request.job == aeraui::Job::kFlashPayload && g_transfer.request.payload_direct &&
      (g_transfer.phase == "installing" || g_transfer.phase == "completed" || g_transfer.phase == "failed")) {
    const auto progress = aeraui::RecoveryPayloadFlashProgress();
    auto &payload = state["payload_progress"];
    payload["available"] = progress.available;
    payload["lanes"] = progress.lanes;
    payload["partitions"] = Json::Value(Json::arrayValue);
    for (const auto &partition : progress.partitions) {
      Json::Value item;
      item["name"] = partition.name;
      item["bytes"] = Json::UInt64(partition.bytes);
      item["done"] = Json::UInt64(partition.done);
      const char *phase = "queued";
      int percent = partition.bytes ? static_cast<int>(
          100.0 * std::min(partition.done, partition.bytes) / partition.bytes) : 0;
      switch (partition.phase) {
        case aeraui::PayloadPhase::Preparing: phase = "preparing"; percent = 0; break;
        case aeraui::PayloadPhase::Writing: phase = "writing"; break;
        case aeraui::PayloadPhase::Written: phase = "written"; percent = 100; break;
        case aeraui::PayloadPhase::Verifying: phase = "verifying"; break;
        case aeraui::PayloadPhase::Verified: phase = "verified"; percent = 100; break;
        case aeraui::PayloadPhase::Failed: phase = "failed"; break;
        case aeraui::PayloadPhase::NotFlashed: phase = "not_flashed"; percent = 0; break;
        default: percent = 0; break;
      }
      item["phase"] = phase;
      item["percent"] = percent;
      payload["partitions"].append(item);
    }
  }
  state["both_slots"] = g_transfer.request.both_slots;
  state["prompt"] = g_prompt;
  state["presentation"] = g_presentation;
  state["package_info"] = g_transfer.package_info;
  state["save_path"] = g_transfer.save_path;
  state["source_path"] = g_transfer.existing_file ? g_transfer.path : "";
  state["root_action"] = g_transfer.request.root_action;
  state["nas_action"] = g_transfer.request.nas_action;
  state["root_provider"] = g_transfer.request.root_provider;
  state["root_slot"] = g_transfer.request.root_slot;
  state["root_inspection"] = g_transfer.root_inspection;
  state["backup_action"] = g_transfer.request.backup_action;
  state["backup_kind"] = g_transfer.request.backup_kind;
  state["cancel_requested"] = g_transfer.cancel_requested;
  state["backup_result"] = g_transfer.backup_result;
  // App artwork is fetched once, not retransmitted with every progress poll.
  auto remove_icons = [](Json::Value &apps) {
    if (!apps.isArray()) return;
    for (auto &app : apps) app.removeMember("icon");
  };
  remove_icons(state["backup_result"]["apps"]);
  auto &snapshots = state["backup_result"]["snapshots"];
  if (snapshots.isArray())
    for (auto &snapshot : snapshots) remove_icons(snapshot["apps"]);
  return state;
}
bool TerminalPhase(const std::string &phase) {
  return phase == "idle" || phase == "completed" || phase == "failed" || phase == "cancelled";
}

bool TerminalAuthorized(const std::string &session) {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_running.load() && g_sessions.count(session);
}

std::string TerminalCookie(const HttpRequest &request) {
  std::istringstream cookies(request.cookie);
  std::string value;
  while (std::getline(cookies, value, ';')) {
    value = Trim(value);
    constexpr char prefix[] = "aera_terminal=";
    if (value.compare(0, sizeof(prefix) - 1, prefix) == 0) {
      value.erase(0, sizeof(prefix) - 1);
      if (IsToken(value)) return value;
    }
  }
  return {};
}
bool ChildOf(const std::string &path, const std::string &root) {
  return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}
bool ResolveStoragePath(const std::string &path, std::string *resolved, std::string *root = nullptr) {
  if (path.empty() || path.size() > 4095 || path.find('\0') != std::string::npos) return false;
  char canonical[4096];
  if (!realpath(path.c_str(), canonical) || ChildOf(canonical, AERA_PC_PRIVATE_DIRECTORY)) return false;
  std::lock_guard<std::mutex> guard(g_lock);
  for (const auto &storage : g_platform["storages"]) {
    const auto candidate = storage["path"].asString();
    if (ChildOf(canonical, candidate)) {
      *resolved = canonical; if (root) *root = candidate; return true;
    }
  }
  return false;
}
bool FileName(const std::string &name) {
  return !name.empty() && name.size() <= 255 && name != "." && name != ".." &&
      name.find('/') == std::string::npos && std::none_of(name.begin(), name.end(),
        [](unsigned char c) { return c < 32 || c == 127; });
}
bool SetPublicStorageMetadata(const std::string &path, const std::string &parent) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat info{};
  struct statfs filesystem{};
  bool okay = fstat(fd, &info) == 0 && (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)) &&
      fstatfs(fd, &filesystem) == 0;
  // FAT, exFAT and FUSE expose mount-controlled permissions, not inode ownership.
  const bool emulated = filesystem.f_type == 0x4d44 || filesystem.f_type == 0x2011bab0 ||
      filesystem.f_type == 0x65735546;
  if (okay && !emulated) {
#ifdef AERA_PC_HOST_TEST
    const gid_t group = getgid();  // Host fixture has no Android media_rw group.
    const mode_t mode = S_ISDIR(info.st_mode) ? 0775 : 0664;
#else
    const gid_t group = FILE_GROUP;
    const mode_t mode = S_ISDIR(info.st_mode) ? DIR_PERM : FILE_PERM;
#endif
    // Match Recovery MTP without changing the private transfer or TLS files.
    okay = fchown(fd, getuid(), group) == 0 && fchmod(fd, mode) == 0;
#ifndef AERA_PC_HOST_TEST
    if (okay && is_selinux_enabled() > 0) {
      const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      char *context = nullptr;
      okay = directory >= 0 && fgetfilecon(directory, &context) >= 0;
      if (okay) okay = fsetfilecon(fd, context) == 0;
      if (context) freecon(context);
      if (directory >= 0) close(directory);
    }
#else
    (void)parent;
#endif
    if (okay) okay = fsync(fd) == 0;
  }
  if (close(fd) != 0) okay = false;
  return okay;
}
bool MoveNoReplace(const std::string &from, const std::string &to) {
#if defined(SYS_renameat2)
  if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1 /* RENAME_NOREPLACE */) == 0)
    return true;
  if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return false;
#endif
  if (link(from.c_str(), to.c_str()) != 0) return false;
  if (unlink(from.c_str()) == 0) return true;
  unlink(to.c_str()); return false;
}
struct FileAccess {
  bool acquired = false;
  FileAccess() {
    std::lock_guard<std::mutex> guard(g_lock);
    acquired = !g_recovery_busy && !g_file_busy && TerminalPhase(g_transfer.phase) && g_reboot_target.empty();
    if (acquired) g_file_busy = true;
  }
  ~FileAccess() { if (acquired) { std::lock_guard<std::mutex> guard(g_lock); g_file_busy = false; } }
};
Json::Value InspectPackage(Transfer &transfer);
const char *ConfigureImageTarget(const Json::Value &body, aeraui::JobRequest *request) {
  if (!body["partition"].isString() || !body["both_slots"].isBool())
    return "Select an image partition and slot mode.";
  for (const auto &partition : g_platform["partitions"]) {
    if (partition["path"] != body["partition"]) continue;
    if ((partition["logical"].asBool() || !partition["slot_select"].asBool()) && body["both_slots"].asBool())
      return "Both slots require a physical A/B partition.";
    request->partitions.push_back(body["partition"].asString());
    request->both_slots = body["both_slots"].asBool();
    return nullptr;
  }
  return "This recovery does not expose the selected image partition.";
}
void FileCommand(SSL *tls, const std::string &command, const Json::Value &body, const std::string &owner) {
  FileAccess access;
  if (!access.acquired) {
    std::lock_guard<std::mutex> guard(g_lock);
    Error(tls, 409, TerminalPhase(g_transfer.phase) ?
        "Recovery is busy. Please try again shortly." :
        "Finish the current operation or discard its transfer first.");
    return;
  }
  std::string path, root;
  if (!body["path"].isString() || !ResolveStoragePath(body["path"].asString(), &path, &root)) {
    Error(tls, 409, "This path is not in an available mounted storage."); return;
  }
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) { Error(tls, 404, "File is no longer available."); return; }
  Json::Value result;
  if (command == "/api/files/prepare") {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    const std::string extension = name.size() > 4 ? Lower(name.substr(name.size() - 4)) : "";
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || (extension != ".zip" && extension != ".img")) {
      Error(tls, 400, "Select a non-empty ZIP or IMG file."); return;
    }
    Transfer next;
    next.id = RandomToken(); next.owner = owner; next.name = name; next.root = root; next.path = path;
    next.bytes = next.received = static_cast<uint64_t>(info.st_size);
    next.existing_file = true; next.source = info;
    next.phase = "prepared"; next.detail = "Reading package information...";
    next.request.job = extension == ".img" ? aeraui::Job::kFlashImage : aeraui::Job::kInstall;
    next.request.title = extension == ".img" ? "Flash image" : "Install ZIP";
    next.request.path = path; next.request.name = name; next.request.present_before_run = true;
    if (next.id.empty()) { Error(tls, 500, "Could not prepare this file."); return; }
    {
      std::lock_guard<std::mutex> guard(g_lock);
      if (next.request.job == aeraui::Job::kFlashImage) {
        if (const auto error = ConfigureImageTarget(body, &next.request)) { Error(tls, 400, error); return; }
      }
      next.computer = g_trusted.get(owner, "Computer").asString();
      RemoveTransferFiles(g_transfer); g_transfer = next;
      g_prompt = Json::Value(Json::objectValue); g_presentation = Json::Value(Json::objectValue);
    }
    if (next.request.job == aeraui::Job::kInstall) next.package_info = InspectPackage(next);
    {
      std::lock_guard<std::mutex> guard(g_lock);
      if (g_transfer.id != next.id || g_transfer.phase != "prepared") {
        Error(tls, 409, "Package selection was cancelled."); return;
      }
      g_transfer.package_info = std::move(next.package_info);
      g_transfer.fast_request = std::move(next.fast_request);
      g_transfer.payload_request = std::move(next.payload_request);
      g_transfer.phase = "ready"; g_transfer.detail = "Existing file ready for review.";
      result = TransferStatus();
    }
  } else if (command == "/api/files/list") {
    if (!S_ISDIR(info.st_mode)) { Error(tls, 400, "Select a folder."); return; }
    struct CloseDirectory { void operator()(DIR *value) const { closedir(value); } };
    std::unique_ptr<DIR, CloseDirectory> directory(opendir(path.c_str()));
    if (!directory) { Error(tls, 409, "Could not open this folder."); return; }
    std::vector<Json::Value> entries;
    while (auto *entry = readdir(directory.get())) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      struct stat child{};
      const auto child_path = path + "/" + name;
      if (lstat(child_path.c_str(), &child) != 0 || ChildOf(child_path, AERA_PC_PRIVATE_DIRECTORY)) continue;
      Json::Value value;
      value["name"] = name; value["path"] = child_path;
      value["directory"] = S_ISDIR(child.st_mode); value["regular"] = S_ISREG(child.st_mode);
      value["bytes"] = Json::UInt64(std::max<int64_t>(0, child.st_size));
      value["modified"] = Json::Int64(child.st_mtime);
      entries.push_back(std::move(value));
      if (entries.size() >= 10000) { result["truncated"] = true; break; }
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
      if (a["directory"] != b["directory"]) return a["directory"].asBool();
      return Lower(a["name"].asString()) < Lower(b["name"].asString());
    });
    const size_t offset = body["offset"].isUInt() ? body["offset"].asUInt() : 0;
    result["path"] = path; result["root"] = root;
    result["parent"] = path == root ? root : path.substr(0, path.find_last_of('/'));
    result["total"] = Json::UInt64(entries.size()); result["entries"] = Json::Value(Json::arrayValue);
    for (size_t i = offset; i < entries.size() && i < offset + 200; ++i) result["entries"].append(entries[i]);
    result["next"] = offset + 200 < entries.size() ? Json::Value(Json::UInt64(offset + 200)) : Json::Value();
  } else if (command == "/api/files/download") {
    if (!S_ISREG(info.st_mode)) { Error(tls, 400, "Only regular files can be downloaded."); return; }
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto it = g_downloads.begin(); it != g_downloads.end();)
      if (Clock::now() > it->second.expires) it = g_downloads.erase(it); else ++it;
    if (g_downloads.size() >= 8) { Error(tls, 409, "Too many pending downloads."); return; }
    const auto ticket = RandomToken();
    if (ticket.empty()) { Error(tls, 500, "Could not prepare download."); return; }
    g_downloads[ticket] = {path, owner, Clock::now() + std::chrono::seconds(60)};
    result["url"] = "/api/files/content/" + ticket;
  } else {
    if (body.get("confirm", false) != true) { Error(tls, 400, "Confirm the file operation."); return; }
    if (command == "/api/files/mkdir") {
      const auto folder = body["name"].isString() ? path + "/" + body["name"].asString() : "";
      if (!S_ISDIR(info.st_mode) || !body["name"].isString() || !FileName(body["name"].asString()) ||
          mkdir(folder.c_str(), 0775) != 0) {
        Error(tls, 409, "Could not create folder. Its name may already exist."); return;
      }
      if (!SetPublicStorageMetadata(folder, path)) {
        rmdir(folder.c_str());
        Error(tls, 409, "Could not set storage permissions for the new folder."); return;
      }
    } else if (command == "/api/files/delete") {
      if (path == root || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) ||
          (S_ISDIR(info.st_mode) ? rmdir(path.c_str()) : unlink(path.c_str())) != 0) {
        Error(tls, 409, "Could not delete. Folders must be empty; storage roots cannot be removed."); return;
      }
    } else if (command == "/api/files/rename") {
      if (path == root || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) ||
          !body["name"].isString() || !FileName(body["name"].asString())) {
        Error(tls, 400, "Select a file or folder and a valid new name."); return;
      }
      const auto next = path.substr(0, path.find_last_of('/') + 1) + body["name"].asString();
      if (!MoveNoReplace(path, next)) { Error(tls, 409, "Could not rename without overwriting another file."); return; }
    } else { Error(tls, 404, "Unknown file operation."); return; }
    result["success"] = true;
  }
  Reply(tls, 200, JsonText(result));
}
void BackupNext(SSL *tls, const std::string &owner, bool finishing = false) {
  auto value = storage::Channel().Poll(owner);
  if (value.isNull()) {
    if (!finishing) { Error(tls, 409, "No active computer storage session."); return; }
    value = Json::Value(Json::objectValue); value["done"] = true; keep_connection = false;
  }
  const auto metadata = JsonText(value);
  const uint32_t length = metadata.size();
  std::string frame; frame.reserve(4 + metadata.size() + (value.get("op", "").asString() == "write" ? storage::kBlock : 0));
  frame.resize(4);
  for (unsigned i = 0; i < 4; ++i) frame[i] = static_cast<char>(length >> (24 - i * 8));
  frame += metadata;
  if (value.get("op", "").asString() == "write") {
    if (!storage::Channel().AppendData(owner, value["id"].asUInt64(), frame)) {
      Error(tls, 409, "Storage request expired."); return;
    }
  }
  Reply(tls, 200, frame, "application/octet-stream");
}
void BackupIo(SSL *tls, const HttpRequest &request, const std::string &owner) {
  constexpr char prefix[] = "/api/backups/io/";
  if ((request.path == "/api/backups/io" || request.path == "/api/backups/io/next") && request.method == "GET") {
    if (request.path == "/api/backups/io/next") {
      keep_connection = request.length == 0;
      BackupNext(tls, owner); return;
    }
    const auto value = storage::Channel().Poll(owner);
    if (value.isNull()) { Error(tls, 409, "No active computer storage session."); return; }
    keep_connection = request.length == 0;
    Reply(tls, 200, JsonText(value)); return;
  }
  constexpr char exchange_prefix[] = "/api/backups/io/exchange/";
  const bool exchange = request.path.compare(0, strlen(exchange_prefix), exchange_prefix) == 0;
  const auto text = request.path.substr(strlen(exchange ? exchange_prefix : prefix));
  if (text.empty() || text.size() > 20 || !std::all_of(text.begin(), text.end(), ::isdigit)) {
    Error(tls, 400, "Invalid computer storage request."); return;
  }
  const uint64_t id = strtoull(text.c_str(), nullptr, 10);
  if (request.method == "GET") {
    std::string data;
    if (!storage::Channel().Data(owner, id, data)) { Error(tls, 403, "Storage request is not owned by this browser."); return; }
    keep_connection = request.length == 0;
    Reply(tls, 200, data, "application/octet-stream"); return;
  }
  Json::Value result;
  std::string data;
  if (request.method == "PUT") {
    if (request.length > storage::kBlock) { Error(tls, 400, "Storage block is too large."); return; }
    data = request.prefix;
    data.reserve(static_cast<size_t>(request.length));
    std::array<char, 65536> buffer{};
    while (data.size() < request.length) {
      const int count = SSL_read(tls, buffer.data(), std::min<uint64_t>(buffer.size(), request.length - data.size()));
      if (count <= 0) { Error(tls, 409, "Storage transfer interrupted."); return; }
      data.append(buffer.data(), count);
    }
    result["errno"] = 0;
  } else if (request.method != "POST" || !ReadJsonBody(tls, request, &result, 1024 * 1024) ||
             !result.isObject() || (result.isMember("errno") && !result["errno"].isInt())) {
    Error(tls, 400, "Invalid computer storage response."); return;
  }
  if (!storage::Channel().Reply(owner, id, result, data)) { Error(tls, 409, "Storage request expired."); return; }
  keep_connection = true;
  if (exchange) { BackupNext(tls, owner, true); return; }
  Json::Value reply; reply["ok"] = true; Reply(tls, 200, JsonText(reply));
}

void QueueBackup(SSL *tls, const Json::Value &body, const std::string &owner) {
  aeraui::JobRequest job; job.job = aeraui::Job::kPcBackupOperation;
  for (const char *field : {"action", "kind", "source"}) {
    if (!body[field].isString()) { Error(tls, 400, "Select backup type, operation and storage."); return; }
  }
  job.backup_action = body["action"].asString(); job.backup_kind = body["kind"].asString();
  job.backup_source = body["source"].asString();
  if ((job.backup_kind != "recovery" && job.backup_kind != "apps") ||
      (job.backup_source != "computer" && job.backup_source != "device") ||
      (job.backup_action != "backup" && job.backup_action != "restore" && job.backup_action != "inspect" &&
       job.backup_action != "apps" && job.backup_action != "snapshots" && job.backup_action != "storage")) {
    Error(tls, 400, "Unknown backup operation."); return;
  }
  if (body.get("confirm", false) != true) { Error(tls, 400, "Confirm the backup operation."); return; }
  const bool computer = job.backup_source == "computer";
  for (const char *field : {"method", "snapshot", "name", "storage", "path"}) {
    if (body.isMember(field) && !body[field].isString()) { Error(tls, 400, "Invalid backup option."); return; }
  }
  if ((body.isMember("user") && (!body["user"].isInt() || body["user"].asInt() < 0)) ||
      (body.isMember("compression") && !body["compression"].isBool()) ||
      (body.isMember("folder_ready") && !body["folder_ready"].isBool())) {
    Error(tls, 400, "Invalid backup option."); return;
  }
  job.backup_method = body.get("method", "stream").asString();
  if (job.backup_method != "stream" && job.backup_method != "transfer") { Error(tls, 400, "Select a restore method."); return; }
  job.backup_user = body.get("user", 0).asInt();
  job.backup_snapshot = body.get("snapshot", "").asString();
  job.name = body.get("name", "AERA-PC-Backup").asString();
  if (!FileName(job.name) || job.name.size() > 64 || job.name.find('\\') != std::string::npos) {
    Error(tls, 400, "Use a short backup name without path separators."); return;
  }
  job.compression = body.get("compression", true).asBool(); job.digest = true;
  if (!body["partitions"].isArray() || body["partitions"].size() > 1024) {
    Error(tls, 400, "Select apps or partitions."); return;
  }
  for (const auto &item : body["partitions"]) {
    if (!item.isString() || item.asString().empty() || item.asString().size() > 255) {
      Error(tls, 400, "Invalid backup selection."); return;
    }
    job.partitions.push_back(item.asString());
  }
  if ((job.backup_action == "backup" || job.backup_action == "restore") && job.partitions.empty()) {
    Error(tls, 400, "Select at least one app or partition."); return;
  }
  if (!computer || (job.backup_action == "restore" && job.backup_method == "transfer")) {
    const auto selected = body.get("storage", "").asString();
    bool offered = false;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      for (const auto &storage : g_platform["backup_storages"])
        if (storage["path"].asString() == selected) offered = true;
    }
    if (!offered) {
      Error(tls, 409, "Select mounted device storage."); return;
    }
    job.backup_storage = selected;
  }
  if (computer) {
    if (job.backup_action != "apps" && !body.get("folder_ready", false).asBool()) { Error(tls, 400, "Choose a computer folder first."); return; }
    job.path = std::string(storage::kMount) +
        (job.backup_kind == "apps" && job.backup_action == "backup" ? "/AERA/AppBackupVault" : "");
  } else if (job.backup_action == "backup" || job.backup_action == "apps" || job.backup_action == "storage") {
    job.path = job.backup_storage + (job.backup_kind == "apps" ? "/AERA/AppBackupVault" : "");
  } else if (!ResolveStoragePath(body.get("path", "").asString(), &job.path) || job.path.size() > 220) {
    Error(tls, 409, "Select an existing backup folder on mounted storage."); return;
  }
  job.title = job.backup_kind == "apps" ? "App Backup Vault" : "Recovery backup";
  job.show_on_device = job.backup_action == "backup" || job.backup_action == "restore";
  job.present_before_run = true;
  Json::Value result;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_recovery_busy || g_file_busy || !TerminalPhase(g_transfer.phase) || !g_reboot_target.empty()) {
      Error(tls, 409, "Finish the current operation first."); return;
    }
    if (job.backup_kind == "apps" && (!g_platform["app_vault_available"].asBool() || g_platform["locked"].asBool())) {
      Error(tls, 409, "Install App Backup Vault and unlock data first."); return;
    }
    const auto id = RandomToken();
    if (id.empty()) { Error(tls, 500, "Could not prepare the backup operation."); return; }
    if (computer && job.backup_action != "apps" && !storage::Channel().Begin(owner, job.backup_action == "backup")) {
      Error(tls, 409, "Another browser is using computer storage."); return;
    }
    RemoveTransferFiles(g_transfer); g_transfer = Transfer{};
    g_backup_cancel.store(false);
    g_transfer.id = id; g_transfer.owner = owner;
    g_transfer.computer = g_trusted.get(owner, "Computer").asString();
    g_transfer.name = job.title; g_transfer.request = std::move(job);
    g_transfer.phase = "queued"; g_transfer.detail = "Waiting for recovery to start the backup operation.";
    g_prompt = Json::Value(Json::objectValue); g_presentation = Json::Value(Json::objectValue);
    result = TransferStatus();
  }
  Reply(tls, 200, JsonText(result));
}

void QueueNetworkStorage(SSL *tls, const Json::Value &body, const std::string &owner) {
  if (!body["action"].isString()) {
    Error(tls, 400, "Select a network storage operation."); return;
  }
  aeraui::JobRequest job;
  job.job = aeraui::Job::kNetworkStorage;
  job.nas_action = body.get("action", "").asString();
  if (job.nas_action != "save" && job.nas_action != "connect" && job.nas_action != "disconnect") {
    Error(tls, 400, "Select a network storage operation."); return;
  }
  auto &config = job.nas_config;
  for (const auto &field : body.getMemberNames()) {
    if (!body[field].isString() || body[field].asString().find('\0') != std::string::npos) {
      Error(tls, 400, "Invalid network storage setting."); return;
    }
  }
  config.type = body.get("type", "sftp").asString();
  config.host = body.get("host", "").asString();
  config.port = body.get("port", "22").asString();
  config.user = body.get("user", "").asString();
  config.password = body.get("password", "").asString();
  config.share = body.get("share", "Backups").asString();
  config.path = body.get("path", "").asString();
  config.domain = body.get("domain", "WORKGROUP").asString();
  config.cache_mode = body.get("cache_mode", "off").asString();
  config.auth = body.get("auth", "password").asString();
  config.key_name = body.get("key_name", "").asString();
  job.nas_private_key = body.get("private_key", "").asString();
  job.nas_password_update = body.isMember("password");
  job.title = "Network storage";
  job.show_on_device = false; job.present_before_run = true;
  Json::Value result;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_recovery_busy || g_file_busy || !TerminalPhase(g_transfer.phase) ||
        !g_reboot_target.empty() || !g_platform["nas"]["supported"].asBool() ||
        g_platform["locked"].asBool()) {
      Error(tls, 409, "Network storage is unavailable or recovery is busy."); return;
    }
    const auto id = RandomToken();
    if (id.empty()) { Error(tls, 500, "Could not prepare network storage."); return; }
    RemoveTransferFiles(g_transfer); g_transfer = Transfer{};
    g_transfer.id = id; g_transfer.owner = owner;
    g_transfer.computer = g_trusted.get(owner, "Computer").asString();
    g_transfer.name = job.title; g_transfer.request = std::move(job);
    g_transfer.phase = "queued"; g_transfer.detail = "Updating network storage";
    g_prompt = Json::Value(Json::objectValue); g_presentation = Json::Value(Json::objectValue);
    result = TransferStatus();
  }
  Reply(tls, 200, JsonText(result));
}

void SendDownload(SSL *tls, int fd, uint64_t bytes, const std::string &name) {
  const auto header = "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: " +
      std::to_string(bytes) + "\r\nContent-Disposition: attachment; filename=\"" + name +
      "\"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
  bool okay = TlsWrite(tls, header.data(), header.size());
  std::array<char, 65536> buffer{};
  while (okay && bytes) {
    const auto count = read(fd, buffer.data(), std::min<uint64_t>(buffer.size(), bytes));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    okay = TlsWrite(tls, buffer.data(), count); bytes -= count;
  }
  close(fd);
}
void DownloadFile(SSL *tls, const std::string &ticket) {
  FileAccess access;
  if (!access.acquired) { Error(tls, 409, "Recovery is busy."); return; }
  Download download;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    const auto found = g_downloads.find(ticket);
    if (found == g_downloads.end() || Clock::now() > found->second.expires) {
      Error(tls, 403, "Download expired."); return;
    }
    download = found->second; g_downloads.erase(found);
    bool authorized = false;
    for (const auto &session : g_sessions) authorized |= session.second == download.owner;
    if (!authorized) { Error(tls, 403, "Computer approval is no longer available."); return; }
  }
  std::string path;
  if (!ResolveStoragePath(download.path, &path)) { Error(tls, 409, "Storage is no longer available."); return; }
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat info{};
  if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    if (fd >= 0) close(fd);
    Error(tls, 404, "File is no longer available."); return;
  }
  std::ostringstream encoded;
  for (unsigned char c : path.substr(path.find_last_of('/') + 1)) {
    if (std::isalnum(c) || c == '.' || c == '-' || c == '_') encoded << c;
    else encoded << '%' << "0123456789ABCDEF"[c >> 4] << "0123456789ABCDEF"[c & 15];
  }
  const auto header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
      std::to_string(info.st_size) + "\r\nContent-Disposition: attachment; filename*=UTF-8''" + encoded.str() +
      "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
  bool okay = TlsWrite(tls, header.data(), header.size());
  std::array<char, 65536> buffer{};
  uint64_t remaining = static_cast<uint64_t>(info.st_size);
  while (okay && remaining) {
    const ssize_t count = read(fd, buffer.data(), std::min<uint64_t>(buffer.size(), remaining));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    okay = TlsWrite(tls, buffer.data(), static_cast<size_t>(count));
    remaining -= static_cast<uint64_t>(count);
  }
  close(fd);
}
void DiagnosticLog(SSL *tls, bool kernel) {
  std::unique_lock<std::mutex> capture(g_diagnostic_lock, std::try_to_lock);
  if (!capture.owns_lock()) { Error(tls, 409, "Another log snapshot is being collected."); return; }
  int descriptors[2];
  if (pipe2(descriptors, O_CLOEXEC) != 0) { Error(tls, 503, "Could not open the log snapshot pipe."); return; }
  const char *logcat[] = {"/system/bin/logcat", "-b", "all", "-d", "-v", "threadtime", "-t", "4000", nullptr};
  const char *dmesg[] = {"/system/bin/toybox", "dmesg", nullptr};
#ifdef AERA_PC_HOST_TEST
  const char *fixture[] = {"/usr/bin/printf", "Simulated diagnostic snapshot. No device logs are read.\n", nullptr};
  const char **command = fixture;
  (void)kernel; (void)logcat; (void)dmesg;
#else
  const char **command = kernel ? dmesg : logcat;
#endif
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, descriptors[0]);
  posix_spawn_file_actions_addclose(&actions, descriptors[1]);
  pid_t child = -1;
  const int spawned = posix_spawn(&child, command[0], &actions, nullptr, const_cast<char **>(command), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(descriptors[1]);
  if (spawned != 0) { close(descriptors[0]); Error(tls, 503, "This build does not provide the requested log command."); return; }
  std::string output;
  std::array<char, 8192> buffer{};
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  bool ended = false, truncated = false;
  while (Clock::now() < deadline) {
    pollfd input{descriptors[0], POLLIN, 0};
    const int ready = poll(&input, 1, 200);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) break;
    if (ready == 0) continue;
    const ssize_t count = read(descriptors[0], buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ended = true; break; }
    output.append(buffer.data(), static_cast<size_t>(count));
    if (output.size() >= 4 * 1024 * 1024) { truncated = true; break; }
  }
  close(descriptors[0]);
  int status = 0;
  if (!ended) kill(child, SIGKILL);
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  if (truncated) output += "\n[AERA: log snapshot limited to 4 MiB]\n";
  else if (!ended || (waited == child && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))) {
    Error(tls, 503, output.empty() ? "Log capture failed or timed out." : output); return;
  }
  Reply(tls, 200, output.empty() ? "The requested log buffer is empty.\n" : output, "text/plain; charset=utf-8");
}
const char *DownloadBinary() {
#ifdef AERA_PC_HOST_TEST
  return "/usr/bin/wget";
#else
  return access("/sbin/busybox", X_OK) == 0 ? "/sbin/busybox" : "/system/bin/busybox";
#endif
}
bool DirectDownloadUrl(const std::string &url) {
  const auto start = url.compare(0, 8, "https://") == 0 ? 8U :
      url.compare(0, 7, "http://") == 0 ? 7U : 0U;
  if (!start || url.size() > 4096 || url.size() <= start) return false;
  const auto end = url.find_first_of("/?#", start);
  const auto authority = url.substr(start, end == std::string::npos ? end : end - start);
  return !authority.empty() && authority.find('@') == std::string::npos &&
      std::none_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}
void FetchTransfer(Transfer transfer, const std::string &url, unsigned generation);
void PrepareTransfer(SSL *tls, const Json::Value &body, const std::string &owner, bool download = false) {
  if (!body["name"].isString() || !body["storage"].isString() ||
      (!download && (!body["sha256"].isString() || !IsToken(body["sha256"].asString()) ||
       !body["bytes"].isUInt64() || (body["bytes"].asUInt64() == 0 && body["kind"] != "file"))) ||
      (download && (!body["url"].isString() || !DirectDownloadUrl(body["url"].asString()) || body["kind"] == "file")) ||
      !body["kind"].isString() || (body["kind"] != "zip" && body["kind"] != "img" && body["kind"] != "file")) {
    Error(tls, 400, "Invalid transfer request."); return;
  }
  const std::string name = body["name"].asString();
  if (name.empty() || name.size() > 256 || std::any_of(name.begin(), name.end(),
      [](unsigned char c) { return c < 32 || c == 127; })) {
    Error(tls, 400, "Invalid filename."); return;
  }
  Json::Value result;
  std::string save_directory, save_root;
  if (body["kind"] == "file" && (!FileName(name) || !body["folder"].isString() ||
      !ResolveStoragePath(body["folder"].asString(), &save_directory, &save_root))) {
    Error(tls, 400, "Select an available destination folder and a valid filename."); return;
  }
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running.load()) { Error(tls, 409, "PC connection is disabled."); return; }
    if (download && (g_network_ip.empty() || access(DownloadBinary(), X_OK) != 0)) {
      Error(tls, 409, "Connect recovery to Wi-Fi before downloading a link."); return;
    }
    if (!TerminalPhase(g_transfer.phase)) { Error(tls, 409, "Another transfer or installation is active."); return; }
    if (g_recovery_busy || g_file_busy || !g_reboot_target.empty()) { Error(tls, 409, "Recovery is busy."); return; }
    const std::string root = body["storage"].asString();
    if (body["kind"] != "file" &&
        (g_platform["locked"].asBool() || !g_platform["transfer_storage"].isObject() ||
         root != g_platform["transfer_storage"]["path"].asString())) {
      Error(tls, 409, "Installation transfers require available, unlocked internal storage."); return;
    }
    bool storage_available = false;
    for (const auto &storage : g_platform["storages"])
      storage_available |= storage["path"].asString() == root;
    if (!storage_available) { Error(tls, 409, "Unlock or mount the selected storage first."); return; }
    struct statvfs filesystem{};
    struct statfs filesystem_type{};
    char canonical[4096];
    const uint64_t size = download ? 0 : body["bytes"].asUInt64();
    if (!realpath(root.c_str(), canonical) || root != canonical ||
        statfs(root.c_str(), &filesystem_type) != 0 ||
        filesystem_type.f_type == 0x01021994 || filesystem_type.f_type == 0x858458f6 ||
        statvfs(root.c_str(), &filesystem) != 0 || (filesystem.f_flag & ST_RDONLY) ||
        size > UINT64_MAX - kStorageReserve ||
        uint64_t(filesystem.f_bavail) * filesystem.f_frsize < size + kStorageReserve) {
      Error(tls, 409, "Not enough writable storage for the complete file."); return;
    }
    Transfer next;
    next.id = RandomToken(); next.owner = owner; next.name = name;
    next.computer = g_trusted.get(owner, "Computer").asString();
    if (g_pair.fingerprint == owner) next.computer = g_pair.name;
    next.bytes = size; next.checksum = download ? "" : body["sha256"].asString(); next.root = root;
    next.from_url = download;
    if (body["kind"] == "file") {
      struct stat folder{}, existing{};
      next.save_path = save_directory + "/" + name;
      if (save_root != root || stat(save_directory.c_str(), &folder) != 0 || !S_ISDIR(folder.st_mode) ||
          lstat(next.save_path.c_str(), &existing) == 0 || errno != ENOENT) {
        Error(tls, 409, "Destination is unavailable or this filename already exists."); return;
      }
    }
    next.request.job = body["kind"] == "img" ? aeraui::Job::kFlashImage : aeraui::Job::kInstall;
    next.request.title = body["kind"] == "zip" ? "Install ZIP" : "Flash image";
    next.request.name = name;
    next.request.present_before_run = true;
    if (next.request.job == aeraui::Job::kFlashImage) {
      if (const auto error = ConfigureImageTarget(body, &next.request)) { Error(tls, 400, error); return; }
    }
    if (next.id.empty()) { Error(tls, 500, "Could not create a transfer."); return; }
    next.directory = root + "/AERA/PCTransfers/" + next.id;
    next.path = next.directory + (body["kind"] == "zip" ? "/payload.zip" : "/payload.img");
    if (!MakeDirectories(next.directory)) { Error(tls, 409, "Could not create the transfer folder."); return; }
    if (!MarkTransfer(next)) {
      RemoveTransferFiles(next);
      Error(tls, 409, "Could not record the temporary transfer ownership."); return;
    }
    next.request.path = next.path;
    next.phase = download ? "uploading" : "prepared";
    if (download) {
      next.detail = "Downloading file on recovery...";
      next.upload_started = next.upload_sample = next.upload_last_data = Clock::now();
    }
    RemoveTransferFiles(g_transfer);
    g_transfer = std::move(next);
    g_prompt = Json::Value(Json::objectValue);
    g_presentation = Json::Value(Json::objectValue);
    result = TransferStatus();
  }
  if (download) {
    std::lock_guard<std::mutex> guard(g_download_lock);
    if (g_download_thread.joinable()) g_download_thread.join();
    Transfer transfer;
    { std::lock_guard<std::mutex> lock(g_lock); transfer = g_transfer; }
    g_download_thread = std::thread(FetchTransfer, transfer, body["url"].asString(), g_generation.load());
  }
  Reply(tls, 200, JsonText(result));
}
Json::Value InspectPackage(Transfer &transfer) {
  Json::Value platform;
  { std::lock_guard<std::mutex> guard(g_lock); platform = g_platform; }
  auto info = aeraui::payload::InspectZip(transfer.path, true);
  if (info.name_from_filename) {
    info.target_build = transfer.name;
    if (info.target_build.size() > 4) info.target_build.resize(info.target_build.size() - 4);
  }
  Json::Value result(Json::objectValue);
  result["is_payload"] = info.is_payload; result["valid"] = info.valid;
  result["error"] = info.error; result["summary"] = aeraui::payload::Summary(info);
  result["build"] = info.target_build;
  result["name_from_filename"] = info.name_from_filename;
  result["device"] = info.target_device; result["codename"] = info.device_codename;
  result["build_id"] = info.build_id; result["fingerprint"] = info.system_fingerprint;
  result["android"] = info.target_sdk; result["security_patch"] = info.security_patch;
  result["incremental"] = info.incremental; result["partial"] = info.partial;
  result["payload_bytes"] = Json::UInt64(info.payload_bytes);
  result["expanded_bytes"] = Json::UInt64(info.expanded_bytes);
  result["format"] = std::to_string(info.format_version) + "." + std::to_string(info.minor_version);
  result["dynamic_partitions"] = info.dynamic_partitions; result["snapshots"] = info.snapshots;
  result["compression"] = info.virtual_ab_compression;
  result["operations"] = Json::UInt64(info.operations); result["operation_types"] = info.operation_types;
  result["arb_available"] = info.arb_available; result["arb_index"] = info.arb_index;
  result["arb_detail"] = info.arb_detail;
  bool includes_firmware = false;
  std::vector<std::string> names;
  for (const auto &partition : info.partitions) names.push_back(partition.name);
  std::vector<aeraui::PayloadFlashTarget> targets;
  for (const auto &target : platform["payload_targets"])
    targets.push_back({target["name"].asString(), target["path"].asString(), target["bytes"].asUInt64(),
        target["raw"].asBool(), target["device"].asUInt64(), {}});
  std::set<std::string> protected_partitions;
  if (platform.get("preserve_abl", false).asBool()) protected_partitions.insert("abl");
  if (platform.get("preserve_recovery", false).asBool()) protected_partitions.insert("recovery");
  result["partitions"] = Json::Value(Json::arrayValue);
  for (const auto &partition : info.partitions) {
    Json::Value entry;
    entry["name"] = partition.name; entry["bytes"] = Json::UInt64(partition.bytes);
    entry["operations"] = Json::UInt64(partition.operations);
    const auto target = std::find_if(targets.begin(), targets.end(),
        [&](const auto &candidate) { return candidate.name == partition.name; });
    entry["extractable"] = partition.extractable;
    entry["extraction_error"] = partition.extraction_error;
    entry["protected"] = protected_partitions.count(partition.name) != 0;
    entry["flash_available"] = partition.extractable &&
        target != targets.end() && !target->path.empty() && (!target->raw || partition.bytes <= target->bytes);
    result["partitions"].append(entry);
    includes_firmware |= partition.name == "xbl_config";
  }
  const auto device = includes_firmware ? aeraui::payload::ReadDeviceArb() : aeraui::payload::DeviceArb{};
  if (device.slot_a.available) result["slot_a_arb"] = device.slot_a.index;
  if (device.slot_b.available) result["slot_b_arb"] = device.slot_b.index;
  using Decision = aeraui::payload::ArbDecision;
  const auto decision = aeraui::payload::CompareArb(includes_firmware,
      {info.arb_available, info.arb_index, info.arb_detail}, device);
  result["arb_decision"] = decision == Decision::Downgrade ? "downgrade" :
      decision == Decision::Upgrade ? "upgrade" : decision == Decision::Same ? "same" :
      decision == Decision::Unknown ? "unknown" : "not_applicable";
  if (info.is_payload) {
    std::vector<std::string> required;
    std::string error;
    const auto slot = platform.get("slot", "").asString();
    bool available = (slot == "A" || slot == "B") && info.manifest_hash.size() == 32 &&
        aeraui::payload::PlanFullFlash(info, targets, required, error, protected_partitions);
    if (!available && error.empty()) error = "The active slot or payload manifest could not be verified.";
    if (available && access(aeraui::payload::OtaripperExecutable(), X_OK) != 0) {
      available = false; error = "The bundled otaripper engine is unavailable.";
    }
    if (available && !platform.get("snapshots_safe", false).asBool()) {
      available = false; error = "OTA snapshots are active or their state cannot be verified.";
    }
    result["fast_available"] = available;
    result["fast_reason"] = error;
    result["fast_slot"] = slot;
    result["advanced_available"] = info.valid && !info.incremental && info.manifest_hash.size() == 32;
    result["direct_available"] = access(aeraui::payload::OtaripperExecutable(), X_OK) == 0;
    if (result["advanced_available"].asBool()) {
      transfer.payload_request = transfer.request;
      transfer.payload_request.payload_manifest_hash = info.manifest_hash;
      transfer.payload_request.payload_slot = slot;
    }
    result["fast_protected"] = Json::Value(Json::arrayValue);
    for (const auto &partition : info.partitions)
      if (protected_partitions.count(partition.name)) result["fast_protected"].append(partition.name);
    if (available) {
      transfer.fast_request = transfer.request;
      transfer.fast_request.job = aeraui::Job::kFlashPayload;
      transfer.fast_request.title = "Fast flash (experimental)";
      transfer.fast_request.partitions = std::move(required);
      transfer.fast_request.payload_manifest_hash = info.manifest_hash;
      transfer.fast_request.payload_slot = slot;
      transfer.fast_request.payload_direct = true;
      transfer.fast_request.payload_full = true;
    }
  }
  return result;
}
void FetchTransfer(Transfer transfer, const std::string &url, unsigned generation) {
  const std::string partial = transfer.path + ".part";
  int fd = open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  int output[2]{-1, -1}, headers[2]{-1, -1};
  pid_t child = -1;
  std::string failure, header_buffer, content_type, first_bytes;
  uint64_t received = 0, expected = 0, maximum = 0;
  bool have_length = false;
  int response_code = 0;
  struct statvfs filesystem{};
  if (fd < 0 || statvfs(transfer.root.c_str(), &filesystem) != 0 ||
      uint64_t(filesystem.f_bavail) * filesystem.f_frsize <= kStorageReserve) {
    failure = "Not enough writable storage for the complete download.";
  } else maximum = uint64_t(filesystem.f_bavail) * filesystem.f_frsize - kStorageReserve;
  SHA256_CTX hash{}; SHA256_Init(&hash);
  if (failure.empty() && (pipe2(output, O_CLOEXEC) != 0 || pipe2(headers, O_CLOEXEC) != 0))
    failure = "Could not start the download.";
  if (failure.empty()) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, headers[1], STDERR_FILENO);
    for (int descriptor : {output[0], output[1], headers[0], headers[1]})
      posix_spawn_file_actions_addclose(&actions, descriptor);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    std::vector<std::string> arguments{DownloadBinary()};
#ifndef AERA_PC_HOST_TEST
    arguments.emplace_back("wget");
#endif
    for (const char *argument : {"-q", "-S", "-T", "20", "-t", "1", "-O", "-"})
      arguments.emplace_back(argument);
    arguments.push_back(url);
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    if (posix_spawn(&child, argv[0], &actions, &attributes, argv.data(), environ) != 0) {
      child = -1; failure = "Could not start the download.";
    }
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
  }
  for (int descriptor : {output[1], headers[1]}) if (descriptor >= 0) close(descriptor);
  output[1] = headers[1] = -1;
  const auto last_started = Clock::now();
  auto last_data = last_started;
  auto current = [&] {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running.load() && g_generation.load() == generation &&
        g_transfer.id == transfer.id && g_transfer.phase == "uploading" &&
        !g_transfer.cancel_requested && !g_network_ip.empty();
  };
  auto header = [&](std::string line) {
    line = Trim(line);
    if (line.compare(0, 5, "HTTP/") == 0) {
      std::istringstream response(line); std::string protocol;
      response >> protocol >> response_code;
      // Redirect responses must not supply the final file's name or size.
      have_length = false; expected = 0; content_type.clear(); transfer.name = transfer.request.name;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) return;
    const auto key = Lower(Trim(line.substr(0, colon)));
    const auto value = Trim(line.substr(colon + 1));
    if (key == "content-length") {
      char *end = nullptr; errno = 0;
      const auto size = strtoull(value.c_str(), &end, 10);
      if (!value.empty() && value.front() >= '0' && value.front() <= '9' && !errno && end && !*end) {
        expected = size; have_length = true;
      }
    } else if (key == "content-type") content_type = Lower(value);
    else if (key == "content-disposition") {
      const auto lower = Lower(value);
      const auto start = lower.find("filename=");
      if (start != std::string::npos) {
        auto name = Trim(value.substr(start + 9));
        if (!name.empty() && name.front() == '"') {
          const auto end_quote = name.find('"', 1);
          name = end_quote == std::string::npos ? "" : name.substr(1, end_quote - 1);
        } else name = Trim(name.substr(0, name.find(';')));
        if (FileName(name)) transfer.name = name;
      }
    }
  };
  std::array<char, 65536> buffer{};
  while (failure.empty() && (output[0] >= 0 || headers[0] >= 0)) {
    if (!current()) { failure = "Download cancelled or Wi-Fi disconnected. No installation was started."; break; }
    if (Clock::now() - last_data > std::chrono::seconds(60)) {
      failure = "The download stopped responding. No installation was started."; break;
    }
    pollfd events[2]{{headers[0], POLLIN | POLLHUP, 0}, {output[0], POLLIN | POLLHUP, 0}};
    const int polled = poll(events, 2, 100);
    if (polled < 0 && errno == EINTR) continue;
    if (polled < 0) { failure = "The download connection failed."; break; }
    for (int index = 0; index < 2 && failure.empty(); ++index) {
      int &descriptor = index == 0 ? headers[0] : output[0];
      if (descriptor < 0 || !events[index].revents) continue;
      const auto count = read(descriptor, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { close(descriptor); descriptor = -1; continue; }
      last_data = Clock::now();
      if (index == 0) {
        header_buffer.append(buffer.data(), count);
        size_t newline;
        while ((newline = header_buffer.find('\n')) != std::string::npos) {
          header(header_buffer.substr(0, newline)); header_buffer.erase(0, newline + 1);
        }
        if (header_buffer.size() > 65536) { failure = "Invalid download response headers."; break; }
      } else {
        if (first_bytes.size() < 128) first_bytes.append(buffer.data(), std::min<size_t>(count, 128 - first_bytes.size()));
        if (uint64_t(count) > maximum - received || !WriteAll(fd, buffer.data(), count)) {
          failure = "Not enough writable storage for the complete download."; break;
        }
        SHA256_Update(&hash, buffer.data(), count); received += count;
      }
      if (response_code == 200 && have_length && expected > maximum) { failure = "Not enough storage for the linked file."; break; }
      if (response_code == 200 && content_type.find("html") != std::string::npos) {
        failure = "This link opens a webpage. Use a direct ZIP or IMG download link."; break;
      }
      std::lock_guard<std::mutex> guard(g_lock);
      if (g_transfer.id != transfer.id) continue;
      g_transfer.bytes = have_length ? expected : 0; g_transfer.received = received;
      g_transfer.name = transfer.name;
      const auto now = Clock::now();
      const double elapsed = std::chrono::duration<double>(now - g_transfer.upload_sample).count();
      g_transfer.upload_last_data = now;
      if (elapsed >= .25) {
        const double speed = (received - g_transfer.upload_sample_bytes) / elapsed;
        g_transfer.upload_speed = g_transfer.upload_speed == 0 ? speed : .7 * g_transfer.upload_speed + .3 * speed;
        g_transfer.upload_sample = now; g_transfer.upload_sample_bytes = received;
      }
    }
  }
  for (int descriptor : {output[0], headers[0]}) if (descriptor >= 0) close(descriptor);
  int status = 0;
  if (child > 0) {
    if (!failure.empty()) { kill(-child, SIGKILL); kill(child, SIGKILL); }
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (failure.empty() && (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0))
      failure = "Download failed. Use a direct link without login or confirmation.";
  }
  if (failure.empty() && (response_code != 200 || !received || (have_length && received != expected)))
    failure = "The download is incomplete. No installation was started.";
  const bool image = transfer.request.job == aeraui::Job::kFlashImage;
  const auto prefix = Lower(Trim(first_bytes));
  if (failure.empty() && (prefix.compare(0, 5, "<html") == 0 || prefix.compare(0, 9, "<!doctype") == 0 ||
      (!image && first_bytes.compare(0, 4, "PK\003\004") != 0)))
    failure = "This link did not return a ZIP or IMG file. No installation was started.";
  const auto extension = transfer.name.size() > 4 ? Lower(transfer.name.substr(transfer.name.size() - 4)) : "";
  if (failure.empty() && extension != (image ? ".img" : ".zip"))
    failure = "The downloaded file type does not match the selected ZIP or IMG target.";
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256_Final(digest.data(), &hash);
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_transfer.id == transfer.id) g_transfer.upload_finalizing = true;
  }
  if (failure.empty() && fsync(fd) != 0) failure = "Could not flush the download to storage.";
  if (fd >= 0 && close(fd) != 0 && failure.empty()) failure = "Could not finish writing the download.";
  if (failure.empty() && !current()) failure = "Download cancelled. No installation was started.";
  if (failure.empty() && rename(partial.c_str(), transfer.path.c_str()) != 0)
    failure = "Could not finalize the downloaded file.";
  if (failure.empty()) aera::shared_storage::Normalize(transfer.path);
  Json::Value package_info(Json::objectValue);
  if (failure.empty() && !image) package_info = InspectPackage(transfer);
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_transfer.id != transfer.id) { RemoveTransferFiles(transfer); return; }
    if (g_transfer.cancel_requested || !g_running.load() || g_generation.load() != generation)
      failure = "Download cancelled. No installation was started.";
    g_transfer.phase = g_transfer.cancel_requested || !g_running.load() ? "cancelled" : failure.empty() ? "ready" : "failed";
    g_transfer.detail = failure.empty() ? "Downloaded and ready to install." : failure;
    g_transfer.bytes = have_length ? expected : received; g_transfer.received = received;
    g_transfer.name = transfer.name; g_transfer.request.name = transfer.name;
    g_transfer.checksum = Hex(digest.data(), digest.size());
    g_transfer.package_info = std::move(package_info);
    g_transfer.fast_request = std::move(transfer.fast_request);
    g_transfer.payload_request = std::move(transfer.payload_request);
    if (!failure.empty()) RemoveTransferFiles(transfer);
  }
}
void Upload(SSL *tls, const HttpRequest &request, const std::string &owner, unsigned generation) {
  Transfer transfer;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_transfer.existing_file || g_transfer.phase != "prepared" || g_transfer.owner != owner ||
        request.path != "/api/upload/" + g_transfer.id || request.length != g_transfer.bytes) {
      Error(tls, 409, "The upload does not match the prepared transfer."); return;
    }
    transfer = g_transfer; g_transfer.phase = "uploading";
    g_transfer.upload_socket = SSL_get_fd(tls);
    g_transfer.upload_started = g_transfer.upload_sample = g_transfer.upload_last_data = Clock::now();
  }
  const std::string partial = transfer.path + ".part";
  const int fd = open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  bool okay = fd >= 0;
  SHA256_CTX hash{}; SHA256_Init(&hash);
  uint64_t received = 0;
  std::array<char, 65536> buffer{};
  auto consume = [&](const char *bytes, size_t size) {
    {
      std::lock_guard<std::mutex> guard(g_lock);
      if (g_transfer.id != transfer.id || g_transfer.cancel_requested ||
          g_transfer.phase != "uploading" || !g_running.load() || g_generation.load() != generation) return false;
    }
    if (!WriteAll(fd, bytes, size)) return false;
    SHA256_Update(&hash, bytes, size); received += size;
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running.load() || g_generation.load() != generation ||
        g_transfer.id != transfer.id || g_transfer.phase != "uploading") return false;
    g_transfer.received = received;
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - g_transfer.upload_sample).count();
    g_transfer.upload_last_data = now;
    if (elapsed >= .25) {
      const double speed = (received - g_transfer.upload_sample_bytes) / elapsed;
      g_transfer.upload_speed = g_transfer.upload_speed == 0 ? speed :
          .7 * g_transfer.upload_speed + .3 * speed;
      g_transfer.upload_sample = now; g_transfer.upload_sample_bytes = received;
    }
    return true;
  };
  if (okay && !request.prefix.empty()) okay = consume(request.prefix.data(), request.prefix.size());
  while (okay && received < transfer.bytes) {
    const int count = SSL_read(tls, buffer.data(),
        static_cast<int>(std::min<uint64_t>(buffer.size(), transfer.bytes - received)));
    okay = count > 0 && consume(buffer.data(), static_cast<size_t>(count));
  }
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256_Final(digest.data(), &hash);
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_transfer.id == transfer.id) {
      g_transfer.upload_socket = -1;
      g_transfer.upload_finalizing = true;
    }
  }
  std::string failure;
  if (!okay || received != transfer.bytes) failure = "Transfer interrupted. No installation was started.";
  else if (Hex(digest.data(), digest.size()) != transfer.checksum) failure = "SHA-256 mismatch. No installation was started.";
  if (failure.empty() && fsync(fd) != 0) failure = "Could not flush the transferred file to storage.";
  if (fd >= 0 && close(fd) != 0 && failure.empty()) failure = "Could not finish writing the transferred file.";
  Json::Value result;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_transfer.id != transfer.id || g_transfer.phase != "uploading" ||
        g_generation.load() != generation || !g_running.load()) {
      unlink(partial.c_str());
      return;
    }
    if (g_transfer.cancel_requested) failure = "Transfer cancelled. No installation was started.";
    if (failure.empty() && rename(partial.c_str(), transfer.path.c_str()) != 0)
      failure = "Could not finalize the transferred file.";
    if (failure.empty()) aera::shared_storage::Normalize(transfer.path);
    if (failure.empty() && transfer.save_path.empty() && transfer.request.job == aeraui::Job::kInstall)
      g_transfer.detail = "Reading package information...";
  }
  // The existing inspector reads metadata/manifest only; never hold the session
  // lock while reading a ZIP or bounded firmware ARB data from storage.
  Json::Value package_info(Json::objectValue);
  if (failure.empty() && transfer.save_path.empty() && transfer.request.job == aeraui::Job::kInstall)
    package_info = InspectPackage(transfer);
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_transfer.id != transfer.id || g_transfer.phase != "uploading" ||
        g_generation.load() != generation || !g_running.load()) return;
    g_transfer.package_info = std::move(package_info);
    g_transfer.fast_request = std::move(transfer.fast_request);
    g_transfer.payload_request = std::move(transfer.payload_request);
    if (g_transfer.cancel_requested) failure = "Transfer cancelled. No installation was started.";
    g_transfer.phase = g_transfer.cancel_requested ? "cancelled" : failure.empty() ? "ready" : "failed";
    g_transfer.detail = failure;
    if (!failure.empty()) RemoveTransferFiles(transfer);
    result = TransferStatus();
  }
  Reply(tls, failure.empty() ? 200 : 409, JsonText(result));
}
void Handle(SSL *tls, unsigned generation) {
  HttpRequest request;
  if (!ReadHeaders(tls, &request)) { Error(tls, 400, "Invalid HTTP request."); return; }
  if (request.method == "GET" && request.path == "/") {
    const auto page = ReadFile(AERA_PC_CLIENT_PATH, 256 * 1024);
    if (page.empty()) Error(tls, 503, "PC connection interface is not available.");
    else Reply(tls, 200, page, "text/html; charset=utf-8");
    return;
  }
  if (request.method == "GET" && request.path == "/sha256.js") {
    const auto script = ReadFile(std::string(AERA_PC_CLIENT_PATH).substr(0,
        std::string(AERA_PC_CLIENT_PATH).find_last_of('/')) + "/sha256.js", 128 * 1024);
    if (script.empty()) Error(tls, 503, "Checksum library is not available.");
    else Reply(tls, 200, script, "text/javascript; charset=utf-8");
    return;
  }
  if (request.method == "GET" && (request.path == "/workspace.js" || request.path == "/workspace.css" ||
                                  request.path == "/backups.js" || request.path == "/backups.css" ||
                                  request.path == "/terminal.js" || request.path == "/terminal.css")) {
    const auto asset = ReadFile(std::string(AERA_PC_CLIENT_PATH).substr(0,
        std::string(AERA_PC_CLIENT_PATH).find_last_of('/')) + request.path, 128 * 1024);
    if (asset.empty()) Error(tls, 404, "Workspace asset is not available.");
    else Reply(tls, 200, asset, request.path.find(".js") != std::string::npos ? "text/javascript; charset=utf-8" : "text/css; charset=utf-8");
    return;
  }
  if (request.method == "GET" && request.path == "/api/hello") {
    Json::Value info;
    { std::lock_guard<std::mutex> guard(g_lock);
      info["device"] = g_platform["device"]; info["version"] = g_platform["version"];
      info["certificate"] = g_status.certificate; info["persistent_certificate"] = g_status.remember_available;
      info["schema"] = 1; }
    Reply(tls, 200, JsonText(info)); return;
  }
  if (request.method == "POST" && (request.path == "/api/connect" || request.path == "/api/connect/status")) {
    Json::Value body;
    if (!ReadJsonBody(tls, request, &body)) Error(tls, 400, "Invalid connection request.");
    else Connect(tls, body, request.path == "/api/connect/status");
    return;
  }
  const std::string owner = SessionOwner(request);
  if (request.path == "/terminal" || request.path.compare(0, 10, "/terminal/") == 0) {
    const bool websocket = request.path == "/terminal/ws";
    if (request.method != "GET" || request.length != 0 ||
        (request.path != "/terminal" && request.path != "/terminal/" &&
         request.path != "/terminal/token" && !websocket)) {
      Error(tls, 400, "Invalid terminal request."); return;
    }
    if (websocket && (request.upgrade != "websocket" || request.origin.empty() ||
        request.websocket_version != "13" || request.websocket_key.size() != 24 ||
        request.websocket_key.substr(22) != "==" ||
        !std::all_of(request.websocket_key.begin(), request.websocket_key.begin() + 22,
            [](unsigned char c) { return std::isalnum(c) || c == '+' || c == '/'; }) ||
        (!request.websocket_protocol.empty() && request.websocket_protocol != "tty"))) {
      Error(tls, 400, "Invalid terminal WebSocket handshake."); return;
    }
    terminal::Request proxy{request.path, request.websocket_key, request.websocket_protocol, websocket};
    if (!terminal::Proxy(tls, proxy, TerminalCookie(request),
          [generation](const std::string &session) {
            return g_generation.load() == generation && TerminalAuthorized(session);
          }))
      Error(tls, 403, "Open the terminal from an authorized PC connection.");
    return;
  }
  if (request.method == "GET" && request.path.compare(0, 19, "/api/files/content/") == 0) {
    DownloadFile(tls, request.path.substr(19)); return;
  }
  if (owner.empty()) { Error(tls, 401, "Connect and authorize this computer first."); return; }
  if (request.path == "/api/terminal/open" || request.path == "/api/terminal/close") {
    Json::Value body;
    if (request.method != "POST" || !ReadJsonBody(tls, request, &body)) {
      Error(tls, 400, "Invalid terminal command."); return;
    }
    const std::string session = request.authorization.substr(7);
    Json::Value result;
    if (request.path == "/api/terminal/close") {
      if (!terminal::Close(session)) { Error(tls, 403, "This terminal belongs to another computer."); return; }
      result["closed"] = true;
      Reply(tls, 200, JsonText(result), "application/json",
            "Set-Cookie: aera_terminal=; Path=/terminal; Max-Age=0; Secure; HttpOnly; SameSite=Strict\r\n");
      return;
    }
    std::string error;
    const auto cookie = terminal::Open(session, &error);
    if (cookie.empty()) { Error(tls, 503, error); return; }
    if (!TerminalAuthorized(session)) {
      terminal::Close(session);
      Error(tls, 401, "This computer is no longer authorized."); return;
    }
    result["url"] = "/terminal/";
    Reply(tls, 200, JsonText(result), "application/json",
          "Set-Cookie: aera_terminal=" + cookie + "; Path=/terminal; Secure; HttpOnly; SameSite=Strict\r\n");
    return;
  }
  if (request.method == "GET" && request.path == "/api/backups/icons") {
    Json::Value result;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      if (g_transfer.owner != owner) { Error(tls, 403, "This backup belongs to another computer."); return; }
      result["id"] = g_transfer.id;
      result["icons"] = Json::Value(Json::objectValue);
      auto collect = [&](const Json::Value &apps) {
        if (!apps.isArray()) return;
        for (const auto &app : apps)
          if (app["icon"].isString() && !app["icon"].asString().empty())
            result["icons"][app["package"].asString()] = app["icon"];
      };
      collect(g_transfer.backup_result["apps"]);
      const auto &snapshots = g_transfer.backup_result["snapshots"];
      if (snapshots.isArray())
        for (const auto &snapshot : snapshots) collect(snapshot["apps"]);
    }
    Reply(tls, 200, JsonText(result)); return;
  }
  if (request.path == "/api/backups/io" || request.path.compare(0, 16, "/api/backups/io/") == 0) {
    BackupIo(tls, request, owner); return;
  }
  if (request.method == "GET" && request.path == "/api/certificate") {
    std::string certificate;
    { std::lock_guard<std::mutex> guard(g_lock); certificate = g_certificate_pem; }
    Reply(tls, 200, certificate, "application/x-pem-file"); return;
  }
  if (request.method == "GET" && request.path == "/api/log") {
    const auto log = ReadLogTail("/tmp/recovery.log", 4 * 1024 * 1024);
    if (log.empty()) Error(tls, 503, "Recovery log is not available yet.");
    else Reply(tls, 200, log, "text/plain; charset=utf-8");
    return;
  }
  if (request.method == "GET" && (request.path == "/api/log/logcat" || request.path == "/api/log/kernel")) {
    DiagnosticLog(tls, request.path == "/api/log/kernel"); return;
  }
  if (request.method == "GET" && request.path == "/api/log/flashes") {
    FileAccess access;
    if (!access.acquired) { Error(tls, 409, "Recovery is busy."); return; }
    Json::Value result(Json::objectValue); result["reports"] = Json::Value(Json::arrayValue);
    for (const auto &report : aera::flashlog::Reports()) {
      Json::Value entry;
      entry["name"] = report.name; entry["bytes"] = Json::UInt64(report.bytes);
      entry["timestamp"] = Json::Int64(report.timestamp);
      entry["result"] = report.name.find("_SUCCESS.zip") != std::string::npos ? "Success" :
          report.name.find("_FAILED.zip") != std::string::npos ? "Failed" : "Incomplete";
      result["reports"].append(entry);
    }
    Reply(tls, 200, JsonText(result)); return;
  }
  if (request.method == "GET" && request.path.compare(0, 15, "/api/log/flash/") == 0) {
    FileAccess access;
    if (!access.acquired) { Error(tls, 409, "Recovery is busy."); return; }
    uint64_t bytes = 0;
    const auto name = request.path.substr(15);
    const int fd = aera::flashlog::OpenReport(name, &bytes);
    if (fd < 0) Error(tls, 404, "Flash report is no longer available.");
    else SendDownload(tls, fd, bytes, name);
    return;
  }
  if (request.method == "GET" && request.path == "/api/status") {
    keep_connection = request.length == 0;
    Json::Value state;
    { std::lock_guard<std::mutex> guard(g_lock); state = g_platform;
      state["job"] = TransferStatus(); state["job"]["owned"] = g_transfer.owner == owner;
      state["remembered"] = g_trusted.isMember(owner); }
    { std::lock_guard<std::mutex> guard(g_lock);
      state["busy"] = g_recovery_busy || g_file_busy || !g_reboot_target.empty();
      state["certificate"] = g_status.certificate; state["persistent_certificate"] = g_status.remember_available;
      state["address"] = g_status.address; state["ip_address"] = g_status.ip_address;
      state["url_download_available"] = !g_network_ip.empty() && access(DownloadBinary(), X_OK) == 0;
      state["usb_address"] = g_status.usb_address; }
    Reply(tls, 200, JsonText(state)); return;
  }
  if (request.method == "PUT" && request.path.compare(0, 12, "/api/upload/") == 0) {
    Upload(tls, request, owner, generation); return;
  }
  Json::Value body;
  if (request.method != "POST" || !ReadJsonBody(tls, request, &body,
        request.path == "/api/nas" ? 98304 : request.path == "/api/backups" ? 65536 : kMaximumJson)) {
    Error(tls, 400, "Invalid command."); return;
  }
  if (request.path == "/api/prepare") { PrepareTransfer(tls, body, owner); return; }
  if (request.path == "/api/download") { PrepareTransfer(tls, body, owner, true); return; }
  if (request.path == "/api/backups") { QueueBackup(tls, body, owner); return; }
  if (request.path == "/api/nas") { QueueNetworkStorage(tls, body, owner); return; }
  if (request.path == "/api/backups/heartbeat") {
    if (!body["id"].isString()) { Error(tls, 400, "Missing backup operation ID."); return; }
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_transfer.owner != owner || g_transfer.id != body["id"].asString() ||
        g_transfer.request.job != aeraui::Job::kPcBackupOperation ||
        g_transfer.request.backup_source != "computer" || g_transfer.phase != "installing" ||
        !storage::Channel().Heartbeat(owner)) {
      Error(tls, 409, "Computer storage session ended."); return;
    }
    Json::Value result; result["ok"] = true;
    keep_connection = true;
    Reply(tls, 200, JsonText(result)); return;
  }
  if (request.path == "/api/backups/cancel") {
    if (!body["id"].isString()) { Error(tls, 400, "Missing backup operation ID."); return; }
    const auto id = body.get("id", "").asString();
    {
      std::lock_guard<std::mutex> guard(g_lock);
      if (g_transfer.owner != owner || g_transfer.id != id || body.get("confirm", false) != true) {
        Error(tls, 403, "Confirm cancellation of your current backup operation."); return;
      }
    }
    if (!CancelBackup(id, body.get("acknowledge_partial_restore", false) == true)) {
      Error(tls, 409, "Operation ended, or restore cancellation was not confirmed."); return;
    }
    Json::Value result;
    { std::lock_guard<std::mutex> guard(g_lock); result = TransferStatus(); }
    Reply(tls, 200, JsonText(result)); return;
  }
  if (request.path.compare(0, 11, "/api/files/") == 0) { FileCommand(tls, request.path, body, owner); return; }
  Json::Value result;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (request.path == "/api/reboot" || request.path == "/api/root") {
      if (g_recovery_busy || g_file_busy || !TerminalPhase(g_transfer.phase) || !g_reboot_target.empty()) {
        Error(tls, 409, "Finish the current recovery operation or discard its transfer first."); return;
      }
      if (body.get("confirm", false) != true) { Error(tls, 400, "Confirm this operation."); return; }
      if (request.path == "/api/reboot") {
        const std::string target = body.get("target", "").asString();
        if (target != "system" && target != "recovery" && target != "bootloader" &&
            target != "fastbootd" && target != "poweroff") { Error(tls, 400, "Invalid reboot target."); return; }
        g_reboot_target = target; g_reboot_at = Clock::now() + std::chrono::seconds(1);
        result["queued"] = target;
      } else {
        if (!aeraui::root::Enabled()) {
          Error(tls, 409, "Root Manager is disabled in this build."); return;
        }
        const std::string action = body.get("action", "").asString();
        const std::string provider = body.get("provider", "").asString();
        const std::string slot = body.get("slot", "").asString();
        if ((action != "inspect" && action != "refresh" && action != "patch" && action != "rollback" && action != "manager") ||
            (provider != "kernelsu" && provider != "next" && provider != "sukisu") || (slot != "a" && slot != "b")) {
          Error(tls, 400, "Select a root operation, provider and slot."); return;
        }
        if (!g_platform["root_available"].asBool() || g_platform["locked"].asBool()) {
          Error(tls, 409, "Root patching requires supported init_boot partitions and unlocked internal storage."); return;
        }
        RemoveTransferFiles(g_transfer); g_transfer = Transfer{};
        g_transfer.id = RandomToken();
        if (g_transfer.id.empty()) { Error(tls, 500, "Could not create root operation."); return; }
        g_transfer.owner = owner; g_transfer.computer = g_trusted.get(owner, "Computer").asString();
        g_transfer.name = "Root Manager / " + action + " / init_boot_" + slot;
        g_transfer.phase = "queued";
        g_transfer.detail = "Waiting for recovery to start the root operation.";
        g_transfer.request.job = aeraui::Job::kRootOperation;
        g_transfer.request.title = "Root Manager"; g_transfer.request.name = g_transfer.name;
        g_transfer.request.present_before_run = true;
        g_transfer.request.show_on_device = false;
        g_transfer.request.root_action = action; g_transfer.request.root_provider = provider;
        g_transfer.request.root_slot = slot;
        g_prompt = Json::Value(Json::objectValue);
        g_presentation = Json::Value(Json::objectValue); result = TransferStatus();
      }
    } else if (request.path == "/api/forget") {
      if (g_transfer.request.job == aeraui::Job::kPcBackupOperation && !TerminalPhase(g_transfer.phase)) {
        Error(tls, 409, "Finish the backup operation before forgetting this browser."); return;
      }
      Json::Value removed; g_trusted.removeMember(owner, &removed);
      if (g_status.remember_available && !SaveTrusted()) {
        if (!removed.isNull()) g_trusted[owner] = removed;
        Error(tls, 409, "Could not update remembered computers."); return;
      }
      for (auto i = g_sessions.begin(); i != g_sessions.end();) {
        if (i->second == owner) i = g_sessions.erase(i); else ++i;
      }
      result["forgotten"] = true;
    } else if (g_transfer.id.empty() || !body["id"].isString() ||
               g_transfer.id != body["id"].asString() || g_transfer.owner != owner) {
      Error(tls, 403, "This computer does not own the requested transfer."); return;
    } else if (request.path == "/api/install") {
      if (g_transfer.phase != "ready" || body.get("confirm", false) != true) {
        Error(tls, 409, "Transfer and verify the file, then confirm installation."); return;
      }
      if (!g_transfer.save_path.empty()) {
        if (g_recovery_busy || g_file_busy) { Error(tls, 409, "Recovery is busy."); return; }
        char parent[4096];
        const auto directory = g_transfer.save_path.substr(0, g_transfer.save_path.find_last_of('/'));
        struct stat payload{}, folder{};
        if (!realpath(directory.c_str(), parent) || directory != parent ||
            !ChildOf(directory, g_transfer.root) || stat(directory.c_str(), &folder) != 0 ||
            !S_ISDIR(folder.st_mode) || lstat(g_transfer.path.c_str(), &payload) != 0 ||
            !S_ISREG(payload.st_mode) || uint64_t(payload.st_size) != g_transfer.bytes) {
          Error(tls, 409, "Could not save without overwriting an existing file. The verified transfer is still available."); return;
        }
        if (!SetPublicStorageMetadata(g_transfer.path, directory)) {
          Error(tls, 409, "Could not set Android storage permissions. The verified transfer is still available."); return;
        }
        if (!MoveNoReplace(g_transfer.path, g_transfer.save_path)) {
          Error(tls, 409, "Could not save without overwriting an existing file. The verified transfer is still available."); return;
        }
        RemoveTransferFiles(g_transfer);
        g_transfer.phase = "completed"; g_transfer.result = 0; g_transfer.progress = 100;
        g_transfer.detail = "Saved to " + g_transfer.save_path;
        result = TransferStatus();
      } else {
      if (!g_transfer.package_info.get("error", "").asString().empty()) {
        Error(tls, 409, g_transfer.package_info["error"].asString()); return;
      }
      const std::string arb = g_transfer.package_info.get("arb_decision", "").asString();
      if (body.isMember("install_method") && !body["install_method"].isString()) {
        Error(tls, 400, "Invalid installation method."); return;
      }
      const std::string method = body.get("install_method", "normal").asString();
      if (method != "normal" && method != "fast" && method != "selected" && method != "direct" && method != "extract") {
        Error(tls, 400, "Select a supported installation or extraction method."); return;
      }
      if (method != "extract" && arb == "downgrade") {
        Error(tls, 409, "Installation blocked: this package lowers the firmware ARB index and can brick the device."); return;
      }
      if (method != "extract" && arb == "upgrade" && body.get("arb_acknowledged", false) != true) {
        Error(tls, 409, "Acknowledge the firmware anti-rollback upgrade before installing."); return;
      }
      if (method == "fast") {
        if (!g_transfer.package_info.get("fast_available", false).asBool() ||
            g_transfer.fast_request.job != aeraui::Job::kFlashPayload) {
          Error(tls, 409, "Experimental fast flash is unavailable for this package."); return;
        }
        if (body.get("fast_acknowledged", false) != true) {
          Error(tls, 409, "Acknowledge the experimental current-slot flash before continuing."); return;
        }
        g_transfer.request = g_transfer.fast_request;
        g_transfer.request.payload_arb_acknowledged = body.get("arb_acknowledged", false).asBool();
      }
      if (method == "selected" || method == "direct" || method == "extract") {
        const bool flash = method != "extract";
        if (!g_transfer.package_info.get("advanced_available", false).asBool() ||
            !body["partitions"].isArray() || body["partitions"].empty() ||
            body["partitions"].size() > g_transfer.package_info["partitions"].size()) {
          Error(tls, 409, "Select supported images from a full standalone payload."); return;
        }
        if (flash && body.get("fast_acknowledged", false) != true) {
          Error(tls, 409, "Acknowledge the selected current-slot flash before continuing."); return;
        }
        if (method == "direct" && !g_transfer.package_info.get("direct_available", false).asBool()) {
          Error(tls, 409, "The bundled otaripper engine is unavailable."); return;
        }
        std::set<std::string> selected;
        for (const auto &name : body["partitions"]) {
          if (!name.isString() || !selected.insert(name.asString()).second) {
            Error(tls, 400, "Invalid or duplicate partition selection."); return;
          }
          bool available = false;
          for (const auto &partition : g_transfer.package_info["partitions"])
            if (partition["name"] == name)
              available = partition.get(flash ? "flash_available" : "extractable", false).asBool();
          if (!available) { Error(tls, 409, "Selected image is protected or unavailable for this operation."); return; }
        }
        g_transfer.request = g_transfer.payload_request;
        g_transfer.request.job = flash ? aeraui::Job::kFlashPayload : aeraui::Job::kExtractPayload;
        g_transfer.request.title = method == "direct" ? "Direct flash selected partitions" :
            flash ? "Flash selected partitions" : "Extract selected images";
        g_transfer.request.partitions.assign(selected.begin(), selected.end());
        g_transfer.request.payload_direct = method == "direct";
        g_transfer.request.payload_override_protection = flash && std::any_of(
            g_transfer.package_info["partitions"].begin(), g_transfer.package_info["partitions"].end(),
            [&](const auto &partition) { return partition.get("protected", false).asBool() &&
                selected.count(partition["name"].asString()); });
        g_transfer.request.payload_arb_acknowledged = body.get("arb_acknowledged", false).asBool();
      }
      g_transfer.phase = "queued"; g_transfer.shown = false;
      g_transfer.detail = "Waiting for recovery to finish its current operation.";
      result = TransferStatus();
      }
    } else if (request.path == "/api/cancel") {
      if (!CancelTransferLocked(g_transfer.id)) { Error(tls, 409, "This operation cannot be cancelled."); return; }
      result = TransferStatus();
    } else if (request.path == "/api/prompt") {
      if (g_transfer.phase != "installing" || !body["prompt"].isString() ||
          g_prompt["id"] != body["prompt"] || !body["accepted"].isBool() ||
          !g_prompt_answer_id.empty()) { Error(tls, 409, "Installer question is no longer active."); return; }
      g_prompt_answer_id = body["prompt"].asString(); g_prompt_answer = body["accepted"].asBool();
      result["accepted"] = true;
    } else { Error(tls, 404, "Unknown command."); return; }
  }
  Reply(tls, 200, JsonText(result));
}

int Listener(int port, const std::string &ip) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  const int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in address{}; address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) { close(fd); return -1; }
  if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 8) != 0) {
    close(fd); return -1;
  }
  return fd;
}
void Serve(int listener, unsigned generation) {
  while (g_running.load() && g_generation.load() == generation) {
    const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) { if (errno == EINTR) continue; break; }
    TlsContext context;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      if (g_connections.size() >= 8 || !g_running.load()) { close(fd); continue; }
      sockaddr_in destination{}; socklen_t length = sizeof(destination);
      if (getsockname(fd, reinterpret_cast<sockaddr *>(&destination), &length) != 0) {
        close(fd); continue;
      }
      char address[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &destination.sin_addr, address, sizeof(address));
      // Only loopback (ADB forwarding) and the current connected WLAN address.
      if ((ntohl(destination.sin_addr.s_addr) >> 24) != 127 && g_network_ip != address) {
        close(fd); continue;
      }
      context = g_tls_context;
      g_connections.insert(fd);
    }
    std::thread([fd, context, generation] {
      // A browser closing its socket must not terminate the recovery process.
      sigset_t blocked; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
      pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
      timeval timeout{30, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      const int no_delay = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
      std::unique_ptr<SSL, decltype(&SSL_free)> tls(SSL_new(context.get()), SSL_free);
      if (tls && SSL_set_fd(tls.get(), fd) == 1 && SSL_accept(tls.get()) == 1 &&
          g_running.load() && g_generation.load() == generation) {
        do {
          pending_error_status = 0; pending_error.clear(); keep_connection = false;
          timeout.tv_sec = 30;
          setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
          Handle(tls.get(), generation);
          if (pending_error_status) {
            keep_connection = false;
            Json::Value error; error["error"] = pending_error;
            Reply(tls.get(), pending_error_status, JsonText(error));
          }
          if (!keep_connection) break;
          // Reuse fully consumed storage/status/heartbeat requests. Restore
          // the body timeout above after this shorter idle wait; every request
          // still passes the normal authorization checks.
          timeout.tv_sec = 5;
          setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
          char next;
          if (SSL_peek(tls.get(), &next, 1) <= 0) break;
        } while (g_running.load() && g_generation.load() == generation);
      }
      {
        std::lock_guard<std::mutex> guard(g_lock);
        g_connections.erase(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
      }
    }).detach();
  }
}
void RedirectHttp(int listener, unsigned generation) {
  while (g_running.load() && g_generation.load() == generation) {
    const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) { if (errno == EINTR) continue; break; }
    timeval timeout{2, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char buffer[2048];
    const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    if (received > 0) {
      std::string host;
      std::istringstream headers(std::string(buffer, static_cast<size_t>(received)));
      std::string line;
      while (std::getline(headers, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos && Lower(Trim(line.substr(0, colon))) == "host")
          host = Trim(line.substr(colon + 1));
      }
      const std::string location = HttpRedirectLocation(host);
      const std::string reply = "HTTP/1.1 302 Found\r\nLocation: " + location +
          "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
      send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
    }
    close(fd);
  }
}
struct MdnsState {
  std::string name = "aera.local.";
  sockaddr_in address{};
  bool probing = true, conflict = false;
};
int MdnsRecord(int socket, const sockaddr *from, size_t address_size,
    mdns_entry_type_t entry, uint16_t query_id, uint16_t type, uint16_t rclass,
    uint32_t, const void *data, size_t size, size_t name_offset, size_t,
    size_t record_offset, size_t record_length, void *user) {
  auto *state = static_cast<MdnsState *>(user);
  char name_buffer[256]; size_t offset = name_offset;
  const auto extracted = mdns_string_extract(data, size, &offset, name_buffer, sizeof(name_buffer));
  if (std::string(extracted.str, extracted.length) != state->name) return 0;
  if (entry != MDNS_ENTRYTYPE_QUESTION && type == MDNS_RECORDTYPE_A) {
    sockaddr_in existing{};
    if (mdns_record_parse_a(data, size, record_offset, record_length, &existing) &&
        existing.sin_addr.s_addr != state->address.sin_addr.s_addr) state->conflict = true;
  }
  if (state->probing || state->conflict || entry != MDNS_ENTRYTYPE_QUESTION ||
      (type != MDNS_RECORDTYPE_A && type != MDNS_RECORDTYPE_ANY)) return 0;
  mdns_record_t answer{}; answer.name = {state->name.data(), state->name.size()};
  answer.type = MDNS_RECORDTYPE_A; answer.data.a.addr = state->address;
  answer.rclass = 0x8001; answer.ttl = 120;
  char response[1024];
  if (rclass & MDNS_UNICAST_RESPONSE)
    mdns_query_answer_unicast(socket, from, address_size, response, sizeof(response),
        query_id, static_cast<mdns_record_type_t>(type), extracted.str, extracted.length,
        answer, nullptr, 0, nullptr, 0);
  else mdns_query_answer_multicast(socket, response, sizeof(response), answer, nullptr, 0, nullptr, 0);
  return 0;
}
[[maybe_unused]] void Mdns(unsigned generation, unsigned network_generation,
    const std::string &ip, const std::string &device, int port) {
  const auto active = [&] { return g_running.load() && g_generation.load() == generation &&
      g_network_generation.load() == network_generation; };
  MdnsState state; state.address.sin_family = AF_INET;
  if (inet_pton(AF_INET, ip.c_str(), &state.address.sin_addr) != 1) return;
  sockaddr_in bind_address{}; bind_address.sin_family = AF_INET;
  bind_address.sin_port = htons(MDNS_PORT);
  const int fd = mdns_socket_open_ipv4(&bind_address);
  if (fd < 0) return;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &state.address.sin_addr, sizeof(state.address.sin_addr));
  std::array<char, 2048> buffer{};
  auto receive = [&](int milliseconds) {
    pollfd input{fd, POLLIN, 0};
    if (poll(&input, 1, milliseconds) > 0 && (input.revents & POLLIN))
      mdns_socket_listen(fd, buffer.data(), buffer.size(), MdnsRecord, &state);
  };
  const std::string suffix = MdnsSuffix(device, ip);
  unsigned conflicts = 0;
  auto next_name = [&] {
    ++conflicts;
    state.name = "aera-" + suffix + (conflicts > 1 ? "-" + std::to_string(conflicts) : "") + ".local.";
  };
  auto announce = [&](bool goodbye) {
    mdns_record_t answer{}; answer.name = {state.name.data(), state.name.size()};
    answer.type = MDNS_RECORDTYPE_A; answer.data.a.addr = state.address;
    answer.rclass = 0x8001; answer.ttl = 120;
    if (goodbye) mdns_goodbye_multicast(fd, buffer.data(), buffer.size(), answer, nullptr, 0, nullptr, 0);
    else mdns_announce_multicast(fd, buffer.data(), buffer.size(), answer, nullptr, 0, nullptr, 0);
  };
  while (active()) {
    if (state.probing) {
      state.conflict = false;
      for (int i = 0; i < 3 && active(); ++i) {
        mdns_query_send(fd, MDNS_RECORDTYPE_A, state.name.data(), state.name.size(),
                        buffer.data(), buffer.size(), 0);
        const auto until = Clock::now() + std::chrono::milliseconds(300);
        while (Clock::now() < until && !state.conflict && active()) receive(50);
      }
      if (!active()) break;
      if (state.conflict) { next_name(); continue; }
      state.probing = false;
      {
        std::lock_guard<std::mutex> guard(g_lock);
        g_hostname = state.name.substr(0, state.name.size() - 1);
        g_status.address = "https://" + g_hostname + (port == 443 ? "" : ":" + std::to_string(port));
      }
      announce(false);
    }
    receive(250);
    if (state.conflict) { announce(true); next_name(); state.probing = true; }
  }
  announce(true); mdns_socket_close(fd);
}

void StopNetwork() {
  g_network_generation.fetch_add(1);
  if (g_http_listener >= 0) {
    shutdown(g_http_listener, SHUT_RDWR); close(g_http_listener); g_http_listener = -1;
  }
  if (g_http_thread.joinable()) g_http_thread.join();
  if (g_mdns_thread.joinable()) g_mdns_thread.join();
}
void RefreshNetworkLocked() {
  if (!g_running.load()) return;
  const std::string ip = aeraui::RecoveryWifiConnection().connected ? remote::Address() : "";
  bool persistent;
  std::string device;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (ip == g_network_ip) return;
    persistent = g_status.remember_available;
    device = g_platform["device"].asString();
  }
  TlsContext context;
  if (!ip.empty()) {
    std::string fingerprint, public_certificate;
    context = MakeTlsContext(persistent, ip, &fingerprint, &public_certificate);
    if (!context) return;
  }
  StopNetwork();
  {
    std::lock_guard<std::mutex> guard(g_lock);
    g_network_ip = ip;
    if (context) g_tls_context = context;
    g_status.ip_address = ip.empty() ? "" : "https://" + ip + (g_port == 443 ? "" : ":" + std::to_string(g_port));
    g_status.address = ip.empty() ? g_status.usb_address : g_status.ip_address;
    g_hostname = ip.empty() ? "localhost" : ip;
  }
  if (ip.empty()) return;
  const unsigned generation = g_generation.load();
  if (g_port == 443 && !remote::Running()) {
    g_http_listener = Listener(80, ip);
    if (g_http_listener >= 0) g_http_thread = std::thread(RedirectHttp, g_http_listener, generation);
  }
#ifndef AERA_PC_HOST_TEST
  g_mdns_thread = std::thread(Mdns, generation, g_network_generation.load(), ip, device, g_port);
#endif
}

}  // namespace

void RefreshNetwork() {
  std::lock_guard<std::mutex> lifecycle(g_lifecycle);
  RefreshNetworkLocked();
}
bool SetEnabled(bool enabled, int port, bool manual) {
  std::lock_guard<std::mutex> lifecycle(g_lifecycle);
  if (manual) { std::lock_guard<std::mutex> guard(g_lock); g_status.manually_disabled = !enabled; }
  if (enabled) {
    if (g_running.load()) return true;
    if (port < 1 || port > 65535) return false;
    RefreshPlatformInfo();
    bool persistent = false;
#ifdef AERA_PC_HOST_TEST
    persistent = MakeDirectories(AERA_PC_PRIVATE_DIRECTORY);
#else
    struct statfs data_filesystem{};
    persistent = !aeraui::RecoveryDataLocked() && statfs("/data", &data_filesystem) == 0 &&
        data_filesystem.f_type != 0x01021994 && data_filesystem.f_type != 0x858458f6 &&
        MakeDirectories(AERA_PC_PRIVATE_DIRECTORY);
#endif
    std::string certificate, public_certificate;
    g_authority_key.reset(); g_authority_certificate.reset();
    auto context = MakeTlsContext(persistent, "", &certificate, &public_certificate);
    if (!context) return false;
    const int listener = Listener(port, "0.0.0.0");
    if (listener < 0) return false;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      g_trusted = Json::Value(Json::objectValue);
      if (persistent) {
        Json::Value saved;
        if (ParseJson(ReadFile(std::string(AERA_PC_PRIVATE_DIRECTORY) + "/computers.json", 16384), &saved)) {
          for (const auto &id : saved.getMemberNames())
            if (IsToken(id) && saved[id].isString() && saved[id].asString().size() <= 80)
              g_trusted[id] = saved[id];
        }
      }
      g_status = Status{};
      g_status.enabled = true; g_status.remember_available = persistent;
      g_status.certificate = certificate;
      g_certificate_pem = public_certificate;
      g_status.usb_address = "https://localhost:" + std::to_string(port == 443 ? 8443 : port);
      g_status.address = g_status.usb_address;
      g_hostname = "localhost"; g_network_ip.clear();
      g_tls_context = context; g_pair = Pair{}; g_sessions.clear();
      g_downloads.clear(); g_reboot_target.clear();
    }
    g_listener = listener;
    g_port = port;
    g_running.store(true);
    const unsigned generation = g_generation.fetch_add(1) + 1;
    g_accept_thread = std::thread(Serve, listener, generation);
    RefreshNetworkLocked();
    return true;
  }
  if (!g_running.exchange(false)) return true;
  terminal::Stop();
  storage::Channel().Stop();
  g_generation.fetch_add(1);
  {
    std::lock_guard<std::mutex> guard(g_download_lock);
    if (g_download_thread.joinable()) g_download_thread.join();
  }
  StopNetwork();
  for (int *listener : {&g_listener, &g_http_listener}) {
    if (*listener >= 0) { shutdown(*listener, SHUT_RDWR); close(*listener); *listener = -1; }
  }
  if (g_accept_thread.joinable()) g_accept_thread.join();
  if (g_http_thread.joinable()) g_http_thread.join();
  if (g_mdns_thread.joinable()) g_mdns_thread.join();
  {
    std::lock_guard<std::mutex> guard(g_lock);
    for (int connection : g_connections) shutdown(connection, SHUT_RDWR);
    g_status.enabled = false; g_pair = Pair{}; g_sessions.clear();
    g_network_ip.clear(); g_tls_context.reset();
    g_downloads.clear(); g_reboot_target.clear();
    if (g_transfer.phase != "installing" && g_transfer.phase != "queued") {
      RemoveTransferFiles(g_transfer);
      if (!TerminalPhase(g_transfer.phase)) {
        g_transfer.phase = "cancelled";
        g_transfer.detail = "PC connection was disabled. No installation was started.";
      }
    }
  }
  return true;
}
void ReleaseHttpRedirect() {
  std::lock_guard<std::mutex> lifecycle(g_lifecycle);
  if (g_http_listener >= 0) {
    shutdown(g_http_listener, SHUT_RDWR); close(g_http_listener); g_http_listener = -1;
  }
  if (g_http_thread.joinable()) g_http_thread.join();
}
void RestoreHttpRedirect() {
  std::lock_guard<std::mutex> lifecycle(g_lifecycle);
  if (!g_running.load() || g_http_listener >= 0 || remote::Running()) return;
  std::string ip;
  { std::lock_guard<std::mutex> guard(g_lock); ip = g_status.ip_address; }
  if (ip.compare(0, 8, "https://") != 0 || ip.find(':', 8) != std::string::npos) return;
  g_http_listener = Listener(80, ip.substr(8));
  if (g_http_listener >= 0) {
    if (g_http_thread.joinable()) g_http_thread.join();
    g_http_thread = std::thread(RedirectHttp, g_http_listener, g_generation.load());
  }
}
std::string HttpRedirectLocation(const std::string &host) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (!g_status.enabled) return {};
  std::string name = Lower(host);
  if (name.size() > 3 && name.compare(name.size() - 3, 3, ":80") == 0) name.resize(name.size() - 3);
  return (name == g_hostname ? g_status.address : g_status.ip_address) + "/";
}
Status GetStatus() {
  std::lock_guard<std::mutex> guard(g_lock);
  Status status = g_status;
  if (!g_pair.id.empty() && !g_pair.allowed && !g_pair.denied && Clock::now() < g_pair.expires)
    status.pending_connection = g_pair.id;
  return status;
}
UploadProgress GetUploadProgress() {
  std::lock_guard<std::mutex> guard(g_lock);
  return UploadProgressLocked();
}
bool CancelUpload(const std::string &id) {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_transfer.phase == "uploading" && CancelTransferLocked(id);
}
bool NeedsUiAttention() {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_transfer.phase == "queued" || g_transfer.phase == "installing" ||
      !g_reboot_target.empty() ||
      (g_running.load() && !g_pair.id.empty() && !g_pair.shown && !g_pair.allowed &&
       !g_pair.denied && Clock::now() < g_pair.expires);
}
void RefreshPlatformInfo() {
  Json::Value info;
  info["terminal_available"] = terminal::Available();
  info["app_vault_available"] = aeraui::AppVaultInstalled();
  const auto nas = aeraui::RecoveryNasStatus();
  auto &network = info["nas"];
  network["supported"] = nas.supported; network["mounted"] = nas.mounted;
  network["status"] = nas.status; network["error"] = nas.error;
  network["type"] = nas.config.type; network["host"] = nas.config.host;
  network["port"] = nas.config.port; network["user"] = nas.config.user;
  network["share"] = nas.config.share; network["path"] = nas.config.path;
  network["domain"] = nas.config.domain; network["cache_mode"] = nas.config.cache_mode;
  network["auth"] = nas.config.auth; network["key_name"] = nas.config.key_name;
  network["key_available"] = nas.key_available;
  network["password_saved"] = !nas.config.password.empty();
  for (const auto *protocol : {"smb", "sftp"}) {
    const auto profile = aeraui::RecoverySavedNasConfig(protocol);
    auto &item = network["profiles"][protocol];
    item["type"] = profile.type; item["host"] = profile.host;
    item["port"] = profile.port; item["user"] = profile.user;
    item["share"] = profile.share; item["path"] = profile.path;
    item["domain"] = profile.domain; item["cache_mode"] = profile.cache_mode;
    item["auth"] = profile.auth; item["key_name"] = profile.key_name;
    item["password_saved"] = !profile.password.empty();
    item["key_available"] = nas.key_available;
  }
  info["app_vault_storage"] = aeraui::RecoveryAppVaultStorage();
  info["backup_storages"] = Json::Value(Json::arrayValue);
  for (const auto &target : aeraui::RecoveryBackupStorages()) {
    Json::Value item;
    item["name"] = target.name; item["path"] = target.path;
    item["mounted"] = target.mounted; item["free"] = Json::UInt64(target.free_bytes);
    info["backup_storages"].append(item);
  }
  info["backup_block_bytes"] = Json::UInt64(storage::kBlock);
  info["backup_partitions"] = Json::Value(Json::arrayValue);
  for (const auto &volume : aeraui::RecoveryVolumes("backup")) {
    Json::Value item; item["path"] = volume.path; item["name"] = volume.name;
    item["bytes"] = Json::UInt64(volume.bytes); info["backup_partitions"].append(item);
  }
  info["users"] = Json::Value(Json::arrayValue);
  for (const auto &user : aeraui::RecoveryAndroidUsers()) {
    if (!user.decrypted) continue;
    Json::Value item; item["id"] = user.id; item["name"] = user.name;
    info["users"].append(item);
  }
  info["device"] = aeraui::RecoveryDevice(); info["version"] = aeraui::RecoveryVersion();
  info["slot"] = aeraui::RecoverySlot(); info["locked"] = aeraui::RecoveryDataLocked();
  info["boot_slot"] = aeraui::RecoveryBootSlot();
  info["extraction_root"] = aeraui::RecoveryStorage() + "/AERA/Extracted";
  info["preserve_abl"] = aeraui::RecoveryAblPreservationSupported() &&
      aeraui::RecoveryPreference(aeraui::Preference::kPreserveAbl);
  info["preserve_recovery"] = aeraui::RecoveryPreservationSupported() &&
      aeraui::RecoveryPreference(aeraui::Preference::kPreserveRecovery);
  utsname kernel{};
  if (uname(&kernel) == 0) { info["kernel"] = kernel.release; info["architecture"] = kernel.machine; }
  info["memory"] = ReadFile("/proc/meminfo", 8192);
  info["uptime"] = ReadFile("/proc/uptime", 256);
  info["battery"] = Trim(ReadFile("/sys/class/power_supply/battery/capacity", 64));
  info["battery_status"] = Trim(ReadFile("/sys/class/power_supply/battery/status", 64));
  info["root_enabled"] = aeraui::root::Enabled();
  info["root_available"] = aeraui::root::Enabled() && access("/sbin/ksud", X_OK) == 0 && access("/sbin/magiskboot", X_OK) == 0 &&
      (access("/dev/block/by-name/init_boot_a", R_OK | W_OK) == 0 ||
       access("/dev/block/by-name/init_boot_b", R_OK | W_OK) == 0);
#ifdef AERA_PC_HOST_TEST
  info["root_available"] = aeraui::root::Enabled();  // The host fixture only simulates root jobs.
#endif
  info["storages"] = Json::Value(Json::arrayValue);
  std::set<std::string> roots;
  auto add_storage = [&](const std::string &name, const std::string &path) {
#ifndef AERA_PC_HOST_TEST
    if (!aeraui::RecoveryBackupPathReady(path)) return;
#endif
    char canonical[4096]; struct statvfs filesystem{}; struct statfs type{};
    if (!realpath(path.c_str(), canonical) || !strcmp(canonical, "/") ||
        statvfs(canonical, &filesystem) != 0 || (filesystem.f_flag & ST_RDONLY) ||
        statfs(canonical, &type) != 0 || type.f_type == 0x01021994 || type.f_type == 0x858458f6 ||
        !roots.insert(canonical).second) return;
    Json::Value storage; storage["name"] = name; storage["path"] = canonical;
    storage["free"] = Json::UInt64(uint64_t(filesystem.f_bavail) * filesystem.f_frsize);
    info["storages"].append(storage);
  };
  info["transfer_storage"] = Json::Value();
  if (!aeraui::RecoveryDataLocked()) {
    const auto internal = aeraui::RecoveryInternalStorage();
    if (!internal.empty()) {
      add_storage("Internal storage", internal);
      char canonical[4096];
      if (realpath(internal.c_str(), canonical)) {
        for (const auto &storage : info["storages"])
          if (storage["path"].asString() == canonical) info["transfer_storage"] = storage;
      }
    }
  }
  for (const auto &volume : aeraui::RecoveryVolumes("storage")) {
    if (aeraui::RecoveryDataLocked() && (volume.path == "/data" || volume.path == "/sdcard")) continue;
#ifndef AERA_PC_HOST_TEST
    if (!aeraui::RecoveryBackupPathReady(volume.path)) continue;
#endif
    add_storage(volume.name, volume.path);
  }
  info["partitions"] = Json::Value(Json::arrayValue);
  std::vector<std::string> payload_names;
  for (const auto &volume : aeraui::RecoveryImageVolumes()) {
    Json::Value partition; partition["name"] = volume.name; partition["path"] = volume.path;
    partition["logical"] = volume.logical; partition["bytes"] = Json::UInt64(volume.bytes);
    partition["slot_select"] = volume.slot_select;
    info["partitions"].append(partition);
    const auto name = volume.path.substr(volume.path.find_last_of('/') + 1);
    payload_names.push_back(name == "system_root" ? "system" : name);
  }
  // PartitionManager snapshots belong to the UI thread, never HTTP workers.
  info["payload_targets"] = Json::Value(Json::arrayValue);
  for (const auto &target : aeraui::RecoveryPayloadTargets(payload_names)) {
    Json::Value value;
    value["name"] = target.name; value["path"] = target.path;
    value["bytes"] = Json::UInt64(target.bytes); value["raw"] = target.raw;
    value["device"] = Json::UInt64(target.device);
    info["payload_targets"].append(value);
  }
  const auto snapshots = aeraui::RecoverySnapshotCowStatus();
  info["snapshots_safe"] = !snapshots.supported || (snapshots.metadata_readable && snapshots.safe_to_remove);
  std::lock_guard<std::mutex> guard(g_lock); g_platform = std::move(info);
}
void CleanupOldTransfers() {
  const auto boot = BootId();
  if (boot.empty()) return;
  const bool locked = aeraui::RecoveryDataLocked();
  std::set<std::string> roots;
  auto add = [&](const std::string &path) {
    char canonical[4096]; struct statvfs filesystem{}; struct statfs type{};
    if (realpath(path.c_str(), canonical) && strcmp(canonical, "/") &&
        (!locked || (strcmp(canonical, "/data") && strncmp(canonical, "/data/", 6))) &&
        statvfs(canonical, &filesystem) == 0 && !(filesystem.f_flag & ST_RDONLY) &&
        statfs(canonical, &type) == 0 && type.f_type != 0x01021994 && type.f_type != 0x858458f6)
      roots.insert(canonical);
  };
  if (!locked) add(aeraui::RecoveryStorage());
  for (const auto &volume : aeraui::RecoveryVolumes("storage")) {
    if (locked && (volume.path == "/data" || volume.path == "/sdcard")) continue;
    add(volume.path);
  }
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_recovery_busy || g_file_busy || !TerminalPhase(g_transfer.phase) || !g_reboot_target.empty()) return;
  for (const auto &root : roots) CleanupTransferRoot(root, boot);
}
bool TakeConnectionRequest(ConnectionRequest *request) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (!request || !g_running.load() || g_pair.id.empty() || g_pair.shown ||
      g_pair.allowed || g_pair.denied || Clock::now() >= g_pair.expires) return false;
  *request = {g_pair.id, g_pair.name, g_pair.fingerprint, g_status.remember_available};
  g_pair.shown = true; return true;
}
void ResolveConnection(const std::string &id, bool allow, bool remember) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (!g_running.load() || g_pair.id != id || g_pair.allowed || g_pair.denied) return;
  if (!allow || Clock::now() >= g_pair.expires) { g_pair.denied = true; return; }
  g_pair.token = RandomToken();
  if (g_pair.token.empty()) { g_pair.denied = true; return; }
  if (remember && g_status.remember_available) {
    g_trusted[g_pair.fingerprint] = g_pair.name;
    if (!SaveTrusted()) g_trusted.removeMember(g_pair.fingerprint);
  }
  if (g_sessions.size() >= 8) g_sessions.clear();
  g_sessions[g_pair.token] = g_pair.fingerprint; g_pair.allowed = true;
}
bool TakeInstallRequest(InstallRequest *request) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (!request || g_transfer.phase != "queued" || g_transfer.shown || g_file_busy || g_recovery_busy) return false;
  *request = {g_transfer.id, g_transfer.name, g_transfer.computer, g_transfer.bytes, g_transfer.request};
  if (g_transfer.request.job == aeraui::Job::kNetworkStorage) {
    g_transfer.request.nas_private_key.clear();
    g_transfer.request.nas_config.password.clear();
  }
  g_transfer.shown = true; return true;
}
bool BeginInstall(const std::string &id) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.id != id || g_transfer.phase != "queued") return false;
  struct stat info{};
  if (g_transfer.request.job != aeraui::Job::kRootOperation &&
      g_transfer.request.job != aeraui::Job::kNetworkStorage &&
      g_transfer.request.job != aeraui::Job::kPcBackupOperation &&
      (lstat(g_transfer.path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
      static_cast<uint64_t>(info.st_size) != g_transfer.bytes)) {
    g_transfer.phase = "failed"; g_transfer.detail = "The verified file is no longer available."; return false;
  }
  if (g_transfer.existing_file && (info.st_dev != g_transfer.source.st_dev ||
      info.st_ino != g_transfer.source.st_ino || info.st_mtim.tv_sec != g_transfer.source.st_mtim.tv_sec ||
      info.st_mtim.tv_nsec != g_transfer.source.st_mtim.tv_nsec ||
      info.st_ctim.tv_sec != g_transfer.source.st_ctim.tv_sec ||
      info.st_ctim.tv_nsec != g_transfer.source.st_ctim.tv_nsec)) {
    g_transfer.phase = "failed"; g_transfer.detail = "The selected file changed. Select it again before installing.";
    return false;
  }
  g_transfer.phase = "installing";
  g_backup_active.store(g_transfer.request.job == aeraui::Job::kPcBackupOperation &&
      (g_transfer.request.backup_action == "backup" || g_transfer.request.backup_action == "restore"));
  g_transfer.detail = g_transfer.request.job == aeraui::Job::kPcBackupOperation ? "Starting backup operation" : "Starting installation";
  g_transfer.progress = 0; return true;
}
void RejectInstall(const std::string &id) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.id != id || g_transfer.phase != "queued") return;
  g_transfer.phase = "cancelled"; RemoveTransferFiles(g_transfer);
  if (g_transfer.request.job == aeraui::Job::kPcBackupOperation) storage::Channel().Stop();
}
void UpdateInstall(int percent, const std::string &detail, const std::string &log) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.phase != "installing") return;
  g_transfer.progress = std::clamp(percent, 0, 100);
  g_transfer.detail = detail;
  g_transfer.log = log.size() > 16384 ? log.substr(log.size() - 16384) : log;
}
void UpdateRootInspection(const aeraui::root::Progress &progress) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.phase != "installing" || g_transfer.request.root_action != "inspect" ||
      !progress.has_inspection) return;
  auto &inspection = g_transfer.root_inspection;
  inspection["target"] = "init_boot_" + progress.inspected_target.slot;
  inspection["kernel"] = progress.inspected_target.kmi;
  inspection["inspected"] = progress.inspected_patch.inspected;
  inspection["patched"] = progress.inspected_patch.patched;
  inspection["verified"] = progress.inspected_patch.aera_verified;
  inspection["provider"] = progress.inspected_patch.provider;
  inspection["version"] = progress.inspected_patch.version;
  inspection["detail"] = progress.inspected_patch.detail;
  inspection["offline_available"] = progress.inspected_offline.available;
  inspection["offline_provider"] = g_transfer.request.root_provider;
  inspection["offline_version"] = progress.inspected_offline.version;
}
void UpdateBackupResult(const Json::Value &result) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.phase == "installing" && g_transfer.request.job == aeraui::Job::kPcBackupOperation)
    g_transfer.backup_result = result;
}
bool BackupOperationActive() { return g_backup_active.load(); }
BackupTransferStatistics GetBackupTransferStatistics() {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.request.job != aeraui::Job::kPcBackupOperation ||
      g_transfer.request.backup_source != "computer" ||
      (g_transfer.request.backup_action != "backup" && g_transfer.request.backup_action != "restore")) return {};
  const auto statistics = storage::Channel().Statistics();
  return {statistics.bytes, statistics.bytes_per_second, statistics.finalizing, statistics.waiting_seconds};
}
bool BackupCancelRequested() { return g_backup_active.load() && g_backup_cancel.load(); }
BackupCancellation GetBackupCancellation() {
  std::lock_guard<std::mutex> guard(g_lock);
  BackupCancellation result;
  result.id = g_transfer.id;
  result.active = g_transfer.request.job == aeraui::Job::kPcBackupOperation &&
      (g_transfer.request.backup_action == "backup" || g_transfer.request.backup_action == "restore") &&
      (g_transfer.phase == "queued" || g_transfer.phase == "installing");
  result.restore = g_transfer.request.backup_action == "restore";
  result.requested = g_transfer.cancel_requested;
  return result;
}
bool CancelBackup(const std::string &id, bool acknowledge_partial_restore) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.id != id || g_transfer.request.job != aeraui::Job::kPcBackupOperation ||
      (g_transfer.phase != "queued" && g_transfer.phase != "installing") ||
      (g_transfer.request.backup_action != "backup" && g_transfer.request.backup_action != "restore") ||
      (g_transfer.request.backup_action == "restore" && !acknowledge_partial_restore)) return false;
  g_transfer.cancel_requested = true; g_backup_cancel.store(true);
  g_transfer.detail = "Cancellation requested. Waiting for the worker to stop.";
  storage::Channel().Stop();
  if (g_transfer.phase == "queued") {
    g_transfer.phase = "cancelled"; g_backup_active.store(false);
    g_transfer.detail = "Backup operation cancelled before it started.";
  }
  return true;
}
void CompleteInstall(const std::string &id, int result) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.id != id || g_transfer.phase != "installing") return;
  if (g_transfer.request.job == aeraui::Job::kPcBackupOperation) storage::Channel().Stop();
  g_transfer.phase = result == 0 ? "completed" : "failed";
  if (g_transfer.cancel_requested) {
    g_transfer.phase = "cancelled"; result = 1;
    g_transfer.detail = g_transfer.request.backup_action == "restore" ?
        "Restore cancelled. Data may be partially restored. Complete a full restore before booting Android." :
        "Backup cancelled. The unfinished backup must not be used for restore.";
    g_transfer.backup_result["error"] = g_transfer.detail;
  }
  g_backup_active.store(false);
  g_transfer.result = result;
  if (result == 0) { g_transfer.progress = 100;
    if (g_transfer.request.job == aeraui::Job::kPcBackupOperation)
      g_transfer.detail = g_transfer.backup_result.get("detail", "Backup operation completed.").asString();
    else if (g_transfer.request.job == aeraui::Job::kNetworkStorage)
      g_transfer.detail = g_transfer.request.nas_action == "save" ? "Network storage settings saved." :
          g_transfer.request.nas_action == "disconnect" ? "Network storage disconnected." : "Network storage connected.";
    else if (g_transfer.request.job != aeraui::Job::kRootOperation) g_transfer.detail = "Installation completed.";
  } else if (g_transfer.cancel_requested) {
    // Preserve the explicit cancellation warning above.
  } else if (g_transfer.request.job == aeraui::Job::kPcBackupOperation)
    g_transfer.detail = g_transfer.backup_result.get("error", "Backup operation failed; review the recovery log.").asString();
  else if (g_transfer.request.job == aeraui::Job::kNetworkStorage)
    g_transfer.detail = "Network storage operation failed. Check the connection settings and network storage status.";
  else if (g_transfer.request.job != aeraui::Job::kRootOperation)
    g_transfer.detail = "Installation failed. Review the installer log.";
  g_prompt = Json::Value(Json::objectValue); g_prompt_answer_id.clear();
  RemoveTransferFiles(g_transfer);
}
void UpdateInstallerPrompt(const aeraui::InstallerPrompt &prompt) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_prompt = Json::Value(Json::objectValue);
  if (!prompt.active) return;
  g_prompt["id"] = prompt.id; g_prompt["title"] = prompt.title;
  g_prompt["message"] = prompt.message; g_prompt["accept"] = prompt.accept;
  g_prompt["decline"] = prompt.decline;
}
void UpdateInstallerPresentation(const aeraui::InstallerPresentation &presentation) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_transfer.phase != "installing" || !presentation.active) return;
  g_presentation["package"] = presentation.package_name;
  g_presentation["device"] = presentation.device;
  g_presentation["author"] = presentation.author;
  g_presentation["stage_title"] = presentation.stage_title;
  g_presentation["stage_detail"] = presentation.stage_detail;
  g_presentation["stage"] = presentation.stage;
  g_presentation["stage_count"] = presentation.stage_count;
  g_presentation["rebooting"] = presentation.rebooting;
  g_presentation["reboot_target"] = presentation.reboot_target;
  g_presentation["reboot_seconds"] = presentation.reboot_seconds;
}
bool TakePromptAnswer(std::string *id, bool *accepted) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (!id || !accepted || g_prompt_answer_id.empty()) return false;
  *id = g_prompt_answer_id; *accepted = g_prompt_answer; g_prompt_answer_id.clear(); return true;
}
bool ForgetComputers() {
  std::lock_guard<std::mutex> guard(g_lock);
  if (!TerminalPhase(g_transfer.phase) && g_transfer.request.job == aeraui::Job::kPcBackupOperation) return false;
  if (!g_status.remember_available) {
    const std::string path = std::string(AERA_PC_PRIVATE_DIRECTORY) + "/computers.json";
    if (unlink(path.c_str()) != 0 && errno != ENOENT) return false;
  }
  const Json::Value previous = g_trusted;
  g_trusted = Json::Value(Json::objectValue);
  if (g_status.remember_available && !SaveTrusted()) { g_trusted = previous; return false; }
  g_sessions.clear(); return true;
}
void SetRecoveryBusy(bool busy) {
  std::lock_guard<std::mutex> guard(g_lock); g_recovery_busy = busy;
}
bool TakeRebootRequest(std::string *target) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (!target || g_reboot_target.empty() || g_recovery_busy || g_file_busy ||
      !TerminalPhase(g_transfer.phase) || Clock::now() < g_reboot_at) return false;
  *target = g_reboot_target; g_reboot_target.clear(); return true;
}

}  // namespace aera::pc
#else
namespace aera::pc {
bool SetEnabled(bool enabled, int, bool) { return !enabled; }
void RefreshNetwork() {}
void ReleaseHttpRedirect() {}
void RestoreHttpRedirect() {}
std::string HttpRedirectLocation(const std::string &) { return {}; }
Status GetStatus() { return {}; }
UploadProgress GetUploadProgress() { return {}; }
bool CancelUpload(const std::string &) { return false; }
bool NeedsUiAttention() { return false; }
void SetRecoveryBusy(bool) {}
bool TakeRebootRequest(std::string *) { return false; }
void RefreshPlatformInfo() {}
void CleanupOldTransfers() {}
bool TakeConnectionRequest(ConnectionRequest *) { return false; }
void ResolveConnection(const std::string &, bool, bool) {}
bool TakeInstallRequest(InstallRequest *) { return false; }
bool BeginInstall(const std::string &) { return false; }
void RejectInstall(const std::string &) {}
void UpdateInstall(int, const std::string &, const std::string &) {}
void CompleteInstall(const std::string &, int) {}
void UpdateRootInspection(const aeraui::root::Progress &) {}
void UpdateBackupResult(const Json::Value &) {}
bool BackupOperationActive() { return false; }
BackupTransferStatistics GetBackupTransferStatistics() { return {}; }
bool BackupCancelRequested() { return false; }
BackupCancellation GetBackupCancellation() { return {}; }
bool CancelBackup(const std::string &, bool) { return false; }
void UpdateInstallerPrompt(const aeraui::InstallerPrompt &) {}
void UpdateInstallerPresentation(const aeraui::InstallerPresentation &) {}
bool TakePromptAnswer(std::string *, bool *) { return false; }
bool ForgetComputers() { return true; }
}
#endif
