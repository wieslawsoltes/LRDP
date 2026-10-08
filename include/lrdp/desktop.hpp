#pragma once
#include "display.hpp"
#include <memory>

namespace lrdp {
struct Frame {
    std::uint32_t width = 0, height = 0;
    Bytes bgra;
    void validate() const;
};
enum class InputKind { scancode, unicode, pointer, pointer_extended, synchronize };
struct InputEvent { InputKind kind; std::uint16_t flags = 0, code = 0, x = 0, y = 0; };
class Desktop {
public:
    virtual ~Desktop() = default;
    virtual Layout layout() const = 0;
    virtual bool resizable() const = 0;
    virtual bool unicode_input() const { return false; }
    virtual bool resize(const Layout&) = 0;
    virtual Frame capture() = 0;
    virtual void input(const InputEvent&) = 0;
    virtual void set_clipboard(std::string text) = 0;
    virtual std::optional<std::string> poll_clipboard() = 0;
    virtual void release_input() = 0;
};
// The deterministic desktop is a visible protocol diagnostic, not a Linux login session.
std::unique_ptr<Desktop> make_demo_desktop();
std::unique_ptr<Desktop> make_x11_desktop(const std::string& display);

// Byte comparisons, not probabilistic hashes: every changed pixel is eventually delivered.
// The transport calls this only when its previous frame has drained, bounding latency/memory.
class BitmapEncoder {
    Frame previous_;
public:
    void invalidate() { previous_ = {}; }
    std::vector<Bytes> encode(const Frame&, std::uint16_t depth);
};
} // namespace lrdp
