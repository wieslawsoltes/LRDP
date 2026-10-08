#include "lrdp/audio/output.hpp"
#include "lrdp/audio/input.hpp"
#include "lrdp/audio/pcm_ring.hpp"
#include "lrdp/security/principal.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
using namespace lrdp;
namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F action) { try { action(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected rejection"); }
Bytes formats(unsigned version, bool supported = true) {
    Writer body; body.le32(1).zeros(10).le16(supported ? 1 : 0).u8(0).le16(version).u8(0);
    if (supported) body.raw(pcm_audio_format({})); return sound_pdu(7,body.bytes());
}
void negotiate(AudioOutput& sound, unsigned version) {
    const auto initial=sound.start(100); check(initial.size()==42 && initial[0]==7,"server formats encoding");
    auto training=sound.receive(formats(version),101);
    if(version>=6) { check(training.empty(),"quality mode precedes training"); training=sound.receive(sound_pdu(12,Bytes{2,0,0,0}),102); }
    check(training.size()==1 && training[0][0]==6 && training[0].size()==8,"training framing");
    check(sound.receive(training[0],103).empty() && sound.ready(),"training response activates sound");
}
void output() {
    const Bytes pcm{0x10,0x11,0x20,0x21,0x30,0x31,0x40,0x41};
    for(unsigned version:{2U,6U,8U}) {
        AudioOutput sound; negotiate(sound,version);
        const auto packet=sound.send(pcm,65540);
        if(version==8) {
            const Bytes golden{13,0,20,0,4,0,0,0,1,0,0,0,4,0,1,0,0x10,0x11,0x20,0x21,0x30,0x31,0x40,0x41};
            check(packet.size()==1 && packet[0]==golden,"Wave2 little-endian golden vector");
        } else {
            const Bytes info{2,0,16,0,4,0,0,0,1,0,0,0,0x10,0x11,0x20,0x21};
            const Bytes wave{0,0,0,0,0x30,0x31,0x40,0x41};
            check(packet.size()==2 && packet[0]==info && packet[1]==wave,"legacy split WaveInfo/Wave golden vectors");
        }
        Writer ack; ack.le16(123).u8(1).u8(0); (void)sound.receive(sound_pdu(5,ack.bytes()),65560);
        check(sound.in_flight()==0 && sound.completion_latency_ms()==20,"client-adjusted timestamp is not treated as a correlation ID");
        rejects([&]{(void)sound.receive(sound_pdu(5,ack.bytes()),65560);});
        for(unsigned i=0;i<8;++i) (void)sound.send(pcm,70000);
        check(!sound.can_send(),"audio backpressure bounds in-flight blocks");
        rejects([&]{(void)sound.send(pcm,70000);});
        check(!sound.poll(74999) && sound.poll(75000).has_value() && sound.closed(),"stalled audio closes without killing desktop");
    }
    AudioOutput unsupported; (void)unsupported.start(0); (void)unsupported.receive(formats(8,false),1); check(unsupported.closed(),"unsupported formats disable sound");
    AudioOutput stalled; (void)stalled.start(0); check(stalled.poll(5000).has_value(),"audio negotiation deadline");
    AudioOutput wrapped; negotiate(wrapped,8);
    for(unsigned i=1;i<=520;++i) {
        const auto sent=wrapped.send(pcm,1000+i); check(sent[0][8]==std::uint8_t(i),"audio block ID wrap");
        Writer ack; ack.le16(0).u8(i&255).u8(0); (void)wrapped.receive(sound_pdu(5,ack.bytes()),1000+i);
    }
}
void input() {
    AudioInput mic; const auto version=mic.start(0); check(version==Bytes({1,2,0,0,0}),"microphone version offer");
    const auto offered=mic.receive(version,1); check(offered.outbound.size()==1 && offered.outbound[0].size()==27,"microphone formats size");
    (void)mic.receive(Bytes{5},1); const auto open=mic.receive(offered.outbound[0],2);
    check(open.outbound.size()==1 && open.outbound[0].size()==27 && open.outbound[0][0]==3,"microphone Open includes capture format");
    (void)mic.receive(Bytes{7,0,0,0,0},3); (void)mic.receive(Bytes{4,0,0,0,0},4); check(mic.ready(),"microphone format/open sequencing");
    Bytes samples(1921,0x34); samples[0]=6; (void)mic.receive(Bytes{5},5);
    const auto data=mic.receive(samples,5); check(data.pcm && data.pcm->size()==1920 && (*data.pcm)[0]==0x34,"microphone exact PCM payload");
    rejects([&]{(void)mic.receive(Bytes{6,0},5);});
    for(unsigned i=0;i<24;++i) (void)mic.receive(samples,5);
    rejects([&]{(void)mic.receive(samples,5);});
    check(mic.receive(samples,25).pcm.has_value(),"microphone rate budget replenishes with time");
    AudioInput deadline; (void)deadline.start(0); check(!deadline.poll(4999) && deadline.poll(5000) && deadline.closed(),"microphone negotiation timeout");
}
void ring() {
    rejects([]{PcmRing invalid(0xffffffff,4);}); rejects([]{PcmRing invalid(16,3);});
    PcmRing ring(16,4,0xfffffff8); Bytes data(24); for(unsigned i=0;i<data.size();++i)data[i]=std::uint8_t(i);
    check(ring.push(data)==16 && ring.dropped_frames()==2,"overflow drops complete new frames");
    Bytes first(8); check(ring.pop(first,true)==8 && std::equal(first.begin(),first.end(),data.begin()),"ring uint32 cursor wrap");
    check(ring.push(View(data).subspan(16))==8,"ring reuses consumed capacity");
    Bytes last(16); check(ring.pop(last,true)==16 && std::equal(last.begin(),last.end(),data.begin()+8),"ring wrap preserves byte ordering");
    check(ring.push(View(data).first(16))==16,"refill ring");
    check(ring.pop(first,true,8)==8 && std::equal(first.begin(),first.end(),data.begin()+8),"consumer trims stale audio to latency budget");
    PcmRing concurrent(1024,4); std::atomic<bool> failed=false, stop=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    std::thread producer([&]{
        for(std::uint32_t n=0;n<100000 && !stop;++n) {
            std::uint8_t bytes[4]; std::memcpy(bytes,&n,4);
            while(!concurrent.push(bytes) && !stop) std::this_thread::yield();
        }
    });
    for(std::uint32_t n=0;n<100000;++n) {
        std::uint8_t bytes[4];
        while(!concurrent.pop(bytes,true)) {
            if(std::chrono::steady_clock::now()>=deadline) {failed=true;stop=true;break;}
            std::this_thread::yield();
        }
        if(stop)break;
        std::uint32_t actual;std::memcpy(&actual,bytes,4);if(actual!=n){failed=true;stop=true;break;}
    }
    stop=true;producer.join();check(!failed,"SPSC concurrent sample ordering");
}
void names() {
    check(authenticated_principal(Bytes{'D','\\','u',0})=="D\\u","NTLM terminal NUL excluded");
    check(authenticated_principal(Bytes{'u','@','R'})=="u@R","Kerberos counted string unchanged");
    rejects([]{(void)authenticated_principal(Bytes{'u',0,0});});
    rejects([]{(void)authenticated_principal(Bytes{'u',0,'x'});});
    rejects([]{(void)authenticated_principal(Bytes{0});});
    rejects([]{(void)authenticated_principal(Bytes{'u','\n'});});
    check(authenticated_principal(Bytes{'U','@','R'})!="u@R","principal identity remains case-sensitive");
}
}
int main() {
    try { output();input();ring();names();
        std::mt19937 rng(0x41554449);
        for(unsigned i=0;i<10000;++i){ Bytes b(rng()%128);for(auto& v:b)v=std::uint8_t(rng());
            try{AudioOutput o;(void)o.start(0);(void)o.receive(b,1);}catch(const ProtocolError&){}
            try{AudioInput m;(void)m.start(0);(void)m.receive(b,1);}catch(const ProtocolError&){}
        }
        std::cout<<"PASS: RDPSND v2/v6/v8 golden packets, training/ACK/window/timeouts, microphone negotiation/rate bounds, 100000 concurrent PCM samples, GSS counted names and 20000 malformed audio messages\n";
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
