#include "lrdp/drive/filesystem.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <set>
#include <unistd.h>

namespace lrdp::drive {
namespace {
constexpr std::uint32_t reparse = 0x400, open_reparse = 0x00200000;
FileInfo directory() { FileInfo info; info.directory = true; info.attributes = 0x10; return info; }
Request operation(const Handle& h, Operation op) { Request r; r.device = h.device; r.handle = h.cookie; r.operation = op; return r; }
class CloseGuard {
    Filesystem& fs_; const Handle& handle_;
public:
    CloseGuard(Filesystem& fs, const Handle& h) : fs_(fs), handle_(h) {}
    ~CloseGuard() { try { fs_.close(handle_); } catch (...) {} }
};
void safe_info(const FileInfo& info) {
    if (info.attributes & reparse) throw IoError(ELOOP, "redirected reparse point is not followed");
    if (info.delete_pending) throw IoError(ENOENT, "redirected file was removed");
}
void range(std::uint64_t offset, std::size_t size) {
    if (size > 1024*1024 || offset > std::uint64_t(std::numeric_limits<std::int64_t>::max()) - size)
        throw IoError(EINVAL, "native I/O range exceeds quota");
}
}
std::string device_name(const Device& device) {
    std::string name = "drive-" + std::to_string(device.key.id) + "-" + std::to_string(device.key.generation) + "-";
    for (const unsigned char c : device.name)
        name += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? char(c) : '_';
    return name;
}
Filesystem::Path Filesystem::resolve(std::string_view path) const {
    if (path.size() > 8192 || path.size() < 2 || path.front() != '/') throw IoError(ENOENT, "missing redirected drive");
    path.remove_prefix(1);
    const auto slash = path.find('/');
    const auto name = path.substr(0, slash);
    std::string relative = slash == std::string_view::npos ? "" : std::string(path.substr(slash + 1));
    try { (void)wire_path(relative); } catch (const ProtocolError&) { throw IoError(EINVAL, "unsafe native path"); }
    for (const auto& device : client_.devices()) if (device_name(device) == name) return {device.key, std::move(relative)};
    throw IoError(ENODEV, "redirected drive is absent or replaced");
}
Reply Filesystem::execute(Request request) {
    auto result = client_.call(std::move(request));
    if (const auto error = status_errno(result.status)) throw IoError(error, "redirected filesystem operation");
    return result;
}
Handle Filesystem::open_relative(const Path& path, std::uint32_t access, std::uint32_t disposition, std::uint32_t options) {
    Request r; r.device = path.device; r.path = path.relative; r.access = access | 0x80; r.disposition = disposition; r.options = options | open_reparse;
    auto result = execute(std::move(r));
    if (!result.handle) throw IoError(EIO, "invalid native handle");
    Handle handle{path.device, result.handle, path.relative};
    // OPEN_REPARSE_POINT protects the final component; the client controls its
    // exported namespace. Never pretend this is a client-side sandbox.
    try { (void)information(handle); } catch (...) { try { close(handle); } catch (...) {} throw; }
    return handle;
}
FileInfo Filesystem::information(const Handle& handle) {
    auto request = operation(handle, Operation::query_information); request.information = 4;
    auto info = basic_information(execute(request).data); safe_info(info);
    request.information = 5; standard_information(info, execute(request).data); safe_info(info); return info;
}
FileInfo Filesystem::stat(std::string_view path) {
    if (path == "/") return directory();
    const auto p = resolve(path);
    if (p.relative.empty()) return directory();
    const auto h = open_relative(p, 0x80, 1, 0); CloseGuard guard(*this, h); return information(h);
}
std::vector<Entry> Filesystem::list(std::string_view path) {
    std::vector<Entry> entries;
    if (path == "/") {
        for (const auto& device : client_.devices()) entries.push_back({device_name(device), directory()});
        return entries;
    }
    const auto p = resolve(path); const auto h = open_relative(p, 0x80000000, 1, 1); CloseGuard guard(*this, h);
    auto request = operation(h, Operation::query_directory); request.information = 1; request.path = p.relative;
    std::set<std::string> names; std::size_t bytes = 0;
    const auto deadline = Clock::now() + client_.limits().timeout;
    unsigned batches = 0;
    for (;;) {
        if (++batches > 4098 || Clock::now() >= deadline) throw IoError(ETIMEDOUT, "directory enumeration deadline");
        auto reply = client_.call(request);
        if (reply.status == no_more_files) break;
        if (const auto error = status_errno(reply.status)) throw IoError(error, "enumerate redirected directory");
        auto batch = directory_information(reply.data);
        if (batch.empty()) break;
        for (auto& item : batch) {
            if (item.name == "." || item.name == "..") continue;
            if (!names.insert(item.name).second) throw IoError(EIO, "repeated directory entry without progress");
            if (item.info.attributes & reparse) continue; // No symlink/mount-point objects exposed to local applications.
            bytes += item.name.size() + sizeof(Entry);
            if (entries.size() >= 4096 || bytes > 1024*1024) throw IoError(EOVERFLOW, "native directory snapshot quota");
            entries.push_back({std::move(item.name), item.info});
        }
        request.initial = false;
    }
    return entries;
}
Handle Filesystem::open(std::string_view path, int flags) {
    const auto p = resolve(path);
    const int mode = flags & O_ACCMODE;
    if (mode != O_RDONLY && mode != O_WRONLY && mode != O_RDWR) throw IoError(EINVAL, "invalid open access");
    if (flags & (O_SYNC | O_DSYNC)) throw IoError(EOPNOTSUPP, "remote synchronous durability is not implemented");
    if (flags & O_APPEND) throw IoError(EOPNOTSUPP, "atomic remote append is not implemented");
    if (mode != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) mutable_path(p);
    const auto access = (mode != O_WRONLY ? 0x80000000U : 0U) | (mode != O_RDONLY ? 0x40000000U : 0U);
    std::uint32_t disposition = 1;
    if (flags & O_CREAT) disposition = flags & O_EXCL ? 2 : flags & O_TRUNC ? 5 : 3;
    else if (flags & O_TRUNC) disposition = 4;
    return open_relative(p, access, disposition, 0x40);
}
void Filesystem::close(const Handle& handle) { (void)execute(operation(handle, Operation::close)); }
Bytes Filesystem::read(const Handle& handle, std::uint64_t offset, std::size_t count) {
    range(offset, count); Bytes output; output.reserve(count);
    while (output.size() < count) {
        auto request = operation(handle, Operation::read); request.offset = offset + output.size();
        request.length = std::uint32_t(std::min<std::size_t>(count - output.size(), client_.limits().transfer));
        auto reply = client_.call(request);
        if (reply.status == end_of_file) break;
        if (const auto error = status_errno(reply.status)) { if (!output.empty()) break; throw IoError(error, "read redirected file"); }
        if (reply.data.size() > request.length) throw IoError(EIO, "oversized native read result");
        output.insert(output.end(), reply.data.begin(), reply.data.end());
        if (reply.data.size() < request.length) break;
    }
    return output;
}
std::size_t Filesystem::write(const Handle& handle, std::uint64_t offset, View data) {
    if (!writable()) throw IoError(EROFS, "readonly redirected mount");
    range(offset, data.size()); std::size_t done = 0;
    while (done < data.size()) {
        auto request = operation(handle, Operation::write); request.offset = offset + done;
        request.length = std::uint32_t(std::min<std::size_t>(data.size() - done, client_.limits().transfer));
        const auto part = data.subspan(done, request.length); request.data.assign(part.begin(), part.end());
        auto reply = client_.call(request);
        if (const auto error = status_errno(reply.status)) { if (done) break; throw IoError(error, "write redirected file"); }
        if (!reply.transferred || reply.transferred > request.length) { if (done) break; throw IoError(EIO, "invalid partial write progress"); }
        done += reply.transferred;
    }
    return done;
}
void Filesystem::truncate(const Handle& handle, std::uint64_t size) {
    if (!writable()) throw IoError(EROFS, "readonly redirected mount");
    range(size, 0); auto request = operation(handle, Operation::set_information); request.information = 20;
    Writer length; write_u64(length, size); request.data = length.bytes(); (void)execute(std::move(request));
}
void Filesystem::mutable_path(const Path& path) const {
    if (!writable() || path.relative.empty()) throw IoError(EROFS, "readonly mount or protected drive root");
}
void Filesystem::mkdir(std::string_view path) {
    const auto p = resolve(path); mutable_path(p);
    const auto h = open_relative(p, 0xc0000000, 2, 1); close(h);
}
void Filesystem::remove(std::string_view path, bool directory_flag) {
    const auto p = resolve(path); mutable_path(p);
    const auto h = open_relative(p, 0x10080, 1, directory_flag ? 1 : 0x40); CloseGuard guard(*this, h);
    auto request = operation(h, Operation::set_information); request.information = 13; (void)execute(std::move(request));
}
void Filesystem::rename(std::string_view source, std::string_view destination, bool replace) {
    const auto from = resolve(source), to = resolve(destination); mutable_path(from); mutable_path(to);
    if (from.device != to.device) throw IoError(EXDEV, "cross-drive rename");
    const auto h = open_relative(from, 0x10080, 1, 0); CloseGuard guard(*this, h);
    auto request = operation(h, Operation::set_information); request.information = 10;
    request.data = rename_information(to.relative, replace); (void)execute(std::move(request));
}
void native_stat(const FileInfo& info, bool writable, struct stat& result) {
    result = {}; result.st_uid = getuid(); result.st_gid = getgid();
    result.st_mode = mode_t(info.directory ? (S_IFDIR | (writable ? 0700 : 0500)) : (S_IFREG | (writable ? 0600 : 0400)));
    result.st_nlink = info.directory ? 2 : std::max(info.links, 1U); result.st_size = off_t(info.size);
    result.st_blksize = 65536; result.st_blocks = blkcnt_t(info.allocated/512 + (info.allocated%512 != 0));
    const auto time = [](std::uint64_t ft) -> timespec {
        if (!ft) return {};
        return {time_t(std::int64_t(ft/10000000) - 11644473600LL), long((ft%10000000)*100)};
    };
    result.st_atim = time(info.accessed); result.st_mtim = time(info.modified); result.st_ctim = time(info.changed);
}
} // namespace lrdp::drive
