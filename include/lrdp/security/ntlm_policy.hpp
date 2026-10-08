#pragma once
#include "lrdp/wire.hpp"
#include <algorithm>
#include <optional>

namespace lrdp {
// Policy checks only. Authentication, signing and encryption remain in system GSS.
// MS-NLMP 2.2.1.3, 2.2.2.7 and 2.2.2.9.2.
inline bool ntlm_signature(View token) {
    constexpr std::uint8_t signature[] = {'N','T','L','M','S','S','P',0};
    return token.size() >= sizeof(signature) && std::equal(std::begin(signature), std::end(signature), token.begin());
}
inline void validate_ntlm_v2_authenticate(View token) {
    Reader in(token); require(ntlm_signature(token), "not an NTLM authentication token");
    in.skip(8); require(in.le32() == 3, "expected NTLM AUTHENTICATE_MESSAGE");
    in.skip(8); const auto length = in.le16(); (void)in.le16(); const auto offset = in.le32();
    in.skip(32); const auto flags = in.le32();
    constexpr std::uint32_t required = 0x60080030; // 128-bit, key exchange, ESS, sign, seal.
    require((flags & required) == required && !(flags & 0x40), "NTLM authentication lacks required session security or requests datagrams");
    require(length >= 48 && offset >= 64 && offset <= token.size() && length <= token.size() - offset,
            "NTLMv2 response bounds are invalid");
    Reader response(token.subspan(offset, length)); response.skip(16);
    require(response.u8() == 1 && response.u8() == 1, "legacy NTLMv1 authentication is disabled");
}
// Extract only the documented SPNEGO token slots; arbitrary encrypted payloads
// are never searched for an NTLM byte sequence. Unknown encodings return empty.
inline std::optional<View> spnego_mechanism_token(View input) {
    if (ntlm_signature(input)) return input;
    try {
        Reader root(input);
        if (!input.empty() && input[0] == 0x60) {
            Reader application(root.tlv(0x60)); root.end();
            const auto mechanism = application.tlv(6);
            constexpr std::uint8_t spnego[] = {0x2b,6,1,5,5,2};
            if (mechanism.size() != sizeof(spnego) || !std::equal(mechanism.begin(), mechanism.end(), std::begin(spnego))) return std::nullopt;
            input = application.take(application.remaining());
        }
        Reader choice(input);
        if (input.empty() || (input[0] != 0xa0 && input[0] != 0xa1)) return std::nullopt;
        Reader wrapper(choice.tlv(input[0])); choice.end();
        const auto sequence = wrapper.tlv(0x30); wrapper.end(); Reader fields(sequence);
        while (!fields.empty()) {
            Reader peek(sequence.subspan(fields.position())); const auto tag = peek.u8();
            Reader field(fields.tlv(tag));
            if (tag == 0xa2) { const auto token = field.tlv(4); field.end(); return token; }
        }
    } catch (const ProtocolError&) {}
    return std::nullopt;
}
inline void validate_ntlm_sequence(View sealed_message, std::uint32_t expected) {
    Reader signature(sealed_message);
    require(signature.le32() == 1, "unsupported NTLM message signature version");
    signature.skip(8);
    require(signature.le32() == expected, "NTLM protected message is replayed or out of sequence");
}
} // namespace lrdp
