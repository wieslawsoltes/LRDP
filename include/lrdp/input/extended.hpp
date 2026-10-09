#pragma once
#include "lrdp/wire.hpp"
#include <array>
#include <optional>

namespace lrdp {
// MS-RDPEI 2.2.2: prefix-length, big-endian payload integers. Signed values
// are sign/magnitude, not LEB128, zig-zag, or two's complement.
std::uint64_t input_unsigned(Reader& in, unsigned maximum_bytes);
std::int32_t input_signed(Reader& in, unsigned maximum_bytes);
void input_unsigned(Writer& out, std::uint64_t value, unsigned maximum_bytes);
void input_signed(Writer& out, std::int32_t value, unsigned maximum_bytes);
Bytes input_pdu(std::uint16_t event, View payload = {});

enum class Digitizer { touch, pen };
struct ContactRect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const ContactRect&) const = default;
};
struct ExtendedContact {
    std::uint8_t id = 0;
    std::int32_t x = 0, y = 0;
    std::uint32_t flags = 0;
    std::optional<ContactRect> rectangle;
    std::optional<std::uint32_t> orientation, pressure, pen_flags, rotation;
    std::optional<std::int32_t> tilt_x, tilt_y;
    bool operator==(const ExtendedContact&) const = default;
};
struct ExtendedFrame {
    Digitizer kind = Digitizer::touch;
    std::uint32_t encode_delay_ms = 0;
    std::uint64_t offset_us = 0;
    std::vector<ExtendedContact> contacts;
    bool operator==(const ExtendedFrame&) const = default;
};
struct ExtendedCapabilities {
    unsigned touches = 0, pens = 0;
};
struct ExtendedResult {
    // Cancellation applies before frames. A malformed contact transaction never
    // commits a prefix of a batch to either the state machine or the backend.
    bool cancel = false;
    std::vector<ExtendedFrame> frames;
};
std::vector<ExtendedFrame> decode_input_frames(View message, unsigned max_touches, unsigned max_pens);

class ExtendedInput final {
    enum class State { out, hover, engaged };
    struct ContactState { State state = State::out; std::int32_t x = 0, y = 0; bool blocked = false; };
    using Contacts = std::array<ContactState, 256>;
    ExtendedCapabilities capabilities_;
    Contacts touches_{}, pens_{};
    std::uint32_t version_ = 0, flags_ = 0;
    unsigned touch_limit_ = 0, pen_limit_ = 0;
    bool started_ = false, suspended_ = false, touch_seen_ = false, pen_seen_ = false;
    bool recovering_ = false;
    static bool transition(ContactState& state, const ExtendedContact& contact);
public:
    explicit ExtendedInput(ExtendedCapabilities capabilities);
    Bytes start();
    ExtendedResult receive(View message, unsigned width, unsigned height);
    std::optional<Bytes> suspend();
    std::optional<Bytes> resume();
    void cancel() noexcept;
    [[nodiscard]] bool ready() const { return version_ != 0; }
    [[nodiscard]] bool suspended() const { return suspended_; }
    [[nodiscard]] unsigned active_contacts() const;
    [[nodiscard]] bool touch_visuals() const { return (flags_ & 1) != 0; }
    [[nodiscard]] std::uint32_t version() const { return version_; }
};
} // namespace lrdp
