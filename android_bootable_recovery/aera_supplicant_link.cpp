#include "aera_supplicant_link.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr size_t kPacketCapacity = 64 * 1024;
std::atomic<unsigned int> g_endpoint_sequence{0};

socklen_t AddressLength(const sockaddr_un& address) {
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                  std::strlen(address.sun_path) + 1);
}

bool IsNotification(const std::string& packet) {
    return (!packet.empty() && packet.front() == '<') ||
           packet.compare(0, 7, "IFNAME=") == 0;
}

bool IsOkay(const std::string& response) {
    return response == "OK" || response == "OK\n";
}

}  // namespace

AeraSupplicantLink::AeraSupplicantLink() : fd_(-1), subscribed_(false) {}

AeraSupplicantLink::~AeraSupplicantLink() {
    Reset();
}

bool AeraSupplicantLink::Connect(const std::string& endpoint) {
    Reset();

    sockaddr_un remote {};
    if (endpoint.empty() || endpoint.size() >= sizeof(remote.sun_path))
        return false;

    const int socket_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        return false;

    sockaddr_un local {};
    local.sun_family = AF_UNIX;
    std::snprintf(local.sun_path, sizeof(local.sun_path),
                  "/tmp/aera_supplicant_%d_%u.sock", static_cast<int>(getpid()),
                  g_endpoint_sequence.fetch_add(1, std::memory_order_relaxed));
    unlink(local.sun_path);

    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&local),
             AddressLength(local)) != 0) {
        close(socket_fd);
        return false;
    }

    remote.sun_family = AF_UNIX;
    std::memcpy(remote.sun_path, endpoint.data(), endpoint.size());
    remote.sun_path[endpoint.size()] = '\0';

    if (connect(socket_fd, reinterpret_cast<const sockaddr*>(&remote),
                AddressLength(remote)) != 0) {
        close(socket_fd);
        unlink(local.sun_path);
        return false;
    }

    fd_ = socket_fd;
    local_endpoint_ = local.sun_path;
    return true;
}

void AeraSupplicantLink::Reset() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (!local_endpoint_.empty()) {
        unlink(local_endpoint_.c_str());
        local_endpoint_.clear();
    }
    subscribed_ = false;
}

bool AeraSupplicantLink::Connected() const {
    return fd_ >= 0;
}

bool AeraSupplicantLink::Receive(std::string* packet, int timeout_ms) {
    if (fd_ < 0 || packet == nullptr || timeout_ms < 0)
        return false;

    pollfd descriptor {};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;

    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);

    if (result <= 0 || (descriptor.revents & POLLIN) == 0)
        return false;

    std::array<char, kPacketCapacity> buffer {};
    const ssize_t received = recv(fd_, buffer.data(), buffer.size(), 0);
    if (received <= 0)
        return false;

    packet->assign(buffer.data(), static_cast<size_t>(received));
    return true;
}

bool AeraSupplicantLink::Execute(const std::string& command, std::string* response,
                                 int timeout_ms) {
    if (fd_ < 0 || response == nullptr || command.empty())
        return false;

    if (send(fd_, command.data(), command.size(), 0) !=
        static_cast<ssize_t>(command.size())) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        std::string packet;
        if (!Receive(&packet, static_cast<int>(remaining.count())))
            return false;
        if (IsNotification(packet))
            continue;
        *response = std::move(packet);
        return true;
    }
    return false;
}

bool AeraSupplicantLink::Subscribe() {
    std::string response;
    subscribed_ = Execute("ATTACH", &response) && IsOkay(response);
    return subscribed_;
}

void AeraSupplicantLink::Unsubscribe() {
    if (!subscribed_)
        return;
    std::string response;
    Execute("DETACH", &response, 500);
    subscribed_ = false;
}

bool AeraSupplicantLink::AwaitAny(const std::vector<std::string>& tokens,
                                  int timeout_ms, std::string* event) {
    if (!subscribed_ || event == nullptr || tokens.empty())
        return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        std::string packet;
        if (!Receive(&packet, static_cast<int>(remaining.count())))
            return false;

        for (const std::string& token : tokens) {
            if (packet.find(token) != std::string::npos) {
                *event = std::move(packet);
                return true;
            }
        }
    }
    return false;
}
