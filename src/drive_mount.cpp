#define FUSE_USE_VERSION 35
#include "lrdp/platform/drive_mount.hpp"
#include "lrdp/platform/unique_fd.hpp"
#include "lrdp/drive/filesystem.hpp"
#include <fuse.h>
#include <openssl/rand.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <map>
#include <thread>
#include <unistd.h>

namespace lrdp {
struct DriveMount::Impl {
    std::shared_ptr<drive::Bridge> bridge;
    drive::Filesystem filesystem;
    UniqueFd root;
    std::string directory, name;
    struct fuse* instance = nullptr;
    std::thread worker;
    std::atomic<bool> stopping = false;
    bool mounted = false;
    std::map<std::uint64_t, drive::Handle> files;
    struct Snapshot { std::vector<drive::Entry> entries; std::size_t bytes; };
    std::map<std::uint64_t, Snapshot> directories;
    std::uint64_t next_handle = 1;
    std::size_t snapshot_bytes = 0;
    explicit Impl(std::shared_ptr<drive::Bridge> value) : bridge(std::move(value)), filesystem(*bridge) {}
    ~Impl() {
        stopping.store(true); bridge->disconnect(ENOTCONN);
        if (instance) {
            fuse_exit(instance);
            if (mounted) fuse_unmount(instance);
        }
        if (worker.joinable()) worker.join();
        if (instance) fuse_destroy(instance);
        if (root && !name.empty()) (void)unlinkat(root.get(), name.c_str(), AT_REMOVEDIR);
    }
    static Impl& self() { return *static_cast<Impl*>(fuse_get_context()->private_data); }
    template<class F> static int protect(F action) noexcept {
        try { return action(self()); }
        catch (const drive::IoError& error) { return -error.code().value(); }
        catch (const std::bad_alloc&) { return -ENOMEM; }
        catch (...) { return -EIO; }
    }
    void capacity() const {
        if (files.size()+directories.size() >= bridge->limits().handles) throw drive::IoError(EMFILE,"native handle quota");
    }
    std::uint64_t allocate_handle() {
        if (next_handle == std::numeric_limits<std::uint64_t>::max()) throw drive::IoError(EOVERFLOW, "native handle identifiers exhausted");
        return next_handle++;
    }
    const drive::Handle& handle(const fuse_file_info* info) const {
        if (!info) throw drive::IoError(EBADF,"missing native file handle");
        const auto found = files.find(info->fh);
        if (found == files.end()) throw drive::IoError(EBADF,"stale native file handle");
        return found->second;
    }
    int open(const char* path, fuse_file_info* info, int extra = 0) {
        capacity(); const auto id = allocate_handle(); auto handle = filesystem.open(path, info->flags|extra);
        try { files.emplace(id, handle); }
        catch (...) { try { filesystem.close(handle); } catch (...) {} throw; }
        info->fh = id; info->direct_io = 1; info->keep_cache = 0; return 0;
    }
    static fuse_operations operations() {
        fuse_operations op{};
        op.init = [](fuse_conn_info* connection, fuse_config* config) -> void* {
            connection->max_write = 65536;
            connection->max_read = 65536;
            connection->max_readahead = 0;
            connection->max_background = std::min(connection->max_background, 16U);
            config->attr_timeout = config->entry_timeout = config->negative_timeout = 0;
            config->direct_io = 1; config->kernel_cache = 0; config->auto_cache = 0; config->use_ino = 0;
            return &self();
        };
        op.getattr = [](const char* path, struct stat* result, fuse_file_info* fi) {
            return protect([&](Impl& s) {
                const auto info = fi && s.files.contains(fi->fh) ? s.filesystem.stat(s.handle(fi)) : s.filesystem.stat(path);
                drive::native_stat(info,s.filesystem.writable(),*result); return 0;
            });
        };
        op.open = [](const char* path, fuse_file_info* info) { return protect([&](Impl& s) { return s.open(path,info); }); };
        op.create = [](const char* path, mode_t, fuse_file_info* info) { return protect([&](Impl& s) { return s.open(path,info,O_CREAT); }); };
        op.release = [](const char*, fuse_file_info* info) {
            return protect([&](Impl& s) { const auto h = s.handle(info); s.files.erase(info->fh); s.filesystem.close(h); return 0; });
        };
        op.read = [](const char*, char* output, std::size_t size, off_t offset, fuse_file_info* info) {
            return protect([&](Impl& s) {
                if (offset<0) return -EINVAL;
                const auto bytes = s.filesystem.read(s.handle(info),std::uint64_t(offset),size);
                std::copy(bytes.begin(),bytes.end(),output); return int(bytes.size());
            });
        };
        op.write = [](const char*, const char* input, std::size_t size, off_t offset, fuse_file_info* info) {
            return protect([&](Impl& s) {
                if (offset<0) return -EINVAL;
                return int(s.filesystem.write(s.handle(info),std::uint64_t(offset),View(reinterpret_cast<const std::uint8_t*>(input),size)));
            });
        };
        op.truncate = [](const char* path, off_t size, fuse_file_info* info) {
            return protect([&](Impl& s) {
                if(size<0)return -EINVAL;
                if(info) s.filesystem.truncate(s.handle(info),std::uint64_t(size));
                else {
                    const auto h=s.filesystem.open(path,O_WRONLY);
                    try {s.filesystem.truncate(h,std::uint64_t(size));}catch(...){try{s.filesystem.close(h);}catch(...){}throw;}
                    s.filesystem.close(h);
                }
                return 0;
            });
        };
        op.mkdir = [](const char* path, mode_t) { return protect([&](Impl& s) { s.filesystem.mkdir(path); return 0; }); };
        op.unlink = [](const char* path) { return protect([&](Impl& s) { s.filesystem.remove(path,false); return 0; }); };
        op.rmdir = [](const char* path) { return protect([&](Impl& s) { s.filesystem.remove(path,true); return 0; }); };
        op.rename = [](const char* source, const char* target, unsigned flags) {
            return protect([&](Impl& s) { if(flags & ~1U)return -EOPNOTSUPP; s.filesystem.rename(source,target,!(flags&1)); return 0; });
        };
        op.opendir = [](const char* path, fuse_file_info* info) {
            return protect([&](Impl& s) {
                s.capacity(); auto entries=s.filesystem.list(path); std::size_t bytes=0;
                for(const auto& entry:entries) bytes+=sizeof(entry)+entry.name.size();
                if(bytes>16*1024*1024-s.snapshot_bytes)return -ENOMEM;
                const auto id=s.allocate_handle();
                s.directories.emplace(id,Snapshot{std::move(entries),bytes});s.snapshot_bytes+=bytes;info->fh=id;return 0;
            });
        };
        op.readdir = [](const char*, void* buffer, fuse_fill_dir_t fill, off_t offset, fuse_file_info* info, fuse_readdir_flags) {
            return protect([&](Impl& s) {
                const auto found=s.directories.find(info->fh);if(found==s.directories.end())return -EBADF;
                const auto& entries=found->second.entries;
                if(offset<0||std::uint64_t(offset)>entries.size()+2)return -EINVAL;
                for(auto index=std::size_t(offset);index<entries.size()+2;++index) {
                    struct stat native{};drive::FileInfo directory_info;directory_info.directory=true;
                    const char* name=index==0?".":index==1?"..":entries[index-2].name.c_str();
                    drive::native_stat(index<2?directory_info:entries[index-2].info,s.filesystem.writable(),native);
                    if(fill(buffer,name,&native,off_t(index+1),fuse_fill_dir_flags(0)))break;
                }
                return 0;
            });
        };
        op.releasedir = [](const char*, fuse_file_info* info) {
            return protect([&](Impl& s) {
                const auto found=s.directories.find(info->fh);if(found==s.directories.end())return -EBADF;
                s.snapshot_bytes-=found->second.bytes;s.directories.erase(found);return 0;
            });
        };
        // Writes complete synchronously at the RDP peer, but no stable-storage
        // flush primitive is implemented. Never claim fsync/O_SYNC durability.
        op.fsync = [](const char*, int, fuse_file_info*) { return -EOPNOTSUPP; };
        return op;
    }
    void start(const std::string& private_root) {
        require(getuid()!=0,"redirected drive mounts must run as an ordinary user");
        require(!private_root.empty()&&private_root.front()=='/',"drive mount root must be absolute");
        root.reset(::open(private_root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        struct stat st{};
        require(root && fstat(root.get(),&st)==0 && st.st_uid==getuid() && !(st.st_mode&0077),
                "drive mount root must be an existing owner-only directory (0700)");
        std::unique_ptr<char,decltype(&free)> canonical(realpath(private_root.c_str(),nullptr),free);
        require(canonical!=nullptr,"cannot resolve drive mount root");
        std::uint8_t random[16];require(RAND_bytes(random,sizeof(random))==1,"cannot create private mount identity");
        name="lrdp-";constexpr char hex[]="0123456789abcdef";
        for(auto byte:random){name+=hex[byte>>4];name+=hex[byte&15];}
        require(mkdirat(root.get(),name.c_str(),0700)==0,"cannot create private drive mount");
        directory=std::string(canonical.get())+'/'+name;
        // Kernel permission checks and noexec/nodev/nosuid apply even if the
        // untrusted peer supplies unusual metadata. No allow_other or root mode.
        const std::string flags=std::string("default_permissions,nodev,nosuid,noexec,max_read=65536,fsname=lrdp,subtype=lrdp")+
            (filesystem.writable()?",rw":",ro");
        const char* argv[]={"lrdp-drives","-o",flags.c_str()};
        fuse_args args=FUSE_ARGS_INIT(3,const_cast<char**>(argv));
        struct FreeArgs{fuse_args& args;~FreeArgs(){fuse_opt_free_args(&args);}} free_args{args};
        const auto ops=operations();instance=fuse_new(&args,&ops,sizeof(ops),this);
        require(instance!=nullptr,"cannot initialize libfuse3");
        require(fuse_mount(instance,directory.c_str())==0,"cannot mount redirected drives; verify /dev/fuse and fusermount3");mounted=true;
        worker=std::thread([this]{(void)fuse_loop(instance);if(!stopping.load())bridge->disconnect(ENOTCONN);});
    }
};
DriveMount::DriveMount(std::shared_ptr<drive::Bridge> bridge,const std::string& root) {
    require(bridge!=nullptr,"drive mount requires native bridge");impl_=std::make_unique<Impl>(std::move(bridge));impl_->start(root);
}
DriveMount::~DriveMount() = default;
const std::string& DriveMount::path() const { return impl_->directory; }
} // namespace lrdp
