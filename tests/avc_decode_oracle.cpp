// Test-only decoder. Uses only FFmpeg's public API, not LRDP's packing/graphics code.
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
struct ContextDelete { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct ParserDelete { void operator()(AVCodecParserContext* p) const { av_parser_close(p); } };
void integer(std::uint32_t n) {
    const std::array<char,4> bytes{char(n),char(n>>8),char(n>>16),char(n>>24)};
    std::cout.write(bytes.data(),4);
}
}
int main(int argc, char** argv) {
    try {
        check(argc==2, "usage: avc_decode_oracle FILE.h264");
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        check(bool(file), "cannot open captured H.264");
        const auto length=file.tellg(); check(length>0 && length<=64*1024*1024, "captured H.264 quota");
        std::vector<std::uint8_t> bytes(std::size_t(length)+AV_INPUT_BUFFER_PADDING_SIZE,0);
        file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()),length); check(bool(file),"truncated capture file");
        const auto* codec=avcodec_find_decoder(AV_CODEC_ID_H264); check(codec,"missing H.264 decoder");
        std::unique_ptr<AVCodecContext,ContextDelete> context(avcodec_alloc_context3(codec));
        check(context!=nullptr,"decoder allocation"); context->thread_count=1;
        check(avcodec_open2(context.get(),codec,nullptr)>=0,"decoder open");
        std::unique_ptr<AVCodecParserContext,ParserDelete> parser(av_parser_init(AV_CODEC_ID_H264));
        std::unique_ptr<AVFrame,FrameDelete> frame(av_frame_alloc());
        std::unique_ptr<AVPacket,PacketDelete> packet(av_packet_alloc());
        check(parser && frame && packet,"decoder resource allocation");
        unsigned pictures=0;
        auto output=[&] {
            for(;;) {
                const auto rc=avcodec_receive_frame(context.get(),frame.get());
                if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF) return;
                check(rc>=0,"H.264 picture rejected");
                check(++pictures<=64,"decoded picture count quota");
                check(frame->width>0 && frame->height>0 && !(frame->width&1) && !(frame->height&1) &&
                    std::uint64_t(frame->width)*frame->height<=16*1024*1024,"decoded dimensions");
                check(frame->format==AV_PIX_FMT_YUV420P || frame->format==AV_PIX_FMT_YUVJ420P,"unexpected decoded pixel format");
                check(frame->color_range==AVCOL_RANGE_JPEG && frame->colorspace==AVCOL_SPC_BT709,"incorrect RDP color metadata");
                integer(std::uint32_t(frame->width)); integer(std::uint32_t(frame->height));
                for(unsigned plane=0;plane<3;++plane) {
                    const auto w=frame->width/(plane?2:1),h=frame->height/(plane?2:1);
                    check(frame->linesize[plane]>=w,"invalid decoded stride");
                    for(int y=0;y<h;++y) std::cout.write(reinterpret_cast<char*>(frame->data[plane])+std::ptrdiff_t(y)*frame->linesize[plane],w);
                }
                check(bool(std::cout),"oracle output write failed"); av_frame_unref(frame.get());
            }
        };
        auto submit=[&](std::uint8_t* data,int size) {
            if(!size) return;
            check(av_new_packet(packet.get(),size)>=0,"packet allocation");
            std::copy_n(data,size,packet->data);
            check(avcodec_send_packet(context.get(),packet.get())>=0,"decoder submission failed");
            av_packet_unref(packet.get()); output();
        };
        std::size_t offset=0;
        while(offset<std::size_t(length)) {
            std::uint8_t* data=nullptr; int size=0;
            const auto consumed=av_parser_parse2(parser.get(),context.get(),&data,&size,bytes.data()+offset,
                int(std::size_t(length)-offset),AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
            check(consumed>=0 && (consumed>0||size>0),"H.264 parser made no progress");
            offset+=std::size_t(consumed); submit(data,size);
        }
        std::uint8_t* data=nullptr; int size=0;
        check(av_parser_parse2(parser.get(),context.get(),&data,&size,nullptr,0,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0)>=0,"parser flush failed");
        submit(data,size);
        check(avcodec_send_packet(context.get(),nullptr)>=0,"decoder flush failed"); output();
        check(pictures>0,"capture contained no decoded pictures");
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
