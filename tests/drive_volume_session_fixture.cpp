#include "lrdp/session.hpp"
#include "lrdp/drive/filesystem.hpp"
#include "lrdp/transport.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <sys/socket.h>
#include <thread>

using namespace lrdp;
namespace {
std::string text(const Bytes& bytes) { return {bytes.begin(),bytes.end()}; }
void native_work(drive::Bridge& bridge, const std::string& mode) {
    const auto deadline = drive::Clock::now()+std::chrono::seconds(10);
    while (bridge.devices().empty()) {
        require(!bridge.stopped() && drive::Clock::now()<deadline,"volume device was not announced");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    drive::Filesystem fs(bridge);
    const auto root = fs.list("/"); require(root.size()==1,"expected one redirected drive");
    const auto path = "/"+root[0].name;
    struct statvfs native{};
    if (mode=="denied") {
        try { fs.statfs(path,native); } catch (const drive::IoError& e) {
            require(e.code().value()==EACCES,"metadata denial must propagate to native caller");
            require(native.f_blocks==0,"failed metadata must not publish partial capacity"); return;
        }
        throw ProtocolError("metadata access denial was hidden");
    }
    if (mode=="malformed") {
        (void)fs.volume_details(path); throw ProtocolError("malformed metadata reached native worker");
    }
    fs.statfs(path,native);
    require(native.f_frsize==4096 && native.f_blocks==(1ULL<<32) && native.f_bavail==123456,
            "64-bit quota-aware native capacity mismatch");
    require(bool(native.f_flag & ST_RDONLY)==(mode!="writable"),"volume/device readonly flag not combined with local policy");
    const auto fallback = fs.space(path+"/hello.txt");
    require(!fallback.quota_aware && fallback.total_units==(1ULL<<33) && fallback.caller_free_units==777,
            "explicitly unsupported full-size query did not fall back exactly once");
    if (mode=="unsupported") {
        const auto info = fs.volume_details(path);
        require(!info.identity && !info.attributes && !info.device && fs.listxattr(path).empty(),"unsupported metadata must be absent");
        try { (void)fs.getxattr(path,"user.lrdp.volume.label"); }
        catch (const drive::IoError& e) { require(e.code().value()==ENODATA,"unsupported metadata xattr errno"); return; }
        throw ProtocolError("unsupported metadata was fabricated");
    }
    const auto info = fs.volume_details(path);
    require(info.identity && info.attributes && info.device,"missing complete volume metadata");
    require(info.identity->label=="Client 日本語 🚀 " && info.identity->serial==0x89abcdef && info.identity->supports_objects,
            "17-byte RDP identity header/Unicode label mismatch");
    require(info.attributes->filesystem=="NTFS" && info.attributes->max_component_utf16==255 && info.device->type==7,
            "filesystem/device metadata mismatch");
    require(text(fs.getxattr(path,"user.lrdp.volume.label"))=="Client 日本語 🚀 ","native label annotation");
    require(text(fs.getxattr(path,"user.lrdp.volume.filesystem"))=="NTFS","native filesystem annotation");
    require(text(fs.getxattr(path,"user.lrdp.volume.serial"))=="0x89abcdef","native serial annotation");
    const auto names=fs.listxattr(path);
    require(std::count(names.begin(),names.end(),0)==8,"native volume annotation list");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==4,"usage: volume fixture certificate key mode");
        std::signal(SIGPIPE,SIG_IGN); const std::string mode=argv[3];
        TlsContext context(argv[1],argv[2]); Socket listener(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0));
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(listener.get(),1)==0,"volume fixture listener");
        socklen_t length=sizeof(address); require(getsockname(listener.get(),reinterpret_cast<sockaddr*>(&address),&length)==0,"volume fixture port");
        std::cout<<"READY "<<ntohs(address.sin_port)<<'\n'<<std::flush;
        Socket peer(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC)); require(peer.get()>=0,"fixture accept");
        const auto negotiation=parse_negotiation(read_negotiation(peer.get())); require(negotiation.protocols&1,"fixture TLS");
        write_raw(peer.get(),negotiation_reply(1,false,1)); TlsStream stream(peer.get(),context);
        drive::Limits limits; limits.writable=mode=="writable" || mode=="device-readonly";
        auto bridge=std::make_shared<drive::Bridge>(limits);
        Session session(make_demo_desktop(),1,1,{},false); session.configure_drives(bridge);
        std::atomic<bool> done=false; std::exception_ptr worker_error;
        std::thread worker([&] {
            try { native_work(*bridge,mode); } catch (...) { worker_error=std::current_exception(); }
            done.store(true);
        });
        try {
            const auto deadline=drive::Clock::now()+std::chrono::seconds(30);
            while (!done.load()) {
                require(drive::Clock::now()<deadline,"volume session deadline"); stream.pump(1);
                while (auto packet=stream.packet()) { session.receive(*packet); stream.enqueue(session.drain()); }
                session.tick(false,false); stream.enqueue(session.drain());
            }
        } catch (...) { bridge->disconnect(ENOTCONN); worker.join(); throw; }
        worker.join(); if (worker_error) std::rethrow_exception(worker_error);
        std::cout<<"PASS: real TLS/RDPDR volume capacity, metadata, annotations and policy: "<<mode<<'\n';
    } catch (const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
