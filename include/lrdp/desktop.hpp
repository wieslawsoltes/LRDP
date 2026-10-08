#pragma once
#include "display.hpp"
#include "pointer.hpp"
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
    // Always called, even with output suppressed or no clipboard channel joined.
    virtual void pump() {}
    virtual Layout layout() const = 0;
    virtual bool resizable() const = 0;
    virtual bool unicode_input() const { return false; }
    virtual bool clipboard_available() const { return true; }
    virtual bool embedded_cursor() const { return false; }
    virtual bool enable_file_clipboard() { return false; }
    virtual std::optional<std::vector<std::string>> poll_clipboard_files() { return {}; }
    virtual void set_clipboard_files(std::vector<std::string>) { throw ProtocolError("backend has no file clipboard"); }
    virtual std::shared_ptr<const PointerShape> pointer_shape() { return {}; }
    virtual bool resize(const Layout&) = 0;
    virtual Frame capture() = 0;
    virtual void input(const InputEvent&) = 0;
    virtual void set_clipboard(std::string text) = 0;
    virtual std::optional<std::string> poll_clipboard() = 0;
    virtual void release_input() = 0;
};
std::unique_ptr<Desktop> make_demo_desktop();
std::unique_ptr<Desktop> make_x11_desktop(const std::string& display);
class BitmapEncoder {
    Frame previous_;
public:
    void invalidate() { previous_ = {}; }
    std::vector<Bytes> encode(const Frame&, std::uint16_t depth);
};
} // namespace lrdp
