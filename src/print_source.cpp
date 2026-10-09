#include <algorithm>
#include "lrdp/printing/native.hpp"
#include <array>
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>

namespace lrdp::printing {
namespace {
constexpr int seals=F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE|F_SEAL_SEAL;
class SealedSource final : public Source {
    UniqueFd fd_;
    std::uint64_t size_;
public:
    explicit SealedSource(UniqueFd fd):fd_(std::move(fd)) {
        const int bits=fcntl(fd_.get(),F_GET_SEALS);
        require(bits>=0 && (bits&seals)==seals,"printer source must be an immutable sealed memfd");
        struct stat st{};require(fstat(fd_.get(),&st)==0 && S_ISREG(st.st_mode) && st.st_size>0 && std::uint64_t(st.st_size)<=Jobs::byte_limit,"invalid sealed print source size/type");
        size_=std::uint64_t(st.st_size);
    }
    std::uint64_t size()const override{return size_;}
    Bytes read(std::uint64_t offset,std::uint32_t length)const override {
        require(length<=65536 && offset<=size_ && length<=size_-offset,"sealed printer source range exceeds quota");
        Bytes bytes(length);std::size_t done=0;
        while(done<bytes.size()) {
            const auto n=pread(fd_.get(),bytes.data()+done,bytes.size()-done,off_t(offset+done));
            if(n<0 && errno==EINTR)continue;
            require(n>0,"sealed print snapshot read failed");done+=std::size_t(n);
        }
        return bytes;
    }
};
}
std::shared_ptr<const Source> sealed_source(UniqueFd fd){return std::make_shared<SealedSource>(std::move(fd));}
UniqueFd snapshot_file(const std::string& path) {
    require(!path.empty() && path.find('\0')==std::string::npos,"invalid print source path");
    UniqueFd input(open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));require(bool(input),"cannot open printer-ready input file");
    struct stat before{};require(fstat(input.get(),&before)==0 && S_ISREG(before.st_mode) && before.st_size>0 &&
        std::uint64_t(before.st_size)<=Jobs::byte_limit,"print source must be a nonempty regular file at most64MiB");
    UniqueFd output(memfd_create("lrdp-print",MFD_CLOEXEC|MFD_ALLOW_SEALING));require(bool(output),"cannot create immutable print snapshot");
    require(fchmod(output.get(),0600)==0,"cannot protect print snapshot");
    std::array<std::uint8_t,65536> bytes{};off_t done=0;
    while(done<before.st_size) {
        const auto n=pread(input.get(),bytes.data(),std::min<std::uint64_t>(bytes.size(),std::uint64_t(before.st_size-done)),done);
        if(n<0 && errno==EINTR)continue;
        require(n>0,"print source changed or became unreadable");std::size_t written=0;
        while(written<std::size_t(n)) {
            const auto size=write(output.get(),bytes.data()+written,std::size_t(n)-written);
            if(size<0 && errno==EINTR)continue;
            require(size>0,"cannot write print snapshot");written+=std::size_t(size);
        }
        done+=n;
    }
    struct stat after{};require(fstat(input.get(),&after)==0 && after.st_size==before.st_size &&
        after.st_mtim.tv_sec==before.st_mtim.tv_sec && after.st_mtim.tv_nsec==before.st_mtim.tv_nsec &&
        after.st_ctim.tv_sec==before.st_ctim.tv_sec && after.st_ctim.tv_nsec==before.st_ctim.tv_nsec,"print source mutated during snapshot");
    require(fcntl(output.get(),F_ADD_SEALS,seals)==0,"cannot seal print snapshot");return output;
}
} // namespace lrdp::printing
