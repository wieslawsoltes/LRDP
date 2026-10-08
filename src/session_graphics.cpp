#include "lrdp/session.hpp"
namespace lrdp {
void Session::invalidate_graphics() { ++graphics_generation_; previous_graphics_ = {}; bitmap_.invalidate(); }
void Session::flush_graphics() { for (const auto& packet : graphics_.drain()) send_dynamic(2, packet); }
void Session::tick(bool transport_ready, bool capture_due) {
    desktop_->pump();
    if (!active()) return;
    tick_audio(); // Audio continues while graphics are suppressed or backpressured.
    if (clipboard_channel_) if (auto text = desktop_->poll_clipboard())
        for (const auto& pdu : clipboard_.set_local(std::move(*text))) send_channel(*clipboard_channel_, pdu);
    if (!transport_ready) return;
    const auto actual = desktop_->layout();
    if (actual.monitors != active_layout_.monitors) { display_.emplace(actual); reactivate(); return; }
    if (display_ && display_->commit([&](const Layout& layout) { return desktop_->resize(layout); })) { reactivate(); return; }
    if (suppressed_) return;
    if (graphics_requested_ && graphics_.ready()) {
        if (graphics_reset_) { graphics_.reset(active_layout_); graphics_reset_ = false; flush_graphics(); }
        if (!graphics_.can_send()) return;
        if (video_) {
            if (auto result = video_->take()) {
                if (!result->error.empty()) {
                    graphics_status_ = "Video encoder failed; using GFX BGRA: " + result->error;
                    video_.reset(); previous_graphics_ = {};
                } else if (result->generation == graphics_generation_) {
                    require(result->frame.has_value(), "encoder returned an empty completion");
                    const auto& frame = *result->frame;
                    graphics_.video_frame(frame.annex_b, frame.width, frame.height);
                    graphics_status_ = frame.encoder + (frame.hardware ? " (hardware encode; CPU capture/upload)" : " (software encode)");
                    flush_graphics(); return;
                }
            }
            if (video_ && !video_->available()) return;
        }
        if (!capture_due) return;
        auto frame = desktop_->capture();
        const bool key_frame = previous_graphics_.width != frame.width || previous_graphics_.height != frame.height;
        if (!key_frame && frame.bgra == previous_graphics_.bgra) return;
        previous_graphics_ = frame;
        if (video_) video_->submit(std::move(frame), graphics_generation_, key_frame);
        else { graphics_.raw_frame(frame); flush_graphics(); }
        return;
    }
    if (!capture_due) return;
    for (auto& packet : bitmap_.encode(desktop_->capture(), settings_.depth)) outbound_.push_back(std::move(packet));
}
} // namespace lrdp
