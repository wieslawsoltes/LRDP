#include "lrdp/session.hpp"
#include "lrdp/input/touch_injector.hpp"
#include <iostream>
#include <memory>
using namespace lrdp;
namespace {
void check(bool value, const char* message) { require(value,message); }
Bytes unhex(std::string_view hex) {
    auto digit = [](char c) { return c <= '9' ? unsigned(c-'0') : unsigned(c-'a'+10); };
    Bytes b; for (std::size_t i=0; i<hex.size(); i+=2) b.push_back(std::uint8_t(digit(hex[i])*16+digit(hex[i+1]))); return b;
}
struct Trace { std::vector<TouchOperation> operations; unsigned cancels = 0; };
class DesktopFixture final : public Desktop {
    std::shared_ptr<Trace> trace_;
    Layout layout_ = validate_layout({Monitor{1,0,0,640,480}});
    bool enabled_;
    TouchInjector touch_;
public:
    DesktopFixture(std::shared_ptr<Trace> trace, bool enabled)
        : trace_(std::move(trace)), enabled_(enabled), touch_([this](auto operations) {
            trace_->operations.insert(trace_->operations.end(), operations.begin(), operations.end());
        }) {}
    Layout layout() const override { return layout_; }
    bool resizable() const override { return true; }
    bool resize(const Layout& next) override { layout_=next; return true; }
    bool clipboard_available() const override { return false; }
    ExtendedCapabilities extended_capabilities() const override { return {enabled_ ? 32U : 0U,0}; }
    void extended_input(const std::vector<ExtendedFrame>& frames) override { touch_.apply(frames,layout_.width,layout_.height,320,240); }
    void cancel_extended_input() override { ++trace_->cancels; touch_.cancel(); }
    Frame capture() override { return {layout_.width,layout_.height,Bytes(std::size_t(layout_.width)*layout_.height*4)}; }
    void input(const InputEvent&) override {}
    void release_input() override {}
    void set_clipboard(std::string) override {}
    std::optional<std::string> poll_clipboard() override { return {}; }
};
Bytes client_data(unsigned channel, View body) {
    Writer w; w.u8(0x64).be16(0).be16(channel).u8(0x70).per_length(body.size()).raw(body); return x224_data(w.bytes());
}
Bytes client_share(unsigned type, View body) {
    Writer w; w.le16(unsigned(body.size()+6)).le16(0x10|type).le16(1001).raw(body); return std::move(w).finish();
}
Bytes client_share_data(unsigned type, View body) {
    Writer w; w.le32(share_id).u8(0).u8(2).le16(unsigned(body.size()+4)).u8(type).u8(0).le16(0).raw(body);
    return client_data(1003,client_share(7,w.bytes()));
}
struct Connection {
    std::shared_ptr<Trace> trace = std::make_shared<Trace>();
    Session session;
    explicit Connection(bool enabled=true) : session(std::make_unique<DesktopFixture>(trace,enabled),1,1,{},false) {}
    void confirm() {
        Writer bitmap; bitmap.le16(24).le16(1).le16(1).le16(1).le16(640).le16(480).le16(0).le16(1).le16(1).u8(0).u8(0).le16(1).le16(0);
        Writer caps; caps.le16(1).le16(0).le16(2).le16(28).raw(bitmap.bytes());
        Writer body; body.le32(share_id).le16(1002).le16(0).le16(unsigned(caps.size())).raw(caps.bytes());
        session.receive(client_data(1003,client_share(3,body.bytes())));
        session.receive(client_share_data(31,Bytes{1,0,0xea,3}));
        session.receive(client_share_data(20,Bytes{1,0,0,0,0,0,0,0}));
        session.receive(client_share_data(39,Bytes{0,0,0,0,3,0,50,0}));
        check(session.active(),"session activation failed");
    }
    void connect() {
        // GCC/MCS golden bytes emitted by the separate Python stdlib fixture.
        session.receive(unhex("0300018802f0807f6582017c0401010401010101ff301a020122020102020100020101020100020101020300ffff020102301a020122020102020100020101020100020101020300ffff020102301a020122020102020100020101020100020101020300ffff0201020482011b000500147c00018112000800100001c00044756361810401c0d800040008008002e00101ca03aa09040000675800004c005200440050002d006600690078007400750072006500000000000000000004000000000000000c0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004ca01000000000018000f0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000006000100000002c00c00000000000000000003c0200002000000636c69707264720000008080647264796e76630000008080"));
        (void)session.drain();
        session.receive(x224_data(Bytes{4,1,0,1,0})); session.receive(x224_data(Bytes{0x28}));
        for (unsigned id : {1001U,1003U,1004U,1005U}) { Writer w; w.u8(0x38).be16(0).be16(id); session.receive(x224_data(w.bytes())); }
        Writer info; info.le16(0x40).le16(0).le32(0).le32(0x10).zeros(20); session.receive(client_data(1003,info.bytes()));
        (void)session.drain(); confirm(); (void)session.drain();
        dvc(Bytes{0x50,0,1,0});
    }
    void dvc(View body) { for (auto& fragment:channel_fragments(body)) session.receive(client_data(1005,fragment)); }
    void input(View body) { Writer w; w.u8(0x30).u8(4).raw(body); dvc(w.bytes()); }
    std::vector<Bytes> dynamic_output() {
        std::vector<Bytes> result;
        ChannelAssembler fragments;
        for (const auto& packet : session.drain()) {
            Reader mcs(parse_x224_data(packet)); if(mcs.u8()!=0x68)continue;
            mcs.skip(2);const auto channel=mcs.be16();mcs.skip(1);const auto data=mcs.take(mcs.per_length());mcs.end();
            if(channel==1005)if(auto value=fragments.accept(data))result.push_back(std::move(*value));
        }
        return result;
    }
    void open() {
        auto created=dynamic_output(); bool found=false;
        for(auto& p:created)if(p.size()>2 && p[0]==0x10 && p[1]==4)
            found = std::string(p.begin()+2,p.end()-1)=="Microsoft::Windows::RDS::Input";
        check(found,"session did not create RDPEI");
        dvc(Bytes{0x10,4,0,0,0,0}); const auto ready=dynamic_output();
        check(ready.size()==1 && ready[0]==Bytes({0x30,4,1,0,10,0,0,0,1,0,1,0}),"touch-only SC_READY golden bytes");
        Writer c;c.le32(2).le32(0x30000).le16(32);input(input_pdu(2,c.bytes()));
    }
    void point(unsigned id,unsigned flags,int x=100,int y=80) {
        Writer b; input_unsigned(b,0,4);input_unsigned(b,1,2);input_unsigned(b,1,2);input_unsigned(b,0,8);
        b.u8(id);input_unsigned(b,0,2);input_signed(b,x,4);input_signed(b,y,4);input_unsigned(b,flags,4);
        input(input_pdu(3,b.bytes()));
    }
};
}
int main() {
    try {
        std::shared_ptr<Trace> lifetime;
        {
            Connection c;c.connect();c.open();c.point(250,25);
            check(c.trace->operations.size()==1 && c.trace->operations[0].x==50 && c.trace->operations[0].y==40,"native touch from complete RDP session");
            c.session.receive(client_share_data(35,Bytes(4)));auto suspended=c.dynamic_output();
            check(suspended.size()==1 && suspended[0]==Bytes({0x30,4,4,0,6,0,0,0}) &&
                c.trace->operations.back().action==TouchAction::up,"suppression sends suspend and releases native touch");
            auto n=c.trace->operations.size();c.point(250,26);check(c.trace->operations.size()==n,"in-flight suspended contact injected");
            Writer allow;allow.u8(1).zeros(3).le16(0).le16(0).le16(639).le16(479);
            c.session.receive(client_share_data(35,allow.bytes()));auto resumed=c.dynamic_output();
            check(resumed.size()==1 && resumed[0]==Bytes({0x30,4,5,0,6,0,0,0}),"resume PDU missing");
            c.point(250,26);check(c.trace->operations.size()==n,"stale contact resumed without down");
            c.point(250,25);check(c.trace->operations.size()==n+1,"fresh contact could not recover");
            Monitor changed;changed.width=800;changed.height=600;
            Writer display;display.u8(0x30).u8(1).raw(encode_layout(validate_layout({changed})));
            c.dvc(Bytes{0x10,1,0,0,0,0});(void)c.dynamic_output();c.dvc(display.bytes());c.session.tick(true,false);
            check(c.session.phase()==SessionPhase::confirm && c.trace->operations.back().action==TouchAction::up,"resize did not cancel touch before reactivation");
            (void)c.dynamic_output();c.confirm();(void)c.dynamic_output();c.point(2,25);lifetime=c.trace;
        }
        check(lifetime && lifetime->operations.back().action==TouchAction::up,"disconnect left a native touch active");
        Connection unsupported(false);unsupported.connect();
        for(const auto& p:unsupported.dynamic_output())check(!(p.size()>1 && p[0]==0x10 && p[1]==4),"unsupported backend advertised touch");
        Connection malformed;malformed.connect();malformed.open();malformed.point(1,25);
        bool rejected=false;try{malformed.point(1,26,9999,0);}catch(const ProtocolError&){rejected=true;}
        check(rejected && malformed.trace->operations.back().action==TouchAction::up,"malformed input failed to cancel native contact");
        std::cout<<"PASS: complete session RDPEI creation/readiness, native slot injection, suppression/resume, stale recovery, resize/disconnect cancellation, backend gating and malformed-batch teardown\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
