#include "lrdp/lossless.hpp"
#include "lrdp/graphics.hpp"
#include <algorithm>
#include <array>
#include <map>

namespace lrdp {
namespace {
struct Rect { unsigned left, top, right, bottom; };
void rectangle(Writer& w, const Rect& r) { w.le16(r.left).le16(r.top).le16(r.right).le16(r.bottom); }
using Color=std::array<std::uint8_t,4>;
Color color_at(const Frame& f, unsigned x, unsigned y) {
    const auto* p=f.bgra.data()+(std::size_t(y)*f.width+x)*4;
    return {p[0],p[1],p[2],p[3]};
}
}
std::vector<Bytes> LosslessEncoder::encode(const Frame& frame) {
    frame.validate();
    require(frame.width<=65535 && frame.height<=65535,"lossless surface dimensions exceed protocol limits");
    const bool full=frame.width!=previous_.width || frame.height!=previous_.height;
    std::vector<Bytes> commands;
    std::map<Color,std::vector<Rect>> fills;
    auto sequence=sequence_;
    LosslessStatistics statistics;
    for(unsigned top=0;top<frame.height;top+=32) for(unsigned left=0;left<frame.width;left+=128) {
        const auto right=std::min(left+128,frame.width),bottom=std::min(top+32,frame.height);
        Rect rect{left,top,right,bottom};
        if(!full) {
            rect={right,bottom,left,top};
            for(unsigned y=top;y<bottom;++y) {
                const auto offset=(std::size_t(y)*frame.width+left)*4;
                const auto* current=frame.bgra.data()+offset;
                const auto* old=previous_.bgra.data()+offset;
                if(std::equal(current,current+(right-left)*4,old)) continue;
                for(unsigned x=left;x<right;++x) {
                    const auto i=(x-left)*4;
                    if(std::equal(current+i,current+i+4,old+i)) continue;
                    rect.left=std::min(rect.left,x); rect.top=std::min(rect.top,y);
                    rect.right=std::max(rect.right,x+1); rect.bottom=std::max(rect.bottom,y+1);
                }
            }
            if(rect.left>=rect.right) continue;
        }
        const auto color=color_at(frame,rect.left,rect.top);
        bool solid=true;
        for(unsigned y=rect.top;solid && y<rect.bottom;++y) for(unsigned x=rect.left;x<rect.right;++x)
            if(color_at(frame,x,y)!=color) {solid=false;break;}
        const auto pixels=std::size_t(rect.right-rect.left)*(rect.bottom-rect.top);
        statistics.changed_pixels+=pixels;
        if(solid) { fills[color].push_back(rect); ++statistics.solid_rectangles; continue; }
        const auto stride=std::size_t(frame.width)*4;
        const auto source=View(frame.bgra).subspan(std::size_t(rect.top)*stride+std::size_t(rect.left)*4);
        auto compressed=clearcodec_residual(source,rect.right-rect.left,rect.bottom-rect.top,stride,sequence,pixels*4);
        Writer body; body.le16(0).le16(compressed?8:0).u8(0x20); rectangle(body,rect);
        if(compressed) {
            body.le32(std::uint32_t(compressed->size())).raw(*compressed);
            sequence=std::uint8_t(sequence+1U); ++statistics.clear_rectangles;
        } else {
            body.le32(std::uint32_t(pixels*4));
            for(unsigned y=0;y<rect.bottom-rect.top;++y) body.raw(source.subspan(std::size_t(y)*stride,std::size_t(rect.right-rect.left)*4));
            ++statistics.raw_rectangles;
        }
        commands.push_back(graphics_pdu(1,body.bytes()));
    }
    // Tile rectangles never overlap. Grouping solid tiles by full BGRA color
    // therefore cannot reorder dependent writes, including partially opaque pixels.
    for(const auto& [color,rectangles]:fills) for(std::size_t start=0;start<rectangles.size();start+=256) {
        const auto count=std::min<std::size_t>(256,rectangles.size()-start);
        Writer body; body.le16(0).raw(color).le16(unsigned(count));
        for(std::size_t i=0;i<count;++i) rectangle(body,rectangles[start+i]);
        commands.push_back(graphics_pdu(4,body.bytes()));
    }
    for(const auto& command:commands) statistics.command_bytes+=command.size();
    // The only persistent allocation is one packed BGRA reference; hashes are
    // not used to decide equality. Throwing allocation/validation cannot advance
    // the compressor's counter or replace the last transmitted reference.
    if(!commands.empty()) { Frame next=frame; previous_=std::move(next); sequence_=sequence; }
    statistics_=statistics;
    return commands;
}
} // namespace lrdp
