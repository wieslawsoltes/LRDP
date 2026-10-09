#include "lrdp/lossless.hpp"
#include "lrdp/graphics.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <random>
using namespace lrdp;
namespace {
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f){++checks;try{f();}catch(const ProtocolError&){return;}throw std::runtime_error("invalid lossless input accepted");}
Bytes decode_clear(View wire,unsigned width,unsigned height,unsigned seq) {
    Reader in(wire);check(in.u8()==0 && in.u8()==seq,"ClearCodec header/sequence");
    const auto size=in.le32();check(in.le32()==0 && in.le32()==0 && size==in.remaining(),"ClearCodec composite lengths");
    Bytes pixels;pixels.reserve(std::size_t(width)*height*4);
    while(!in.empty()) {
        const auto b=in.u8(),g=in.u8(),r=in.u8();std::uint32_t count=in.u8();
        if(count==255){count=in.le16();if(count==65535)count=in.le32();}
        check(count>0 && count<=std::size_t(width)*height-pixels.size()/4,"ClearCodec residual run extent");
        for(unsigned i=0;i<count;++i)pixels.insert(pixels.end(),{b,g,r,255});
    }
    check(pixels.size()==std::size_t(width)*height*4,"ClearCodec residual pixel count");return pixels;
}
class Decoder {
public:
    Frame frame;
    unsigned sequence=0;
    unsigned clears=0,raw=0,fills=0;
    explicit Decoder(unsigned w,unsigned h):frame{w,h,Bytes(std::size_t(w)*h*4)}{}
    void accept(View packet) {
        Reader in(packet);const auto kind=in.le16();check(in.le16()==0 && in.le32()==packet.size(),"GFX command framing");
        check(in.le16()==0,"GFX surface ID");
        auto region=[&] {std::array<unsigned,4> r{in.le16(),in.le16(),in.le16(),in.le16()};check(r[0]<r[2] && r[1]<r[3] && r[2]<=frame.width && r[3]<=frame.height,"GFX rectangle bounds");return r;};
        if(kind==4) {
            const auto color=in.take(4);const auto count=in.le16();check(count>0 && count<=256,"SolidFill count");fills+=count;
            for(unsigned i=0;i<count;++i){const auto r=region();for(unsigned y=r[1];y<r[3];++y)for(unsigned x=r[0];x<r[2];++x)
                std::copy(color.begin(),color.end(),frame.bgra.begin()+std::ptrdiff_t((std::size_t(y)*frame.width+x)*4));}
        } else {
            check(kind==1,"unexpected lossless command");const auto codec=in.le16();check(in.u8()==32,"lossless pixel format");
            const auto r=region();const auto payload=in.take(in.le32());Bytes pixels;
            if(codec==8){pixels=decode_clear(payload,r[2]-r[0],r[3]-r[1],sequence);sequence=(sequence+1)&255;++clears;}
            else {check(codec==0,"unexpected lossless codec");pixels.assign(payload.begin(),payload.end());++raw;}
            const auto stride=std::size_t(r[2]-r[0])*4;check(pixels.size()==stride*(r[3]-r[1]),"decoded rectangle length");
            for(unsigned y=r[1];y<r[3];++y)std::copy_n(pixels.data()+std::size_t(y-r[1])*stride,stride,frame.bgra.data()+(std::size_t(y)*frame.width+r[0])*4);
        }
        in.end();
    }
};
Frame solid(unsigned w,unsigned h,std::array<std::uint8_t,4> pixel={20,40,60,255}) {
    Frame f{w,h,Bytes(std::size_t(w)*h*4)};for(std::size_t i=0;i<f.bgra.size();i+=4)std::copy(pixel.begin(),pixel.end(),f.bgra.begin()+std::ptrdiff_t(i));return f;
}
void codec() {
    for(unsigned n:{10U,254U,255U,256U,65534U,65535U,65536U,1048576U}) {
        const unsigned w=n<=65535?n:256,h=n<=65535?1:n/256;
        auto f=solid(w,h);auto encoded=clearcodec_residual(f.bgra,w,h,w*4,57,f.bgra.size());check(encoded.has_value(),"solid residual did not compress");
        check(decode_clear(*encoded,w,h,57)==f.bgra,"residual run boundary corruption");
        Reader in(*encoded);in.skip(17);const auto run=in.u8();check((std::size_t(w)*h<255)==(run<255),"noncanonical run length");
    }
    // Exact >65535 runs with a single row are prohibited by bitmap dimensions;
    // exercise the sentinel at 65535 on a 255 x 257 rectangle.
    auto f=solid(255,257);auto encoded=clearcodec_residual(f.bgra,255,257,1020,0,f.bgra.size());
    check(encoded && encoded->size()==24,"32-bit sentinel run encoding");check(decode_clear(*encoded,255,257,0)==f.bgra,"65535 run");
    f=solid(20,3);Bytes stride(3*100,0x7d);for(unsigned y=0;y<3;++y)std::copy_n(f.bgra.data()+y*80,80,stride.data()+y*100);
    encoded=clearcodec_residual(stride,20,3,100,255,240);check(encoded && decode_clear(*encoded,20,3,255)==f.bgra,"padded-stride codec");
    stride[100+3]=254;check(!clearcodec_residual(stride,20,3,100,0,240),"alpha was discarded");
    check(!clearcodec_residual(f.bgra,20,3,80,0,18),"codec expanded budget");
    rejects([&]{clearcodec_residual(f.bgra,0,1,80,0,100);});
    rejects([&]{clearcodec_residual(f.bgra,20,3,79,0,100);});
    rejects([&]{clearcodec_residual(View(f.bgra).first(239),20,3,80,0,100);});
    rejects([&]{clearcodec_residual(f.bgra,0xffffffffU,0xffffffffU,80,0,100);});
}
void frames() {
    LosslessEncoder encoder;Decoder decoder(258,201);auto frame=solid(258,201);
    auto update=[&]{auto commands=encoder.encode(frame);for(const auto& c:commands)decoder.accept(c);check(decoder.frame.bgra==frame.bgra,"frame decoder pixel mismatch");return commands;};
    check(update().size()==1 && decoder.fills==21,"solid rectangles not grouped");
    check(update().empty(),"unchanged frame produced commands");
    const auto point=(std::size_t(90)*frame.width+130)*4;frame.bgra[point]=199;
    auto one=update();check(one.size()==1 && encoder.statistics().changed_pixels==1 && encoder.statistics().command_bytes==24,"single pixel update not bounded");
    // Patterned stripes compress; high-entropy and alpha preserve raw BGRA.
    for(unsigned y=0;y<frame.height;++y)for(unsigned x=0;x<frame.width;++x)frame.bgra[(std::size_t(y)*frame.width+x)*4]=std::uint8_t(y%2?30:90);
    update();check(decoder.clears>0,"ClearCodec path unused");
    std::mt19937 random(0x434c4541);
    for(auto& value:frame.bgra)value=std::uint8_t(random());
    update();check(decoder.raw>0,"raw alpha fallback unused");
    const auto sequence=decoder.sequence;encoder.invalidate();update();check(decoder.sequence==sequence,"non-ClearCodec frame altered sequence");
    frame=solid(258,201);frame.bgra[0]=21;update();
    for(unsigned i=0;i<300;++i) {
        for(unsigned y=0;y<frame.height;++y)for(unsigned x=0;x<frame.width;++x)frame.bgra[(std::size_t(y)*frame.width+x)*4]=std::uint8_t((y%2?20:40)+i%2);
        if(i%7==0)encoder.invalidate();
        update();
    }
    check(decoder.clears>256,"ClearCodec sequence wrap not exercised");
    auto invalid=frame;invalid.bgra.pop_back();rejects([&]{encoder.encode(invalid);});check(update().empty(),"rejected frame advanced reference");
    for(unsigned trial=0;trial<200;++trial) {
        for(unsigned i=0;i<100;++i){const auto n=std::size_t(random())%(frame.bgra.size()/4);frame.bgra[n*4+random()%4]=std::uint8_t(random());}
        update();
    }
}
Bytes unwrap(View data) {Reader in(data);check(in.u8()==0xe0 && in.u8()==4,"test segment header");const auto b=in.take(in.remaining());return Bytes(b.begin(),b.end());}
void graphics() {
    Graphics g;Writer cap;cap.le16(1).le32(0x80105).le32(4).le32(0);g.receive(graphics_pdu(0x12,cap.bytes()),false);
    Monitor monitor;monitor.width=256;monitor.height=200;g.reset(validate_layout({monitor}));(void)g.drain();
    auto frame=solid(256,200);g.lossless_frame(frame);auto packets=g.drain();check(packets.size()==3 && g.in_flight()==1,"lossless frame boundaries");
    check(unwrap(packets[0])[0]==11 && unwrap(packets[2])[0]==12,"lossless Start/End order");
    g.lossless_frame(frame);check(g.drain().empty() && g.in_flight()==1,"empty damage consumed frame credit");
    frame.bgra[0]=99;g.lossless_frame(frame);(void)g.drain();frame.bgra[0]=80;
    rejects([&]{g.lossless_frame(frame);});check(g.drain().empty(),"backpressure produced partial frame");
    Writer ack;ack.le32(0).le32(2).le32(2);g.receive(graphics_pdu(13,ack.bytes()),false);
    g.lossless_frame(frame);check(g.lossless_statistics().changed_pixels==1,"blocked reference lost damage");
}
}
int main(){try{codec();frames();graphics();std::cout<<"PASS: "<<checks<<" lossless codec/damage/sequence/alpha/ACK checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
