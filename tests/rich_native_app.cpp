// Independent native application: no LRDP library, wire codecs or image codecs.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
namespace {
using Bytes=std::vector<unsigned char>;
std::uint32_t le(const Bytes& data,std::size_t offset,unsigned n=4){
    if(n>4 || offset>data.size() || n>data.size()-offset) throw std::runtime_error("truncated bitmap");
    std::uint32_t value=0;for(unsigned i=0;i<n;++i)value|=std::uint32_t(data[offset+i])<<(i*8);return value;
}
void put(Bytes& data,std::uint32_t value,unsigned n=4){for(unsigned i=0;i<n;++i)data.push_back(static_cast<unsigned char>(value>>(i*8)));}
std::string html(){ return "<p>"+std::string(1200000,'z')+" 🚀 native</p>"; }
Bytes bitmap(){
    Bytes out{'B','M'};put(out,78);put(out,0);put(out,54);put(out,40);put(out,3);put(out,2);put(out,1,2);put(out,24,2);put(out,0);put(out,24);
    for(unsigned i=0;i<4;++i)put(out,0);
    for(unsigned row=0;row<2;++row){for(unsigned x=0;x<3;++x){out.push_back(static_cast<unsigned char>(10+row*30+x));out.push_back(70);out.push_back(120);}out.insert(out.end(),3,0);}
    return out;
}
}
int main(int argc,char** argv){
    if(argc!=2)return 1;
    Display* d=XOpenDisplay(nullptr);if(!d)return 2;
    const auto window=XCreateSimpleWindow(d,DefaultRootWindow(d),0,0,300,180,0,0,0x336699);
    auto atom=[&](const char* s){return XInternAtom(d,s,False);};
    const auto clipboard=atom("CLIPBOARD"),targets=atom("TARGETS"),html_type=atom("text/html"),bmp=atom("image/bmp"),incr=atom("INCR"),property=atom("_RICH_PASTE");
    std::ofstream log(argv[1],std::ios::app);
    const auto offered_html=html();std::map<Atom,Bytes> offered{{html_type,{offered_html.begin(),offered_html.end()}},{bmp,bitmap()}};
    struct Transfer{Atom type;Bytes bytes;std::size_t offset;};std::map<std::pair<Window,Atom>,Transfer> sending;
    Atom receiving=None;bool incremental=false;Bytes incoming;
    auto request=[&](Atom type){receiving=type;incremental=false;incoming.clear();XConvertSelection(d,clipboard,type,property,window,CurrentTime);XFlush(d);};
    auto finish=[&]{
        if(receiving==html_type){
            const std::string expected="<strong>"+std::string(200007,'h')+" 🌍</strong>";
            if(std::string(incoming.begin(),incoming.end())!=expected)throw std::runtime_error("HTML paste bytes differ");
            log<<"HTML_OK\n"<<std::flush; request(bmp);
        }else if(receiving==bmp){
            if(le(incoming,0,2)!=0x4d42 || le(incoming,2)!=incoming.size() || le(incoming,14)!=124 || le(incoming,18)!=202 || le(incoming,22)!=102 || le(incoming,28,2)!=32)
                throw std::runtime_error("pasted bitmap header differs");
            const auto offset=le(incoming,10);if(incoming.size()!=offset+202*102*4)throw std::runtime_error("pasted bitmap size differs");
            for(unsigned row=0;row<102;++row)for(unsigned x=0;x<202;++x){
                const auto at=offset+(row*202+x)*4;const unsigned y=101-row;
                if(incoming[at]!=x%256 || incoming[at+1]!=y%256 || incoming[at+2]!=90 || incoming[at+3]!=(x+y)%256)throw std::runtime_error("pasted bitmap pixel/alpha differs");
            }
            log<<"IMAGE_OK\nPASTE_OK\n"<<std::flush; receiving=None;incremental=false;
        }
    };
    auto read=[&](bool first){
        Atom type=None;int format=0;unsigned long count=0,after=0;unsigned char* raw=nullptr;
        if(XGetWindowProperty(d,window,property,0,4*1024*1024,True,AnyPropertyType,&type,&format,&count,&after,&raw)!=Success)throw std::runtime_error("paste read failed");
        struct Free{unsigned char* p;~Free(){XFree(p);}} guard{raw};
        if(after || count>8*1024*1024)throw std::runtime_error("paste size exceeds limit");
        if(first && type==incr){if(format!=32 || count!=1)throw std::runtime_error("bad INCR");incremental=true;}
        else{
            if(type!=receiving || format!=8 || count>8*1024*1024-incoming.size())throw std::runtime_error("paste type/bounds");
            if(count)incoming.insert(incoming.end(),raw,raw+count);
            if(!incremental || !count)finish();
        }
        XFlush(d);
    };
    XSelectInput(d,window,ButtonPressMask|PropertyChangeMask);XMapWindow(d,window);XSetInputFocus(d,window,RevertToParent,CurrentTime);
    XSetSelectionOwner(d,clipboard,window,CurrentTime);XFlush(d);log<<"READY "<<getpid()<<'\n'<<std::flush;
    try{
        for(;;){XEvent event{};XNextEvent(d,&event);
            if(event.type==SelectionClear)log<<"REMOTE_OWNER\n"<<std::flush;
            else if(event.type==ButtonPress && receiving==None)request(html_type);
            else if(event.type==SelectionNotify){if(event.xselection.property!=property)throw std::runtime_error("paste conversion rejected");read(true);}
            else if(event.type==SelectionRequest){
                const auto& r=event.xselectionrequest;XEvent response{};auto& v=response.xselection;
                v.type=SelectionNotify;v.display=d;v.requestor=r.requestor;v.selection=r.selection;v.target=r.target;v.time=r.time;v.property=r.property;
                if(r.target==targets){const Atom types[]{targets,html_type,bmp};XChangeProperty(d,r.requestor,r.property,XA_ATOM,32,PropModeReplace,reinterpret_cast<const unsigned char*>(types),3);}
                else if(auto it=offered.find(r.target);it!=offered.end()){
                    const auto& content=it->second;
                    if(content.size()<=16000)XChangeProperty(d,r.requestor,r.property,r.target,8,PropModeReplace,content.data(),int(content.size()));
                    else{const unsigned long length=content.size();sending.insert_or_assign({r.requestor,r.property},Transfer{r.target,content,0});XSelectInput(d,r.requestor,PropertyChangeMask);
                        XChangeProperty(d,r.requestor,r.property,incr,32,PropModeReplace,reinterpret_cast<const unsigned char*>(&length),1);}
                }else v.property=None;
                XSendEvent(d,r.requestor,False,NoEventMask,&response);XFlush(d);
            }else if(event.type==PropertyNotify){const auto& p=event.xproperty;
                if(p.window==window && p.atom==property && p.state==PropertyNewValue && incremental)read(false);
                else if(p.state==PropertyDelete){auto it=sending.find({p.window,p.atom});if(it==sending.end())continue;auto& t=it->second;
                    const auto n=std::min<std::size_t>(16000,t.bytes.size()-t.offset);XChangeProperty(d,p.window,p.atom,t.type,8,PropModeReplace,t.bytes.data()+t.offset,int(n));
                    t.offset+=n;if(!n)sending.erase(it);XFlush(d);}
            }
        }
    }catch(const std::exception& e){log<<"FAIL "<<e.what()<<'\n'<<std::flush;return 3;}
}
