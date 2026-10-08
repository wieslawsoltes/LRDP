#include "lrdp/graphics.hpp"
#include "lrdp/video.hpp"
#include <atomic>
#include <iostream>
#include <random>
#include <thread>
using namespace lrdp;
namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F action) { try { action(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected protocol rejection"); }
Bytes unwrap(View packet) {
    Reader in(packet); Writer out;
    const auto kind = in.u8();
    if (kind == 0xe0) { check(in.u8() == 4, "single segment flags"); out.raw(in.take(in.remaining())); }
    else {
        check(kind == 0xe1, "multipart descriptor"); const auto count = in.le16(); const auto size = in.le32();
        for (unsigned i = 0; i < count; ++i) { const auto n = in.le32(); check(n > 0 && in.u8() == 4, "multipart size/flags"); out.raw(in.take(n - 1)); }
        check(out.size() == size, "multipart uncompressed length"); in.end();
    }
    return std::move(out).finish();
}
Bytes caps(unsigned flags) { Writer w; w.le16(1).le32(0x80105).le32(4).le32(flags); return graphics_pdu(0x12, w.bytes()); }
void acknowledge(Graphics& g, unsigned id, unsigned depth = 0) { Writer w; w.le32(depth).le32(id).le32(id); (void)g.receive(graphics_pdu(13, w.bytes()), true); }
void graphics() {
    for (unsigned size : {0U,1U,65535U,65536U,200000U}) {
        Bytes original(size); for (unsigned i = 0; i < size; ++i) original[i] = std::uint8_t(i);
        check(unwrap(graphics_segments(original)) == original, "segmented graphics roundtrip");
    }
    Graphics g; check(g.receive(caps(0x10), true) && g.avc420(), "AVC420 capability selection");
    const auto selected = g.drain(); check(selected.size() == 1, "one confirmation");
    const auto raw = unwrap(selected[0]); Reader confirm(raw); check(confirm.le16() == 0x13, "capabilities confirmation command");
    rejects([&]{ (void)g.receive(caps(0x10), true); });
    Monitor monitor; monitor.width = 640; monitor.height = 480; const auto layout = validate_layout({monitor});
    g.reset(layout); const auto setup = g.drain(); check(setup.size() == 3 && unwrap(setup[0]).size() == 340, "fixed reset graphics size");
    Frame frame{640,480,Bytes(640*480*4,37)};
    g.raw_frame(frame); check(g.in_flight() == 1, "frame acknowledgement tracking");
    const auto packets = g.drain(); check(packets.size() == 10, "frame start/eight bands/end");
    for (std::size_t index = 1; index + 1 < packets.size(); ++index) {
        const auto body = unwrap(packets[index]); Reader wire(body);
        check(wire.le16() == 1 && wire.le16() == 0 && wire.le32() == body.size(), "surface PDU framing");
        check(wire.le16() == 0 && wire.le16() == 0 && wire.u8() == 0x20, "raw surface codec");
        check(wire.le16() == 0, "surface left"); const auto top = wire.le16(); check(wire.le16() == 640, "exclusive rectangle right");
        const auto bottom = wire.le16(); const auto n = wire.le32();
        check(top == (index - 1)*64 && n == std::uint32_t(bottom-top)*640*4, "band geometry");
        const auto bits = wire.take(n); for (auto byte : bits) check(byte == 37, "uncompressed BGRA bytes"); wire.end();
    }
    g.raw_frame(frame); check(!g.can_send(), "two-frame bounded window"); rejects([&]{ g.raw_frame(frame); });
    acknowledge(g, 1); check(g.can_send() && g.in_flight() == 1, "frame acknowledgement opens slot");
    rejects([&]{ acknowledge(g, 100); });
    acknowledge(g, 2); (void)g.drain();
    g.video_frame(Bytes{0,0,0,1,0x65,0x88},640,480);
    auto video = g.drain(); check(video.size() == 3, "AVC420 frame framing");
    const auto surface = unwrap(video[1]); Reader v(surface); v.skip(10); check(v.le16() == 0x0b, "AVC420 codec identifier");
    v.skip(9); const auto encoded = v.le32(); check(encoded == 20, "AVC metadata size");
    check(v.le32() == 1 && v.le16() == 0 && v.le16() == 0 && v.le16() == 640 && v.le16() == 480 && v.u8() == 22 && v.u8() == 75, "AVC420 region and quantizer");
    acknowledge(g, 3, 0xffffffffU); check(g.in_flight() == 0 && g.can_send(), "acknowledgement suspension");
    Graphics raw_only; check(raw_only.receive(caps(0x10), false) && !raw_only.avc420(), "do not advertise unavailable encoder");
    for (unsigned n = 0; n < caps(0x10).size(); ++n) rejects([&]{ Graphics value; (void)value.receive(View(caps(0x10)).first(n),true); });
}
class FakeEncoder final : public VideoEncoder {
public:
    EncodedVideo encode(const Frame& frame, bool key) override { return {{0,0,1,0x65},frame.width,frame.height,key,false,"fixture"}; }
};
void worker() {
    const auto owner = std::this_thread::get_id(); std::atomic<unsigned> creates = 0;
    VideoWorker w([&](unsigned,unsigned) {
        check(std::this_thread::get_id() != owner, "factory must execute off network thread"); ++creates;
        return std::make_unique<FakeEncoder>();
    });
    auto await = [&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;) {
            if (auto value = w.take()) return *value;
            check(std::chrono::steady_clock::now() < deadline,"encoder worker deadline"); std::this_thread::yield();
        }
    };
    Frame f{32,32,Bytes(32*32*4)}; w.submit(f,7,false);
    rejects([&]{ w.submit(f,8,false); });
    auto first = await(); check(first.frame && first.generation == 7 && first.frame->key_frame && creates == 1,"worker completion ownership/keyframe");
    check(w.available(),"completion consumption frees slot");
    f.width = 64; f.bgra.resize(64*32*4); w.submit(f,8,false);
    auto second = await(); check(second.frame && second.generation == 8 && second.frame->key_frame && creates == 2,"resize rebuilds encoder");
    VideoWorker fail([](unsigned,unsigned) -> std::unique_ptr<VideoEncoder> { throw ProtocolError("driver rejected device"); });
    fail.submit(f,9,false); const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        if (auto result = fail.take()) { check(!result->frame && result->error == "driver rejected device", "driver failure crosses worker boundary"); break; }
        check(std::chrono::steady_clock::now() < deadline,"driver failure completion deadline"); std::this_thread::yield();
    }
}
}
int main() {
    try { graphics(); worker(); std::cout << "PASS: segmented GFX, raw bands, AVC420 metadata, ACK backpressure, malformed headers, generation-tagged single-slot worker\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
