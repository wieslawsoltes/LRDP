#include "lrdp/desktop.hpp"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

using namespace lrdp;
namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
class Application {
public:
    Display* d = XOpenDisplay(nullptr);
    Window window;
    Atom clipboard, utf8, incr, property;
    std::string owned;
    std::optional<std::string> received;
    bool reading_increment = false;
    std::string partial;
    Window target = 0;
    Atom target_property = 0;
    std::size_t offset = 0;
    int key_down = 0, key_up = 0, button_down = 0, button_up = 0, motion = 0;
    Application() {
        check(d != nullptr, "test X display unavailable");
        const auto root = DefaultRootWindow(d);
        window = XCreateSimpleWindow(d, root, 0, 0, 640, 480, 0, 0, 0x336699);
        XSelectInput(d, window, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | PropertyChangeMask);
        XMapRaised(d, window); XSetInputFocus(d, window, RevertToParent, CurrentTime);
        clipboard = XInternAtom(d, "CLIPBOARD", False); utf8 = XInternAtom(d, "UTF8_STRING", False);
        incr = XInternAtom(d, "INCR", False); property = XInternAtom(d, "_TEST_CLIP", False);
        XSync(d, False);
    }
    ~Application() { XDestroyWindow(d, window); XCloseDisplay(d); }
    void own(std::string value) { owned = std::move(value); XSetSelectionOwner(d, clipboard, window, CurrentTime); XFlush(d); }
    void request() {
        received.reset(); partial.clear(); reading_increment = false;
        XConvertSelection(d, clipboard, utf8, property, window, CurrentTime); XFlush(d);
    }
    void read(bool first) {
        Atom type; int format; unsigned long n, after; unsigned char* raw = nullptr;
        XGetWindowProperty(d, window, property, 0, 1024*1024, True, AnyPropertyType, &type, &format, &n, &after, &raw);
        if (first && type == incr) reading_increment = true;
        else {
            check(type == utf8 && format == 8 && after == 0, "invalid native clipboard data");
            if (n) partial.append(reinterpret_cast<const char*>(raw), n);
            if (!reading_increment || !n) received = partial;
        }
        XFree(raw); XFlush(d);
    }
    void pump() {
        for (unsigned i = 0; i < 1024 && XPending(d); ++i) {
            XEvent e{}; XNextEvent(d, &e);
            if (e.type == KeyPress) ++key_down; else if (e.type == KeyRelease) ++key_up;
            else if (e.type == ButtonPress) ++button_down; else if (e.type == ButtonRelease) ++button_up;
            else if (e.type == MotionNotify) ++motion;
            else if (e.type == SelectionNotify && e.xselection.requestor == window) {
                check(e.xselection.property == property, "X11 conversion rejected"); read(true);
            } else if (e.type == SelectionRequest) {
                const auto& request = e.xselectionrequest;
                XEvent reply{}; reply.xselection.type = SelectionNotify; reply.xselection.display = d;
                reply.xselection.requestor = request.requestor; reply.xselection.selection = request.selection;
                reply.xselection.target = request.target; reply.xselection.time = request.time;
                reply.xselection.property = request.property;
                if (request.target != utf8) reply.xselection.property = None;
                else if (owned.size() <= 32768) XChangeProperty(d, request.requestor, request.property, utf8, 8, PropModeReplace,
                                                               reinterpret_cast<const unsigned char*>(owned.data()), int(owned.size()));
                else {
                    target = request.requestor; target_property = request.property; offset = 0;
                    const unsigned long size = owned.size(); XSelectInput(d, target, PropertyChangeMask);
                    XChangeProperty(d, target, target_property, incr, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&size), 1);
                }
                XSendEvent(d, request.requestor, False, NoEventMask, &reply); XFlush(d);
            } else if (e.type == PropertyNotify) {
                if (e.xproperty.window == window && e.xproperty.atom == property && e.xproperty.state == PropertyNewValue && reading_increment) read(false);
                else if (target && e.xproperty.window == target && e.xproperty.atom == target_property && e.xproperty.state == PropertyDelete) {
                    const auto n = std::min<std::size_t>(8192, owned.size() - offset);
                    XChangeProperty(d, target, target_property, utf8, 8, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(owned.data() + offset), int(n));
                    offset += n; if (!n) target = 0; XFlush(d);
                }
            }
        }
    }
};
}
int main() {
    try {
        Application app; auto desktop = make_x11_desktop("");
        const auto frame = desktop->capture();
        check(frame.width == 640 && frame.height == 480 && frame.bgra[0] == 0x99 && frame.bgra[1] == 0x66 && frame.bgra[2] == 0x33, "X11 pixel capture mismatch");
        check(!desktop->resizable(), "physical X11 display must not advertise virtual resizing");
        std::optional<std::string> native;
        auto until = [&](const std::function<bool()>& predicate) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!predicate()) {
                if (auto value = desktop->poll_clipboard()) native = std::move(*value);
                app.pump();
                check(std::chrono::steady_clock::now() < deadline, "X11 test condition timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        app.own("native → RDP 🚀"); until([&]{ return native.has_value(); });
        check(*native == "native → RDP 🚀", "native clipboard conversion failed");
        native.reset(); app.own("changed owner value"); until([&]{ return native.has_value(); });
        check(*native == "changed owner value", "XFixes missed repeated owner update");
        std::string large(180000, 'a'); large += "日本語";
        native.reset(); app.own(large); until([&]{ return native.has_value(); });
        check(*native == large, "native INCR receive corrupted data");
        desktop->set_clipboard("RDP → native 🚀");
        until([&]{ const auto owner = XGetSelectionOwner(app.d, app.clipboard); return owner != None && owner != app.window; });
        app.request(); until([&]{ return app.received.has_value(); });
        check(*app.received == "RDP → native 🚀", "RDP clipboard ownership failed");
        desktop->set_clipboard(large); for (int i = 0; i < 8; ++i) { (void)desktop->poll_clipboard(); app.pump(); }
        app.request(); until([&]{ return app.received.has_value(); });
        check(*app.received == large, "native INCR send corrupted data");
        desktop->input({InputKind::pointer, 0x8000 | 0x1000, 0, 200, 150});
        desktop->input({InputKind::scancode, 0, 0x1e});
        until([&]{ return app.key_down > 0 && app.button_down > 0 && app.motion > 0; });
        desktop->release_input(); until([&]{ return app.key_up > 0 && app.button_up > 0; });
        std::cout << "PASS: X11 BGRA capture, native keyboard/pointer, input release, Unicode clipboard ownership, repeated owner changes, bidirectional 180 KiB INCR transfers\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
