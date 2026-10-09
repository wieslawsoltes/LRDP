#pragma once
#include "wire.hpp"
#include <array>
#include <chrono>
#include <optional>

namespace lrdp {
// MS-RDPBCGR 2.2.14. These PDUs are carried on the dedicated MCS message
// channel with a Basic Security Header, not as Share Data or virtual channels.
enum class NetworkRequest : std::uint16_t { rtt = 0x0001, bandwidth_start = 0x0014, bandwidth_stop = 0x0429 };
enum class NetworkResponseKind { rtt, bandwidth };
struct NetworkResponse {
    NetworkResponseKind kind;
    std::uint16_t sequence = 0;
    std::uint32_t milliseconds = 0, bytes = 0;
};
Bytes network_request(NetworkRequest type, std::uint16_t sequence);
NetworkResponse decode_network_response(View security_and_body);

struct NetworkPolicy {
    std::chrono::milliseconds rtt_interval{2000};
    std::chrono::milliseconds bandwidth_interval{5000};
    std::chrono::milliseconds bandwidth_window{250};
    std::chrono::milliseconds response_timeout{5000};
    std::uint32_t minimum_bytes = 1024;
};
struct NetworkMetrics {
    // Server monotonic-clock RTT. EWMA and jitter are diagnostics, not frame credits.
    std::optional<std::uint64_t> rtt_us, minimum_rtt_us, smoothed_rtt_us, jitter_us;
    // Peer-reported application-limited receive throughput, NOT available link capacity.
    std::optional<std::uint64_t> peer_kbps;
    std::uint32_t peer_bytes = 0, peer_milliseconds = 0;
    std::uint64_t measured_wire_bytes = 0, rtt_samples = 0, bandwidth_samples = 0;
    std::uint64_t ignored_responses = 0, invalid_measurements = 0, timeouts = 0;
    std::uint64_t last_queue_delay_us = 0;
    bool sequence_exhausted = false, rtt_disabled = false, bandwidth_disabled = false;
};

// One network-owner thread; two bounded outstanding operations. The clocks are
// supplied by the transport, enabling exact deterministic timeout/race tests.
class NetworkAutodetect final {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
private:
    struct Rtt { std::uint16_t sequence; Time queued; std::optional<Time> sent; };
    enum class BandwidthPhase { start_queued, collecting, stop_queued, awaiting };
    struct Bandwidth {
        std::uint16_t sequence;
        BandwidthPhase phase = BandwidthPhase::start_queued;
        Time queued{}, started{}, stopped{};
        std::uint64_t start_bytes = 0, wire_bytes = 0;
    };
    std::uint16_t channel_;
    NetworkPolicy policy_;
    NetworkMetrics metrics_;
    std::optional<Rtt> rtt_;
    std::optional<Bandwidth> bandwidth_;
    std::optional<Time> started_, last_time_, last_rtt_, last_bandwidth_;
    Time next_rtt_{}, next_bandwidth_{};
    std::uint32_t sequence_ = 1;
    std::uint64_t written_ = 0;
    unsigned rtt_failures_ = 0, bandwidth_failures_ = 0;
    std::array<std::uint64_t, 32> rtt_history_{};
    std::size_t history_count_ = 0, history_next_ = 0;
    void clock(Time now);
    void expire(Time now);
    Bytes packet(NetworkRequest request, std::uint16_t sequence) const;
public:
    explicit NetworkAutodetect(std::uint16_t channel, NetworkPolicy policy = {});
    void start(Time now); // Called once, after the first Font Map actually leaves TLS.
    bool started() const noexcept { return started_.has_value(); }
    std::uint16_t channel() const noexcept { return channel_; }
    // Existing measurement stop/deadlines progress even during reactivation.
    // New probes require an empty transport queue and a fully active RDP session.
    std::vector<Bytes> poll(Time now, bool active, bool transport_idle, bool application_data_pending);
    // Called once per FULLY transmitted plaintext RDP packet, including media.
    // The byte counter includes that packet, and must never reset during a connection.
    void transmitted(View whole_packet, std::uint64_t total_bytes, Time now);
    void receive(View security_and_body, Time now);
    const NetworkMetrics& metrics() const noexcept { return metrics_; }
    std::optional<std::uint64_t> rtt_age_ms(Time now) const;
    std::optional<std::uint64_t> bandwidth_age_ms(Time now) const;
};
} // namespace lrdp
