#pragma once
#include "wire.hpp"
#include "clipboard/file_transfer.hpp"
#include <optional>
#include <deque>
#include "clipboard/rich_content.hpp"

namespace lrdp {
struct ClipboardResult {
    std::vector<Bytes> outbound;
    std::optional<std::string> remote_text;
    std::optional<RichClipboard> remote_rich;
    std::optional<std::vector<std::string>> remote_files;
    std::optional<std::string> file_error;
};
Bytes clipboard_pdu(std::uint16_t type, std::uint16_t flags, View data = {});

// Session-thread ownership. File capabilities remain off until a confined native
// store has explicitly been configured. Neither source paths nor executable modes
// are transmitted to a receiver.
class Clipboard {
    std::size_t limit_;
    bool started_ = false, long_names_ = false, caps_seen_ = false;
    bool offer_in_flight_ = false, offer_dirty_ = false, local_present_ = false;
    bool remote_unicode_ = false, request_files_ = false;
    std::uint64_t remote_generation_ = 0;
    std::optional<std::uint64_t> request_generation_;
    std::optional<std::uint32_t> remote_files_format_;
    bool rich_enabled_ = false;
    std::shared_ptr<const RichClipboard> local_rich_, published_rich_;
    std::deque<std::pair<std::uint32_t, unsigned>> rich_requests_;
    std::optional<unsigned> rich_kind_;
    RichClipboard received_rich_;
    std::string local_text_;
    std::optional<std::string> published_text_;
    std::unique_ptr<ClipboardFiles> files_;
    std::shared_ptr<const ClipboardFileSource> local_files_;
    Bytes make_offer();
    void local_changed();
    void request_remote(ClipboardResult& result);
    void collect_files(ClipboardResult& result);
    std::vector<Bytes> offer_changed();
public:
    explicit Clipboard(std::size_t limit = 1024 * 1024) : limit_(limit) {}
    void configure_files(std::shared_ptr<ClipboardFileStore> store, FileClipboardLimits limits = {});
    bool files_enabled() const { return bool(files_); }
    void configure_rich();
    bool rich_enabled() const { return rich_enabled_; }
    std::size_t message_limit() const { return limit_ + 8; }
    std::vector<Bytes> set_local_rich(RichClipboard content);
    std::vector<Bytes> start();
    std::vector<Bytes> set_local(std::string text);
    std::vector<Bytes> set_local_files(const std::vector<std::string>& paths);
    ClipboardResult accept(View pdu);
    ClipboardResult tick();
};
} // namespace lrdp
