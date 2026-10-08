#pragma once
#include "lrdp/wire.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
namespace lrdp {
// One producer and one consumer; only the consumer advances read_. Real-time
// operations never allocate, lock, throw, or overwrite a consumer-owned slot.
class PcmRing final {
    static std::uint32_t validated(std::uint32_t capacity, std::uint32_t block, std::uint32_t initial) {
        require(capacity >= 16 && capacity <= 1024 * 1024 && std::has_single_bit(capacity) && block &&
                capacity % block == 0 && initial % block == 0, "invalid PCM ring geometry"); return capacity;
    }
    Bytes bytes_;
    std::uint32_t mask_, block_;
    alignas(128) std::atomic<std::uint32_t> read_;
    alignas(128) std::atomic<std::uint32_t> written_;
    alignas(128) std::atomic<std::uint32_t> dropped_{0};
    void copy_in(std::uint32_t p, View input) noexcept {
        const auto offset = p & mask_; const auto first = std::min(input.size(), bytes_.size() - offset);
        std::memcpy(bytes_.data() + offset, input.data(), first);
        if (first < input.size()) std::memcpy(bytes_.data(), input.data() + first, input.size() - first);
    }
    void copy_out(std::uint32_t p, std::span<std::uint8_t> output) noexcept {
        const auto offset = p & mask_; const auto first = std::min(output.size(), bytes_.size() - offset);
        std::memcpy(output.data(), bytes_.data() + offset, first);
        if (first < output.size()) std::memcpy(output.data() + first, bytes_.data(), output.size() - first);
    }
public:
    explicit PcmRing(std::uint32_t capacity, std::uint32_t block, std::uint32_t initial = 0)
        : bytes_(validated(capacity, block, initial)), mask_(capacity - 1), block_(block), read_(initial), written_(initial) {
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    }
    PcmRing(const PcmRing&) = delete;
    PcmRing& operator=(const PcmRing&) = delete;
    std::size_t push(View input) noexcept {
        if (input.size() % block_) return 0;
        const auto write = written_.load(std::memory_order_relaxed), read = read_.load(std::memory_order_acquire);
        const auto count = std::min(bytes_.size() - std::uint32_t(write - read), input.size());
        if (count) { copy_in(write, input.first(count)); written_.store(write + std::uint32_t(count), std::memory_order_release); }
        dropped_.fetch_add(std::uint32_t((input.size() - count) / block_), std::memory_order_relaxed); return count;
    }
    std::size_t pop(std::span<std::uint8_t> output, bool exact = false, std::size_t keep_latest = 0) noexcept {
        if (output.size() % block_) return 0;
        auto read = read_.load(std::memory_order_relaxed); const auto write = written_.load(std::memory_order_acquire);
        auto available = std::uint32_t(write - read);
        if (keep_latest) {
            keep_latest -= keep_latest % block_;
            if (available > keep_latest) {
                const auto excess = available - std::uint32_t(keep_latest); read += excess; available -= excess;
                dropped_.fetch_add(excess / block_, std::memory_order_relaxed); read_.store(read, std::memory_order_release);
            }
        }
        if (exact && available < output.size()) return 0;
        const auto count = std::min<std::size_t>(available, output.size());
        if (count) { copy_out(read, output.first(count)); read_.store(read + std::uint32_t(count), std::memory_order_release); }
        return count;
    }
    void discard() noexcept { read_.store(written_.load(std::memory_order_acquire), std::memory_order_release); }
    std::uint32_t dropped_frames() const noexcept { return dropped_.load(std::memory_order_relaxed); }
};
} // namespace lrdp
