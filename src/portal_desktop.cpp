#include "lrdp/platform/portal_desktop.hpp"
#include "lrdp/platform/pipewire_capture.hpp"
#include "lrdp/platform/capture_frame.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <set>

namespace lrdp {
namespace {
class PortalDesktop final : public Desktop {
    PortalSession portal_;
    PipeWireCapture capture_{portal_.open_pipewire(), portal_.stream()};
    std::shared_ptr<const Frame> frame_;
    Layout layout_;
    std::uint64_t sequence_ = 0;
    std::set<unsigned> keys_, buttons_;
    std::set<std::uint32_t> symbols_;
    std::optional<std::uint16_t> high_down_, high_up_;
    int vertical_ = 0, horizontal_ = 0;
    void refresh() {
        portal_.poll(); const auto newest = capture_.latest();
        if (!newest.frame || newest.sequence == sequence_) return;
        Monitor monitor; monitor.width = (newest.frame->width + 1U) & ~1U; monitor.height = newest.frame->height;
        auto layout = validate_layout({monitor}); frame_ = newest.frame; layout_ = std::move(layout); sequence_ = newest.sequence;
    }
    void unicode(std::uint16_t unit, bool down) {
        auto& high = down ? high_down_ : high_up_;
        if (unit >= 0xd800 && unit <= 0xdbff) { high = unit; return; }
        std::uint32_t cp = unit;
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            require(high.has_value(), "Unicode input contains an orphan low surrogate");
            cp = 0x10000 + ((std::uint32_t(*high) - 0xd800) << 10) + unit - 0xdc00;
        } else require(!high, "Unicode input contains an unfinished surrogate pair");
        high.reset();
        const auto symbol = cp <= 0xff ? cp : 0x01000000U | cp;
        if (down) symbols_.insert(symbol); else if (!symbols_.erase(symbol)) return;
        portal_.keysym(symbol, down);
    }
public:
    PortalDesktop() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!frame_) {
            refresh(); require(std::chrono::steady_clock::now() < deadline, "portal stream did not produce a first frame");
            if (!frame_) ::poll(nullptr, 0, 5);
        }
    }
    ~PortalDesktop() override { try { release_input(); portal_.flush(); } catch (...) {} }
    void pump() override { refresh(); }
    Layout layout() const override { return layout_; }
    bool resizable() const override { return false; }
    bool unicode_input() const override { return true; }
    bool clipboard_available() const override { return portal_.clipboard_available(); }
    bool embedded_cursor() const override { return portal_.stream().embedded_cursor; }
    bool resize(const Layout&) override { return false; }
    Frame capture() override {
        // Keep geometry fixed from pump() until the session has inspected layout().
        require(frame_ != nullptr, "portal has no captured frame");
        if (frame_->width == layout_.width) return *frame_;
        Frame padded{layout_.width, frame_->height, Bytes(std::size_t(layout_.width) * frame_->height * 4)};
        for (unsigned y = 0; y < frame_->height; ++y) {
            const auto* source = frame_->bgra.data() + std::size_t(y) * frame_->width * 4;
            auto* target = padded.bgra.data() + std::size_t(y) * padded.width * 4;
            std::memcpy(target, source, std::size_t(frame_->width) * 4);
            std::memcpy(target + frame_->width * 4, source + (frame_->width - 1) * 4, 4);
        }
        return padded;
    }
    void input(const InputEvent& event) override {
        const bool down = !(event.flags & 0x8000);
        if (event.kind == InputKind::scancode) {
            const auto key = rdp_evdev_key(event.code, event.flags); if (!key) return;
            if (down) keys_.insert(key); else if (!keys_.erase(key)) return;
            portal_.key(key, down);
        } else if (event.kind == InputKind::unicode) unicode(event.code, down);
        else if (event.kind == InputKind::synchronize) release_input();
        else {
            const auto& stream = portal_.stream();
            const auto logical_width = stream.logical_width ? stream.logical_width : frame_->width;
            const auto logical_height = stream.logical_height ? stream.logical_height : frame_->height;
            const auto px = std::min<unsigned>(event.x, frame_->width - 1);
            const auto py = std::min<unsigned>(event.y, frame_->height - 1);
            portal_.pointer(double(px) * logical_width / frame_->width, double(py) * logical_height / frame_->height);
            auto button = [&](unsigned value) {
                const bool pressed = (event.flags & 0x8000) != 0;
                if (pressed) buttons_.insert(value); else if (!buttons_.erase(value)) return;
                portal_.button(value, pressed);
            };
            if (event.kind == InputKind::pointer_extended) {
                if (event.flags & 1) button(275);
                if (event.flags & 2) button(276);
            } else if (event.flags & (0x200 | 0x400)) {
                int delta = event.flags & 0x1ff; if (delta & 0x100) delta -= 512;
                const bool horizontal = (event.flags & 0x400) != 0;
                auto& accumulated = horizontal ? horizontal_ : vertical_; accumulated += delta;
                const int steps = accumulated / 120; accumulated -= steps * 120;
                if (steps) portal_.wheel(horizontal, horizontal ? steps : -steps);
            } else {
                if (event.flags & 0x1000) button(272);
                if (event.flags & 0x2000) button(273);
                if (event.flags & 0x4000) button(274);
            }
        }
    }
    bool enable_rich_clipboard() override { return portal_.enable_rich_clipboard(); }
    bool enable_file_clipboard() override { return portal_.enable_file_clipboard(); }
    void set_clipboard_rich(RichClipboard content) override { portal_.set_clipboard_rich(std::move(content)); }
    std::optional<RichClipboard> poll_clipboard_rich() override { return portal_.take_clipboard_rich(); }
    void set_clipboard_files(std::vector<std::string> paths) override { portal_.set_clipboard_files(std::move(paths)); }
    std::optional<std::vector<std::string>> poll_clipboard_files() override { return portal_.take_clipboard_files(); }
    void set_clipboard(std::string text) override { portal_.set_clipboard(std::move(text)); }
    std::optional<std::string> poll_clipboard() override { return portal_.take_clipboard(); }
    void release_input() override {
        for (auto key : keys_) portal_.key(key, false);
        for (auto symbol : symbols_) portal_.keysym(symbol, false);
        for (auto button : buttons_) portal_.button(button, false);
        keys_.clear(); symbols_.clear(); buttons_.clear(); high_down_.reset(); high_up_.reset(); vertical_ = horizontal_ = 0;
    }
};
}
std::unique_ptr<Desktop> make_portal_desktop() { return std::make_unique<PortalDesktop>(); }
} // namespace lrdp
