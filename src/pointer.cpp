#include "lrdp/pointer.hpp"
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
Bytes pointer_image(const PointerShape& shape, std::uint16_t slot, bool alpha) {
    shape.validate(); require(shape.width <= 32 && shape.height <= 32, "large cursors were not negotiated");
    const unsigned pixel_size = alpha ? 4U : 3U;
    const unsigned xor_stride = (unsigned(shape.width) * pixel_size + 1U) & ~1U;
    const unsigned and_stride = ((unsigned(shape.width) + 15U) / 16U) * 2U;
    Writer out; out.le16(alpha ? 8 : 6).le16(0);
    if (alpha) out.le16(32);
    out.le16(slot).le16(shape.hot_x).le16(shape.hot_y).le16(shape.width).le16(shape.height)
        .le16(and_stride * shape.height).le16(xor_stride * shape.height);
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
void PointerEncoder::configure(unsigned color_slots, unsigned alpha_slots) {
    require(color_slots <= 65535 && alpha_slots <= 65535, "invalid pointer capability");
    alpha_ = alpha_slots != 0;
    capacity_ = std::min(32U, alpha_ ? alpha_slots : color_slots);
    cache_.clear(); selected_.reset(); hidden_ = false; clock_ = 0;
}
std::optional<Bytes> PointerEncoder::update(const PointerShape& native) {
    auto shape = fit_pointer(native);
    bool visible = false;
    for (std::size_t i = 3; i < shape.bgra.size(); i += 4) visible |= shape.bgra[i] != 0;
    if (!visible) {
        if (hidden_) return std::nullopt;
        hidden_ = true; selected_.reset(); return pointer_system(true);
    }
    if (!capacity_) {
        if (!hidden_) return std::nullopt;
        hidden_ = false; return pointer_system(false);
    }
    require(clock_ != std::numeric_limits<std::uint64_t>::max(), "cursor cache clock exhausted");
    ++clock_;
    for (unsigned i = 0; i < cache_.size(); ++i) {
        if (cache_[i].shape != shape) continue;
        cache_[i].used = clock_;
        if (selected_ == i && !hidden_) return std::nullopt;
        selected_ = i; hidden_ = false;
        Writer out; out.le16(7).le16(0).le16(i); return std::move(out).finish();
    }
    unsigned slot = unsigned(cache_.size());
    if (slot == capacity_) slot = unsigned(std::min_element(cache_.begin(), cache_.end(),
        [](const Entry& a, const Entry& b) { return a.used < b.used; }) - cache_.begin());
    auto result = pointer_image(shape, std::uint16_t(slot), alpha_);
    if (slot == cache_.size()) cache_.push_back({std::move(shape), clock_});
    else cache_[slot] = {std::move(shape), clock_};
    selected_ = slot; hidden_ = false; return result;
}
} // namespace lrdp
