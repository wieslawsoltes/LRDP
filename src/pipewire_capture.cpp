#include "lrdp/platform/pipewire_capture.hpp"
#include "lrdp/platform/capture_frame.hpp"
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/meta.h>
#include <atomic>
#include <mutex>

namespace lrdp {
struct PipeWireCapture::Impl {
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{}, core_listener{};
    spa_video_info_raw format{};
    bool running = false, formatted = false;
    std::atomic<bool> failed = false;
    mutable std::mutex mutex;
    CaptureSnapshot snapshot;
    ~Impl() {
        if (running) pw_thread_loop_stop(loop);
        if (stream) { spa_hook_remove(&stream_listener); pw_stream_destroy(stream); }
        if (core) { spa_hook_remove(&core_listener); pw_core_disconnect(core); }
        if (context) pw_context_destroy(context);
        if (loop) pw_thread_loop_destroy(loop);
    }
    void parameters(std::uint32_t id, const spa_pod* param) {
        if (id != SPA_PARAM_Format || !param) return;
        std::uint32_t type = 0, subtype = 0;
        require(spa_format_parse(param, &type, &subtype) >= 0 && type == SPA_MEDIA_TYPE_video && subtype == SPA_MEDIA_SUBTYPE_raw,
                "PipeWire negotiated a non-raw video format");
        require(spa_format_video_raw_parse(param, &format) >= 0, "invalid PipeWire raw-video format");
        const auto w = format.size.width, h = format.size.height;
        require(w > 0 && h > 0 && w <= 8192 && h <= 8192 && std::uint64_t(w) * h <= 16 * 1024 * 1024,
                "PipeWire negotiated excessive dimensions");
        require(format.format == SPA_VIDEO_FORMAT_BGRA || format.format == SPA_VIDEO_FORMAT_BGRx ||
                format.format == SPA_VIDEO_FORMAT_RGBA || format.format == SPA_VIDEO_FORMAT_RGBx, "unsupported PipeWire pixel format");
        formatted = true;
        { std::lock_guard lock(mutex); snapshot.frame.reset(); }
        std::uint8_t buffer[1024]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[3];
        params[0] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
        params[1] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))));
        params[2] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region))));
        require(pw_stream_update_params(stream, params, 3) >= 0, "cannot configure mapped PipeWire buffers");
    }
    void process() {
        if (!formatted || failed.load()) return;
        // Consume only the newest currently queued image. Always return every buffer.
        pw_buffer* newest = nullptr;
        for (unsigned i = 0; i < 16; ++i) {
            auto* next = pw_stream_dequeue_buffer(stream); if (!next) break;
            if (newest) pw_stream_queue_buffer(stream, newest);
            newest = next;
        }
        if (!newest) return;
        struct ReturnBuffer { pw_stream* stream; pw_buffer* buffer; ~ReturnBuffer() { pw_stream_queue_buffer(stream, buffer); } } returned{stream, newest};
        auto* buffer = newest->buffer;
        require(buffer && buffer->n_datas == 1, "unsupported PipeWire plane layout");
        const auto& plane = buffer->datas[0];
        require((plane.type == SPA_DATA_MemPtr || plane.type == SPA_DATA_MemFd) && plane.data && plane.chunk && plane.maxsize,
                "PipeWire buffer is not a mapped CPU-accessible plane");
        if (!plane.chunk->size || (plane.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) return;
        const auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header)));
        if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) return;
        const auto offset = plane.chunk->offset % plane.maxsize;
        const auto size = std::min(plane.chunk->size, plane.maxsize);
        require(size <= plane.maxsize - offset, "wrapped video chunks are not supported");
        std::optional<CaptureCrop> crop;
        const auto* metadata = static_cast<spa_meta_region*>(spa_buffer_find_meta_data(buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)));
        if (metadata && metadata->region.size.width && metadata->region.size.height) {
            require(metadata->region.position.x >= 0 && metadata->region.position.y >= 0, "negative PipeWire crop origin");
            crop = CaptureCrop{unsigned(metadata->region.position.x), unsigned(metadata->region.position.y),
                               metadata->region.size.width, metadata->region.size.height};
        }
        const auto packed = format.format == SPA_VIDEO_FORMAT_BGRA ? PackedFormat::bgra :
                            format.format == SPA_VIDEO_FORMAT_BGRx ? PackedFormat::bgrx :
                            format.format == SPA_VIDEO_FORMAT_RGBA ? PackedFormat::rgba : PackedFormat::rgbx;
        auto frame = std::make_shared<const Frame>(copy_capture_frame(
            View(static_cast<const std::uint8_t*>(plane.data) + offset, size), format.size.width, format.size.height, plane.chunk->stride, packed, crop));
        { std::lock_guard lock(mutex); snapshot.frame = std::move(frame); ++snapshot.sequence; }
    }
    void start(UniqueFd remote, const PortalStream& selected) {
        static std::once_flag initialized; std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
        loop = pw_thread_loop_new("lrdp-capture", nullptr); require(loop != nullptr, "cannot create PipeWire capture loop");
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0); require(context != nullptr, "cannot create PipeWire context");
        // connect_fd consumes and closes the descriptor on both disconnect and error.
        core = pw_context_connect_fd(context, remote.release(), nullptr, 0); require(core != nullptr, "cannot connect the portal-granted PipeWire remote");
        static const pw_core_events core_events = [] {
            pw_core_events value{}; value.version = PW_VERSION_CORE_EVENTS;
            value.error = [](void* data, std::uint32_t, int, int, const char*) { static_cast<Impl*>(data)->failed.store(true); };
            return value;
        }();
        pw_core_add_listener(core, &core_listener, &core_events, this);
        const auto target = selected.serial ? std::to_string(*selected.serial) : std::to_string(selected.node);
        stream = pw_stream_new(core, "LRDP consented desktop", pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_TARGET_OBJECT, target.c_str(), nullptr));
        require(stream != nullptr, "cannot create PipeWire capture stream");
        static const pw_stream_events events = [] {
            pw_stream_events value{}; value.version = PW_VERSION_STREAM_EVENTS;
            value.state_changed = [](void* data, pw_stream_state previous, pw_stream_state state, const char*) {
                if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && previous != PW_STREAM_STATE_UNCONNECTED))
                    static_cast<Impl*>(data)->failed.store(true);
            };
            value.param_changed = [](void* data, std::uint32_t id, const spa_pod* parameter) {
                auto& self = *static_cast<Impl*>(data); try { self.parameters(id, parameter); } catch (...) { self.failed.store(true); }
            };
            value.process = [](void* data) { auto& self = *static_cast<Impl*>(data); try { self.process(); } catch (...) { self.failed.store(true); } };
            return value;
        }();
        pw_stream_add_listener(stream, &stream_listener, &events, this);
        std::uint8_t bytes[1024]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
        const spa_rectangle preferred{1280, 720}, minimum{1, 1}, maximum{8192, 8192};
        const spa_fraction rate{30, 1}, low{0, 1}, high{120, 1};
        const spa_pod* parameter = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &low, &high)));
        require(pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), &parameter, 1) >= 0,
            "cannot connect PipeWire video capture");
        require(pw_thread_loop_start(loop) >= 0, "cannot start PipeWire capture thread"); running = true;
    }
};
PipeWireCapture::PipeWireCapture(UniqueFd remote, const PortalStream& stream) : impl_(std::make_unique<Impl>()) {
    require(bool(remote), "PipeWire capture requires a granted descriptor"); impl_->start(std::move(remote), stream);
}
PipeWireCapture::~PipeWireCapture() = default;
CaptureSnapshot PipeWireCapture::latest() const {
    require(!impl_->failed.load(), "PipeWire capture was disconnected or supplied an invalid buffer");
    std::lock_guard lock(impl_->mutex); return impl_->snapshot;
}
} // namespace lrdp
