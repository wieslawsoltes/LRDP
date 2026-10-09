#include "lrdp/desktop.hpp"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <thread>
using namespace lrdp;
namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
Bytes bytes(std::string_view text) { return {text.begin(),text.end()}; }
class Application {
public:
    Display* display = XOpenDisplay(nullptr);
    Window window;
    Atom clipboard, utf8, html, bmp, png, targets, incr, property;
    std::map<Atom,Bytes> owned;
    std::optional<Bytes> received;
    Atom receiving = None;
    bool incremental = false;
    Bytes partial;
    struct Transfer { Atom type; Bytes payload; std::size_t offset=0; };
    std::map<std::pair<Window,Atom>,Transfer> outgoing;
    Application() {
        check(display!=nullptr,"X11 test display"); window=XCreateSimpleWindow(display,DefaultRootWindow(display),0,0,1,1,0,0,0);
        XSelectInput(display,window,PropertyChangeMask);
        auto atom=[&](const char* s){ return XInternAtom(display,s,False); };
        clipboard=atom("CLIPBOARD"); utf8=atom("UTF8_STRING"); html=atom("text/html"); bmp=atom("image/bmp"); png=atom("image/png");
        targets=atom("TARGETS"); incr=atom("INCR"); property=atom("_RICH_TEST"); XSync(display,False);
    }
    ~Application(){ XDestroyWindow(display,window); XCloseDisplay(display); }
    void publish(std::map<Atom,Bytes> content) {
        owned=std::move(content); XSetSelectionOwner(display,clipboard,window,CurrentTime); XFlush(display);
    }
    void request(Atom type) {
        received.reset(); partial.clear(); incremental=false; receiving=type;
        XConvertSelection(display,clipboard,type,property,window,CurrentTime); XFlush(display);
    }
    void read(bool first) {
        Atom type=None; int format=0; unsigned long count=0,after=0; unsigned char* data=nullptr;
        check(XGetWindowProperty(display,window,property,0,4*1024*1024,True,AnyPropertyType,&type,&format,&count,&after,&data)==Success,"read application clipboard property");
        struct Release { unsigned char* data; ~Release(){ XFree(data); } } release{data};
        check(after==0,"application clipboard bound");
        if(first && type==incr) { check(format==32 && count==1,"native INCR preamble"); incremental=true; }
        else if(receiving==targets) {
            check(type==XA_ATOM && format==32,"native TARGETS type");
            const auto* atoms=reinterpret_cast<const unsigned long*>(data);
            Writer w; for(unsigned long i=0;i<count;++i) w.le32(std::uint32_t(atoms[i])); received=std::move(w).finish();
        } else {
            check(type==receiving && format==8,"native rich content type");
            if(count) partial.insert(partial.end(),data,data+count);
            if(!incremental || !count) received=partial;
        }
        XFlush(display);
    }
    void pump() {
        for(unsigned i=0;i<256 && XPending(display);++i) {
            XEvent event{}; XNextEvent(display,&event);
            if(event.type==SelectionNotify && event.xselection.requestor==window) {
                check(event.xselection.property==property,"native rich conversion rejected"); read(true);
            } else if(event.type==SelectionRequest) {
                const auto& r=event.xselectionrequest;
                XEvent reply{}; reply.xselection.type=SelectionNotify; reply.xselection.display=display;
                reply.xselection.requestor=r.requestor; reply.xselection.selection=r.selection; reply.xselection.target=r.target;
                reply.xselection.time=r.time; reply.xselection.property=r.property;
                if(r.target==targets) {
                    std::vector<Atom> values{targets}; for(const auto& [kind,payload]:owned){ (void)payload; values.push_back(kind); }
                    XChangeProperty(display,r.requestor,r.property,XA_ATOM,32,PropModeReplace,reinterpret_cast<const unsigned char*>(values.data()),int(values.size()));
                } else if(auto it=owned.find(r.target);it!=owned.end()) {
                    const auto& content=it->second;
                    if(content.size()<=8192) XChangeProperty(display,r.requestor,r.property,r.target,8,PropModeReplace,content.data(),int(content.size()));
                    else {
                        const unsigned long length=content.size();
                        outgoing.insert_or_assign({r.requestor,r.property},Transfer{r.target,content,0});
                        XSelectInput(display,r.requestor,PropertyChangeMask);
                        XChangeProperty(display,r.requestor,r.property,incr,32,PropModeReplace,reinterpret_cast<const unsigned char*>(&length),1);
                    }
                } else reply.xselection.property=None;
                XSendEvent(display,r.requestor,False,NoEventMask,&reply); XFlush(display);
            } else if(event.type==PropertyNotify) {
                const auto& p=event.xproperty;
                if(p.window==window && p.atom==property && p.state==PropertyNewValue && incremental) read(false);
                else if(p.state==PropertyDelete) {
                    auto it=outgoing.find({p.window,p.atom}); if(it==outgoing.end()) continue;
                    auto& t=it->second; const auto n=std::min<std::size_t>(8192,t.payload.size()-t.offset);
                    XChangeProperty(display,p.window,p.atom,t.type,8,PropModeReplace,t.payload.data()+t.offset,int(n));
                    t.offset+=n; if(!n) outgoing.erase(it); XFlush(display);
                }
            }
        }
    }
};
}
int main(){
    try {
        Application app; auto desktop=make_x11_desktop(""); check(desktop->enable_rich_clipboard(),"native rich clipboard availability");
        std::optional<RichClipboard> remote;
        auto until=[&](const std::function<bool()>& test){
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(!test()){
                if(auto content=desktop->poll_clipboard_rich()) remote=std::move(content);
                app.pump(); check(std::chrono::steady_clock::now()<deadline,"native rich clipboard timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        ClipboardImage image{202,102,Bytes(202*102*4)};
        for(std::size_t i=0;i<image.bgra.size();++i) image.bgra[i]=std::uint8_t(i*19);
        const std::string html="<p>"+std::string(180000,'x')+"🚀世界</p>";
        app.publish({{app.utf8,bytes("native plain 🚀")},{app.html,bytes(html)},{app.bmp,encode_clipboard_bmp(image)}});
        until([&]{ return remote.has_value(); });
        check(remote->html==html && remote->text=="native plain 🚀" && remote->image==image,"native HTML/BMP multi-target import");
#ifdef LRDP_TEST_PNG
        remote.reset(); app.publish({{app.png,encode_clipboard_png(image)},{app.html,bytes("<b>PNG</b>")}});
        until([&]{return remote.has_value();}); check(remote->image==image && remote->html=="<b>PNG</b>","native PNG import");
#endif
        RichClipboard content; content.text="server plain"; content.html=html; content.image=image;
        desktop->set_clipboard_rich(content); remote.reset();
        until([&]{const auto owner=XGetSelectionOwner(app.display,app.clipboard);return owner!=None && owner!=app.window;});
        app.request(app.html); until([&]{return app.received.has_value();}); check(*app.received==bytes(html),"native HTML incremental export");
        app.request(app.bmp); until([&]{return app.received.has_value();}); check(decode_clipboard_bmp(*app.received)==image,"native BMP incremental export");
#ifdef LRDP_TEST_PNG
        app.request(app.png); until([&]{return app.received.has_value();}); check(decode_clipboard_png(*app.received)==image,"native PNG export");
#endif
        check(!remote,"remote ownership must not echo");
        desktop->set_clipboard("only text");
        for(unsigned n=0;n<10;++n){ (void)desktop->poll_clipboard_rich(); app.pump(); }
        app.request(app.targets); until([&]{return app.received.has_value();}); Reader formats(*app.received);
        while(!formats.empty()){const auto atom=formats.le32();check(atom!=app.html && atom!=app.bmp && atom!=app.png,"old rich formats not advertised after text-only selection");}
        std::cout<<"PASS: native X11 HTML, BMP and optional PNG; atomic multi-target snapshots; bidirectional large INCR transfers; ownership and format revocation\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
