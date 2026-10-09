#pragma once
#include "lrdp/platform/unique_fd.hpp"
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace lrdp {
// Immutable byte snapshots: aliases can share storage; readers retain the exact
// selection they requested even if ownership changes during a pipe write.
using MimeBytes = std::shared_ptr<const Bytes>;
using MimeContent = std::map<std::string, MimeBytes, std::less<>>;
struct MimeClipboardTransport {
    virtual ~MimeClipboardTransport() = default;
    virtual UniqueFd read_mime(const std::string& mime) = 0;
    virtual UniqueFd write_mime(std::uint32_t serial) = 0;
    virtual void finish_mime(std::uint32_t serial, bool success) = 0;
    virtual void offer_mimes(const std::vector<std::string>& formats) = 0;
};
struct MimeClipboardLimits {
    std::size_t bytes = 8 * 1024 * 1024;
    std::size_t turn_bytes = 64 * 1024;
    unsigned transfers = 16;
    std::chrono::milliseconds timeout{5000};
};
class MimeClipboard final {
    using Clock = std::chrono::steady_clock;
    struct Incoming { UniqueFd fd; std::string mime; Bytes bytes; Clock::time_point deadline; };
    struct Outgoing { UniqueFd fd; MimeBytes bytes; std::size_t offset = 0; Clock::time_point deadline; };
    struct Request { std::uint32_t serial; MimeBytes bytes; };
    MimeClipboardTransport& transport_;
    MimeClipboardLimits limits_;
    std::vector<std::string> supported_, offered_;
    bool own_ = false, observed_ = false;
    std::deque<std::string> reads_;
    std::optional<Incoming> incoming_;
    std::map<std::uint32_t, Outgoing> outgoing_;
    std::deque<Request> requests_;
    std::set<std::uint32_t> serials_;
    MimeContent local_, pending_;
    std::optional<MimeContent> ready_;
    std::size_t received_ = 0;
    void begin_read();
    void complete_read();
    void finish(std::uint32_t serial, bool success);
public:
    explicit MimeClipboard(MimeClipboardTransport& transport, MimeClipboardLimits limits = {});
    void supported(std::vector<std::string> formats);
    void owner_changed(std::vector<std::string> formats, bool own);
    void transfer(std::uint32_t serial, const std::string& mime);
    void set(MimeContent content);
    void poll();
    void clear() noexcept;
    std::optional<MimeContent> take();
    std::size_t in_flight() const { return serials_.size(); }
};
} // namespace lrdp
