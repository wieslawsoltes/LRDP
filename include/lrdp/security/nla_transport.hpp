#pragma once
#include "credssp.hpp"
#include <openssl/ssl.h>

namespace lrdp {
// Completes CredSSP over an already-handshaken TLS socket before TPKT parsing.
// A single absolute deadline covers all fragments and all authentication rounds.
std::string authenticate_nla(SSL* tls, int fd, const GssOptions& options,
                             const std::function<bool(const std::string&)>& authorize);
} // namespace lrdp
