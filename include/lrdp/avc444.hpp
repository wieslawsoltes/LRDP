#pragma once
#include "wire.hpp"
#include <array>

namespace lrdp {
enum class VideoCodec { avc420, avc444, avc444v2 };
constexpr const char* codec_name(VideoCodec codec) noexcept {
    switch (codec) {
    case VideoCodec::avc420: return "AVC420";
    case VideoCodec::avc444: return "AVC444";
    case VideoCodec::avc444v2: return "AVC444v2";
    }
    return "unknown";
}
struct ConstPlane { View bytes; std::size_t stride = 0; };
struct MutablePlane { std::span<std::uint8_t> bytes; std::size_t stride = 0; };
struct Yuv444View {
    unsigned width = 0, height = 0;
    std::array<ConstPlane, 3> planes; // Y, U, V. Positive byte strides.
};
struct Yuv420View {
    unsigned width = 0, height = 0;
    std::array<MutablePlane, 3> planes;
};
// MS-RDPEGFX 3.3.8.3.2/.3: a full-chroma image becomes two ordinary
// 4:2:0 views, to be encoded IN ORDER by ONE H.264 encoder context.
// No allocation, RGB conversion, filtering beyond the specified 2x2 mean,
// or codec operation occurs here. Caller pads edge pixels to 16x16 blocks.
// Invalid/overlapping storage is rejected before any output byte is written.
void pack_avc444(const Yuv444View& source, const Yuv420View& main,
                 const Yuv420View& auxiliary, VideoCodec codec);
} // namespace lrdp
