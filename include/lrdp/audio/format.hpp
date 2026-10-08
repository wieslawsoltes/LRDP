#pragma once
#include "lrdp/wire.hpp"

namespace lrdp {
struct PcmFormat {
    std::uint32_t rate = 48000;
    std::uint16_t channels = 2;
    std::uint16_t bits = 16;
    std::uint16_t block_size() const { return std::uint16_t(channels * (bits / 8)); }
    std::uint32_t byte_rate() const { return rate * block_size(); }
    bool operator==(const PcmFormat&) const = default;
};
struct AudioFormat {
    std::uint16_t tag = 0, channels = 0;
    std::uint32_t rate = 0, byte_rate = 0;
    std::uint16_t block_size = 0, bits = 0;
    Bytes extra;
    static AudioFormat read(Reader& reader);
    bool matches(const PcmFormat& pcm) const;
};
Bytes pcm_audio_format(PcmFormat pcm);
Bytes sound_pdu(std::uint8_t type, View body = {});
} // namespace lrdp
