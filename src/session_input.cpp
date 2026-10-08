#include "lrdp/session.hpp"
namespace lrdp {
void Session::dispatch_input(const std::vector<InputEvent>& events) {
    // Never apply old-client coordinates to a desktop whose layout has changed.
    const bool coordinates_valid = active() && active_layout_.monitors == desktop_->layout().monitors;
    if (!coordinates_valid) {
        for (const auto& event : events) if (event.kind == InputKind::synchronize) desktop_->release_input();
        return;
    }
    for (const auto& event : events) desktop_->input(event);
}
void Session::input_slow(View payload) {
    Reader in(payload); const auto count = in.le16(); in.skip(2);
    require(count <= 255 && in.remaining() == std::size_t(count) * 12, "invalid slow-path input batch");
    std::vector<InputEvent> events; events.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        in.skip(4); const auto type = in.le16(); InputEvent event{};
        if (type == 4 || type == 5) {
            event.kind = type == 4 ? InputKind::scancode : InputKind::unicode;
            event.flags = in.le16(); event.code = in.le16(); in.skip(2);
            require((event.flags & ~(type == 4 ? 0xc300U : 0xc000U)) == 0, "invalid slow keyboard flags");
        } else if (type == 0x8001 || type == 0x8002) {
            event.kind = type == 0x8001 ? InputKind::pointer : InputKind::pointer_extended;
            event.flags = in.le16(); event.x = in.le16(); event.y = in.le16();
        } else if (type == 0) {
            event.kind = InputKind::synchronize; in.skip(2); const auto flags = in.le32(); require(flags <= 15, "invalid toggle state"); event.flags = std::uint16_t(flags);
        } else throw ProtocolError("unsupported slow-path input event");
        require(event.kind != InputKind::unicode || desktop_->unicode_input(), "Unicode input unavailable on this backend"); events.push_back(event);
    }
    dispatch_input(events);
}
void Session::input_fast(View packet) {
    Reader in(packet); const auto header = in.u8(); require((header & 0xc3) == 0, "invalid TLS fast-path input header");
    require(in.per_length() == packet.size(), "fast-path input length mismatch");
    unsigned count = (header >> 2) & 15; if (!count) count = in.u8(); require(count > 0, "empty fast-path input batch");
    std::vector<InputEvent> events; events.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto code = in.u8(); const unsigned kind = code >> 5, flags = code & 31; InputEvent event{};
        if (kind == 0 || kind == 4) {
            require((flags & ~(kind == 0 ? 7U : 1U)) == 0, "invalid fast-path keyboard flags");
            event.kind = kind == 0 ? InputKind::scancode : InputKind::unicode;
            event.flags = std::uint16_t((flags & 1 ? 0x8000 : 0) | (flags & 2 ? 0x100 : 0) | (flags & 4 ? 0x200 : 0));
            event.code = kind == 0 ? in.u8() : in.le16();
        } else if (kind == 1 || kind == 2) {
            require(flags == 0, "nonzero reserved fast-path pointer flags");
            event.kind = kind == 1 ? InputKind::pointer : InputKind::pointer_extended;
            event.flags = in.le16(); event.x = in.le16(); event.y = in.le16();
        } else if (kind == 3) {
            require(flags <= 15, "invalid sync toggles"); event.kind = InputKind::synchronize; event.flags = std::uint16_t(flags);
        } else throw ProtocolError("unnegotiated fast-path input event");
        require(event.kind != InputKind::unicode || desktop_->unicode_input(), "Unicode input unavailable on this backend"); events.push_back(event);
    }
    in.end(); dispatch_input(events);
}
} // namespace lrdp
