#include "lrdp/audio/output.hpp"
#include <algorithm>
namespace lrdp {
Bytes AudioOutput::start(std::uint64_t now) {
    require(state_ == State::idle, "RDPSND already initialized");
    Writer body; body.zeros(14).le16(1).u8(0).le16(8).u8(0).raw(pcm_audio_format(format_));
    state_ = State::formats; deadline_ = now + 5000; return sound_pdu(7, body.bytes());
}
Bytes AudioOutput::training(std::uint64_t now) {
    state_ = State::training; deadline_ = now + 5000; training_stamp_ = std::uint16_t(now);
    Writer body; body.le16(training_stamp_).le16(0); return sound_pdu(6, body.bytes());
}
std::vector<Bytes> AudioOutput::receive(View packet, std::uint64_t now) {
    Reader in(packet); const auto type = in.u8(); in.skip(1); const auto length = in.le16();
    require(length == in.remaining(), "RDPSND body length mismatch"); std::vector<Bytes> result;
    if (type == 7) {
        require(state_ == State::formats, "RDPSND formats out of sequence");
        const auto flags = in.le32(); in.skip(10); const auto count = in.le16(); in.skip(1);
        version_ = std::min<std::uint16_t>(in.le16(), 8); in.skip(1);
        require(version_ >= 2 && count <= 64, "unsupported audio version or excessive format count");
        std::optional<unsigned> selected;
        for (unsigned i = 0; i < count; ++i) if (AudioFormat::read(in).matches(format_) && !selected) selected = i;
        in.end();
        if (!(flags & 1) || !selected) { state_ = State::closed; return result; }
        format_index_ = std::uint16_t(*selected);
        if (version_ >= 6) { state_ = State::quality; deadline_ = now + 5000; }
        else result.push_back(training(now));
    } else if (type == 12) {
        const auto quality = in.le16(); in.skip(2); in.end(); require(quality <= 2, "invalid audio quality mode");
        if (state_ == State::closed) return result;
        require(state_ == State::quality && version_ >= 6, "audio quality mode out of sequence"); result.push_back(training(now));
    } else if (type == 6) {
        require(state_ == State::training && in.le16() == training_stamp_ && in.le16() == 0, "audio training response does not match request");
        in.end(); state_ = State::ready;
    } else if (type == 5) {
        (void)in.le16(); const auto block = in.u8(); in.skip(1); in.end();
        if (state_ == State::closed) return result;
        require(ready(), "audio confirmation before training");
        const auto found = std::find_if(pending_.begin(), pending_.end(), [block](const auto& pending) { return pending.block == block; });
        require(found != pending_.end(), "duplicate or unsent audio block confirmation");
        require(now >= found->sent, "audio completion clock moved backwards");
        completed_latency_ = now - found->sent; pending_.erase(found);
    } else throw ProtocolError("unsupported client RDPSND message");
    return result;
}
std::vector<Bytes> AudioOutput::send(View pcm, std::uint64_t now) {
    require(can_send(), "audio output window is unavailable");
    require(pcm.size() >= 4 && pcm.size() <= format_.byte_rate() / 10 && pcm.size() % format_.block_size() == 0,
            "audio block is unaligned or exceeds 100 ms");
    next_block_ = std::uint8_t(unsigned(next_block_) + 1);
    const auto stamp = std::uint16_t(now); std::vector<Bytes> result;
    if (version_ >= 8) {
        Writer body; body.le16(stamp).le16(format_index_).u8(next_block_).zeros(3).le32(std::uint32_t(now)).raw(pcm);
        result.push_back(sound_pdu(13, body.bytes()));
    } else {
        Writer info; info.u8(2).u8(0).le16(unsigned(pcm.size() + 8)).le16(stamp).le16(format_index_)
            .u8(next_block_).zeros(3).raw(pcm.first(4));
        Writer wave; wave.zeros(4).raw(pcm.subspan(4));
        result.push_back(std::move(info).finish()); result.push_back(std::move(wave).finish());
    }
    pending_.push_back({next_block_, now}); return result;
}
std::optional<Bytes> AudioOutput::poll(std::uint64_t now) {
    if (state_ == State::idle || closed()) return std::nullopt;
    if ((!ready() && now >= deadline_) || (!pending_.empty() && now >= pending_.front().sent + 5000)) return close();
    return std::nullopt;
}
Bytes AudioOutput::close() { state_ = State::closed; pending_.clear(); return sound_pdu(1); }
} // namespace lrdp
