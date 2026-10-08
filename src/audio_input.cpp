#include "lrdp/audio/input.hpp"
#include <algorithm>

namespace lrdp {
Bytes AudioInput::start(std::uint64_t now) {
    require(state_ == State::idle, "microphone channel already initialized");
    Writer version; version.u8(1).le32(2); state_ = State::version; deadline_ = now + 5000;
    return std::move(version).finish();
}
AudioInputResult AudioInput::receive(View pdu, std::uint64_t now) {
    require(!closed() && state_ != State::idle && pdu.size() <= 65536, "microphone channel unavailable or message too large");
    Reader in(pdu); const auto type = in.u8(); AudioInputResult result;
    require(!incoming_ || type == 2 || type == 6, "microphone diagnostic marker not followed by formats or data");
    if (type == 5) {
        require(state_ == State::formats || ready(), "microphone data marker out of sequence"); in.end(); incoming_ = true; return result;
    }
    incoming_ = false;
    if (type == 1) {
        require(state_ == State::version, "microphone version out of sequence");
        const auto version = in.le32(); require(version >= 1, "unsupported microphone protocol version"); in.end();
        Writer formats; formats.u8(2).le32(1).le32(27).raw(pcm_audio_format(format_));
        result.outbound.push_back(std::move(formats).finish()); state_ = State::formats; deadline_ = now + 5000;
    } else if (type == 2) {
        require(state_ == State::formats, "microphone formats out of sequence");
        const auto count = in.le32(), meaningful_size = in.le32(); require(count <= 1, "client returned unoffered microphone formats");
        if (count) require(AudioFormat::read(in).matches(format_), "client returned an unoffered microphone codec");
        require(meaningful_size == in.position(), "microphone format packet size mismatch");
        in.skip(in.remaining()); // MS-RDPEAI permits opaque diagnostic ExtraData.
        if (!count) { state_ = State::closed; return result; }
        Writer open; open.u8(3).le32(frames_per_packet).le32(0).raw(pcm_audio_format(format_));
        result.outbound.push_back(std::move(open).finish()); state_ = State::opening; deadline_ = now + 5000;
    } else if (type == 7) {
        require(state_ == State::opening && !format_confirmed_ && in.le32() == 0, "unrequested microphone format change");
        in.end(); format_confirmed_ = true;
    } else if (type == 4) {
        require(state_ == State::opening, "microphone Open Reply out of sequence");
        const auto status = in.le32(); in.end();
        if (status & 0x80000000U) { state_ = State::closed; return result; }
        require(format_confirmed_, "microphone opened without initial format confirmation");
        state_ = State::ready; budget_clock_ = now; budget_ = std::uint64_t(format_.byte_rate()) * 500;
    } else if (type == 6) {
        require(ready(), "microphone samples arrived before successful opening");
        require(in.remaining() == frames_per_packet * format_.block_size(), "microphone PCM packet does not match the negotiated frame count");
        require(now >= budget_clock_, "microphone clock moved backwards");
        const auto elapsed = std::min<std::uint64_t>(now - budget_clock_, 500);
        // Fixed-point byte budget permits a half-second burst, then real-time PCM.
        budget_ = std::min<std::uint64_t>(std::uint64_t(format_.byte_rate()) * 500,
                    budget_ + elapsed * format_.byte_rate()); budget_clock_ = now;
        const auto cost = std::uint64_t(in.remaining()) * 1000;
        require(cost <= budget_, "microphone sender exceeds the negotiated sample rate"); budget_ -= cost;
        const auto pcm = in.take(in.remaining()); result.pcm = Bytes(pcm.begin(), pcm.end());
    } else throw ProtocolError("unsupported microphone PDU");
    return result;
}
bool AudioInput::poll(std::uint64_t now) {
    if (state_ != State::idle && !ready() && !closed() && now >= deadline_) { close(); return true; }
    return false;
}
} // namespace lrdp
