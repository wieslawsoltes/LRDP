#pragma once
#include "desktop.hpp"
#include "avc444.hpp"
#include "lossless.hpp"
#include <chrono>
#include <deque>

namespace lrdp {
// MS-RDPEGFX 2.2: client graphics traffic is unwrapped; server traffic is
// RDP_SEGMENTED_DATA. Only an explicitly offered, implemented capability is selected.
struct EncodedVideo;
Bytes graphics_pdu(std::uint16_t command, View payload);
Bytes graphics_segments(View payload);

// Client telemetry is advisory and must never release frame credits.
struct GraphicsQoe {
    std::uint32_t frame_id = 0, timestamp = 0;
    std::uint16_t decode_ms = 0, render_ms = 0;
    // Relative to the first sample; absent after a backward/ambiguous timestamp.
    std::optional<std::uint64_t> client_elapsed_ms;
};
using GraphicsQoeSample = GraphicsQoe;
struct GraphicsTelemetry {
    std::uint64_t qoe_samples = 0, timestamp_discontinuities = 0;
    std::optional<GraphicsQoe> latest_qoe;
};

class Graphics {
    using Clock = std::chrono::steady_clock;
    LosslessEncoder lossless_;
    bool negotiated_ = false, video_enabled_ = false, surface_ = false;
    VideoCodec codec_ = VideoCodec::avc420;
    bool acknowledgements_ = true;
    std::uint32_t frame_id_ = 0, queue_depth_ = 0;
    std::uint32_t version_ = 0, first_generation_frame_ = 1;
    std::uint64_t negotiation_generation_ = 0;
    GraphicsTelemetry telemetry_;
    std::uint16_t width_ = 0, height_ = 0;
    std::deque<std::pair<std::uint32_t, Clock::time_point>> pending_;
    std::vector<Bytes> outbound_;
    void emit(std::uint16_t command, View payload);
    std::uint32_t begin_frame();
    void end_frame(std::uint32_t id);
    void surface_bits(std::uint16_t codec, unsigned top, unsigned bottom, View bitmap);
    void video_bits(View primary, View auxiliary, unsigned width, unsigned height, unsigned qp, VideoCodec codec);
public:
    // Returns false only when no implemented graphics version was offered.
    bool receive(View message, bool video_available);
    void reset(const Layout& layout);
    void raw_frame(const Frame& frame);
    void lossless_frame(const Frame& frame);
    void invalidate_lossless() { lossless_.invalidate(); }
    const LosslessStatistics& lossless_statistics() const { return lossless_.statistics(); }
    void video_frame(View annex_b, unsigned width, unsigned height, unsigned qp = 22);
    void video_frame(const EncodedVideo& frame);
    [[nodiscard]] const std::optional<GraphicsQoe>& latest_qoe() const { return telemetry_.latest_qoe; }
    [[nodiscard]] std::uint32_t capability_version() const { return version_; }
    [[nodiscard]] std::uint32_t version() const { return version_; }
    [[nodiscard]] std::uint64_t negotiation_generation() const { return negotiation_generation_; }
    [[nodiscard]] const GraphicsTelemetry& telemetry() const { return telemetry_; }
    [[nodiscard]] bool ready() const { return negotiated_ && surface_; }
    [[nodiscard]] bool avc420() const { return video_enabled_ && codec_ == VideoCodec::avc420; }
    [[nodiscard]] bool video_enabled() const { return video_enabled_; }
    [[nodiscard]] VideoCodec video_codec() const { return codec_; }
    [[nodiscard]] bool can_send() const;
    [[nodiscard]] std::size_t in_flight() const { return pending_.size(); }
    std::vector<Bytes> drain();
};
} // namespace lrdp
