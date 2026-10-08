#include "lrdp/platform/headless_server.hpp"
#include "lrdp/platform/randr_outputs.hpp"
#include "lrdp/platform/x11_managed.hpp"
#include <X11/Xlib.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
using namespace lrdp;
int main() {
    try {
        HeadlessOptions options; HeadlessServer server(options);
        require(setenv("XAUTHORITY",server.authority().c_str(),1) == 0,"test authority setup");
        auto outputs = std::make_shared<RandrOutputs>();
        auto desktop = make_managed_x11_desktop(server.display(),[&](auto* d,const Layout& l) { return outputs->apply(d,l); });
        std::unique_ptr<Display,decltype(&XCloseDisplay)> app(XOpenDisplay(server.display().c_str()),XCloseDisplay);
        require(app != nullptr,"native application could not authenticate");
        const auto root = DefaultRootWindow(app.get());
        const auto window = XCreateSimpleWindow(app.get(),root,10,10,180,120,0,0,0x336699);
        XMapWindow(app.get(),window); XSync(app.get(),False);
        Monitor monitor; monitor.width = 800; monitor.height = 600;
        require(desktop->resize(validate_layout({monitor})),"native 800x600 resize rejected");
        auto frame = desktop->capture(); require(frame.width == 800 && frame.height == 600,"native framebuffer was not resized");
        const auto pixel = (std::size_t(20)*800+20)*4;
        require(frame.bgra[pixel] == 0x99 && frame.bgra[pixel+1] == 0x66 && frame.bgra[pixel+2] == 0x33,"application lost after resize");
        auto left = monitor; left.flags = 0; left.left = -800;
        require(desktop->resize(validate_layout({monitor,left})),"native dual-monitor resize rejected");
        frame = desktop->capture(); require(frame.width == 1600 && frame.height == 600 && desktop->layout().left == -800,"negative-origin desktop translation");
        XWindowAttributes attributes{}; require(XGetWindowAttributes(app.get(),window,&attributes) && attributes.map_state == IsViewable,"application restarted during topology change");
        // Repeated mode churn must retire old custom modes and preserve the X connection.
        for (unsigned i = 0; i < 20; ++i) {
            monitor.width = 640+(i%4)*160; monitor.height = 480+(i%3)*120;
            require(desktop->resize(validate_layout({monitor})),"repeated native resize rejected");
            frame = desktop->capture(); require(frame.width == monitor.width && frame.height == monitor.height,"stale capture geometry");
        }
        const auto previous = desktop->layout(); auto invalid = monitor; invalid.width = 201;
        try { (void)desktop->resize(validate_layout({invalid})); throw std::runtime_error("invalid dimensions accepted"); }
        catch (const ProtocolError&) {}
        require(desktop->layout().monitors == previous.monitors,"rejected layout altered committed desktop");
        // Authentication is enforced by Xorg itself, not only by LRDP's listener.
        unsetenv("XAUTHORITY");
        Display* rejected = XOpenDisplay(server.display().c_str());
        if (rejected) { XCloseDisplay(rejected); throw std::runtime_error("private Xorg allowed an unauthenticated client"); }
        std::cout << "PASS: private authenticated Xorg; actual framebuffer resize; dual monitors with negative origin; application survival; mode churn; rejected layouts\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
