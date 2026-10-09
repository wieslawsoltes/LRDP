#pragma once
#include "desktop.hpp"
#include "avc444.hpp"
#include <chrono>
#include <deque>

namespace lrdp {
// MS-RDPEGFX 2.2: client graphics traffic is unwrapped; server traffic is
// RDP_SEGMENTED_DATA. Only an explicitly offered, implemented capability is selected.
struct EncodedVideo;
Bytes graphics_pdu(std::uint16_t command, View payload);
Bytes graphics_segments(View payload);

class Graphics {
    using Clock = std::chrono::steady_clock;
    bool negotiated_ = false, video_enabled_ = false, surface_ = false;
    VideoCodec codec_ = VideoCodec::avc420;
    bool acknowledgements_ = true;
    std::uint32_t frame_id_ = 0, queue_depth_ = 0;
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
    void video_frame(View annex_b, unsigned width, unsigned height, unsigned qp = 22);
    void video_frame(const EncodedVideo& frame);
    [[nodiscard]] bool ready() const { return negotiated_ && surface_; }
    [[nodiscard]] bool avc420() const { return video_enabled_ && codec_ == VideoCodec::avc420; }
    [[nodiscard]] bool video_enabled() const { return video_enabled_; }
    [[nodiscard]] VideoCodec video_codec() const { return codec_; }
    [[nodiscard]] bool can_send() const;
    [[nodiscard]] std::size_t in_flight() const { return pending_.size(); }
    std::vector<Bytes> drain();
};
} // namespace lrdp
