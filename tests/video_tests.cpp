#include "lrdp/video.hpp"
#include <cmath>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
using namespace lrdp;
namespace {
void check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
struct CodecDelete { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct ScaleDelete { void operator()(SwsContext* p) const { sws_freeContext(p); } };
}
int main() {
    try {
        VideoOptions options; options.backend = "software"; options.qp = 18;
        auto encoder = ffmpeg_video_factory(options)(202,202,VideoCodec::avc420);
        const auto* h264 = avcodec_find_decoder(AV_CODEC_ID_H264); check(h264 != nullptr,"H.264 decoder missing");
        std::unique_ptr<AVCodecContext,CodecDelete> decoder(avcodec_alloc_context3(h264));
        check(decoder && avcodec_open2(decoder.get(),h264,nullptr) >= 0,"cannot open independent decoder");
        std::unique_ptr<AVFrame,FrameDelete> decoded(av_frame_alloc());
        std::unique_ptr<AVPacket,PacketDelete> packet(av_packet_alloc());
        Frame frame{202,202,Bytes(202*202*4)};
        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            for (std::size_t i = 0; i < frame.bgra.size(); i += 4) {
                frame.bgra[i] = 40; frame.bgra[i+1] = 110; frame.bgra[i+2] = std::uint8_t(200-iteration*20); frame.bgra[i+3] = 255;
            }
            const auto encoded = encoder->encode(frame,iteration != 1);
            check(!encoded.hardware && encoded.encoder == "libx264" && !encoded.annex_b.empty(),"software encoder reporting");
            check(iteration == 1 || encoded.key_frame,"forced keyframe");
            check(av_new_packet(packet.get(),int(encoded.annex_b.size())) == 0,"packet allocation");
            std::copy(encoded.annex_b.begin(),encoded.annex_b.end(),packet->data);
            check(avcodec_send_packet(decoder.get(),packet.get()) >= 0,"decoder rejected Annex B"); av_packet_unref(packet.get());
            check(avcodec_receive_frame(decoder.get(),decoded.get()) >= 0,"decoder did not output immediate frame");
            check(decoded->width == 208 && decoded->height == 208,"AVC420 16-pixel padding");
            check(decoded->color_range == AVCOL_RANGE_JPEG && decoded->colorspace == AVCOL_SPC_BT709,"AVC full-range BT709 signaling");
            std::unique_ptr<SwsContext,ScaleDelete> scale(sws_getContext(208,208,AVPixelFormat(decoded->format),208,208,AV_PIX_FMT_BGRA,SWS_BILINEAR,nullptr,nullptr,nullptr));
            check(scale != nullptr,"decoder colorspace converter");
            const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
            check(sws_setColorspaceDetails(scale.get(),coefficients,1,coefficients,1,0,1<<16,1<<16) >= 0,"decoder BT709 conversion");
            Bytes pixels(208*208*4); std::uint8_t* output[] = {pixels.data(),nullptr,nullptr,nullptr}; int strides[] = {208*4,0,0,0};
            check(sws_scale(scale.get(),decoded->data,decoded->linesize,0,208,output,strides) == 208,"decoder full frame");
            for (const unsigned point : {0U,100U*208+100,207U*208+207}) for (unsigned component = 0; component < 3; ++component)
                check(std::abs(int(pixels[point*4+component])-int(frame.bgra[component])) <= 6,"decoded color/padded-edge error exceeds tolerance");
            av_frame_unref(decoded.get());
        }
        std::cout << "PASS: native FFmpeg software encode/decode, Annex B, forced keyframes, 16-aligned edge padding, full-range BT709 pixel oracle\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
