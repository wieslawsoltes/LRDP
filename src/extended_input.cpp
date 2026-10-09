#include "lrdp/input/extended.hpp"
#include <algorithm>
#include <limits>

namespace lrdp {
namespace {
bool known_version(std::uint32_t version) { return version == 0x10000 || version == 0x10001 || version == 0x20000 || version == 0x30000; }
}
ExtendedInput::ExtendedInput(ExtendedCapabilities capabilities) : capabilities_(capabilities) {
    require(capabilities.touches > 0 && capabilities.touches <= 256 && capabilities.pens <= 4, "invalid backend digitizer capabilities");
}
Bytes ExtendedInput::start() {
    require(!started_, "RDPEI already initialized"); started_ = true;
    Writer out; out.le32(capabilities_.pens ? 0x30000U : 0x10001U);
    if (capabilities_.pens) out.le32(capabilities_.pens == 4 ? 1U : 0U);
    return input_pdu(1, out.bytes());
}
void ExtendedInput::cancel() noexcept {
    for (auto* contacts : {&touches_, &pens_}) for (auto& c : *contacts) {
        c.blocked = c.state != State::out || c.blocked; c.state = State::out;
    }
    recovering_ = true;
}
std::optional<Bytes> ExtendedInput::suspend() {
    cancel();
    if (!ready() || suspended_) return {};
    suspended_ = true; return input_pdu(4);
}
std::optional<Bytes> ExtendedInput::resume() {
    if (!ready() || !suspended_) return {};
    suspended_ = false; return input_pdu(5);
}
bool ExtendedInput::transition(ContactState& state, const ExtendedContact& c) {
    bool valid = false; auto next = state.state;
    switch (state.state) {
    case State::out:
        valid = c.flags == 25 || c.flags == 10;
        next = c.flags == 25 ? State::engaged : State::hover; break;
    case State::hover:
        valid = c.flags == 10 || c.flags == 25 || c.flags == 2 || c.flags == 34;
        next = c.flags == 25 ? State::engaged : c.flags == 10 ? State::hover : State::out; break;
    case State::engaged:
        valid = c.flags == 26 || c.flags == 12 || c.flags == 4 || c.flags == 36;
        next = c.flags == 26 ? State::engaged : c.flags == 12 ? State::hover : State::out;
        // MS-RDPEI 3.1.1.1: a release cannot also move the contact.
        if (next != State::engaged && (state.x != c.x || state.y != c.y)) valid = false;
        break;
    }
    if (valid) { state.state = next; state.x = c.x; state.y = c.y; }
    return valid;
}
unsigned ExtendedInput::active_contacts() const {
    unsigned count = 0;
    for (const auto* contacts : {&touches_, &pens_}) for (const auto& c : *contacts) count += c.state != State::out;
    return count;
}
ExtendedResult ExtendedInput::receive(View message, unsigned width, unsigned height) {
    require(started_ && message.size() <= 1024 * 1024, "RDPEI state or message quota");
    Reader in(message); const auto type = in.le16(); require(in.le32() == message.size(), "RDPEI message length mismatch");
    if (type == 2) {
        require(!ready(), "duplicate RDPEI readiness");
        const auto flags = in.le32(), version = in.le32(); const auto contacts = in.le16(); in.end();
        require(known_version(version) && contacts > 0 && (flags & ~7U) == 0, "invalid RDPEI client capability");
        const auto server_version = capabilities_.pens ? 0x30000U : 0x10001U;
        // Each endpoint advertises its own maximum; negotiation is the minimum.
        version_ = std::min(version, server_version); flags_ = flags;
        touch_limit_ = std::min(unsigned(contacts), capabilities_.touches);
        pen_limit_ = version_ >= 0x20000 && capabilities_.pens ? 1U : 0U;
        if (version_ >= 0x30000 && (flags & 4) && capabilities_.pens == 4) pen_limit_ = 4;
        return {};
    }
    require(ready(), "RDPEI input before client readiness");
    if (type == 6) {
        const auto id = in.u8(); in.end();
        if (!suspended_ && touches_[id].state == State::hover) {
            ExtendedContact contact; contact.id = id; contact.x = touches_[id].x; contact.y = touches_[id].y; contact.flags = 2;
            touches_[id].state = State::out; return {false, {{Digitizer::touch, 0, 0, {contact}}}};
        }
        return {};
    }
    // Unknown event IDs are length-checked then ignored per MS-RDPEI 3.1.5.
    if (type != 3 && type != 8) return {};
    auto frames = decode_input_frames(message, touch_limit_, pen_limit_);
    if (suspended_) return {}; // In-flight packets after Suspend are harmless.
    require(width > 0 && height > 0 && width <= 32768 && height <= 32768, "invalid input desktop geometry");
    auto touches = touches_, pens = pens_;
    bool touch_seen = touch_seen_, pen_seen = pen_seen_;
    ExtendedResult result; result.frames.reserve(frames.size());
    std::uint64_t total_offset = 0;
    for (auto& frame : frames) {
        bool& seen = frame.kind == Digitizer::touch ? touch_seen : pen_seen;
        if (flags_ & 2) { frame.offset_us = 0; frame.encode_delay_ms = 0; }
        else {
            require(seen || frame.offset_us == 0, "first digitizer frame must have zero offset");
            require(frame.offset_us <= std::numeric_limits<std::uint64_t>::max() - total_offset, "digitizer time offset overflow");
            total_offset += frame.offset_us;
        }
        seen = true;
        auto& states = frame.kind == Digitizer::touch ? touches : pens;
        const auto limit = frame.kind == Digitizer::touch ? touch_limit_ : pen_limit_;
        std::vector<ExtendedContact> accepted; accepted.reserve(frame.contacts.size());
        for (const auto& contact : frame.contacts) {
            require(contact.x >= 0 && contact.y >= 0 && unsigned(contact.x) < width && unsigned(contact.y) < height,
                    "digitizer coordinates outside the active desktop");
            auto& state = states[contact.id];
            if (state.blocked || (recovering_ && state.state == State::out)) {
                if (contact.flags != 25 && contact.flags != 10) continue;
                state.blocked = false;
            }
            if (!transition(state, contact)) {
                cancel();
                // Mark every ID in the rejected batch as stale, including those
                // not previously live. A fresh DOWN/hover entry can recover it.
                for (const auto& rejected : frames) for (const auto& c : rejected.contacts)
                    (rejected.kind == Digitizer::touch ? touches_ : pens_)[c.id].blocked = true;
                touch_seen_ = touch_seen; pen_seen_ = pen_seen;
                return {true, {}};
            }
            accepted.push_back(contact);
        }
        unsigned active = 0;
        for (const auto& contact : states) active += contact.state != State::out;
        require(active <= limit, "simultaneous digitizer contacts exceed negotiated quota");
        frame.contacts = std::move(accepted);
        if (!frame.contacts.empty()) result.frames.push_back(std::move(frame));
    }
    touches_ = touches; pens_ = pens; touch_seen_ = touch_seen; pen_seen_ = pen_seen;
    return result;
}
} // namespace lrdp
