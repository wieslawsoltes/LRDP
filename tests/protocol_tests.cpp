#include "lrdp/wire.hpp"
#include "lrdp/display.hpp"
#include "lrdp/channels.hpp"
#include "lrdp/clipboard.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <random>

using namespace lrdp;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void rejects(const std::function<void()>& action) {
    ++checks; try { action(); } catch (const ProtocolError&) { return; }
    throw std::runtime_error("expected ProtocolError");
}
Bytes packet(std::initializer_list<std::uint8_t> value) { return Bytes(value); }
void binary() {
    Writer w; w.u8(7).le16(0x1234).be16(0x5678).le32(0xdeadbeef).be32(0x87654321);
    Reader r(w.bytes()); check(r.u8() == 7 && r.le16() == 0x1234 && r.be16() == 0x5678 && r.le32() == 0xdeadbeef && r.be32() == 0x87654321, "integer byte order"); r.end();
    rejects([&]{ (void)r.u8(); }); rejects([]{ Writer(1).le16(1); });
    for (std::uint32_t n : {0U, 1U, 127U, 128U, 255U, 256U, 65535U, 65536U, 0x80000000U, 0xffffffffU}) {
        const auto data = ber_integer(n); Reader in(data); check(read_ber_integer(in) == n, "BER integer roundtrip"); in.end();
        Writer out; out.compact(n, compact_width(n)); Reader back(out.bytes()); check(back.compact(compact_width(n)) == n, "compact integer roundtrip");
    }
    for (unsigned n : {0U, 127U, 128U, 32767U}) { Writer out; out.per_length(n); Reader in(out.bytes()); check(in.per_length() == n, "PER length roundtrip"); }
    rejects([]{ Writer out; out.per_length(32768); });
    rejects([]{ auto b = packet({0x80}); Reader in(b); (void)in.ber_length(); });
    const auto utf = utf16le("Zażółć gęślą jaźń 🚀\n日本語"); check(from_utf16le(utf) == "Zażółć gęślą jaźń 🚀\n日本語", "Unicode roundtrip");
    rejects([]{ (void)utf16le(std::string("\xc0\xaf", 2)); });
    rejects([]{ (void)utf16le(std::string("a\0b", 3)); });
    rejects([]{ auto b = packet({0, 0xd8, 0, 0}); (void)from_utf16le(b); });
    rejects([]{ auto b = packet({0, 0xdc, 0, 0}); (void)from_utf16le(b); });
    rejects([]{ auto b = packet({65, 0}); (void)from_utf16le(b); });
    for (std::size_t n = 0; n < utf.size(); ++n) rejects([&]{ (void)from_utf16le(View(utf).first(n)); });
    Writer cr; cr.raw({14, 0xe0, 0, 0, 0, 0, 0, 1, 0, 8, 0}).le32(3);
    const auto request = tpkt(cr.bytes()); const auto neg = parse_negotiation(request);
    check(neg.present && neg.protocols == 3 && neg.flags == 0, "negotiation request");
    for (std::size_t n = 0; n < request.size(); ++n) rejects([&]{ (void)parse_negotiation(View(request).first(n)); });
    const auto data = x224_data(packet({1,2,3})); const auto body = parse_x224_data(data);
    check(body.size() == 3 && body[2] == 3, "TPKT X224 framing");
}
void display() {
    auto a = Monitor{}; auto b = a; b.flags = 0; b.left = -1280;
    auto layout = validate_layout({a,b}); auto bytes = encode_layout(layout); auto decoded = decode_layout(bytes);
    check(decoded.width == 2560 && decoded.left == -1280 && decoded.monitors == layout.monitors, "negative-origin monitor layout");
    for (std::size_t n = 0; n < bytes.size(); ++n) rejects([&]{ (void)decode_layout(View(bytes).first(n)); });
    rejects([&]{ auto m = a; m.width = 201; (void)validate_layout({m}); });
    rejects([&]{ (void)validate_layout({a,a}); });
    rejects([&]{ auto m = b; m.left = 1; (void)validate_layout({a,m}); });
    rejects([&]{ auto m = b; m.left = 2147483647; (void)validate_layout({a,m}); });
    DisplayController controller(validate_layout({a})); controller.request(bytes);
    check(!controller.commit([](const Layout&) { return false; }) && controller.current().width == 1280 && controller.generation() == 0, "failed resize preserves state");
    controller.request(bytes); check(controller.commit([](const Layout&) { return true; }) && controller.generation() == 1, "successful resize commits");
    auto m = a; m.desktop_scale = 99; m.device_scale = 200; m.physical_width = 1;
    auto normalized = validate_layout({m}); check(normalized.monitors[0].desktop_scale == 100 && normalized.monitors[0].physical_width == 0, "display invalid optional attributes ignored");
}
void channels() {
    Bytes source(9876); for (std::size_t i = 0; i < source.size(); ++i) source[i] = std::uint8_t(i);
    ChannelAssembler assembler; std::optional<Bytes> complete;
    for (const auto& pdu : channel_fragments(source)) complete = assembler.accept(pdu);
    check(complete && *complete == source, "static channel fragmentation");
    check(assembler.accept(channel_fragments({})[0])->empty(), "empty static message");
    rejects([&]{ auto pdu = channel_fragments(source)[1]; (void)assembler.accept(pdu); }); assembler.reset();
    auto fragments = channel_fragments(source); (void)assembler.accept(fragments[0]);
    rejects([&]{ (void)assembler.accept(fragments[0]); }); assembler.reset();
    ChannelAssembler tiny(8); rejects([&]{ (void)tiny.accept(fragments[0]); });
    DynamicChannels dvc; auto ready = dvc.accept(packet({0x50, 0, 1, 0})); check(ready.size() == 1 && dvc.ready(), "DVC capabilities");
    const auto create = dvc.create(70000, "Microsoft::Windows::RDS::DisplayControl"); check(create[0] == 0x12, "32-bit DVC IDs");
    Writer response; response.u8(0x12).le32(70000).le32(1); auto opened = dvc.accept(response.bytes());
    check(opened[0].kind == DvcEventKind::opened, "nonnegative HRESULT succeeds");
    std::vector<DvcEvent> events;
    for (const auto& pdu : dvc.send(70000, source)) {
        check(pdu.size() <= 1600, "DVC max fragment size");
        auto next = dvc.accept(pdu); events.insert(events.end(), next.begin(), next.end());
    }
    check(events.size() == 1 && events[0].data == source && dvc.buffered() == 0, "DVC bounded reassembly");
    auto first = dvc.send(70000, source)[0]; (void)dvc.accept(first);
    rejects([&]{ (void)dvc.accept(first); });
    auto close = dvc.close(70000); check(dvc.buffered() == 0 && close[0] == 0x42, "DVC close discards partial data");
    rejects([&]{ (void)dvc.accept(packet({0x33, 0})); });
}
void clipboard() {
    Clipboard clip; auto initial = clip.start(); check(initial.size() == 2 && initial[0][0] == 7 && initial[1][0] == 1, "clipboard capability ordering");
    Writer cap; cap.le16(1).le16(0).le16(1).le16(12).le32(2).le32(2);
    (void)clip.accept(clipboard_pdu(7, 0, cap.bytes()));
    auto offer = clip.set_local("hello\n世界"); check(offer.size() == 1 && offer[0].size() == 14, "long format name offer");
    auto ack = clip.accept(clipboard_pdu(3, 1)); check(ack.outbound.empty(), "format list ACK");
    Writer req; req.le32(13); auto text = clip.accept(clipboard_pdu(4, 0, req.bytes()));
    Reader payload(text.outbound[0]); payload.skip(8); check(from_utf16le(payload.take(payload.remaining())) == "hello\r\n世界", "clipboard CRLF conversion");
    Writer formats; formats.le32(13).le16(0);
    auto remote = clip.accept(clipboard_pdu(2, 0, formats.bytes())); check(remote.outbound.size() == 2, "format ACK and data request");
    // Simulate a newer offer while an older uncorrelated data request is outstanding.
    (void)clip.accept(clipboard_pdu(2, 0, formats.bytes()));
    auto stale = clip.accept(clipboard_pdu(5, 1, utf16le("stale"))); check(!stale.remote_text && stale.outbound.size() == 1, "stale clipboard completion discarded");
    auto fresh = clip.accept(clipboard_pdu(5, 1, utf16le("fresh\r\n🚀"))); check(fresh.remote_text == "fresh\n🚀", "fresh clipboard generation accepted");
    rejects([&]{ (void)clip.accept(clipboard_pdu(5, 1, utf16le("unsolicited"))); });
    Writer unsupported; unsupported.le32(999); auto no = clip.accept(clipboard_pdu(4, 0, unsupported.bytes())); check(no.outbound[0][2] == 2, "unknown format fails explicitly");
}
void adversarial() {
    std::mt19937 random(0x4c524450);
    for (unsigned trial = 0; trial < 20000; ++trial) {
        Bytes bytes(random() % 512); for (auto& b : bytes) b = std::uint8_t(random());
        try { (void)decode_layout(bytes); } catch (const ProtocolError&) {}
        try { (void)parse_negotiation(bytes); } catch (const ProtocolError&) {}
        try { ChannelAssembler a(4096); (void)a.accept(bytes); } catch (const ProtocolError&) {}
        try { DynamicChannels a; (void)a.accept(bytes); } catch (const ProtocolError&) {}
        try { Clipboard c; (void)c.start(); (void)c.accept(bytes); } catch (const ProtocolError&) {}
    }
    check(true, "20000 deterministic malformed-input probes completed");
}
}
int main() {
    try { binary(); display(); channels(); clipboard(); adversarial(); std::cout << checks << " checks passed; 100000 malformed-parser invocations completed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1; }
}
