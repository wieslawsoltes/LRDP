#include "lrdp/platform/capture_frame.hpp"
#include <cstring>

namespace lrdp {
Frame copy_capture_frame(View bytes, unsigned width, unsigned height, std::int32_t pitch,
                         PackedFormat format, std::optional<CaptureCrop> requested) {
    require(width > 0 && height > 0 && width <= 8192 && height <= 8192 && std::uint64_t(width) * height <= 16 * 1024 * 1024,
            "capture geometry exceeds policy");
    require(pitch > 0 && std::uint64_t(pitch) >= std::uint64_t(width) * 4, "invalid or unsupported capture pitch");
    const auto required = std::uint64_t(height - 1) * unsigned(pitch) + std::uint64_t(width) * 4;
    require(required <= bytes.size(), "capture chunk is shorter than its declared geometry");
    const auto crop = requested.value_or(CaptureCrop{0, 0, width, height});
    require(crop.width && crop.height && crop.x < width && crop.y < height && crop.width <= width - crop.x && crop.height <= height - crop.y,
            "capture crop falls outside the frame");
    Frame result{crop.width, crop.height, Bytes(std::size_t(crop.width) * crop.height * 4)};
    const bool red_first = format == PackedFormat::rgba || format == PackedFormat::rgbx;
    for (unsigned y = 0; y < crop.height; ++y) {
        const auto* input = bytes.data() + std::size_t(y + crop.y) * unsigned(pitch) + std::size_t(crop.x) * 4;
        auto* output = result.bgra.data() + std::size_t(y) * crop.width * 4;
        std::memcpy(output, input, std::size_t(crop.width) * 4);
        for (unsigned x = 0; x < crop.width; ++x) {
            if (red_first) std::swap(output[x * 4], output[x * 4 + 2]);
            output[x * 4 + 3] = 255;
        }
    }
    return result;
}
unsigned rdp_evdev_key(std::uint16_t code, std::uint16_t flags) {
    if (flags & 0x200) return code == 0x1d || code == 0x45 ? 119U : 0U;
    if (!(flags & 0x100)) return code > 0 && code <= 0x58 ? code : 0;
    switch (code) {
    case 0x1c: return 96; case 0x1d: return 97; case 0x35: return 98;
    case 0x37: return 99; case 0x38: return 100; case 0x46: return 119;
    case 0x47: return 102; case 0x48: return 103; case 0x49: return 104;
    case 0x4b: return 105; case 0x4d: return 106; case 0x4f: return 107;
    case 0x50: return 108; case 0x51: return 109; case 0x52: return 110;
    case 0x53: return 111; case 0x5b: return 125; case 0x5c: return 126;
    case 0x5d: return 127; default: return 0;
    }
}
} // namespace lrdp
