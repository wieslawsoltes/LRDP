#include "lrdp/graphics.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
using namespace lrdp;
namespace {
unsigned checks = 0;
void check(bool value, const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F f) {
    ++checks; try { f(); } catch (const ProtocolError&) { return; }
    throw std::runtime_error("expected graphics protocol rejection");
}
Bytes offer(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> sets) {
    Writer w; w.le16(unsigned(sets.size()));
    for (auto [version, flags] : sets) {
        w.le32(version).le32(version == 0xa0100 ? 16 : 4);
        if (version == 0xa0100) w.zeros(16); else w.le32(flags);
    }
    return graphics_pdu(0x12, w.bytes());
}
Bytes qoe(unsigned frame, unsigned timestamp, unsigned decode = 7, unsigned render = 9) {
    Writer w; w.le32(frame).le32(timestamp).le16(decode).le16(render);
    return graphics_pdu(0x16, w.bytes());
}
void ack(Graphics& g, unsigned frame, unsigned depth = 0) {
    Writer w; w.le32(depth).le32(frame).le32(frame);
    check(g.receive(graphics_pdu(13, w.bytes()), false), "normal ACK accepted");
}
Layout layout() { Monitor m; m.width = m.height = 200; return validate_layout({m}); }
Frame image() { return {200, 200, Bytes(200*200*4, 128)}; }
void caps() {
    for (auto version : {0xa0301U, 0xa0400U, 0xa0502U, 0xa0600U, 0xa0701U}) {
        for (bool encoder : {false, true}) for (unsigned flags : {0U, 0x20U, 0x40U}) {
            Graphics g; check(g.receive(offer({{version, flags}}), encoder), "modern capability accepted");
            check(g.version() == version && g.negotiation_generation() == 1, "exact offered version");
            check(g.video_enabled() == (encoder && !(flags & 0x20)), "AVC availability intersection");
            if (g.video_enabled()) check(g.video_codec() == VideoCodec::avc444, "conservative full-chroma codec");
            auto out = g.drain(); check(out.size() == 1 && out[0].size() == 22, "confirmation size");
            Reader r(out[0]); check(r.u8() == 0xe0 && r.u8() == 4, "uncompressed graphics segment");
            r.skip(8); check(r.le32() == version && r.le32() == 4, "confirmation capability header");
            const auto confirmed = r.le32(); r.end();
            check(((confirmed & 0x20) != 0) == !g.video_enabled(), "AVC disabled flag");
            check(((confirmed & 0x80) != 0) == (version == 0xa0701), "scaled mapping disabled only in 10.7");
            check(!(confirmed & 0x40), "client preference is not reflected as a server promise");
        }
        Graphics invalid; rejects([&] { invalid.receive(offer({{version, 0x60}}), true); });
        check(invalid.drain().empty() && invalid.negotiation_generation() == 0, "invalid flags transactional");
    }
    for (bool reverse : {false, true}) {
        Graphics g; auto p = reverse ? offer({{0xa0701,0},{0xa0301,0},{0xa0200,0}})
                                   : offer({{0xa0200,0},{0xa0301,0},{0xa0701,0}});
        check(g.receive(p, true) && g.version() == 0xa0701, "version ties independent of offer ordering");
    }
    Graphics unknown; check(!unknown.receive(offer({{0xa0601,0},{0xb0300,0}}), true), "unknown versions not inferred");
}
void telemetry() {
    Graphics g; g.receive(offer({{0xa0002,0}}), false); g.reset(layout()); (void)g.drain();
    g.raw_frame(image()); g.raw_frame(image()); (void)g.drain();
    check(g.in_flight() == 2 && !g.can_send(), "normal frame credit bound");
    check(g.receive(qoe(1, 0xfffffff0), false), "required v10 QoE parsed");
    check(g.receive(qoe(2, 0x10, 0, 65535), false), "timestamp rollover accepted");
    const auto& s = g.telemetry();
    check(s.qoe_samples == 2 && s.latest_qoe->client_elapsed_ms == 32, "relative timestamp unwrap");
    check(s.latest_qoe->decode_ms == 0 && s.latest_qoe->render_ms == 65535, "raw timing semantics retained");
    check(g.in_flight() == 2 && !g.can_send(), "QoE must not release credits");
    g.receive(qoe(1,123), false);
    check(g.telemetry().latest_qoe->frame_id == 2, "late sample does not rewind telemetry");
    const auto before = g.telemetry().qoe_samples;
    auto malformed = qoe(2,20); malformed.push_back(0); malformed[4]++;
    rejects([&] { g.receive(malformed,false); });
    check(g.telemetry().qoe_samples == before, "malformed QoE leaves state untouched");
    rejects([&] { g.receive(qoe(3,100),false); });
    ack(g,2); g.raw_frame(image()); (void)g.drain(); g.receive(qoe(3,0),false);
    check(!g.telemetry().latest_qoe->client_elapsed_ms && g.telemetry().timestamp_discontinuities == 1,
          "ambiguous backward clocks are not presented as measured elapsed time");
    for (unsigned n=0; n<qoe(3,100).size(); ++n) {
        auto p=qoe(3,100); rejects([&] { g.receive(View(p).first(n),false); });
    }
    for (auto version : {0x80105U, 0xa0100U}) {
        Graphics unsupported; unsupported.receive(offer({{version,16}}), true);
        rejects([&] { unsupported.receive(qoe(0,0),true); });
    }
}
void renegotiation() {
    Graphics g; g.receive(offer({{0xa0701,0}}),true); g.reset(layout()); (void)g.drain();
    g.raw_frame(image()); g.raw_frame(image());
    check(!g.can_send(), "pending pre-reset frames");
    const auto epoch=g.negotiation_generation();
    auto bad=offer({{0xa0701,0},{0xa0701,0}});
    rejects([&] { g.receive(bad,true); });
    check(g.negotiation_generation()==epoch && g.in_flight()==2 && g.ready(), "bad reset does not discard live state");
    g.receive(offer({{0xa0502,0x20}}),true);
    check(g.version()==0xa0502 && !g.ready() && !g.video_enabled() && g.in_flight()==0,
          "readvertisement clears surfaces, encoder selection and old credits");
    auto replies=g.drain(); check(replies.size()==1, "no old unsent frame prefixes survive confirmation");
    g.reset(layout()); g.raw_frame(image()); (void)g.drain();
    check(g.in_flight()==1, "new generation sends normally");
    ack(g,2,0xffffffff); check(g.in_flight()==1, "old opt-out ACK cannot erase new credit");
    g.receive(qoe(2,0),true); check(g.telemetry().qoe_samples==0, "old generation telemetry ignored");
    g.receive(qoe(3,100),true); check(g.telemetry().latest_qoe->frame_id==3, "frame IDs not reused on reset");
    ack(g,3); check(g.in_flight()==0, "new generation ACK accepted");
    for (auto version : {0x80105U, 0xa0002U, 0xa0100U, 0xa0200U}) {
        Graphics legacy; legacy.receive(offer({{version,16}}),true);
        rejects([&] { legacy.receive(offer({{0xa0701,0}}),true); });
    }
}

void lossless_generations() {
    Graphics g; g.receive(offer({{0xa0701,0x20}}), false); g.reset(layout()); (void)g.drain();
    auto frame=image();
    for (unsigned y=0; y<frame.height; ++y) for (unsigned x=0; x<frame.width; ++x) {
        const auto offset=(std::size_t(y)*frame.width+x)*4;
        frame.bgra[offset]=std::uint8_t((x/8)%2 ? 160 : 40);
        frame.bgra[offset+1]=90; frame.bgra[offset+2]=120; frame.bgra[offset+3]=255;
    }
    unsigned sequence=0, rectangles=0;
    auto inspect=[&] {
        auto packets=g.drain(); unsigned found=0;
        for (const auto& packet:packets) {
            Reader in(packet);
            check(in.u8()==0xe0 && in.u8()==4,"small lossless command segment");
            const auto command=in.le16(); in.skip(6);
            if (command!=1) continue;
            check(in.le16()==0 && in.le16()==8 && in.u8()==32,"striped fixture uses ClearCodec");
            in.skip(8); const auto size=in.le32();
            check(size==in.remaining() && in.u8()==0,"ClearCodec residual payload");
            check(in.u8()==(sequence&255),"ClearCodec sequence matches current decoder generation");
            ++sequence; ++found;
        }
        check(found>0,"ClearCodec regression requires encoded rectangles"); rectangles+=found;
    };
    g.lossless_frame(frame); inspect(); ack(g,1);
    // A normal desktop refresh/resize must preserve the existing codec sequence.
    g.reset(layout()); (void)g.drain(); g.lossless_frame(frame); inspect(); ack(g,2);
    check(rectangles>1 && sequence>0,"old decoder state established");
    // Capability re-advertisement discards both the old reference and sequence.
    g.receive(offer({{0xa0600,0x20}}),false); sequence=0;
    g.reset(layout()); (void)g.drain(); g.lossless_frame(frame); inspect(); ack(g,3);
    g.lossless_frame(frame); check(g.drain().empty(),"reset reference commits and suppresses identical frames");
}

void fuzz() {
    std::mt19937 random(0x514f4521);
    for (unsigned n=0; n<10000; ++n) {
        Bytes p(random()%128); for(auto& b:p) b=std::uint8_t(random());
        try { Graphics g; (void)g.receive(p, true); } catch(const ProtocolError&) {}
    }
}
}
int main() {
    try { caps(); telemetry(); renegotiation(); lossless_generations(); fuzz(); std::cout<<"PASS: "<<checks<<" checks; modern GFX sets, QoE isolation/wrap, transactional re-advertisement and 10000 malformed probes\n"; }
    catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
