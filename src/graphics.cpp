#include "lrdp/graphics.hpp"
#include "lrdp/video.hpp"
#include <set>
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
            // MS-RDPEGFX 3.2.5.18 permits a complete in-channel reset from 10.3 onward.
            require(!negotiated_ || version_ >= 0x000a0301,
                    "graphics capability reset requires previously negotiated version 10.3+");
            const auto count = in.le16(); require(count > 0 && count <= 64, "invalid graphics capability count");
            struct Choice {
                unsigned score = 0;
                std::uint32_t version = 0, flags = 0, length = 4;
                bool video = false;
                VideoCodec codec = VideoCodec::avc420;
            } selected;
            std::set<std::uint32_t> seen;
            for (unsigned i = 0; i < count; ++i) {
                const auto version = in.le32(), length = in.le32();
                require(length <= 4096 && seen.insert(version).second, "oversized or duplicate graphics capability");
                Reader cap(in.take(length)); Choice choice; choice.version = version;
                if (version == 0x00080105) {
                    require(length == 4, "invalid graphics 8.1 capability");
                    choice.video = video_available && (cap.le32() & 0x10);
                    choice.flags = 2U | (choice.video ? 0x10U : 0U);
                    choice.score = choice.video ? 10U : 1U;
                } else if (version == 0x000a0002 || version == 0x000a0200 ||
                           version == 0x000a0301 || version == 0x000a0400 ||
                           version == 0x000a0502 || version == 0x000a0600 || version == 0x000a0701) {
                    require(length == 4, "invalid graphics 10.x capability");
                    const auto flags = cap.le32();
                    require(version < 0x000a0301 || (flags & 0x60) != 0x60,
                            "AVC_THINCLIENT conflicts with AVC_DISABLED");
                    choice.video = video_available && !(flags & 0x20);
                    // These sets mandate YUV444. Select v1 conservatively; only
                    // the explicit 10.1 capability selects the existing v2 path.
                    choice.codec = VideoCodec::avc444;
                    choice.flags = (version == 0x000a0301 ? 0U : 2U) |
                                   (choice.video ? 0U : 0x20U);
                    // LRDP maps to unscaled outputs; never promise scaled mapping.
                    if (version == 0x000a0701) choice.flags |= 0x80U;
                    choice.score = choice.video ? 20U : 2U;
                } else if (version == 0x000a0100) {
                    // 10.1 has 16 reserved bytes, not the 4-byte 10.x flags field.
                    // It mandates AVC444v2; do not select it without an encoder.
                    require(length == 16, "invalid graphics 10.1 capability"); cap.skip(16);
                    choice.length = 16; choice.video = video_available;
                    choice.codec = VideoCodec::avc444v2; choice.score = choice.video ? 30U : 0U;
                }
                if (choice.score > selected.score ||
                    (choice.score && choice.score == selected.score && choice.version > selected.version))
                    selected = choice;
            }
            in.end();
            if (!selected.score) return false;
            Writer confirm; confirm.le32(selected.version).le32(selected.length);
            if (selected.length == 16) confirm.zeros(16); else confirm.le32(selected.flags);
            auto confirmation = graphics_segments(graphics_pdu(0x13, confirm.bytes()));
            require(frame_id_ < std::numeric_limits<std::uint32_t>::max() &&
                    negotiation_generation_ < std::numeric_limits<std::uint64_t>::max(),
                    "graphics identifiers exhausted");
            // Build the reply before mutation. The client has discarded every
            // pre-confirmation surface/cache/decoder and old frame credit.
            std::vector<Bytes> next; next.push_back(std::move(confirmation));
            outbound_.swap(next); pending_.clear(); surface_ = false;
            // A capability reset discards the client ClearCodec decoder too.
            // Ordinary desktop resize continues to use invalidate(), not this.
            lossless_ = LosslessEncoder{};
            width_ = height_ = 0; queue_depth_ = 0; acknowledgements_ = true;
            first_generation_frame_ = frame_id_ + 1; telemetry_ = {};
            codec_ = selected.codec; video_enabled_ = selected.video; negotiated_ = true;
            version_ = selected.version; ++negotiation_generation_;
        } else if (command == 0x0d) {
            require(negotiated_, "graphics acknowledgement before capabilities");
            const auto depth = in.le32(), id = in.le32(); (void)in.le32(); in.end();
            require(id <= frame_id_, "graphics acknowledgement refers to an unsent frame");
            // Late acknowledgements from the discarded generation are harmless.
            if (id < first_generation_frame_) continue;
            queue_depth_ = depth; acknowledgements_ = depth != 0xffffffffU;
            if (!acknowledgements_) pending_.clear();
            else while (!pending_.empty() && pending_.front().first <= id) pending_.pop_front();
        } else if (command == 0x16) {
            // MS-RDPEGFX 2.2.2.21 / 3.2.5.21: informational, NOT a frame ACK.
            require(negotiated_ && version_ >= 0x000a0002 && version_ != 0x000a0100,
                    "QoE telemetry was not negotiated");
            GraphicsQoeSample sample;
            sample.frame_id = in.le32(); sample.timestamp = in.le32();
            sample.decode_ms = in.le16(); sample.render_ms = in.le16(); in.end();
            require(sample.frame_id > 0 && sample.frame_id <= frame_id_, "QoE annotation refers to an unsent frame");
            if (sample.frame_id < first_generation_frame_) continue;
            auto& stats = telemetry_;
            if (stats.qoe_samples != std::numeric_limits<std::uint64_t>::max()) ++stats.qoe_samples;
            if (stats.latest_qoe && sample.frame_id <= stats.latest_qoe->frame_id) continue;
            if (!stats.latest_qoe) sample.client_elapsed_ms = 0;
            else {
                const auto delta = std::uint32_t(sample.timestamp - stats.latest_qoe->timestamp);
                if (delta <= 0x7fffffffU && stats.latest_qoe->client_elapsed_ms &&
                    *stats.latest_qoe->client_elapsed_ms <= std::numeric_limits<std::uint64_t>::max() - delta)
                    sample.client_elapsed_ms = *stats.latest_qoe->client_elapsed_ms + delta;
                else if (stats.timestamp_discontinuities != std::numeric_limits<std::uint64_t>::max())
                    ++stats.timestamp_discontinuities;
            }
            stats.latest_qoe = sample;
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
    lossless_.invalidate();
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
    video_bits(annex_b, {}, width, height, qp, VideoCodec::avc420);
}
void Graphics::video_frame(const EncodedVideo& frame) {
    video_bits(frame.annex_b, frame.auxiliary, frame.width, frame.height, frame.qp, frame.codec);
}
void Graphics::video_bits(View primary, View auxiliary, unsigned width, unsigned height,
                          unsigned qp, VideoCodec codec) {
    require(video_enabled_ && codec == codec_ && width == width_ && height == height_,
            "video completion does not match the negotiated surface/codec");
    constexpr std::size_t limit = 8*1024*1024;
    require(qp <= 51 && !primary.empty() && primary.size() <= limit && auxiliary.size() <= limit-primary.size(),
            "invalid AVC frame or quantizer");
    const bool full_chroma = codec != VideoCodec::avc420;
    require(full_chroma ? !auxiliary.empty() : auxiliary.empty(), "invalid AVC picture pair");
    Writer bitmap(limit + 64);
    // LC=0 means both subframes; cbAvc420EncodedBitstream1 includes its 14-byte
    // region/quantizer block. The auxiliary block has its own identical metadata.
    if (full_chroma) bitmap.le32(std::uint32_t(primary.size()+14));
    auto append = [&](View bytes) {
        bitmap.le32(1).le16(0).le16(0).le16(width_).le16(height_).u8(qp).u8(75).raw(bytes);
    };
    append(primary); if (full_chroma) append(auxiliary);
    const auto id = begin_frame();
    surface_bits(codec == VideoCodec::avc420 ? 0x0b : codec == VideoCodec::avc444 ? 0x0e : 0x0f,
                 0, height_, bitmap.bytes());
    end_frame(id);
}
} // namespace lrdp
