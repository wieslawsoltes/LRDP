#include "lrdp/network_autodetect.hpp"
#include "lrdp/mcs.hpp"
#include <algorithm>
#include <limits>

namespace lrdp {
namespace {
using Clock = NetworkAutodetect::Clock;
using Time = NetworkAutodetect::Time;
void increment(std::uint64_t& value) { if (value != std::numeric_limits<std::uint64_t>::max()) ++value; }
std::uint64_t micros(Time newer, Time older) {
    require(newer >= older, "network monotonic clock moved backwards");
    return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(newer - older).count());
}
std::optional<std::uint64_t> age(Time now, std::optional<Time> sample) {
    if (!sample) return {};
    return micros(now, *sample) / 1000;
}
}
Bytes network_request(NetworkRequest type, std::uint16_t sequence) {
    require(type == NetworkRequest::rtt || type == NetworkRequest::bandwidth_start || type == NetworkRequest::bandwidth_stop,
            "unsupported continuous network request");
    Writer out(10);
    out.le16(0x1000).le16(0).u8(6).u8(0).le16(sequence).le16(static_cast<std::uint16_t>(type));
    return std::move(out).finish();
}
NetworkResponse decode_network_response(View payload) {
    // Entire payload validated before any request state is consumed. No allocation.
    require(payload.size() == 10 || payload.size() == 18, "invalid network response size");
    Reader in(payload); const auto flags = in.le16();
    // RESET_SEQNO/IGNORE_SEQNO and flagsHi are ignored as required by 2.2.8.1.1.2.1.
    require((flags & ~0x8030U) == 0x2000, "invalid network response security flags");
    in.skip(2);
    const auto length = in.u8(); require(length == payload.size() - 4 && in.u8() == 1, "invalid network response header");
    const auto sequence = in.le16(), type = in.le16();
    NetworkResponse result{NetworkResponseKind::rtt, sequence, 0, 0};
    if (type == 0) require(length == 6, "invalid RTT response length");
    else {
        require(type == 0x0b && length == 14, "unrequested connect-time or unknown network response");
        result.kind = NetworkResponseKind::bandwidth; result.milliseconds = in.le32(); result.bytes = in.le32();
    }
    in.end(); return result;
}
NetworkAutodetect::NetworkAutodetect(std::uint16_t channel, NetworkPolicy policy) : channel_(channel), policy_(policy) {
    using namespace std::chrono_literals;
    require(channel > global_channel && policy.rtt_interval >= 1s && policy.rtt_interval <= 60s &&
            policy.bandwidth_interval >= 1s && policy.bandwidth_interval <= 60s &&
            policy.bandwidth_window >= 100ms && policy.bandwidth_window <= 1s &&
            policy.response_timeout >= 1s && policy.response_timeout <= 30s &&
            policy.minimum_bytes >= 64 && policy.minimum_bytes <= 1024 * 1024, "invalid network measurement policy");
}
void NetworkAutodetect::clock(Time now) {
    require(!last_time_ || now >= *last_time_, "network clock regressed"); last_time_ = now;
}
void NetworkAutodetect::start(Time now) {
    clock(now); require(!started_, "network measurements already started"); started_ = now;
    next_rtt_ = now; next_bandwidth_ = now;
}
Bytes NetworkAutodetect::packet(NetworkRequest request, std::uint16_t sequence) const {
    return mcs_data(channel_, network_request(request, sequence));
}
void NetworkAutodetect::expire(Time now) {
    if (rtt_ && rtt_->sent && now - *rtt_->sent >= policy_.response_timeout) {
        rtt_.reset(); increment(metrics_.timeouts);
        metrics_.rtt_disabled = ++rtt_failures_ >= 3;
        next_rtt_ = now + policy_.rtt_interval * (1U << std::min(rtt_failures_, 3U));
    }
    if (bandwidth_ && bandwidth_->phase == BandwidthPhase::awaiting && now - bandwidth_->stopped >= policy_.response_timeout) {
        bandwidth_.reset(); increment(metrics_.timeouts);
        metrics_.bandwidth_disabled = ++bandwidth_failures_ >= 3;
        next_bandwidth_ = now + policy_.bandwidth_interval * (1U << std::min(bandwidth_failures_, 3U));
    }
}
NetworkBatch NetworkAutodetect::poll(Time now, bool active, bool idle, bool data_pending) {
    clock(now); expire(now); NetworkBatch result;
    if (!started_) return result;
    // A STOP is never gated on idleness, graphics activity or the ability to start
    // another probe. It follows existing normal packets and terminates one window.
    if (bandwidth_ && bandwidth_->phase == BandwidthPhase::collecting && now - bandwidth_->started >= policy_.bandwidth_window) {
        result.after_data.push_back(packet(NetworkRequest::bandwidth_stop, bandwidth_->sequence));
        bandwidth_->phase = BandwidthPhase::stop_queued;
    }
    if (!active || !idle) return result;
    // IDs are never reused. At exhaustion, monitoring stops (not the desktop),
    // rather than letting delayed client responses alias newly issued operations.
    if (sequence_ > 65535) { metrics_.sequence_exhausted = true; return result; }
    if (!metrics_.rtt_disabled && !rtt_ && now >= next_rtt_) {
        const auto id = std::uint16_t(sequence_);
        result.before_data.push_back(packet(NetworkRequest::rtt, id)); rtt_ = Rtt{id, now, {}}; ++sequence_;
    }
    if (sequence_ <= 65535 && !metrics_.bandwidth_disabled && !bandwidth_ && data_pending && now >= next_bandwidth_) {
        const auto id = std::uint16_t(sequence_);
        result.before_data.push_back(packet(NetworkRequest::bandwidth_start, id));
        bandwidth_ = Bandwidth{id, BandwidthPhase::start_queued, now}; ++sequence_;
    }
    return result;
}
void NetworkAutodetect::transmitted(View whole_packet, std::uint64_t total_bytes, Time now) {
    clock(now);
    require(total_bytes >= written_ && total_bytes - written_ >= whole_packet.size(), "network transport byte counter regressed");
    written_ = total_bytes;
    if (!started_ || whole_packet.empty() || whole_packet[0] != 3) return;
    // Only locally constructed server traffic is inspected here, never peer data.
    // TPKT messages other than our ten-byte message-channel payload are ignored.
    Reader mcs(parse_x224_data(whole_packet));
    if (mcs.u8() != 0x68) return;
    mcs.skip(2); if (mcs.be16() != channel_) return;
    require(mcs.u8() == 0x70, "invalid local network MCS priority");
    Reader in(mcs.take(mcs.per_length())); mcs.end();
    require(in.remaining() == 10 && in.le16() == 0x1000 && in.le16() == 0 && in.u8() == 6 && in.u8() == 0,
            "invalid local network request");
    const auto id = in.le16(); const auto type = static_cast<NetworkRequest>(in.le16());
    if (type == NetworkRequest::rtt) {
        require(rtt_ && rtt_->sequence == id && !rtt_->sent, "untracked RTT transmission");
        rtt_->sent = now; metrics_.last_queue_delay_us = micros(now, rtt_->queued);
        next_rtt_ = now + policy_.rtt_interval;
    } else if (type == NetworkRequest::bandwidth_start) {
        require(bandwidth_ && bandwidth_->sequence == id && bandwidth_->phase == BandwidthPhase::start_queued, "untracked bandwidth START");
        bandwidth_->phase = BandwidthPhase::collecting; bandwidth_->started = now; bandwidth_->start_bytes = written_;
    } else {
        require(type == NetworkRequest::bandwidth_stop && bandwidth_ && bandwidth_->sequence == id &&
                bandwidth_->phase == BandwidthPhase::stop_queued, "untracked bandwidth STOP");
        bandwidth_->phase = BandwidthPhase::awaiting; bandwidth_->stopped = now;
        // Loose upper bound includes protocol/transport plaintext headers and STOP.
        // It must not be confused with the peer's smaller protocol byte counter.
        bandwidth_->wire_bytes = written_ - bandwidth_->start_bytes;
        next_bandwidth_ = now + policy_.bandwidth_interval;
    }
}
void NetworkAutodetect::receive(View payload, Time now) {
    const auto response = decode_network_response(payload); // Transactional parse.
    clock(now); require(started(), "network response before completed RDP activation"); expire(now);
    if (response.kind == NetworkResponseKind::rtt) {
        if (!rtt_ || !rtt_->sent || response.sequence != rtt_->sequence) { increment(metrics_.ignored_responses); return; }
        const auto elapsed = micros(now, *rtt_->sent); rtt_.reset(); rtt_failures_ = 0;
        const auto previous = metrics_.smoothed_rtt_us.value_or(elapsed);
        const auto difference = elapsed >= previous ? elapsed - previous : previous - elapsed;
        metrics_.jitter_us = metrics_.jitter_us ? (*metrics_.jitter_us * 3 + difference) / 4 : 0;
        metrics_.smoothed_rtt_us = (previous * 7 + elapsed) / 8; metrics_.rtt_us = elapsed;
        rtt_history_[history_next_] = elapsed; history_next_ = (history_next_ + 1) % rtt_history_.size();
        history_count_ = std::min(history_count_ + 1, rtt_history_.size());
        metrics_.minimum_rtt_us = *std::min_element(rtt_history_.begin(), rtt_history_.begin() + std::ptrdiff_t(history_count_));
        last_rtt_ = now; increment(metrics_.rtt_samples); return;
    }
    if (!bandwidth_ || bandwidth_->phase != BandwidthPhase::awaiting || response.sequence != bandwidth_->sequence) {
        increment(metrics_.ignored_responses); return;
    }
    const auto measured = *bandwidth_; bandwidth_.reset(); bandwidth_failures_ = 0;
    const auto local_ms = micros(now, measured.started) / 1000;
    if (response.milliseconds == 0 || response.bytes < policy_.minimum_bytes || response.bytes > measured.wire_bytes ||
        response.milliseconds > 30000 || response.milliseconds > local_ms + 1000) {
        increment(metrics_.invalid_measurements); return;
    }
    metrics_.peer_bytes = response.bytes; metrics_.peer_milliseconds = response.milliseconds;
    metrics_.measured_wire_bytes = measured.wire_bytes;
    // bytes * 8 / milliseconds == decimal kilobits/second. Never narrow to 32 bits.
    metrics_.peer_kbps = std::uint64_t(response.bytes) * 8 / response.milliseconds;
    last_bandwidth_ = now; increment(metrics_.bandwidth_samples);
}
std::optional<std::uint64_t> NetworkAutodetect::rtt_age_ms(Time now) const { return age(now, last_rtt_); }
std::optional<std::uint64_t> NetworkAutodetect::bandwidth_age_ms(Time now) const { return age(now, last_bandwidth_); }
} // namespace lrdp
