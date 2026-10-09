#pragma once
#include "protocol.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace lrdp::drive {
// Native errno and protocol NTSTATUS are kept separate; no unchecked narrowing.
class IoError final : public std::system_error {
public:
    explicit IoError(int code, const char* operation)
        : std::system_error(code, std::generic_category(), operation) {}
};
int status_errno(std::uint32_t status) noexcept;
class Client {
public:
    virtual ~Client() = default;
    virtual Reply call(Request request) = 0;
    virtual std::vector<Device> devices() const = 0;
    virtual const Limits& limits() const = 0;
};

// Native callbacks may block on this bridge, never the RDP event-loop thread.
// Disconnect/timeout wakes every waiter. No promise is abandoned and no timed-out
// completion can be reused against a different file or device generation.
class Bridge final : public Client {
    struct Waiter { Request request; std::optional<Reply> reply; std::condition_variable ready; };
    const Limits limits_;
    const std::thread::id owner_ = std::this_thread::get_id();
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<Waiter>> waiters_;
    std::deque<std::uint64_t> queued_;
    std::vector<Device> devices_;
    std::uint64_t next_ticket_ = 1;
    int stopped_ = 0;
    void stop_locked(int error) noexcept;
    void owner() const;
public:
    explicit Bridge(Limits limits = {});
    ~Bridge() override;
    Reply call(Request request) override;
    std::vector<Device> devices() const override;
    const Limits& limits() const override { return limits_; }
    void publish(std::vector<Device> devices);
    std::vector<Request> take_requests();
    void complete(Reply reply);
    void disconnect(int error) noexcept;
    int stopped() const;
};
} // namespace lrdp::drive
