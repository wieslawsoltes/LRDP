#include "lrdp/platform/randr_outputs.hpp"
#include "lrdp/platform/shared_library.hpp"
#include <X11/Xlib.h>
#include <X11/extensions/randr.h>
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>

namespace lrdp {
namespace {
// Public libXrandr ABI. Resolved at runtime, like the existing XTEST/XFIXES
// integrations. These declarations describe platform structures, not RDP code.
using Mode = XID; using Output = XID; using Crtc = XID;
struct ModeInfo {
    Mode id; unsigned width, height; unsigned long dotClock;
    unsigned hSyncStart, hSyncEnd, hTotal, hSkew, vSyncStart, vSyncEnd, vTotal;
    char* name; unsigned nameLength; unsigned long modeFlags;
};
struct Resources {
    Time timestamp, configTimestamp; int ncrtc; Crtc* crtcs;
    int noutput; Output* outputs; int nmode; ModeInfo* modes;
};
struct OutputInfo {
    Time timestamp; Crtc crtc; char* name; int nameLen;
    unsigned long mm_width, mm_height; Connection connection; SubpixelOrder subpixel_order;
    int ncrtc; Crtc* crtcs; int nclone; Output* clones; int nmode, npreferred; Mode* modes;
};
struct CrtcInfo {
    Time timestamp; int x, y; unsigned width, height; Mode mode; Rotation rotation;
    int noutput; Output* outputs; Rotation rotations; int npossible; Output* possible;
};
class ErrorTrap {
    inline static thread_local ErrorTrap* current_ = nullptr;
    Display* display_; XErrorHandler previous_; ErrorTrap* parent_; bool failed_ = false;
    static int handler(Display* d, XErrorEvent* e) {
        if (current_ && current_->display_ == d) { current_->failed_ = true; return 0; }
        return current_ && current_->previous_ ? current_->previous_(d,e) : 0;
    }
public:
    explicit ErrorTrap(Display* d) : display_(d), previous_(nullptr), parent_(current_) {
        XSync(d,False); current_ = this; previous_ = XSetErrorHandler(handler);
    }
    ~ErrorTrap() { XSync(display_,False); XSetErrorHandler(previous_); current_ = parent_; }
    bool ok() { XSync(display_,False); return !std::exchange(failed_, false); }
};
struct ServerGrab {
    Display* display;
    explicit ServerGrab(Display* d) : display(d) { XGrabServer(d); }
    ~ServerGrab() { XUngrabServer(display); XFlush(display); }
};
struct Configuration {
    Crtc crtc; Mode mode; int x, y; Rotation rotation; std::vector<Output> outputs;
};
struct OwnedMode { Mode mode; Output output; };
}
struct RandrOutputs::Impl {
    SharedLibrary library{"libXrandr.so.2"};
#define LRDP_RANDR(name, type) type name = library.symbol<type>("XRR" #name)
    using Query = Status (*)(Display*,int*,int*); LRDP_RANDR(QueryVersion, Query);
    using Get = Resources* (*)(Display*,Window); LRDP_RANDR(GetScreenResources, Get);
    using Free = void (*)(Resources*); LRDP_RANDR(FreeScreenResources, Free);
    using GetOutput = OutputInfo* (*)(Display*,Resources*,Output); LRDP_RANDR(GetOutputInfo, GetOutput);
    using FreeOutput = void (*)(OutputInfo*); LRDP_RANDR(FreeOutputInfo, FreeOutput);
    using GetCrtc = CrtcInfo* (*)(Display*,Resources*,Crtc); LRDP_RANDR(GetCrtcInfo, GetCrtc);
    using FreeCrtc = void (*)(CrtcInfo*); LRDP_RANDR(FreeCrtcInfo, FreeCrtc);
    using SetCrtc = Status (*)(Display*,Resources*,Crtc,Time,int,int,Mode,Rotation,Output*,int);
    LRDP_RANDR(SetCrtcConfig, SetCrtc);
    using Range = Status (*)(Display*,Window,int*,int*,int*,int*); LRDP_RANDR(GetScreenSizeRange, Range);
    using Screen = void (*)(Display*,Window,int,int,int,int); LRDP_RANDR(SetScreenSize, Screen);
    using Create = Mode (*)(Display*,Window,ModeInfo*); LRDP_RANDR(CreateMode, Create);
    using Destroy = void (*)(Display*,Mode); LRDP_RANDR(DestroyMode, Destroy);
    using OutputMode = void (*)(Display*,Output,Mode);
    LRDP_RANDR(AddOutputMode, OutputMode); LRDP_RANDR(DeleteOutputMode, OutputMode);
    using GetPrimary = Output (*)(Display*,Window); LRDP_RANDR(GetOutputPrimary, GetPrimary);
    using SetPrimary = void (*)(Display*,Window,Output); LRDP_RANDR(SetOutputPrimary, SetPrimary);
#undef LRDP_RANDR
    std::vector<OwnedMode> owned;
    std::uint64_t generation = 0;
    auto resources(Display* d, Window root) {
        std::unique_ptr<Resources, Free> value(GetScreenResources(d,root), FreeScreenResources);
        require(value && value->ncrtc > 0 && value->ncrtc <= 64 && value->noutput > 0 && value->noutput <= 64,
                "invalid or unavailable RandR resources");
        return value;
    }
    bool set(Display* d, Window root, const Configuration& c) {
        auto r = resources(d,root);
        return SetCrtcConfig(d,r.get(),c.crtc,CurrentTime,c.x,c.y,c.mode,c.rotation,
            const_cast<Output*>(c.outputs.data()),int(c.outputs.size())) == RRSetConfigSuccess;
    }
    void discard(Display* d, const std::vector<OwnedMode>& modes) {
        for (const auto& m : modes) { DeleteOutputMode(d,m.output,m.mode); DestroyMode(d,m.mode); }
    }
    bool apply(Display* d, const Layout& supplied) {
        const auto layout = validate_layout(supplied.monitors);
        require(layout.width == supplied.width && layout.height == supplied.height &&
                layout.left == supplied.left && layout.top == supplied.top, "inconsistent headless layout");
        int major = 1, minor = 3;
        require(QueryVersion(d,&major,&minor) && (major > 1 || (major == 1 && minor >= 3)), "RandR 1.3 required");
        const auto root = DefaultRootWindow(d); ServerGrab grab(d); ErrorTrap errors(d);
        auto r = resources(d,root);
        int min_w, min_h, max_w, max_h;
        if (!GetScreenSizeRange(d,root,&min_w,&min_h,&max_w,&max_h) || int(layout.width) < min_w || int(layout.width) > max_w ||
            int(layout.height) < min_h || int(layout.height) > max_h) return false;
        XWindowAttributes old{}; require(XGetWindowAttributes(d,root,&old), "cannot query headless root");
        const auto old_primary = GetOutputPrimary(d,root);
        std::vector<Configuration> before, after;
        for (int i = 0; i < r->ncrtc; ++i) {
            std::unique_ptr<CrtcInfo,FreeCrtc> c(GetCrtcInfo(d,r.get(),r->crtcs[i]),FreeCrtcInfo);
            require(c && c->noutput >= 0 && c->noutput <= 64,"invalid RandR CRTC");
            Configuration saved{r->crtcs[i],c->mode,c->x,c->y,c->rotation,{}};
            if (c->noutput) saved.outputs.assign(c->outputs,c->outputs+c->noutput);
            before.push_back(std::move(saved));
        }
        // Reject even an otherwise valid layout on non-private hardware.
        std::vector<Output> outputs;
        for (int i = 0; i < r->noutput; ++i) {
            std::unique_ptr<OutputInfo,FreeOutput> o(GetOutputInfo(d,r.get(),r->outputs[i]),FreeOutputInfo);
            require(o && o->nameLen > 0 && o->nameLen <= 64,"invalid RandR output");
            require(std::string_view(o->name,std::size_t(o->nameLen)).starts_with("DUMMY"), "refusing to resize non-dummy outputs");
            outputs.push_back(r->outputs[i]);
        }
        if (layout.monitors.size() > outputs.size()) return false;
        std::vector<OwnedMode> created; created.reserve(layout.monitors.size());
        std::set<Crtc> used; bool changed = false; Output primary = None;
        try {
            ++generation;
            for (std::size_t i = 0; i < layout.monitors.size(); ++i) {
                const auto& m = layout.monitors[i];
                const Rotation rotation = m.orientation == 90 ? RR_Rotate_90 : m.orientation == 180 ? RR_Rotate_180 :
                                          m.orientation == 270 ? RR_Rotate_270 : RR_Rotate_0;
                std::unique_ptr<OutputInfo,FreeOutput> o(GetOutputInfo(d,r.get(),outputs[i]),FreeOutputInfo);
                require(o && o->ncrtc > 0 && o->ncrtc <= 64,"no CRTC for virtual output"); Crtc selected = None;
                for (int j = 0; j < o->ncrtc && selected == None; ++j) if (!used.contains(o->crtcs[j])) {
                    std::unique_ptr<CrtcInfo,FreeCrtc> c(GetCrtcInfo(d,r.get(),o->crtcs[j]),FreeCrtcInfo);
                    if (c && (c->rotations & rotation)) selected = o->crtcs[j];
                }
                require(selected != None,"virtual output cannot provide the requested rotation"); used.insert(selected);
                const bool rotated = m.orientation == 90 || m.orientation == 270;
                ModeInfo mode{}; mode.width = rotated ? m.height : m.width; mode.height = rotated ? m.width : m.height;
                // Timings are only for the dummy DDX, never programmed into hardware.
                mode.hSyncStart = mode.width+8; mode.hSyncEnd = mode.width+16; mode.hTotal = mode.width+32;
                mode.vSyncStart = mode.height+1; mode.vSyncEnd = mode.height+2; mode.vTotal = mode.height+8;
                mode.dotClock = static_cast<unsigned long>(mode.hTotal)*mode.vTotal*30;
                auto name = "LRDP-"+std::to_string(generation)+"-"+std::to_string(i);
                mode.name = name.data(); mode.nameLength = unsigned(name.size());
                const auto id = CreateMode(d,root,&mode); require(id != None && errors.ok(),"dummy mode creation failed");
                created.push_back({id,outputs[i]}); AddOutputMode(d,outputs[i],id); require(errors.ok(),"dummy mode attachment failed");
                after.push_back({selected,id,int(std::int64_t(m.left)-layout.left),int(std::int64_t(m.top)-layout.top),rotation,{outputs[i]}});
                if (m.flags & 1) primary = outputs[i];
            }
            changed = true;
            for (const auto& c : before) if (c.mode) require(set(d,root,{c.crtc,None,0,0,RR_Rotate_0,{}}),"cannot disable virtual CRTC");
            SetScreenSize(d,root,int(layout.width),int(layout.height),std::max(1,int(layout.width)*254/960),std::max(1,int(layout.height)*254/960));
            require(errors.ok(),"headless framebuffer resize failed");
            for (const auto& c : after) require(set(d,root,c),"virtual CRTC configuration rejected");
            SetOutputPrimary(d,root,primary); require(errors.ok(),"headless output commit failed");
            XWindowAttributes actual{};
            require(XGetWindowAttributes(d,root,&actual) && actual.width == int(layout.width) && actual.height == int(layout.height),"headless resize verification failed");
        } catch (const ProtocolError&) {
            bool restored = true;
            if (changed) {
                (void)errors.ok();
                for (const auto& c : before) restored = set(d,root,{c.crtc,None,0,0,RR_Rotate_0,{}}) && restored;
                SetScreenSize(d,root,old.width,old.height,std::max(1,old.width*254/960),std::max(1,old.height*254/960));
                restored = errors.ok() && restored;
                for (const auto& c : before) if (c.mode) restored = set(d,root,c) && restored;
                SetOutputPrimary(d,root,old_primary); restored = errors.ok() && restored;
            }
            discard(d,created); (void)errors.ok();
            require(restored,"headless display rollback failed; closing session"); return false;
        }
        discard(d,owned); (void)errors.ok(); owned = std::move(created); return true;
    }
};
RandrOutputs::RandrOutputs() : impl_(std::make_unique<Impl>()) {}
RandrOutputs::~RandrOutputs() = default;
bool RandrOutputs::apply(Display* d, const Layout& layout) { require(d != nullptr,"missing X display"); return impl_->apply(d,layout); }
}
