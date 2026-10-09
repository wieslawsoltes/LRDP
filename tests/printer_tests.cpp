#include "lrdp/printing/jobs.hpp"
#include <algorithm>
#include <iostream>
#include <random>
using namespace lrdp;
using namespace lrdp::drive;
namespace {
unsigned checks=0;
void check(bool ok,const char* text){++checks;require(ok,text);}
template<class F>void rejects(F f){++checks;try{f();}catch(const ProtocolError&){return;}throw std::runtime_error("invalid printer request accepted");}
Bytes metadata(std::string name="Printer 日本",unsigned flags=0){
    const auto driver=flags&1?Bytes{'P','S',0}:utf16le("PostScript"),label=utf16le(name);
    Writer w;w.le32(flags).le32(0).le32(0).le32(unsigned(driver.size())).le32(unsigned(label.size())).le32(0).raw(driver).raw(label);return std::move(w).finish();
}
void initialize(Protocol& p,bool printer_expected){
    p.start();(void)p.drain();Writer announce;announce.le16(1).le16(13).le32(1);p.receive(pdu(0x4343,announce.bytes()));
    Writer name;name.le32(1).le32(0).le32(4).le16('p').le16(0);p.receive(pdu(0x434e,name.bytes()));
    auto outgoing=p.drain();Reader in(outgoing[0]);in.skip(4);const auto count=in.le16();in.skip(2);bool printer=false;
    for(unsigned i=0;i<count;++i){const auto kind=in.le16(),size=in.le16();const auto version=in.le32();if(kind==2){check(size==8&&version==1,"printer capability version");printer=true;}in.skip(size-8);}
    check(printer==printer_expected,"printer opt-in capability gating");
    auto caps=outgoing[0];caps[2]=0x50;caps[3]=0x43;p.receive(caps);(void)p.drain();
}
void announce(Protocol& p,unsigned id=9,unsigned flags=0){const auto m=metadata("Printer 日本",flags);Writer w;w.le32(1).le32(4).le32(id).raw({'P','R','N','1',0,0,0,0}).le32(unsigned(m.size())).raw(m);p.receive(pdu(0x4441,w.bytes()));}
struct Wire {unsigned device,completion,major;Bytes body;};
Wire take(Protocol& p){auto packets=p.drain();check(packets.size()==1,"one printer wire request");Reader in(packets[0]);in.skip(4);Wire w;w.device=in.le32();in.skip(4);w.completion=in.le32();w.major=in.le32();check(in.le32()==0,"printer minor function");auto b=in.take(in.remaining());w.body.assign(b.begin(),b.end());return w;}
Reply respond(Protocol& p,const Wire& w,unsigned status,View body={}){Writer b;b.le32(w.device).le32(w.completion).le32(status).raw(body);p.receive(pdu(0x4943,b.bytes()));auto r=p.take_replies();check(r.size()==1,"single printer completion");return r[0];}
void protocol(){
    Protocol disabled;initialize(disabled,false);announce(disabled);check(disabled.devices().empty(),"printer enabled without opt-in");
    Limits l;l.printers=true;Protocol p(l);initialize(p,true);announce(p);(void)p.drain();
    auto printer=p.devices()[0];check(printer.printer&&printer.printer->name=="Printer 日本"&&printer.printer->driver=="PostScript","printer metadata");
    Request r;r.ticket=1;r.device=printer.key;
    check(p.submit(r)&&p.take_replies()[0].status==denied&&p.drain().empty(),"filesystem request routed to printer");
    r.purpose=Purpose::printer;check(p.submit(r),"printer create blocked by readonly drive policy");auto w=take(p);check(w.major==0&&w.body==Bytes(32),"printer CREATE path length must be zero");
    auto reply=respond(p,w,0,Bytes{17,0,0,0});check(reply.purpose==Purpose::printer&&reply.handle,"printer handle reply domain");
    r.handle=reply.handle;r.ticket=2;r.operation=Operation::write;r.length=3;r.data={1,2,3};
    check(p.submit(r),"printer write refused");w=take(p);Reader body(w.body);check(body.le32()==3,"printer write size");for(auto b:body.take(28))check(b==0,"printer write reserved bytes");check(body.take(3)[2]==3,"printer bytes");
    reply=respond(p,w,0,Bytes{3,0,0,0,0});check(reply.transferred==3,"printer write completion count");
    r.ticket=3;r.operation=Operation::read;check(p.submit(r)&&p.take_replies()[0].status==unsupported,"printer read enabled");
    r.operation=Operation::close;r.data.clear();r.ticket=4;check(p.submit(r),"printer close");w=take(p);check(w.body==Bytes(32),"printer close padding");respond(p,w,0,Bytes(4));
    r.operation=Operation::open;r.handle=0;r.ticket=5;check(p.submit(r),"pending unplug create");w=take(p);
    Writer remove;remove.le32(1).le32(9);p.receive(pdu(0x444d,remove.bytes()));auto gone=p.take_replies();check(gone.size()==1&&gone[0].purpose==Purpose::printer&&gone[0].status==removed,"unplug domain propagation");
    announce(p);(void)p.drain();check(p.devices()[0].key.generation!=printer.key.generation,"printer replug generation");
    Writer late;late.le32(9).le32(w.completion).le32(0).le32(88);p.receive(pdu(0x4943,late.bytes()));check(p.take_replies().empty(),"late printer create resurrected stale handle");
    Request evil;r.path="bad";rejects([&]{request_body(r,65536);});
    // Filesystem and printer tickets may have the same number but cannot complete each other's requests.
    Writer drive;drive.le32(1).le32(8).le32(7).raw({'D','A','T','A',0,0,0,0}).le32(0);p.receive(pdu(0x4441,drive.bytes()));(void)p.drain();
    auto devices=p.devices();auto disk=*std::find_if(devices.begin(),devices.end(),[](const auto& d){return !d.printer;});
    r={};r.ticket=100;r.device=disk.key;check(p.submit(r),"drive create coexistence");auto disk_wire=take(p);
    r.device=p.devices().back().key;r.purpose=Purpose::printer;check(p.submit(r),"cross-domain ticket collision");auto print_wire=take(p);
    check(disk_wire.completion!=print_wire.completion,"shared completion namespace collision");
    check(respond(p,print_wire,0,Bytes{4,0,0,0}).purpose==Purpose::printer,"printer reply lost domain");
    check(respond(p,disk_wire,0,Bytes{4,0,0,0}).purpose==Purpose::filesystem,"drive reply lost domain");
}
class Buffer final:public printing::Source {
public:
    Bytes bytes;
    explicit Buffer(unsigned n):bytes(n){for(unsigned i=0;i<n;++i)bytes[i]=std::uint8_t(i);}
    std::uint64_t size()const override{return bytes.size();}
    Bytes read(std::uint64_t offset,std::uint32_t n)const override{return Bytes(bytes.begin()+std::ptrdiff_t(offset),bytes.begin()+std::ptrdiff_t(offset+n));}
};
void jobs(){
    printing::Jobs jobs;Device d{{9,3},"PRN1",PrinterInfo{0,"PS","Printer"}};jobs.publish({d});
    auto source=std::make_shared<Buffer>(180003);jobs.submit(1,d.key,source);jobs.submit(2,d.key,source);
    rejects([&]{jobs.submit(3,{9,2},source);});
    std::uint64_t handle=1;Bytes accepted;unsigned finished=0;
    while(jobs.size()) {
        auto requests=jobs.take_requests();check(requests.size()==1,"same printer jobs interleaved");const auto& r=requests[0];
        Reply reply{r.ticket,0,0,0,{},Purpose::printer};
        if(r.operation==Operation::open){reply.handle=handle++;accepted.clear();}
        else if(r.operation==Operation::write){reply.transferred=std::min<unsigned>(4093,r.length);accepted.insert(accepted.end(),r.data.begin(),r.data.begin()+reply.transferred);}
        else check(accepted==source->bytes,"partial printer writes lost bytes");
        jobs.complete(reply);auto result=jobs.take_results();
        if(!result.empty()){check(r.operation==Operation::close && result[0].status==0 && result[0].transferred==source->size(),"job success before close");++finished;}
    }
    check(finished==2,"queued printer jobs not completed");
    jobs.submit(3,d.key,source);auto r=jobs.take_requests()[0];jobs.complete({r.ticket,0,1,0,{},Purpose::printer});
    r=jobs.take_requests()[0];jobs.complete({r.ticket,0,0,0,{},Purpose::printer});
    r=jobs.take_requests()[0];check(r.operation==Operation::close,"zero-progress printer write looped");jobs.complete({r.ticket,0,0,0,{},Purpose::printer});
    check(jobs.take_results()[0].status!=0,"zero-progress print reported success");
    jobs.submit(4,d.key,source);r=jobs.take_requests()[0];jobs.cancel(4);jobs.complete({r.ticket,0,9,0,{},Purpose::printer});
    r=jobs.take_requests()[0];check(r.operation==Operation::close,"cancel did not close completed create");jobs.complete({r.ticket,0,0,0,{},Purpose::printer});
    check(jobs.take_results()[0].status==0xc0000120,"cancel reported print success");
}
void malformed(){
    const auto m=metadata();for(std::size_t n=0;n<m.size();++n)rejects([&]{printer_information(View(m).first(n));});
    check(printer_information(metadata("ASCII",1)).driver=="PS","ASCII driver decode");
    rejects([&]{printer_information(metadata("bad\nname"));});
    std::mt19937 random(0x50524e31);for(unsigned i=0;i<20000;++i){Bytes data(random()%512);for(auto& b:data)b=std::uint8_t(random());try{(void)printer_information(data);}catch(const ProtocolError&){} }
}
}
int main(){try{protocol();jobs();malformed();std::cout<<"PASS: "<<checks<<" printer checks and 20000 malformed metadata decodes; shared channel isolation, readonly preservation, queue/ACK/cancel/replug\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
