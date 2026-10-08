#pragma once
#include "lrdp/desktop.hpp"
#include <functional>
struct _XDisplay;
namespace lrdp {
// Only a backend owning its display may supply a mode-setting callback.
using X11Resize = std::function<bool(_XDisplay*, const Layout&)>;
std::unique_ptr<Desktop> make_managed_x11_desktop(const std::string& display, X11Resize resize);
}
