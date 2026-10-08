#pragma once
#include "wire.hpp"
#include <optional>

namespace lrdp {
// Canonical cursor pixels: top-down, premultiplied BGRA, independent of host
// unsigned-long width. Native resources are never retained in the wire cache.
struct PointerShape {
    std::uint16_t width = 0, height = 0, hot_x = 0, hot_y = 0;
    Bytes bgra;
    void validate() const;
    bool operator==(const PointerShape&) const = default;
};
PointerShape fit_pointer(const PointerShape& shape, unsigned maximum = 32);
Bytes pointer_image(const PointerShape& shape, std::uint16_t slot, bool alpha);
Bytes pointer_system(bool hidden);
// Uncompressed TLS fast-path output. max_request bounds reassembly.
std::vector<Bytes> fastpath_output(unsigned code, View data, std::uint32_t max_request,
                                  std::size_t fragment_size = 16000);
struct PointerUpdate {
    Bytes payload;
    std::optional<unsigned> fastpath_code;
    std::vector<Bytes> packets(std::uint32_t max_request) const;
};

// MS-RDPBCGR 2.2.7.1.5 and 2.2.9.1.1.4. Exact cache comparisons avoid hash
// collisions; a fixed upper bound prevents an untrusted cache-size advertisement
// from allocating unbounded memory. All three pointer limits are negotiated.
class PointerEncoder {
    struct Entry { PointerShape shape; std::uint64_t used = 0; };
    std::vector<Entry> cache_;
    unsigned capacity_ = 0;
    bool alpha_ = false;
    unsigned maximum_ = 32;
    std::uint32_t max_request_ = 0;
    std::uint64_t clock_ = 0;
    std::optional<unsigned> selected_;
    bool hidden_ = false;
public:
    void configure(unsigned color_slots, unsigned alpha_slots, unsigned large_flags = 0,
                   std::uint32_t max_request = 0, bool fastpath = false);
    std::optional<PointerUpdate> update(const PointerShape& shape);
    std::uint32_t max_request() const { return max_request_; }
    unsigned maximum() const { return maximum_; }
    [[nodiscard]] unsigned capacity() const { return capacity_; }
    [[nodiscard]] bool alpha() const { return alpha_; }
};
} // namespace lrdp
