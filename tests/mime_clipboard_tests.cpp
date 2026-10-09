#include "lrdp/platform/mime_clipboard.hpp"
#include <algorithm>
#include <iostream>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

using namespace lrdp;
namespace {
void check(bool value, const char* text) { require(value, text); }
template<class F> void rejects(F&& action) {
    try { action(); } catch (const ProtocolError&) { return; }
    throw ProtocolError("expected MIME clipboard rejection");
}
UniqueFd duplicate(int fd) { return UniqueFd(fcntl(fd, F_DUPFD_CLOEXEC, 3)); }
UniqueFd memory(View bytes) {
    UniqueFd fd(memfd_create("lrdp-mime-test", MFD_CLOEXEC)); check(bool(fd), "memfd creation");
    check(write(fd.get(), bytes.data(), bytes.size()) == ssize_t(bytes.size()), "memfd contents");
    check(lseek(fd.get(), 0, SEEK_SET) == 0, "memfd rewind"); return fd;
}
MimeBytes sample(std::size_t n, unsigned seed = 0) {
    auto bytes = std::make_shared<Bytes>(n);
    for (std::size_t i = 0; i < n; ++i) (*bytes)[i] = std::uint8_t(i + seed);
    return bytes;
}
struct Transport final : MimeClipboardTransport {
    MimeContent source;
    std::map<std::uint32_t, UniqueFd> writes;
    std::map<std::uint32_t, bool> completed;
    std::vector<std::string> advertised;
    std::optional<UniqueFd> read_pipe, write_pipe;
    UniqueFd read_mime(const std::string& mime) override {
        if (read_pipe) { auto result = std::move(*read_pipe); read_pipe.reset(); return result; }
        return memory(*source.at(mime));
    }
    UniqueFd write_mime(std::uint32_t serial) override {
        if (write_pipe) { auto result = std::move(*write_pipe); write_pipe.reset(); return result; }
        auto fd = memory({}); auto result = duplicate(fd.get()); writes.emplace(serial, std::move(fd)); return result;
    }
    void finish_mime(std::uint32_t serial, bool ok) override { check(completed.emplace(serial, ok).second, "one completion per serial"); }
    void offer_mimes(const std::vector<std::string>& formats) override { advertised = formats; }
    Bytes written(std::uint32_t serial) {
        const auto fd = writes.at(serial).get(); const auto size = lseek(fd, 0, SEEK_END);
        check(size >= 0, "seek output"); Bytes bytes(static_cast<std::size_t>(size));
        check(pread(fd, bytes.data(), bytes.size(), 0) == size, "read output"); return bytes;
    }
};
template<class Predicate> void spin(MimeClipboard& clipboard, Predicate predicate) {
    for (unsigned i = 0; i < 10000; ++i) { clipboard.poll(); if (predicate()) return; }
    throw ProtocolError("MIME clipboard test did not complete");
}
void transfers() {
    Transport transport; MimeClipboard clipboard(transport);
    clipboard.supported({"text/html", "image/png"}); check(!clipboard.take(), "no invented initial selection");
    transport.source = {{"text/html", sample(180003)}, {"image/png", sample(200007, 83)}};
    clipboard.owner_changed({"image/png", "text/html", "application/unsupported"}, false);
    std::optional<MimeContent> received;
    spin(clipboard, [&] { received = clipboard.take(); return received.has_value(); });
    check(received->size() == 2 && *received->at("text/html") == *transport.source.at("text/html") &&
        *received->at("image/png") == *transport.source.at("image/png"), "binary multi-format snapshot");
    clipboard.set(transport.source); clipboard.transfer(1, "text/html"); clipboard.transfer(2, "image/png");
    // Changing the owner must preserve the immutable snapshots captured by pending requests.
    clipboard.set({{"text/html", sample(7)}});
    spin(clipboard, [&] { return transport.completed.size() == 2; });
    check(transport.completed.at(1) && transport.completed.at(2) && transport.written(1) == *transport.source.at("text/html") &&
        transport.written(2) == *transport.source.at("image/png"), "pinned outgoing snapshots");
    clipboard.transfer(3, "not/offered"); spin(clipboard, [&] { return transport.completed.contains(3); });
    check(!transport.completed.at(3), "unsupported MIME failure");
    clipboard.transfer(4, "text/html"); rejects([&] { clipboard.transfer(4, "text/html"); });
    spin(clipboard, [&] { return transport.completed.contains(4); });
    clipboard.owner_changed({}, false); check(clipboard.take()->empty(), "unsupported selection clears stale snapshot");
    clipboard.owner_changed({"text/html"}, true); check(!clipboard.take(), "ownership echo suppressed");
}
void stale_and_closed_pipes() {
    Transport transport; MimeClipboard clipboard(transport); clipboard.supported({"text/html"});
    int pipe[2]; check(pipe2(pipe, O_CLOEXEC | O_NONBLOCK) == 0, "input pipe");
    UniqueFd producer(pipe[1]); transport.read_pipe.emplace(pipe[0]);
    clipboard.owner_changed({"text/html"}, false); clipboard.poll(); check(!clipboard.take(), "partial read remains unpublished");
    transport.source = {{"text/html", sample(41)}};
    clipboard.owner_changed({"text/html"}, false);
    std::optional<MimeContent> received;
    spin(clipboard, [&] { received = clipboard.take(); return received.has_value(); });
    check(*received->at("text/html") == *transport.source.at("text/html"), "stale pipe cannot publish into new selection");
    clipboard.set(transport.source);
    check(pipe2(pipe, O_CLOEXEC | O_NONBLOCK) == 0, "output pipe");
    close(pipe[0]); transport.write_pipe.emplace(pipe[1]); clipboard.transfer(8, "text/html");
    spin(clipboard, [&] { return transport.completed.contains(8); });
    check(!transport.completed.at(8), "EPIPE returns failure without process SIGPIPE");
    clipboard.clear(); check(clipboard.in_flight() == 0, "clear drops pending descriptors");
}
void limits() {
    Transport transport; MimeClipboardLimits limits; limits.bytes = 1024; limits.turn_bytes = 64; limits.transfers = 2;
    MimeClipboard clipboard(transport, limits); clipboard.supported({"text/html", "image/png"});
    const auto blob = sample(900);
    clipboard.set({{"text/html", blob}, {"text/plain", blob}}); // Count aliases once.
    rejects([&] { clipboard.set({{"text/html", sample(1025)}}); });
    rejects([&] { clipboard.supported({"text/html", "text/html"}); });
    rejects([&] { clipboard.set({{"bad\nname", blob}}); });
    clipboard.transfer(1, "text/html"); clipboard.transfer(2, "text/plain");
    rejects([&] { clipboard.transfer(3, "text/html"); });
    spin(clipboard, [&] { return transport.completed.size() == 2; });
    transport.source = {{"text/html", sample(600)}, {"image/png", sample(600)}};
    clipboard.owner_changed({"text/html", "image/png"}, false); std::optional<MimeContent> content;
    spin(clipboard, [&] { content = clipboard.take(); return content.has_value(); });
    check(content->size() == 1 && content->contains("text/html"), "aggregate read quota is enforced");
    MimeClipboardLimits timeout; timeout.timeout = std::chrono::milliseconds(2);
    MimeClipboard slow(transport, timeout); slow.supported({"text/html"});
    int pipe[2]; check(pipe2(pipe, O_CLOEXEC | O_NONBLOCK) == 0, "deadline pipe");
    UniqueFd producer(pipe[1]); transport.read_pipe.emplace(pipe[0]); slow.owner_changed({"text/html"}, false); slow.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); slow.poll(); check(slow.take()->empty(), "absolute transfer deadline");
}
}
int main() {
    try { transfers(); stale_and_closed_pipes(); limits(); std::cout << "PASS: multi-format MIME snapshots, binary pipes, pinned generations, EPIPE, aliases, aggregate quotas and deadlines\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
