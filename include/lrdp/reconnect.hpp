#pragma once
#include "wire.hpp"
#include <array>
#include <optional>

namespace lrdp {
// MS-RDPBCGR 2.2.4.2/3. Only Enhanced RDP Security is implemented.
struct ReconnectCookie {
    std::uint32_t logon_id = 0;
    std::array<std::uint8_t, 16> bytes{}; // Server random OR client verifier, according to direction.
    bool operator==(const ReconnectCookie&) const = default;
};
ReconnectCookie decode_reconnect_cookie(View bytes);
Bytes encode_reconnect_cookie(const ReconnectCookie& cookie);
// Validates Client Info without copying/storing any credential string. Only the
// optional verifier survives the call. An absent extension remains compatible.
std::optional<ReconnectCookie> client_info_reconnect(View payload);
Bytes reconnect_logon_info(const ReconnectCookie& server_cookie);
class ReconnectService {
public:
    virtual ~ReconnectService() = default;
    virtual void prepare(const std::optional<ReconnectCookie>& client_cookie) = 0;
    virtual std::optional<ReconnectCookie> activated(bool peer_supports_reconnect) = 0;
    virtual std::optional<ReconnectCookie> refresh() = 0;
};
} // namespace lrdp
