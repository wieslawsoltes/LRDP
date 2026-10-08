#include "lrdp/session.hpp"
namespace lrdp {
void Session::static_channel(std::uint16_t channel, View payload) {
    require(channels_started_, "virtual channel data before activation");
    if (receive_audio_static(channel, payload)) return;
    if ((!clipboard_channel_ || channel != *clipboard_channel_) && (!dynamic_channel_ || channel != *dynamic_channel_)) return;
    auto [it, unused] = assemblers_.try_emplace(channel, 1024 * 1024); (void)unused;
    auto complete = it->second.accept(payload); if (!complete) return;
    if (clipboard_channel_ && channel == *clipboard_channel_) {
        auto result = clipboard_.accept(*complete);
        for (const auto& pdu : result.outbound) send_channel(channel, pdu);
        if (result.remote_text) desktop_->set_clipboard(std::move(*result.remote_text));
        return;
    }
    for (const auto& event : dynamic_.accept(*complete)) {
        receive_audio_dynamic(event);
        if (event.kind == DvcEventKind::ready) {
            if (desktop_->resizable() && client_resize_) send_channel(channel, dynamic_.create(1, "Microsoft::Windows::RDS::DisplayControl"));
            if (graphics_requested_) send_channel(channel, dynamic_.create(2, "Microsoft::Windows::RDS::Graphics"));
        } else if (event.id == 1 && event.kind == DvcEventKind::opened) send_dynamic(1, display_caps());
        else if (event.id == 1 && event.kind == DvcEventKind::data) {
            require(active(), "display request during activation"); display_->request(event.data);
        } else if (event.id == 2 && event.kind == DvcEventKind::data) {
            if (!graphics_.receive(event.data, bool(video_factory_))) {
                send_channel(channel, dynamic_.close(2)); graphics_requested_ = false;
                graphics_status_ = "No implemented GFX capability offered; using bitmap updates";
            } else {
                if (!graphics_.ready()) {
                    graphics_.reset(active_layout_); graphics_reset_ = false; previous_graphics_ = {};
                    if (graphics_.avc420()) video_ = std::make_unique<VideoWorker>(video_factory_);
                    graphics_status_ = graphics_.avc420() ? "AVC420 negotiated; encoder initialization pending" : "GFX uncompressed BGRA";
                }
                flush_graphics();
            }
        } else if (event.id == 2 && (event.kind == DvcEventKind::rejected || event.kind == DvcEventKind::closed)) {
            graphics_requested_ = false; video_.reset(); bitmap_.invalidate();
            graphics_status_ = "GFX channel unavailable; using bitmap updates";
        }
    }
}
} // namespace lrdp
