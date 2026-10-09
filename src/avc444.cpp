#include "lrdp/avc444.hpp"
#include <algorithm>
#include <limits>

namespace lrdp {
namespace {
template<class Plane> void validate(const Plane& plane, unsigned width, unsigned height) {
    require(plane.stride >= width && plane.stride <= 1024*1024, "invalid AVC plane stride");
    const auto needed = std::size_t(height - 1) * plane.stride + width;
    require(plane.bytes.size() >= needed, "truncated AVC plane");
}
bool overlaps(View a, View b) {
    const auto x = reinterpret_cast<std::uintptr_t>(a.data()), y = reinterpret_cast<std::uintptr_t>(b.data());
    // Subtraction, not pointer-end addition: no address-space overflow.
    return x <= y ? y-x < a.size() : x-y < b.size();
}
}
void pack_avc444(const Yuv444View& src, const Yuv420View& main,
                 const Yuv420View& aux, VideoCodec codec) {
    require(codec == VideoCodec::avc444 || codec == VideoCodec::avc444v2, "invalid full-chroma packing mode");
    const auto w = src.width, h = src.height;
    require(w && h && w <= 16384 && h <= 16384 && w%16 == 0 && h%16 == 0 &&
            std::uint64_t(w)*h <= 16*1024*1024, "AVC444 requires bounded 16-aligned planes");
    require(main.width == w && main.height == h && aux.width == w && aux.height == h,
            "AVC444 view dimensions do not match");
    for (const auto& plane : src.planes) validate(plane, w, h);
    const std::array<MutablePlane, 6> outputs{main.planes[0],main.planes[1],main.planes[2],
                                           aux.planes[0],aux.planes[1],aux.planes[2]};
    for (std::size_t i=0; i<outputs.size(); ++i) {
        validate(outputs[i], i%3 == 0 ? w : w/2, i%3 == 0 ? h : h/2);
        for (const auto& input : src.planes) require(!overlaps(input.bytes, outputs[i].bytes), "aliased AVC444 input/output");
        for (std::size_t j=0; j<i; ++j) require(!overlaps(outputs[i].bytes, outputs[j].bytes), "aliased AVC444 output planes");
    }
    for (unsigned y=0; y<h; ++y)
        std::copy_n(src.planes[0].bytes.data()+y*src.planes[0].stride, w,
                    main.planes[0].bytes.data()+y*main.planes[0].stride);
    for (unsigned channel=1; channel<=2; ++channel) {
        const auto& input = src.planes[channel];
        const auto& filtered = main.planes[channel];
        for (unsigned y=0; y<h/2; ++y) {
            const auto* even = input.bytes.data()+2*y*input.stride;
            const auto* odd = even+input.stride;
            auto* average = filtered.bytes.data()+y*filtered.stride;
            for (unsigned x=0; x<w/2; ++x)
                average[x] = std::uint8_t((unsigned(even[2*x])+even[2*x+1]+odd[2*x]+odd[2*x+1])/4);
            if (codec == VideoCodec::avc444) {
                // B4/B5: U and V odd source rows interleaved every eight rows.
                const unsigned row = (y/8)*16 + y%8 + (channel-1)*8;
                std::copy_n(odd, w, aux.planes[0].bytes.data()+row*aux.planes[0].stride);
                auto* remaining = aux.planes[channel].bytes.data()+y*aux.planes[channel].stride;
                for (unsigned x=0; x<w/2; ++x) remaining[x] = even[2*x+1];
            } else {
                // B4/B5: all odd columns in side-by-side U/V halves of aux Y.
                for (unsigned row=0; row<2; ++row) {
                    const auto* source = row ? odd : even;
                    auto* dest = aux.planes[0].bytes.data()+(2*y+row)*aux.planes[0].stride+(channel-1)*(w/2);
                    for (unsigned x=0; x<w/2; ++x) dest[x] = source[2*x+1];
                }
                // B6..B9: odd rows' remaining even columns split by x modulo 4.
                for (unsigned plane=1; plane<=2; ++plane) {
                    auto* dest = aux.planes[plane].bytes.data()+y*aux.planes[plane].stride+(channel-1)*(w/4);
                    for (unsigned x=0; x<w/4; ++x) dest[x] = odd[4*x+2*(plane-1)];
                }
            }
        }
    }
}
} // namespace lrdp
