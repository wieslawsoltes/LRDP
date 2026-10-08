#include "lrdp/clipboard.hpp"
#include <algorithm>

namespace lrdp {
Bytes clipboard_pdu(std::uint16_t type, std::uint16_t flags, View data) {
    require(data.size() <= 16 * 1024 * 1024, "clipboard PDU exceeds limit");
    Writer out; out.le16(type).le16(flags).le32(std::uint32_t(data.size())).raw(data); return std::move(out).finish();
}
std::vector<Bytes> Clipboard::start() {
    require(!started_, "clipboard already started"); started_ = true;
    Writer caps; caps.le16(1).le16(0).le16(1).le16(12).le32(2).le32(2);
    return {clipboard_pdu(7, 0, caps.bytes()), clipboard_pdu(1, 0)};
}
Bytes Clipboard::make_offer() const {
    Writer list;
    if (local_present_) { list.le32(13); if (long_names_) list.le16(0); else list.zeros(32); }
    return clipboard_pdu(2, 0, list.bytes());
}
std::vector<Bytes> Clipboard::set_local(std::string text) {
    // Enforce both the UTF-8 storage bound and the actual UTF-16 wire bound.
    require(text.size() <= limit_ && utf16le(text).size() <= limit_, "clipboard text exceeds policy limit");
    local_text_ = std::move(text); local_present_ = true; offer_dirty_ = true;
    if (!started_ || !caps_seen_ || offer_in_flight_) return {};
    offer_dirty_ = false; offer_in_flight_ = true; return {make_offer()};
}
void Clipboard::request_remote(ClipboardResult& result) {
    if (!remote_unicode_ || request_generation_) return;
    Writer request; request.le32(13); result.outbound.push_back(clipboard_pdu(4, 0, request.bytes()));
    request_generation_ = remote_generation_;
}
ClipboardResult Clipboard::accept(View pdu) {
    require(started_, "clipboard channel used before initialization");
    Reader in(pdu); const auto type = in.le16(), flags = in.le16(); const auto length = in.le32();
    require(length <= limit_ && length == in.remaining(), "clipboard payload length mismatch or policy limit");
    ClipboardResult result;
    if (type == 7) {
        require(!caps_seen_, "duplicate clipboard capabilities");
        const auto count = in.le16(); in.skip(2); require(count <= 16, "clipboard capability count exceeds limit");
        bool general_seen = false;
        for (unsigned i = 0; i < count; ++i) {
            const auto kind = in.le16(), size = in.le16(); require(size >= 4, "invalid clipboard capability size");
            Reader cap(in.take(size - 4));
            if (kind == 1) {
                require(!general_seen && size == 12, "invalid clipboard general capability"); general_seen = true;
                const auto version = cap.le32(); require(version == 1 || version == 2, "unsupported clipboard version");
                long_names_ = (cap.le32() & 2) != 0;
            }
        }
        in.end(); caps_seen_ = true;
        if (offer_dirty_ && !offer_in_flight_) { offer_dirty_ = false; offer_in_flight_ = true; result.outbound.push_back(make_offer()); }
    } else if (type == 2) {
        // Version-1 clients may omit the capabilities PDU.
        caps_seen_ = true; bool unicode = false; unsigned formats = 0;
        while (!in.empty()) {
            require(++formats <= 4096, "clipboard format count exceeds limit");
            const auto id = in.le32(); unicode |= id == 13;
            if (long_names_) {
                Writer name(1024); bool done = false;
                while (!in.empty()) { const auto c = in.le16(); name.le16(c); if (c == 0) { done = true; break; } }
                require(done, "unterminated clipboard format name"); (void)from_utf16le(name.bytes());
            } else in.skip(32); // Fixed short-format names may be ANSI or UTF-16.
        }
        remote_unicode_ = unicode; ++remote_generation_;
        result.outbound.push_back(clipboard_pdu(3, 1)); request_remote(result);
        if (offer_dirty_ && !offer_in_flight_) { offer_dirty_ = false; offer_in_flight_ = true; result.outbound.push_back(make_offer()); }
    } else if (type == 3) {
        in.end(); require(flags == 1 || flags == 2, "invalid clipboard format-list response flags");
        require(offer_in_flight_, "unsolicited clipboard format-list response");
        offer_in_flight_ = false;
        if (offer_dirty_) { offer_dirty_ = false; offer_in_flight_ = true; result.outbound.push_back(make_offer()); }
    } else if (type == 4) {
        const auto format = in.le32(); in.end();
        if (format == 13 && local_present_) {
            std::string windows; windows.reserve(local_text_.size());
            for (std::size_t i = 0; i < local_text_.size(); ++i) {
                const char c = local_text_[i]; if (c == '\n' && (i == 0 || local_text_[i - 1] != '\r')) windows += '\r'; windows += c;
            }
            auto payload = utf16le(windows);
            if (payload.size() <= limit_) result.outbound.push_back(clipboard_pdu(5, 1, payload));
            else result.outbound.push_back(clipboard_pdu(5, 2));
        } else result.outbound.push_back(clipboard_pdu(5, 2));
    } else if (type == 5) {
        require(request_generation_.has_value(), "unsolicited clipboard data response");
        require(flags == 1 || flags == 2, "invalid clipboard data response flags");
        const bool current = *request_generation_ == remote_generation_; request_generation_.reset();
        if (flags == 1 && current) {
            auto text = from_utf16le(in.take(in.remaining())); std::string unix_text; unix_text.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
                unix_text += text[i];
            }
            result.remote_text = std::move(unix_text);
        } else in.skip(in.remaining());
        if (!current) request_remote(result);
    } else if (type == 6) {
        // Legacy temporary-directory hint is irrelevant without file redirection.
        in.skip(in.remaining());
    } else if (type == 8) {
        // Never advertise file streaming, but return an explicit failure for a stray request.
        const auto stream = in.le32(); Writer failure; failure.le32(stream);
        result.outbound.push_back(clipboard_pdu(9, 2, failure.bytes()));
    } else throw ProtocolError("unsupported clipboard PDU");
    return result;
}
} // namespace lrdp
