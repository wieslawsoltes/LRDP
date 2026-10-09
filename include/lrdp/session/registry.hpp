#pragma once
#include "lrdp/reconnect.hpp"
#include <chrono>
#include <map>

namespace lrdp::persistent {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
struct Lease {
    std::uint32_t id = 0;
    bool created = false;
};
struct Summary {
    std::uint32_t id;
    bool attached;
    std::string principal;
};
// Single-owner state machine. The native broker associates owner with an accepted
// private Unix connection, never with a client-controlled PID or numeric cookie.
class Registry {
    struct Record {
        std::string principal;
        ReconnectCookie current, candidate;
        std::uint64_t owner = 0;
        bool retained = false, committed = false;
        Time expires{}, rotated{};
        ~Record();
    };
    std::map<std::uint32_t, Record> records_;
    std::size_t capacity_;
    std::chrono::seconds retention_;
    Record& owned(std::uint64_t owner);
public:
    Registry(std::size_t capacity, std::chrono::seconds retention);
    Lease acquire(std::string principal, const std::optional<ReconnectCookie>& verifier, std::uint64_t owner, Time now);
    std::optional<ReconnectCookie> commit(std::uint64_t owner, bool retain, Time now);
    ReconnectCookie rotate(std::uint64_t owner, Time now);
    // A failed, uncommitted resume does not extend the original expiration.
    std::optional<std::uint32_t> release(std::uint64_t owner, Time now);
    std::vector<std::uint32_t> expire(Time now);
    bool erase(std::uint32_t id);
    std::vector<Summary> list() const;
    std::size_t size() const { return records_.size(); }
};
ReconnectCookie enhanced_verifier(const ReconnectCookie& server);
} // namespace lrdp::persistent
