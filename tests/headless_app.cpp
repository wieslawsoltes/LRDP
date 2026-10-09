#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <fstream>
#include <cstring>
#include <unistd.h>
// Independent native Linux application; no LRDP library is linked.
int main(int argc,char** argv) {
    if (argc != 2) return 1;
    Display* d = XOpenDisplay(nullptr); if (!d) return 2;
    std::ofstream log(argv[1],std::ios::app); if (!log) return 3;
    const auto root = DefaultRootWindow(d);
    const auto window = XCreateSimpleWindow(d,root,10,10,180,120,0,0,0x336699);
    const auto clipboard = XInternAtom(d,"CLIPBOARD",False), utf8 = XInternAtom(d,"UTF8_STRING",False);
    const auto targets = XInternAtom(d,"TARGETS",False);
    XSelectInput(d,root,StructureNotifyMask);
    XSelectInput(d,window,ButtonPressMask|ButtonReleaseMask|KeyPressMask|KeyReleaseMask|ExposureMask);
    XMapWindow(d,window); XSetInputFocus(d,window,RevertToParent,CurrentTime);
    XSetSelectionOwner(d,clipboard,window,CurrentTime); XFlush(d);
    log << "START " << getpid() << ' ' << window << '\n' << std::flush;
    unsigned clicks = 0;
    for (;;) {
        XEvent event{}; XNextEvent(d,&event);
        if (event.type == ConfigureNotify && event.xconfigure.window == root) {
            log << "SIZE " << event.xconfigure.width << ' ' << event.xconfigure.height << '\n' << std::flush;
        } else if (event.type == ButtonPress) {
            ++clicks; XSetWindowBackground(d,window,clicks & 1 ? 0x55aa33 : 0x336699); XClearWindow(d,window); XFlush(d);
            log << "CLICK " << clicks << '\n' << "MODIFIERS " << event.xbutton.state << '\n' << std::flush;
        } else if (event.type == KeyPress || event.type == KeyRelease) {
            log << (event.type == KeyPress ? "KEY_DOWN " : "KEY_UP ") << event.xkey.keycode << '\n' << std::flush;
        } else if (event.type == SelectionRequest) {
            const auto& r = event.xselectionrequest; XEvent response{};
            response.xselection.type = SelectionNotify; response.xselection.display = d;
            response.xselection.requestor = r.requestor; response.xselection.selection = r.selection;
            response.xselection.target = r.target; response.xselection.time = r.time;
            response.xselection.property = None;
            const auto property = r.property == None ? r.target : r.property;
            if (r.target == utf8) {
                constexpr char text[] = "persistent Linux application clipboard";
                XChangeProperty(d,r.requestor,property,utf8,8,PropModeReplace,reinterpret_cast<const unsigned char*>(text),sizeof(text)-1);
                response.xselection.property = property;
            } else if (r.target == targets) {
                const Atom formats[] = {utf8,targets};
                XChangeProperty(d,r.requestor,property,XA_ATOM,32,PropModeReplace,reinterpret_cast<const unsigned char*>(formats),2);
                response.xselection.property = property;
            }
            XSendEvent(d,r.requestor,False,NoEventMask,&response); XFlush(d);
        }
    }
}
