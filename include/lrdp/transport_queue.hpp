#pragma once
#include "wire.hpp"
#include <deque>
#include <optional>
namespace lrdp {
// Priority applies only between complete transport packets. The active packet
// remains stable across partial writes and SSL_ERROR_WANT_* retries.
class TransportQueue final {
    struct Active { Bytes bytes; std::size_t offset = 0; bool media = false; };
    std::deque<Bytes> normal_, media_;
    std::optional<Active> active_;
    std::size_t normal_bytes_ = 0, media_bytes_ = 0;
    unsigned media_burst_ = 0;
public:
    void enqueue(std::vector<Bytes> packets, bool media = false) {
        std::size_t added = 0;
        const std::size_t quota = media ? 1024 * 1024 : 72 * 1024 * 1024;
        auto& queued = media ? media_bytes_ : normal_bytes_;
        for (const auto& packet : packets) {
            require(!packet.empty() && packet.size() <= quota - queued - added, "transport output quota exceeded"); added += packet.size();
        }
        auto& queue = media ? media_ : normal_;
        for (auto& packet : packets) queue.push_back(std::move(packet));
        queued += added;
    }
    View peek() {
        if (!active_) {
            const bool media = !media_.empty() && (normal_.empty() || media_burst_ < 8);
            auto& queue = media ? media_ : normal_;
            if (queue.empty()) return {};
            active_.emplace(Active{std::move(queue.front()), 0, media}); queue.pop_front();
            media_burst_ = media ? std::min(8U, media_burst_ + 1) : 0;
        }
        return View(active_->bytes).subspan(active_->offset);
    }
    void consume(std::size_t count) {
        require(active_ && count > 0 && count <= active_->bytes.size() - active_->offset, "invalid transport write completion");
        auto& queued = active_->media ? media_bytes_ : normal_bytes_; queued -= count; active_->offset += count;
        if (active_->offset == active_->bytes.size()) active_.reset();
    }
    std::size_t queued() const { return normal_bytes_ + media_bytes_; }
    std::size_t normal_queued() const { return normal_bytes_; }
};
} // namespace lrdp
