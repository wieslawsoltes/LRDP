#include "lrdp/drive/protocol.hpp"
#include <iostream>
using namespace lrdp;
namespace {
void name(View text, std::uint32_t unicode=1) {
    drive::Protocol p; p.start(); (void)p.drain();
    Writer announce; announce.le16(1).le16(13).le32(77);
    p.receive(drive::pdu(0x4343,announce.bytes()));
    Writer request; request.le32(unicode).le32(0).le32(std::uint32_t(text.size())).raw(text);
    p.receive(drive::pdu(0x434e,request.bytes()));
    require(p.drain().size()==2,"client-name did not advance negotiation");
}
template<class F> void rejects(F f) {try {f();} catch(const ProtocolError&) {return;}throw std::runtime_error("invalid computer name accepted");}
}
int main() {
    try {
        auto text=utf16le("test-host-日本語"); name(text);
        text.insert(text.end(),{0,0});name(text);name(text,0xffff0001);
        text.insert(text.end(),{0,0,0,0});name(text);
        // Same padding must NOT change shared Unicode/path parsing semantics.
        rejects([&]{(void)from_utf16le(text);});
        text.insert(text.end(),{65,0,0,0});rejects([&]{name(text);});
        rejects([]{name(Bytes{65,0,0});});
        rejects([]{name(Bytes{65,0});});
        rejects([]{name(Bytes{0,0xd8,0,0,0,0});});
        rejects([]{name(Bytes(4098));});
        name(Bytes{0,0,0,0});
        std::cout<<"PASS: RDPDR informational UTF-16 name padding; hidden suffixes, odd lengths, malformed surrogates and quota rejection\n";
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
