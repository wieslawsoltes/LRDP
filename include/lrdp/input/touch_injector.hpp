#pragma once
#include "extended.hpp"
#include <functional>
#include <span>

namespace lrdp {
enum class TouchAction { down, motion, up };
struct TouchOperation {
    TouchAction action;
    unsigned slot;
    double x = 0, y = 0;
};
// Maps the protocol's sparse byte IDs to bounded native slots. The whole input
// batch is validated before any native call. A sink failure releases every slot
// that might have been injected, including newly allocated slots in that batch.
class TouchInjector final {
    std::function<void(std::span<const TouchOperation>)> submit_;
    std::array<int, 256> slots_;
    unsigned capacity_;
public:
    explicit TouchInjector(std::function<void(std::span<const TouchOperation>)> submit, unsigned capacity = 32);
    void apply(std::span<const ExtendedFrame> frames, unsigned width, unsigned height,
               unsigned logical_width, unsigned logical_height);
    void cancel();
    [[nodiscard]] unsigned active() const;
};
} // namespace lrdp
