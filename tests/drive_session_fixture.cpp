#include "lrdp/session.hpp"
#include "lrdp/drive/filesystem.hpp"
#include "lrdp/transport.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <sys/socket.h>
#include <thread>
using namespace lrdp;
namespace {
Bytes pattern(std::size_t size) { Bytes result(size); for (std::size_t i=0;i<size;++i) result[i]=std::uint8_t(i*17+3); return result; }
template<class F> void error(F action, int expected) {
    try { action(); } catch (const drive::IoError& e) { require(e.code().value()==expected,"unexpected native errno"); return; }
    throw ProtocolError("native filesystem accepted forbidden operation");
}
void native_work(drive::Bridge& bridge, bool writable) {
    const auto deadline=drive::Clock::now()+std::chrono::seconds(10);
    while(bridge.devices().empty()) { require(!bridge.stopped() && drive::Clock::now()<deadline,"drive was not announced"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    drive::Filesystem fs(bridge); const auto root=fs.list("/"); require(root.size()==1,"one exported drive expected");
    const auto prefix="/"+root[0].name;
    require(fs.stat(prefix).directory,"virtual drive root");
    const auto listed=fs.list(prefix); require(listed.size()==3,"directory enumeration");
    const auto source=prefix+"/日本語.bin"; auto file=fs.open(source,O_RDONLY);
    const auto expected=pattern(180003);
    require(fs.read(file,0,expected.size())==expected,"native multi-chunk read content mismatch");
    require(fs.read(file,180000,20)==Bytes(expected.end()-3,expected.end()),"native short EOF read");
    require(fs.read(file,std::uint64_t(1)<<40,3).empty(),"64-bit EOF read"); fs.close(file);
    const auto info=fs.stat(source); require(info.size==expected.size()&&!info.directory,"native file metadata");
    struct stat st{}; drive::native_stat(info,writable,st); require(st.st_size==off_t(expected.size())&&!(st.st_mode&0111),"nonexecutable native file metadata");
    error([&]{(void)fs.stat(prefix+"/../escape");},EINVAL);
    error([&]{(void)fs.open(source,O_APPEND);},EOPNOTSUPP);
    if(!writable) { error([&]{(void)fs.open(source,O_WRONLY);},EROFS); error([&]{fs.mkdir(prefix+"/new");},EROFS); return; }
    const auto created=prefix+"/new.txt"; file=fs.open(created,O_CREAT|O_EXCL|O_RDWR);
    const auto data=pattern(200007); require(fs.write(file,0,data)==data.size(),"native short-write retry");
    require(fs.read(file,0,data.size())==data,"native write/read roundtrip"); fs.truncate(file,197); fs.close(file);
    require(fs.stat(created).size==197,"remote truncation");
    fs.rename(created,prefix+"/renamed-世界.txt",false);
    require(fs.stat(prefix+"/renamed-世界.txt").size==197,"Unicode native rename");
    fs.mkdir(prefix+"/new-directory"); fs.remove(prefix+"/new-directory",true); fs.remove(prefix+"/renamed-世界.txt",false);
    error([&]{(void)fs.stat(prefix+"/renamed-世界.txt");},ENOENT);
}
}
int main(int argc,char** argv) {
    try {
        require(argc==4,"usage: fixture certificate key readonly|writable"); std::signal(SIGPIPE,SIG_IGN);
        TlsContext context(argv[1],argv[2]); Socket listener(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener.get(),1)==0,"fixture listener");
        socklen_t length=sizeof(address);require(getsockname(listener.get(),reinterpret_cast<sockaddr*>(&address),&length)==0,"fixture port");
        std::cout<<"READY "<<ntohs(address.sin_port)<<'\n'<<std::flush;
        Socket peer(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC));require(peer.get()>=0,"fixture accept");
        const auto negotiation=parse_negotiation(read_negotiation(peer.get()));require(negotiation.protocols&1,"fixture TLS");
        write_raw(peer.get(),negotiation_reply(1,false,1));TlsStream stream(peer.get(),context);
        drive::Limits policy;policy.writable=std::string_view(argv[3])=="writable";
        auto bridge=std::make_shared<drive::Bridge>(policy);
        Session session(make_demo_desktop(),1,1,{},false);session.configure_drives(bridge);
        std::atomic<bool> done=false;std::exception_ptr worker_error;
        std::thread worker([&]{try{native_work(*bridge,policy.writable);}catch(...){worker_error=std::current_exception();}done.store(true);});
        try {
            const auto deadline=drive::Clock::now()+std::chrono::seconds(30);
            while(!done.load()) {
                require(drive::Clock::now()<deadline,"native session test deadline"); stream.pump(1);
                while(auto packet=stream.packet()) { session.receive(*packet);stream.enqueue(session.drain()); }
                session.tick(false,false);stream.enqueue(session.drain());
            }
        } catch(...) {bridge->disconnect(ENOTCONN);worker.join();throw;}
        worker.join();if(worker_error)std::rethrow_exception(worker_error);
        std::cout<<"PASS: native drive enumeration, metadata, Unicode, bounded reads, 64-bit EOF, policy";
        if(policy.writable)std::cout<<", create, partial writes, readback, truncate, rename, mkdir, unlink and rmdir";
        std::cout<<'\n';return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
