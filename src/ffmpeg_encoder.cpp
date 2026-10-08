#include "lrdp/video.hpp"
#include <algorithm>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace lrdp {
namespace {
void avcheck(int result, const char* operation) {
    if (result >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(result, text, sizeof(text));
    throw ProtocolError(std::string(operation) + ": " + text);
}
struct CodecDelete { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct BufferDelete { void operator()(AVBufferRef* p) const { av_buffer_unref(&p); } };
struct ScaleDelete { void operator()(SwsContext* p) const { sws_freeContext(p); } };
using AvFrame = std::unique_ptr<AVFrame, FrameDelete>;
struct Options {
    AVDictionary* value = nullptr;
    ~Options() { av_dict_free(&value); }
    void set(const char* key, const std::string& text) { avcheck(av_dict_set(&value, key, text.c_str(), 0), "set encoder option"); }
};

class FfmpegEncoder final : public VideoEncoder {
    VideoOptions options_;
    std::string name_;
    unsigned width_, height_, coded_width_, coded_height_;
    std::unique_ptr<AVCodecContext, CodecDelete> codec_;
    std::unique_ptr<AVBufferRef, BufferDelete> device_;
    std::unique_ptr<SwsContext, ScaleDelete> scale_;
    AvFrame yuv_{av_frame_alloc()};
    Bytes padded_;
    std::int64_t pts_ = 0;
    bool hardware_;
public:
    FfmpegEncoder(VideoOptions options, std::string name, unsigned width, unsigned height)
        : options_(std::move(options)), name_(std::move(name)), width_(width), height_(height),
          coded_width_((width + 15U) & ~15U), coded_height_((height + 15U) & ~15U), hardware_(name_ != "libx264") {
        require(width && height && std::uint64_t(coded_width_) * coded_height_ <= 16 * 1024 * 1024,
                "padded AVC dimensions exceed allocation quota");
        const auto* implementation = avcodec_find_encoder_by_name(name_.c_str());
        require(implementation != nullptr, "requested FFmpeg H.264 encoder is not installed: " + name_);
        codec_.reset(avcodec_alloc_context3(implementation)); require(codec_ && yuv_, "cannot allocate codec resources");
        auto* c = codec_.get();
        c->width = int(coded_width_); c->height = int(coded_height_);
        c->time_base = AVRational{1, int(options_.fps)}; c->framerate = AVRational{int(options_.fps), 1};
        c->gop_size = int(options_.fps * 4); c->max_b_frames = 0; c->thread_count = 2;
        c->color_range = AVCOL_RANGE_JPEG; c->colorspace = AVCOL_SPC_BT709;
        c->color_primaries = AVCOL_PRI_BT709; c->color_trc = AVCOL_TRC_BT709;
        c->flags |= AV_CODEC_FLAG_LOW_DELAY;
        const bool vaapi = name_ == "h264_vaapi";
        const auto software_format = hardware_ ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        c->pix_fmt = vaapi ? AV_PIX_FMT_VAAPI : software_format;
        if (vaapi) {
            AVBufferRef* device = nullptr;
            avcheck(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, options_.device.c_str(), nullptr, 0), "open VA-API device");
            device_.reset(device);
            std::unique_ptr<AVBufferRef, BufferDelete> frames(av_hwframe_ctx_alloc(device));
            require(frames != nullptr, "cannot allocate VA-API frame pool");
            auto* pool = reinterpret_cast<AVHWFramesContext*>(frames->data);
            pool->format = AV_PIX_FMT_VAAPI; pool->sw_format = AV_PIX_FMT_NV12;
            pool->width = c->width; pool->height = c->height; pool->initial_pool_size = 4;
            avcheck(av_hwframe_ctx_init(frames.get()), "initialize VA-API frame pool");
            c->hw_frames_ctx = av_buffer_ref(frames.get()); require(c->hw_frames_ctx != nullptr, "cannot reference VA-API frame pool");
        }
        Options settings;
        if (name_ == "libx264") {
            settings.set("preset", "veryfast"); settings.set("tune", "zerolatency"); settings.set("qp", std::to_string(options_.qp));
            settings.set("x264-params", "annexb=1:repeat-headers=1:bframes=0:scenecut=0:fullrange=on:colormatrix=bt709");
        } else if (vaapi) {
            settings.set("rc_mode", "CQP"); settings.set("qp", std::to_string(options_.qp)); settings.set("async_depth", "1");
        } else {
            settings.set("preset", "p4"); settings.set("tune", "ull"); settings.set("rc", "constqp");
            settings.set("qp", std::to_string(options_.qp)); settings.set("zerolatency", "1"); settings.set("delay", "0");
            settings.set("forced-idr", "1");
        }
        avcheck(avcodec_open2(c, implementation, &settings.value), "open H.264 encoder");
        require(av_dict_count(settings.value) == 0, "H.264 encoder did not accept its latency/color options");
        yuv_->format = software_format; yuv_->width = c->width; yuv_->height = c->height;
        yuv_->color_range = c->color_range; yuv_->colorspace = c->colorspace;
        yuv_->color_primaries = c->color_primaries; yuv_->color_trc = c->color_trc;
        avcheck(av_frame_get_buffer(yuv_.get(), 32), "allocate YUV frame");
        scale_.reset(sws_getContext(c->width, c->height, AV_PIX_FMT_BGRA, c->width, c->height,
                                   software_format, SWS_BILINEAR, nullptr, nullptr, nullptr));
        require(scale_ != nullptr, "cannot create BGRA-to-YUV converter");
        const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        avcheck(sws_setColorspaceDetails(scale_.get(), coefficients, 1, coefficients, 1, 0, 1 << 16, 1 << 16), "configure full-range BT.709 conversion");
        padded_.resize(std::size_t(coded_width_) * coded_height_ * 4);
    }
    EncodedVideo encode(const Frame& frame, bool force_key_frame) override {
        frame.validate(); require(frame.width == width_ && frame.height == height_, "video dimensions changed without rebuilding the encoder");
        // Pad edge texels rather than stretching the desktop. RDP's region rectangle
        // masks padding outside the logical surface; H.264 dimensions stay 16-aligned.
        const auto stride = std::size_t(coded_width_) * 4;
        for (unsigned y = 0; y < coded_height_; ++y) {
            const auto* row = frame.bgra.data() + std::size_t(std::min(y, height_ - 1)) * width_ * 4;
            auto* out = padded_.data() + std::size_t(y) * stride;
            std::memcpy(out, row, std::size_t(width_) * 4);
            for (unsigned x = width_; x < coded_width_; ++x) std::memcpy(out + x * 4, row + (width_ - 1) * 4, 4);
        }
        avcheck(av_frame_make_writable(yuv_.get()), "make YUV frame writable");
        const std::uint8_t* sources[] = {padded_.data(), nullptr, nullptr, nullptr};
        const int strides[] = {int(stride), 0, 0, 0};
        require(sws_scale(scale_.get(), sources, strides, 0, int(coded_height_), yuv_->data, yuv_->linesize) == int(coded_height_), "incomplete YUV conversion");
        yuv_->pts = pts_++; yuv_->pict_type = force_key_frame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        AvFrame hardware;
        AVFrame* input = yuv_.get();
        if (name_ == "h264_vaapi") {
            hardware.reset(av_frame_alloc()); require(hardware != nullptr, "cannot allocate hardware frame");
            avcheck(av_hwframe_get_buffer(codec_->hw_frames_ctx, hardware.get(), 0), "acquire VA-API surface");
            avcheck(av_hwframe_transfer_data(hardware.get(), yuv_.get(), 0), "upload NV12 to VA-API");
            avcheck(av_frame_copy_props(hardware.get(), yuv_.get()), "copy hardware frame metadata"); input = hardware.get();
        }
        avcheck(avcodec_send_frame(codec_.get(), input), "submit H.264 frame");
        std::unique_ptr<AVPacket, PacketDelete> packet(av_packet_alloc()); require(packet != nullptr, "cannot allocate encoded packet");
        EncodedVideo result; result.width = width_; result.height = height_; result.hardware = hardware_; result.encoder = name_;
        for (;;) {
            const int rc = avcodec_receive_packet(codec_.get(), packet.get());
            if (rc == AVERROR(EAGAIN)) break;
            avcheck(rc, "receive H.264 frame");
            require(packet->pts == input->pts && packet->size > 0, "encoder reordered a zero-latency frame");
            require(std::size_t(packet->size) <= 8 * 1024 * 1024 - result.annex_b.size(), "encoded frame exceeds quota");
            result.annex_b.insert(result.annex_b.end(), packet->data, packet->data + packet->size);
            result.key_frame |= (packet->flags & AV_PKT_FLAG_KEY) != 0; av_packet_unref(packet.get());
        }
        const auto& b = result.annex_b;
        require(b.size() >= 4 && b[0] == 0 && b[1] == 0 && (b[2] == 1 || (b[2] == 0 && b[3] == 1)),
                "encoder did not produce immediate Annex-B output");
        require(!force_key_frame || result.key_frame, "encoder did not honor key-frame request");
        return result;
    }
};

class CandidateEncoder final : public VideoEncoder {
    VideoOptions options_;
    unsigned width_, height_;
    std::vector<std::string> candidates_;
    std::size_t candidate_ = 0;
    std::unique_ptr<VideoEncoder> encoder_;
public:
    CandidateEncoder(VideoOptions options, unsigned width, unsigned height)
        : options_(std::move(options)), width_(width), height_(height) {
        if (options_.backend == "auto") candidates_ = {"h264_vaapi", "h264_nvenc", "libx264"};
        else if (options_.backend == "vaapi") candidates_ = {"h264_vaapi"};
        else if (options_.backend == "nvenc") candidates_ = {"h264_nvenc"};
        else if (options_.backend == "software") candidates_ = {"libx264"};
        else throw ProtocolError("unknown video backend");
    }
    EncodedVideo encode(const Frame& frame, bool key_frame) override {
        std::string failures;
        for (; candidate_ < candidates_.size(); ++candidate_) {
            try {
                if (!encoder_) { encoder_ = std::make_unique<FfmpegEncoder>(options_, candidates_[candidate_], width_, height_); key_frame = true; }
                return encoder_->encode(frame, key_frame);
            } catch (const std::exception& error) {
                failures += candidates_[candidate_] + ": " + error.what() + "; "; encoder_.reset();
            }
        }
        throw ProtocolError("all configured H.264 encoders failed: " + failures);
    }
};
}
VideoFactory ffmpeg_video_factory(VideoOptions options) {
    require(options.fps > 0 && options.fps <= 120 && options.qp <= 51, "invalid video configuration");
    return [options = std::move(options)](unsigned width, unsigned height) {
        return std::make_unique<CandidateEncoder>(options, width, height);
    };
}
} // namespace lrdp
