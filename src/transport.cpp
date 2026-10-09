#include "lrdp/transport.hpp"
#include <algorithm>
#include <limits>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
namespace lrdp {
namespace {
using Clock = std::chrono::steady_clock;
void ready(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        require(left > 0, "transport operation deadline exceeded");
        pollfd item{fd, events, 0}; const int rc = poll(&item, 1, int(std::min<std::int64_t>(left, INT_MAX)));
        if (rc < 0 && errno == EINTR) continue;
        require(rc > 0 && !(item.revents & (POLLERR | POLLHUP | POLLNVAL)), "transport disconnected or timed out");
        if (item.revents & events) return;
    }
}
void raw_read(int fd, std::span<std::uint8_t> bytes, Clock::time_point deadline) {
    while (!bytes.empty()) {
        ready(fd, POLLIN, deadline); const auto n = recv(fd, bytes.data(), bytes.size(), 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        require(n > 0, "negotiation connection closed"); bytes = bytes.subspan(std::size_t(n));
    }
}
}
Socket::~Socket() { if (fd_ >= 0) ::close(fd_); }
TlsContext::TlsContext(const std::string& certificate, const std::string& key) {
    // Configuration paths are local trusted input; permit certificate-manager
    // symlinks, but reject pipes/devices and bound parsing before OpenSSL reads.
    for (const auto* path : {&certificate, &key}) {
        struct stat info{};
        require(path->find('\0') == std::string::npos && stat(path->c_str(), &info) == 0 &&
                S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 4 * 1024 * 1024,
                "TLS credentials must be nonempty regular files at most 4 MiB");
    }
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    require(ctx != nullptr, "cannot create TLS context");
    // A service must fail rather than waiting for a terminal passphrase. Secure
    // password-provider configuration is not implemented; encrypted keys fail.
    SSL_CTX_set_default_passwd_cb(ctx.get(), [](char*, int, int, void*) { return 0; });
    require(SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) == 1, "cannot require TLS 1.2");
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_OFF);
    require(SSL_CTX_use_certificate_chain_file(ctx.get(), certificate.c_str()) == 1, "cannot load TLS certificate");
    require(SSL_CTX_use_PrivateKey_file(ctx.get(), key.c_str(), SSL_FILETYPE_PEM) == 1 && SSL_CTX_check_private_key(ctx.get()) == 1,
            "cannot load matching TLS private key"); ctx_ = ctx.release();
}
TlsContext::~TlsContext() { SSL_CTX_free(ctx_); }
Bytes read_negotiation(int fd) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    Bytes bytes(4); raw_read(fd, bytes, deadline); Reader header(bytes);
    require(header.u8() == 3 && header.u8() == 0, "invalid initial TPKT"); const auto size = header.be16();
    require(size >= 11 && size <= 260, "negotiation exceeds X.224 limit");
    bytes.resize(size); raw_read(fd, std::span(bytes).subspan(4), deadline); return bytes;
}
void write_raw(int fd, View bytes) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!bytes.empty()) {
        ready(fd, POLLOUT, deadline); const auto n = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        require(n > 0, "negotiation write failed"); bytes = bytes.subspan(std::size_t(n));
    }
}
TlsStream::TlsStream(int fd, TlsContext& context) : fd_(fd), write_wait_(POLLOUT), read_wait_(POLLIN) {
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context.get()), SSL_free);
    require(ssl && SSL_set_fd(ssl.get(), fd) == 1, "cannot bind TLS socket");
    const auto flags = fcntl(fd, F_GETFL, 0);
    require(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "cannot configure nonblocking socket");
    SSL_set_mode(ssl.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto rc = SSL_accept(ssl.get()); if (rc == 1) break;
        const auto error = SSL_get_error(ssl.get(), rc);
        require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE, "TLS handshake failed");
        ready(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline);
    }
    ssl_ = ssl.release();
}
TlsStream::~TlsStream() {
    OPENSSL_cleanse(input_.data(), input_.size()); OPENSSL_cleanse(read_buffer_.data(), read_buffer_.size()); SSL_free(ssl_);
}
void TlsStream::pump(int timeout_ms) {
    pollfd item{fd_, short(read_wait_ | (output_.queued() ? write_wait_ : 0)), 0};
    int rc; do { rc = poll(&item, 1, SSL_pending(ssl_) ? 0 : timeout_ms); } while (rc < 0 && errno == EINTR);
    require(rc >= 0 && !(item.revents & (POLLERR | POLLNVAL)), "TLS socket poll failed");
    // Service a blocked TLS write before another read when it requires read-side
    // handshake progress. Retry uses precisely the same packet and byte count.
    if (output_.queued() && ((item.revents & write_wait_) || (write_wait_ == POLLIN && SSL_pending(ssl_)))) {
        const auto front = output_.peek();
        if (!retry_size_) retry_size_ = std::min<std::size_t>(front.size(), 16384);
        const auto n = SSL_write(ssl_, front.data(), int(retry_size_));
        if (n > 0) {
            require(std::uint64_t(n) <= std::numeric_limits<std::uint64_t>::max() - written_, "TLS byte counter exhausted");
            written_ += std::uint64_t(n);
            if (packet_sent_ && std::size_t(n) == front.size()) packet_sent_(output_.active_packet(), written_, Clock::now());
            output_.consume(std::size_t(n)); retry_size_ = 0; write_wait_ = POLLOUT;
        }
        else {
            const auto error = SSL_get_error(ssl_, n);
            require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE, "TLS write failed");
            write_wait_ = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        }
    }
    if ((item.revents & (read_wait_ | POLLHUP)) || SSL_pending(ssl_)) {
        const auto n = SSL_read(ssl_, read_buffer_.data(), int(read_buffer_.size()));
        if (n > 0) {
            require(input_.size() + std::size_t(n) <= 128 * 1024, "session input queue exceeds quota");
            if (input_.empty()) partial_since_ = Clock::now();
            input_.insert(input_.end(), read_buffer_.begin(), read_buffer_.begin() + n);
            OPENSSL_cleanse(read_buffer_.data(), std::size_t(n)); read_wait_ = POLLIN;
        } else {
            const auto error = SSL_get_error(ssl_, n);
            require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE, "TLS peer disconnected");
            read_wait_ = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        }
    }
    if (partial_since_) require(Clock::now() - *partial_since_ < std::chrono::seconds(10), "incomplete packet deadline exceeded");
}
std::optional<Bytes> TlsStream::packet() {
    if (input_.size() < 2) return std::nullopt;
    std::size_t size = 0, header_size = 0;
    if (input_[0] == 3) {
        if (input_.size() < 4) return std::nullopt;
        require(input_[1] == 0, "invalid TPKT reserved field"); size = std::size_t(input_[2]) << 8 | input_[3]; header_size = 4;
    } else {
        const bool extended = (input_[1] & 0x80) != 0; header_size = extended ? 3 : 2;
        if (input_.size() < header_size) return std::nullopt;
        size = extended ? std::size_t(input_[1] & 0x7f) << 8 | input_[2] : input_[1];
    }
    require(size >= header_size && size <= 65535, "invalid packet framing length");
    if (input_.size() < size) return std::nullopt;
    Bytes result(input_.begin(), input_.begin() + std::ptrdiff_t(size));
    const auto remaining = input_.size() - size;
    std::memmove(input_.data(), input_.data() + size, remaining); OPENSSL_cleanse(input_.data() + remaining, size);
    input_.resize(remaining); partial_since_ = input_.empty() ? std::nullopt : std::optional(Clock::now()); return result;
}
} // namespace lrdp
