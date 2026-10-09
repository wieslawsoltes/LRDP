#include "lrdp/session.hpp"
namespace lrdp {
namespace {
Bytes control(unsigned action, unsigned grant = 0, unsigned id = 0) {
    Writer out; out.le16(action).le16(grant).le32(id); return share_data(20, out.bytes());
}
}
void Session::share_packet(View payload) {
    Reader in(payload); require(in.le16() == payload.size(), "Share Control length mismatch");
    const auto type = in.le16(); require((type & 0xfff0) == 0x10, "invalid Share Control version");
    require(in.le16() == client_user, "invalid Share Control source");
    if ((type & 15) == 3) {
        require(phase_ == SessionPhase::confirm, "Confirm Active out of sequence");
        require(in.le32() == share_id && in.le16() == server_user, "Confirm Active share/originator mismatch");
        const auto descriptor = in.le16(), combined = in.le16();
        require(descriptor <= 256 && combined >= 4, "invalid Confirm Active lengths"); in.skip(descriptor);
        Reader capabilities(in.take(combined)); in.end();
        const auto count = capabilities.le16(); capabilities.skip(2); require(count <= 64, "too many capabilities");
        std::set<unsigned> types; bool bitmap_seen = false;
        unsigned color_slots = 0, alpha_slots = 0, large_flags = 0;
        std::uint32_t max_request = 0;
        bool fastpath = false;
        for (unsigned i = 0; i < count; ++i) {
            const auto kind = capabilities.le16(), size = capabilities.le16();
            require(size >= 4 && types.insert(kind).second, "invalid or duplicate capability");
            Reader cap(capabilities.take(size - 4));
            if (kind == 1) {
                require(size == 24, "invalid general capability length");
                cap.skip(10); fastpath = (cap.le16() & 1) != 0;
            } else if (kind == 26) {
                require(size == 8, "invalid multifragment capability length"); max_request = cap.le32();
            } else if (kind == 27) {
                require(size == 6, "invalid large pointer capability length"); large_flags = cap.le16();
            } else if (kind == 8) {
                require(size == 8 || size == 10, "invalid pointer capability length");
                (void)cap.le16(); color_slots = cap.le16();
                alpha_slots = cap.empty() ? 0U : cap.le16();
            } else if (kind == 2) {
                require(size == 28, "invalid bitmap capability length"); bitmap_seen = true;
                const auto depth = cap.le16(); require(depth == 15 || depth == 16 || depth == 24 || depth == 32, "unsupported bitmap depth");
                cap.skip(12); client_resize_ = cap.le16() != 0;
            }
        }
        capabilities.end(); require(bitmap_seen, "client bitmap capability required");
        pointer_.configure(color_slots, alpha_slots, large_flags, max_request, fastpath);
        Writer sync; sync.le16(1).le16(client_user); send_global(share_data(31, sync.bytes())); send_global(control(4));
        phase_ = SessionPhase::finalize; return;
    }
    require((type & 15) == 7 && (phase_ == SessionPhase::finalize || active()), "unexpected Share Data state");
    require(in.le32() == share_id, "Share Data share mismatch"); in.skip(2);
    (void)in.le16(); const auto subtype = in.u8(), compression = in.u8(); const auto compressed = in.le16();
    require(compression == 0 && compressed == 0, "bulk compression was not negotiated");
    // MS-RDPBCGR 4.1.14 / 4.1.18 document inconsistent client uncompressedLength.
    const auto body = in.take(in.remaining()); Reader data(body);
    switch (subtype) {
    case 31:
        require(data.le16() == 1 && data.le16() == server_user, "invalid client synchronize"); data.end(); synchronized_ = true; break;
    case 20: {
        const auto action = data.le16(); data.skip(6); data.end();
        if (action == 1) { require(synchronized_, "control requested before synchronize"); send_global(control(2, client_user, server_user)); control_granted_ = true; }
        else require(action == 4, "unsupported client control action");
        break;
    }
    case 39: {
        require(phase_ == SessionPhase::finalize && synchronized_ && control_granted_, "Font List before finalization");
        data.skip(4); const auto flags = data.le16(); data.skip(2); data.end(); if (!(flags & 2)) break;
        Writer font; font.le16(0).le16(0).le16(3).le16(4); send_global(share_data(40, font.bytes()));
        Writer pointer; pointer.le16(1).le16(0).le32(desktop_->embedded_cursor() ? 0 : 0x7f00); send_global(share_data(27, pointer.bytes()));
        phase_ = SessionPhase::active;
        synchronize_extended();
        if (!channels_started_) {
            channels_started_ = true;
            if (clipboard_channel_) for (const auto& pdu : clipboard_.start()) send_channel(*clipboard_channel_, pdu);
            start_audio();
            if (dynamic_channel_ && ((desktop_->resizable() && client_resize_) || graphics_requested_ || audio_dynamic_needed() || desktop_->extended_capabilities().touches))
                send_channel(*dynamic_channel_, DynamicChannels::capabilities());
        }
        break;
    }
    case 28: input_slow(body); break;
    case 33: {
        const auto count = data.u8(); data.skip(3); require(count > 0 && data.remaining() == std::size_t(count) * 8, "invalid Refresh Rect");
        invalidate_graphics(); break;
    }
    case 35: {
        const auto allow = data.u8(); require(allow <= 1, "invalid Suppress Output flag"); data.skip(3);
        if (allow) { data.skip(8); invalidate_graphics(); } data.end(); suppressed_ = !allow;
        if (suppressed_) release_all_input();
        synchronize_extended();
        break;
    }
    case 36: send_global(share_data(37, {})); break;
    case 43: case 3: break;
    default: throw ProtocolError("unsupported client Share Data PDU");
    }
}
} // namespace lrdp
