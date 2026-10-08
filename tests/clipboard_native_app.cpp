#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
// Independent Xlib application. Does not link LRDP or use its URI/file codecs.
namespace {
std::string read(const std::filesystem::path& path) { std::ifstream f(path,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
std::string decode(const std::string& uri) {
    if (!uri.starts_with("file:///")) throw std::runtime_error("nonlocal paste URI");
    std::string path;
    for(std::size_t i=7;i<uri.size();++i) {
        if(uri[i]=='%') { if(i+2>=uri.size()) throw std::runtime_error("bad URI escape"); path+=char(std::stoi(uri.substr(i+1,2),nullptr,16)); i+=2; }
        else path+=uri[i];
    }
    return path;
}
void verify(const std::string& text,std::ofstream& log) {
    std::size_t total=0,files=0;
    for(std::size_t start=0;start<text.size();) {
        auto end=text.find('\n',start); if(end==std::string::npos) end=text.size();
        auto line=text.substr(start,end-start); start=end+1; if(!line.empty() && line.back()=='\r') line.pop_back(); if(line.empty()) continue;
        const std::filesystem::path path=decode(line); log<<"URI "<<line<<'\n';
        auto check_file=[&](const std::filesystem::path& p) {
            auto bytes=read(p);
            if(p.filename()=="żółć 🚀.bin") {
                if(bytes.size()!=200007) throw std::runtime_error("wrong pasted file size");
                for(std::size_t i=0;i<bytes.size();++i) if(static_cast<unsigned char>(bytes[i])!=std::uint8_t(i*7+3)) throw std::runtime_error("wrong pasted file bytes");
            } else if(!bytes.empty() || p.filename()!="zero") throw std::runtime_error("unexpected pasted file");
            total+=bytes.size(); ++files;
        };
        if(std::filesystem::is_directory(path)) { for(const auto& e:std::filesystem::recursive_directory_iterator(path)) if(e.is_regular_file()) check_file(e.path()); }
        else check_file(path);
    }
    if(files!=2 || total!=200007) throw std::runtime_error("incomplete pasted tree");
    log<<"PASTE_OK "<<total<<'\n'<<std::flush;
}
}
int main(int argc,char** argv) {
    if(argc!=3) return 1;
    Display* d=XOpenDisplay(nullptr); if(!d) return 2;
    std::ofstream log(argv[2],std::ios::app); const auto offered=read(std::filesystem::path(argv[1])/"offer.uris");
    const auto root=DefaultRootWindow(d), window=XCreateSimpleWindow(d,root,0,0,300,180,0,0,0x336699);
    const auto clipboard=XInternAtom(d,"CLIPBOARD",False),uri=XInternAtom(d,"text/uri-list",False),targets=XInternAtom(d,"TARGETS",False);
    const auto property=XInternAtom(d,"_LRDP_TEST_PASTE",False);
    XSelectInput(d,window,ButtonPressMask|StructureNotifyMask);
    XMapWindow(d,window); XSetInputFocus(d,window,RevertToParent,CurrentTime);
    const char source_bits[]={1,2,4},mask_bits[]={7,7,7};
    const auto source=XCreateBitmapFromData(d,window,source_bits,3,3),mask=XCreateBitmapFromData(d,window,mask_bits,3,3);
    XColor red{}; red.red=65535; XColor green{}; green.green=65535;
    const auto cursor=XCreatePixmapCursor(d,source,mask,&red,&green,1,2); XDefineCursor(d,window,cursor);
    XWarpPointer(d,None,window,0,0,0,0,50,50); XSetSelectionOwner(d,clipboard,window,CurrentTime); XFlush(d);
    log<<"READY "<<getpid()<<'\n'<<std::flush;
    try {
        for(;;) {
            XEvent event{}; XNextEvent(d,&event);
            if(event.type==SelectionClear) log<<"REMOTE_OWNER\n"<<std::flush;
            else if(event.type==ButtonPress) { XConvertSelection(d,clipboard,uri,property,window,CurrentTime); XFlush(d); }
            else if(event.type==SelectionNotify) {
                if(event.xselection.property==None) throw std::runtime_error("paste conversion failed");
                Atom type=None; int format=0; unsigned long n=0,after=0; unsigned char* data=nullptr;
                if(XGetWindowProperty(d,window,property,0,16384,True,AnyPropertyType,&type,&format,&n,&after,&data)!=Success) throw std::runtime_error("paste read failed");
                std::string text(reinterpret_cast<char*>(data),n); XFree(data);
                if(type!=uri || format!=8 || after) throw std::runtime_error("invalid paste property");
                verify(text,log);
            } else if(event.type==SelectionRequest) {
                const auto& r=event.xselectionrequest; XEvent reply{}; auto& v=reply.xselection;
                v.type=SelectionNotify; v.display=d; v.requestor=r.requestor; v.selection=r.selection; v.target=r.target; v.time=r.time;
                const auto dest=r.property==None?r.target:r.property; v.property=dest;
                if(r.target==targets) { const Atom types[]={targets,uri}; XChangeProperty(d,r.requestor,dest,XA_ATOM,32,PropModeReplace,reinterpret_cast<const unsigned char*>(types),2); }
                else if(r.target==uri) XChangeProperty(d,r.requestor,dest,uri,8,PropModeReplace,reinterpret_cast<const unsigned char*>(offered.data()),int(offered.size()));
                else v.property=None;
                XSendEvent(d,r.requestor,False,NoEventMask,&reply); XFlush(d);
            }
        }
    } catch(const std::exception& e) { log<<"FAIL "<<e.what()<<'\n'<<std::flush; return 3; }
}
