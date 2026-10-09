#include "lrdp/input/extended.hpp"
#include <algorithm>
#include <bitset>
#include <limits>

namespace lrdp {
namespace {
unsigned prefix_bits(unsigned maximum) {
    require(maximum == 2 || maximum == 4 || maximum == 8, "invalid RDPEI integer width");
    return maximum == 2 ? 1U : maximum == 4 ? 2U : 3U;
}
std::uint64_t magnitude(Reader& in, unsigned first, unsigned tail, unsigned bits) {
    std::uint64_t value = first & ((1U << bits) - 1);
    for (unsigned i = 0; i < tail; ++i) value = (value << 8) | in.u8();
    return value;
}
void integer(Writer& out, std::uint64_t value, unsigned maximum, bool negative, bool sign) {
    const auto prefix = prefix_bits(maximum), bits = 8U - prefix - unsigned(sign);
    require(!sign || maximum != 8, "RDPEI has no signed 8-byte integer");
    const auto capacity = bits + (maximum - 1) * 8;
    require(value < (std::uint64_t(1) << capacity), "RDPEI integer overflow");
    unsigned tail = 0;
    while (value >= (std::uint64_t(1) << (bits + tail * 8))) ++tail;
    out.u8((tail << (8 - prefix)) | (negative ? 1U << bits : 0U) | unsigned(value >> (tail * 8)));
    for (unsigned i = tail; i; --i) out.u8(unsigned((value >> ((i - 1) * 8)) & 255));
}
bool contact_flags(std::uint32_t flags) {
    // The eight combinations in MS-RDPEI 2.2.3.3.1.1 and 2.2.3.7.1.1.
    return flags == 4 || flags == 36 || flags == 2 || flags == 34 ||
           flags == 25 || flags == 26 || flags == 12 || flags == 10;
}
}
std::uint64_t input_unsigned(Reader& in, unsigned maximum) {
    const auto prefix = prefix_bits(maximum); const auto first = in.u8();
    return magnitude(in, first, first >> (8 - prefix), 8 - prefix);
}
std::int32_t input_signed(Reader& in, unsigned maximum) {
    require(maximum == 2 || maximum == 4, "invalid signed RDPEI width");
    const auto prefix = prefix_bits(maximum), bits = 7U - prefix; const auto first = in.u8();
    const auto value = std::int32_t(magnitude(in, first, first >> (8 - prefix), bits));
    return first & (1U << bits) ? -value : value;
}
void input_unsigned(Writer& out, std::uint64_t value, unsigned maximum) { integer(out, value, maximum, false, false); }
void input_signed(Writer& out, std::int32_t value, unsigned maximum) {
    const auto absolute = value < 0 ? std::uint64_t(-std::int64_t(value)) : std::uint64_t(value);
    integer(out, absolute, maximum, value < 0, true);
}
Bytes input_pdu(std::uint16_t event, View payload) {
    require(payload.size() <= 1024 * 1024 - 6, "RDPEI PDU exceeds policy");
    Writer out; out.le16(event).le32(std::uint32_t(payload.size() + 6)).raw(payload); return std::move(out).finish();
}
std::vector<ExtendedFrame> decode_input_frames(View message, unsigned max_touches, unsigned max_pens) {
    require(message.size() <= 1024 * 1024, "RDPEI message exceeds allocation quota");
    Reader in(message); const auto type = in.le16(); require(type == 3 || type == 8, "expected touch or pen input");
    require(in.le32() == message.size(), "RDPEI PDU length mismatch");
    const bool pen = type == 8;
    const auto limit = pen ? max_pens : max_touches;
    require(limit > 0 && limit <= (pen ? 4U : 256U), "digitizer was not negotiated");
    const auto delay = std::uint32_t(input_unsigned(in, 4));
    const auto count = input_unsigned(in, 2); require(count > 0 && count <= 64, "RDPEI frame count exceeds policy");
    std::vector<ExtendedFrame> frames; frames.reserve(std::size_t(count));
    unsigned total = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto contacts = input_unsigned(in, 2);
        require(contacts > 0 && contacts <= limit && contacts <= 1024 - total, "RDPEI contact count exceeds policy");
        total += unsigned(contacts);
        ExtendedFrame frame; frame.kind = pen ? Digitizer::pen : Digitizer::touch;
        frame.encode_delay_ms = delay; frame.offset_us = input_unsigned(in, 8); frame.contacts.reserve(std::size_t(contacts));
        std::bitset<256> seen;
        for (std::uint64_t j = 0; j < contacts; ++j) {
            ExtendedContact c; c.id = in.u8();
            require(!seen.test(c.id) && (!pen || max_pens != 1 || c.id == 0), "duplicate or unnegotiated digitizer ID"); seen.set(c.id);
            const auto fields = input_unsigned(in, 2); require((fields & ~(pen ? 31ULL : 7ULL)) == 0, "unknown digitizer fields");
            c.x = input_signed(in, 4); c.y = input_signed(in, 4); c.flags = std::uint32_t(input_unsigned(in, 4));
            require(contact_flags(c.flags), "invalid digitizer contact flag combination");
            if (!pen) {
                if (fields & 1) {
                    ContactRect r; r.left = input_signed(in, 2); r.top = input_signed(in, 2);
                    r.right = input_signed(in, 2); r.bottom = input_signed(in, 2);
                    require(r.left <= r.right && r.top <= r.bottom, "inverted touch contact rectangle"); c.rectangle = r;
                }
                if (fields & 2) { c.orientation = std::uint32_t(input_unsigned(in, 4)); require(*c.orientation <= 359, "invalid touch orientation"); }
                if (fields & 4) c.pressure = std::uint32_t(input_unsigned(in, 4));
            } else {
                if (fields & 1) { c.pen_flags = std::uint32_t(input_unsigned(in, 4)); require((*c.pen_flags & ~7U) == 0, "unknown pen flags"); }
                if (fields & 2) c.pressure = std::uint32_t(input_unsigned(in, 4));
                if (fields & 4) { c.rotation = std::uint32_t(input_unsigned(in, 2)); require(*c.rotation <= 359, "invalid pen rotation"); }
                if (fields & 8) c.tilt_x = input_signed(in, 2);
                if (fields & 16) c.tilt_y = input_signed(in, 2);
                require((!c.tilt_x || (*c.tilt_x >= -90 && *c.tilt_x <= 90)) &&
                        (!c.tilt_y || (*c.tilt_y >= -90 && *c.tilt_y <= 90)), "invalid pen tilt");
            }
            require(!c.pressure || *c.pressure <= 1024, "invalid contact pressure");
            frame.contacts.push_back(c);
        }
        frames.push_back(std::move(frame));
    }
    in.end(); return frames;
}
} // namespace lrdp
