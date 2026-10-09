#include "lrdp/drive/protocol.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <random>
using namespace lrdp;
using namespace lrdp::drive;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;require(value,message);}
template<class F>void rejects(F f){++checks;try{f();}catch(const ProtocolError&){return;}throw std::runtime_error("expected malformed request rejection");}
Bytes unhex(std::string_view hex){Bytes out;for(std::size_t i=0;i<hex.size();i+=2){const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};out.push_back(std::uint8_t(digit(hex[i])*16+digit(hex[i+1])));}return out;}
Bytes capabilities(){return unhex("7244434301000d0044332211");}
void initialize(Protocol& p){
    p.start(0x11223344);check(p.drain()==std::vector<Bytes>{unhex("72446e4901000d0044332211")},"server announce golden vector");
    p.receive(capabilities());
    p.receive(unhex("72444e43010000000000000006000000700063000000"));
    const auto reply=p.drain();check(reply.size()==2,"server capabilities and ID confirmation");
    check(reply[1]==capabilities(),"client ID echoed without treating it as an identity");
    auto cap=reply[0];cap[2]=0x50;cap[3]=0x43;p.receive(cap);
    check(p.ready() && p.drain()==std::vector<Bytes>{unhex("72444c55")},"capability handshake and user logged on");
}
Device announce(Protocol& p,unsigned id=7){
    Writer d;d.le32(1).le32(8).le32(id).raw({'D','A','T','A',0,0,0,0}).le32(0);p.receive(pdu(0x4441,d.bytes()));
    const auto replies=p.drain();Reader ack(replies.at(0));check(ack.le16()==0x4472 && ack.le16()==0x6472 && ack.le32()==id && ack.le32()==0,"drive accepted");ack.end();
    const auto devices=p.devices();return *std::find_if(devices.begin(),devices.end(),[&](const auto& device){return device.key.id==id;});
}
struct WireRequest {unsigned device,file,completion,major,minor;Bytes body;};
WireRequest consume(Protocol& p){
    auto packets=p.drain();check(packets.size()==1,"one pending wire operation");Reader in(packets[0]);
    check(in.le16()==0x4472 && in.le16()==0x4952,"I/O request header");
    WireRequest r{in.le32(),in.le32(),in.le32(),in.le32(),in.le32(),{}};auto body=in.take(in.remaining());r.body.assign(body.begin(),body.end());return r;
}
void completion(Protocol& p,const WireRequest& r,unsigned status,View body={}){Writer out;out.le32(r.device).le32(r.completion).le32(status).raw(body);p.receive(pdu(0x4943,out.bytes()));}
std::uint64_t open(Protocol& p,Device device,unsigned ticket,unsigned remote=99,unsigned access=0x80000000,bool directory=false){
    Request r;r.device=device.key;r.ticket=ticket;r.path="folder/日本.txt";r.access=access;r.options|=directory?1:0x40;
    check(p.submit(r),"open accepted");const auto wire=consume(p);check(wire.major==0 && wire.file==0 && wire.device==device.key.id,"open request routing");
    Reader body(wire.body);check(body.le32()==access && read_u64(body)==0,"create access and allocation");body.skip(8);check(body.le32()==1,"FILE_OPEN disposition");body.skip(4);const auto length=body.le32();
    check(from_utf16le(body.take(length))=="\\folder\\日本.txt","UTF-16 drive rooted path");body.end();
    Writer reply;reply.le32(remote);completion(p,wire,0,reply.bytes());auto result=p.take_replies();check(result.size()==1 && result[0].ticket==ticket && result[0].status==0 && result[0].handle,"create response and local handle");return result[0].handle;
}
void protocol(){
    Protocol p;initialize(p);const auto device=announce(p);const auto handle=open(p,device,1);
    Request read;read.ticket=2;read.device=device.key;read.operation=Operation::read;read.handle=handle;read.offset=(std::uint64_t(1)<<40)+3;read.length=7;
    check(p.submit(read),"read submission");const auto r=consume(p);check(r.major==3 && r.file==99,"read uses remote file ID");Reader b(r.body);
    check(b.le32()==7 && read_u64(b)==read.offset && b.remaining()==20,"64-bit read offset and padding");
    auto second=read;second.ticket=3;check(!p.submit(second),"one outstanding operation per file");
    Writer data;data.le32(3).raw({1,2,3});completion(p,r,0,data.bytes());auto result=p.take_replies();check(result[0].data==Bytes({1,2,3}) && result[0].transferred==3,"short read surfaced exactly");
    auto bad=read;bad.ticket=4;bad.operation=Operation::write;bad.length=1;bad.data={9};
    check(p.submit(bad) && p.drain().empty() && p.take_replies()[0].status==denied,"readonly prevents writes on wire");
    bad.operation=Operation::open;bad.access=0x40000000;bad.path="x";check(p.submit(bad) && p.take_replies()[0].status==denied,"readonly rejects write intent opens");
    check(p.submit(second),"read slot reused");const auto outstanding=consume(p);
    Writer remove;remove.le32(1).le32(device.key.id);p.receive(pdu(0x444d,remove.bytes()));
    check(p.devices().empty() && p.take_replies()[0].status==removed && p.in_flight()==1,"unplug cancels native waiter and quarantines completion");
    const auto replacement=announce(p,device.key.id);check(replacement.key.generation!=device.key.generation,"replug produces new identity");
    Request stale=read;stale.ticket=5;check(p.submit(stale) && p.take_replies()[0].status==removed,"old device generation rejected");
    completion(p,outstanding,0,data.bytes());check(p.take_replies().empty() && p.in_flight()==0,"late unplugged reply discarded");
    stale.device=replacement.key;check(p.submit(stale) && p.take_replies()[0].status==invalid_handle,"old local file cookie rejected after replug");
    rejects([&]{completion(p,outstanding,0,data.bytes());});
    const auto new_handle=open(p,replacement,6,99);
    Request close;close.device=replacement.key;close.ticket=7;close.operation=Operation::close;close.handle=new_handle;
    check(p.submit(close),"close submission");auto c=consume(p);check(c.major==2 && c.body==Bytes(32),"close request framing");completion(p,c,0,Bytes(5));
    check(p.take_replies()[0].status==success,"close response padding tolerated");
    close.ticket=8;check(p.submit(close) && p.take_replies()[0].status==invalid_handle,"closed handle cannot be reused");
}
void mutations(){
    Limits limits;limits.writable=true;limits.outstanding=1;Protocol p(limits);initialize(p);auto device=announce(p);auto handle=open(p,device,1,13,0xc0010103);
    Request r;r.device=device.key;r.handle=handle;r.ticket=2;r.operation=Operation::write;r.data={1,2,3};r.length=3;r.offset=10;
    check(p.submit(r),"explicit writable request");auto first=consume(p);check(first.completion==1 && first.major==4,"completion ID reused after retired open");
    Writer out;out.le32(2).u8(0);completion(p,first,0,out.bytes());check(p.take_replies()[0].transferred==2,"partial write count");
    r.ticket=3;r.operation=Operation::set_information;r.information=13;r.data.clear();check(p.submit(r),"delete disposition request");auto deletion=consume(p);
    check(deletion.completion==first.completion && deletion.major==6 && deletion.body==unhex("0d00000000000000000000000000000000000000000000000000000000000000"),"implied-delete empty buffer golden vector");
    completion(p,deletion,0,Bytes(4));(void)p.take_replies();
    r.ticket=4;r.information=10;r.data=rename_information("new/名前.txt",true);check(p.submit(r),"rename submission");auto rename=consume(p);
    Reader body(rename.body);check(body.le32()==10 && body.le32()==r.data.size(),"rename class and size");body.skip(24);check(body.u8()==1 && body.u8()==0,"RDP rename has byte root directory, not native pointer");
    auto length=body.le32();check(from_utf16le(body.take(length),false)=="\\new\\名前.txt","rename Unicode target");body.end();
    out=Writer();out.le32(std::uint32_t(r.data.size()));completion(p,rename,0,out.bytes());(void)p.take_replies();
    r.ticket=5;r.operation=Operation::read;r.length=3;r.data.clear();const auto before=Clock::now();check(p.submit(r,before),"timeout fixture");consume(p);
    rejects([&]{p.tick(before+limits.timeout);});
}
void bounds(){
    for(const std::string_view path:{"/absolute","../parent","ok/../bad","\\unc","name:stream","a//b","x/","x.","x ","a*"})rejects([&]{(void)wire_path(path);});
    check(wire_path("")=="\\" && wire_path("a/b",true)=="\\a\\b\\*","safe root and directory wildcard");
    Request r;r.operation=Operation::read;r.length=65537;rejects([&]{(void)request_body(r,65536);});
    r.length=1;r.offset=0xffffffffffffffffULL;rejects([&]{(void)request_body(r,65536);});
    r.operation=Operation::set_information;r.information=13;r.data={1};rejects([&]{(void)request_body(r,65536);});
    Writer entry;entry.le32(0).le32(0);for(int i=0;i<4;++i)write_u64(entry,123);write_u64(entry,42);write_u64(entry,4096);entry.le32(0x80);const auto name=utf16le("世界.txt",false);entry.le32(std::uint32_t(name.size())).raw(name);
    auto decoded=directory_information(entry.bytes());check(decoded.size()==1 && decoded[0].name=="世界.txt" && decoded[0].info.size==42,"directory record decoding");
    for(std::size_t i=1;i<entry.size();++i)rejects([&]{(void)directory_information(View(entry.bytes()).first(i));});
    auto evil=entry.bytes();evil[0]=1;rejects([&]{(void)directory_information(evil);});
    Protocol p;initialize(p);
    Writer announce_bad;announce_bad.le32(2).le32(8).le32(1).raw({'x',':','y',0,0,0,0,0}).le32(0).le32(4).le32(2).raw({'P','R','N',0,0,0,0,0}).le32(0);
    p.receive(pdu(0x4441,announce_bad.bytes()));check(p.devices().empty(),"invalid DOS name and unsupported printer never mounted");auto replies=p.drain();check(replies.size()==2,"explicit device policy rejections");
    std::mt19937 random(0x52445044);
    for(unsigned trial=0;trial<20000;++trial){Bytes b(random()%512);for(auto& byte:b)byte=std::uint8_t(random());try{Protocol value;value.start();value.receive(b);}catch(const ProtocolError&){}try{(void)directory_information(b);}catch(const ProtocolError&){} }
}
}
int main(){try{protocol();mutations();bounds();std::cout<<"PASS: "<<checks<<" RDPDR checks; 40,000 seeded malformed decodes; handshake, generations, completion reuse, read-only policy, bounded I/O, partial completions, Unicode metadata and removal quarantine\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
