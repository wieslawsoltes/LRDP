#include "lrdp/drive/client.hpp"
#include <atomic>
#include <cerrno>
#include <future>
#include <iostream>
using namespace lrdp;
using namespace lrdp::drive;
namespace {
void check(bool value, const char* text) { require(value, text); }
Request query() { Request request; request.device = {7, 1}; request.operation = Operation::query_information; request.handle = 3; return request; }
std::vector<Request> await(Bridge& bridge) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    for (;;) { auto value = bridge.take_requests(); if (!value.empty()) return value;
        check(Clock::now() < deadline, "worker submission timed out"); std::this_thread::yield(); }
}
void tests() {
    Limits limits; limits.outstanding = 2; limits.timeout = std::chrono::seconds(1);
    Bridge bridge(limits);
    bridge.publish({{{7, 1}, "HOME"}});
    check(bridge.devices().size() == 1, "native device snapshot");
    try { (void)bridge.call(query()); throw std::runtime_error("event loop blocked"); }
    catch (const IoError& error) { check(error.code().value() == EDEADLK, "event loop must never block on native I/O"); }
    auto one = std::async(std::launch::async, [&] { return bridge.call(query()); });
    const auto first = await(bridge);
    auto two = std::async(std::launch::async, [&] { return bridge.call(query()); });
    const auto second = await(bridge);
    auto crowded = std::async(std::launch::async, [&] {
        try { (void)bridge.call(query()); return 0; } catch (const IoError& e) { return e.code().value(); }
    });
    check(crowded.get() == EAGAIN, "bounded queue rejects overload even after dequeue");
    bridge.complete({second[0].ticket, success, 22, 0, {2}});
    bridge.complete({first[0].ticket, success, 11, 0, {1}});
    check(two.get().handle == 22 && one.get().handle == 11, "out of order completions correlate to callers");
    auto cancelled = std::async(std::launch::async, [&] {
        try { (void)bridge.call(query()); return 0; } catch (const IoError& e) { return e.code().value(); }
    });
    const auto pending = await(bridge); bridge.disconnect(ENOTCONN);
    check(cancelled.get() == ENOTCONN && bridge.devices().empty(), "disconnect wakes native worker and clears devices");
    bridge.complete({pending[0].ticket, success, 0, 0, {}}); // Harmless shutdown completion.
    Bridge timeout(limits);
    auto wait = std::async(std::launch::async, [&] {
        try { (void)timeout.call(query()); return 0; } catch (const IoError& e) { return e.code().value(); }
    });
    (void)await(timeout);
    check(wait.get() == ETIMEDOUT && timeout.stopped() == ETIMEDOUT, "timeout closes bridge rather than leaking remote open");
    Bridge validation;
    auto malformed = std::async(std::launch::async, [&] {
        Request request; request.path = "../outside";
        try { (void)validation.call(request); return 0; } catch (const IoError& e) { return e.code().value(); }
    });
    check(malformed.get() == EINVAL && validation.take_requests().empty(), "unsafe native path rejected before transmission");
    check(status_errno(0xc0000034) == ENOENT && status_errno(0xc0000035) == EEXIST && status_errno(denied) == EACCES && status_errno(0xdeadbeef) == EIO,
          "NTSTATUS errno conversion");
}
}
int main() { try { tests(); std::cout << "PASS: bounded worker bridge, event-loop affinity, out-of-order completion, disconnect cancellation, terminal timeouts, request validation\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
