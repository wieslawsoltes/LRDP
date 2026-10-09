#pragma once
#include "lrdp/drive/protocol.hpp"
#include <memory>

namespace lrdp::printing {
// Must expose an immutable snapshot for the lifetime of a submitted print job.
class Source {
public:
    virtual ~Source() = default;
    virtual std::uint64_t size() const = 0;
    virtual Bytes read(std::uint64_t offset, std::uint32_t length) const = 0;
};
struct Result { std::uint64_t cookie=0, transferred=0; std::uint32_t status=0; };
class Jobs {
    enum class State { open, write, close };
    struct Job {
        std::uint64_t cookie=0, offset=0, handle=0, pending=0;
        drive::DeviceKey device;
        std::shared_ptr<const Source> source;
        State state=State::open;
        std::uint32_t status=0, requested=0;
        drive::Clock::time_point deadline;
    };
    std::map<std::uint64_t, Job> jobs_;
    std::vector<drive::Device> devices_;
    std::vector<Result> results_;
    std::uint64_t next_ticket_=1, held_bytes_=0;
    void finish(std::map<std::uint64_t, Job>::iterator job);
public:
    static constexpr std::uint64_t byte_limit=64*1024*1024;
    static constexpr unsigned job_limit=4;
    void publish(std::vector<drive::Device> devices);
    const std::vector<drive::Device>& devices() const { return devices_; }
    // Rejects before any remote operation; explicit device generation is mandatory.
    void submit(std::uint64_t cookie, drive::DeviceKey device, std::shared_ptr<const Source> source);
    void cancel(std::uint64_t cookie);
    // Connection loss cannot complete outstanding remote jobs; drop local snapshots.
    void discard() noexcept;
    std::vector<drive::Request> take_requests(std::uint32_t chunk=65536);
    void complete(const drive::Reply& reply);
    std::vector<Result> take_results();
    std::size_t size() const { return jobs_.size(); }
};
class Endpoint {
public:
    virtual ~Endpoint()=default;
    virtual void publish(std::vector<drive::Device>)=0;
    virtual void pump()=0;
    virtual std::vector<drive::Request> take_requests(std::uint32_t chunk)=0;
    virtual void complete(const drive::Reply&)=0;
    virtual void disconnect() noexcept=0;
};
} // namespace lrdp::printing
