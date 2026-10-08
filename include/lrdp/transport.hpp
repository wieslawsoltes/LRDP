#pragma once
#include "wire.hpp"
#include "transport_queue.hpp"
#include <array>
#include <chrono>
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
    std::array<std::uint8_t, 16384> read_buffer_{};
    TransportQueue output_;
    short write_wait_ = 0, read_wait_ = 0;
    std::size_t retry_size_ = 0;
    std::uint64_t written_ = 0;
    std::optional<std::chrono::steady_clock::time_point> partial_since_;
public:
    TlsStream(int fd, TlsContext& context);
    ~TlsStream();
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    SSL* native_tls() const noexcept { return ssl_; }
    void enqueue(std::vector<Bytes> packets) { output_.enqueue(std::move(packets)); }
    void enqueue_media(std::vector<Bytes> packets) { output_.enqueue(std::move(packets), true); }
    void pump(int timeout_ms);
    std::optional<Bytes> packet();
    std::size_t queued() const { return output_.queued(); }
    std::size_t normal_queued() const { return output_.normal_queued(); }
    std::uint64_t bytes_written() const { return written_; }
};
} // namespace lrdp
