#include "lrdp/session.hpp"
#include "lrdp/monitor_layout.hpp"
#include <algorithm>

namespace lrdp {
namespace {
void validate_client_info(View payload) {
    Reader in(payload); require(in.le16() == 0x40 && in.le16() == 0, "expected protected Client Info");
    in.skip(4); const auto flags = in.le32(); require(flags & 0x10, "Unicode Client Info required");
    unsigned lengths[5];
    for (auto& n : lengths) { n = in.le16(); require(n <= 4096 && !(n & 1), "Client Info string exceeds policy"); }
    for (const auto n : lengths) { Reader value(in.take(n + 2)); value.skip(n); require(value.le16() == 0, "unterminated Client Info string"); }
    require(in.remaining() <= 8192, "extended Client Info exceeds policy");
}
}
Session::Session(std::unique_ptr<Desktop> desktop, std::uint32_t requested, std::uint32_t selected, VideoFactory video, bool graphics)
    : desktop_(std::move(desktop)), requested_protocols_(requested), selected_protocol_(selected),
      video_factory_(std::move(video)), graphics_enabled_(graphics) { require(desktop_ != nullptr, "session requires a desktop"); }
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
    invalidate_graphics(); graphics_reset_ = true; active_layout_ = desktop_->layout();
    require(active_layout_.width <= 65535 && active_layout_.height <= 65535, "desktop exceeds basic RDP bounds");
    send_global(demand_active(std::uint16_t(active_layout_.width), std::uint16_t(active_layout_.height), settings_.depth,
                              desktop_->resizable(), desktop_->unicode_input()));
    if (settings_.early_caps & 0x40) send_global(monitor_layout_pdu(active_layout_));
    synchronized_ = control_granted_ = false; phase_ = SessionPhase::confirm;
}
void Session::reactivate() {
    desktop_->release_input(); Writer body; body.le32(share_id).le16(0); send_global(share_control(6, body.bytes())); activate();
}
void Session::receive(View packet) {
    require(phase_ != SessionPhase::closed && !packet.empty(), "session closed or packet empty");
    if (packet[0] != 3) { require(active() || phase_ == SessionPhase::finalize, "fast-path input before Confirm Active"); input_fast(packet); return; }
    const auto payload = parse_x224_data(packet);
    if (phase_ == SessionPhase::connect) {
        settings_ = connect_initial(payload, selected_protocol_);
        graphics_requested_ = graphics_enabled_ && (settings_.early_caps & 0x100);
        if (desktop_->resizable()) {
            Monitor monitor; monitor.width = (settings_.width + 1U) & ~1U; monitor.height = settings_.height;
            const auto initial = settings_.monitors ? *settings_.monitors : validate_layout({monitor});
            require(desktop_->resize(initial), "initial resize rejected by backend");
        }
        display_.emplace(desktop_->layout());
        if (auto it = settings_.channels.find("cliprdr"); it != settings_.channels.end() && desktop_->clipboard_available()) clipboard_channel_ = it->second;
        if (auto it = settings_.channels.find("drdynvc"); it != settings_.channels.end()) dynamic_channel_ = it->second;
        outbound_.push_back(x224_data(connect_response(settings_, requested_protocols_))); phase_ = SessionPhase::erect; return;
    }
    Reader in(payload); const auto command = in.u8();
    if (command == 0x04) {
        require(phase_ == SessionPhase::erect, "MCS ErectDomain out of sequence");
        for (int i = 0; i < 2; ++i) { const auto size = in.per_length(); require(size > 0 && size <= 4, "invalid ErectDomain integer"); in.skip(size); }
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
    require(command == 0x64, "unsupported MCS command");
    require(in.be16() == client_user - 1001, "wrong MCS SendData initiator");
    const auto channel = in.be16(); require(joined_.contains(channel), "data on unjoined channel");
    require(in.u8() == 0x70, "segmented MCS data was not negotiated");
    const auto data = in.take(in.per_length()); in.end();
    if (phase_ == SessionPhase::info) {
        require(channel == global_channel, "Client Info must use global channel");
        validate_client_info(data); send_global(valid_client_license()); activate(); return;
    }
    if (channel == global_channel) {
        Reader shares(data);
        while (!shares.empty()) {
            require(shares.remaining() >= 6, "truncated Share Control header");
            Reader header(data.subspan(shares.position())); const auto size = header.le16();
            require(size >= 6, "invalid Share Control length"); share_packet(shares.take(size));
        }
    } else static_channel(channel, data);
}
} // namespace lrdp
