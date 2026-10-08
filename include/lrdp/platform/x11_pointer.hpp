#pragma once
#include "lrdp/pointer.hpp"
#include <memory>
struct _XDisplay;
namespace lrdp {
// Calls only the public XFixes client API. The X Display must outlive this object.
class X11Pointer {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit X11Pointer(_XDisplay* display);
    ~X11Pointer();
    X11Pointer(const X11Pointer&) = delete;
    X11Pointer& operator=(const X11Pointer&) = delete;
    void event(int type);
    std::shared_ptr<const PointerShape> current();
};
}
