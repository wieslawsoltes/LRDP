#pragma once
#include "lrdp/desktop.hpp"

namespace lrdp {
enum class PackedFormat { bgra, bgrx, rgba, rgbx };
struct CaptureCrop { unsigned x = 0, y = 0, width = 0, height = 0; };
// The span contains exactly the declared valid chunk, starting at its offset.
// Negative pitches and wrapped video chunks are rejected, never guessed at.
Frame copy_capture_frame(View chunk, unsigned width, unsigned height, std::int32_t pitch,
                         PackedFormat format, std::optional<CaptureCrop> crop = std::nullopt);
unsigned rdp_evdev_key(std::uint16_t code, std::uint16_t flags);
} // namespace lrdp
