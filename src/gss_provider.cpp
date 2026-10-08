#include "lrdp/security/credssp.hpp"
#include "lrdp/security/ntlm_policy.hpp"
#include <cstring>
#include <gssapi/gssapi.h>
#include <openssl/crypto.h>

namespace lrdp {
namespace {
struct Buffer {
    gss_buffer_desc value = GSS_C_EMPTY_BUFFER;
    ~Buffer() { OM_uint32 minor = 0; if (value.value) { OPENSSL_cleanse(value.value, value.length); gss_release_buffer(&minor, &value); } }
    Bytes copy() const { const auto* p = static_cast<const std::uint8_t*>(value.value); return p ? Bytes(p, p + value.length) : Bytes{}; }
};
struct Name {
    gss_name_t value = GSS_C_NO_NAME;
    ~Name() { OM_uint32 minor = 0; if (value != GSS_C_NO_NAME) gss_release_name(&minor, &value); }
};
void success(OM_uint32 major, const char* operation) {
    require(major == GSS_S_COMPLETE, std::string(operation) + " failed (GSS status " + std::to_string(major) + ")");
}
bool oid(gss_OID value, View bytes) {
    return value && value->length == bytes.size() && std::memcmp(value->elements, bytes.data(), bytes.size()) == 0;
}
class GssProvider final : public SecurityProvider {
    gss_cred_id_t credential_ = GSS_C_NO_CREDENTIAL;
    gss_ctx_id_t context_ = GSS_C_NO_CONTEXT;
    bool established_ = false, allow_ntlm_, ntlm_ = false, ntlm_v2_seen_ = false;
    std::uint32_t receive_sequence_ = 0, send_sequence_ = 0;
public:
    explicit GssProvider(const GssOptions& options) : allow_ntlm_(options.allow_ntlm) {
        require(!options.service.empty() && options.service.size() <= 512 && options.service.find('@') != std::string::npos,
                "GSS service requires an explicit host-based name such as TERMSRV@host.example.org");
        OM_uint32 minor = 0; Name name;
        gss_buffer_desc input{options.service.size(), const_cast<char*>(options.service.data())};
        success(gss_import_name(&minor, &input, GSS_C_NT_HOSTBASED_SERVICE, &name.value), "import GSS acceptor service");
        success(gss_acquire_cred(&minor, name.value, GSS_C_INDEFINITE, GSS_C_NO_OID_SET, GSS_C_ACCEPT,
                                &credential_, nullptr, nullptr), "acquire GSS acceptor credentials");
    }
    ~GssProvider() override {
        OM_uint32 minor = 0;
        if (context_ != GSS_C_NO_CONTEXT) gss_delete_sec_context(&minor, &context_, GSS_C_NO_BUFFER);
        if (credential_ != GSS_C_NO_CREDENTIAL) gss_release_cred(&minor, &credential_);
    }
    SecurityStep accept(View token) override {
        require(!established_ && !token.empty() && token.size() <= 65536, "invalid GSS authentication state");
        if (const auto mechanism_token = spnego_mechanism_token(token); mechanism_token && ntlm_signature(*mechanism_token)) {
            require(allow_ntlm_, "NTLM is disabled by server policy");
            Reader header(*mechanism_token); header.skip(8);
            if (header.le32() == 3) { validate_ntlm_v2_authenticate(*mechanism_token); ntlm_v2_seen_ = true; }
        }
        OM_uint32 minor = 0, flags = 0; Name source; Buffer output; gss_OID mechanism = GSS_C_NO_OID;
        gss_buffer_desc input{token.size(), const_cast<std::uint8_t*>(token.data())};
        const auto major = gss_accept_sec_context(&minor, &context_, credential_, &input, GSS_C_NO_CHANNEL_BINDINGS,
                                                 &source.value, &mechanism, &output.value, &flags, nullptr, nullptr);
        require(major == GSS_S_COMPLETE || major == GSS_S_CONTINUE_NEEDED, "GSS authentication failed");
        SecurityStep step{output.copy(), major == GSS_S_COMPLETE, {}};
        if (step.complete) {
            const Bytes kerberos{0x2a,0x86,0x48,0x86,0xf7,0x12,1,2,2};
            const Bytes ms_kerberos{0x2a,0x86,0x48,0x82,0xf7,0x12,1,2,2};
            const Bytes ntlm{0x2b,6,1,4,1,0x82,0x37,2,2,0x0a};
            ntlm_ = oid(mechanism, ntlm);
            require(oid(mechanism, kerberos) || oid(mechanism, ms_kerberos) || (allow_ntlm_ && ntlm_),
                    "GSS selected a mechanism excluded by server policy");
            constexpr OM_uint32 protection = GSS_C_INTEG_FLAG | GSS_C_CONF_FLAG;
            require((flags & protection) == protection && !(flags & GSS_C_ANON_FLAG), "GSS context lacks authenticated confidentiality/integrity");
            if (ntlm_) {
                // GSS-NTLMSSP acceptors report SIGN/SEAL but not the abstract
                // GSS sequence/replay flags. Enforce ESS sequence numbers in
                // addition to mandatory GSS signature verification on each PDU.
                require(ntlm_v2_seen_, "GSS NTLM context did not authenticate an inspected NTLMv2 response");
            } else {
                constexpr OM_uint32 order = GSS_C_SEQUENCE_FLAG | GSS_C_REPLAY_FLAG;
                require((flags & order) == order, "Kerberos context lacks replay and sequence protection");
            }
            Buffer text; success(gss_display_name(&minor, source.value, &text.value, nullptr), "read authenticated principal");
            require(text.value.length > 0 && text.value.length <= 1024, "invalid GSS principal length");
            step.principal.assign(static_cast<const char*>(text.value.value), text.value.length);
            for (unsigned char c : step.principal) require(c >= 32 && c != 127, "GSS principal contains control characters");
            established_ = true;
        }
        return step;
    }
    Bytes seal(View plaintext) override {
        require(established_ && plaintext.size() <= 1024 * 1024, "GSS seal outside authenticated context");
        OM_uint32 minor = 0; int confidential = 0; Buffer output;
        gss_buffer_desc input{plaintext.size(), const_cast<std::uint8_t*>(plaintext.data())};
        success(gss_wrap(&minor, context_, 1, GSS_C_QOP_DEFAULT, &input, &confidential, &output.value), "GSS encrypt");
        require(confidential != 0 && output.value.length <= 1024 * 1024, "GSS did not encrypt the message");
        auto result = output.copy();
        if (ntlm_) { require(send_sequence_ < 16, "excessive NTLM protected messages"); validate_ntlm_sequence(result, send_sequence_++); }
        return result;
    }
    Bytes unseal(View ciphertext) override {
        require(established_ && !ciphertext.empty() && ciphertext.size() <= 1024 * 1024, "GSS unseal outside authenticated context");
        if (ntlm_) { require(receive_sequence_ < 16, "excessive NTLM protected messages"); validate_ntlm_sequence(ciphertext, receive_sequence_); }
        OM_uint32 minor = 0; int confidential = 0; Buffer output; gss_qop_t qop = GSS_C_QOP_DEFAULT;
        gss_buffer_desc input{ciphertext.size(), const_cast<std::uint8_t*>(ciphertext.data())};
        success(gss_unwrap(&minor, context_, &input, &output.value, &confidential, &qop), "GSS decrypt");
        require(confidential != 0 && qop == GSS_C_QOP_DEFAULT && output.value.length <= 1024 * 1024, "invalid GSS encrypted message");
        if (ntlm_) ++receive_sequence_;
        return output.copy();
    }
};
}
std::unique_ptr<SecurityProvider> make_gss_provider(const GssOptions& options) { return std::make_unique<GssProvider>(options); }
} // namespace lrdp
