#pragma once
#include "wire.hpp"
#include <functional>
#include <optional>

namespace lrdp {
struct Monitor {
    std::uint32_t flags = 1;
    std::int32_t left = 0, top = 0;
    std::uint32_t width = 1280, height = 720;
    std::uint32_t physical_width = 0, physical_height = 0;
    std::uint32_t orientation = 0, desktop_scale = 100, device_scale = 100;
    bool operator==(const Monitor&) const = default;
};
struct DisplayLimits {
    std::uint32_t max_monitors = 16;
    std::uint32_t area_a = 1024, area_b = 1024;
    std::uint32_t max_desktop_dimension = 16384;
};
struct Layout {
    std::vector<Monitor> monitors;
    std::int32_t left = 0, top = 0;
    std::uint32_t width = 0, height = 0;
};
Layout validate_layout(std::vector<Monitor> monitors, DisplayLimits limits = {});
Layout decode_layout(View pdu, DisplayLimits limits = {});
Bytes encode_layout(const Layout& layout);
Bytes display_caps(DisplayLimits limits = {});

// A backend must commit atomically or leave the previous configuration intact.
// Protocol receipt alone never changes the visible/output configuration.
class DisplayController {
    DisplayLimits limits_;
    Layout current_;
    std::optional<Layout> pending_;
    std::uint64_t generation_ = 0;
public:
    explicit DisplayController(Layout initial, DisplayLimits limits = {});
    void request(View pdu);
    bool commit(const std::function<bool(const Layout&)>& apply);
    [[nodiscard]] const Layout& current() const { return current_; }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }
};
} // namespace lrdp
