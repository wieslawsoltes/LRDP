#include "lrdp/clipboard.hpp"
#include <algorithm>

namespace lrdp {
namespace {
constexpr std::uint32_t local_file_format = 0xc000;
constexpr std::string_view file_format_name = "FileGroupDescriptorW";
std::string format_name(Reader& in, bool long_names, std::uint16_t flags) {
    if (long_names) {
        Writer name(1024); bool done = false;
        while (!in.empty()) { const auto c = in.le16(); name.le16(c); if (c == 0) { done = true; break; } }
        require(done, "unterminated clipboard format name"); return from_utf16le(name.bytes());
    }
    const auto bytes = in.take(32);
    if (flags & 4) {
        const auto end = std::find(bytes.begin(), bytes.end(), 0); return {bytes.begin(), end};
    }
    std::size_t size = 0;
    while (size < bytes.size() && (bytes[size] || bytes[size + 1])) size += 2;
    return from_utf16le(bytes.first(size), false);
}
}
Bytes clipboard_pdu(std::uint16_t type, std::uint16_t flags, View data) {
    require(data.size() <= 16 * 1024 * 1024, "clipboard PDU exceeds limit");
    Writer out; out.le16(type).le16(flags).le32(std::uint32_t(data.size())).raw(data); return std::move(out).finish();
}
void Clipboard::configure_files(std::shared_ptr<ClipboardFileStore> store, FileClipboardLimits limits) {
    require(!started_ && !files_, "file clipboard must be configured once before initialization");
    require(limits.entries <= 128 && limits.chunk + 4 <= limit_, "file clipboard exceeds static-channel policy");
    files_ = std::make_unique<ClipboardFiles>(std::move(store), limits);
}
std::vector<Bytes> Clipboard::start() {
    require(!started_, "clipboard already started"); started_ = true;
    Writer caps; caps.le16(1).le16(0).le16(1).le16(12).le32(2).le32(files_ ? 0x1e : 2);
    return {clipboard_pdu(7, 0, caps.bytes()), clipboard_pdu(1, 0)};
}
Bytes Clipboard::make_offer() {
    Writer list; std::uint16_t flags = 0;
    const bool file_offer = files_ && files_->supported() && local_files_;
    if (file_offer) {
        list.le32(local_file_format);
        if (long_names_) list.raw(utf16le(file_format_name));
        else {
            flags = 4; list.raw(View(reinterpret_cast<const std::uint8_t*>(file_format_name.data()), file_format_name.size()))
                .zeros(32 - file_format_name.size());
        }
    } else if (local_present_) {
        list.le32(13); if (long_names_) list.le16(0); else list.zeros(32);
    }
    // In-flight offers retain their published snapshot until a new offer is
    // actually sent. Locked file sources survive subsequent publications.
    published_text_ = local_present_ ? std::optional(local_text_) : std::nullopt;
    if (files_) files_->publish(file_offer ? local_files_ : nullptr);
    return clipboard_pdu(2, flags, list.bytes());
}
void Clipboard::local_changed() {
    ++remote_generation_; remote_unicode_ = false; remote_files_format_.reset();
    if (files_) files_->cancel();
    offer_dirty_ = true;
}
std::vector<Bytes> Clipboard::offer_changed() {
    std::vector<Bytes> result;
    if (files_) result = files_->drain();
    if (!started_ || !caps_seen_ || offer_in_flight_) return result;
    offer_dirty_ = false; offer_in_flight_ = true; result.push_back(make_offer()); return result;
}
std::vector<Bytes> Clipboard::set_local(std::string text) {
    require(text.size() <= limit_ && utf16le(text).size() <= limit_, "clipboard text exceeds policy limit");
    local_text_ = std::move(text); local_present_ = true; local_files_.reset(); local_changed(); return offer_changed();
}
std::vector<Bytes> Clipboard::set_local_files(const std::vector<std::string>& paths) {
    require(files_ != nullptr, "file clipboard is disabled");
    auto source = files_->offer(paths); local_files_ = std::move(source); local_present_ = false; local_text_.clear();
    local_changed(); return offer_changed();
}
void Clipboard::collect_files(ClipboardResult& result) {
    if (!files_) return;
    for (auto& pdu : files_->drain()) result.outbound.push_back(std::move(pdu));
    if (auto paths = files_->take_completed()) result.remote_files = std::move(paths);
    if (auto error = files_->take_error()) result.file_error = std::move(error);
}
void Clipboard::request_remote(ClipboardResult& result) {
    if (request_generation_) return;
    const bool want_files = files_ && files_->supported() && remote_files_format_;
    if (!want_files && !remote_unicode_) return;
    if (want_files) { files_->begin_remote(); collect_files(result); }
    Writer request; request.le32(want_files ? *remote_files_format_ : 13);
    result.outbound.push_back(clipboard_pdu(4, 0, request.bytes()));
    request_generation_ = remote_generation_; request_files_ = want_files;
}
ClipboardResult Clipboard::tick() {
    ClipboardResult result; if (files_) { files_->tick(); collect_files(result); } return result;
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
                (void)cap.le32(); // Version is informational; capabilities determine behavior.
                const auto features = cap.le32(); long_names_ = (features & 2) != 0;
                if (files_) files_->negotiate(features);
            }
        }
        in.end(); caps_seen_ = true;
        if (offer_dirty_ && !offer_in_flight_) { offer_dirty_ = false; offer_in_flight_ = true; result.outbound.push_back(make_offer()); }
    } else if (type == 2) {
        caps_seen_ = true; bool unicode = false; unsigned formats = 0; std::optional<std::uint32_t> file_format;
        while (!in.empty()) {
            require(++formats <= 4096, "clipboard format count exceeds limit");
            const auto id = in.le32(); unicode |= id == 13;
            const auto name = format_name(in, long_names_, flags);
            if (name == file_format_name && id >= 0xc000) {
                require(!file_format, "ambiguous file clipboard formats"); file_format = id;
            }
        }
        if (files_) files_->cancel();
        remote_unicode_ = unicode; remote_files_format_ = file_format; ++remote_generation_;
        result.outbound.push_back(clipboard_pdu(3, 1)); request_remote(result);
        if (offer_dirty_ && !offer_in_flight_) { offer_dirty_ = false; offer_in_flight_ = true; result.outbound.push_back(make_offer()); }
    } else if (type == 3) {
        in.end(); require(flags == 1 || flags == 2, "invalid clipboard format-list response flags");
        require(offer_in_flight_, "unsolicited clipboard format-list response"); offer_in_flight_ = false;
        if (offer_dirty_) { offer_dirty_ = false; offer_in_flight_ = true; result.outbound.push_back(make_offer()); }
    } else if (type == 4) {
        require(flags == 0, "invalid clipboard data request flags"); const auto format = in.le32(); in.end();
        if (format == local_file_format && files_) {
            try { result.outbound.push_back(clipboard_pdu(5, 1, files_->file_list())); }
            catch (const ProtocolError&) { result.outbound.push_back(clipboard_pdu(5, 2)); }
        } else if (format == 13 && published_text_) {
            std::string windows; windows.reserve(published_text_->size());
            for (std::size_t i = 0; i < published_text_->size(); ++i) {
                const char c = (*published_text_)[i];
                if (c == '\n' && (i == 0 || (*published_text_)[i - 1] != '\r')) windows += '\r';
                windows += c;
            }
            auto payload = utf16le(windows);
            result.outbound.push_back(payload.size() <= limit_ ? clipboard_pdu(5, 1, payload) : clipboard_pdu(5, 2));
        } else result.outbound.push_back(clipboard_pdu(5, 2));
    } else if (type == 5) {
        require(request_generation_.has_value(), "unsolicited clipboard data response");
        require(flags == 1 || flags == 2, "invalid clipboard data response flags");
        const bool current = *request_generation_ == remote_generation_; request_generation_.reset();
        if (request_files_) {
            if (files_ && current && files_->awaiting_list()) {
                if (flags == 1) files_->list(in.take(in.remaining())); else files_->cancel();
            }
        } else if (flags == 1 && current) {
            auto text = from_utf16le(in.take(in.remaining())); std::string unix_text; unix_text.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
                unix_text += text[i];
            }
            result.remote_text = std::move(unix_text);
        }
        if (!current) request_remote(result);
    } else if (type >= 8 && type <= 11 && files_) {
        files_->accept(type, flags, in.take(in.remaining()));
    } else if (type == 6) {
        in.skip(in.remaining()); // Legacy temporary-directory hint is never trusted.
    } else if (type == 8) {
        const auto stream = in.le32(); Writer failure; failure.le32(stream);
        result.outbound.push_back(clipboard_pdu(9, 2, failure.bytes()));
    } else throw ProtocolError("unsupported clipboard PDU");
    collect_files(result); return result;
}
} // namespace lrdp
