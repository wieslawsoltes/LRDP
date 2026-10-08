#pragma once
#include "channels.hpp"
#include "clipboard.hpp"
#include "desktop.hpp"
#include "mcs.hpp"
#include "graphics.hpp"
#include "video.hpp"
#include <set>

namespace lrdp {
enum class SessionPhase { connect, erect, attach, join, info, confirm, finalize, active, closed };
class Session {
    std::unique_ptr<Desktop> desktop_;
    ClientSettings settings_;
    Layout active_layout_;
    SessionPhase phase_ = SessionPhase::connect;
    std::uint32_t requested_protocols_, selected_protocol_;
    std::set<std::uint16_t> joined_;
    Clipboard clipboard_;
    DynamicChannels dynamic_;
    std::map<std::uint16_t, ChannelAssembler> assemblers_;
    std::optional<std::uint16_t> clipboard_channel_, dynamic_channel_;
    std::optional<DisplayController> display_;
    BitmapEncoder bitmap_;
    Graphics graphics_;
    VideoFactory video_factory_;
    std::unique_ptr<VideoWorker> video_;
    Frame previous_graphics_;
    std::uint64_t graphics_generation_ = 0;
    bool graphics_enabled_ = true, graphics_requested_ = false, graphics_reset_ = false;
    std::string graphics_status_;
    bool channels_started_ = false, suppressed_ = false;
    bool synchronized_ = false, control_granted_ = false, client_resize_ = false;
    std::vector<Bytes> outbound_;
    void send_global(View data);
    void send_channel(std::uint16_t id, View message);
    void send_dynamic(std::uint32_t id, View message);
    void activate();
    void reactivate();
    void invalidate_graphics();
    void flush_graphics();
    void share_packet(View payload);
    void static_channel(std::uint16_t channel, View payload);
    void input_slow(View payload);
    void input_fast(View packet);
    void dispatch_input(const std::vector<InputEvent>& events);
public:
    explicit Session(std::unique_ptr<Desktop> desktop, std::uint32_t requested_protocols,
                     std::uint32_t selected_protocol = 1, VideoFactory video = {}, bool graphics = true);
    ~Session();
    void receive(View packet);
    void tick(bool graphics_ready = true, bool capture_due = true);
    const std::string& graphics_status() const { return graphics_status_; }
    std::vector<Bytes> drain();
    SessionPhase phase() const { return phase_; }
    bool active() const { return phase_ == SessionPhase::active; }
    const ClientSettings& settings() const { return settings_; }
};
} // namespace lrdp
