#pragma once
#include "lrdp/wire.hpp"
namespace lrdp {
// GSS-NTLMSSP includes the C terminator in gss_display_name's buffer length;
// Kerberos normally excludes it. Remove exactly that terminator, never a domain,
// whitespace, embedded NUL, case distinction, or any other identity component.
inline std::string authenticated_principal(View buffer) {
    require(!buffer.empty() && buffer.size() <= 1025, "invalid GSS principal length");
    if (buffer.back() == 0) buffer = buffer.first(buffer.size() - 1);
    require(!buffer.empty() && buffer.size() <= 1024, "empty or oversized GSS principal");
    for (auto byte : buffer) require(byte >= 32 && byte != 127, "GSS principal contains embedded control characters");
    return {buffer.begin(), buffer.end()};
}
} // namespace lrdp
