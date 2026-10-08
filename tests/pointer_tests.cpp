#include "lrdp/pointer.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
using namespace lrdp;
namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn) { try { fn(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected rejection"); }
PointerShape sample() {
    return {3,2,1,1, {0,0,255,255, 0,128,0,128, 0,0,0,0,
                     255,0,0,255, 50,60,70,255, 10,20,30,255}};
}
unsigned kind(const Bytes& pdu) { Reader r(pdu); return r.le16(); }
void wire() {
    const auto shape = sample(); auto alpha = pointer_image(shape, 9, true); Reader a(alpha);
    check(a.le16() == 8 && a.le16() == 0 && a.le16() == 32, "alpha pointer header");
    check(a.le16() == 9 && a.le16() == 1 && a.le16() == 1 && a.le16() == 3 && a.le16() == 2, "pointer slot/hotspot/dimensions");
    check(a.le16() == 4 && a.le16() == 24, "32-bit pointer mask lengths");
    const auto bottom = a.take(12), top = a.take(12);
    check(std::equal(bottom.begin(), bottom.end(), shape.bgra.begin()+12), "bottom-up alpha pixels");
    check(std::equal(top.begin(), top.end(), shape.bgra.begin()), "premultiplied alpha retained");
    check(a.u8() == 0 && a.u8() == 0 && a.u8() == 0x20 && a.u8() == 0, "AND mask bit order/padding"); a.end();
    auto color = pointer_image(shape, 2, false); Reader c(color);
    check(c.le16() == 6 && c.le16() == 0 && c.le16() == 2, "24-bit pointer header"); c.skip(8);
    check(c.le16() == 4 && c.le16() == 20, "word-padded 24-bit scanlines");
    c.skip(10); const auto row = c.take(10);
    check(row[0] == 0 && row[2] == 255 && row[3] == 0 && row[4] == 255 && row[5] == 0, "unpremultiply legacy cursor");
    check(row[6] == 0 && row[7] == 0 && row[8] == 0 && row[9] == 0, "transparent legacy XOR and padding");
    check(c.u8() == 0 && c.u8() == 0 && c.u8() == 0x20 && c.u8() == 0, "legacy mask"); c.end();
    check(pointer_system(true) == Bytes({1,0,0,0,0,0,0,0}), "hidden system pointer");
    auto large = PointerShape{96,48,95,47,Bytes(96*48*4,255)}; const auto small = fit_pointer(large);
    check(small.width == 32 && small.height == 16 && small.hot_x == 31 && small.hot_y == 15, "large shape aspect/hotspot");
    rejects([&]{ (void)pointer_image(large,0,true); });
    rejects([&]{ auto bad=shape; bad.hot_x=3; bad.validate(); });
    rejects([&]{ auto bad=shape; bad.bgra.pop_back(); bad.validate(); });
    rejects([]{ PointerShape{}.validate(); });
}
void caching() {
    PointerEncoder e; auto first=sample(); auto second=first; second.hot_x=2;
    e.configure(4,2); check(e.capacity()==2 && e.alpha(),"new pointer negotiation");
    check(kind(*e.update(first))==8 && !e.update(first),"new shape and unchanged suppression");
    check(kind(*e.update(second))==8,"second cache shape");
    check(*e.update(first)==Bytes({7,0,0,0,0,0}),"cache hit");
    auto third=first; third.hot_x=0;
    check(kind(*e.update(third))==8,"LRU replacement");
    check(kind(*e.update(first))==7,"MRU entry survives replacement");
    check(kind(*e.update(second))==8,"evicted shape gets full image");
    auto hidden=first; std::fill(hidden.bgra.begin(),hidden.bgra.end(),0);
    check(*e.update(hidden)==pointer_system(true) && !e.update(hidden),"hidden update suppression");
    check(kind(*e.update(second))==7,"cached restoration after hidden cursor");
    e.configure(1,0); check(!e.alpha() && kind(*e.update(first))==6,"legacy negotiation");
    e.configure(0,0); check(!e.update(first),"no image sent without client cache");
    check(*e.update(hidden)==pointer_system(true) && *e.update(first)==pointer_system(false),"system pointer fallback");
    e.configure(65535,65535); check(e.capacity()==32,"cache allocation quota");
}
}
int main() {
    try { wire(); caching(); std::cout << "PASS: pointer negotiation, alpha/legacy masks, bottom-up padded rows, bounded exact LRU, hiding and hotspot-preserving downscaling\n"; return 0; }
    catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
