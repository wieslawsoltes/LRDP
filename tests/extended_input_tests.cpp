#include "lrdp/input/extended.hpp"
#include <functional>
#include <iostream>
#include <random>

using namespace lrdp;
namespace {
unsigned checks = 0;
void check(bool value, const char* text) { ++checks; require(value, text); }
template<class F> void rejects(F&& action) {
    ++checks; try { action(); } catch (const ProtocolError&) { return; }
    throw ProtocolError("expected digitizer rejection");
}
Bytes ready(unsigned version = 0x30000, unsigned flags = 0, unsigned contacts = 16) {
    Writer w; w.le32(flags).le32(version).le16(contacts); return input_pdu(2, w.bytes());
}
Bytes batch(const std::vector<ExtendedFrame>& frames) {
    Writer out; input_unsigned(out, frames.front().encode_delay_ms, 4); input_unsigned(out, frames.size(), 2);
    for (const auto& f : frames) {
        input_unsigned(out, f.contacts.size(), 2); input_unsigned(out, f.offset_us, 8);
        for (const auto& c : f.contacts) {
            unsigned fields = 0;
            if (f.kind == Digitizer::touch) fields = unsigned(bool(c.rectangle)) | (c.orientation ? 2U : 0U) | (c.pressure ? 4U : 0U);
            else fields = (c.pen_flags ? 1U : 0U) | (c.pressure ? 2U : 0U) | (c.rotation ? 4U : 0U) | (c.tilt_x ? 8U : 0U) | (c.tilt_y ? 16U : 0U);
            out.u8(c.id); input_unsigned(out, fields, 2); input_signed(out, c.x, 4); input_signed(out, c.y, 4); input_unsigned(out, c.flags, 4);
            if (f.kind == Digitizer::touch) {
                if (c.rectangle) for (auto n : {c.rectangle->left, c.rectangle->top, c.rectangle->right, c.rectangle->bottom}) input_signed(out, n, 2);
                if (c.orientation) input_unsigned(out, *c.orientation, 4);
                if (c.pressure) input_unsigned(out, *c.pressure, 4);
            } else {
                if (c.pen_flags) input_unsigned(out, *c.pen_flags, 4);
                if (c.pressure) input_unsigned(out, *c.pressure, 4);
                if (c.rotation) input_unsigned(out, *c.rotation, 2);
                if (c.tilt_x) input_signed(out, *c.tilt_x, 2);
                if (c.tilt_y) input_signed(out, *c.tilt_y, 2);
            }
        }
    }
    return input_pdu(frames.front().kind == Digitizer::touch ? 3 : 8, out.bytes());
}
ExtendedContact point(unsigned id = 1, unsigned flags = 25, int x = 120, int y = 140) {
    ExtendedContact c; c.id = std::uint8_t(id); c.flags = flags; c.x = x; c.y = y; return c;
}
ExtendedFrame frame(std::vector<ExtendedContact> contacts, Digitizer kind = Digitizer::touch) {
    ExtendedFrame f; f.kind = kind; f.contacts = std::move(contacts); return f;
}
void integers() {
    struct Golden { unsigned width; bool sign; std::int64_t value; Bytes data; };
    const std::vector<Golden> cases{
        {2,false,0x1a1b,{0x9a,0x1b}}, {2,true,-0x1a1b,{0xda,0x1b}}, {2,true,-2,{0x42}},
        {4,false,0x1a1b1c,{0x9a,0x1b,0x1c}}, {4,true,-0x1a1b1c,{0xba,0x1b,0x1c}}, {4,true,-2,{0x22}},
        {8,false,0x1a1b1c1d1e1f2aLL,{0xda,0x1b,0x1c,0x1d,0x1e,0x1f,0x2a}}};
    for (const auto& c : cases) {
        Reader r(c.data); Writer w;
        if (c.sign) { check(input_signed(r,c.width)==c.value,"specification signed vector"); input_signed(w,std::int32_t(c.value),c.width); }
        else { check(input_unsigned(r,c.width)==std::uint64_t(c.value),"specification unsigned vector"); input_unsigned(w,std::uint64_t(c.value),c.width); }
        r.end(); check(w.bytes()==c.data,"specification vector encoding");
        for (std::size_t n=0;n<c.data.size();++n) rejects([&]{ Reader truncated(View(c.data).first(n)); if(c.sign) (void)input_signed(truncated,c.width); else (void)input_unsigned(truncated,c.width); });
    }
    for (unsigned width : {2U,4U,8U}) {
        const unsigned payload = width == 2 ? 7 : width == 4 ? 6 : 5;
        for (unsigned bits=0;bits<payload+8*(width-1);++bits) {
            const auto n=std::uint64_t(1)<<bits;
            for (auto value : {n-1,n}) { Writer w; input_unsigned(w,value,width); Reader r(w.bytes()); check(input_unsigned(r,width)==value,"integer prefix boundary"); r.end(); }
        }
        rejects([&]{ Writer w; input_unsigned(w,std::uint64_t(1)<<(payload+8*(width-1)),width); });
    }
    for (unsigned width : {2U,4U}) {
        const auto maximum = width==2 ? 0x3fff : 0x1fffffff;
        for (int n : {0,1,-1,maximum,-maximum}) { Writer w; input_signed(w,n,width); Reader r(w.bytes()); check(input_signed(r,width)==n,"signed edge"); }
        rejects([&]{ Writer w; input_signed(w,maximum+1,width); });
    }
    rejects([]{ Writer w; input_signed(w,INT32_MIN,4); });
    rejects([]{ Writer w; input_signed(w,1,8); });
    rejects([]{ Writer w; input_unsigned(w,1,3); });
}
void codecs() {
    auto c = point(255); c.rectangle=ContactRect{-7,-8,11,12}; c.orientation=359; c.pressure=1024;
    auto f = frame({c}); f.encode_delay_ms=17;
    const auto wire = batch({f}); check(decode_input_frames(wire,16,0)==std::vector<ExtendedFrame>{f},"touch metadata roundtrip");
    for (std::size_t n=0;n<wire.size();++n) rejects([&]{ (void)decode_input_frames(View(wire).first(n),16,0); });
    auto p=point(7); p.pen_flags=7; p.pressure=999; p.rotation=270; p.tilt_x=-90; p.tilt_y=90;
    auto pen=frame({p},Digitizer::pen); pen.offset_us=0x1a1b1c1d1e1f2aULL;
    const auto encoded= batch({pen}); check(decode_input_frames(encoded,16,4)==std::vector<ExtendedFrame>{pen},"pen metadata roundtrip");
    rejects([&]{ (void)decode_input_frames(encoded,16,1); });
    rejects([&]{ (void)decode_input_frames(encoded,16,0); });
    rejects([&]{ auto bad=f; bad.contacts[0].pressure=1025; (void)decode_input_frames(batch({bad}),16,0); });
    rejects([&]{ auto bad=f; bad.contacts[0].orientation=360; (void)decode_input_frames(batch({bad}),16,0); });
    rejects([&]{ auto bad=pen; bad.contacts[0].tilt_y=-91; (void)decode_input_frames(batch({bad}),16,4); });
    rejects([&]{ auto bad=f; bad.contacts.push_back(c); (void)decode_input_frames(batch({bad}),16,0); });
    rejects([&]{ auto bad=f; bad.contacts[0].flags=1; (void)decode_input_frames(batch({bad}),16,0); });
    rejects([&]{ std::vector<ExtendedFrame> excessive(65,f); (void)decode_input_frames(batch(excessive),16,0); });
    // Header length is checked independently, including when a valid prefix is followed by junk.
    auto trailing=wire; trailing.push_back(0); rejects([&]{ (void)decode_input_frames(trailing,16,0); });
}
void states() {
    ExtendedInput input({16,4}); const auto server=input.start(); Reader sc(server);
    check(sc.le16()==1 && sc.le32()==14 && sc.le32()==0x30000 && sc.le32()==1,"server feature advertisement");
    rejects([&]{ (void)input.start(); });
    (void)input.receive(ready(0x30000,4),640,480);
    rejects([&]{ (void)input.receive(ready(),640,480); });
    auto f=frame({point()}); auto accepted=input.receive(batch({f}),640,480);
    check(!accepted.cancel && accepted.frames.size()==1 && input.active_contacts()==1,"touch down");
    f.contacts[0].flags=26; f.contacts[0].x=130; f.offset_us=1000;
    check(input.receive(batch({f}),640,480).frames.size()==1,"engaged movement");
    f.contacts[0].flags=12;
    check(input.receive(batch({f}),640,480).frames.size()==1 && input.active_contacts()==1,"release to hover");
    Writer dismiss; dismiss.u8(1); auto ended=input.receive(input_pdu(6,dismiss.bytes()),640,480);
    check(ended.frames.size()==1 && ended.frames[0].contacts[0].flags==2 && input.active_contacts()==0,"dismiss hover");
    check(input.receive(input_pdu(6,dismiss.bytes()),640,480).frames.empty(),"dismiss unknown ignored");
    f=frame({point()}); (void)input.receive(batch({f}),640,480);
    auto next=f; next.contacts[0].flags=26; next.contacts[0].x=150;
    auto bad=next; bad.contacts[0].flags=4; bad.contacts[0].x=160;
    auto rejected=input.receive(batch({next,bad}),640,480);
    check(rejected.cancel && rejected.frames.empty() && input.active_contacts()==0,"invalid release cancels entire batch");
    check(input.receive(batch({next}),640,480).frames.empty(),"stale update after cancellation ignored");
    check(input.receive(batch({f}),640,480).frames.size()==1,"fresh contact restores transaction");
    check(input.suspend()==input_pdu(4) && input.active_contacts()==0,"suspend releases state");
    check(!input.suspend() && input.receive(batch({f}),640,480).frames.empty(),"suspended packets ignored");
    check(input.resume()==input_pdu(5) && !input.resume(),"resume idempotence");
    check(input.receive(batch({next}),640,480).frames.empty(),"resume does not resurrect held contact");
    check(input.receive(batch({f}),640,480).frames.size()==1,"fresh down after resume");
    auto outside=next; outside.contacts[0].x=-1;
    rejects([&]{ (void)input.receive(batch({outside}),640,480); });
    check(input.active_contacts()==1,"invalid wire has no state prefix commit");
    auto pen=frame({point(2),point(3,25,180,200)},Digitizer::pen);
    check(input.receive(batch({pen}),640,480).frames.size()==1 && input.active_contacts()==3,"multipen independent IDs");
    auto badwire=batch({next}); badwire.pop_back();
    rejects([&]{ (void)input.receive(badwire,640,480); }); check(input.active_contacts()==3,"truncation preserves state until session abort");
}
void negotiation() {
    ExtendedInput touch({2,0}); (void)touch.start(); (void)touch.receive(ready(),640,480);
    check(touch.version()==0x10001,"endpoint maximum intersection");
    rejects([&]{ (void)touch.receive(batch({frame({point(0)},Digitizer::pen)}),640,480); });
    auto f=frame({point()}); f.offset_us=1;
    rejects([&]{ (void)touch.receive(batch({f}),640,480); });
    f.offset_us=0; (void)touch.receive(batch({f}),640,480);
    f.contacts={point(2)}; (void)touch.receive(batch({f}),640,480);
    f.contacts={point(3)}; rejects([&]{ (void)touch.receive(batch({f}),640,480); });
    check(touch.active_contacts()==2,"simultaneous quota transaction rollback");
    ExtendedInput no_time({16,4}); (void)no_time.start(); (void)no_time.receive(ready(0x20000,3),640,480);
    f=frame({point()}); f.encode_delay_ms=999; f.offset_us=1234;
    const auto result=no_time.receive(batch({f}),640,480);
    check(result.frames[0].offset_us==0 && result.frames[0].encode_delay_ms==0 && no_time.touch_visuals(),"disable timestamp injection semantics");
    rejects([&]{ (void)no_time.receive(batch({frame({point(1)},Digitizer::pen)}),640,480); });
    rejects([]{ ExtendedInput empty({0,4}); });
    ExtendedInput early({2,0}); (void)early.start(); rejects([&]{ (void)early.receive(batch({frame({point()})}),640,480); });
}
void adversarial() {
    std::mt19937 random(0x52445049);
    for (unsigned trial=0;trial<20000;++trial) {
        Bytes body(random()%128); for (auto& b:body) b=std::uint8_t(random());
        const auto wire=input_pdu(trial%2?3:8,body);
        try { (void)decode_input_frames(wire,16,4); } catch (const ProtocolError&) {}
    }
    check(true,"20000 deterministic malformed input batches");
}
}
int main() {
    try { integers(); codecs(); states(); negotiation(); adversarial(); std::cout<<"PASS: "<<checks<<" checks, RDPEI golden integers, touch/pen metadata, atomic FSM, quotas, suspend/recovery; 20000 malformed batches\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n'; return 1; }
}
