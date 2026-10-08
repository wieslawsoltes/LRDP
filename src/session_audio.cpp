#include "lrdp/session.hpp"
#include <chrono>
namespace lrdp {
namespace {
std::uint64_t audio_time() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
void Session::configure_audio(std::unique_ptr<AudioDevices> devices) {
    require(phase_ == SessionPhase::connect && !audio_ && devices != nullptr, "audio must be configured before session negotiation");
    devices->check(); audio_ = std::move(devices);
}
std::vector<Bytes> Session::drain_media() { std::vector<Bytes> packets; packets.swap(media_); return packets; }
void Session::send_media(std::uint16_t channel, View message) {
    for (const auto& fragment : channel_fragments(message)) media_.push_back(mcs_data(channel, fragment));
}
void Session::send_microphone(View message) {
    require(dynamic_channel_.has_value(), "microphone transport unavailable");
    for (const auto& fragment : dynamic_.send(3, message)) send_media(*dynamic_channel_, fragment);
}
bool Session::audio_dynamic_needed() const { return audio_ && audio_->microphone_available(); }
void Session::start_audio() {
    if (!audio_ || !audio_->playback_available()) return;
    const auto channel = settings_.channels.find("rdpsnd"); if (channel == settings_.channels.end()) return;
    sound_channel_ = channel->second;
    // Initialization stays behind Font Map in the ordinary ordered queue.
    send_channel(*sound_channel_, sound_.start(audio_time()));
}
bool Session::receive_audio_static(std::uint16_t channel, View payload) {
    if (!sound_channel_ || channel != *sound_channel_) return false;
    auto [it, inserted] = assemblers_.try_emplace(channel, 65536); (void)inserted;
    if (auto complete = it->second.accept(payload))
        for (const auto& pdu : sound_.receive(*complete, audio_time())) send_media(channel, pdu);
    return true;
}
void Session::receive_audio_dynamic(const DvcEvent& event) {
    if (!audio_dynamic_needed()) return;
    if (event.kind == DvcEventKind::ready) {
        require(dynamic_channel_.has_value(), "microphone DVC transport unavailable");
        send_channel(*dynamic_channel_, dynamic_.create(3, "AUDIO_INPUT"));
    } else if (event.id == 3 && event.kind == DvcEventKind::opened) {
        microphone_started_ = true; send_microphone(microphone_.start(audio_time()));
    } else if (event.id == 3 && event.kind == DvcEventKind::data) {
        auto result = microphone_.receive(event.data, audio_time());
        for (const auto& pdu : result.outbound) send_microphone(pdu);
        if (result.pcm) audio_->feed_microphone(*result.pcm);
    } else if (event.id == 3 && (event.kind == DvcEventKind::closed || event.kind == DvcEventKind::rejected)) microphone_.close();
}
void Session::tick_audio() {
    if (!audio_) return;
    audio_->check(); const auto now = audio_time();
    if (sound_channel_) {
        if (auto close = sound_.poll(now)) send_media(*sound_channel_, *close);
        const bool ready = sound_.ready();
        if (ready != playback_enabled_) { audio_->enable_playback(ready); playback_enabled_ = ready; }
        if (sound_.can_send()) if (auto pcm = audio_->take_playback(960))
            for (const auto& pdu : sound_.send(*pcm, now)) send_media(*sound_channel_, pdu);
    }
    if (microphone_started_) {
        (void)microphone_.poll(now);
        if (microphone_.closed() && dynamic_.is_open(3)) send_media(*dynamic_channel_, dynamic_.close(3));
        const bool ready = microphone_.ready();
        if (ready != microphone_enabled_) { audio_->enable_microphone(ready); microphone_enabled_ = ready; }
    }
}
} // namespace lrdp
