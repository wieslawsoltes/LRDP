#include "lrdp/network_autodetect.hpp"
#include "lrdp/mcs.hpp"
#include <functional>
#include <iostream>
#include <random>

using namespace lrdp;
using namespace std::chrono_literals;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) { ++checks; try { action(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected ProtocolError"); }
Bytes response(std::uint16_t seq, bool bandwidth = false, std::uint32_t ms = 0, std::uint32_t bytes = 0) {
    Writer out; out.le16(0x2000).le16(0).u8(bandwidth ? 14 : 6).u8(1).le16(seq).le16(bandwidth ? 11 : 0);
    if (bandwidth) out.le32(ms).le32(bytes);
    return std::move(out).finish();
}
std::uint16_t seq(const Bytes& packet) { check(packet.size() == 24, "probe framing size"); return std::uint16_t(packet[20] | unsigned(packet[21]) << 8); }
unsigned type(const Bytes& packet) { return packet[22] | unsigned(packet[23]) << 8; }
void wire() {
    check(network_request(NetworkRequest::rtt, 0x1234) == Bytes({0,16,0,0,6,0,0x34,0x12,1,0}), "RTT wire golden");
    check(network_request(NetworkRequest::bandwidth_start, 0x1234) == Bytes({0,16,0,0,6,0,0x34,0x12,0x14,0}), "START wire golden");
    check(network_request(NetworkRequest::bandwidth_stop, 0x1234) == Bytes({0,16,0,0,6,0,0x34,0x12,0x29,4}), "continuous STOP has no payload length");
    rejects([] { (void)network_request(static_cast<NetworkRequest>(0x1001), 1); });
    const auto r = decode_network_response(Bytes{0,32,0,0,6,1,0x34,0x12,0,0});
    check(r.kind == NetworkResponseKind::rtt && r.sequence == 0x1234, "independent RTT response golden");
    auto sample = response(9, true, 100, 50000);
    const auto b = decode_network_response(sample);
    check(b.kind == NetworkResponseKind::bandwidth && b.sequence == 9 && b.milliseconds == 100 && b.bytes == 50000, "bandwidth fields");
    for (std::size_t n = 0; n < sample.size(); ++n) rejects([&] { (void)decode_network_response(View(sample).first(n)); });
    for (unsigned flag : {1U,4U,8U,64U,128U,0x1000U,0x4000U}) {
        auto invalid = sample; invalid[0] |= std::uint8_t(flag); invalid[1] |= std::uint8_t(flag >> 8);
        rejects([&] { (void)decode_network_response(invalid); });
    }
    auto ignored = sample; ignored[0] |= 0x30; ignored[1] |= 0x80; ignored[2] = 77; ignored[3] = 255;
    check(decode_network_response(ignored).bytes == 50000, "reserved/ignored security flags accepted");
    for (unsigned kind : {3U,24U,100U}) {
        auto invalid = sample; invalid[8] = std::uint8_t(kind);
        rejects([&] { (void)decode_network_response(invalid); });
    }
    auto trailing = sample; trailing.push_back(0); rejects([&] { (void)decode_network_response(trailing); });
}
struct Fixture {
    NetworkAutodetect net{1006};
    NetworkAutodetect::Time now{};
    std::uint64_t written = 0;
    Fixture() { net.start(now); }
    void tx(const Bytes& p, std::chrono::microseconds delta = 0us) { now += delta; written += p.size(); net.transmitted(p, written, now); }
    void payload(std::uint64_t n) { written += n; net.transmitted(Bytes{0}, written, now); }
    void rx(const Bytes& p, std::chrono::microseconds delta = 0us) { now += delta; net.receive(p, now); }
};
void measurement() {
    Fixture f;
    check(f.net.poll(f.now, false, true, true).empty(), "inactive session never initiates probes");
    check(f.net.poll(f.now, true, false, true).empty(), "congested transport does not initiate probes");
    auto requests = f.net.poll(f.now, true, true, true).before_data;
    check(requests.size() == 2 && type(requests[0]) == 1 && type(requests[1]) == 0x14, "RTT and passive bandwidth START");
    f.rx(response(seq(requests[0])), 10ms); check(f.net.metrics().rtt_samples == 0, "unsent request response ignored");
    f.tx(requests[0], 3s); f.rx(response(seq(requests[0])), 12500us);
    check(f.net.metrics().rtt_us == 12500 && f.net.metrics().last_queue_delay_us == 3010000, "RTT excludes TLS queue residence");
    check(f.net.metrics().minimum_rtt_us == 12500 && f.net.rtt_age_ms(f.now) == 0, "first RTT sample");
    f.rx(response(seq(requests[0]))); check(f.net.metrics().rtt_samples == 1 && f.net.metrics().ignored_responses == 2, "duplicate RTT ignored");
    f.tx(requests[1]); f.payload(50000); f.now += 250ms;
    auto stop = f.net.poll(f.now, false, false, false).after_data;
    check(stop.size() == 1 && type(stop[0]) == 0x429 && seq(stop[0]) == seq(requests[1]), "STOP progresses during congestion/reactivation");
    check(f.net.poll(f.now, true, false, false).empty(), "STOP queued exactly once");
    f.rx(response(seq(stop[0]), true, 250, 49000)); check(f.net.metrics().bandwidth_samples == 0, "response before STOP transmission ignored");
    f.tx(stop[0]); f.rx(response(seq(stop[0]), true, 250, 49000), 5ms);
    const auto m = f.net.metrics();
    check(m.peer_kbps == 1568 && m.peer_bytes == 49000 && m.measured_wire_bytes == 50024, "peer throughput checked against actual write budget");
    check(m.bandwidth_samples == 1 && f.net.bandwidth_age_ms(f.now + 2s) == 2000, "bandwidth samples have age");
    f.now += 2s; const auto next = f.net.poll(f.now, true, true, false).before_data;
    check(next.size() == 1 && type(next[0]) == 1, "idle desktop uses RTT but no bulk measurement");
    f.tx(next[0]); f.rx(response(seq(next[0])), 20500us);
    check(f.net.metrics().smoothed_rtt_us == 13500 && f.net.metrics().jitter_us == 2000, "bounded RTT EWMA and jitter");
    check(f.net.metrics().minimum_rtt_us == 12500, "32-sample baseline minimum");
    rejects([&] { f.net.poll(f.now - 1ms, true, true, false); });
}
void bad_samples() {
    for (const auto [ms, bytes] : {std::pair{0U, 2048U}, {1U, 0U}, {1U, 999999U}, {30001U, 2048U}, {4000U,2048U}}) {
        Fixture f; auto requests = f.net.poll(f.now, true, true, true).before_data; f.tx(requests[0]); f.tx(requests[1]); f.payload(10000);
        f.now += 250ms; auto stop = f.net.poll(f.now, true, false, false).after_data; f.tx(stop[0]);
        f.rx(response(seq(stop[0]), true, ms, bytes));
        check(!f.net.metrics().peer_kbps && f.net.metrics().invalid_measurements == 1, "bad peer sample does not fabricate throughput");
    }
    Fixture f; auto p = f.net.poll(f.now, true, true, false).before_data; f.tx(p[0]);
    auto malformed = response(seq(p[0])); malformed[5] = 0;
    rejects([&] { f.rx(malformed); });
    f.rx(response(seq(p[0])), 5ms); check(f.net.metrics().rtt_us == 5000, "malformed response did not consume pending request");
}
void timeout() {
    Fixture f;
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        auto p = f.net.poll(f.now, true, true, false).before_data; check(p.size() == 1, "timeout retry bounded");
        f.tx(p[0]); f.now += 5s;
        auto ignored = f.net.poll(f.now, true, true, false); check(ignored.empty(), "timeout applies backoff");
        f.rx(response(seq(p[0]))); check(f.net.metrics().rtt_samples == 0, "expired reply ignored");
        f.now += 60s;
    }
    check(f.net.metrics().rtt_disabled && f.net.metrics().timeouts == 3, "nonresponding client disables RTT only");
    check(f.net.poll(f.now, true, true, false).empty(), "no perpetual requests to nonresponding client");
    auto bandwidth = f.net.poll(f.now, true, true, true).before_data; check(bandwidth.size() == 1 && type(bandwidth[0]) == 0x14, "RTT failure does not disable unrelated measurement");
    f.tx(bandwidth[0]); f.now += 250ms; auto stop = f.net.poll(f.now, false, false, false).after_data; f.tx(stop[0]);
    f.now += 5s; (void)f.net.poll(f.now, false, false, false);
    check(f.net.metrics().timeouts == 4 && !f.net.metrics().bandwidth_disabled, "bandwidth expiry works when RDP is reactivating");
}
void windows_and_bounds() {
    Fixture f;
    for (unsigned i = 0; i < 33; ++i) {
        const auto p = f.net.poll(f.now, true, true, false).before_data;
        check(p.size() == 1, "one RTT per interval");
        f.tx(p[0]); f.rx(response(seq(p[0])), i == 0 ? 1us : 50000us); f.now += 2s;
    }
    check(f.net.metrics().minimum_rtt_us == 50000, "minimum RTT ages out of the fixed 32-sample window");
    Fixture large;
    const auto first = large.net.poll(large.now, true, true, true).before_data;
    large.tx(first[0]); large.rx(response(seq(first[0]))); large.tx(first[1]); large.payload(0x100000000ULL);
    large.now += 250ms;
    const auto stop = large.net.poll(large.now, true, false, false).after_data;
    large.tx(stop[0]); large.rx(response(seq(stop[0]), true, 1, 0xffffffffU));
    check(large.net.metrics().peer_kbps == 34359738360ULL, "throughput arithmetic does not narrow or multiply in 32 bits");
    Fixture missing;
    for (unsigned i = 0; i < 3; ++i) {
        const auto p = missing.net.poll(missing.now, true, true, true).before_data;
        check(p.size() == 2, "missing bandwidth replies do not stop RTT requests");
        missing.tx(p[0]); missing.rx(response(seq(p[0])), 1ms); missing.tx(p[1]);
        missing.now += 250ms;
        const auto end = missing.net.poll(missing.now, false, false, false).after_data;
        missing.tx(end[0]); missing.now += 5s;
        check(missing.net.poll(missing.now, false, false, false).empty(), "no retry storm during inactive phase");
        missing.now += 60s;
    }
    check(missing.net.metrics().bandwidth_disabled && !missing.net.metrics().rtt_disabled &&
          missing.net.metrics().timeouts == 3 && missing.net.metrics().rtt_samples == 3,
          "three bandwidth timeouts disable only bandwidth monitoring");
    NetworkAutodetect unstarted(1006);
    rejects([&]{ unstarted.receive(response(1), NetworkAutodetect::Time{}); });
    rejects([]{ NetworkAutodetect invalid(global_channel); });
    rejects([]{ NetworkPolicy p; p.response_timeout = 1ms; NetworkAutodetect invalid(1006,p); });
    rejects([]{ NetworkPolicy p; p.bandwidth_window = 2s; NetworkAutodetect invalid(1006,p); });
}
void exhaustion() {
    Fixture f;
    for (unsigned id = 1; id <= 65535; ++id) {
        auto p = f.net.poll(f.now, true, true, false).before_data; check(p.size() == 1 && seq(p[0]) == id, "unique probe identifiers");
        f.tx(p[0]); f.rx(response(std::uint16_t(id)), 1us); f.now += 2s;
    }
    check(f.net.poll(f.now, true, true, true).empty() && f.net.metrics().sequence_exhausted, "identifier exhaustion stops metrics without aliasing");
    f.rx(response(1)); check(f.net.metrics().rtt_samples == 65535, "old reply cannot alias after ID exhaustion");
}
void fuzz() {
    std::mt19937 rng(0x5244504e);
    for (unsigned i = 0; i < 20000; ++i) {
        Bytes input(rng() % 40); for (auto& b : input) b = std::uint8_t(rng());
        try { (void)decode_network_response(input); } catch (const ProtocolError&) {}
    }
    check(true, "malformed decode probes finished");
}
}
int main() {
    try { wire(); measurement(); bad_samples(); timeout(); windows_and_bounds(); exhaustion(); fuzz(); std::cout << checks << " checks; 20000 malformed decodes; deterministic egress-clock/timeout/ID coverage passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1; }
}
