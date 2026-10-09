#include "lrdp/graphics.hpp"
#include "lrdp/session.hpp"
namespace lrdp {
void Graphics::lossless_frame(const Frame& frame) {
    require(!video_enabled_ && ready() && frame.width==width_ && frame.height==height_,
            "lossless frame does not match a negotiated non-video surface");
    require(can_send(),"lossless frame acknowledgement window is full");
    auto commands=lossless_.encode(frame);
    if(commands.empty()) return;
    const auto id=begin_frame();
    for(const auto& command:commands) outbound_.push_back(graphics_segments(command));
    end_frame(id);
}
void Session::configure_lossless_graphics() {
    require(phase_==SessionPhase::connect && graphics_enabled_,"lossless graphics must be configured before connection");
    lossless_graphics_=true;
    video_factory_={}; // Never advertise lossy H.264 in explicitly lossless mode.
}
} // namespace lrdp
