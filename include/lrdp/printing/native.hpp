#pragma once
#include "jobs.hpp"
#include "lrdp/platform/unique_fd.hpp"

namespace lrdp::printing {
// Per-connection, same-Unix-user submission. Construction occurs after NLA.
class NativeEndpoint final : public Endpoint {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit NativeEndpoint(const std::string& private_root);
    ~NativeEndpoint() override;
    const std::string& path() const;
    void publish(std::vector<drive::Device>) override;
    void pump() override;
    std::vector<drive::Request> take_requests(std::uint32_t chunk) override;
    void complete(const drive::Reply&) override;
    void disconnect() noexcept override;
};
// Snapshot on the submitting thread/process, not on the network event loop.
// Regular files only; returned memfd cannot change, shrink, or grow.
UniqueFd snapshot_file(const std::string& path);
std::shared_ptr<const Source> sealed_source(UniqueFd descriptor);
std::vector<drive::Device> list_printers(const std::string& socket_path);
Result submit_file(const std::string& socket_path, drive::DeviceKey printer, const std::string& source);
} // namespace lrdp::printing
