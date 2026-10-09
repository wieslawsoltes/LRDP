#include "lrdp/monitor_layout.hpp"
#include "lrdp/mcs.hpp"
#include <iostream>
#include <random>
using namespace lrdp;
template<class F> void rejects(F f) { try { f(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected rejection"); }
int main() {
    try {
        Writer def; def.le32(0).le32(2).le32(0).le32(0).le32(799).le32(599).le32(1)
            .le32(std::uint32_t(-800)).le32(0).le32(std::uint32_t(-1)).le32(599).le32(0);
        Writer attrs; attrs.le32(0).le32(20).le32(2).le32(300).le32(200).le32(0).le32(150).le32(140)
            .le32(0).le32(200).le32(45).le32(99).le32(180);
        const auto decoded = decode_initial_monitors(def.bytes(),attrs.bytes());
        require(decoded.width == 1600 && decoded.height == 600 && decoded.left == -800,"initial negative layout");
        require(decoded.monitors[0].desktop_scale == 150 && decoded.monitors[0].device_scale == 140,"extended monitor scales");
        require(decoded.monitors[1].physical_height == 0 && decoded.monitors[1].orientation == 0 && decoded.monitors[1].device_scale == 100,"invalid paired attributes must be ignored");
        for (std::size_t n = 0; n < def.size(); ++n) rejects([&]{ (void)decode_initial_monitors(View(def.bytes()).first(n)); });
        for (std::size_t n = 1; n < attrs.size(); ++n) rejects([&]{ (void)decode_initial_monitors(def.bytes(),View(attrs.bytes()).first(n)); });
        auto mismatch = attrs.bytes(); mismatch[8] = 1; rejects([&]{ (void)decode_initial_monitors(def.bytes(),mismatch); });
        const auto pdu = monitor_layout_pdu(decoded); Reader wire(pdu);
        require(wire.le16() == pdu.size() && wire.le16() == 0x17 && wire.le16() == 0,"monitor layout source must be zero");
        wire.skip(8); require(wire.u8() == 55,"monitor layout type"); wire.skip(3);
        require(wire.le32() == 2 && wire.i32() == 0 && wire.i32() == 0 && wire.i32() == 799 && wire.i32() == 599 && wire.le32() == 1,"inclusive primary edges");
        require(wire.i32() == -800 && wire.i32() == 0 && wire.i32() == -1 && wire.i32() == 599 && wire.le32() == 0,"negative secondary edges"); wire.end();
        const auto demand = demand_active(640,480,24,true); Reader active(demand);
        active.skip(10); const auto descriptor = active.le16(), combined = active.le16(); active.skip(descriptor);
        Reader caps(active.take(combined)); const auto count = caps.le16(); caps.skip(2);
        require(count == 10,"server capability count");
        for (unsigned i = 0; i < count; ++i) {
            const auto kind = caps.le16(), size = caps.le16(); Reader cap(caps.take(size-4));
            if (kind == 1) { require(size == 24,"general capability length"); cap.skip(18); require(cap.u8() == 1 && cap.u8() == 1,"refresh/suppress capability offsets"); cap.end(); }
        }
        caps.end();
        std::mt19937 random(19);
        for (unsigned n = 0; n < 10000; ++n) { Bytes bytes(random()%700); for (auto& b : bytes) b = std::uint8_t(random());
            try { (void)decode_initial_monitors(bytes,bytes); } catch (const ProtocolError&) {} }
        std::cout << "PASS: initial monitor topology, attributes, zero-source layout PDU, truncation and 10000 malformed inputs\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
