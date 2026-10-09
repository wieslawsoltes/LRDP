#pragma once
#include "channels.hpp"
#include "network_autodetect.hpp"
#include "clipboard.hpp"
#include "desktop.hpp"
#include "mcs.hpp"
#include "graphics.hpp"
#include "video.hpp"
#include "audio/devices.hpp"
#include "audio/output.hpp"
#include "audio/input.hpp"
#include "drive/client.hpp"
#include "printing/jobs.hpp"
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
    std::shared_ptr<drive::Bridge> drive_bridge_;
    std::unique_ptr<printing::Endpoint> printers_;
    void complete_device_replies();
    std::optional<drive::Protocol> drives_;
    std::optional<std::uint16_t> drive_channel_;
    std::deque<drive::Request> drive_requests_;
    void start_drives();
    void tick_drives();
    bool receive_drives(std::uint16_t channel, View payload);
    bool last_packet_network_ = false;
    std::optional<NetworkPolicy> network_policy_;
    std::optional<NetworkAutodetect> network_;
    void negotiate_network();
    bool receive_network(std::uint16_t channel, View payload);
    Clipboard clipboard_;
    DynamicChannels dynamic_;
    std::optional<ExtendedInput> extended_;
    void receive_extended_dynamic(const DvcEvent& event);
    void synchronize_extended();
    void suspend_extended();
    void release_all_input();
    std::map<std::uint16_t, ChannelAssembler> assemblers_;
    std::optional<std::uint16_t> clipboard_channel_, dynamic_channel_, sound_channel_;
    std::optional<DisplayController> display_;
    BitmapEncoder bitmap_;
    PointerEncoder pointer_;
    Graphics graphics_;
    VideoFactory video_factory_;
    bool lossless_graphics_ = false;
    std::unique_ptr<VideoWorker> video_;
    Frame previous_graphics_;
    std::uint64_t graphics_generation_ = 0;
    bool graphics_enabled_ = true, graphics_requested_ = false, graphics_reset_ = false;
    std::string graphics_status_, clipboard_status_;
    void apply_clipboard(ClipboardResult result);
    void tick_clipboard();
    bool channels_started_ = false, suppressed_ = false;
    bool reconnect_peer_ = false, reconnect_activated_ = false;
    bool synchronized_ = false, control_granted_ = false, client_resize_ = false;
    std::unique_ptr<AudioDevices> audio_;
    AudioOutput sound_;
    AudioInput microphone_;
    bool microphone_started_ = false, playback_enabled_ = false, microphone_enabled_ = false;
    std::vector<Bytes> outbound_, media_;
    void send_global(View data);
    void send_channel(std::uint16_t id, View message);
    void send_dynamic(std::uint32_t id, View message);
    void send_media(std::uint16_t channel, View message);
    void send_microphone(View message);
    void start_audio();
    bool audio_dynamic_needed() const;
    bool receive_audio_static(std::uint16_t channel, View payload);
    void receive_audio_dynamic(const DvcEvent& event);
    void tick_audio();
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
    void configure_network_metrics(NetworkPolicy policy = {});
    bool last_packet_was_network() const noexcept { return last_packet_network_; }
    NetworkBatch poll_network(NetworkAutodetect::Time now, bool transport_idle, bool application_data_pending);
    void network_transmitted(View whole_packet, std::uint64_t total_bytes, NetworkAutodetect::Time now);
    const NetworkAutodetect* network_metrics() const noexcept { return network_ ? &*network_ : nullptr; }
    void configure_lossless_graphics();
    void configure_audio(std::unique_ptr<AudioDevices> devices);
    void configure_rich_clipboard();
    void configure_drives(std::shared_ptr<drive::Bridge> bridge);
    void configure_printers(std::unique_ptr<printing::Endpoint> endpoint);
    void configure_file_clipboard(std::shared_ptr<ClipboardFileStore> store, FileClipboardLimits limits = {});
    const std::string& clipboard_status() const { return clipboard_status_; }
    void receive(View packet);
    void tick(bool graphics_ready = true, bool capture_due = true);
    const std::string& graphics_status() const { return graphics_status_; }
    std::vector<Bytes> drain();
    std::vector<Bytes> drain_media();
    SessionPhase phase() const { return phase_; }
    bool active() const { return phase_ == SessionPhase::active; }
    const ClientSettings& settings() const { return settings_; }
};
} // namespace lrdp
