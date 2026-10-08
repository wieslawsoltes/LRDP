#include "lrdp/desktop.hpp"
#include "lrdp/mcs.hpp"
#include <algorithm>
#include <cmath>

namespace lrdp {
void Frame::validate() const {
    require(width > 0 && height > 0 && std::uint64_t(width) * height <= 16 * 1024 * 1024, "frame dimensions exceed quota");
    require(bgra.size() == std::size_t(width) * height * 4, "invalid BGRA frame stride or size");
}
namespace {
class DemoDesktop final : public Desktop {
    Layout layout_ = validate_layout({Monitor{}});
    std::uint16_t x_ = 160, y_ = 160, last_key_ = 0;
    unsigned presses_ = 0;
    std::optional<std::string> clipboard_ = "LRDP native protocol laboratory — clipboard works in both directions.";
public:
    Layout layout() const override { return layout_; }
    bool resizable() const override { return true; }
    bool unicode_input() const override { return true; }
    bool resize(const Layout& value) override {
        // Validate allocation before publishing the new layout.
        if (std::uint64_t(value.width) * value.height > 16 * 1024 * 1024) return false;
        auto next = value; layout_ = std::move(next); return true;
    }
    Frame capture() override {
        Frame out{layout_.width, layout_.height, Bytes(std::size_t(layout_.width) * layout_.height * 4)};
        for (std::uint32_t y = 0; y < out.height; ++y) for (std::uint32_t x = 0; x < out.width; ++x) {
            auto* p = out.bgra.data() + (std::size_t(y) * out.width + x) * 4;
            const bool grid = x % 64 == 0 || y % 64 == 0;
            p[0] = std::uint8_t(grid ? 68 : 32 + x * 72 / out.width);
            p[1] = std::uint8_t(grid ? 68 : 28 + y * 60 / out.height); p[2] = 22;
            if (y < 48) { p[0] = 100; p[1] = 60; p[2] = 30; }
            if ((std::abs(int(x) - x_) < 2 && std::abs(int(y) - y_) < 15) ||
                (std::abs(int(y) - y_) < 2 && std::abs(int(x) - x_) < 15)) { p[0] = 255; p[1] = 240; p[2] = 60; }
            if (y > 70 && y < 110 && x < 24 * (1 + presses_ % 24)) { p[0] = 90; p[1] = 200; p[2] = 50; }
            if (y > 125 && y < 150 && x / 20 < 16 && ((last_key_ >> (x / 20)) & 1)) { p[0] = 220; p[1] = 180; p[2] = 30; }
            p[3] = 255;
        }
        return out;
    }
    void input(const InputEvent& event) override {
        if (event.kind == InputKind::pointer || event.kind == InputKind::pointer_extended) {
            x_ = event.x; y_ = event.y;
            if (event.flags & 0x8000) ++presses_;
        } else if ((event.kind == InputKind::scancode || event.kind == InputKind::unicode) && !(event.flags & 0x8000)) {
            last_key_ = event.code; ++presses_;
            if (event.kind == InputKind::scancode && event.code == 0x43) clipboard_ = "LRDP server clipboard sample " + std::to_string(presses_); // F9
        }
    }
    void set_clipboard(std::string text) override { (void)text; } // Consume without generating an echo offer.
    std::optional<std::string> poll_clipboard() override { auto value = std::move(clipboard_); clipboard_.reset(); return value; }
    void release_input() override {}
};
}
std::unique_ptr<Desktop> make_demo_desktop() { return std::make_unique<DemoDesktop>(); }
std::vector<Bytes> BitmapEncoder::encode(const Frame& frame, std::uint16_t depth) {
    frame.validate(); require(depth == 15 || depth == 16 || depth == 24, "unsupported bitmap depth");
    std::vector<Bytes> packets;
    const bool full = frame.width != previous_.width || frame.height != previous_.height;
    const unsigned pixel_bytes = depth == 24 ? 3 : 2;
    for (unsigned top = 0; top < frame.height; top += 64) for (unsigned left = 0; left < frame.width; left += 128) {
        const auto width = std::min(128U, frame.width - left), height = std::min(64U, frame.height - top);
        bool dirty = full;
        for (unsigned y = 0; !dirty && y < height; ++y) {
            const auto offset = (std::size_t(top + y) * frame.width + left) * 4;
            dirty = !std::equal(frame.bgra.begin() + std::ptrdiff_t(offset), frame.bgra.begin() + std::ptrdiff_t(offset + width * 4),
                                previous_.bgra.begin() + std::ptrdiff_t(offset));
        }
        if (!dirty) continue;
        const unsigned stride = (width * pixel_bytes + 3) & ~3U;
        Writer update; update.le16(1).le16(1).le16(left).le16(top).le16(left + width - 1).le16(top + height - 1)
            .le16(width).le16(height).le16(depth).le16(0).le16(stride * height);
        for (unsigned row = height; row; --row) {
            for (unsigned x = 0; x < width; ++x) {
                const auto* pixel = frame.bgra.data() + (std::size_t(top + row - 1) * frame.width + left + x) * 4;
                if (depth == 24) update.raw(View(pixel, 3));
                else if (depth == 16) update.le16((unsigned(pixel[2] >> 3) << 11) | (unsigned(pixel[1] >> 2) << 5) | (pixel[0] >> 3));
                else update.le16((unsigned(pixel[2] >> 3) << 10) | (unsigned(pixel[1] >> 3) << 5) | (pixel[0] >> 3));
            }
            update.zeros(stride - width * pixel_bytes);
        }
        packets.push_back(mcs_data(global_channel, share_data(2, update.bytes())));
    }
    previous_ = frame;
    return packets;
}
} // namespace lrdp
