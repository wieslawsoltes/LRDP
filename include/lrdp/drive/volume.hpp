#pragma once
#include "protocol.hpp"
#include <sys/statvfs.h>

namespace lrdp::drive {
struct VolumeSpace {
    std::uint64_t total_units = 0;
    std::uint64_t caller_free_units = 0;
    std::uint64_t actual_free_units = 0;
    std::uint64_t unit_bytes = 4096;
    bool quota_aware = false;
    bool operator==(const VolumeSpace&) const = default;
};
struct VolumeIdentity {
    std::uint64_t created_100ns = 0;
    std::uint32_t serial = 0;
    bool supports_objects = false;
    std::string label;
    bool operator==(const VolumeIdentity&) const = default;
};
struct VolumeAttributes {
    std::uint32_t flags = 0, max_component_utf16 = 255;
    std::string filesystem;
    bool read_only() const { return (flags & 0x00080000U) != 0; }
    bool operator==(const VolumeAttributes&) const = default;
};
struct VolumeDevice {
    std::uint32_t type = 0, characteristics = 0;
    bool read_only() const { return (characteristics & 2U) != 0; }
    bool operator==(const VolumeDevice&) const = default;
};
struct VolumeDetails {
    std::optional<VolumeIdentity> identity;
    std::optional<VolumeAttributes> attributes;
    std::optional<VolumeDevice> device;
    bool read_only() const {
        return (attributes && attributes->read_only()) || (device && device->read_only());
    }
};
// RDP's class-1 record has a 17-byte header: the generic FSCC reserved
// byte MUST NOT be present. Labels retain spaces; one terminal NUL is optional.
VolumeIdentity volume_identity(View data);
VolumeAttributes volume_attributes(View data);
VolumeDevice volume_device(View data);
void validate_volume_information(View data, std::uint32_t information_class);
// Native xattr size probes and copies share one tested bounds contract. Values
// are length-delimited bytes, never implicitly NUL-terminated. Failure is atomic.
int native_xattr(View value, char* output, std::size_t size);
// MS-FSCC 2.5.4 / 2.5.8, fixed-size records; never allocate from peer dimensions.
VolumeSpace volume_space(View data, std::uint32_t information_class);
// Conservative POSIX normalization preserves quota-limited caller availability.
// The caller's output is unchanged if any conversion/overflow check fails.
void native_statvfs(const VolumeSpace& space, bool writable, struct statvfs& result);
} // namespace lrdp::drive
