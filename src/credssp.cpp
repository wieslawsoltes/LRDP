#include "lrdp/security/credssp.hpp"
#include <algorithm>
#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace lrdp {
namespace {
View der(Reader& in, unsigned expected) {
    require(in.u8() == expected, "unexpected CredSSP DER tag");
    const auto first = in.u8(); std::size_t length = first;
    if (first & 0x80) {
        const unsigned count = first & 0x7f;
        require(count > 0 && count <= 3, "invalid CredSSP DER length");
        length = in.u8(); require(length != 0, "noncanonical CredSSP DER length");
        for (unsigned i = 1; i < count; ++i) length = (length << 8) | in.u8();
        require(length >= 128, "overlong CredSSP DER length");
    }
    require(length <= 1024 * 1024, "CredSSP allocation quota exceeded"); return in.take(length);
}
std::uint32_t integer(Reader& in) {
    Reader value(der(in, 2));
    require(value.remaining() > 0 && value.remaining() <= 5, "invalid CredSSP integer");
    const auto first = value.u8(); require(!(first & 0x80), "negative CredSSP integer");
    if (first == 0 && !value.empty()) {
        const auto next = value.u8(); require(next & 0x80, "noncanonical CredSSP integer");
        std::uint32_t result = next; while (!value.empty()) result = (result << 8) | value.u8(); return result;
    }
    require(value.remaining() < 4, "CredSSP integer overflow");
    std::uint32_t result = first; while (!value.empty()) result = (result << 8) | value.u8(); return result;
}
void unicode(View bytes) {
    require(bytes.size() <= 65536 && bytes.size() % 2 == 0, "invalid delegated UTF-16 length");
    Reader in(bytes);
    while (!in.empty()) {
        const auto c = in.le16();
        if (c >= 0xd800 && c <= 0xdbff) { const auto low = in.le16(); require(low >= 0xdc00 && low <= 0xdfff, "invalid delegated surrogate pair"); }
        else require(c < 0xdc00 || c > 0xdfff, "unpaired delegated surrogate");
    }
}
void validate_delegation(View bytes) {
    Reader outer(bytes); Reader envelope(der(outer, 0x30)); outer.end();
    Reader type(der(envelope, 0xa0)); require(integer(type) == 1, "only TSPasswordCreds delegation is supported"); type.end();
    Reader data(der(envelope, 0xa1)); Reader payload(der(data, 4)); data.end(); envelope.end();
    Reader password(der(payload, 0x30)); payload.end();
    for (unsigned tag = 0xa0; tag <= 0xa2; ++tag) { Reader item(der(password, tag)); unicode(der(item, 4)); item.end(); }
    password.end();
    // The already-authenticated GSS principal is the authorization identity.
    // Delegated passwords are intentionally neither logged, stored nor executed.
}
struct Wipe { Bytes& bytes; ~Wipe() { OPENSSL_cleanse(bytes.data(), bytes.size()); } };
}
TsRequest decode_ts_request(View bytes) {
    require(bytes.size() <= 1024 * 1024, "CredSSP message too large");
    Reader outer(bytes); Reader fields(der(outer, 0x30)); outer.end(); TsRequest result;
    Reader version(der(fields, 0xa0)); result.version = integer(version); version.end();
    unsigned last = 0xa0;
    while (!fields.empty()) {
        Reader peek(bytes.subspan(bytes.size() - fields.remaining())); const auto tag = peek.u8();
        require(tag > last && tag <= 0xa5, "duplicate, unknown or unordered CredSSP field"); last = tag;
        Reader field(der(fields, tag));
        if (tag == 0xa1) {
            Reader list(der(field, 0x30)); Reader item(der(list, 0x30)); list.end();
            Reader token(der(item, 0xa0)); item.end(); result.token = der(token, 4); token.end();
            require(!result.token->empty() && result.token->size() <= 65536, "invalid SPNEGO token size");
        } else if (tag == 0xa4) result.error = integer(field);
        else {
            auto value = der(field, 4);
            if (tag == 0xa2) result.auth_info = value;
            else if (tag == 0xa3) result.public_key_auth = value;
            else result.nonce = value;
        }
        field.end();
    }
    return result;
}
Bytes encode_ts_request(const TsRequest& request) {
    Writer fields; fields.raw(ber(0xa0, ber_integer(request.version)));
    if (request.token) fields.raw(ber(0xa1, ber(0x30, ber(0x30, ber(0xa0, ber(4, *request.token))))));
    if (request.auth_info) fields.raw(ber(0xa2, ber(4, *request.auth_info)));
    if (request.public_key_auth) fields.raw(ber(0xa3, ber(4, *request.public_key_auth)));
    if (request.error) fields.raw(ber(0xa4, ber_integer(*request.error)));
    if (request.nonce) fields.raw(ber(0xa5, ber(4, *request.nonce)));
    return ber(0x30, fields.bytes());
}
Bytes credssp_binding_hash(View key, View nonce, bool server) {
    require(nonce.size() == 32 && !key.empty() && key.size() <= 16384, "invalid CredSSP TLS binding material");
    static constexpr char client_magic[] = "CredSSP Client-To-Server Binding Hash";
    static constexpr char server_magic[] = "CredSSP Server-To-Client Binding Hash";
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1, "cannot initialize TLS binding digest");
    const auto* magic = server ? server_magic : client_magic;
    const auto size = server ? sizeof(server_magic) : sizeof(client_magic);
    require(EVP_DigestUpdate(context.get(), magic, size) == 1 && EVP_DigestUpdate(context.get(), nonce.data(), nonce.size()) == 1 &&
            EVP_DigestUpdate(context.get(), key.data(), key.size()) == 1, "cannot hash TLS binding");
    Bytes hash(32); unsigned length = 0;
    require(EVP_DigestFinal_ex(context.get(), hash.data(), &length) == 1 && length == 32, "invalid TLS binding digest"); return hash;
}
CredsspServer::CredsspServer(SecurityProvider& provider, Bytes key, std::function<bool(const std::string&)> authorize)
    : provider_(provider), public_key_(std::move(key)), authorize_(std::move(authorize)) {
    require(!public_key_.empty() && public_key_.size() <= 16384 && bool(authorize_), "CredSSP requires a TLS public key and authorization policy");
}
CredsspReply CredsspServer::receive(View message) {
    try {
        require(state_ != State::complete && state_ != State::failed && ++rounds_ <= 16, "invalid CredSSP state or excessive round trips");
        const auto request = decode_ts_request(message);
        require(request.version >= 5 && (!peer_version_ || request.version == peer_version_), "insecure or changing CredSSP version");
        peer_version_ = request.version; version_ = std::min(6U, request.version);
        require(!request.error || *request.error == 0, "CredSSP peer reported an authentication error");
        if (state_ == State::credentials) {
            require(request.auth_info && !request.token && !request.public_key_auth && !request.nonce, "unexpected CredSSP credential-phase fields");
            auto plaintext = provider_.unseal(*request.auth_info); Wipe wipe{plaintext}; validate_delegation(plaintext);
            state_ = State::complete; return {std::nullopt, true};
        }
        require(!request.auth_info, "delegated credentials arrived before TLS binding");
        Bytes output;
        if (state_ == State::negotiate) {
            require(request.token.has_value(), "missing CredSSP SPNEGO token");
            auto step = provider_.accept(*request.token); output = std::move(step.token);
            if (step.complete) {
                require(!step.principal.empty() && authorize_(step.principal), "authenticated principal is not authorized for this desktop");
                principal_ = std::move(step.principal); state_ = State::binding;
            }
        } else require(!request.token, "SPNEGO token after authentication completed");
        TsRequest response; response.version = version_;
        if (!output.empty()) response.token = View(output);
        Bytes binding;
        if (request.public_key_auth) {
            require(state_ == State::binding && request.nonce && request.nonce->size() == 32, "premature or incomplete CredSSP TLS binding");
            auto actual = provider_.unseal(*request.public_key_auth); Wipe wipe{actual};
            const auto expected = credssp_binding_hash(public_key_, *request.nonce, false);
            require(actual.size() == expected.size() && CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0, "CredSSP TLS public-key binding mismatch");
            binding = provider_.seal(credssp_binding_hash(public_key_, *request.nonce, true)); response.public_key_auth = View(binding);
            state_ = State::credentials;
        } else require(!request.nonce, "CredSSP nonce without public-key authentication");
        if (!response.token && !response.public_key_auth) return {};
        return {encode_ts_request(response), false};
    } catch (...) { state_ = State::failed; throw; }
}
} // namespace lrdp
