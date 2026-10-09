#include "lrdp/clipboard/rich_content.hpp"
#include <algorithm>
#include <bit>
#include <limits>
namespace lrdp {
void ClipboardImage::validate() const {
    require(width && height && width<=8192 && height<=8192 && std::uint64_t(width)*height<=(rich_clipboard_limit-124)/4,"clipboard image allocation quota");
    require(bgra.size()==std::size_t(width)*height*4,"clipboard image byte count");
}
Bytes encode_clipboard_dib(const ClipboardImage& image, bool v5) {
    image.validate(); const unsigned stride = v5 ? image.width*4 : (image.width*3+3)&~3U;
    Writer out(rich_clipboard_limit); out.le32(v5?124:40).le32(image.width).le32(image.height).le16(1).le16(v5?32:24)
        .le32(v5?3:0).le32(stride*image.height).zeros(16);
    if(v5) out.le32(0xff0000).le32(0xff00).le32(0xff).le32(0xff000000).le32(0x73524742).zeros(48).le32(4).zeros(12);
    for (unsigned y=image.height;y;--y) {
        const auto* row=image.bgra.data()+std::size_t(y-1)*image.width*4;
        for(unsigned x=0;x<image.width;++x) out.raw(View(row+x*4,v5?4:3));
        out.zeros(stride-image.width*(v5?4:3));
    }
    return std::move(out).finish();
}
ClipboardImage decode_clipboard_dib(View data) {
    require(data.size()<=rich_clipboard_limit,"DIB payload quota"); Reader in(data);
    const auto header=in.le32(); require(header==40 || header==108 || header==124,"unsupported DIB header");
    require(data.size()>=header,"truncated DIB header");
    const auto width=in.i32(), signed_height=in.i32();
    require(width>0 && width<=8192 && signed_height && signed_height>=-8192 && signed_height<=8192,"invalid DIB geometry");
    const unsigned height=unsigned(signed_height<0?-signed_height:signed_height);
    require(std::uint64_t(width)*height<=(rich_clipboard_limit-124)/4,"DIB decoded allocation quota");
    require(in.le16()==1,"invalid DIB planes"); const auto bits=in.le16();
    require(bits==1 || bits==4 || bits==8 || bits==16 || bits==24 || bits==32,"unsupported DIB pixel depth");
    const auto compression=in.le32(), image_size=in.le32(); in.skip(8); const auto colors=in.le32(); in.skip(4);
    require(compression==0 || (compression==3 && (bits==16 || bits==32)),"compressed DIB unsupported");
    std::uint32_t masks[4] = {bits==16?0x7c00U:0xff0000U,bits==16?0x3e0U:0xff00U,bits==16?0x1fU:0xffU,0};
    if(header>40) {
        for(auto& mask:masks) mask=in.le32();
        const auto space=in.le32();
        require(space==0x73524742 || space==0x57696e20 || space==0,"unsupported DIB color space");
        auto calibration=in.take(48);
        if(space==0) require(std::all_of(calibration.begin(),calibration.end(),[](auto b){return b==0;}),"calibrated DIB requires color management");
        if(header==124) { in.skip(4); require(in.le32()==0 && in.le32()==0,"embedded/linked DIB profiles unsupported"); in.skip(4); }
    } else if(compression==3) for(unsigned i=0;i<3;++i) masks[i]=in.le32();
    if(compression==0) { masks[0]=bits==16?0x7c00:0xff0000; masks[1]=bits==16?0x3e0:0xff00; masks[2]=bits==16?0x1f:0xff; masks[3]=0; }
    if(bits==16 || bits==32) {
        std::uint32_t used=0;
        for(unsigned i=0;i<4;++i) {
            const auto mask=masks[i]; require(mask || i==3,"empty DIB RGB mask"); if(!mask) continue;
            const auto shifted=mask>>std::countr_zero(mask);
            require((shifted&(shifted+1U))==0 && !(mask&used) && (bits==32 || mask<=65535),"noncontiguous/overlapping DIB masks"); used|=mask;
        }
    }
    const auto palette_size=bits<=8?(colors?colors:1U<<bits):colors;
    require(palette_size<=256 && (bits>8 || palette_size<=1U<<bits),"DIB palette quota"); const auto palette=in.take(std::size_t(palette_size)*4);
    const auto stride=(std::size_t(width)*bits+31)/32*4;
    require((image_size==0 || image_size==stride*height) && in.remaining()==stride*height,"DIB pixel payload size mismatch");
    const auto pixels=in.take(in.remaining()); ClipboardImage result{unsigned(width),height,Bytes(std::size_t(width)*height*4)};
    auto component=[](std::uint32_t p,std::uint32_t mask) -> std::uint8_t {
        if(!mask) return 255;
        const auto shift=std::countr_zero(mask); const auto max=mask>>shift;
        return std::uint8_t((std::uint64_t((p&mask)>>shift)*255+max/2)/max);
    };
    for(unsigned y=0;y<height;++y) {
        const auto row=pixels.subspan(std::size_t(signed_height<0?y:height-y-1)*stride,stride);
        for(unsigned x=0;x<unsigned(width);++x) {
            auto* out=result.bgra.data()+(std::size_t(y)*unsigned(width)+x)*4;
            if(bits<=8) {
                const unsigned index=bits==8?row[x]:bits==4?(row[x/2]>>(x%2?0:4))&15:(row[x/8]>>(7-x%8))&1;
                require(index<palette_size,"DIB palette index out of bounds"); std::copy_n(palette.data()+index*4,3,out); out[3]=255;
            } else if(bits==24) { std::copy_n(row.data()+x*3,3,out); out[3]=255; }
            else {
                Reader p(row.subspan(std::size_t(x)*bits/8)); const auto pixel=bits==16?std::uint32_t(p.le16()):p.le32();
                out[0]=component(pixel,masks[2]); out[1]=component(pixel,masks[1]); out[2]=component(pixel,masks[0]); out[3]=component(pixel,masks[3]);
            }
        }
    }
    return result;
}
Bytes encode_clipboard_bmp(const ClipboardImage& image) {
    const auto dib=encode_clipboard_dib(image); Writer out(rich_clipboard_limit+14);
    out.raw({'B','M'}).le32(std::uint32_t(dib.size()+14)).le32(0).le32(138).raw(dib); return std::move(out).finish();
}
ClipboardImage decode_clipboard_bmp(View data) {
    Reader in(data); require(in.le16()==0x4d42 && in.le32()==data.size(),"invalid BMP framing");
    in.skip(4); const auto offset=in.le32(); const auto dib=in.take(in.remaining());
    // Validate using the exact packed DIB profile. Do not follow file offsets into metadata.
    auto image=decode_clipboard_dib(dib); Reader header(dib); const auto size=header.le32();
    header.skip(10); const auto bits=header.le16(); const auto compression=header.le32(); header.skip(12); const auto colors=header.le32();
    const auto palette=bits<=8?(colors?colors:1U<<bits):colors;
    require(offset==14+size+(size==40 && compression==3?12:0)+palette*4,"BMP pixel offset mismatch"); return image;
}
}
