#include "lrdp/reconnect.hpp"
#include <iostream>
#include <random>
using namespace lrdp;
namespace {
unsigned checks = 0;
void check(bool yes) { ++checks; require(yes, "reconnection wire assertion failed"); }
template<class F> void rejects(F f) { ++checks; try { f(); } catch(const ProtocolError&) { return; } throw std::runtime_error("expected rejection"); }
Bytes info() { Writer w; w.le16(0x40).le16(0).le32(0).le32(0x10).zeros(20); return std::move(w).finish(); }
}
int main() {
    try {
        ReconnectCookie cookie{0x12345678, {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}};
        const auto encoded = encode_reconnect_cookie(cookie); check(encoded.size() == 28 && encoded[8] == 0x78);
        check(decode_reconnect_cookie(encoded) == cookie);
        for (std::size_t n=0;n<encoded.size();++n) rejects([&]{ (void)decode_reconnect_cookie(View(encoded).first(n)); });
        for (unsigned byte : {0U,4U}) { auto bad=encoded; bad[byte] ^= 3; rejects([&]{(void)decode_reconnect_cookie(bad);}); }
        auto basic=info(); check(!client_info_reconnect(basic));
        for (std::size_t n=0;n<basic.size();++n) rejects([&]{(void)client_info_reconnect(View(basic).first(n));});
        Writer extended; extended.raw(basic).le16(2).le16(2).le16(0).le16(2).le16(0);
        check(!client_info_reconnect(extended.bytes()));
        const auto base_size=extended.size(); extended.zeros(172).le32(0).le32(0).le16(28).raw(encoded);
        check(client_info_reconnect(extended.bytes()) == cookie);
        for(std::size_t n=base_size+1;n<extended.size();++n) {
            if(n==base_size+172 || n==base_size+176 || n==base_size+180) check(!client_info_reconnect(View(extended.bytes()).first(n)));
            else rejects([&]{(void)client_info_reconnect(View(extended.bytes()).first(n));});
        }
        extended.le16(1).le16(0).le16(2).le16('A').le16(1);
        check(client_info_reconnect(extended.bytes()) == cookie);
        extended.u8(1); rejects([&]{(void)client_info_reconnect(extended.bytes());});
        const auto logon = reconnect_logon_info(cookie); Reader r(logon);
        check(r.le32()==3 && r.le16()==608 && r.le32()==1 && r.le32()==28);
        check(decode_reconnect_cookie(r.take(28))==cookie && r.remaining()==570);
        for (auto b : r.take(570)) check(b==0);
        std::mt19937 rng(0x41524331); for(unsigned i=0;i<20000;++i) {
            Bytes b(rng()%1024);for(auto& byte:b)byte=std::uint8_t(rng());
            try{(void)client_info_reconnect(b);}catch(const ProtocolError&){}
        }
        std::cout<<checks<<" checks; 20000 malformed Client Info probes passed\n";return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
