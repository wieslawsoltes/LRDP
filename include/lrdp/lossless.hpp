#pragma once
#include "desktop.hpp"

namespace lrdp {
// Encodes only ClearCodec's complete residual layer. No glyph/band caches,
// lossy subcodecs, pixel format conversion, or alpha removal are implicit.
// nullopt requests an uncompressed fallback (alpha or no compression benefit).
std::optional<Bytes> clearcodec_residual(View bgra, unsigned width, unsigned height,
                                       std::size_t stride, std::uint8_t sequence,
                                       std::size_t byte_budget);
struct LosslessStatistics {
    std::uint32_t solid_rectangles = 0, clear_rectangles = 0, raw_rectangles = 0;
    std::uint64_t changed_pixels = 0, command_bytes = 0;
};
class LosslessEncoder {
    Frame previous_;
    std::uint8_t sequence_ = 0;
    LosslessStatistics statistics_;
public:
    // Invalidation preserves the session-wide ClearCodec sequence counter.
    void invalidate() { previous_ = {}; }
    // Produces unsegmented GFX commands for surface 0. Call only when the caller
    // can transmit a whole frame. Reference/sequence commit only after planning.
    std::vector<Bytes> encode(const Frame& frame);
    const LosslessStatistics& statistics() const { return statistics_; }
};
} // namespace lrdp
