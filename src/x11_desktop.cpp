#include "lrdp/desktop.hpp"
#include <deque>
#include "lrdp/clipboard/file_uri.hpp"
#include "lrdp/platform/x11_managed.hpp"
#include "lrdp/platform/x11_pointer.hpp"
#include "lrdp/platform/shared_library.hpp"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <map>
#include <set>

namespace lrdp {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t clipboard_limit = 1024 * 1024, chunk_size = 32768;

// Public libXfixes event ABI. Kept local so installations need only Xlib development
// headers; the actual library and extension are resolved and checked at runtime.
struct SelectionChange {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    int subtype;
    Window owner;
    Atom selection;
    Time timestamp, selection_timestamp;
};
struct DisplayCloser { void operator()(Display* value) const { if (value) XCloseDisplay(value); } };
struct ImageCloser { void operator()(XImage* value) const { if (value) XDestroyImage(value); } };
struct XFreeDeleter { void operator()(unsigned char* value) const { if (value) XFree(value); } };
using XMemory = std::unique_ptr<unsigned char, XFreeDeleter>;

// Xlib's default handler exits on ordinary asynchronous BadWindow errors. The
// single-session X11 backend traps errors on its own Display only; errors for
// unrelated Displays retain their previous handler. No requestor can exit LRDP.
class XErrorScope {
    inline static Display* own_ = nullptr;
    inline static int error_ = 0;
    inline static XErrorHandler previous_ = nullptr;
    static int handler(Display* display, XErrorEvent* event) {
        if (display == own_) { error_ = event->error_code; return 0; }
        return previous_ ? previous_(display, event) : 0;
    }
public:
    explicit XErrorScope(Display* display) {
        require(own_ == nullptr, "only one X11 backend is supported in a process");
        own_ = display; previous_ = XSetErrorHandler(handler);
    }
    ~XErrorScope() { XSetErrorHandler(previous_); own_ = nullptr; }
    bool sync(Display* display) { XSync(display, False); const bool ok = error_ == 0; error_ = 0; return ok; }
};

unsigned evdev_code(const InputEvent& event) {
    if (event.flags & 0x200) return event.code == 0x1d || event.code == 0x45 ? 119U : 0U; // Pause/E1.
    if (!(event.flags & 0x100)) return event.code > 0 && event.code <= 0x58 ? event.code : 0;
    switch (event.code) {
    case 0x1c: return 96; case 0x1d: return 97; case 0x35: return 98;
    case 0x37: return 99; case 0x38: return 100; case 0x46: return 119;
    case 0x47: return 102; case 0x48: return 103; case 0x49: return 104;
    case 0x4b: return 105; case 0x4d: return 106; case 0x4f: return 107;
    case 0x50: return 108; case 0x51: return 109; case 0x52: return 110;
    case 0x53: return 111; case 0x5b: return 125; case 0x5c: return 126;
    case 0x5d: return 127; default: return 0;
    }
}

class X11Desktop final : public Desktop {
    // Extension libraries own Xlib close-display callbacks and must outlive Display.
    SharedLibrary xtest_{"libXtst.so.6"}, xfixes_{"libXfixes.so.3"};
    std::unique_ptr<Display, DisplayCloser> display_;
    XErrorScope errors_;
    using KeyFunction = int (*)(Display*, unsigned, Bool, unsigned long);
    using MotionFunction = int (*)(Display*, int, int, int, unsigned long);
    KeyFunction key_ = xtest_.symbol<KeyFunction>("XTestFakeKeyEvent");
    KeyFunction button_ = xtest_.symbol<KeyFunction>("XTestFakeButtonEvent");
    MotionFunction motion_ = xtest_.symbol<MotionFunction>("XTestFakeMotionEvent");
    Window root_ = 0, window_ = 0;
    Atom clipboard_ = 0, utf8_ = 0, targets_ = 0, timestamp_ = 0, property_ = 0, incr_ = 0, clock_ = 0;
    Atom uri_ = 0, copied_files_ = 0, html_ = 0, bmp_ = 0, png_ = 0;
    bool rich_enabled_ = false, owns_rich_ = false;
    std::optional<RichClipboard> ready_rich_;
    std::shared_ptr<const std::string> owned_html_, owned_bmp_, owned_png_;
    std::size_t selection_limit() const { return rich_enabled_ ? rich_clipboard_limit : clipboard_limit; }
    bool files_enabled_ = false, owns_files_ = false;
    std::shared_ptr<const std::string> owned_uris_, owned_copied_;
    std::optional<std::vector<std::string>> ready_files_;
    int screen_ = 0, selection_event_ = 0;
    Layout layout_;
    X11Resize resize_;
    std::unique_ptr<X11Pointer> pointer_;
    std::set<unsigned> pressed_keys_, pressed_buttons_;
    int wheel_ = 0, horizontal_wheel_ = 0;
    Time ownership_time_ = 0;
    bool claiming_ = false;
    std::shared_ptr<const std::string> owned_text_ = std::make_shared<const std::string>();
    std::optional<std::string> ready_text_;
    std::uint64_t selection_generation_ = 0;
    struct Incoming {
        Window window = 0, owner = 0;
        Atom target = 0;
        std::deque<Atom> pending;
        RichClipboard rich;
        bool gathering_rich = false;
        std::uint64_t generation = 0;
        bool incremental = false;
        Bytes bytes;
        Clock::time_point deadline;
    };
    std::optional<Incoming> incoming_;
    struct Outgoing {
        Window window;
        Atom property;
        Atom target;
        std::shared_ptr<const std::string> text;
        std::size_t offset = 0;
        Clock::time_point deadline;
    };
    std::map<std::pair<Window, Atom>, Outgoing> outgoing_;

    Display* d() const { return display_.get(); }
    Atom atom(const char* value) { return XInternAtom(d(), value, False); }
    void cancel_incoming() {
        if (incoming_) { XDestroyWindow(d(), incoming_->window); incoming_.reset(); }
    }
    void fetch_selection(Window owner) {
        ++selection_generation_; cancel_incoming(); ready_text_.reset(); ready_files_.reset(); ready_rich_.reset();
        if (owner == window_ || owner == None) return;
        // A fresh requestor window is a bounded correlation token. Late events from
        // an old owner cannot complete a newer clipboard conversion.
        Incoming next;
        next.window = XCreateSimpleWindow(d(), root_, 0, 0, 1, 1, 0, 0, 0);
        next.owner = owner; next.generation = selection_generation_; next.deadline = Clock::now() + std::chrono::seconds(5);
        XSelectInput(d(), next.window, PropertyChangeMask);
        next.target = files_enabled_ || rich_enabled_ ? targets_ : utf8_;
        XConvertSelection(d(), clipboard_, next.target, property_, next.window, CurrentTime);
        incoming_ = std::move(next); XFlush(d());
    }
    void complete_selection() {
        if (incoming_->generation != selection_generation_ || XGetSelectionOwner(d(), clipboard_) != incoming_->owner) { cancel_incoming(); return; }
        auto& transfer = *incoming_;
        try {
            if (transfer.target == uri_ || transfer.target == copied_files_) {
                std::string text(transfer.bytes.begin(),transfer.bytes.end());
                ready_files_ = decode_file_uris(text,transfer.target == copied_files_);
            } else if (transfer.gathering_rich) {
                if (transfer.target == bmp_) transfer.rich.image = decode_clipboard_bmp(transfer.bytes);
#ifdef LRDP_HAVE_PNG
                else if (transfer.target == png_) transfer.rich.image = decode_clipboard_png(transfer.bytes);
#endif
                else {
                    std::string text(transfer.bytes.begin(),transfer.bytes.end());
                    if (!text.empty() && text.back() == 0) text.pop_back();
                    (void)utf16le(text);
                    if (transfer.target == html_) transfer.rich.html = std::move(text);
                    else transfer.rich.text = std::move(text);
                }
                transfer.rich.validate();
                if (!transfer.pending.empty()) {
                    const auto next=transfer.pending.front(); transfer.pending.pop_front(); convert_target(next); return;
                }
                ready_rich_ = std::move(transfer.rich);
            } else {
                std::string text(transfer.bytes.begin(),transfer.bytes.end());
                require(utf16le(text).size() <= clipboard_limit,"X11 clipboard exceeds text policy"); ready_text_ = std::move(text);
            }
        } catch (const ProtocolError&) { /* Invalid local formats never reach the network. */ }
        cancel_incoming();
    }
    void convert_target(Atom target) {
        incoming_->target = target; incoming_->bytes.clear(); incoming_->incremental = false;
        XConvertSelection(d(), clipboard_, target, property_, incoming_->window, CurrentTime); XFlush(d());
    }
    void read_selection(bool notification) {
        if (!incoming_) return;
        Atom type = None; int format = 0; unsigned long count = 0, after = 0; unsigned char* raw = nullptr;
        const int status = XGetWindowProperty(d(), incoming_->window, property_, 0, long(selection_limit() / 4 + 1), True,
                                              AnyPropertyType, &type, &format, &count, &after, &raw);
        XMemory memory(raw);
        if (status != Success || after != 0) { cancel_incoming(); return; }
        if (incoming_->target == targets_) {
            if (type != XA_ATOM || format != 32 || count > 256 || (!raw && count)) { cancel_incoming(); return; }
            const auto* atoms = reinterpret_cast<const unsigned long*>(raw);
            const auto has = [&](Atom value) { return std::find(atoms, atoms + count, value) != atoms + count; };
            if (files_enabled_ && (has(uri_) || has(copied_files_))) convert_target(has(uri_) ? uri_ : copied_files_);
            else if (rich_enabled_ && (has(html_) || has(bmp_) || (png_ && has(png_)))) {
                auto& transfer=*incoming_; transfer.gathering_rich=true;
                if(has(utf8_)) transfer.pending.push_back(utf8_);
                if(has(html_)) transfer.pending.push_back(html_);
                if(png_ && has(png_)) transfer.pending.push_back(png_); else if(has(bmp_)) transfer.pending.push_back(bmp_);
                const auto next=transfer.pending.front(); transfer.pending.pop_front(); convert_target(next);
            } else if(has(utf8_)) convert_target(utf8_);
            else cancel_incoming();
            return;
        }
        if (notification && type == incr_) {
            if (format != 32 || count != 1 || *reinterpret_cast<unsigned long*>(raw) > selection_limit()) { cancel_incoming(); return; }
            incoming_->incremental = true; XDeleteProperty(d(), incoming_->window, property_); XFlush(d()); return;
        }
        if (type != incoming_->target || format != 8 || count > selection_limit() - incoming_->bytes.size()) { cancel_incoming(); return; }
        if (count) incoming_->bytes.insert(incoming_->bytes.end(), raw, raw + count);
        if (!incoming_->incremental || count == 0) complete_selection();
        XFlush(d());
    }
    std::shared_ptr<const std::string> selection_payload(Atom target) const {
        if (owns_files_) return target == uri_ ? owned_uris_ : target == copied_files_ ? owned_copied_ : nullptr;
        if (owns_rich_) {
            if (target == html_) return owned_html_;
            if (target == bmp_) return owned_bmp_;
            if (png_ && target == png_) return owned_png_;
        }
        return target == utf8_ ? owned_text_ : nullptr;
    }
    void selection_request(const XSelectionRequestEvent& request) {
        XEvent reply{}; reply.xselection.type = SelectionNotify; reply.xselection.display = d();
        reply.xselection.requestor = request.requestor; reply.xselection.selection = request.selection;
        reply.xselection.target = request.target; reply.xselection.time = request.time; reply.xselection.property = None;
        const Atom property = request.property == None ? request.target : request.property;
        bool accepted = false;
        // X timestamps wrap; signed modular differences are valid within half a wrap.
        const bool timely = request.time == CurrentTime || std::bit_cast<std::int32_t>(std::uint32_t(request.time - ownership_time_)) >= 0;
        if (request.selection == clipboard_ && XGetSelectionOwner(d(), clipboard_) == window_ && timely) {
            if (request.target == targets_) {
                std::vector<Atom> values = {targets_, timestamp_};
                if (owns_files_) { values.push_back(uri_); values.push_back(copied_files_); }
                else {
                    if (owned_text_) values.push_back(utf8_);
                    if (owns_rich_ && owned_html_) values.push_back(html_);
                    if (owns_rich_ && owned_bmp_) values.push_back(bmp_);
                    if (owns_rich_ && owned_png_ && png_) values.push_back(png_);
                }
                XChangeProperty(d(), request.requestor, property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(values.data()), int(values.size())); accepted = true;
            } else if (request.target == timestamp_) {
                const unsigned long time = ownership_time_;
                XChangeProperty(d(), request.requestor, property, XA_INTEGER, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(&time), 1); accepted = true;
            } else if (auto payload = selection_payload(request.target)) {
                if (payload->size() <= chunk_size) {
                    XChangeProperty(d(), request.requestor, property, request.target, 8, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(payload->data()), int(payload->size())); accepted = true;
                } else if (outgoing_.size() < 8 && !outgoing_.contains({request.requestor, property})) {
                    const unsigned long length = payload->size();
                    XSelectInput(d(), request.requestor, PropertyChangeMask | StructureNotifyMask);
                    XChangeProperty(d(), request.requestor, property, incr_, 32, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(&length), 1);
                    outgoing_.emplace(std::make_pair(request.requestor, property),
                        Outgoing{request.requestor, property, request.target, payload, 0, Clock::now() + std::chrono::seconds(5)});
                    accepted = true;
                }
            }
        }
        if (errors_.sync(d()) && accepted) reply.xselection.property = property;
        XSendEvent(d(), request.requestor, False, NoEventMask, &reply); XFlush(d());
    }
    void send_increment(Window window, Atom property) {
        auto it = outgoing_.find({window, property}); if (it == outgoing_.end()) return;
        auto& transfer = it->second;
        const auto length = std::min(chunk_size, transfer.text->size() - transfer.offset);
        XChangeProperty(d(), window, property, transfer.target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(transfer.text->data() + transfer.offset), int(length));
        transfer.offset += length;
        if (length == 0) outgoing_.erase(it);
        XFlush(d());
    }
    void events() {
        // Bound each event-loop turn even if another local client floods X events.
        for (unsigned n = 0; n < 512 && XPending(d()); ++n) {
            XEvent event{}; XNextEvent(d(), &event);
            if (pointer_) pointer_->event(event.type);
            if (event.type == selection_event_) {
                SelectionChange changed{}; static_assert(sizeof(changed) <= sizeof(event));
                std::memcpy(&changed, &event, sizeof(changed));
                if (changed.selection == clipboard_) fetch_selection(changed.owner);
            } else if (event.type == SelectionRequest) selection_request(event.xselectionrequest);
            else if (event.type == SelectionNotify && incoming_ && event.xselection.requestor == incoming_->window) {
                if (event.xselection.selection != clipboard_ || event.xselection.target != incoming_->target) cancel_incoming();
                else if (event.xselection.property == None && incoming_->target == targets_) convert_target(utf8_);
                else if (event.xselection.property != property_) cancel_incoming();
                else read_selection(true);
            } else if (event.type == PropertyNotify) {
                const auto& p = event.xproperty;
                if (p.window == window_ && p.atom == clock_ && p.state == PropertyNewValue && claiming_) {
                    ownership_time_ = p.time; claiming_ = false;
                    XSetSelectionOwner(d(), clipboard_, window_, p.time); XFlush(d());
                } else if (incoming_ && incoming_->incremental && p.window == incoming_->window && p.atom == property_ && p.state == PropertyNewValue) read_selection(false);
                else if (p.state == PropertyDelete) send_increment(p.window, p.atom);
            } else if (event.type == DestroyNotify) {
                std::erase_if(outgoing_, [&](const auto& item) { return item.second.window == event.xdestroywindow.window; });
            }
        }
        const auto now = Clock::now();
        if (incoming_ && now >= incoming_->deadline) cancel_incoming();
        std::erase_if(outgoing_, [&](const auto& item) { return now >= item.second.deadline; });
        (void)errors_.sync(d());
    }
    void key_event(unsigned key, bool down) {
        if (!key || key > 255) return;
        if (down) pressed_keys_.insert(key); else if (!pressed_keys_.erase(key)) return;
        key_(d(), key, down ? True : False, CurrentTime);
    }
    void button_event(unsigned button, bool down) {
        if (down) pressed_buttons_.insert(button); else if (!pressed_buttons_.erase(button)) return;
        button_(d(), button, down ? True : False, CurrentTime);
    }
public:
    explicit X11Desktop(const std::string& name, X11Resize resize = {})
        : display_(XOpenDisplay(name.empty() ? nullptr : name.c_str())), errors_(display_.get()), resize_(std::move(resize)) {
        require(d() != nullptr, "cannot open X11 display; check DISPLAY and Xauthority");
        screen_ = DefaultScreen(d()); root_ = RootWindow(d(), screen_);
        int event = 0, error = 0, major = 0, minor = 0;
        auto query = xtest_.symbol<Bool (*)(Display*, int*, int*, int*, int*)>("XTestQueryExtension");
        require(query(d(), &event, &error, &major, &minor), "XTEST input extension is unavailable");
        auto fixes_query = xfixes_.symbol<Bool (*)(Display*, int*, int*)>("XFixesQueryExtension");
        require(fixes_query(d(), &selection_event_, &error), "XFIXES selection extension is unavailable");
        Monitor monitor; monitor.width = unsigned(DisplayWidth(d(), screen_)); monitor.height = unsigned(DisplayHeight(d(), screen_));
        layout_ = validate_layout({monitor});
        window_ = XCreateSimpleWindow(d(), root_, 0, 0, 1, 1, 0, 0, 0);
        XSelectInput(d(), window_, PropertyChangeMask);
        clipboard_ = atom("CLIPBOARD"); utf8_ = atom("UTF8_STRING"); targets_ = atom("TARGETS"); timestamp_ = atom("TIMESTAMP");
        html_ = atom("text/html"); bmp_ = atom("image/bmp");
#ifdef LRDP_HAVE_PNG
        png_ = atom("image/png");
#endif
        uri_ = atom("text/uri-list"); copied_files_ = atom("x-special/gnome-copied-files");
        property_ = atom("_LRDP_CLIPBOARD"); clock_ = atom("_LRDP_CLOCK"); incr_ = atom("INCR");
        auto select = xfixes_.symbol<void (*)(Display*, Window, Atom, unsigned long)>("XFixesSelectSelectionInput");
        select(d(), window_, clipboard_, 7);
        pointer_ = std::make_unique<X11Pointer>(d());
        fetch_selection(XGetSelectionOwner(d(), clipboard_));
        require(errors_.sync(d()), "failed to initialize X11 desktop resources");
    }
    ~X11Desktop() override {
        release_input(); cancel_incoming();
        if (window_) XDestroyWindow(d(), window_);
        XSync(d(), False);
        pointer_.reset();
        display_.reset();
    }
    Layout layout() const override { return layout_; }
    bool resizable() const override { return bool(resize_); }
    bool resize(const Layout& requested) override {
        if (!resize_) return false;
        auto next = validate_layout(requested.monitors);
        if (next.monitors == layout_.monitors) return true;
        release_input();
        if (!resize_(d(), next)) return false;
        layout_ = std::move(next); return true;
    }
    std::shared_ptr<const PointerShape> pointer_shape() override { return pointer_->current(); }
    Frame capture() override {
        events();
        XWindowAttributes attributes{};
        require(XGetWindowAttributes(d(), root_, &attributes), "cannot query X11 root");
        // A compositor mode change requires RDP reactivation, not a mismatched bitmap.
        require(attributes.width == int(layout_.width) && attributes.height == int(layout_.height), "X11 display geometry changed; reconnect to adopt the new mode");
        std::unique_ptr<XImage, ImageCloser> image(XGetImage(d(), root_, 0, 0, layout_.width, layout_.height, AllPlanes, ZPixmap));
        require(image != nullptr, "X11 framebuffer capture failed");
        Frame frame{layout_.width, layout_.height, Bytes(std::size_t(layout_.width) * layout_.height * 4)};
        const bool direct = image->bits_per_pixel == 32 && image->byte_order == LSBFirst && image->red_mask == 0xff0000 && image->green_mask == 0xff00 && image->blue_mask == 0xff;
        auto component = [](unsigned long pixel, unsigned long mask) -> std::uint8_t {
            if (!mask) return 0;
            const auto shift = std::countr_zero(mask); const auto maximum = mask >> shift;
            return std::uint8_t(((pixel & mask) >> shift) * 255 / maximum);
        };
        for (unsigned y = 0; y < frame.height; ++y) {
            auto* target = frame.bgra.data() + std::size_t(y) * frame.width * 4;
            if (direct) {
                std::memcpy(target, image->data + std::ptrdiff_t(y) * image->bytes_per_line, std::size_t(frame.width) * 4);
                for (unsigned x = 0; x < frame.width; ++x) target[x * 4 + 3] = 255;
            } else for (unsigned x = 0; x < frame.width; ++x) {
                const auto pixel = XGetPixel(image.get(), int(x), int(y));
                target[x * 4] = component(pixel, image->blue_mask); target[x * 4 + 1] = component(pixel, image->green_mask);
                target[x * 4 + 2] = component(pixel, image->red_mask); target[x * 4 + 3] = 255;
            }
        }
        return frame;
    }
    void input(const InputEvent& event) override {
        if (event.kind == InputKind::scancode) {
            const auto evdev = evdev_code(event); if (evdev) key_event(evdev + 8, !(event.flags & 0x8000));
        } else if (event.kind == InputKind::synchronize) {
            // Never retain keys across a focus/activation synchronization boundary.
            release_input();
            const auto num = XkbKeysymToModifiers(d(), XK_Num_Lock), scroll = XkbKeysymToModifiers(d(), XK_Scroll_Lock);
            const auto mask = unsigned(LockMask) | num | scroll;
            const auto desired = (event.flags & 4 ? unsigned(LockMask) : 0U) | (event.flags & 2 ? num : 0U) | (event.flags & 1 ? scroll : 0U);
            XkbLockModifiers(d(), XkbUseCoreKbd, mask, desired);
        } else if (event.kind == InputKind::unicode) {
            // Do not mutate the global keyboard map or synthesize Ctrl+Shift+U: both
            // change application semantics. Scancode input is the supported X11 path.
            return;
        } else {
            const int x = std::min<unsigned>(event.x, layout_.width - 1), y = std::min<unsigned>(event.y, layout_.height - 1);
            motion_(d(), screen_, x, y, CurrentTime);
            const bool down = (event.flags & 0x8000) != 0;
            if (event.kind == InputKind::pointer_extended) {
                if (event.flags & 1) button_event(8, down);
                if (event.flags & 2) button_event(9, down);
            } else if (event.flags & (0x200 | 0x400)) {
                int delta = event.flags & 0x1ff; if (delta & 0x100) delta -= 512;
                const bool horizontal = (event.flags & 0x400) != 0;
                auto& accumulated = horizontal ? horizontal_wheel_ : wheel_; accumulated += delta;
                while (std::abs(accumulated) >= 120) {
                    const unsigned button = horizontal ? (accumulated > 0 ? 7U : 6U) : (accumulated > 0 ? 4U : 5U);
                    button_(d(), button, True, CurrentTime); button_(d(), button, False, CurrentTime);
                    accumulated += accumulated > 0 ? -120 : 120;
                }
            } else {
                if (event.flags & 0x1000) button_event(1, down);
                if (event.flags & 0x2000) button_event(3, down);
                if (event.flags & 0x4000) button_event(2, down);
            }
        }
        XFlush(d());
    }
    bool enable_rich_clipboard() override {
        rich_enabled_ = true; fetch_selection(XGetSelectionOwner(d(), clipboard_)); return true;
    }
    void set_clipboard_rich(RichClipboard content) override {
        require(rich_enabled_, "X11 rich clipboard disabled"); content.validate();
        auto buffer=[](const Bytes& bytes) { return std::make_shared<const std::string>(bytes.begin(), bytes.end()); };
        auto html=content.html ? std::make_shared<const std::string>(*content.html) : nullptr;
        auto text=content.text ? std::make_shared<const std::string>(*content.text) : nullptr;
        auto bmp=content.image ? buffer(encode_clipboard_bmp(*content.image)) : nullptr;
        std::shared_ptr<const std::string> png;
#ifdef LRDP_HAVE_PNG
        if(content.image) {
            try { png=buffer(encode_clipboard_png(*content.image)); }
            catch(const ProtocolError&) { /* BMP remains available if PNG exceeds its encoded-size quota. */ }
        }
#endif
        owned_text_=std::move(text); owned_html_=std::move(html); owned_bmp_=std::move(bmp); owned_png_=std::move(png);
        owns_rich_=true; owns_files_=false; owned_uris_.reset(); owned_copied_.reset();
        ready_text_.reset(); ready_files_.reset(); ready_rich_.reset(); cancel_incoming(); claiming_=true;
        XChangeProperty(d(),window_,clock_,XA_INTEGER,8,PropModeAppend,nullptr,0); XFlush(d());
    }
    std::optional<RichClipboard> poll_clipboard_rich() override {
        events(); auto result=std::move(ready_rich_); ready_rich_.reset(); return result;
    }
    bool enable_file_clipboard() override {
        files_enabled_ = true; fetch_selection(XGetSelectionOwner(d(), clipboard_)); return true;
    }
    void set_clipboard_files(std::vector<std::string> paths) override {
        require(files_enabled_, "X11 file clipboard is disabled");
        owns_rich_=false; owned_html_.reset(); owned_bmp_.reset(); owned_png_.reset(); ready_rich_.reset();
        auto uris = std::make_shared<const std::string>(encode_file_uris(paths));
        auto copied = std::make_shared<const std::string>(encode_file_uris(paths, true));
        owned_uris_ = std::move(uris); owned_copied_ = std::move(copied); owns_files_ = true;
        ready_files_.reset(); ready_text_.reset(); cancel_incoming(); claiming_ = true;
        XChangeProperty(d(), window_, clock_, XA_INTEGER, 8, PropModeAppend, nullptr, 0); XFlush(d());
    }
    std::optional<std::vector<std::string>> poll_clipboard_files() override {
        events(); auto result = std::move(ready_files_); ready_files_.reset(); return result;
    }
    void set_clipboard(std::string text) override {
        require(text.size() <= clipboard_limit && utf16le(text).size() <= clipboard_limit, "X11 clipboard exceeds policy");
        owned_text_ = std::make_shared<const std::string>(std::move(text));
        owns_rich_=false; owned_html_.reset(); owned_bmp_.reset(); owned_png_.reset(); ready_rich_.reset();
        owns_files_ = false; owned_uris_.reset(); owned_copied_.reset(); ready_files_.reset();
        ready_text_.reset(); cancel_incoming(); claiming_ = true;
        XChangeProperty(d(), window_, clock_, XA_INTEGER, 8, PropModeAppend, nullptr, 0); XFlush(d());
    }
    std::optional<std::string> poll_clipboard() override {
        events(); auto result = std::move(ready_text_); ready_text_.reset(); return result;
    }
    void release_input() override {
        if (!d()) return;
        for (const auto key : pressed_keys_) key_(d(), key, False, CurrentTime);
        for (const auto button : pressed_buttons_) button_(d(), button, False, CurrentTime);
        pressed_keys_.clear(); pressed_buttons_.clear(); wheel_ = horizontal_wheel_ = 0; XFlush(d());
    }
};
} // namespace
std::unique_ptr<Desktop> make_x11_desktop(const std::string& display) { return std::make_unique<X11Desktop>(display); }
std::unique_ptr<Desktop> make_managed_x11_desktop(const std::string& display, X11Resize resize) {
    require(bool(resize), "managed X11 requires an output controller");
    return std::make_unique<X11Desktop>(display, std::move(resize));
}
} // namespace lrdp
