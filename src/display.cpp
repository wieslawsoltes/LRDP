#include "lrdp/display.hpp"
#include <algorithm>
#include <bit>
#include <limits>

namespace lrdp {
Layout validate_layout(std::vector<Monitor> monitors, DisplayLimits limits) {
    require(limits.max_monitors > 0 && limits.max_monitors <= 16 && limits.area_a && limits.area_b, "invalid display limits");
    require(!monitors.empty() && monitors.size() <= limits.max_monitors, "monitor count exceeds capability");
    unsigned primary = 0;
    std::int64_t left = 0, top = 0, right = 0, bottom = 0;
    std::uint64_t area = 0;
    for (auto& m : monitors) {
        require((m.flags & ~1U) == 0, "unknown display flags");
        require(m.width >= 200 && m.width <= 8192 && !(m.width & 1) && m.height >= 200 && m.height <= 8192, "invalid monitor dimensions");
        if (m.flags & 1) { ++primary; require(m.left == 0 && m.top == 0, "primary monitor must start at the origin"); }
        require(m.orientation == 0 || m.orientation == 90 || m.orientation == 180 || m.orientation == 270, "invalid monitor orientation");
        // The specification says receivers ignore out-of-range physical sizes and scales.
        if (m.physical_width < 10 || m.physical_width > 10000) m.physical_width = 0;
        if (m.physical_height < 10 || m.physical_height > 10000) m.physical_height = 0;
        if (m.desktop_scale < 100 || m.desktop_scale > 500) m.desktop_scale = 100;
        if (m.device_scale != 100 && m.device_scale != 140 && m.device_scale != 180) m.device_scale = 100;
        left = std::min(left, std::int64_t(m.left)); top = std::min(top, std::int64_t(m.top));
        right = std::max(right, std::int64_t(m.left) + m.width); bottom = std::max(bottom, std::int64_t(m.top) + m.height);
        area += std::uint64_t(m.width) * m.height;
    }
    require(primary == 1, "exactly one primary monitor required");
    for (std::size_t i = 0; i < monitors.size(); ++i) for (std::size_t j = i + 1; j < monitors.size(); ++j) {
        const auto& a = monitors[i]; const auto& b = monitors[j];
        const bool overlap = std::int64_t(a.left) < std::int64_t(b.left) + b.width &&
            std::int64_t(b.left) < std::int64_t(a.left) + a.width &&
            std::int64_t(a.top) < std::int64_t(b.top) + b.height && std::int64_t(b.top) < std::int64_t(a.top) + a.height;
        require(!overlap, "overlapping monitors are not supported");
    }
    const auto factor_area = std::uint64_t(limits.area_a) * limits.area_b;
    require(factor_area <= std::numeric_limits<std::uint64_t>::max() / limits.max_monitors, "display area capability overflow");
    const auto maximum_area = factor_area * limits.max_monitors;
    require(area <= maximum_area && std::uint64_t(right - left) * std::uint64_t(bottom - top) <= maximum_area,
            "display allocation exceeds advertised area");
    require(right - left <= limits.max_desktop_dimension && bottom - top <= limits.max_desktop_dimension, "desktop extent exceeds limit");
    return {std::move(monitors), std::int32_t(left), std::int32_t(top), std::uint32_t(right - left), std::uint32_t(bottom - top)};
}
Layout decode_layout(View pdu, DisplayLimits limits) {
    Reader in(pdu); require(in.le32() == 2, "expected display monitor layout");
    require(in.le32() == pdu.size(), "display PDU length mismatch");
    require(in.le32() == 40, "unsupported monitor layout element size");
    const auto count = in.le32(); require(count > 0 && count <= limits.max_monitors && count <= 16, "invalid monitor count");
    require(in.remaining() == std::size_t(count) * 40, "monitor layout length mismatch");
    std::vector<Monitor> monitors; monitors.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Monitor m; m.flags = in.le32(); m.left = in.i32(); m.top = in.i32(); m.width = in.le32(); m.height = in.le32();
        m.physical_width = in.le32(); m.physical_height = in.le32(); m.orientation = in.le32();
        m.desktop_scale = in.le32(); m.device_scale = in.le32(); monitors.push_back(m);
    }
    return validate_layout(std::move(monitors), limits);
}
Bytes encode_layout(const Layout& layout) {
    require(layout.monitors.size() <= 16, "invalid layout count");
    Writer out; out.le32(2).le32(std::uint32_t(16 + layout.monitors.size() * 40)).le32(40).le32(std::uint32_t(layout.monitors.size()));
    for (const auto& m : layout.monitors) {
        out.le32(m.flags).le32(std::bit_cast<std::uint32_t>(m.left)).le32(std::bit_cast<std::uint32_t>(m.top))
           .le32(m.width).le32(m.height).le32(m.physical_width).le32(m.physical_height)
           .le32(m.orientation).le32(m.desktop_scale).le32(m.device_scale);
    }
    return std::move(out).finish();
}
Bytes display_caps(DisplayLimits limits) {
    Writer out; out.le32(5).le32(20).le32(limits.max_monitors).le32(limits.area_a).le32(limits.area_b);
    return std::move(out).finish();
}
DisplayController::DisplayController(Layout initial, DisplayLimits limits)
    : limits_(limits), current_(validate_layout(std::move(initial.monitors), limits)) {}
void DisplayController::request(View pdu) { pending_ = decode_layout(pdu, limits_); }
bool DisplayController::commit(const std::function<bool(const Layout&)>& apply) {
    if (!pending_) return false;
    auto next = std::move(*pending_); pending_.reset();
    if (next.monitors == current_.monitors || !apply(next)) return false;
    current_ = std::move(next); ++generation_; return true;
}
} // namespace lrdp
