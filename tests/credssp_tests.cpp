#include "lrdp/security/credssp.hpp"
#include <iostream>
#include <random>
using namespace lrdp;
namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F f) { try { f(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected rejection"); }
// Intentionally NOT cryptography: isolates state-machine policy from the GSS provider.
class FixtureProvider final : public SecurityProvider {
public:
    SecurityStep accept(View token) override { check(!token.empty(), "token view"); return {{1,2},true,"alice@EXAMPLE.TEST"}; }
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
void run() {
    const Bytes key{1,2,3,4}, nonce(32,7), token{1};
    auto allow = [](const std::string& name) { return name == "alice@EXAMPLE.TEST"; };
    FixtureProvider provider; CredsspServer server(provider,key,allow);
    TsRequest req; req.version=6; req.token=token;
    const auto encoded = encode_ts_request(req); const auto decoded=decode_ts_request(encoded);
    check(decoded.version==6 && decoded.token && decoded.token->size()==1,"TSRequest DER roundtrip");
    for (std::size_t size=0;size<encoded.size();++size) rejects([&]{ (void)decode_ts_request(View(encoded).first(size)); });
    auto first=server.receive(encoded); check(first.packet && !first.complete,"SPNEGO response");
    req.token.reset(); req.nonce=nonce;
    const auto binding=provider.seal(credssp_binding_hash(key,nonce,false)); req.public_key_auth=binding;
    auto bound=server.receive(encode_ts_request(req)); check(bound.packet.has_value(),"public-key binding response");
    const auto response=decode_ts_request(*bound.packet);
    check(response.public_key_auth && provider.unseal(*response.public_key_auth)==credssp_binding_hash(key,nonce,true),"server binding direction");
    const auto delegated=provider.seal(credentials()); req.public_key_auth.reset(); req.nonce.reset(); req.auth_info=delegated;
    check(server.receive(encode_ts_request(req)).complete,"delegation finalizes authenticated session");
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
    std::mt19937 rng(0x43535350);
    for (unsigned i=0;i<20000;++i) {
        Bytes b(rng()%256); for (auto& byte:b) byte=std::uint8_t(rng());
        try { (void)decode_ts_request(b); } catch (const ProtocolError&) {}
    }
}
}
int main() {
    try { run(); std::cout<<"PASS: CredSSP DER, TLS binding directions, authorization, delegation order, downgrade/tamper rejection and 20000 malformed records\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
