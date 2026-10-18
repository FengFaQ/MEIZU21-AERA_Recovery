#pragma once

#include <string>
#include <vector>

// Lightweight recovery-side client for wpa_supplicant's Unix datagram control
// interface. A link owns its local socket endpoint and may be used either for
// request/reply traffic or as an attached event subscription.
class AeraSupplicantLink {
public:
    AeraSupplicantLink();
    ~AeraSupplicantLink();

    AeraSupplicantLink(const AeraSupplicantLink&) = delete;
    AeraSupplicantLink& operator=(const AeraSupplicantLink&) = delete;

    bool Connect(const std::string& endpoint);
    void Reset();
    bool Connected() const;

    bool Execute(const std::string& command, std::string* response,
                 int timeout_ms = 2000);
    bool Subscribe();
    void Unsubscribe();
    bool AwaitAny(const std::vector<std::string>& tokens, int timeout_ms,
                  std::string* event);

private:
    bool Receive(std::string* packet, int timeout_ms);

    int fd_;
    bool subscribed_;
    std::string local_endpoint_;
};
