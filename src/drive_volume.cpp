#include "lrdp/drive/volume.hpp"
#include "lrdp/drive/client.hpp"
#include <algorithm>
#include <limits>
#include <type_traits>

namespace lrdp::drive {
namespace {
void validate(const VolumeSpace& space) {
    require(space.unit_bytes != 0, "zero filesystem allocation-unit size");
    const auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max());
    for (const auto units : {space.total_units, space.caller_free_units, space.actual_free_units}) {
        require(units <= maximum, "negative filesystem allocation-unit count");
        require(units <= std::numeric_limits<std::uint64_t>::max() / space.unit_bytes,
                "filesystem capacity exceeds 64-bit byte representation");
    }
}
template<class T> T native_number(std::uint64_t value) {
    static_assert(std::is_integral_v<T>);
    if (value > std::uint64_t(std::numeric_limits<T>::max()))
        throw IoError(EOVERFLOW, "filesystem capacity exceeds native representation");
    return static_cast<T>(value);
}
}
VolumeSpace volume_space(View data, std::uint32_t information_class) {
    require(information_class == 3 || information_class == 7, "unsupported volume-space class");
    Reader in(data); VolumeSpace result;
    result.total_units = read_u64(in); result.caller_free_units = read_u64(in);
    result.actual_free_units = information_class == 7 ? read_u64(in) : result.caller_free_units;
    result.quota_aware = information_class == 7;
    const auto sectors = in.le32(), bytes = in.le32(); in.end();
    require(sectors != 0 && bytes != 0, "invalid filesystem sector geometry");
    result.unit_bytes = std::uint64_t(sectors) * bytes;
    validate(result); return result;
}
void native_statvfs(const VolumeSpace& space, bool writable, struct statvfs& result) {
    validate(space);
    // The kernel FUSE reply has 32-bit bsize/frsize even when statvfs uses
    // unsigned long. Reject rather than letting libfuse truncate the geometry.
    if (space.unit_bytes > std::numeric_limits<std::uint32_t>::max())
        throw IoError(EOVERFLOW, "allocation unit exceeds the FUSE wire representation");
    struct statvfs next{};
    next.f_bsize = native_number<decltype(next.f_bsize)>(space.unit_bytes);
    next.f_frsize = native_number<decltype(next.f_frsize)>(space.unit_bytes);
    next.f_blocks = native_number<decltype(next.f_blocks)>(space.total_units);
    // FileFsFullSizeInformation.TotalAllocationUnits may reflect a user quota,
    // whereas ActualAvailableAllocationUnits describes the entire volume.
    // Do not reject such a legitimate response or expose physical free space as
    // the user's allowance. Inconsistent/racing counters are clamped, not grown.
    const auto free = std::min(space.total_units, space.actual_free_units);
    next.f_bfree = native_number<decltype(next.f_bfree)>(free);
    next.f_bavail = native_number<decltype(next.f_bavail)>(std::min(free, space.caller_free_units));
    // The protocol provides no inode totals. Zero denotes unspecified here, not
    // a fabricated unlimited inode pool. 255 is a conservative ASCII component
    // limit; remote encoding/path policy is still checked for each operation.
    next.f_namemax = 255;
    next.f_flag = static_cast<unsigned long>(ST_NOSUID) |
                  (writable ? 0UL : static_cast<unsigned long>(ST_RDONLY));
#ifdef ST_NODEV
    next.f_flag |= ST_NODEV;
#endif
#ifdef ST_NOEXEC
    next.f_flag |= ST_NOEXEC;
#endif
    result = next;
}
VolumeIdentity volume_identity(View data) {
    Reader in(data); VolumeIdentity result;
    result.created_100ns = read_u64(in);
    require(result.created_100ns <= std::uint64_t(std::numeric_limits<std::int64_t>::max()),
            "negative volume creation time");
    result.serial = in.le32(); const auto length = in.le32();
    result.supports_objects = in.u8() != 0; // FSCC Boolean: ANY nonzero value is true.
    require(length <= 4096 && (length & 1U) == 0, "volume label exceeds Unicode/policy bounds");
    auto text = in.take(length); in.end();
    if (text.size() >= 2 && text[text.size()-2] == 0 && text.back() == 0)
        text = text.first(text.size()-2);
    result.label = from_utf16le(text, false);
    require(result.label.find('\0') == std::string::npos, "embedded volume-label NUL");
    return result;
}
VolumeAttributes volume_attributes(View data) {
    Reader in(data); VolumeAttributes result;
    result.flags = in.le32(); result.max_component_utf16 = in.le32();
    const auto length = in.le32();
    if (result.max_component_utf16 == 0 || result.max_component_utf16 > 255)
        throw ProtocolError("invalid filesystem component limit: " + std::to_string(result.max_component_utf16));
    require((result.flags & 0x8010U) != 0x8010U, "incompatible filesystem compression flags");
    require(length > 0 && length <= 4096 && !(length & 1U), "invalid filesystem name length");
    result.filesystem = from_utf16le(in.take(length), false); in.end();
    require(result.filesystem.find('\0') == std::string::npos, "filesystem name is not length-delimited Unicode");
    // Other flags remain opaque metadata. They do not enable unsupported local
    // semantics; in particular the name is never used to infer NTFS behaviour.
    return result;
}
VolumeDevice volume_device(View data) {
    Reader in(data); VolumeDevice result{in.le32(), in.le32()}; in.end();
    require(result.type == 2 || result.type == 7, "unsupported filesystem device type");
    return result;
}
void validate_volume_information(View data, std::uint32_t information_class) {
    switch (information_class) {
    case 1: (void)volume_identity(data); break;
    case 3: case 7: (void)volume_space(data, information_class); break;
    case 4: (void)volume_device(data); break;
    case 5: (void)volume_attributes(data); break;
    default: throw ProtocolError("unsupported volume information class");
    }
}
int native_xattr(View value, char* output, std::size_t size) {
    if (value.size() > std::size_t(std::numeric_limits<int>::max()))
        throw IoError(EOVERFLOW, "native xattr length overflow");
    if (!size) return int(value.size());
    if (size < value.size()) throw IoError(ERANGE, "native xattr buffer is too small");
    if (!output) throw IoError(EINVAL, "native xattr destination is missing");
    std::copy(value.begin(), value.end(), output); return int(value.size());
}
} // namespace lrdp::drive
