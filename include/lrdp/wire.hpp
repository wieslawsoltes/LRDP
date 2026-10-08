#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lrdp {
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

class ProtocolError final : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& message) : std::runtime_error(message) {}
};
void require(bool condition, std::string_view message);

// Wire buffers are never interpreted through packed structs or unaligned casts.
class Reader {
    View bytes_;
    std::size_t offset_ = 0;
public:
    explicit Reader(View bytes) : bytes_(bytes) {}
    [[nodiscard]] std::size_t remaining() const { return bytes_.size() - offset_; }
    [[nodiscard]] std::size_t position() const { return offset_; }
    [[nodiscard]] bool empty() const { return remaining() == 0; }
    View take(std::size_t length);
    void skip(std::size_t length) { (void)take(length); }
    void end() const { require(empty(), "unexpected trailing wire data"); }
    std::uint8_t u8();
    std::uint16_t le16();
    std::uint16_t be16();
    std::uint32_t le32();
    std::int32_t i32();
    std::uint32_t be32();
    std::uint32_t compact(unsigned width_code);
    std::size_t per_length();
    std::size_t ber_length();
    View tlv(unsigned tag);
};

class Writer {
    Bytes bytes_;
    std::size_t limit_;
public:
    explicit Writer(std::size_t limit = 16 * 1024 * 1024) : limit_(limit) {}
    [[nodiscard]] const Bytes& bytes() const { return bytes_; }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    Bytes finish() && { return std::move(bytes_); }
    Writer& raw(View bytes);
    Writer& raw(std::initializer_list<std::uint8_t> bytes) { return raw(View(bytes.begin(), bytes.size())); }
    Writer& zeros(std::size_t length);
    Writer& u8(unsigned value);
    Writer& le16(unsigned value);
    Writer& be16(unsigned value);
    Writer& le32(std::uint32_t value);
    Writer& be32(std::uint32_t value);
    Writer& compact(std::uint32_t value, unsigned width_code);
    Writer& per_length(std::size_t length);
    Writer& ber_length(std::size_t length);
    Writer& tlv(unsigned tag, View value);
};
[[nodiscard]] unsigned compact_width(std::uint32_t value);
[[nodiscard]] Bytes ber(unsigned tag, View value);
[[nodiscard]] Bytes ber_integer(std::uint32_t value, unsigned tag = 2);
[[nodiscard]] std::uint32_t read_ber_integer(Reader& reader, unsigned tag = 2);
[[nodiscard]] Bytes utf16le(std::string_view utf8, bool terminated = true);
[[nodiscard]] std::string from_utf16le(View bytes, bool terminated = true);
[[nodiscard]] Bytes tpkt(View body);
[[nodiscard]] View parse_tpkt(View packet);
[[nodiscard]] Bytes x224_data(View payload);
[[nodiscard]] View parse_x224_data(View payload);

struct NegotiationRequest {
    std::uint32_t protocols = 0;
    std::uint8_t flags = 0;
    bool present = false;
};
[[nodiscard]] NegotiationRequest parse_negotiation(View packet);
[[nodiscard]] Bytes negotiation_reply(std::uint32_t protocol_or_error, bool failure = false,
                                     std::uint8_t flags = 1); // EXTENDED_CLIENT_DATA_SUPPORTED
} // namespace lrdp
