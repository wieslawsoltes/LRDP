#include "lrdp/security/nla_transport.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <openssl/crypto.h>
#include <openssl/x509.h>

namespace lrdp {
namespace {
using Clock = std::chrono::steady_clock;
void wait_tls(SSL* tls, int fd, int result, Clock::time_point deadline) {
    const auto error = SSL_get_error(tls, result);
    require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE, "CredSSP TLS connection ended");
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        require(left > 0, "CredSSP authentication deadline exceeded");
        pollfd item{fd, short(error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT), 0};
        const int rc = poll(&item, 1, int(std::min<std::int64_t>(left, 30000)));
        if (rc < 0 && errno == EINTR) continue;
        require(rc > 0 && !(item.revents & (POLLERR | POLLNVAL)), "CredSSP authentication transport failed"); return;
    }
}
void read_exact(SSL* tls, int fd, std::span<std::uint8_t> output, Clock::time_point deadline) {
    while (!output.empty()) {
        require(Clock::now() < deadline, "CredSSP authentication deadline exceeded");
        const auto n = SSL_read(tls, output.data(), int(std::min<std::size_t>(output.size(), 16384)));
        if (n > 0) output = output.subspan(std::size_t(n)); else wait_tls(tls, fd, n, deadline);
    }
}
void write_exact(SSL* tls, int fd, View input, Clock::time_point deadline) {
    while (!input.empty()) {
        require(Clock::now() < deadline, "CredSSP authentication deadline exceeded");
        const auto n = SSL_write(tls, input.data(), int(std::min<std::size_t>(input.size(), 16384)));
        if (n > 0) input = input.subspan(std::size_t(n)); else wait_tls(tls, fd, n, deadline);
    }
}
Bytes request(SSL* tls, int fd, Clock::time_point deadline) {
    Bytes bytes(2); read_exact(tls, fd, bytes, deadline);
    require(bytes[0] == 0x30, "expected CredSSP TSRequest");
    std::size_t length = bytes[1];
    if (length & 0x80) {
        const auto count = length & 0x7f; require(count > 0 && count <= 3, "invalid CredSSP record length");
        bytes.resize(2 + count); read_exact(tls, fd, std::span(bytes).subspan(2), deadline);
        length = 0; for (std::size_t i = 2; i < bytes.size(); ++i) length = (length << 8) | bytes[i];
    }
    require(length <= 1024 * 1024 - bytes.size(), "CredSSP record exceeds quota");
    const auto header = bytes.size(); bytes.resize(header + length);
    read_exact(tls, fd, std::span(bytes).subspan(header), deadline); return bytes;
}
}
std::string authenticate_nla(SSL* tls, int fd, const GssOptions& options,
                             const std::function<bool(const std::string&)>& authorize) {
    require(tls != nullptr && SSL_is_init_finished(tls), "NLA requires an established TLS connection");
    auto* certificate = SSL_get_certificate(tls); require(certificate != nullptr, "NLA server TLS certificate missing");
    const auto* key = X509_get0_pubkey_bitstr(certificate);
    require(key && key->length > 0, "TLS certificate SubjectPublicKey unavailable");
    auto provider = make_gss_provider(options);
    CredsspServer server(*provider, Bytes(key->data, key->data + key->length), authorize);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    for (;;) {
        auto bytes = request(tls, fd, deadline);
        struct Wipe { Bytes& bytes; ~Wipe() { OPENSSL_cleanse(bytes.data(), bytes.size()); } } wipe{bytes};
        const auto result = server.receive(bytes);
        if (result.packet) write_exact(tls, fd, *result.packet, deadline);
        if (result.complete) return server.principal();
    }
}
} // namespace lrdp
