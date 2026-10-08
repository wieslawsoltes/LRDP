#include "lrdp/security/credssp.hpp"
#include "lrdp/security/ntlm_policy.hpp"
#include <iostream>
#include <random>
using namespace lrdp;
namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F f) { try { f(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected rejection"); }
// This deliberately non-cryptographic provider isolates the state machine. The
// separate client_nla test uses real FreeRDP, TLS, and system GSS-NTLMSSP.
class FixtureProvider final : public SecurityProvider {
public:
    SecurityStep accept(View token) override { check(!token.empty(), "empty token"); return {{1,2},true,"alice@EXAMPLE.TEST"}; }
    Bytes seal(View plain) override { Bytes b{0x42}; b.insert(b.end(), plain.begin(), plain.end()); return b; }
    Bytes unseal(View cipher) override { require(!cipher.empty() && cipher[0] == 0x42,"fixture integrity failure"); return {cipher.begin()+1,cipher.end()}; }
};
Bytes credentials() {
    Writer fields;
    for (const auto& pair : {std::pair{0xa0,"EXAMPLE.TEST"},std::pair{0xa1,"alice"},std::pair{0xa2,"fixture-only"}})
        fields.raw(ber(unsigned(pair.first),ber(4,utf16le(pair.second,false))));
    Writer envelope; envelope.raw(ber(0xa0,ber_integer(1))).raw(ber(0xa1,ber(4,ber(0x30,fields.bytes()))));
    return ber(0x30,envelope.bytes());
}
void state_machine() {
    const Bytes key{1,2,3,4}, nonce(32,7), token{1};
    auto allow = [](const std::string& name) { return name == "alice@EXAMPLE.TEST"; };
    FixtureProvider provider; CredsspServer server(provider,key,allow);
    TsRequest req; req.version=6; req.token=token; req.nonce=nonce;
    const auto encoded=encode_ts_request(req); const auto decoded=decode_ts_request(encoded);
    check(decoded.version==6 && decoded.token && decoded.token->size()==1 && decoded.nonce && decoded.nonce->size()==32,"TSRequest DER roundtrip");
    for (std::size_t size=0;size<encoded.size();++size) rejects([&]{ (void)decode_ts_request(View(encoded).first(size)); });
    auto first=server.receive(encoded); check(first.packet && !first.complete,"early client nonce with SPNEGO token");
    req.token.reset();
    const auto binding=provider.seal(credssp_binding_hash(key,nonce,false)); req.public_key_auth=binding;
    auto bound=server.receive(encode_ts_request(req)); check(bound.packet.has_value(),"stable repeated nonce accepted");
    const auto response=decode_ts_request(*bound.packet);
    check(response.public_key_auth && provider.unseal(*response.public_key_auth)==credssp_binding_hash(key,nonce,true),"server binding direction");
    const auto delegated=provider.seal(credentials()); req.public_key_auth.reset(); req.auth_info=delegated;
    check(server.receive(encode_ts_request(req)).complete,"delegation with stable nonce finalizes authentication");
    rejects([&]{ (void)server.receive(encoded); });
    FixtureProvider denied; CredsspServer unauthorized(denied,key,[](const std::string&){return false;});
    rejects([&]{ (void)unauthorized.receive(encoded); });
    FixtureProvider old; CredsspServer legacy(old,key,allow); req={}; req.version=4; req.token=token;
    rejects([&]{ (void)legacy.receive(encode_ts_request(req)); });
    FixtureProvider corrupt; CredsspServer mismatch(corrupt,key,allow); (void)mismatch.receive(encoded);
    req={}; req.version=6; req.nonce=nonce; const auto wrong=provider.seal(credssp_binding_hash(Bytes{9},nonce,false)); req.public_key_auth=wrong;
    rejects([&]{ (void)mismatch.receive(encode_ts_request(req)); });
    FixtureProvider early; CredsspServer premature(early,key,allow); req={}; req.version=6; req.auth_info=delegated; req.token=token;
    rejects([&]{ (void)premature.receive(encode_ts_request(req)); });
    FixtureProvider changed; CredsspServer nonce_changed(changed,key,allow); (void)nonce_changed.receive(encoded);
    const Bytes other_nonce(32,8); req={}; req.version=6; req.nonce=other_nonce; req.public_key_auth=binding;
    rejects([&]{ (void)nonce_changed.receive(encode_ts_request(req)); });
    FixtureProvider omitted; CredsspServer no_repeat(omitted,key,allow); (void)no_repeat.receive(encoded);
    req={}; req.version=6; req.public_key_auth=binding;
    check(no_repeat.receive(encode_ts_request(req)).packet.has_value(),"previously established nonce remains available");
    const Bytes expected{0x6e,0x73,0x9d,0x01,0x1f,0xda,0xc8,0x53,0xae,0xfb,0xc0,0x6b,0xb8,0xd3,0x90,0x4b,
                         0x9d,0x4a,0x78,0xad,0xc8,0x84,0x40,0x6d,0xa6,0xef,0x7f,0xa5,0xc7,0x4a,0xad,0x64};
    check(credssp_binding_hash(key,nonce,false)==expected,"independent SHA256 binding golden vector including NUL");
}
void ntlm_policy() {
    Writer type3; type3.raw({'N','T','L','M','S','S','P',0}).le32(3).zeros(8)
        .le16(48).le16(48).le32(64).zeros(32).le32(0x60080030).zeros(16).raw({1,1}).zeros(30);
    const auto valid=type3.bytes(); validate_ntlm_v2_authenticate(valid);
    auto weak=valid; weak[63]&=0xdf; rejects([&]{validate_ntlm_v2_authenticate(weak);});
    auto datagram=valid; datagram[60]|=0x40; rejects([&]{validate_ntlm_v2_authenticate(datagram);});
    auto legacy=valid; legacy[80]=0; rejects([&]{validate_ntlm_v2_authenticate(legacy);});
    auto outside=valid; outside[24]=0xff; rejects([&]{validate_ntlm_v2_authenticate(outside);});
    for(std::size_t n=0;n<valid.size();++n) rejects([&]{validate_ntlm_v2_authenticate(View(valid).first(n));});
    check(spnego_mechanism_token(valid)->size()==valid.size(),"raw NTLM token extraction");
    const auto response=ber(0xa1,ber(0x30,ber(0xa2,ber(4,valid))));
    check(spnego_mechanism_token(response)->size()==valid.size(),"SPNEGO responseToken extraction");
    Writer application; application.raw(ber(6,Bytes{0x2b,6,1,5,5,2})).raw(ber(0xa0,ber(0x30,ber(0xa2,ber(4,valid)))));
    const auto initial=ber(0x60,application.bytes());
    check(spnego_mechanism_token(initial)->size()==valid.size(),"SPNEGO initial mechToken extraction");
    check(!spnego_mechanism_token(ber(4,valid)),"do not scan arbitrary payloads for mechanism tokens");
    Writer signature; signature.le32(1).zeros(8).le32(2).zeros(32);
    validate_ntlm_sequence(signature.bytes(),2);
    rejects([&]{validate_ntlm_sequence(signature.bytes(),1);});
    rejects([&]{validate_ntlm_sequence(signature.bytes(),3);});
    for (std::size_t n=0;n<16;++n) rejects([&]{validate_ntlm_sequence(View(signature.bytes()).first(n),2);});
}
void malformed() {
    std::mt19937 rng(0x43535350);
    for (unsigned i=0;i<20000;++i) {
        Bytes b(rng()%256); for (auto& byte:b) byte=std::uint8_t(rng());
        try { (void)decode_ts_request(b); } catch (const ProtocolError&) {}
        (void)spnego_mechanism_token(b);
        try { validate_ntlm_v2_authenticate(b); } catch (const ProtocolError&) {}
    }
}
}
int main() {
    try { state_machine(); ntlm_policy(); malformed();
        std::cout<<"PASS: CredSSP DER/binding golden vector, early/stable nonce, authorization/delegation, NTLMv2 downgrade/replay guards and 60000 malformed parser invocations\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
