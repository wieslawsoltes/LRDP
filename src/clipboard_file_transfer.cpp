#include "lrdp/clipboard/file_transfer.hpp"
#include "lrdp/clipboard.hpp"
#include <algorithm>
#include <limits>

namespace lrdp {
ClipboardFiles::ClipboardFiles(std::shared_ptr<ClipboardFileStore> store, FileClipboardLimits limits)
    : store_(std::move(store)), limits_(limits) {
    require(store_ && limits_.window > 0 && limits_.window <= 8 && limits_.chunk > 0 && limits_.chunk <= 65536,
            "invalid clipboard transfer policy");
    (void)validate_clipboard_files({{"policy-check", false, 0, {}}}, limits_);
}
void ClipboardFiles::negotiate(std::uint32_t flags) { peer_stream_ = (flags & 4) != 0; peer_lock_ = (flags & 16) != 0; }
std::shared_ptr<const ClipboardFileSource> ClipboardFiles::offer(const std::vector<std::string>& paths) {
    auto source = store_->offer(paths); require(source != nullptr, "file provider returned no source");
    (void)validate_clipboard_files(source->files(), limits_, true); return source;
}
Bytes ClipboardFiles::file_list() const {
    require(peer_stream_ && published_, "clipboard has no published files");
    return encode_clipboard_files(published_->files(), limits_);
}
void ClipboardFiles::unlock() {
    if (incoming_lock_) {
        Writer out; out.le32(*incoming_lock_); outbound_.push_back(clipboard_pdu(11, 0, out.bytes())); incoming_lock_.reset();
    }
}
void ClipboardFiles::cancel() {
    unlock(); awaiting_list_ = receiving_ = false; pending_.clear(); sink_.reset(); incoming_.clear();
    scheduled_.clear(); queried_.clear(); completed_.reset();
}
void ClipboardFiles::fail(const std::string& message) { cancel(); error_ = message; }
void ClipboardFiles::begin_remote() {
    require(peer_stream_, "file streaming not negotiated"); cancel(); error_.reset();
    if (peer_lock_) {
        require(next_lock_ < std::numeric_limits<std::uint32_t>::max(), "clipboard lock identifiers exhausted");
        incoming_lock_ = ++next_lock_; Writer out; out.le32(*incoming_lock_);
        outbound_.push_back(clipboard_pdu(10, 0, out.bytes()));
    }
    awaiting_list_ = true; deadline_ = Clock::now() + std::chrono::seconds(30);
}
void ClipboardFiles::list(View payload) {
    require(awaiting_list_, "unsolicited file descriptor response");
    try {
        incoming_ = decode_clipboard_files(payload, limits_); awaiting_list_ = false; receiving_ = true;
        scheduled_.resize(incoming_.size()); queried_.resize(incoming_.size()); advance();
    } catch (const ProtocolError& e) { fail(e.what()); }
}
void ClipboardFiles::request(Read read) {
    require(next_stream_ < std::numeric_limits<std::uint32_t>::max(), "clipboard stream identifiers exhausted");
    const auto id = ++next_stream_;
    Writer out; out.le32(id).le32(std::uint32_t(read.index)).le32(read.size ? 1 : 2)
        .le32(std::uint32_t(read.offset)).le32(std::uint32_t(read.offset >> 32)).le32(read.count);
    if (incoming_lock_) out.le32(*incoming_lock_);
    outbound_.push_back(clipboard_pdu(8, 0, out.bytes())); pending_.emplace(id, read);
}
void ClipboardFiles::advance() {
    if (!receiving_) return;
    bool missing = false;
    for (std::size_t i = 0; i < incoming_.size(); ++i) if (!incoming_[i].directory && !incoming_[i].size) {
        missing = true;
        if (!queried_[i] && pending_.size() < limits_.window) { queried_[i] = true; request({i, 0, 8, true}); }
    }
    if (missing) return;
    if (!sink_) {
        (void)validate_clipboard_files(incoming_, limits_, true); sink_ = store_->receive(incoming_);
        require(sink_ != nullptr, "file provider returned no sink");
    }
    for (std::size_t i = 0; i < incoming_.size(); ++i) {
        if (incoming_[i].directory) continue;
        while (scheduled_[i] < *incoming_[i].size && pending_.size() < limits_.window) {
            const auto count = std::uint32_t(std::min<std::uint64_t>(limits_.chunk, *incoming_[i].size - scheduled_[i]));
            request({i, scheduled_[i], count, false}); scheduled_[i] += count;
        }
    }
    if (pending_.empty()) {
        auto paths = sink_->finish(); sink_.reset(); receiving_ = false; incoming_.clear();
        completed_ = std::move(paths); unlock();
    }
}
void ClipboardFiles::response(std::uint16_t flags, View payload) {
    Reader in(payload); const auto stream = in.le32();
    require(flags == 1 || flags == 2, "invalid file response flags");
    auto it = pending_.find(stream);
    if (it == pending_.end()) {
        require(stream > 0 && stream <= next_stream_, "unsolicited clipboard file response");
        return; // Delayed/duplicate responses never complete a new generation.
    }
    const auto read = it->second; pending_.erase(it);
    if (flags != 1) { fail("remote clipboard file read failed"); return; }
    try {
        if (read.size) {
            require(in.remaining() == 8, "invalid clipboard file size response");
            const auto low = in.le32(); incoming_[read.index].size = std::uint64_t(in.le32()) << 32 | low;
        } else {
            const auto count = std::uint32_t(in.remaining());
            require(count > 0 && count <= read.count, "premature EOF or oversized clipboard file response");
            sink_->write(read.index, read.offset, in.take(count));
            // RANGE specifies a maximum, not a mandatory exact response size.
            if (count < read.count) request({read.index, read.offset + count, read.count - count, false});
        }
        deadline_ = Clock::now() + std::chrono::seconds(30); advance();
    } catch (const ProtocolError& e) { fail(e.what()); }
}
void ClipboardFiles::serve(View payload) {
    Reader in(payload); const auto stream = in.le32();
    Writer reply; reply.le32(stream);
    try {
        require(peer_stream_, "file streaming not negotiated");
        const auto index = in.i32(); const auto flags = in.le32(); const auto low = in.le32(), high = in.le32();
        const auto count = in.le32(); auto source = published_;
        if (!in.empty()) {
            require(peer_lock_ && in.remaining() == 4, "invalid file lock reference");
            const auto lock = locked_.find(in.le32()); require(lock != locked_.end(), "unknown clipboard lock"); source = lock->second;
        }
        in.end(); require(source && index >= 0 && std::size_t(index) < source->files().size(), "unavailable clipboard file");
        const auto& file = source->files()[std::size_t(index)];
        require(!file.directory && file.size, "file contents requested for a directory");
        const auto offset = std::uint64_t(high) << 32 | low;
        if (flags == 1) {
            require(offset == 0 && count == 8, "invalid file size request");
            reply.le32(std::uint32_t(*file.size)).le32(std::uint32_t(*file.size >> 32));
        } else {
            require(flags == 2 && offset <= *file.size && count <= 1024*1024, "invalid file range request");
            const auto expected = std::uint32_t(std::min<std::uint64_t>({count, limits_.chunk, *file.size - offset}));
            const auto bytes = source->read(std::size_t(index), offset, expected);
            require(bytes.size() == expected, "local clipboard file changed or became unreadable"); reply.raw(bytes);
        }
        outbound_.push_back(clipboard_pdu(9, 1, reply.bytes()));
    } catch (const ProtocolError&) {
        Writer failed; failed.le32(stream); outbound_.push_back(clipboard_pdu(9, 2, failed.bytes()));
    }
}
void ClipboardFiles::accept(std::uint16_t type, std::uint16_t flags, View payload) {
    if (type == 8) { require(flags == 0, "invalid file request flags"); serve(payload); }
    else if (type == 9) response(flags, payload);
    else {
        require(peer_stream_ && peer_lock_ && flags == 0, "unnegotiated clipboard lock");
        Reader in(payload); const auto id = in.le32(); in.end();
        if (type == 10) {
            require(!locked_.contains(id) && locked_.size() < 8, "duplicate clipboard lock or lock quota");
            locked_.emplace(id, published_);
        } else { require(type == 11, "invalid clipboard file PDU"); locked_.erase(id); }
    }
}
void ClipboardFiles::tick(Clock::time_point now) {
    if ((awaiting_list_ || receiving_) && now >= deadline_) fail("clipboard file transfer timed out");
}
std::vector<Bytes> ClipboardFiles::drain() { std::vector<Bytes> value; value.swap(outbound_); return value; }
std::optional<std::vector<std::string>> ClipboardFiles::take_completed() { auto value = std::move(completed_); completed_.reset(); return value; }
std::optional<std::string> ClipboardFiles::take_error() { auto value = std::move(error_); error_.reset(); return value; }
} // namespace lrdp
