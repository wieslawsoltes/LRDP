#include "lrdp/session.hpp"
#include <algorithm>
#include <iostream>
using namespace lrdp;
namespace {
unsigned checks = 0;
void check(bool value, const char* text) { ++checks; require(value,text); }
template<class F> void rejects(F f) { ++checks; try { f(); } catch(const ProtocolError&) { return; } throw std::runtime_error("expected rejection"); }
Bytes unhex(std::string_view text) {
    auto digit=[](char c){return c <= '9' ? unsigned(c-'0') : unsigned(c-'a'+10);};
    Bytes bytes; for(std::size_t i=0;i<text.size();i+=2)bytes.push_back(std::uint8_t(digit(text[i])*16+digit(text[i+1])));return bytes;
}
Bytes initial() {
    // Golden packet emitted by the separate Python wire fixture, not LRDP's encoder.
    return unhex("0300019002f0807f658201840401010401010101ff301a020123020103020100020101020100020101020300ffff020102301a020123020103020100020101020100020101020300ffff020102301a020123020103020100020101020100020101020300ffff02010204820123000500147c0001811a000800100001c00044756361810c06c008000000000002c00c00000000000000000003c0200002000000636c69707264720000008080647264796e7663000000808001c0d800040008008002e00101ca03aa090400006758000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004ca01000000000018000f00800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000");
}
Bytes client(unsigned channel, View data) { Writer w;w.u8(0x64).be16(0).be16(channel).u8(0x70).per_length(data.size()).raw(data);return x224_data(w.bytes()); }
Bytes response() { return {0,32,0,0,6,1,1,0,0,0}; }
void share(Session& session,unsigned type,View data) {
    Writer body;body.le32(share_id).u8(0).u8(2).le16(unsigned(data.size()+4)).u8(type).u8(0).le16(0).raw(data);
    Writer pdu;pdu.le16(unsigned(body.size()+6)).le16(0x17).le16(1001).raw(body.bytes());session.receive(client(1003,pdu.bytes()));
}
void test() {
    auto bytes=initial();const Bytes pattern{6,0xc0,8,0,0,0,0,0};
    const auto position=std::search(bytes.begin(),bytes.end(),pattern.begin(),pattern.end());
    check(position!=bytes.end(),"missing extended channel golden field");
    auto invalid=bytes;invalid[std::size_t(position-bytes.begin())+4]=1;
    rejects([&]{ (void)connect_initial(parse_x224_data(invalid),1); });
    auto parsed=connect_initial(parse_x224_data(bytes),1);
    check(parsed.early_caps==0x80 && parsed.message_channel_requested && parsed.channel_ids.size()==2,"message field independent of block order");
    parsed.message_channel=1004;rejects([&]{(void)connect_response(parsed,1);});
    parsed.message_channel=1006;parsed.message_channel_requested=false;
    rejects([&]{(void)connect_response(parsed,1);});
    // Replace only the optional block's type with an unknown, length-delimited
    // extension. The Client Core capability remains set and all lengths remain valid.
    auto no_request=bytes;no_request[std::size_t(position-bytes.begin())]=0xff;
    Session unrequested(make_demo_desktop(),1,1,{},false);unrequested.configure_network_metrics();
    unrequested.receive(no_request);
    check(!unrequested.settings().message_channel_requested && !unrequested.settings().message_channel &&
          !unrequested.network_metrics(),"Client Core alone cannot allocate an unrequested message channel");
    Session disabled(make_demo_desktop(),1,1,{},false);disabled.receive(bytes);
    check(disabled.settings().message_channel_requested && !disabled.settings().message_channel &&
          !disabled.network_metrics(),"explicit channel request cannot bypass server opt-in policy");
    Session s(make_demo_desktop(),1,1,{},false);s.configure_network_metrics();
    s.receive(bytes);check(s.settings().message_channel==1006,"dedicated channel allocated after static channels");
    rejects([&]{s.configure_network_metrics();});
    (void)s.drain();s.receive(x224_data(Bytes{4,1,0,1,0}));s.receive(x224_data(Bytes{0x28}));
    rejects([&]{s.receive(client(1006,response()));}); // Never accepts data on an unjoined channel.
    for(unsigned id:{1006U,1001U,1003U,1004U,1005U}) {
        Writer join;join.u8(0x38).be16(0).be16(id);s.receive(x224_data(join.bytes()));
    }
    check(s.phase()==SessionPhase::info,"message channel participates in complete join accounting");
    rejects([&]{s.receive(client(1006,response()));});
    Writer info;info.le16(0x40).le16(0).le32(0).le32(0x10).zeros(20);s.receive(client(1003,info.bytes()));(void)s.drain();
    Writer bitmap;bitmap.le16(24).le16(1).le16(1).le16(1).le16(640).le16(480).le16(0).le16(1).le16(1).u8(0).u8(0).le16(1).le16(0);
    Writer caps;caps.le16(1).le16(0).le16(2).le16(28).raw(bitmap.bytes());
    Writer confirm;confirm.le32(share_id).le16(1002).le16(0).le16(unsigned(caps.size())).raw(caps.bytes());
    Writer pdu;pdu.le16(unsigned(confirm.size()+6)).le16(0x13).le16(1001).raw(confirm.bytes());s.receive(client(1003,pdu.bytes()));
    share(s,31,Bytes{1,0,0xea,3});share(s,20,Bytes{1,0,0,0,0,0,0,0});share(s,39,Bytes{0,0,0,0,3,0,50,0});
    check(s.active() && !s.network_metrics()->started(),"logical activation is not TLS egress");
    check(s.poll_network(NetworkAutodetect::Clock::now(),true,true).empty(),"no probes before Font Map egress");
    rejects([&]{s.receive(client(1006,response()));});
    std::uint64_t written=0;
    for(const auto& packet:s.drain()) { written+=packet.size();s.network_transmitted(packet,written,NetworkAutodetect::Clock::now()); }
    check(s.network_metrics()->started(),"Font Map egress starts monitoring");
    auto probes=s.poll_network(NetworkAutodetect::Clock::now(),true,false);
    check(probes.before_data.size()==1 && probes.after_data.empty(),"one RTT before application data");
    for(const auto& packet:probes.before_data) { written+=packet.size();s.network_transmitted(packet,written,NetworkAutodetect::Clock::now()); }
    s.receive(client(1006,response()));check(s.network_metrics()->metrics().rtt_samples==1 && s.last_packet_was_network(),"diagnostic response excluded from ordinary idle activity");
    s.receive(client(1006,response()));check(s.last_packet_was_network() && s.network_metrics()->metrics().ignored_responses==1,"stale replies also excluded from idle activity");
    share(s,35,Bytes(4));check(!s.last_packet_was_network(),"non-network packet clears idle classification");
    s.receive(Bytes{4,3,0x60});check(!s.last_packet_was_network(),"fast-path activity clears idle classification");
    auto malformed=response();malformed[0]=1;rejects([&]{s.receive(client(1006,malformed));});
    check(s.network_metrics()->metrics().rtt_samples==1,"malformed response cannot publish metrics");
}
}
int main(){try{test();std::cout<<checks<<" checks: complete Session activation, message-channel guards, egress gate and idle classification passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
