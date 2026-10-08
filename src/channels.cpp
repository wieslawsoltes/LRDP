#include "lrdp/channels.hpp"
#include <algorithm>

namespace lrdp {
std::optional<Bytes> ChannelAssembler::accept(View pdu) {
    Reader in(pdu); const auto length = in.le32(); const auto flags = in.le32();
    require(length <= limit_, "static channel message exceeds allocation limit");
    require((flags & ~0x13U) == 0, "unsupported static channel flags or compression");
    if (flags & 1) {
        require(!expected_, "interleaved static channel messages");
        expected_ = length; buffer_.clear();
    }
    require(expected_.has_value() && *expected_ == length, "orphan or inconsistent static fragment");
    require(in.remaining() <= length - buffer_.size(), "static fragment exceeds declared size");
    auto data = in.take(in.remaining()); buffer_.insert(buffer_.end(), data.begin(), data.end());
    if (flags & 2) {
        require(buffer_.size() == length, "premature static LAST fragment");
        expected_.reset(); Bytes complete; complete.swap(buffer_); return complete;
    }
    require(buffer_.size() < length, "static message is missing LAST flag");
    return std::nullopt;
}
std::vector<Bytes> channel_fragments(View message, std::size_t chunk_size) {
    require(chunk_size > 0 && chunk_size <= 1600 && message.size() <= 16 * 1024 * 1024, "invalid static channel fragmentation limits");
    std::vector<Bytes> result;
    std::size_t offset = 0;
    do {
        const auto length = std::min(chunk_size, message.size() - offset);
        const auto flags = (offset == 0 ? 1U : 0U) | (offset + length == message.size() ? 2U : 0U);
        Writer out; out.le32(std::uint32_t(message.size())).le32(flags).raw(message.subspan(offset, length));
        result.push_back(std::move(out).finish()); offset += length;
    } while (offset < message.size());
    return result;
}
Bytes DynamicChannels::capabilities() {
    // Version 1 avoids advertising the version-3 compressed-data commands before a
    // complete receive decompressor is available. ID and length widths remain 32-bit.
    return {0x50, 0, 1, 0};
}
Bytes DynamicChannels::create(std::uint32_t id, std::string_view name, unsigned priority) {
    require(ready(), "DVC capabilities have not been negotiated");
    require(channels_.size() < 64 && !channels_.contains(id), "DVC ID collision or channel limit");
    require(!name.empty() && name.size() <= 255 && name.find('\0') == std::string_view::npos, "invalid DVC listener name");
    require(priority == 0, "priority requires DVC version 2");
    for (unsigned char c : name) require(c >= 32 && c <= 126, "DVC listener must be ASCII");
    Writer out; const auto width = compact_width(id);
    out.u8(0x10 | width).compact(id, width);
    out.raw(View(reinterpret_cast<const std::uint8_t*>(name.data()), name.size())).u8(0);
    channels_.emplace(id, Channel{}); return std::move(out).finish();
}
bool DynamicChannels::is_open(std::uint32_t id) const {
    auto it = channels_.find(id); return it != channels_.end() && it->second.open;
}
std::vector<DvcEvent> DynamicChannels::accept(View pdu) {
    require(pdu.size() <= 1600, "DVC PDU exceeds protocol maximum");
    Reader in(pdu); const auto header = in.u8(); const unsigned command = header >> 4;
    const unsigned width = header & 3, length_width = (header >> 2) & 3;
    if (command == 5) {
        require(!ready() && header == 0x50 && in.u8() == 0, "invalid DVC capability state");
        const auto version = in.le16(); require(version == 1, "unsupported DVC version response");
        in.end(); version_ = version; return {{DvcEventKind::ready, 0, {}}};
    }
    require(ready(), "DVC used before capability exchange");
    const auto id = in.compact(width); auto it = channels_.find(id);
    require(it != channels_.end(), "unknown dynamic channel ID"); auto& channel = it->second;
    if (command == 1) {
        require(!channel.open, "duplicate DVC create response");
        const auto status = in.i32(); in.end();
        if (status < 0) { channels_.erase(it); return {{DvcEventKind::rejected, id, {}}}; }
        channel.open = true; return {{DvcEventKind::opened, id, {}}};
    }
    if (command == 4) {
        in.end(); buffered_ -= channel.partial.size(); channels_.erase(it);
        return {{DvcEventKind::closed, id, {}}};
    }
    require(channel.open, "DVC data before successful create response");
    require(command == 2 || command == 3, "unsupported DVC command");
    if (command == 2) {
        require(!channel.expected, "overlapping DVC DATA_FIRST messages");
        const auto length = in.compact(length_width);
        require(length <= limit_, "DVC message exceeds allocation limit");
        require(length > in.remaining(), "DATA_FIRST must begin a fragmented message");
        channel.expected = length;
    }
    auto data = in.take(in.remaining());
    if (!channel.expected) return {{DvcEventKind::data, id, Bytes(data.begin(), data.end())}};
    require(!data.empty(), "zero-progress DVC fragment");
    require(data.size() <= *channel.expected - channel.partial.size(), "DVC fragment overflow");
    require(data.size() <= limit_ - buffered_, "DVC aggregate buffering limit");
    channel.partial.insert(channel.partial.end(), data.begin(), data.end()); buffered_ += data.size();
    if (channel.partial.size() == *channel.expected) {
        Bytes complete; complete.swap(channel.partial); buffered_ -= complete.size(); channel.expected.reset();
        return {{DvcEventKind::data, id, std::move(complete)}};
    }
    return {};
}
std::vector<Bytes> DynamicChannels::send(std::uint32_t id, View message) const {
    require(is_open(id) && message.size() <= limit_, "DVC unavailable or output too large");
    const auto width = compact_width(id); const auto id_bytes = std::size_t(1) << width;
    const auto length_width = compact_width(std::uint32_t(message.size()));
    std::vector<Bytes> packets; std::size_t offset = 0;
    do {
        const bool first_fragment = offset == 0 && message.size() > 1600 - 1 - id_bytes;
        Writer out(1600);
        out.u8((first_fragment ? (0x20 | length_width << 2) : 0x30) | width).compact(id, width);
        if (first_fragment) out.compact(std::uint32_t(message.size()), length_width);
        const auto count = std::min(message.size() - offset, 1600 - out.size());
        out.raw(message.subspan(offset, count)); packets.push_back(std::move(out).finish()); offset += count;
    } while (offset < message.size());
    return packets;
}
Bytes DynamicChannels::close(std::uint32_t id) {
    auto it = channels_.find(id); require(it != channels_.end(), "unknown DVC close ID");
    const auto width = compact_width(id); Writer out; out.u8(0x40 | width).compact(id, width);
    buffered_ -= it->second.partial.size(); channels_.erase(it); return std::move(out).finish();
}
} // namespace lrdp
