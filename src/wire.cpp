#include "lrdp/wire.hpp"
#include <algorithm>
#include <bit>
#include <limits>

namespace lrdp {
void require(bool condition, std::string_view message) {
    if (!condition) throw ProtocolError(std::string(message));
}
View Reader::take(std::size_t length) {
    require(length <= remaining(), "truncated wire data");
    auto result = bytes_.subspan(offset_, length);
    offset_ += length;
    return result;
}
std::uint8_t Reader::u8() { return take(1)[0]; }
std::uint16_t Reader::le16() { auto b = take(2); return std::uint16_t(b[0] | unsigned(b[1]) << 8); }
std::uint16_t Reader::be16() { auto b = take(2); return std::uint16_t(unsigned(b[0]) << 8 | b[1]); }
std::uint32_t Reader::le32() {
    auto b = take(4); return std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 |
        std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
}
std::int32_t Reader::i32() { return std::bit_cast<std::int32_t>(le32()); }
std::uint32_t Reader::be32() {
    auto b = take(4); return std::uint32_t(b[0]) << 24 | std::uint32_t(b[1]) << 16 |
        std::uint32_t(b[2]) << 8 | std::uint32_t(b[3]);
}
std::uint32_t Reader::compact(unsigned code) {
    switch (code) { case 0: return u8(); case 1: return le16(); case 2: return le32();
        default: throw ProtocolError("reserved compact integer width"); }
}
std::size_t Reader::per_length() {
    const auto first = u8();
    return first & 0x80 ? std::size_t(first & 0x7f) << 8 | u8() : first;
}
std::size_t Reader::ber_length() {
    const auto first = u8();
    if (!(first & 0x80)) return first;
    const unsigned count = first & 0x7f;
    require(count > 0 && count <= 4, "unsupported BER length");
    std::size_t value = 0;
    for (unsigned i = 0; i < count; ++i) value = (value << 8) | u8();
    return value;
}
View Reader::tlv(unsigned tag) {
    if (tag > 255) { require(u8() == tag >> 8, "unexpected BER tag"); tag &= 255; }
    require(u8() == tag, "unexpected BER tag");
    return take(ber_length());
}
Writer& Writer::raw(View bytes) {
    require(bytes.size() <= limit_ - bytes_.size(), "wire output exceeds limit");
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); return *this;
}
Writer& Writer::zeros(std::size_t length) {
    require(length <= limit_ - bytes_.size(), "wire output exceeds limit");
    bytes_.resize(bytes_.size() + length, 0); return *this;
}
Writer& Writer::u8(unsigned value) {
    require(value <= 255, "wire u8 overflow");
    const auto b = std::uint8_t(value); return raw(View(&b, 1));
}
Writer& Writer::le16(unsigned value) {
    require(value <= 65535, "wire u16 overflow"); return u8(value & 255).u8(value >> 8);
}
Writer& Writer::be16(unsigned value) {
    require(value <= 65535, "wire u16 overflow"); return u8(value >> 8).u8(value & 255);
}
Writer& Writer::le32(std::uint32_t value) { return le16(value & 65535).le16(value >> 16); }
Writer& Writer::be32(std::uint32_t value) { return be16(value >> 16).be16(value & 65535); }
Writer& Writer::compact(std::uint32_t value, unsigned code) {
    switch (code) { case 0: return u8(value); case 1: return le16(value); case 2: return le32(value);
        default: throw ProtocolError("reserved compact integer width"); }
}
Writer& Writer::per_length(std::size_t length) {
    require(length <= 32767, "PER length exceeds short framing");
    if (length < 128) return u8(unsigned(length));
    return be16(unsigned(length) | 0x8000);
}
Writer& Writer::ber_length(std::size_t length) {
    require(length <= 0xffffffffULL, "BER length exceeds limit");
    if (length < 128) return u8(unsigned(length));
    unsigned count = 0;
    for (auto n = length; n; n >>= 8) ++count;
    u8(0x80 | count);
    for (unsigned i = count; i; --i) u8(unsigned((length >> ((i - 1) * 8)) & 255));
    return *this;
}
Writer& Writer::tlv(unsigned tag, View value) {
    if (tag > 255) u8(tag >> 8);
    return u8(tag & 255).ber_length(value.size()).raw(value);
}
unsigned compact_width(std::uint32_t value) { return value <= 255 ? 0 : value <= 65535 ? 1 : 2; }
Bytes ber(unsigned tag, View value) { Writer out; out.tlv(tag, value); return std::move(out).finish(); }
Bytes ber_integer(std::uint32_t value, unsigned tag) {
    Writer body;
    unsigned count = 1;
    for (auto n = value; n > 255; n >>= 8) ++count;
    if ((value >> ((count - 1) * 8)) & 0x80) body.u8(0);
    for (unsigned i = count; i; --i) body.u8((value >> ((i - 1) * 8)) & 255);
    return ber(tag, body.bytes());
}
std::uint32_t read_ber_integer(Reader& reader, unsigned tag) {
    Reader value(reader.tlv(tag));
    require(value.remaining() > 0 && value.remaining() <= 5, "invalid BER integer size");
    auto first = value.u8();
    require(!(first & 0x80), "negative BER integer");
    require(value.remaining() != 4 || first == 0, "BER integer overflow");
    std::uint32_t result = first;
    while (!value.empty()) result = result << 8 | value.u8();
    return result;
}
Bytes utf16le(std::string_view input, bool terminated) {
    Writer out;
    for (std::size_t i = 0; i < input.size();) {
        const auto first = static_cast<unsigned char>(input[i++]);
        std::uint32_t cp = 0; unsigned extra = 0; std::uint32_t minimum = 0;
        if (first < 0x80) cp = first;
        else if ((first & 0xe0) == 0xc0) { cp = first & 31; extra = 1; minimum = 0x80; }
        else if ((first & 0xf0) == 0xe0) { cp = first & 15; extra = 2; minimum = 0x800; }
        else if ((first & 0xf8) == 0xf0) { cp = first & 7; extra = 3; minimum = 0x10000; }
        else throw ProtocolError("invalid UTF-8 lead byte");
        require(extra <= input.size() - i, "truncated UTF-8 sequence");
        for (unsigned j = 0; j < extra; ++j) {
            const auto next = static_cast<unsigned char>(input[i++]);
            require((next & 0xc0) == 0x80, "invalid UTF-8 continuation");
            cp = (cp << 6) | (next & 63);
        }
        require(cp >= minimum && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff), "invalid Unicode scalar");
        require(!terminated || cp != 0, "embedded Unicode NUL");
        if (cp <= 65535) out.le16(cp);
        else { cp -= 0x10000; out.le16(0xd800 | (cp >> 10)).le16(0xdc00 | (cp & 1023)); }
    }
    if (terminated) out.le16(0);
    return std::move(out).finish();
}
std::string from_utf16le(View bytes, bool terminated) {
    require(bytes.size() % 2 == 0, "odd UTF-16 length");
    Reader in(bytes); std::string out;
    while (!in.empty()) {
        std::uint32_t cp = in.le16();
        if (terminated && cp == 0) { in.end(); return out; }
        if (cp >= 0xd800 && cp <= 0xdbff) {
            auto low = in.le16(); require(low >= 0xdc00 && low <= 0xdfff, "invalid UTF-16 surrogate pair");
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        } else require(cp < 0xdc00 || cp > 0xdfff, "unpaired UTF-16 low surrogate");
        if (cp < 0x80) out.push_back(char(cp));
        else if (cp < 0x800) { out.push_back(char(0xc0 | cp >> 6)); out.push_back(char(0x80 | (cp & 63))); }
        else if (cp < 0x10000) { out.push_back(char(0xe0 | cp >> 12)); out.push_back(char(0x80 | ((cp >> 6) & 63))); out.push_back(char(0x80 | (cp & 63))); }
        else { out.push_back(char(0xf0 | cp >> 18)); out.push_back(char(0x80 | ((cp >> 12) & 63))); out.push_back(char(0x80 | ((cp >> 6) & 63))); out.push_back(char(0x80 | (cp & 63))); }
    }
    require(!terminated, "missing UTF-16 terminator"); return out;
}
Bytes tpkt(View body) {
    require(body.size() <= 65531, "TPKT exceeds 16-bit framing");
    Writer out(65535); out.raw({3, 0}).be16(unsigned(body.size() + 4)).raw(body); return std::move(out).finish();
}
View parse_tpkt(View packet) {
    Reader in(packet); require(in.u8() == 3 && in.u8() == 0, "invalid TPKT header");
    require(in.be16() == packet.size(), "TPKT length mismatch"); return in.take(in.remaining());
}
Bytes x224_data(View payload) { Writer out; out.raw({2, 0xf0, 0x80}).raw(payload); return tpkt(out.bytes()); }
View parse_x224_data(View payload) {
    Reader in(parse_tpkt(payload)); require(in.u8() == 2 && in.u8() == 0xf0 && in.u8() == 0x80, "invalid X.224 data TPDU");
    return in.take(in.remaining());
}
NegotiationRequest parse_negotiation(View packet) {
    Reader in(parse_tpkt(packet));
    const auto length = in.u8(); require(length == in.remaining(), "X.224 negotiation length mismatch");
    require(in.u8() == 0xe0, "expected X.224 connection request");
    in.skip(4); require(in.u8() == 0, "unsupported X.224 class");
    auto rest = in.take(in.remaining()); Reader n(rest);
    if (!rest.empty() && rest[0] != 1) {
        const auto it = std::search(rest.begin(), rest.end(), "\r\n", "\r\n" + 2);
        require(it != rest.end(), "unterminated negotiation cookie");
        n.skip(std::size_t(it - rest.begin()) + 2);
    }
    NegotiationRequest result;
    if (n.empty()) return result;
    require(n.u8() == 1, "invalid RDP negotiation type"); result.flags = n.u8();
    require(n.le16() == 8, "invalid RDP negotiation size"); result.protocols = n.le32(); result.present = true;
    if (result.flags & 8) {
        require(n.u8() == 6 && n.u8() == 0 && n.le16() == 36, "invalid RDP correlation structure"); n.skip(32);
    }
    n.end(); return result;
}
Bytes negotiation_reply(std::uint32_t value, bool failure, std::uint8_t flags) {
    Writer out; out.raw({14, 0xd0, 0, 0, 0, 0, 0}).u8(failure ? 3 : 2).u8(failure ? 0 : flags).le16(8).le32(value);
    return tpkt(out.bytes());
}
} // namespace lrdp
