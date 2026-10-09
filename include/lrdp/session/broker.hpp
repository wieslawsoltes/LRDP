#pragma once
#include "lrdp/platform/headless_server.hpp"
#include "lrdp/platform/unique_fd.hpp"
#include "registry.hpp"

namespace lrdp::persistent {
struct Attachment { std::uint32_t id; bool created; std::string display, authority; };
// Rootless owner-only control socket. This API trusts an authenticated principal
// supplied by lrdpd; it never obtains that principal from an RDP username field.
class Broker {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Broker(std::string directory, HeadlessOptions options, unsigned capacity = 4,
           std::chrono::seconds retention = std::chrono::minutes(30));
    ~Broker();
    void poll(int timeout_ms);
    const std::string& socket_path() const;
};
class BrokerClient {
    UniqueFd socket_;
    bool acquired_ = false;
    Bytes transact(unsigned operation, View payload = {});
public:
    explicit BrokerClient(const std::string& socket_path);
    Attachment acquire(std::string_view principal, const std::optional<ReconnectCookie>& verifier);
    std::optional<ReconnectCookie> commit(bool retain);
    ReconnectCookie rotate();
    void check() const;
    std::vector<Summary> list();
    void terminate(std::uint32_t id);
};
} // namespace lrdp::persistent
