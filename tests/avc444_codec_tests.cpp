#include "lrdp/video.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
using namespace lrdp;
namespace {
void check(bool ok,const char* s){if(!ok)throw std::runtime_error(s);}
struct CodecDelete{void operator()(AVCodecContext* p)const{avcodec_free_context(&p);}};
struct FrameDelete{void operator()(AVFrame* p)const{av_frame_free(&p);}};
struct PacketDelete{void operator()(AVPacket* p)const{av_packet_free(&p);}};
struct ScaleDelete{void operator()(SwsContext* p)const{sws_freeContext(p);}};
struct Planes{unsigned w,h;std::array<Bytes,3> p;};
class Decoder {
    std::unique_ptr<AVCodecContext,CodecDelete> c_;
public:
    Decoder(){auto* impl=avcodec_find_decoder(AV_CODEC_ID_H264);check(impl,"H.264 decoder unavailable");c_.reset(avcodec_alloc_context3(impl));check(c_&&avcodec_open2(c_.get(),impl,nullptr)>=0,"decoder open");}
    Planes decode(View data){
        std::unique_ptr<AVPacket,PacketDelete> packet(av_packet_alloc());
        check(packet&&av_new_packet(packet.get(),int(data.size()))==0,"decode packet allocation");std::copy(data.begin(),data.end(),packet->data);
        check(avcodec_send_packet(c_.get(),packet.get())>=0,"decode submitted picture");
        std::unique_ptr<AVFrame,FrameDelete> f(av_frame_alloc());check(f&&avcodec_receive_frame(c_.get(),f.get())>=0,"immediate picture decode");
        check(f->width==208&&f->height==224,"16x16 padded dimensions");
        check(f->format==AV_PIX_FMT_YUV420P||f->format==AV_PIX_FMT_YUVJ420P,"both AVC444 pictures must remain 4:2:0");
        check(f->color_range==AVCOL_RANGE_JPEG&&f->colorspace==AVCOL_SPC_BT709,"full-range BT709 signaling");
        Planes result{unsigned(f->width),unsigned(f->height),{}};
        for(unsigned i=0;i<3;++i){auto w=i?result.w/2:result.w,h=i?result.h/2:result.h;result.p[i].resize(std::size_t(w)*h);for(unsigned y=0;y<h;++y)std::copy_n(f->data[i]+std::size_t(y)*f->linesize[i],w,result.p[i].data()+std::size_t(y)*w);}
        return result;
    }
};
Planes reconstruct(const Planes& a,const Planes& b,VideoCodec mode){
    const auto w=a.w,h=a.h;check(w==b.w&&h==b.h,"mismatched decoded pair");Planes out{w,h,{}};out.p[0]=a.p[0];
    for(unsigned c=1;c<=2;++c){
        auto& full=out.p[c];full.resize(std::size_t(w)*h);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            if(!(y&1)&&!(x&1))continue;
            unsigned plane,px,py;
            if(mode==VideoCodec::avc444){if(y&1){plane=0;px=x;py=(y/16)*16+(y%16)/2+(c-1)*8;}else{plane=c;px=x/2;py=y/2;}}
            else {if(x&1){plane=0;px=x/2+(c-1)*(w/2);py=y;}else{plane=(x%4)?2:1;px=x/4+(c-1)*(w/4);py=y/2;}}
            full[std::size_t(y)*w+x]=b.p[plane][std::size_t(py)*(plane?w/2:w)+px];
        }
        // Inverse of the specified floor(mean(2x2)). This reference decoder
        // deliberately omits the optional edge threshold postfilter.
        for(unsigned y=0;y<h;y+=2)for(unsigned x=0;x<w;x+=2){
            const auto i=std::size_t(y)*w+x;
            const int value=4*int(a.p[c][std::size_t(y/2)*(w/2)+x/2])-full[i+1]-full[i+w]-full[i+w+1];
            full[i]=std::uint8_t(std::clamp(value,0,255));
        }
    }
    return out;
}
Bytes bgra(const Planes& frame){
    std::unique_ptr<SwsContext,ScaleDelete> s(sws_getContext(int(frame.w),int(frame.h),AV_PIX_FMT_YUV444P,int(frame.w),int(frame.h),AV_PIX_FMT_BGRA,SWS_BILINEAR,nullptr,nullptr,nullptr));check(s!=nullptr,"reference YUV444 converter");
    const auto* coefficients=sws_getCoefficients(SWS_CS_ITU709);check(sws_setColorspaceDetails(s.get(),coefficients,1,coefficients,1,0,1<<16,1<<16)>=0,"reference BT709 coefficients");
    const std::uint8_t* inputs[]={frame.p[0].data(),frame.p[1].data(),frame.p[2].data(),nullptr};const int pitches[]={int(frame.w),int(frame.w),int(frame.w),0};
    Bytes pixels(std::size_t(frame.w)*frame.h*4);std::uint8_t* outputs[]={pixels.data(),nullptr,nullptr,nullptr};const int strides[]={int(frame.w*4),0,0,0};
    check(sws_scale(s.get(),inputs,pitches,0,int(frame.h),outputs,strides)==int(frame.h),"reference RGB conversion");return pixels;
}
void run(VideoCodec mode){
    VideoOptions options;options.backend="software";options.qp=0; // Isolate packing/matrix rounding from H.264 quantization.
    auto encoder=ffmpeg_video_factory(options)(202,218,mode);Decoder decoder;
    for(unsigned frame_id=0;frame_id<4;++frame_id){
        Frame source{202,218,Bytes(202*218*4)};
        for(unsigned y=0;y<source.height;++y)for(unsigned x=0;x<source.width;++x){
            auto* p=source.bgra.data()+(std::size_t(y)*source.width+x)*4;
            p[0]=std::uint8_t(x%2?210:35);p[1]=std::uint8_t((x+y+frame_id)%3?57:190);p[2]=std::uint8_t(y%2?39:220);p[3]=255;
        }
        const auto encoded=encoder->encode(source,frame_id==0||frame_id==3);
        check(encoded.codec==mode&&!encoded.hardware&&encoded.qp==0&&!encoded.auxiliary.empty(),"encoded pair metadata");
        check(frame_id!=0||encoded.key_frame,"initial IDR missing");
        const auto primary=decoder.decode(encoded.annex_b),auxiliary=decoder.decode(encoded.auxiliary);
        const auto pixels=bgra(reconstruct(primary,auxiliary,mode));int maximum=0;
        for(unsigned y=0;y<224;++y)for(unsigned x=0;x<208;++x)for(unsigned c=0;c<3;++c){
            const auto expected=source.bgra[(std::size_t(std::min(y,217U))*202+std::min(x,201U))*4+c];
            maximum=std::max(maximum,std::abs(int(pixels[(std::size_t(y)*208+x)*4+c])-int(expected)));
        }
        // The floor-average discards at most 3 chroma levels; BT709 inverse and
        // integer RGB conversion add bounded rounding. This is not lossless RGB.
        check(maximum<=12,"full-chroma reconstruction or edge padding exceeds color tolerance");
        std::cout<<codec_name(mode)<<" frame "<<frame_id<<": max RGB error "<<maximum<<"/255\n";
    }
}
}
int main(){try{run(VideoCodec::avc444);run(VideoCodec::avc444v2);std::cout<<"PASS: one H.264 decoder per desktop stream, sequential primary/auxiliary pictures, full-chroma pixel oracle and padded edges\n";}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
