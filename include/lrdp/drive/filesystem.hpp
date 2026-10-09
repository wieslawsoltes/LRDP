#pragma once
#include "client.hpp"
#include "volume.hpp"
#include <sys/stat.h>

namespace lrdp::drive {
struct Entry { std::string name; FileInfo info; };
struct Handle { DeviceKey device; std::uint64_t cookie = 0; std::string path; };
std::string device_name(const Device& device);
// Synchronous native facade, intended for a filesystem worker, not the network
// loop. Remote operations remain checked/bounded by the independent wire engine.
class Filesystem final {
    Client& client_;
    struct Path { DeviceKey device; std::string relative; };
    Path resolve(std::string_view path) const;
    Reply execute(Request request);
    Handle open_relative(const Path& path, std::uint32_t access, std::uint32_t disposition, std::uint32_t options);
    FileInfo information(const Handle& handle);
    VolumeSpace space(const Handle& handle);
    std::optional<Bytes> optional_volume(const Handle& handle, std::uint32_t information_class);
    void mutable_path(const Path& path) const;
public:
    explicit Filesystem(Client& client) : client_(client) {}
    bool writable() const { return client_.limits().writable; }
    FileInfo stat(std::string_view path);
    VolumeSpace space(std::string_view path);
    VolumeDetails volume_details(std::string_view path);
    void statfs(std::string_view path, struct statvfs& result);
    // Read-only LRDP volume annotations, not arbitrary remote EAs or ACLs.
    Bytes getxattr(std::string_view path, std::string_view name);
    Bytes listxattr(std::string_view path);
    FileInfo stat(const Handle& handle) { return information(handle); }
    std::vector<Entry> list(std::string_view path);
    Handle open(std::string_view path, int flags);
    void close(const Handle& handle);
    Bytes read(const Handle& handle, std::uint64_t offset, std::size_t count);
    std::size_t write(const Handle& handle, std::uint64_t offset, View data);
    void truncate(const Handle& handle, std::uint64_t size);
    void mkdir(std::string_view path);
    void remove(std::string_view path, bool directory);
    void rename(std::string_view source, std::string_view destination, bool replace);
};
void native_stat(const FileInfo& info, bool writable, struct stat& result);
} // namespace lrdp::drive
