#include "lrdp/avc444.hpp"
#include <algorithm>
#include <iostream>
#include <random>

using namespace lrdp;
namespace {
unsigned checks = 0;
void check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) { ++checks; try { f(); } catch (const ProtocolError&) { return; } throw std::runtime_error("invalid planes were accepted"); }
struct Storage {
    unsigned w,h; std::array<Bytes,3> planes; std::array<std::size_t,3> strides;
    Storage(unsigned width, unsigned height, bool full) : w(width),h(height) {
        for (unsigned c=0;c<3;++c) {
            auto columns = c && !full ? w/2 : w, rows = c && !full ? h/2 : h;
            strides[c]=columns+17; planes[c].resize(strides[c]*rows+31,0xda);
        }
    }
    Yuv444View source() const {return {w,h,{{{planes[0],strides[0]},{planes[1],strides[1]},{planes[2],strides[2]}}}};}
    Yuv420View target() {return {w,h,{{{planes[0],strides[0]},{planes[1],strides[1]},{planes[2],strides[2]}}}};}
    std::uint8_t at(unsigned c,unsigned x,unsigned y) const {return planes[c][y*strides[c]+x];}
    void guards() const {
        for(unsigned c=0;c<3;++c) {
            const auto width=c?w/2:w, height=c?h/2:h;
            for(unsigned y=0;y<height;++y) for(auto x=width;x<strides[c];++x)
                check(planes[c][y*strides[c]+x]==0xda,"overwritten row guard");
            for(auto i=height*strides[c];i<planes[c].size();++i) check(planes[c][i]==0xda,"overwritten plane guard");
        }
    }
};
void vectors(unsigned w,unsigned h,VideoCodec codec,std::mt19937& random) {
    Storage s(w,h,true), a(w,h,false), b(w,h,false);
    for(unsigned c=0;c<3;++c) for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x)
        s.planes[c][y*s.strides[c]+x]=std::uint8_t(random());
    pack_avc444(s.source(),a.target(),b.target(),codec);
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) check(a.at(0,x,y)==s.at(0,x,y),"luma changed");
    // Independent inverse from the published B1..B9 equations; no production
    // decoder or encoder roundtrip is used to define the expected values.
    for(unsigned c=1;c<3;++c) for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        if(x%2==0 && y%2==0) {
            const int reconstructed=4*a.at(c,x/2,y/2)-s.at(c,x+1,y)-s.at(c,x,y+1)-s.at(c,x+1,y+1);
            check(int(s.at(c,x,y))-reconstructed>=0 && int(s.at(c,x,y))-reconstructed<=3,"2x2 mean is not invertible within rounding bound");
            continue;
        }
        unsigned px=0,py=0,plane=0;
        if(codec==VideoCodec::avc444) {
            if(y%2) {px=x;py=(y/16)*16+(y%16)/2+(c-1)*8;}
            else {plane=c;px=x/2;py=y/2;}
        } else {
            if(x%2) {px=x/2+(c-1)*(w/2);py=y;}
            else {plane=x%4?2:1;px=x/4+(c-1)*(w/4);py=y/2;}
        }
        check(b.at(plane,px,py)==s.at(c,x,y),"lost or transposed full-chroma sample");
    }
    a.guards();b.guards();
    auto bad=a.target();bad.planes[1].bytes=bad.planes[1].bytes.first(3);
    const auto before=a.planes;
    rejects([&]{pack_avc444(s.source(),bad,b.target(),codec);});check(a.planes==before,"partial output on invalid input");
    bad=a.target();bad.planes[2]=bad.planes[1];rejects([&]{pack_avc444(s.source(),bad,b.target(),codec);});
    auto input=s.source();input.planes[0].bytes=a.planes[0];input.planes[0].stride=a.strides[0];
    rejects([&]{pack_avc444(input,a.target(),b.target(),codec);});
}
}
int main() {
    try {
        std::mt19937 random(0x444123);
        for(auto codec:{VideoCodec::avc444,VideoCodec::avc444v2})
            for(auto [w,h]:{std::pair{16U,16U},{32U,48U},{80U,32U},{1920U,1088U}}) vectors(w,h,codec,random);
        Storage s(16,16,true),a(16,16,false),b(16,16,false);
        for(unsigned bad:{0U,1U,15U,17U,16385U,0xffffffffU}) {
            auto input=s.source();input.width=bad;rejects([&]{pack_avc444(input,a.target(),b.target(),VideoCodec::avc444);});
        }
        rejects([&]{pack_avc444(s.source(),a.target(),b.target(),VideoCodec::avc420);});
        std::cout<<"PASS: "<<checks<<" AVC444/v2 plane, macroblock, stride, exact chroma, rounding, aliasing and atomic-validation checks\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
