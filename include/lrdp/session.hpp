#pragma once
#include "channels.hpp"
#include "clipboard.hpp"
#include "desktop.hpp"
#include "mcs.hpp"
#include <set>

namespace lrdp {
enum class SessionPhase { connect, erect, attach, join, info, confirm, finalize, active, closed };
class Session {
    std::unique_ptr<Desktop> desktop_;
    ClientSettings settings_;
    SessionPhase phase_ = SessionPhase::connect;
    std::uint32_t requested_protocols_, selected_protocol_;
    std::set<std::uint16_t> joined_;
    Clipboard clipboard_;
    DynamicChannels dynamic_;
    std::map<std::uint16_t, ChannelAssembler> assemblers_;
    std::optional<std::uint16_t> clipboard_channel_, dynamic_channel_;
    std::optional<DisplayController> display_;
    BitmapEncoder bitmap_;
    bool channels_started_ = false, suppressed_ = false;
    bool synchronized_ = false, control_granted_ = false, client_resize_ = false;
    std::vector<Bytes> outbound_;
    void send_global(View data);
    void send_channel(std::uint16_t id, View message);
    void send_dynamic(std::uint32_t id, View message);
    void activate();
    void share_packet(View payload);
    void static_channel(std::uint16_t channel, View payload);
    void input_slow(View payload);
    void input_fast(View packet);
public:
    explicit Session(std::unique_ptr<Desktop> desktop, std::uint32_t requested_protocols,
                     std::uint32_t selected_protocol = 1);
    ~Session();
    void receive(View packet);
    void tick(bool graphics_ready = true);
    std::vector<Bytes> drain();
    SessionPhase phase() const { return phase_; }
    bool active() const { return phase_ == SessionPhase::active; }
    const ClientSettings& settings() const { return settings_; }
};
} // namespace lrdp
