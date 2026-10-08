#include "lrdp/pointer.hpp"
#include "lrdp/mcs.hpp"
#include <algorithm>
#include <limits>

namespace lrdp {
void PointerShape::validate() const {
    require(width > 0 && height > 0 && width <= 384 && height <= 384, "native cursor dimensions exceed policy");
    require(hot_x < width && hot_y < height, "cursor hotspot outside image");
    require(bgra.size() == std::size_t(width) * height * 4, "invalid cursor pixel buffer");
}
PointerShape fit_pointer(const PointerShape& shape, unsigned maximum) {
    shape.validate(); require(maximum > 0 && maximum <= 384, "invalid cursor bound");
    if (shape.width <= maximum && shape.height <= maximum) return shape;
    // Preserve aspect ratio and hotspot for clients without large-pointer support.
    // The native image is left intact. Nearest-neighbour preserves sharp masks.
    const auto largest = std::max(shape.width, shape.height);
    PointerShape result;
    result.width = std::uint16_t(std::max(1U, unsigned(shape.width) * maximum / largest));
    result.height = std::uint16_t(std::max(1U, unsigned(shape.height) * maximum / largest));
    result.hot_x = std::uint16_t(unsigned(shape.hot_x) * result.width / shape.width);
    result.hot_y = std::uint16_t(unsigned(shape.hot_y) * result.height / shape.height);
    result.bgra.resize(std::size_t(result.width) * result.height * 4);
    for (unsigned y = 0; y < result.height; ++y) for (unsigned x = 0; x < result.width; ++x) {
        const auto sx = x * shape.width / result.width, sy = y * shape.height / result.height;
        std::copy_n(shape.bgra.data() + (std::size_t(sy) * shape.width + sx) * 4, 4,
                    result.bgra.data() + (std::size_t(y) * result.width + x) * 4);
    }
    return result;
}
Bytes pointer_system(bool hidden) {
    Writer out; out.le16(1).le16(0).le32(hidden ? 0 : 0x7f00); return std::move(out).finish();
}
namespace {
Bytes image(const PointerShape& shape, std::uint16_t slot, bool alpha, bool large) {
    shape.validate();
    const unsigned pixel_size = alpha ? 4U : 3U;
    const unsigned xor_stride = (unsigned(shape.width) * pixel_size + 1U) & ~1U;
    const unsigned and_stride = ((unsigned(shape.width) + 15U) / 16U) * 2U;
    Writer out;
    if (large) out.le16(alpha ? 32 : 24);
    else { out.le16(alpha ? 8 : 6).le16(0); if (alpha) out.le16(32); }
    out.le16(slot).le16(shape.hot_x).le16(shape.hot_y).le16(shape.width).le16(shape.height);
    if (large) out.le32(and_stride * shape.height).le32(xor_stride * shape.height);
    else out.le16(and_stride * shape.height).le16(xor_stride * shape.height);
    Bytes mask(std::size_t(and_stride) * shape.height);
    for (unsigned row = 0; row < shape.height; ++row) {
        const unsigned y = shape.height - row - 1;
        for (unsigned x = 0; x < shape.width; ++x) {
            const auto* p = shape.bgra.data() + (std::size_t(y) * shape.width + x) * 4;
            const unsigned a = p[3];
            const bool transparent = alpha ? a == 0 : a < 128;
            if (transparent) mask[std::size_t(row) * and_stride + x / 8] |= std::uint8_t(0x80U >> (x % 8));
            if (alpha) {
                // Windows alpha cursors and XRender/XFixes use premultiplied color.
                for (unsigned c = 0; c < 3; ++c) out.u8(std::min(unsigned(p[c]), a));
                out.u8(a);
            } else {
                for (unsigned c = 0; c < 3; ++c)
                    out.u8(transparent ? 0 : std::min(255U, (unsigned(p[c]) * 255U + a / 2U) / a));
            }
        }
        out.zeros(xor_stride - unsigned(shape.width) * pixel_size);
    }
    out.raw(mask); return std::move(out).finish();
}
} // namespace
Bytes pointer_image(const PointerShape& shape, std::uint16_t slot, bool alpha) {
    require(shape.width <= 32 && shape.height <= 32, "large cursors require capability negotiation");
    return image(shape, slot, alpha, false);
}
std::vector<Bytes> fastpath_output(unsigned code, View data, std::uint32_t max_request, std::size_t fragment_size) {
    require(code <= 12 && code != 7, "invalid fast-path output code");
    require(data.size() <= max_request && max_request <= 16 * 1024 * 1024, "fast-path update exceeds negotiated quota");
    require(fragment_size > 0 && fragment_size <= 16377, "invalid fast-path fragment size");
    std::vector<Bytes> packets;
    std::size_t offset = 0;
    do {
        const auto count = std::min(fragment_size, data.size() - offset);
        const unsigned fragment = data.size() <= fragment_size ? 0U :
            offset == 0 ? 2U : offset + count == data.size() ? 1U : 3U;
        // Two-byte total length; one update fragment per PDU.
        Writer out(16383); out.u8(0).be16(unsigned(count + 6) | 0x8000)
            .u8(code | (fragment << 4)).le16(unsigned(count)).raw(data.subspan(offset, count));
        packets.push_back(std::move(out).finish()); offset += count;
    } while (offset < data.size());
    return packets;
}
std::vector<Bytes> PointerUpdate::packets(std::uint32_t max_request) const {
    if (fastpath_code) return fastpath_output(*fastpath_code, payload, max_request);
    return {mcs_data(global_channel, share_data(27, payload))};
}
void PointerEncoder::configure(unsigned color_slots, unsigned alpha_slots, unsigned large_flags,
                               std::uint32_t max_request, bool fastpath) {
    require(color_slots <= 65535 && alpha_slots <= 65535, "invalid pointer capability");
    alpha_ = alpha_slots != 0;
    maximum_ = 32; max_request_ = std::min(max_request, 16U * 1024 * 1024);
    if (fastpath && alpha_) {
        if ((large_flags & 2) && max_request_ >= 608299) maximum_ = 384;
        else if ((large_flags & 1) && max_request_ >= 38055) maximum_ = 96;
    }
    capacity_ = std::min(32U, alpha_ ? alpha_slots : color_slots);
    cache_.clear(); selected_.reset(); hidden_ = false; clock_ = 0;
}
std::optional<PointerUpdate> PointerEncoder::update(const PointerShape& native) {
    auto shape = fit_pointer(native, maximum_);
    bool visible = false;
    for (std::size_t i = 3; i < shape.bgra.size(); i += 4) visible |= shape.bgra[i] != 0;
    if (!visible) {
        if (hidden_) return std::nullopt;
        hidden_ = true; selected_.reset(); return PointerUpdate{pointer_system(true), {}};
    }
    if (!capacity_) {
        if (!hidden_) return std::nullopt;
        hidden_ = false; return PointerUpdate{pointer_system(false), {}};
    }
    require(clock_ != std::numeric_limits<std::uint64_t>::max(), "cursor cache clock exhausted");
    ++clock_;
    for (unsigned i = 0; i < cache_.size(); ++i) {
        if (cache_[i].shape != shape) continue;
        cache_[i].used = clock_;
        if (selected_ == i && !hidden_) return std::nullopt;
        selected_ = i; hidden_ = false;
        Writer out; out.le16(7).le16(0).le16(i); return PointerUpdate{std::move(out).finish(), {}};
    }
    unsigned slot = unsigned(cache_.size());
    if (slot == capacity_) slot = unsigned(std::min_element(cache_.begin(), cache_.end(),
        [](const Entry& a, const Entry& b) { return a.used < b.used; }) - cache_.begin());
    const bool large = shape.width > 96 || shape.height > 96;
    const bool fast = shape.width > 32 || shape.height > 32;
    auto payload = image(shape, std::uint16_t(slot), alpha_, large);
    if (fast && !large) payload.erase(payload.begin(), payload.begin() + 4);
    PointerUpdate result{std::move(payload), fast ? std::optional<unsigned>(large ? 12U : 11U) : std::nullopt};
    if (slot == cache_.size()) cache_.push_back({std::move(shape), clock_});
    else cache_[slot] = {std::move(shape), clock_};
    selected_ = slot; hidden_ = false; return result;
}
} // namespace lrdp
