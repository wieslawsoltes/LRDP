#include "lrdp/graphics.hpp"
#include "lrdp/video.hpp"
#include <algorithm>
#include <iostream>
#include <thread>
using namespace lrdp;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f){try{f();}catch(const ProtocolError&){return;}throw std::runtime_error("invalid AVC framing accepted");}
Bytes offer(std::initializer_list<std::pair<std::uint32_t,std::uint32_t>> versions) {
    Writer body;body.le16(unsigned(versions.size()));
    for(auto [version,flags]:versions){body.le32(version).le32(version==0xa0100?16:4);if(version==0xa0100)body.zeros(16);else body.le32(flags);}
    return graphics_pdu(0x12,body.bytes());
}
Bytes unsegment(View packet) {
    Reader in(packet);Writer result;
    if(in.u8()==0xe0){check(in.u8()==4,"unexpected segment flags");result.raw(in.take(in.remaining()));}
    else {const auto count=in.le16();const auto size=in.le32();for(unsigned i=0;i<count;++i){const auto n=in.le32();check(n&&in.u8()==4,"invalid segment");result.raw(in.take(n-1));}check(result.size()==size,"segmented size");in.end();}
    return std::move(result).finish();
}
void negotiation() {
    for(const bool reverse:{false,true}) {
        Graphics g;
        auto input=reverse?offer({{0xa0100,0},{0xa0200,0},{0x80105,16}}):offer({{0x80105,16},{0xa0200,0},{0xa0100,0}});
        check(g.receive(input,true)&&g.video_enabled()&&g.video_codec()==VideoCodec::avc444v2,"version selection depends on ordering");
        auto output=g.drain();check(output.size()==1,"missing capability confirmation");
        auto plain=unsegment(output[0]);Reader in(plain);check(in.le16()==0x13&&in.le16()==0&&in.le32()==32,"10.1 confirmation framing");
        check(in.le32()==0xa0100&&in.le32()==16,"10.1 reserved capability size");for(auto b:in.take(16))check(b==0,"nonzero reserved confirmation");in.end();
    }
    for(auto version:{0xa0002U,0xa0200U}) {
        Graphics g;check(g.receive(offer({{version,0}}),true)&&g.video_codec()==VideoCodec::avc444,"10.x full chroma negotiation");
        Graphics disabled;check(disabled.receive(offer({{version,0x20}}),true)&&!disabled.video_enabled(),"peer AVC_DISABLED ignored");
        Graphics unavailable;check(unavailable.receive(offer({{version,0}}),false)&&!unavailable.video_enabled(),"unavailable encoder advertised");
        auto plain=unsegment(unavailable.drain()[0]);Reader in(plain);in.skip(16);check(in.le32()==0x22,"server AVC_DISABLED confirmation missing");
    }
    Graphics reserved;auto input=offer({{0xa0100,0}});std::fill(input.end()-16,input.end(),0xa5);
    check(reserved.receive(input,true),"reserved client bytes must be ignored");
    Graphics no_encoder;check(!no_encoder.receive(offer({{0xa0100,0}}),false),"10.1 must not be selected without video");
    Graphics legacy;check(legacy.receive(offer({{0xa0100,0},{0x80105,16}}),false)&&!legacy.video_enabled(),"legacy raw fallback missing");
    Graphics duplicate;rejects([&]{duplicate.receive(offer({{0x80105,16},{0x80105,16}}),true);});check(duplicate.drain().empty(),"partial confirmation before full capability validation");
    Graphics unknown;check(!unknown.receive(offer({{0xdeadbeef,0}}),true),"unknown capability silently selected");
}
void framing(VideoCodec mode) {
    Graphics g;check(g.receive(offer({{mode==VideoCodec::avc444?0xa0002U:0xa0100U,0}}),true),"negotiation");
    Monitor m;m.width=640;m.height=480;g.reset(validate_layout({m}));(void)g.drain();
    EncodedVideo video{{0,0,0,1,0x65,0x88},640,480,true,false,"fixture",mode,{0,0,1,0x41,0x99},18};
    auto invalid=video;invalid.auxiliary.clear();rejects([&]{g.video_frame(invalid);});
    invalid=video;invalid.width=642;rejects([&]{g.video_frame(invalid);});
    invalid=video;invalid.qp=52;rejects([&]{g.video_frame(invalid);});
    invalid=video;invalid.codec=VideoCodec::avc420;rejects([&]{g.video_frame(invalid);});
    check(g.drain().empty()&&g.in_flight()==0,"invalid completion published a frame prefix");
    g.video_frame(video);auto output=g.drain();check(output.size()==3&&g.in_flight()==1,"one desktop frame for both subframes");
    auto plain=unsegment(output[1]);Reader in(plain);
    check(in.le16()==1&&in.le16()==0&&in.le32()==plain.size(),"WireToSurface header");
    check(in.le16()==0&&in.le16()==(mode==VideoCodec::avc444?14:15)&&in.u8()==32,"AVC444 codec identifier");
    check(in.le16()==0&&in.le16()==0&&in.le16()==640&&in.le16()==480,"AVC444 exclusive update rectangle");
    const auto n=in.le32();check(n==in.remaining(),"bitmap length");
    const auto info=in.le32();check((info>>30)==0&&(info&0x3fffffff)==14+video.annex_b.size(),"LC or first-subframe length excludes metadata");
    for(const auto& picture:{video.annex_b,video.auxiliary}) {
        check(in.le32()==1&&in.le16()==0&&in.le16()==0&&in.le16()==640&&in.le16()==480,"subframe regions");
        check(in.u8()==18&&in.u8()==75,"subframe quantizer metadata");auto bytes=in.take(picture.size());check(std::equal(bytes.begin(),bytes.end(),picture.begin()),"subframe ordering");
    }
    in.end();
}
class Encoder final:public VideoEncoder {
    VideoCodec codec_;
public:
    explicit Encoder(VideoCodec c):codec_(c){}
    EncodedVideo encode(const Frame& f,bool key)override{return {{0,0,1,0x65},f.width,f.height,key,false,"fixture",codec_,{0,0,1,0x41},22};}
};
void worker() {
    unsigned creates=0;
    VideoWorker worker([&](unsigned w,unsigned h,VideoCodec c){check(w==32&&h==32&&c==VideoCodec::avc444v2,"codec not delivered to worker factory");++creates;return std::make_unique<Encoder>(c);},VideoCodec::avc444v2);
    auto await=[&]{const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);for(;;){if(auto v=worker.take())return *v;check(std::chrono::steady_clock::now()<deadline,"worker timed out");std::this_thread::yield();}};
    Frame f{32,32,Bytes(4096)};
    worker.submit(f,1,false);auto first=await();check(first.frame&&first.error.empty()&&creates==1,"initial worker frame");
    worker.submit(f,2,true);auto second=await();check(second.frame&&second.frame->key_frame&&creates==2,"generation refresh must discard old codec references");
}
}
int main(){try{negotiation();framing(VideoCodec::avc444);framing(VideoCodec::avc444v2);worker();std::cout<<"PASS: AVC444/v2 capability intersection, LC/region framing, atomic rejection, subframe ordering and worker reference reset\n";}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
