/* SPDX-License-Identifier: Apache-2.0 */

#include "aera_remote.hpp"
#include "pc_connection.hpp"

#include "frame_broker.hpp"
#include "input.hpp"

#include <json/json.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <map>
#include <memory>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <cstdlib>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>

namespace aera::remote {
namespace {

constexpr size_t kMaximumHeaders = 16 * 1024;
constexpr size_t kMaximumBody = 32 * 1024;
constexpr char kClientPath[] = "/system/etc/aera/remote/index.html";
constexpr char kBoundary[] = "aeraframe";

struct Request {
  std::string method;
  std::string path;
  std::string query;
  std::map<std::string, std::string> headers;
  std::string body;
};

std::mutex g_lifecycle_lock;
std::atomic<bool> g_running{false};
int g_listener = -1;
int g_port = 0;
std::thread g_accept_thread;
std::string g_access_code;

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string Trim(const std::string& value) {
  const size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

bool SendAll(int descriptor, const void* bytes, size_t length) {
  const char* data = static_cast<const char*>(bytes);
  size_t sent = 0;
  while (sent < length) {
    const ssize_t count = send(descriptor, data + sent, length - sent, MSG_NOSIGNAL);
    if (count > 0) sent += static_cast<size_t>(count);
    else if (count < 0 && errno == EINTR) continue;
    else return false;
  }
  return true;
}

bool SendAll(int descriptor, const std::string& data) {
  return SendAll(descriptor, data.data(), data.size());
}

void Reply(int descriptor, const char* status, const char* content_type,
           const std::string& body) {
  std::ostringstream header;
  header << "HTTP/1.1 " << status << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "X-Content-Type-Options: nosniff\r\n"
         << "Referrer-Policy: no-referrer\r\n"
         << "Connection: close\r\n\r\n";
  SendAll(descriptor, header.str());
  SendAll(descriptor, body);
}

std::string Header(const Request& request, const std::string& name) {
  const auto found = request.headers.find(Lower(name));
  return found == request.headers.end() ? std::string{} : found->second;
}

std::string Decode(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '+' ) {
      decoded.push_back(' ');
    } else if (value[i] == '%' && i + 2 < value.size() &&
               std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
      const std::string hex = value.substr(i + 1, 2);
      decoded.push_back(static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16)));
      i += 2;
    } else {
      decoded.push_back(value[i]);
    }
  }
  return decoded;
}

std::string QueryValue(const std::string& query, const std::string& name) {
  size_t cursor = 0;
  while (cursor <= query.size()) {
    const size_t separator = query.find('&', cursor);
    const size_t end = separator == std::string::npos ? query.size() : separator;
    const size_t equals = query.find('=', cursor);
    if (equals != std::string::npos && equals < end &&
        Decode(query.substr(cursor, equals - cursor)) == name)
      return Decode(query.substr(equals + 1, end - equals - 1));
    if (separator == std::string::npos) break;
    cursor = separator + 1;
  }
  return {};
}

bool ConstantEqual(const std::string& left, const std::string& right) {
  const size_t length = std::max(left.size(), right.size());
  unsigned int difference = static_cast<unsigned int>(left.size() ^ right.size());
  for (size_t i = 0; i < length; ++i) {
    const unsigned char a = i < left.size() ? left[i] : 0;
    const unsigned char b = i < right.size() ? right[i] : 0;
    difference |= static_cast<unsigned int>(a ^ b);
  }
  return difference == 0;
}

bool Authorized(const Request& request) {
  const std::string expected = AccessCode();
  if (expected.empty()) return false;
  if (ConstantEqual(QueryValue(request.query, "code"), expected)) return true;
  if (ConstantEqual(Header(request, "x-aera-code"), expected)) return true;
  const std::string authorization = Header(request, "authorization");
  return authorization.size() > 7 &&
         Lower(authorization.substr(0, 7)) == "bearer " &&
         ConstantEqual(authorization.substr(7), expected);
}

bool ReadRequest(int descriptor, Request* request) {
  timeval timeout{5, 0};
  setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  std::string received;
  std::array<char, 4096> block{};
  size_t header_end = std::string::npos;
  while ((header_end = received.find("\r\n\r\n")) == std::string::npos) {
    const ssize_t count = recv(descriptor, block.data(), block.size(), 0);
    if (count <= 0) return false;
    received.append(block.data(), static_cast<size_t>(count));
    if (received.size() > kMaximumHeaders) return false;
  }

  const std::string header_text = received.substr(0, header_end);
  std::istringstream lines(header_text);
  std::string line;
  if (!std::getline(lines, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::istringstream first(line);
  std::string target;
  std::string version;
  if (!(first >> request->method >> target >> version) ||
      version.compare(0, 5, "HTTP/") != 0)
    return false;
  const size_t question = target.find('?');
  request->path = question == std::string::npos ? target : target.substr(0, question);
  request->query = question == std::string::npos ? "" : target.substr(question + 1);

  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    request->headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
  }

  size_t body_length = 0;
  const std::string length = Header(*request, "content-length");
  if (!length.empty()) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(length.c_str(), &end, 10);
    if (!end || *end || parsed > kMaximumBody) return false;
    body_length = static_cast<size_t>(parsed);
  }
  request->body = received.substr(header_end + 4);
  while (request->body.size() < body_length) {
    const ssize_t count = recv(descriptor, block.data(), block.size(), 0);
    if (count <= 0) return false;
    request->body.append(block.data(), static_cast<size_t>(count));
    if (request->body.size() > kMaximumBody) return false;
  }
  request->body.resize(body_length);
  return true;
}

bool ParseJson(const std::string& body, Json::Value* value) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::string error;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  return reader->parse(body.data(), body.data() + body.size(), value, &error) &&
         value->isObject();
}

std::string LoadClient() {
  std::ifstream input(kClientPath, std::ios::binary);
  if (!input) return {};
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

void Stream(int descriptor) {
  std::ostringstream header;
  header << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: multipart/x-mixed-replace; boundary=" << kBoundary << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n";
  if (!SendAll(descriptor, header.str())) return;

  uint64_t last_generation = 0;
  while (g_running.load(std::memory_order_acquire)) {
    frames::Request();
    std::string jpeg;
    uint64_t generation = 0;
    if (frames::Latest(&jpeg, &generation) && generation != last_generation) {
      last_generation = generation;
      std::ostringstream part;
      part << "--" << kBoundary << "\r\n"
           << "Content-Type: image/jpeg\r\n"
           << "Content-Length: " << jpeg.size() << "\r\n\r\n";
      if (!SendAll(descriptor, part.str()) || !SendAll(descriptor, jpeg) ||
          !SendAll(descriptor, "\r\n"))
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void Handle(int descriptor) {
  Request request;
  if (!ReadRequest(descriptor, &request)) {
    Reply(descriptor, "400 Bad Request", "text/plain; charset=utf-8", "Bad request\n");
    return;
  }
  if (request.method == "OPTIONS") {
    Reply(descriptor, "204 No Content", "text/plain", "");
    return;
  }
  if (request.method == "GET" && Port() == 80 &&
      request.path == "/pc") {
    const std::string location = aera::pc::HttpRedirectLocation(Header(request, "host"));
    if (!location.empty()) {
      SendAll(descriptor, "HTTP/1.1 302 Found\r\nLocation: " + location +
          "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
      return;
    }
  }
  if (request.method == "GET" && (request.path == "/" || request.path == "/index.html" ||
                                 request.path == "/remote" || request.path == "/remote/")) {
    const std::string client = LoadClient();
    if (client.empty())
      Reply(descriptor, "503 Service Unavailable", "text/plain; charset=utf-8",
            "AERA Remote client is not installed.\n");
    else
      Reply(descriptor, "200 OK", "text/html; charset=utf-8", client);
    return;
  }
  if (request.method == "GET" && request.path == "/favicon.ico") {
    Reply(descriptor, "204 No Content", "image/x-icon", "");
    return;
  }
  if (!Authorized(request)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Reply(descriptor, "401 Unauthorized", "application/json", "{\"error\":\"invalid_code\"}");
    return;
  }
  if (request.method == "GET" && request.path == "/api/status") {
    Json::Value status(Json::objectValue);
    status["width"] = frames::Width();
    status["height"] = frames::Height();
    status["stream"] = true;
    status["input"] = true;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    Reply(descriptor, "200 OK", "application/json", Json::writeString(writer, status));
    return;
  }
  if (request.method == "GET" && request.path == "/stream.mjpeg") {
    Stream(descriptor);
    return;
  }
  if (request.method == "GET" && request.path == "/screen.jpg") {
    frames::Request();
    std::string jpeg;
    for (int attempt = 0; attempt < 20 && !frames::Latest(&jpeg, nullptr); ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (jpeg.empty()) Reply(descriptor, "503 Service Unavailable", "text/plain", "No frame\n");
    else Reply(descriptor, "200 OK", "image/jpeg", jpeg);
    return;
  }
  if (request.method == "POST" && request.path == "/api/input/touch") {
    Json::Value body;
    if (!ParseJson(request.body, &body) || !body["action"].isString()) {
      Reply(descriptor, "400 Bad Request", "application/json", "{\"error\":\"invalid_touch\"}");
      return;
    }
    const std::string action = body["action"].asString();
    const bool okay = input::Touch(action, body.get("x", 0).asInt(), body.get("y", 0).asInt());
    Reply(descriptor, okay ? "204 No Content" : "409 Conflict", "application/json", "");
    return;
  }
  if (request.method == "POST" && request.path == "/api/input/key") {
    Json::Value body;
    const bool okay = ParseJson(request.body, &body) && body["key"].isString() &&
                      input::Key(body["key"].asString());
    Reply(descriptor, okay ? "204 No Content" : "400 Bad Request", "application/json", "");
    return;
  }
  Reply(descriptor, "404 Not Found", "application/json", "{\"error\":\"not_found\"}");
}

void ServeConnections(int listener) {
  while (g_running.load(std::memory_order_acquire)) {
    const int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection < 0) {
      if (errno == EINTR) continue;
      if (!g_running.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      continue;
    }
    std::thread([connection] {
      Handle(connection);
      shutdown(connection, SHUT_RDWR);
      close(connection);
    }).detach();
  }
}

std::string GenerateCode() {
  uint64_t random = 0;
  const int source = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (source >= 0) {
    const ssize_t count = read(source, &random, sizeof(random));
    close(source);
    if (count != static_cast<ssize_t>(sizeof(random))) random = 0;
  }
  if (random == 0) {
    random = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
             static_cast<uint64_t>(getpid());
  }
  char code[9];
  std::snprintf(code, sizeof(code), "%08llu",
                static_cast<unsigned long long>(random % 100000000ULL));
  return code;
}

}  // namespace

bool Start(int port) {
  if (port < 1 || port > 65535) return false;
  if (port == 80) aera::pc::ReleaseHttpRedirect();
  std::lock_guard<std::mutex> guard(g_lifecycle_lock);
  if (g_running.load(std::memory_order_acquire)) return g_port == port;

  const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0) return false;
  const int enabled = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener, 8) != 0) {
    close(listener);
    return false;
  }

  g_access_code = GenerateCode();
  g_listener = listener;
  g_port = port;
  g_running.store(true, std::memory_order_release);
  g_accept_thread = std::thread(ServeConnections, listener);
  return true;
}

void Stop() {
  std::lock_guard<std::mutex> guard(g_lifecycle_lock);
  if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
  if (g_listener >= 0) {
    shutdown(g_listener, SHUT_RDWR);
    close(g_listener);
    g_listener = -1;
  }
  if (g_accept_thread.joinable()) g_accept_thread.join();
  input::Shutdown();
  frames::Reset();
  g_access_code.clear();
  g_port = 0;
}

bool Running() {
  return g_running.load(std::memory_order_acquire);
}

int Port() {
  std::lock_guard<std::mutex> guard(g_lifecycle_lock);
  return g_port;
}

std::string Address() {
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return {};
  std::string preferred;
  std::string fallback;
  for (ifaddrs* entry = interfaces; entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
        (entry->ifa_flags & IFF_LOOPBACK))
      continue;
    char text[INET_ADDRSTRLEN]{};
    const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
    if (!inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) continue;
    if (std::strncmp(entry->ifa_name, "wlan", 4) == 0) preferred = text;
    else if (fallback.empty()) fallback = text;
  }
  freeifaddrs(interfaces);
  return preferred.empty() ? fallback : preferred;
}

std::string AccessCode() {
  std::lock_guard<std::mutex> guard(g_lifecycle_lock);
  return g_access_code;
}

bool ShouldRenderFrame() {
  return Running() && frames::ShouldRender();
}

void CaptureAfterRender() {
  if (Running()) frames::CaptureAfterRender();
}

}  // namespace aera::remote
