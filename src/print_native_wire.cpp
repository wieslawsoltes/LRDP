#include <algorithm>
#include "print_native_internal.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>

namespace lrdp::printing::local {
UniqueFd directory(const std::string& path, bool private_leaf) {
    require(!path.empty() && path[0]=='/' && path.find('\0')==std::string::npos && path.size()<=4096,
            "print directory must be an absolute path");
    UniqueFd fd(open("/", O_RDONLY|O_DIRECTORY|O_CLOEXEC)); require(bool(fd), "cannot open filesystem root");
    for(std::size_t start=1; start<path.size();) {
        const auto end=path.find('/',start); const auto part=path.substr(start,end==std::string::npos?path.size()-start:end-start);
        require(!part.empty() && part!="." && part!="..", "invalid print directory component");
        UniqueFd next(openat(fd.get(),part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        require(bool(next), "cannot open print directory without symlinks"); fd=std::move(next);
        if(end==std::string::npos) break;
        start=end+1;
    }
    struct stat st{}; require(fstat(fd.get(),&st)==0, "cannot inspect print directory");
    if(private_leaf) require(st.st_uid==geteuid() && (st.st_mode&0777)==0700, "print directory must be owned by this user with mode0700");
    return fd;
}
std::string pinned(int fd,std::string_view leaf) { return "/proc/self/fd/"+std::to_string(fd)+"/"+std::string(leaf); }
void same_user(int fd) {
    ucred credentials{}; socklen_t size=sizeof(credentials);
    require(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&size)==0 && size==sizeof(credentials) && credentials.uid==geteuid(),
            "print socket peer belongs to another Unix user");
}
void wait(int fd,short events,drive::Clock::time_point deadline) {
    for(;;) {
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-drive::Clock::now()).count();
        require(left>0, "native print operation deadline exceeded; do not retry uncertain jobs automatically");
        pollfd item{fd,events,0}; const auto rc=poll(&item,1,int(std::min<std::int64_t>(left,1000)));
        if(rc<0 && errno==EINTR) continue;
        require(rc>=0 && !(item.revents&(POLLNVAL|POLLERR)), "native print socket failed");
        if(item.revents&(events|POLLHUP)) return;
    }
}
UniqueFd connect(const std::string& path) {
    require(!path.empty() && path[0]=='/' && path.size()<sizeof(sockaddr_un::sun_path) && path.find('\0')==std::string::npos,
            "invalid native print socket path");
    const auto separator=path.find_last_of('/'); const auto leaf=path.substr(separator+1);
    require(!leaf.empty() && leaf!="." && leaf!="..", "invalid native print socket name");
    auto parent=directory(path.substr(0,separator),true);
    struct stat st{}; require(fstatat(parent.get(),leaf.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISSOCK(st.st_mode) &&
            st.st_uid==geteuid() && (st.st_mode&0777)==0600, "native print socket has unsafe ownership or permissions");
    UniqueFd fd(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0)); require(bool(fd), "cannot create native print connection");
    sockaddr_un address{};address.sun_family=AF_UNIX;const auto resolved=pinned(parent.get(),leaf);
    require(resolved.size()<sizeof(address.sun_path),"pinned print path exceeds socket limit");
    std::memcpy(address.sun_path,resolved.c_str(),resolved.size()+1);
    const auto rc=::connect(fd.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address));
    if(rc<0) {
        require(errno==EINPROGRESS,"cannot connect native print endpoint");
        wait(fd.get(),POLLOUT,drive::Clock::now()+std::chrono::seconds(5));int error=0;socklen_t n=sizeof(error);
        require(getsockopt(fd.get(),SOL_SOCKET,SO_ERROR,&error,&n)==0 && !error,"native print connection failed");
    }
    same_user(fd.get());return fd;
}
std::optional<Message> receive(int fd) {
    std::array<std::uint8_t,packet_limit> bytes{};
    alignas(cmsghdr) std::array<std::uint8_t,CMSG_SPACE(sizeof(int)*8)> control{};
    iovec io{bytes.data(),bytes.size()};msghdr header{};header.msg_iov=&io;header.msg_iovlen=1;header.msg_control=control.data();header.msg_controllen=control.size();
    // Preallocate ownership before recvmsg can install descriptors in this process.
    Message result;result.descriptors.reserve(8);bool invalid=false;
    ssize_t n;
    do {n=recvmsg(fd,&header,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);}while(n<0 && errno==EINTR);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return std::nullopt;
    require(n>=0,"native print receive failed");
    // Own every received descriptor before checking flags, shape or quota.
    for(auto* c=CMSG_FIRSTHDR(&header);c;c=CMSG_NXTHDR(&header,c)) {
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len<CMSG_LEN(0)) {invalid=true;continue;}
        const auto size=c->cmsg_len-CMSG_LEN(0);if(size%sizeof(int))invalid=true;
        for(std::size_t offset=0;offset+sizeof(int)<=size;offset+=sizeof(int)) {
            int value=-1;std::memcpy(&value,CMSG_DATA(c)+offset,sizeof(value));result.descriptors.emplace_back(value);
        }
    }
    require(!invalid && !(header.msg_flags&(MSG_TRUNC|MSG_CTRUNC)),"invalid or truncated native print packet");
    result.bytes.assign(bytes.begin(),bytes.begin()+n);return result;
}
bool send(int fd,View bytes,int descriptor) {
    require(!bytes.empty() && bytes.size()<=packet_limit,"native print packet exceeds quota");
    iovec io{const_cast<std::uint8_t*>(bytes.data()),bytes.size()};msghdr h{};h.msg_iov=&io;h.msg_iovlen=1;
    alignas(cmsghdr) std::array<std::uint8_t,CMSG_SPACE(sizeof(int))> control{};
    if(descriptor>=0) {
        h.msg_control=control.data();h.msg_controllen=control.size();auto* c=CMSG_FIRSTHDR(&h);
        c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));std::memcpy(CMSG_DATA(c),&descriptor,sizeof(descriptor));
    }
    ssize_t n;do{n=sendmsg(fd,&h,MSG_DONTWAIT|MSG_NOSIGNAL);}while(n<0 && errno==EINTR);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return false;
    require(n==ssize_t(bytes.size()),"native print packet was not delivered");return true;
}
Bytes request(unsigned command,View body) {Writer out(packet_limit);out.le32(0x3150524c).le32(command).raw(body);return std::move(out).finish();}
Bytes reply(unsigned command,unsigned status,View body) {Writer out(packet_limit);out.le32(0x3150524c).le32(command|response).le32(status).raw(body);return std::move(out).finish();}
Bytes exchange(int fd,unsigned command,View body,int descriptor,std::chrono::seconds timeout) {
    const auto deadline=drive::Clock::now()+timeout;const auto packet=request(command,body);
    while(!send(fd,packet,descriptor))wait(fd,POLLOUT,deadline);
    for(;;) {
        if(auto m=receive(fd)) {
            require(!m->bytes.empty() && m->descriptors.empty(),"native print endpoint closed or returned an unexpected descriptor; job outcome may be uncertain");
            Reader in(m->bytes);require(in.le32()==0x3150524c && in.le32()==(command|response),"invalid native print response");
            auto rest=in.take(in.remaining());return Bytes(rest.begin(),rest.end());
        }
        wait(fd,POLLIN,deadline);
    }
}
void text(Writer& w,std::string_view value) {require(value.size()<=1024,"printer text exceeds quota");w.le32(unsigned(value.size())).raw(View(reinterpret_cast<const std::uint8_t*>(value.data()),value.size()));}
std::string text(Reader& in) {
    const auto n=in.le32();require(n<=1024,"printer text exceeds quota");const auto b=in.take(n);std::string s(b.begin(),b.end());
    (void)utf16le(s);for(unsigned char c:s)require(c>=32 && c!=127,"printer text contains control characters");return s;
}
} // namespace lrdp::printing::local
