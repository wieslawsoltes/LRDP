#include "lrdp/session.hpp"
#include <algorithm>

namespace lrdp {
namespace {
Bytes control(unsigned action, unsigned grant = 0, unsigned control_id = 0) {
    Writer out; out.le16(action).le16(grant).le32(control_id); return share_data(20, out.bytes());
}
void validate_client_info(View payload) {
    Reader in(payload);
    require(in.le16() == 0x40 && in.le16() == 0, "expected TLS-protected Client Info");
    in.skip(4); const auto flags = in.le32(); require(flags & 0x10, "Unicode Client Info required");
    unsigned lengths[5];
    for (auto& n : lengths) { n = in.le16(); require(n <= 4096 && !(n & 1), "Client Info string exceeds policy"); }
    // Validate strings without persisting credentials or accepting them as authentication.
    for (const auto n : lengths) {
        Reader value(in.take(n + 2));
        value.skip(n); require(value.le16() == 0, "unterminated Client Info string");
    }
    require(in.remaining() <= 8192, "extended Client Info exceeds policy");
}
}
Session::Session(std::unique_ptr<Desktop> desktop, std::uint32_t requested_protocols, std::uint32_t selected_protocol)
    : desktop_(std::move(desktop)), requested_protocols_(requested_protocols), selected_protocol_(selected_protocol) {
    require(desktop_ != nullptr, "a session requires a desktop");
}
Session::~Session() { try { desktop_->release_input(); } catch (...) {} }
void Session::send_global(View data) { outbound_.push_back(mcs_data(global_channel, data)); }
void Session::send_channel(std::uint16_t id, View message) {
    for (const auto& fragment : channel_fragments(message)) outbound_.push_back(mcs_data(id, fragment));
}
void Session::send_dynamic(std::uint32_t id, View message) {
    require(dynamic_channel_.has_value(), "dynamic transport unavailable");
    for (const auto& fragment : dynamic_.send(id, message)) send_channel(*dynamic_channel_, fragment);
}
std::vector<Bytes> Session::drain() { std::vector<Bytes> result; result.swap(outbound_); return result; }
void Session::activate() {
    const auto layout = desktop_->layout();
    require(layout.width <= 65535 && layout.height <= 65535, "desktop does not fit basic RDP bounds");
    send_global(demand_active(std::uint16_t(layout.width), std::uint16_t(layout.height), settings_.depth, desktop_->resizable(), desktop_->unicode_input()));
    synchronized_ = control_granted_ = false; phase_ = SessionPhase::confirm; bitmap_.invalidate();
}
void Session::receive(View packet) {
    require(phase_ != SessionPhase::closed && !packet.empty(), "session is closed or packet empty");
    if (packet[0] != 3) { require(active(), "fast-path input before activation"); input_fast(packet); return; }
    const auto payload = parse_x224_data(packet);
    if (phase_ == SessionPhase::connect) {
        settings_ = connect_initial(payload, selected_protocol_);
        if (desktop_->resizable()) {
            Monitor monitor; monitor.width = (settings_.width + 1U) & ~1U; monitor.height = settings_.height;
            require(desktop_->resize(validate_layout({monitor})), "initial resize rejected by backend");
        }
        display_.emplace(desktop_->layout());
        if (auto it = settings_.channels.find("cliprdr"); it != settings_.channels.end()) clipboard_channel_ = it->second;
        if (auto it = settings_.channels.find("drdynvc"); it != settings_.channels.end()) dynamic_channel_ = it->second;
        outbound_.push_back(x224_data(connect_response(settings_, requested_protocols_)));
        phase_ = SessionPhase::erect; return;
    }
    Reader in(payload); const auto command = in.u8();
    if (command == 0x04) {
        require(phase_ == SessionPhase::erect, "MCS ErectDomain out of sequence");
        // subHeight/subInterval are unconstrained PER integers, each length-prefixed.
        for (int i = 0; i < 2; ++i) { auto length = in.per_length(); require(length > 0 && length <= 4, "invalid ErectDomain integer"); in.skip(length); }
        in.end(); phase_ = SessionPhase::attach; return;
    }
    if (command == 0x28) {
        require(phase_ == SessionPhase::attach, "MCS AttachUser out of sequence"); in.end();
        Writer response; response.u8(0x2e).u8(0).be16(client_user - 1001);
        outbound_.push_back(x224_data(response.bytes())); phase_ = SessionPhase::join; return;
    }
    if (command == 0x38) {
        require(phase_ == SessionPhase::join, "MCS ChannelJoin out of sequence");
        require(in.be16() == client_user - 1001, "wrong MCS initiator"); const auto channel = in.be16(); in.end();
        require(channel == client_user || channel == global_channel ||
                std::find(settings_.channel_ids.begin(), settings_.channel_ids.end(), channel) != settings_.channel_ids.end(), "unauthorized channel join");
        require(joined_.insert(channel).second, "duplicate MCS ChannelJoin");
        Writer response; response.u8(0x3e).u8(0).be16(client_user - 1001).be16(channel).be16(channel);
        outbound_.push_back(x224_data(response.bytes()));
        if (joined_.size() == settings_.channel_ids.size() + 2) phase_ = SessionPhase::info;
        return;
    }
    if ((command & 0xfc) == 0x20) { desktop_->release_input(); phase_ = SessionPhase::closed; return; }
    require(command == 0x64, "unsupported MCS domain command");
    require(in.be16() == client_user - 1001, "wrong MCS SendData initiator");
    const auto channel = in.be16(); require(joined_.contains(channel), "data on unjoined channel");
    require(in.u8() == 0x70, "segmented MCS data is not negotiated");
    const auto data = in.take(in.per_length()); in.end();
    if (phase_ == SessionPhase::info) {
        require(channel == global_channel, "Client Info must use the global channel");
        validate_client_info(data); send_global(valid_client_license()); activate(); return;
    }
    if (channel == global_channel) {
        // An MCS payload can contain more than one Share Control PDU.
        Reader shares(data);
        while (!shares.empty()) {
            require(shares.remaining() >= 6, "truncated Share Control header");
            const auto rest = data.subspan(shares.position()); Reader header(rest); const auto size = header.le16();
            require(size >= 6, "invalid Share Control length"); share_packet(shares.take(size));
        }
    } else static_channel(channel, data);
}
void Session::share_packet(View payload) {
    Reader in(payload); require(in.le16() == payload.size(), "Share Control length mismatch");
    const auto type = in.le16(); require((type & 0xfff0) == 0x10, "invalid Share Control version");
    require(in.le16() == client_user, "invalid Share Control source");
    if ((type & 15) == 3) {
        require(phase_ == SessionPhase::confirm, "Confirm Active out of sequence");
        require(in.le32() == share_id, "Confirm Active share mismatch");
        require(in.le16() == server_user, "invalid Confirm Active originator");
        const auto descriptor = in.le16(), combined = in.le16();
        require(descriptor <= 256 && combined >= 4, "invalid Confirm Active lengths"); in.skip(descriptor);
        Reader capabilities(in.take(combined)); in.end();
        const auto count = capabilities.le16(); capabilities.skip(2); require(count <= 64, "too many capabilities");
        std::set<unsigned> types; bool bitmap_seen = false;
        for (unsigned i = 0; i < count; ++i) {
            const auto kind = capabilities.le16(), size = capabilities.le16();
            require(size >= 4 && types.insert(kind).second, "invalid or duplicate capability");
            Reader cap(capabilities.take(size - 4));
            if (kind == 2) {
                require(size == 28, "invalid bitmap capability length"); bitmap_seen = true;
                const auto depth = cap.le16(); require(depth == 15 || depth == 16 || depth == 24 || depth == 32, "unsupported client bitmap depth");
                cap.skip(12); client_resize_ = cap.le16() != 0;
                // The server's Demand Active depth is authoritative for bitmap output.
            }
        }
        capabilities.end(); require(bitmap_seen, "client bitmap capability required");
        Writer sync; sync.le16(1).le16(client_user); send_global(share_data(31, sync.bytes())); send_global(control(4));
        phase_ = SessionPhase::finalize; return;
    }
    require((type & 15) == 7 && (phase_ == SessionPhase::finalize || active()), "unexpected Share Data state");
    require(in.le32() == share_id, "Share Data share mismatch"); in.skip(2);
    const auto unpacked = in.le16(); const auto subtype = in.u8(), compression = in.u8(); const auto compressed = in.le16();
    require(compression == 0 && compressed == 0, "bulk compression was not negotiated");
    require(unpacked == payload.size(), "Share Data uncompressed length mismatch");
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
        require(phase_ == SessionPhase::finalize && synchronized_ && control_granted_, "Font List before session finalization");
        data.skip(4); const auto flags = data.le16(); data.skip(2); data.end();
        if (!(flags & 2)) break;
        Writer font; font.le16(0).le16(0).le16(3).le16(4); send_global(share_data(40, font.bytes()));
        Writer pointer; pointer.le16(1).le16(0).le32(0x7f00); send_global(share_data(27, pointer.bytes()));
        phase_ = SessionPhase::active;
        if (!channels_started_) {
            channels_started_ = true;
            if (clipboard_channel_) for (const auto& pdu : clipboard_.start()) send_channel(*clipboard_channel_, pdu);
            if (dynamic_channel_ && desktop_->resizable() && client_resize_) send_channel(*dynamic_channel_, DynamicChannels::capabilities());
        }
        break;
    }
    case 28: require(active(), "input before activation"); input_slow(body); break;
    case 33: {
        const auto count = data.u8(); data.skip(3); require(count > 0 && data.remaining() == std::size_t(count) * 8, "invalid Refresh Rect");
        bitmap_.invalidate(); break;
    }
    case 35: {
        const auto allow = data.u8(); require(allow <= 1, "invalid Suppress Output flag"); data.skip(3);
        if (allow) { data.skip(8); bitmap_.invalidate(); } data.end(); suppressed_ = !allow;
        if (suppressed_) desktop_->release_input();
        break;
    }
    case 36: send_global(share_data(37, {})); break; // Remote clients cannot shut down the Linux host.
    case 43: case 3: break; // Client persistent-cache list / update synchronize: no caches negotiated.
    default: throw ProtocolError("unsupported client Share Data PDU");
    }
}
void Session::static_channel(std::uint16_t channel, View payload) {
    require(channels_started_, "virtual channel data before activation");
    if ((!clipboard_channel_ || channel != *clipboard_channel_) && (!dynamic_channel_ || channel != *dynamic_channel_)) return;
    auto [it, unused] = assemblers_.try_emplace(channel, 1024 * 1024); (void)unused;
    auto complete = it->second.accept(payload); if (!complete) return;
    if (clipboard_channel_ && channel == *clipboard_channel_) {
        auto result = clipboard_.accept(*complete);
        for (const auto& pdu : result.outbound) send_channel(channel, pdu);
        if (result.remote_text) desktop_->set_clipboard(std::move(*result.remote_text));
    } else {
        require(desktop_->resizable() && client_resize_, "dynamic display channel unavailable");
        for (const auto& event : dynamic_.accept(*complete)) {
            if (event.kind == DvcEventKind::ready) send_channel(channel, dynamic_.create(1, "Microsoft::Windows::RDS::DisplayControl"));
            else if (event.id == 1 && event.kind == DvcEventKind::opened) send_dynamic(1, display_caps());
            else if (event.id == 1 && event.kind == DvcEventKind::data) { require(active(), "display request during activation"); display_->request(event.data); }
        }
    }
}
void Session::input_slow(View payload) {
    Reader in(payload); const auto count = in.le16(); in.skip(2);
    require(count <= 255 && in.remaining() == std::size_t(count) * 12, "invalid slow-path input batch");
    std::vector<InputEvent> events; events.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        in.skip(4); const auto type = in.le16(); InputEvent event{};
        if (type == 4 || type == 5) { event.kind = type == 4 ? InputKind::scancode : InputKind::unicode; event.flags = in.le16(); event.code = in.le16(); in.skip(2); }
        else if (type == 0x8001 || type == 0x8002) { event.kind = type == 0x8001 ? InputKind::pointer : InputKind::pointer_extended; event.flags = in.le16(); event.x = in.le16(); event.y = in.le16(); }
        else if (type == 0) { event.kind = InputKind::synchronize; in.skip(2); const auto flags = in.le32(); require(flags <= 15, "invalid toggle state"); event.flags = std::uint16_t(flags); }
        else throw ProtocolError("unsupported slow-path input event");
        require(event.kind != InputKind::unicode || desktop_->unicode_input(), "Unicode input is unavailable on this backend");
        events.push_back(event);
    }
    for (const auto& event : events) desktop_->input(event);
}
void Session::input_fast(View packet) {
    Reader in(packet); const auto header = in.u8(); require((header & 0xc3) == 0, "invalid TLS fast-path input header");
    require(in.per_length() == packet.size(), "fast-path input length mismatch");
    unsigned count = (header >> 2) & 15; if (!count) count = in.u8();
    require(count > 0, "empty fast-path input batch");
    std::vector<InputEvent> events; events.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto code = in.u8(); const unsigned kind = code >> 5, flags = code & 31;
        InputEvent event{};
        if (kind == 0 || kind == 4) {
            require((flags & ~(kind == 0 ? 7U : 1U)) == 0, "invalid fast-path keyboard flags");
            event.kind = kind == 0 ? InputKind::scancode : InputKind::unicode;
            event.flags = std::uint16_t((flags & 1 ? 0x8000 : 0) | (flags & 2 ? 0x100 : 0) | (flags & 4 ? 0x200 : 0));
            event.code = kind == 0 ? in.u8() : in.le16();
        } else if (kind == 1 || kind == 2) {
            event.kind = kind == 1 ? InputKind::pointer : InputKind::pointer_extended; event.flags = in.le16(); event.x = in.le16(); event.y = in.le16();
        } else if (kind == 3) { require(flags <= 15, "invalid sync toggles"); event.kind = InputKind::synchronize; event.flags = std::uint16_t(flags); }
        else throw ProtocolError("unnegotiated fast-path input event");
        require(event.kind != InputKind::unicode || desktop_->unicode_input(), "Unicode input is unavailable on this backend");
        events.push_back(event);
    }
    in.end(); // Validate the complete batch before injecting any event into the desktop.
    for (const auto& event : events) desktop_->input(event);
}
void Session::tick(bool graphics_ready) {
    if (!active()) return;
    if (clipboard_channel_) if (auto text = desktop_->poll_clipboard()) {
        for (const auto& pdu : clipboard_.set_local(std::move(*text))) send_channel(*clipboard_channel_, pdu);
    }
    if (!graphics_ready) return;
    if (display_ && display_->commit([&](const Layout& layout) { return desktop_->resize(layout); })) {
        desktop_->release_input();
        Writer body; body.le32(share_id).le16(0); send_global(share_control(6, body.bytes()));
        activate(); return;
    }
    if (suppressed_) return;
    auto packets = bitmap_.encode(desktop_->capture(), settings_.depth);
    for (auto& packet : packets) outbound_.push_back(std::move(packet));
}
} // namespace lrdp
