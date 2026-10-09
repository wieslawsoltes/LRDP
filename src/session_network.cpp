#include "lrdp/session.hpp"

namespace lrdp {
void Session::configure_network_metrics(NetworkPolicy policy) {
    require(phase_ == SessionPhase::connect && !network_policy_, "configure network measurements before connection");
    (void)NetworkAutodetect(1004, policy); // Validate before publishing configuration.
    network_policy_ = policy;
}
void Session::negotiate_network() {
    if (network_policy_ && (settings_.early_caps & 0x0080)) {
        // The message channel is not included in SC_NET's static-channel array.
        // CS_MCS_MSGCHANNEL is not required when Client Core advertises netchar.
        settings_.message_channel = std::uint16_t(global_channel + 1 + settings_.channel_ids.size());
        network_.emplace(settings_.message_channel, *network_policy_);
    }
}
bool Session::receive_network(std::uint16_t channel, View payload) {
    if (!network_ || channel != network_->channel()) return false;
    require(phase_ == SessionPhase::active || phase_ == SessionPhase::confirm || phase_ == SessionPhase::finalize,
            "network measurement before RDP activation");
    network_->receive(payload, NetworkAutodetect::Clock::now()); last_packet_network_ = true; return true;
}
NetworkBatch Session::poll_network(NetworkAutodetect::Time now, bool idle, bool pending) {
    return network_ ? network_->poll(now, active(), idle, pending) : NetworkBatch{};
}
void Session::network_transmitted(View packet, std::uint64_t bytes, NetworkAutodetect::Time now) {
    if (!network_) return;
    if (!network_->started() && !packet.empty() && packet[0] == 3) {
        Reader mcs(parse_x224_data(packet));
        if (mcs.u8() == 0x68) {
            mcs.skip(2); const auto channel = mcs.be16(); mcs.skip(1);
            const auto body = mcs.take(mcs.per_length()); mcs.end();
            // Font Map finishes RDP activation. New probes cannot overtake it,
            // including when activation messages were buffered behind TLS writes.
            if (channel == global_channel && body.size() >= 18 && body[2] == 0x17 && body[3] == 0 && body[14] == 40)
                network_->start(now);
        }
    }
    network_->transmitted(packet, bytes, now);
}
} // namespace lrdp
