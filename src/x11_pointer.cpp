#include "lrdp/platform/x11_pointer.hpp"
#include "lrdp/platform/shared_library.hpp"
#include <X11/Xlib.h>

namespace lrdp {
namespace {
// Public XFixesCursorImage ABI. Pixel elements are unsigned long on both ILP32
// and LP64, but only their low 32 bits contain the premultiplied ARGB value.
struct CursorImage {
    short x, y;
    unsigned short width, height, xhot, yhot;
    unsigned long serial;
    unsigned long* pixels;
    Atom atom;
    const char* name;
};
}
struct X11Pointer::Impl {
    SharedLibrary library{"libXfixes.so.3"};
    Display* display;
    int event_base = 0;
    bool dirty = true;
    using Get = CursorImage* (*)(Display*);
    Get get = library.symbol<Get>("XFixesGetCursorImage");
    std::shared_ptr<const PointerShape> shape;
    explicit Impl(Display* value) : display(value) {
        require(value != nullptr, "cursor capture needs an X11 display");
        int error = 0;
        auto query = library.symbol<Bool (*)(Display*, int*, int*)>("XFixesQueryExtension");
        require(query(display, &event_base, &error), "XFixes cursor extension is unavailable");
        auto select = library.symbol<void (*)(Display*, Window, unsigned long)>("XFixesSelectCursorInput");
        select(display, DefaultRootWindow(display), 1); XFlush(display);
    }
};
X11Pointer::X11Pointer(Display* display) : impl_(std::make_unique<Impl>(display)) {}
X11Pointer::~X11Pointer() = default;
void X11Pointer::event(int type) { if (type == impl_->event_base + 1) impl_->dirty = true; }
std::shared_ptr<const PointerShape> X11Pointer::current() {
    auto& self = *impl_;
    if (!self.dirty) return self.shape;
    std::unique_ptr<CursorImage, decltype(&XFree)> image(self.get(self.display), XFree);
    require(image != nullptr, "cannot capture X11 cursor");
    require(image->width <= 384 && image->height <= 384 && image->pixels, "X11 cursor exceeds allocation policy");
    PointerShape next{image->width, image->height, image->xhot, image->yhot, {}};
    next.bgra.reserve(std::size_t(next.width) * next.height * 4);
    for (std::size_t i = 0; i < std::size_t(next.width) * next.height; ++i) {
        const auto argb = std::uint32_t(image->pixels[i]);
        for (unsigned shift = 0; shift < 32; shift += 8) next.bgra.push_back(std::uint8_t(argb >> shift));
    }
    next.validate(); self.shape = std::make_shared<const PointerShape>(std::move(next)); self.dirty = false;
    return self.shape;
}
}
