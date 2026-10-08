#pragma once
#include "format.hpp"
#include <optional>

namespace lrdp {
struct AudioInputResult { std::vector<Bytes> outbound; std::optional<Bytes> pcm; };
// MS-RDPEAI, AUDIO_INPUT DVC. Only explicitly offered PCM formats are accepted.
class AudioInput final {
    enum class State { idle, version, formats, opening, ready, closed };
    State state_ = State::idle;
    PcmFormat format_{48000, 1, 16};
    bool incoming_ = false, format_confirmed_ = false;
    std::uint64_t deadline_ = 0, budget_clock_ = 0;
    std::uint64_t budget_ = 0;
public:
    static constexpr unsigned frames_per_packet = 960;
    Bytes start(std::uint64_t now_ms);
    AudioInputResult receive(View pdu, std::uint64_t now_ms);
    bool poll(std::uint64_t now_ms);
    void close() { state_ = State::closed; }
    bool ready() const { return state_ == State::ready; }
    bool closed() const { return state_ == State::closed; }
    const PcmFormat& format() const { return format_; }
};
} // namespace lrdp
