#include "lrdp/audio/format.hpp"
namespace lrdp {
AudioFormat AudioFormat::read(Reader& in) {
    AudioFormat f; f.tag = in.le16(); f.channels = in.le16(); f.rate = in.le32(); f.byte_rate = in.le32();
    f.block_size = in.le16(); f.bits = in.le16(); const auto length = in.le16();
    require(length <= 4096, "audio format extension exceeds quota"); const auto extra = in.take(length);
    f.extra.assign(extra.begin(), extra.end()); return f;
}
bool AudioFormat::matches(const PcmFormat& pcm) const {
    return tag == 1 && channels == pcm.channels && rate == pcm.rate && bits == pcm.bits &&
           byte_rate == pcm.byte_rate() && block_size == pcm.block_size() && extra.empty();
}
Bytes pcm_audio_format(PcmFormat pcm) {
    require(pcm.rate >= 8000 && pcm.rate <= 192000 && pcm.channels >= 1 && pcm.channels <= 8 && pcm.bits == 16,
            "unsupported PCM format");
    Writer out; out.le16(1).le16(pcm.channels).le32(pcm.rate).le32(pcm.byte_rate()).le16(pcm.block_size()).le16(pcm.bits).le16(0);
    return std::move(out).finish();
}
Bytes sound_pdu(std::uint8_t type, View body) {
    require(body.size() <= 65535, "RDPSND body exceeds 16-bit framing");
    Writer out; out.u8(type).u8(0).le16(unsigned(body.size())).raw(body); return std::move(out).finish();
}
} // namespace lrdp
