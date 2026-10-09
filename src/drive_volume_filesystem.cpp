#include "lrdp/drive/filesystem.hpp"
#include <algorithm>
#include <array>

namespace lrdp::drive {
namespace {
class CloseVolume {
    Filesystem& filesystem_;
    const Handle& handle_;
public:
    CloseVolume(Filesystem& filesystem, const Handle& handle) : filesystem_(filesystem), handle_(handle) {}
    ~CloseVolume() { try { filesystem_.close(handle_); } catch (...) {} }
};
struct Annotation { std::string_view name; std::uint32_t information_class; };
constexpr std::array annotations{
    Annotation{"user.lrdp.volume.label", 1},
    Annotation{"user.lrdp.volume.serial", 1},
    Annotation{"user.lrdp.volume.created-100ns", 1},
    Annotation{"user.lrdp.volume.filesystem", 5},
    Annotation{"user.lrdp.volume.flags", 5},
    Annotation{"user.lrdp.volume.max-component-utf16", 5},
    Annotation{"user.lrdp.volume.device-type", 4},
    Annotation{"user.lrdp.volume.device-characteristics", 4}
};
std::string hex32(std::uint32_t value) {
    std::string result = "0x00000000";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) result[9-i] = digits[(value >> (i*4)) & 15];
    return result;
}
void assign(VolumeDetails& result, View data, std::uint32_t kind) {
    switch (kind) {
    case 1: result.identity = volume_identity(data); break;
    case 4: result.device = volume_device(data); break;
    case 5: result.attributes = volume_attributes(data); break;
    default: throw ProtocolError("invalid volume metadata class");
    }
}
}
VolumeSpace Filesystem::space(std::string_view path) {
    // The virtual mount root is not a volume. Summing exports would double-count
    // aliases and confuse per-user quotas, so it has no aggregate capacity.
    if (path == "/") return {};
    const auto p = resolve(path);
    const auto h = open_relative(p, 0x80, 1, p.relative.empty() ? 1 : 0);
    CloseVolume guard(*this, h);
    return space(h);
}
VolumeSpace Filesystem::space(const Handle& h) {
    Request request; request.device = h.device; request.handle = h.cookie;
    request.operation = Operation::query_volume; request.information = 7;
    auto reply = client_.call(request);
    if (reply.status == unsupported || reply.status == 0xc0000003 || reply.status == 0xc0000010) {
        // Fall back only for an explicitly unsupported class/operation. An
        // authentication failure, malformed response or disconnection is not a
        // reason to mask the error with a second query.
        request.information = 3;
        reply = client_.call(request);
    }
    if (const auto error = status_errno(reply.status)) throw IoError(error, "query redirected volume capacity");
    return volume_space(reply.data, request.information);
}
std::optional<Bytes> Filesystem::optional_volume(const Handle& handle, std::uint32_t information_class) {
    Request request;
    request.device = handle.device; request.handle = handle.cookie;
    request.operation = Operation::query_volume; request.information = information_class;
    auto reply = client_.call(std::move(request));
    if (reply.status == unsupported || reply.status == 0xc0000003U || reply.status == 0xc0000010U)
        return std::nullopt;
    if (const auto error = status_errno(reply.status)) throw IoError(error, "query redirected volume metadata");
    // Client implementations other than Bridge must obey the same bounded parser.
    validate_volume_information(reply.data, information_class);
    return std::move(reply.data);
}
VolumeDetails Filesystem::volume_details(std::string_view path) {
    if (path == "/") return {};
    const auto p = resolve(path);
    const auto h = open_relative(p, 0x80, 1, p.relative.empty() ? 1 : 0);
    CloseVolume guard(*this, h);
    VolumeDetails result;
    for (const auto kind : {1U, 5U, 4U})
        if (auto bytes = optional_volume(h, kind)) assign(result, *bytes, kind);
    return result;
}
void Filesystem::statfs(std::string_view path, struct statvfs& result) {
    if (path == "/") { native_statvfs({}, writable(), result); return; }
    const auto p = resolve(path);
    const auto h = open_relative(p, 0x80, 1, p.relative.empty() ? 1 : 0);
    CloseVolume guard(*this, h);
    const auto capacity = space(h);
    VolumeDetails details;
    for (const auto kind : {5U, 4U})
        if (auto bytes = optional_volume(h, kind)) assign(details, *bytes, kind);
    // Descriptor identity remains generation-bound throughout all queries. These
    // separate peer responses are not an atomic remote filesystem snapshot.
    // Missing optional classes preserve local policy; errors are not swallowed.
    native_statvfs(capacity, writable() && !details.read_only(), result);
}
Bytes Filesystem::getxattr(std::string_view path, std::string_view name) {
    const auto annotation = std::find_if(annotations.begin(), annotations.end(),
        [&](const Annotation& value) { return value.name == name; });
    if (annotation == annotations.end() || path == "/") throw IoError(ENODATA, "no such volume annotation");
    const auto p = resolve(path);
    const auto h = open_relative(p, 0x80, 1, p.relative.empty() ? 1 : 0);
    CloseVolume guard(*this, h);
    const auto bytes = optional_volume(h, annotation->information_class);
    if (!bytes) throw IoError(ENODATA, "peer does not provide this volume annotation");
    const auto index = std::size_t(annotation - annotations.begin());
    std::string value;
    if (index < 3) {
        const auto info = volume_identity(*bytes);
        if (index == 0) value = info.label;
        else if (index == 1) value = hex32(info.serial);
        else value = std::to_string(info.created_100ns);
    } else if (index < 6) {
        const auto info = volume_attributes(*bytes);
        if (index == 3) value = info.filesystem;
        else if (index == 4) value = hex32(info.flags);
        else value = std::to_string(info.max_component_utf16);
    } else {
        const auto info = volume_device(*bytes);
        value = hex32(index == 6 ? info.type : info.characteristics);
    }
    return Bytes(value.begin(), value.end());
}
Bytes Filesystem::listxattr(std::string_view path) {
    const auto details = volume_details(path);
    Bytes result;
    for (const auto& annotation : annotations) {
        const bool present = annotation.information_class == 1 ? bool(details.identity) :
                             annotation.information_class == 5 ? bool(details.attributes) : bool(details.device);
        if (!present) continue;
        result.insert(result.end(), annotation.name.begin(), annotation.name.end()); result.push_back(0);
    }
    return result;
}
} // namespace lrdp::drive
