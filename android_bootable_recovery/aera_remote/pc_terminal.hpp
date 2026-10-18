/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <functional>
#include <string>
#include <openssl/ssl.h>
namespace aera::pc::terminal {
struct Request {
  std::string path, websocket_key, websocket_protocol;
  bool websocket = false;
};
bool Available();
std::string Open(const std::string &session, std::string *error);
bool Close(const std::string &session);
void Stop();
bool Proxy(SSL *tls, const Request &request, const std::string &cookie,
           const std::function<bool(const std::string &)> &authorized);
}
