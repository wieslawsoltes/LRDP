#pragma once
#include "format.hpp"
#include <deque>
#include <optional>

namespace lrdp {
// MS-RDPEA: reliable virtual-channel audio. PCM is never sent before training.
class AudioOutput final {
    enum class State { idle, formats, quality, training, ready, closed };
    struct Pending { std::uint8_t block; std::uint64_t sent; };
    State state_ = State::idle;
    PcmFormat format_;
    std::uint16_t version_ = 0, format_index_ = 0, training_stamp_ = 0;
    std::uint8_t next_block_ = 0;
    std::uint64_t deadline_ = 0, completed_latency_ = 0;
    std::deque<Pending> pending_;
    Bytes training(std::uint64_t now_ms);
public:
    explicit AudioOutput(PcmFormat format = {}) : format_(format) {}
    Bytes start(std::uint64_t now_ms);
    std::vector<Bytes> receive(View packet, std::uint64_t now_ms);
    std::vector<Bytes> send(View pcm, std::uint64_t now_ms);
    // Returns CLOSE if negotiation or client consumption stalls; the desktop survives.
    std::optional<Bytes> poll(std::uint64_t now_ms);
    Bytes close();
    bool ready() const { return state_ == State::ready; }
    bool closed() const { return state_ == State::closed; }
    bool can_send() const { return ready() && pending_.size() < 8; }
    std::size_t in_flight() const { return pending_.size(); }
    std::uint64_t completion_latency_ms() const { return completed_latency_; }
    const PcmFormat& format() const { return format_; }
};
} // namespace lrdp
