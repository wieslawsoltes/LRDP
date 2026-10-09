#include "lrdp/session/registry.hpp"
#include <iostream>
using namespace lrdp;using namespace lrdp::persistent;
namespace {
unsigned checks=0;
void check(bool ok){++checks;require(ok,"registry assertion");}
template<class F>void rejects(F f){++checks;try{f();}catch(const ProtocolError&){return;}throw std::runtime_error("expected rejection");}
}
int main(){try{
    const auto now=Time{}; Registry r(2,std::chrono::seconds(60));
    const auto a=r.acquire("DOMAIN\\Alice",{},1,now);check(a.created && a.id);
    rejects([&]{r.acquire("DOMAIN\\Alice",{},1,now);});
    auto server=*r.commit(1,true,now);check(server.logon_id==a.id);
    auto proof=enhanced_verifier(server);check(proof.bytes!=server.bytes);
    rejects([&]{r.acquire("DOMAIN\\Alice",proof,2,now);}); // No live-session stealing.
    check(!r.release(1,now));
    rejects([&]{r.acquire("DOMAIN\\alice",proof,2,now);});
    auto bad=proof;bad.bytes[15]^=1;rejects([&]{r.acquire("DOMAIN\\Alice",bad,2,now);});
    auto b=r.acquire("DOMAIN\\Alice",proof,2,now+std::chrono::seconds(59));check(!b.created && b.id==a.id);
    check(!r.release(2,now+std::chrono::seconds(59))); // Abort must not refresh TTL.
    rejects([&]{r.acquire("DOMAIN\\Alice",proof,3,now+std::chrono::seconds(60));});
    check(r.expire(now+std::chrono::seconds(60))==std::vector<std::uint32_t>{a.id});
    auto c=r.acquire("A",{},4,now);auto first=*r.commit(4,true,now);r.release(4,now);
    r.acquire("A",enhanced_verifier(first),5,now);auto second=*r.commit(5,true,now);check(first.bytes!=second.bytes);
    r.release(5,now);rejects([&]{r.acquire("A",enhanced_verifier(first),6,now);});
    r.acquire("A",enhanced_verifier(second),6,now);r.commit(6,true,now);
    rejects([&]{r.rotate(6,now+std::chrono::seconds(3599));});auto rotated=r.rotate(6,now+std::chrono::hours(1));check(rotated.logon_id==c.id);
    check(r.expire(now+std::chrono::hours(2)).empty()); // Connected desktops do not expire.
    auto d=r.acquire("B",{},7,now);rejects([&]{r.acquire("C",{},8,now);});
    check(r.release(7,now)==d.id); // Uncommitted new desktop is deleted.
    d=r.acquire("B",{},7,now);check(!r.commit(7,false,now));check(r.release(7,now)==d.id);
    check(r.list().size()==1 && r.list()[0].attached);check(r.erase(c.id));check(r.size()==0);
    rejects([&]{r.acquire(std::string("A\0B",3),{},1,now);});
    ReconnectCookie vector{7,{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}};
    const std::array<std::uint8_t,16> expected={0xb6,0x39,0xc8,0x73,0x16,0x38,0x61,0x8b,0x70,0x79,0x72,0xaa,0x6e,0x96,0xcf,0x90};
    check(enhanced_verifier(vector).bytes==expected);
    std::cout<<checks<<" registry checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
