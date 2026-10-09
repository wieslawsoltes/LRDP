#include "lrdp/session/broker.hpp"
#include <openssl/crypto.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

namespace lrdp::persistent {
namespace {
constexpr unsigned magic=0x3152534c, maximum=131072;
constexpr auto lease_deadline=std::chrono::seconds(30);
constexpr auto reply_deadline=std::chrono::seconds(10);
struct Scrub { Bytes& bytes; ~Scrub(){OPENSSL_cleanse(bytes.data(),bytes.size());} };
void text(Writer& out,std::string_view s) {
    require(s.size()<=4096 && s.find('\0')==std::string_view::npos,"broker string exceeds limit");
    out.le16(unsigned(s.size())).raw(View(reinterpret_cast<const std::uint8_t*>(s.data()),s.size()));
}
std::string text(Reader& in) {
    const auto n=in.le16();require(n<=4096,"broker string exceeds limit");auto bytes=in.take(n);
    require(std::find(bytes.begin(),bytes.end(),0)==bytes.end(),"broker string contains NUL");return {bytes.begin(),bytes.end()};
}
UniqueFd directory_fd(const std::string& directory) {
    require(!directory.empty() && directory.front()=='/',"broker directory must be absolute");
    UniqueFd fd(open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));struct stat info{};
    require(fd && fstat(fd.get(),&info)==0 && info.st_uid==geteuid() && (info.st_mode&0777)==0700,
            "broker directory must be an existing owner-controlled 0700 directory");return fd;
}
sockaddr_un address(const std::string& path) {
    sockaddr_un a{};a.sun_family=AF_UNIX;require(path.size()<sizeof(a.sun_path),"broker socket path is too long");
    std::memcpy(a.sun_path,path.c_str(),path.size()+1);return a;
}
void peer_owner(int fd) {
    ucred peer{};socklen_t n=sizeof(peer);
    require(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n)==0 && n==sizeof(peer) && peer.uid==geteuid(),
            "broker peer has a different Unix owner");
}
void await(int fd,short events,Time deadline) {
    for(;;) {
        require(Clock::now()<deadline,"broker request deadline exceeded");pollfd p{fd,events,0};
        const int n=::poll(&p,1,50);if(n<0 && errno==EINTR)continue;
        require(n>=0 && !(p.revents&(POLLERR|POLLNVAL)),"broker socket failed");
        if(p.revents&(events|POLLHUP))return;
    }
}
std::optional<Bytes> receive(int fd) {
    std::array<std::uint8_t,maximum> bytes{};
    struct Wipe { decltype(bytes)& value; ~Wipe(){OPENSSL_cleanse(value.data(),value.size());} } wipe{bytes};
    iovec part{bytes.data(),bytes.size()};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
    const auto n=recvmsg(fd,&message,MSG_DONTWAIT);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return {};
    require(n>0 && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)),"broker packet truncated or disconnected");
    return Bytes(bytes.begin(),bytes.begin()+n);
}
bool send_packet(int fd,View data) {
    const auto n=send(fd,data.data(),data.size(),MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return false;
    require(n==std::ptrdiff_t(data.size()),"broker packet write failed");return true;
}
Bytes frame(unsigned operation,View body) {Writer out(maximum);out.le32(magic).le32(operation).raw(body);return std::move(out).finish();}
}
struct Broker::Impl {
    struct Peer {
        UniqueFd fd;std::uint64_t owner;std::optional<std::uint32_t> lease;
        Bytes output;Time deadline;bool committed=false,closing=false;
        ~Peer(){OPENSSL_cleanse(output.data(),output.size());}
        Peer(UniqueFd socket,std::uint64_t id):fd(std::move(socket)),owner(id),deadline(Clock::now()+lease_deadline){}
    };
    std::string path;
    UniqueFd directory,listener;
    struct stat bound{};
    bool bound_socket=false;
    HeadlessOptions options;
    Registry registry;
    std::uint64_t next_owner=0;
    std::map<int,std::unique_ptr<Peer>> peers;
    std::map<std::uint32_t,std::unique_ptr<HeadlessServer>> desktops;
    Impl(std::string root,HeadlessOptions opts,unsigned capacity,std::chrono::seconds retention)
      :path(root+"/broker.sock"),directory(directory_fd(root)),options(std::move(opts)),registry(capacity,retention) {
        require(getuid()!=0 && geteuid()==getuid(),"session broker must run as an ordinary Unix user");
        listener.reset(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0));require(bool(listener),"cannot create broker socket");
        const auto a=address("/proc/self/fd/"+std::to_string(directory.get())+"/broker.sock");
        const auto mask=umask(077);const int result=bind(listener.get(),reinterpret_cast<const sockaddr*>(&a),sizeof(a));umask(mask);
        require(result==0,"cannot bind broker socket; an existing socket is never removed automatically");
        // If setup fails after binding, the Impl destructor will not run.
        // Remove only the socket we just created, never an existing endpoint.
        if(fstatat(directory.get(),"broker.sock",&bound,AT_SYMLINK_NOFOLLOW)!=0) {
            unlinkat(directory.get(),"broker.sock",0);
            throw ProtocolError("cannot inspect newly bound broker socket");
        }
        bound_socket=true;
        if(listen(listener.get(),16)!=0) {remove_socket();throw ProtocolError("cannot listen on broker socket");}
    }
    void remove_socket() noexcept {
        struct stat current{};
        if(bound_socket && fstatat(directory.get(),"broker.sock",&current,AT_SYMLINK_NOFOLLOW)==0 &&
           current.st_dev==bound.st_dev && current.st_ino==bound.st_ino)unlinkat(directory.get(),"broker.sock",0);
        bound_socket=false;
    }
    ~Impl(){peers.clear();desktops.clear();remove_socket();}
    void drop(int fd) {
        auto it=peers.find(fd);if(it==peers.end())return;
        if(auto id=registry.release(it->second->owner,Clock::now()))desktops.erase(*id);
        peers.erase(it);
    }
    void terminate(std::uint32_t id) {
        require(registry.erase(id),"unknown persistent session");
        for(auto& [fd,peer]:peers)if(peer->lease==id){shutdown(fd,SHUT_RDWR);peer->closing=true;}
        desktops.erase(id);
    }
    Bytes dispatch(Peer& peer,View bytes) {
        Reader in(bytes);require(in.le32()==magic,"broker protocol mismatch");const auto op=in.le32();Writer out(maximum);out.le32(0);
        if(op==1) {
            require(!peer.lease,"broker connection already owns a desktop");auto principal=text(in);
            const auto length=in.le16();require(length==0 || length==28,"invalid broker reconnect cookie");
            std::optional<ReconnectCookie> proof;if(length)proof=decode_reconnect_cookie(in.take(length));in.end();
            auto lease=registry.acquire(std::move(principal),proof,peer.owner,Clock::now());peer.lease=lease.id;
            if(lease.created) {
                auto server=std::make_unique<HeadlessServer>(options);server->start_desktop(options);
                desktops.emplace(lease.id,std::move(server));
            }
            auto& server=*desktops.at(lease.id);server.check();out.le32(lease.id).u8(lease.created?1:0);
            text(out,server.display());text(out,server.authority());
        } else if(op==2) {
            const auto retain=in.u8();require(retain<=1 && peer.lease,"invalid broker commit");in.end();
            const auto cookie=registry.commit(peer.owner,retain!=0,Clock::now());peer.committed=true;
            if(cookie)out.raw(encode_reconnect_cookie(*cookie));
        } else if(op==3) {
            in.end();out.raw(encode_reconnect_cookie(registry.rotate(peer.owner,Clock::now())));
        } else if(op==4) {
            require(!peer.lease,"admin command on leased connection");in.end();const auto entries=registry.list();out.le32(unsigned(entries.size()));
            for(const auto& e:entries){out.le32(e.id).u8(e.attached?1:0);text(out,e.principal);}
        } else if(op==5) {
            require(!peer.lease,"admin command on leased connection");const auto id=in.le32();in.end();terminate(id);
        } else throw ProtocolError("unknown broker operation");
        return std::move(out).finish();
    }
    void poll(int timeout) {
        for(auto id:registry.expire(Clock::now()))desktops.erase(id);
        std::vector<std::uint32_t> dead;
        for(const auto& [id,desktop]:desktops) {try{desktop->check();}catch(const ProtocolError&){dead.push_back(id);}}
        for(auto id:dead)terminate(id);
        std::vector<pollfd> fds{{listener.get(),POLLIN,0}};
        for(const auto& [fd,peer]:peers)fds.push_back({fd,short(peer->output.empty()?POLLIN:POLLOUT),0});
        const int rc=::poll(fds.data(),fds.size(),timeout);
        if(rc<0 && errno==EINTR)return;
        require(rc>=0,"broker poll failed");
        if(fds[0].revents&POLLIN) {
            UniqueFd fd(accept4(listener.get(),nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC));
            if(fd && peers.size()<128 && next_owner<std::numeric_limits<std::uint64_t>::max()) {
                try {peer_owner(fd.get());const auto key=fd.get();peers.emplace(key,std::make_unique<Peer>(std::move(fd),++next_owner));}catch(const ProtocolError&){}
            }
        }
        for(std::size_t index=1;index<fds.size();++index) {
            const auto p=fds[index];auto it=peers.find(p.fd);if(it==peers.end())continue;auto& peer=*it->second;
            try {
                if(p.revents&(POLLHUP|POLLERR|POLLNVAL)) {drop(p.fd);continue;}
                if((!peer.committed || !peer.output.empty()) && Clock::now()>=peer.deadline) {drop(p.fd);continue;}
                if(p.revents&POLLIN) {
                    if(auto message=receive(p.fd)) {
                        Scrub scrub{*message};
                        try {peer.output=dispatch(peer,*message);}
                        catch(const std::exception&) {peer.output={1,0,0,0};peer.closing=true;}
                        peer.deadline=Clock::now()+reply_deadline;
                    }
                }
                if(!peer.output.empty() && send_packet(p.fd,peer.output)) {
                    OPENSSL_cleanse(peer.output.data(),peer.output.size());peer.output.clear();
                    if(peer.closing){drop(p.fd);continue;}
                    if(!peer.committed)peer.deadline=Clock::now()+lease_deadline;
                }
            } catch(const ProtocolError&) {drop(p.fd);}
        }
    }
};
Broker::Broker(std::string directory,HeadlessOptions options,unsigned capacity,std::chrono::seconds retention)
 :impl_(std::make_unique<Impl>(std::move(directory),std::move(options),capacity,retention)){}
Broker::~Broker()=default;
void Broker::poll(int timeout){impl_->poll(timeout);}
const std::string& Broker::socket_path()const{return impl_->path;}
BrokerClient::BrokerClient(const std::string& path) {
    const std::filesystem::path p(path);require(p.filename()=="broker.sock","invalid broker socket filename");
    auto directory=directory_fd(p.parent_path().string());struct stat s{};
    require(fstatat(directory.get(),"broker.sock",&s,AT_SYMLINK_NOFOLLOW)==0 && S_ISSOCK(s.st_mode) &&
        s.st_uid==geteuid() && (s.st_mode&0077)==0,"broker socket is not owner-only");
    socket_.reset(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0));require(bool(socket_),"cannot create broker connection");
    const auto a=address("/proc/self/fd/"+std::to_string(directory.get())+"/broker.sock");
    require(connect(socket_.get(),reinterpret_cast<const sockaddr*>(&a),sizeof(a))==0,"cannot connect to session broker");peer_owner(socket_.get());
}
Bytes BrokerClient::transact(unsigned op,View payload) {
    auto request=frame(op,payload);Scrub scrub{request};const auto deadline=Clock::now()+std::chrono::seconds(20);
    while(!send_packet(socket_.get(),request))await(socket_.get(),POLLOUT,deadline);
    for(;;) {
        if(auto result=receive(socket_.get())) {
            Reader in(*result);require(in.le32()==0,"session broker rejected request");
            result->erase(result->begin(),result->begin()+4);return std::move(*result);
        }
        await(socket_.get(),POLLIN,deadline);
    }
}
Attachment BrokerClient::acquire(std::string_view principal,const std::optional<ReconnectCookie>& verifier) {
    require(!acquired_,"broker lease already acquired");Writer out(maximum);text(out,principal);
    out.le16(verifier?28:0);if(verifier)out.raw(encode_reconnect_cookie(*verifier));
    auto result=transact(1,out.bytes());Scrub scrub{result};Reader in(result);Attachment a{};
    a.id=in.le32();const auto created=in.u8();require(a.id && created<=1,"invalid attachment response");a.created=created!=0;
    a.display=text(in);a.authority=text(in);in.end();
    require(a.display.starts_with(':') && a.authority.starts_with('/'),"invalid broker desktop address");acquired_=true;return a;
}
std::optional<ReconnectCookie> BrokerClient::commit(bool retain) {
    const std::array<std::uint8_t,1> value{std::uint8_t(retain)};auto bytes=transact(2,value);Scrub scrub{bytes};
    if(!retain){require(bytes.empty(),"unexpected nonpersistent cookie");return {};}
    return decode_reconnect_cookie(bytes);
}
ReconnectCookie BrokerClient::rotate(){auto bytes=transact(3);Scrub scrub{bytes};return decode_reconnect_cookie(bytes);}
void BrokerClient::check()const {
    pollfd p{socket_.get(),POLLIN,0};const int n=::poll(&p,1,0);
    require(n==0 || (n<0 && errno==EINTR),"persistent desktop lease was revoked");
}
std::vector<Summary> BrokerClient::list() {
    const auto bytes=transact(4);Reader in(bytes);const auto count=in.le32();require(count<=64,"broker session count exceeded");
    std::vector<Summary> result;for(unsigned i=0;i<count;++i){auto id=in.le32();auto state=in.u8();require(state<=1,"invalid lease state");result.push_back({id,state!=0,text(in)});}in.end();return result;
}
void BrokerClient::terminate(std::uint32_t id){Writer out;out.le32(id);const auto bytes=transact(5,out.bytes());require(bytes.empty(),"invalid termination response");}
} // namespace lrdp::persistent
