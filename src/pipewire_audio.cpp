#include "lrdp/audio/devices.hpp"
#include "lrdp/audio/pcm_ring.hpp"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unistd.h>
namespace lrdp {
namespace {
struct AudioEndpoint {
    pw_stream* stream = nullptr;
    spa_hook listener{};
    PcmFormat format;
    PcmRing ring;
    const bool sink;
    std::atomic<bool> enabled{false}, failed{false};
    std::string name;
    explicit AudioEndpoint(bool playback)
        : format{48000, std::uint16_t(playback ? 2 : 1), 16}, ring(playback ? 32768 : 16384, format.block_size()), sink(playback),
          name("lrdp." + std::to_string(getpid()) + (playback ? ".speakers" : ".microphone")) {}
    ~AudioEndpoint() { if (stream) { spa_hook_remove(&listener); pw_stream_destroy(stream); } }
    void process() noexcept {
        auto* packet = pw_stream_dequeue_buffer(stream); if (!packet) return;
        struct Return { pw_stream* stream; pw_buffer* packet; ~Return() { pw_stream_queue_buffer(stream, packet); } } returned{stream, packet};
        auto* buffer = packet->buffer;
        if (!buffer || buffer->n_datas != 1) { failed.store(true); return; }
        auto& data = buffer->datas[0]; const auto block = format.block_size();
        if (!data.data || !data.chunk || data.maxsize < block || (data.type != SPA_DATA_MemPtr && data.type != SPA_DATA_MemFd)) {
            failed.store(true); return;
        }
        auto* bytes = static_cast<std::uint8_t*>(data.data);
        if (sink) {
            if (!enabled.load() || !data.chunk->size || (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) return;
            const auto offset = data.chunk->offset % data.maxsize, size = std::min(data.chunk->size, data.maxsize);
            if (offset % block || size % block || data.maxsize % block || size > 65536 ||
                (data.chunk->stride != 0 && data.chunk->stride != block)) { failed.store(true); return; }
            const auto first = std::min(size, data.maxsize - offset);
            (void)ring.push(View(bytes + offset, first)); if (first < size) (void)ring.push(View(bytes, size - first));
        } else {
            auto frames = std::min<std::uint64_t>(data.maxsize / block, 4096);
            if (packet->requested) frames = std::min(frames, packet->requested);
            const auto size = std::size_t(frames) * block; std::memset(bytes, 0, size);
            if (enabled.load()) (void)ring.pop(std::span(bytes, size), false, format.byte_rate() / 10);
            else ring.discard();
            data.chunk->offset = 0; data.chunk->size = std::uint32_t(size); data.chunk->stride = block; packet->size = frames;
        }
    }
    void connect(pw_core* core) {
        stream = pw_stream_new(core, sink ? "LRDP remote speakers" : "LRDP remote microphone", pw_properties_new(
            PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION, sink ? "LRDP Remote Speakers" : "LRDP Remote Microphone",
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS, sink ? "Audio/Sink" : "Audio/Source",
            PW_KEY_MEDIA_CATEGORY, sink ? "Capture" : "Playback", PW_KEY_MEDIA_ROLE, "Communication",
            "node.virtual", "true", "node.want-driver", "true", "node.latency", "960/48000",
            // Virtual Audio/Sink and Audio/Source adapters need their DSP ports
            // configured explicitly when no desktop session manager does it.
            "adapter.auto-port-config", "{ mode = dsp monitor = false control = false position = preserve }",
            "audio.position", sink ? "[ FL FR ]" : "[ MONO ]", nullptr));
        require(stream != nullptr, "cannot create virtual PipeWire audio device");
        static const pw_stream_events events = [] {
            pw_stream_events e{}; e.version = PW_VERSION_STREAM_EVENTS;
            e.state_changed = [](void* data, pw_stream_state old, pw_stream_state next, const char*) {
                if (next == PW_STREAM_STATE_ERROR || (next == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED))
                    static_cast<AudioEndpoint*>(data)->failed.store(true);
            };
            e.param_changed = [](void* data, std::uint32_t id, const spa_pod* pod) {
                if (id != SPA_PARAM_Format || !pod) return;
                auto& self = *static_cast<AudioEndpoint*>(data); spa_audio_info_raw actual{};
                if (spa_format_audio_raw_parse(pod, &actual) < 0 || actual.format != SPA_AUDIO_FORMAT_S16_LE ||
                    actual.rate != self.format.rate || actual.channels != self.format.channels) self.failed.store(true);
            };
            e.process = [](void* data) { static_cast<AudioEndpoint*>(data)->process(); }; return e;
        }();
        pw_stream_add_listener(stream, &listener, &events, this);
        std::uint8_t bytes[1024]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
        spa_audio_info_raw info{}; info.format = SPA_AUDIO_FORMAT_S16_LE; info.rate = format.rate; info.channels = format.channels;
        info.position[0] = sink ? SPA_AUDIO_CHANNEL_FL : SPA_AUDIO_CHANNEL_MONO; if (sink) info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* parameter = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
        require(pw_stream_connect(stream, sink ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY,
            pw_stream_flags(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE), &parameter, 1) >= 0,
            "cannot publish virtual PipeWire audio device");
    }
};
class PipeWireAudio final : public AudioDevices {
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    spa_hook core_listener_{};
    bool running_ = false;
    std::atomic<bool> failed_{false};
    std::unique_ptr<AudioEndpoint> playback_, microphone_;
    void cleanup() noexcept {
        if (running_) { pw_thread_loop_stop(loop_); running_ = false; }
        playback_.reset(); microphone_.reset();
        if (core_) { spa_hook_remove(&core_listener_); pw_core_disconnect(core_); core_ = nullptr; }
        if (context_) { pw_context_destroy(context_); context_ = nullptr; }
        if (loop_) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
    }
    void enable(AudioEndpoint* endpoint, bool enabled) {
        if (!endpoint) { require(!enabled, "audio device was not enabled by the operator"); return; }
        check(); pw_thread_loop_lock(loop_);
        if (endpoint->enabled.load() != enabled) {
            endpoint->enabled.store(enabled);
            const auto result = pw_stream_set_active(endpoint->stream, enabled);
            if (!enabled && endpoint->sink) endpoint->ring.discard();
            if (result < 0) endpoint->failed.store(true);
        }
        pw_thread_loop_unlock(loop_); check();
    }
public:
    explicit PipeWireAudio(AudioOptions options) {
        require(options.playback || options.microphone, "no audio devices requested");
        try {
            static std::once_flag initialized; std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
            loop_ = pw_thread_loop_new("lrdp-audio", nullptr); require(loop_ != nullptr, "cannot create audio loop");
            context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0); require(context_ != nullptr, "cannot create audio context");
            core_ = pw_context_connect(context_, nullptr, 0); require(core_ != nullptr, "cannot connect to the user's PipeWire audio service");
            static const pw_core_events events = [] {
                pw_core_events e{}; e.version = PW_VERSION_CORE_EVENTS;
                e.error = [](void* data, std::uint32_t, int, int, const char*) { static_cast<PipeWireAudio*>(data)->failed_.store(true); }; return e;
            }();
            pw_core_add_listener(core_, &core_listener_, &events, this);
            if (options.playback) { playback_ = std::make_unique<AudioEndpoint>(true); playback_->connect(core_); }
            if (options.microphone) { microphone_ = std::make_unique<AudioEndpoint>(false); microphone_->connect(core_); }
            require(pw_thread_loop_start(loop_) >= 0, "cannot start audio loop"); running_ = true;
        } catch (...) { cleanup(); throw; }
    }
    ~PipeWireAudio() override { cleanup(); }
    bool playback_available() const override { return playback_ != nullptr; }
    bool microphone_available() const override { return microphone_ != nullptr; }
    void check() const override {
        require(!failed_.load() && (!playback_ || !playback_->failed.load()) && (!microphone_ || !microphone_->failed.load()),
                "PipeWire audio device failed or was disconnected");
    }
    void enable_playback(bool enabled) override { enable(playback_.get(), enabled); }
    void enable_microphone(bool enabled) override { enable(microphone_.get(), enabled); }
    std::optional<Bytes> take_playback(unsigned frames) override {
        check(); require(playback_ && frames > 0 && frames <= 4800, "invalid audio capture request");
        Bytes pcm(std::size_t(frames) * playback_->format.block_size());
        if (playback_->ring.pop(pcm, true, playback_->format.byte_rate() / 10) == pcm.size()) return pcm;
        return std::nullopt;
    }
    void feed_microphone(View pcm) override {
        check(); require(microphone_ && pcm.size() <= 9600 && pcm.size() % microphone_->format.block_size() == 0, "invalid microphone block");
        (void)microphone_->ring.push(pcm);
    }
    std::string description() const override {
        return (playback_ ? playback_->name : "playback disabled") + "; " + (microphone_ ? microphone_->name : "microphone disabled");
    }
};
}
std::unique_ptr<AudioDevices> make_pipewire_audio(AudioOptions options) { return std::make_unique<PipeWireAudio>(options); }
} // namespace lrdp
