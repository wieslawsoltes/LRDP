#include "lrdp/graphics.hpp"
#include <algorithm>
#include <bit>
#include <limits>

namespace lrdp {
Bytes graphics_pdu(std::uint16_t command, View payload) {
    require(payload.size() <= 16 * 1024 * 1024 - 8, "graphics PDU exceeds quota");
    Writer out; out.le16(command).le16(0).le32(std::uint32_t(payload.size() + 8)).raw(payload);
    return std::move(out).finish();
}
Bytes graphics_segments(View payload) {
    require(payload.size() <= 15 * 1024 * 1024, "segmented graphics message exceeds quota");
    Writer out;
    if (payload.size() <= 65535) out.u8(0xe0).u8(4).raw(payload);
    else {
        const auto count = (payload.size() + 65534) / 65535;
        out.u8(0xe1).le16(unsigned(count)).le32(std::uint32_t(payload.size()));
        for (std::size_t offset = 0; offset < payload.size();) {
            const auto size = std::min<std::size_t>(65535, payload.size() - offset);
            out.le32(std::uint32_t(size + 1)).u8(4).raw(payload.subspan(offset, size)); offset += size;
        }
    }
    return std::move(out).finish();
}
void Graphics::emit(std::uint16_t command, View payload) {
    outbound_.push_back(graphics_segments(graphics_pdu(command, payload)));
}
std::vector<Bytes> Graphics::drain() { std::vector<Bytes> result; result.swap(outbound_); return result; }
bool Graphics::receive(View message, bool video_available) {
    require(!message.empty(), "empty graphics message");
    Reader messages(message);
    while (!messages.empty()) {
        const auto command = messages.le16(); require(messages.le16() == 0, "invalid graphics header flags");
        const auto size = messages.le32(); require(size >= 8, "invalid graphics PDU size");
        Reader in(messages.take(size - 8));
        if (command == 0x12) {
            require(!negotiated_, "duplicate graphics capability advertisement");
            const auto count = in.le16(); require(count > 0 && count <= 64, "invalid graphics capability count");
            std::optional<std::uint32_t> flags;
            for (unsigned i = 0; i < count; ++i) {
                const auto version = in.le32(), length = in.le32();
                require(length <= 4096, "oversized graphics capability"); Reader cap(in.take(length));
                if (version == 0x00080105) {
                    require(!flags && length == 4, "invalid or duplicate graphics 8.1 capability"); flags = cap.le32();
                }
            }
            in.end();
            if (!flags) return false;
            avc420_ = video_available && (*flags & 0x10);
            Writer confirm; confirm.le32(0x00080105).le32(4).le32(2U | (avc420_ ? 0x10U : 0U));
            emit(0x13, confirm.bytes()); negotiated_ = true;
        } else if (command == 0x0d) {
            require(negotiated_, "graphics acknowledgement before capabilities");
            const auto depth = in.le32(), id = in.le32(); (void)in.le32(); in.end();
            require(id <= frame_id_, "graphics acknowledgement refers to an unsent frame");
            queue_depth_ = depth; acknowledgements_ = depth != 0xffffffffU;
            if (!acknowledgements_) pending_.clear();
            else while (!pending_.empty() && pending_.front().first <= id) pending_.pop_front();
        } else if (command == 0x10) {
            require(negotiated_, "graphics cache offer before capabilities");
            const auto count = in.le16();
            require(count < 5462 && in.remaining() == std::size_t(count) * 12, "invalid cache import offer");
            // No persistent client cache is trusted or used by the initial backend.
            Writer reply; reply.le16(0); emit(0x11, reply.bytes());
        } else throw ProtocolError("unsupported client graphics PDU");
    }
    return true;
}
void Graphics::reset(const Layout& layout) {
    require(negotiated_, "graphics reset before capabilities");
    const auto validated = validate_layout(layout.monitors);
    require(validated.width == layout.width && validated.height == layout.height, "inconsistent graphics layout");
    if (surface_) { Writer id; id.le16(0); emit(0x0a, id.bytes()); }
    Writer reset(340 - 8); reset.le32(layout.width).le32(layout.height).le32(unsigned(layout.monitors.size()));
    for (const auto& monitor : layout.monitors) {
        reset.le32(std::bit_cast<std::uint32_t>(monitor.left)).le32(std::bit_cast<std::uint32_t>(monitor.top))
            .le32(std::uint32_t(std::int64_t(monitor.left) + monitor.width - 1))
            .le32(std::uint32_t(std::int64_t(monitor.top) + monitor.height - 1)).le32(monitor.flags);
    }
    reset.zeros(332 - reset.size()); emit(0x0e, reset.bytes());
    width_ = std::uint16_t(layout.width); height_ = std::uint16_t(layout.height);
    Writer surface; surface.le16(0).le16(width_).le16(height_).u8(0x20); emit(9, surface.bytes());
    Writer mapping; mapping.le16(0).le16(0).le32(0).le32(0); emit(0x0f, mapping.bytes());
    surface_ = true;
}
bool Graphics::can_send() const {
    if (!ready()) return false;
    if (!pending_.empty()) require(Clock::now() - pending_.front().second < std::chrono::seconds(10), "graphics acknowledgement deadline exceeded");
    const auto window = queue_depth_ > 4 * 1024 * 1024 && queue_depth_ != 0xffffffffU ? 1U : 2U;
    return !acknowledgements_ || pending_.size() < window;
}
std::uint32_t Graphics::begin_frame() {
    require(can_send(), "graphics frame window is full");
    require(frame_id_ < std::numeric_limits<std::uint32_t>::max(), "graphics frame identifiers exhausted; reconnect required");
    const auto id = ++frame_id_; Writer start; start.le32(0).le32(id); emit(0x0b, start.bytes());
    return id;
}
void Graphics::end_frame(std::uint32_t id) {
    Writer end; end.le32(id); emit(0x0c, end.bytes());
    if (acknowledgements_) pending_.emplace_back(id, Clock::now());
}
void Graphics::surface_bits(std::uint16_t codec, unsigned top, unsigned bottom, View bitmap) {
    Writer surface; surface.le16(0).le16(codec).u8(0x20).le16(0).le16(top).le16(width_).le16(bottom)
        .le32(std::uint32_t(bitmap.size())).raw(bitmap); emit(1, surface.bytes());
}
void Graphics::raw_frame(const Frame& frame) {
    frame.validate(); require(frame.width == width_ && frame.height == height_, "graphics frame dimensions changed without reset");
    const auto id = begin_frame();
    // Banded top-down BGRA keeps individual DVC messages below their allocation quota.
    for (unsigned top = 0; top < frame.height; top += 64) {
        const auto bottom = std::min(top + 64, frame.height);
        surface_bits(0, top, bottom, View(frame.bgra).subspan(std::size_t(top) * frame.width * 4, std::size_t(bottom - top) * frame.width * 4));
    }
    end_frame(id);
}
void Graphics::video_frame(View annex_b, unsigned width, unsigned height, unsigned qp) {
    require(avc420_ && width == width_ && height == height_, "AVC420 unavailable or stale video frame");
    require(qp <= 51 && !annex_b.empty() && annex_b.size() <= 8 * 1024 * 1024, "invalid AVC420 frame or quantizer");
    Writer bitmap; bitmap.le32(1).le16(0).le16(0).le16(width_).le16(height_).u8(qp).u8(75).raw(annex_b);
    const auto id = begin_frame(); surface_bits(0x0b, 0, height_, bitmap.bytes()); end_frame(id);
}
} // namespace lrdp
