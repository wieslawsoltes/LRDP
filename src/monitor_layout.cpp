#include "lrdp/monitor_layout.hpp"
#include "lrdp/mcs.hpp"
#include <bit>
namespace lrdp {
Layout decode_initial_monitors(View definitions, View attributes) {
    Reader in(definitions); require(in.le32() == 0,"invalid client monitor flags");
    const auto count = in.le32();
    require(count > 0 && count <= 16 && in.remaining() == std::size_t(count)*20,"invalid client monitor definitions");
    std::vector<Monitor> monitors; monitors.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        Monitor m; m.left = in.i32(); m.top = in.i32(); const auto right = in.i32(), bottom = in.i32(); m.flags = in.le32();
        const auto width = std::int64_t(right)-m.left+1, height = std::int64_t(bottom)-m.top+1;
        require(width >= 200 && width <= 8192 && height >= 200 && height <= 8192,"initial monitor extent exceeds policy");
        m.width = std::uint32_t(width); m.height = std::uint32_t(height); monitors.push_back(m);
    }
    if (!attributes.empty()) {
        Reader ex(attributes);
        require(ex.le32() == 0 && ex.le32() == 20 && ex.le32() == count && ex.remaining() == std::size_t(count)*20,
                "client monitor attributes do not match definitions");
        for (auto& m : monitors) {
            m.physical_width = ex.le32(); m.physical_height = ex.le32(); m.orientation = ex.le32();
            m.desktop_scale = ex.le32(); m.device_scale = ex.le32();
            // These optional values MUST be ignored when invalid, not rejected.
            if (m.physical_width < 10 || m.physical_width > 10000 || m.physical_height < 10 || m.physical_height > 10000)
                m.physical_width = m.physical_height = 0;
            if (m.orientation != 0 && m.orientation != 90 && m.orientation != 180 && m.orientation != 270) m.orientation = 0;
            if (m.desktop_scale < 100 || m.desktop_scale > 500 || (m.device_scale != 100 && m.device_scale != 140 && m.device_scale != 180))
                m.desktop_scale = m.device_scale = 100;
        }
    }
    return validate_layout(std::move(monitors));
}
Bytes monitor_layout_pdu(const Layout& supplied) {
    const auto layout = validate_layout(supplied.monitors);
    Writer out; out.le32(unsigned(layout.monitors.size()));
    for (const auto& m : layout.monitors)
        out.le32(std::bit_cast<std::uint32_t>(m.left)).le32(std::bit_cast<std::uint32_t>(m.top))
            .le32(std::uint32_t(std::int64_t(m.left)+m.width-1)).le32(std::uint32_t(std::int64_t(m.top)+m.height-1)).le32(m.flags);
    return share_data(55,out.bytes(),0);
}
}
