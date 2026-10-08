#pragma once
#include "wire.hpp"
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <openssl/ssl.h>

namespace lrdp {
class Socket {
    int fd_ = -1;
public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int get() const { return fd_; }
};
class TlsContext {
    SSL_CTX* ctx_ = nullptr;
public:
    TlsContext(const std::string& certificate, const std::string& key);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    SSL_CTX* get() const { return ctx_; }
};
Bytes read_negotiation(int socket);
void write_raw(int socket, View bytes);
class TlsStream {
    SSL* ssl_ = nullptr;
    int fd_;
    Bytes input_;
    std::deque<Bytes> output_;
    std::size_t output_offset_ = 0, queued_ = 0;
    short write_wait_ = 0;
    std::optional<std::chrono::steady_clock::time_point> partial_since_;
public:
    TlsStream(int fd, TlsContext& context);
    ~TlsStream();
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    // Only the pre-RDP authentication stage may use this handle directly.
    SSL* native_tls() const noexcept { return ssl_; }
    void enqueue(std::vector<Bytes> packets);
    void pump(int timeout_ms);
    std::optional<Bytes> packet();
    std::size_t queued() const { return queued_; }
};
} // namespace lrdp
