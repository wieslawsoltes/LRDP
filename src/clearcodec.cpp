#include "lrdp/lossless.hpp"
#include <algorithm>
#include <array>

namespace lrdp {
std::optional<Bytes> clearcodec_residual(View source, unsigned width, unsigned height,
                                       std::size_t stride, std::uint8_t sequence,
                                       std::size_t budget) {
    require(width && height && width<=65535 && height<=65535 &&
            std::uint64_t(width)*height<=16*1024*1024, "ClearCodec dimensions exceed quota");
    const auto row_bytes=std::size_t(width)*4;
    require(stride>=row_bytes && source.size()>=row_bytes &&
            std::size_t(height-1)<=(source.size()-row_bytes)/stride, "invalid ClearCodec source stride or extent");
    // Strictly cheaper than raw pixels; includes the 2-byte stream header and
    // three 32-bit composite lengths. Both paths have the same GFX header.
    budget=std::min(budget,std::size_t(width)*height*4);
    if(budget<=18) return std::nullopt;
    Writer residual(budget-14);
    std::array<std::uint8_t,3> color{};
    std::uint32_t count=0;
    auto flush=[&] {
        const std::size_t size=count<255?4:count<65535?6:10;
        if(size>=budget-14-residual.size()) return false;
        residual.raw(color);
        if(count<255) residual.u8(count);
        else { residual.u8(255); if(count<65535) residual.le16(count); else residual.le16(65535).le32(count); }
        return true;
    };
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        const auto* pixel=source.data()+std::size_t(y)*stride+std::size_t(x)*4;
        if(pixel[3]!=255) return std::nullopt; // Preserve alpha through raw BGRA.
        const std::array<std::uint8_t,3> next{pixel[0],pixel[1],pixel[2]};
        if(count && next!=color) { if(!flush()) return std::nullopt; count=0; }
        color=next; ++count;
    }
    if(!flush()) return std::nullopt;
    Writer output(budget); output.u8(0).u8(sequence).le32(std::uint32_t(residual.size())).le32(0).le32(0).raw(residual.bytes());
    return std::move(output).finish();
}
} // namespace lrdp
