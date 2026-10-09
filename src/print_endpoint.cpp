#include <array>
#include "print_native_internal.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <openssl/rand.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>

namespace lrdp::printing {
struct NativeEndpoint::Impl {
    struct Peer {
        UniqueFd fd;
        std::uint64_t cookie=0;
        Bytes outgoing;
        drive::Clock::time_point deadline=drive::Clock::now()+std::chrono::seconds(5);
    };
    UniqueFd root,folder,listener;
    std::string name,path;
    ino_t socket_inode=0,folder_inode=0;
    dev_t socket_device=0,folder_device=0;
    bool stopped=false;
    std::uint64_t next_cookie=1;
    Jobs jobs;
    std::vector<Peer> peers;
    ~Impl(){close();}
    void close()noexcept {
        stopped=true;peers.clear();jobs.discard();listener.reset();
        // Do not unlink a replacement planted at the published name.
        struct stat st{};
        if(folder && socket_inode && fstatat(folder.get(),"print.sock",&st,AT_SYMLINK_NOFOLLOW)==0 && st.st_ino==socket_inode && st.st_dev==socket_device)
            (void)unlinkat(folder.get(),"print.sock",0);
        if(root && folder_inode && !name.empty() && fstatat(root.get(),name.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0 && st.st_ino==folder_inode && st.st_dev==folder_device)
            (void)unlinkat(root.get(),name.c_str(),AT_REMOVEDIR);
        folder.reset();root.reset();
    }
    void start(const std::string& location) {
        require(geteuid()!=0,"run native printer submission as the desktop's ordinary Unix user");
        root=local::directory(location,true);
        std::array<unsigned char,12> random{};require(RAND_bytes(random.data(),int(random.size()))==1,"cannot generate print endpoint identity");
        constexpr char hex[]="0123456789abcdef";name="print-";for(auto b:random){name+=hex[b>>4];name+=hex[b&15];}
        path=location+(location.back()=='/'?"":"/")+name+"/print.sock";
        require(path.size()<sizeof(sockaddr_un::sun_path),"print directory path is too long for a local socket");
        require(mkdirat(root.get(),name.c_str(),0700)==0,"cannot create private printer endpoint directory");
        // Record the created inode before the next fallible open, so constructor
        // failure still retires exactly our directory, never a replacement.
        struct stat created{};require(fstatat(root.get(),name.c_str(),&created,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(created.st_mode),"cannot inspect created printer directory");
        folder_inode=created.st_ino;folder_device=created.st_dev;
        folder.reset(openat(root.get(),name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));require(bool(folder),"cannot pin printer endpoint directory");
        struct stat st{};require(fstat(folder.get(),&st)==0 && st.st_uid==geteuid() && (st.st_mode&0777)==0700,"unsafe printer endpoint directory");folder_inode=st.st_ino;folder_device=st.st_dev;
        listener.reset(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0));require(bool(listener),"cannot create printer endpoint socket");
        sockaddr_un address{};address.sun_family=AF_UNIX;const auto bind_path=local::pinned(folder.get(),"print.sock");
        require(bind_path.size()<sizeof(address.sun_path),"pinned print endpoint path overflow");std::memcpy(address.sun_path,bind_path.c_str(),bind_path.size()+1);
        require(bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"cannot bind native printer endpoint");
        require(fstatat(folder.get(),"print.sock",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISSOCK(st.st_mode),"cannot inspect bound printer endpoint");socket_inode=st.st_ino;socket_device=st.st_dev;
        require(fchmodat(folder.get(),"print.sock",0600,0)==0 && listen(listener.get(),16)==0,"cannot protect/listen on printer endpoint");
    }
    void collect() {
        for(const auto& result:jobs.take_results()) {
            auto it=std::find_if(peers.begin(),peers.end(),[&](const Peer& p){return p.cookie==result.cookie;});
            if(it==peers.end())continue;
            Writer body;drive::write_u64(body,result.transferred);
            it->outgoing=local::reply(local::submit,result.status,body.bytes());it->cookie=0;it->deadline=drive::Clock::now()+std::chrono::seconds(5);
        }
    }
    void request(Peer& peer,local::Message message) {
        Reader in(message.bytes);require(in.le32()==0x3150524c,"invalid native print protocol");const auto command=in.le32();
        if(command==local::list) {
            in.end();require(message.descriptors.empty(),"list request must not transfer descriptors");Writer out(local::packet_limit-12);out.le32(unsigned(jobs.devices().size()));
            for(const auto& device:jobs.devices()) {
                out.le32(device.key.id);drive::write_u64(out,device.key.generation);out.le32(device.printer->flags);
                local::text(out,device.printer->name);local::text(out,device.printer->driver);
            }
            peer.outgoing=local::reply(command,0,out.bytes());
        } else if(command==local::submit) {
            const drive::DeviceKey key{in.le32(),drive::read_u64(in)};in.end();require(message.descriptors.size()==1,"submit requires one sealed descriptor");
            require(next_cookie!=std::numeric_limits<std::uint64_t>::max(),"local printer cookies exhausted");
            const auto cookie=next_cookie++;auto source=sealed_source(std::move(message.descriptors[0]));jobs.submit(cookie,key,std::move(source));
            peer.cookie=cookie;peer.deadline=drive::Clock::now()+std::chrono::minutes(6);
        } else throw ProtocolError("unsupported native print command");
    }
    void pump() {
        if(stopped)return;
        collect();
        for(unsigned n=0;n<16 && peers.size()<16;++n) {
            UniqueFd peer(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK));
            if(!peer){if(errno==EINTR)continue;require(errno==EAGAIN||errno==EWOULDBLOCK,"printer accept failed");break;}
            try{local::same_user(peer.get());peers.push_back(Peer{std::move(peer),0,{}});}catch(const ProtocolError&){}
        }
        for(auto it=peers.begin();it!=peers.end();) {
            bool erase=false;
            try {
                if(drive::Clock::now()>=it->deadline)erase=true;
                else if(!it->outgoing.empty())erase=local::send(it->fd.get(),it->outgoing);
                else if(auto message=local::receive(it->fd.get())) {
                    if(message->bytes.empty())erase=true;
                    else if(it->cookie)throw ProtocolError("print connection pipelining is unsupported");
                    else {
                        try{request(*it,std::move(*message));}
                        catch(const ProtocolError&){Writer empty;drive::write_u64(empty,0);it->outgoing=local::reply(local::submit,0xc000000d,empty.bytes());}
                    }
                }
            } catch(const ProtocolError&){erase=true;}
            if(erase){if(it->cookie)jobs.cancel(it->cookie);it=peers.erase(it);}else ++it;
        }
    }
};
NativeEndpoint::NativeEndpoint(const std::string& root):impl_(std::make_unique<Impl>()){impl_->start(root);}
NativeEndpoint::~NativeEndpoint()=default;
const std::string& NativeEndpoint::path()const{return impl_->path;}
void NativeEndpoint::publish(std::vector<drive::Device> devices){if(!impl_->stopped)impl_->jobs.publish(std::move(devices));}
void NativeEndpoint::pump(){impl_->pump();}
std::vector<drive::Request> NativeEndpoint::take_requests(std::uint32_t chunk){return impl_->stopped?std::vector<drive::Request>{}:impl_->jobs.take_requests(chunk);}
void NativeEndpoint::complete(const drive::Reply& reply){if(!impl_->stopped){impl_->jobs.complete(reply);impl_->collect();}}
void NativeEndpoint::disconnect()noexcept{impl_->close();}
} // namespace lrdp::printing
