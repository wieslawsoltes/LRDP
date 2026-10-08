#pragma once
#include "lrdp/wire.hpp"
#include <functional>
#include <memory>
#include <optional>

namespace lrdp {
struct TsRequest {
    std::uint32_t version = 0;
    std::optional<View> token, auth_info, public_key_auth, nonce;
    std::optional<std::uint32_t> error;
};
TsRequest decode_ts_request(View bytes);
Bytes encode_ts_request(const TsRequest& request);
Bytes credssp_binding_hash(View subject_public_key, View nonce, bool server_to_client);
struct SecurityStep { Bytes token; bool complete = false; std::string principal; };
class SecurityProvider {
public:
    virtual ~SecurityProvider() = default;
    virtual SecurityStep accept(View token) = 0;
    virtual Bytes seal(View plaintext) = 0;
    virtual Bytes unseal(View ciphertext) = 0;
};
struct CredsspReply { std::optional<Bytes> packet; bool complete = false; };
class CredsspServer {
    enum class State { negotiate, binding, credentials, complete, failed };
    SecurityProvider& provider_;
    Bytes public_key_, nonce_;
    std::function<bool(const std::string&)> authorize_;
    State state_ = State::negotiate;
    std::uint32_t peer_version_ = 0, version_ = 6;
    std::string principal_;
    unsigned rounds_ = 0;
public:
    CredsspServer(SecurityProvider& provider, Bytes subject_public_key,
                  std::function<bool(const std::string&)> authorize);
    CredsspReply receive(View message);
    const std::string& principal() const { return principal_; }
};
struct GssOptions { std::string service; bool allow_ntlm = false; };
std::unique_ptr<SecurityProvider> make_gss_provider(const GssOptions& options);
} // namespace lrdp
