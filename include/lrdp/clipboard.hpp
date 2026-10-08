#pragma once
#include "wire.hpp"
#include <optional>

namespace lrdp {
struct ClipboardResult {
    std::vector<Bytes> outbound;
    std::optional<std::string> remote_text;
};
Bytes clipboard_pdu(std::uint16_t type, std::uint16_t flags, View data = {});

// The session serializes channel and local-clipboard events on its own event loop.
// Only text is advertised. File transfer capability flags remain clear.
class Clipboard {
    std::size_t limit_;
    bool started_ = false, long_names_ = false, caps_seen_ = false;
    bool offer_in_flight_ = false, offer_dirty_ = false, local_present_ = false;
    bool remote_unicode_ = false;
    std::uint64_t remote_generation_ = 0;
    std::optional<std::uint64_t> request_generation_;
    std::string local_text_;
    Bytes make_offer() const;
    void request_remote(ClipboardResult& result);
public:
    explicit Clipboard(std::size_t limit = 1024 * 1024) : limit_(limit) {}
    std::vector<Bytes> start();
    std::vector<Bytes> set_local(std::string text);
    ClipboardResult accept(View pdu);
};
} // namespace lrdp
