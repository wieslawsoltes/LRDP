#include "lrdp/platform/mime_clipboard.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <unistd.h>

namespace lrdp {
namespace {
void validate_mime(const std::string& mime) {
    require(!mime.empty() && mime.size() <= 255, "invalid clipboard MIME name length");
    for (const unsigned char c : mime) require(c >= 32 && c < 127, "invalid clipboard MIME name");
}
// A portal supplies a pipe, not necessarily a socket. Do not change the process
// signal disposition and do not consume a SIGPIPE that was pending beforehand.
ssize_t pipe_write(int fd, const void* data, std::size_t count) {
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    const int error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (error) { errno = error; return -1; }
    const bool existed = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
    const auto result = ::write(fd, data, count); const int saved = errno;
    if (result < 0 && saved == EPIPE && !existed) {
        timespec zero{};
        while (sigtimedwait(&blocked, nullptr, &zero) < 0 && errno == EINTR) {}
    }
    (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr); errno = saved; return result;
}
}
MimeClipboard::MimeClipboard(MimeClipboardTransport& transport, MimeClipboardLimits limits)
    : transport_(transport), limits_(limits) {
    require(limits.bytes > 0 && limits.bytes <= 16 * 1024 * 1024 && limits.turn_bytes > 0 &&
        limits.turn_bytes <= limits.bytes && limits.transfers > 0 && limits.transfers <= 16 &&
        limits.timeout.count() > 0, "invalid native clipboard limits");
}
void MimeClipboard::supported(std::vector<std::string> formats) {
    require(formats.size() <= 16, "too many supported clipboard MIME types");
    std::set<std::string> names;
    for (const auto& mime : formats) { validate_mime(mime); require(names.insert(mime).second, "duplicate clipboard MIME type"); }
    supported_ = std::move(formats);
    // Configuring a feature after portal Start must not lose an already observed
    // selection. Restart from the remembered offer, never from a partial snapshot.
    if (observed_) owner_changed(offered_, own_);
}
void MimeClipboard::owner_changed(std::vector<std::string> formats, bool own) {
    require(formats.size() <= 256, "native clipboard format count exceeds quota");
    for (const auto& mime : formats) validate_mime(mime);
    offered_ = std::move(formats); own_ = own; observed_ = true;
    incoming_.reset(); reads_.clear(); pending_.clear(); ready_.reset(); received_ = 0;
    if (own) return;
    local_.clear();
    for (const auto& mime : supported_)
        if (std::find(offered_.begin(), offered_.end(), mime) != offered_.end()) reads_.push_back(mime);
    if (reads_.empty()) ready_.emplace(); // Clear stale data even for unsupported selections.
}
void MimeClipboard::set(MimeContent content) {
    require(content.size() <= 16, "native clipboard snapshot format quota");
    std::set<const Bytes*> storage;
    std::size_t bytes = 0;
    for (const auto& [mime, data] : content) {
        validate_mime(mime); require(bool(data), "null native clipboard format");
        if (storage.insert(data.get()).second) {
            require(data->size() <= limits_.bytes - bytes, "native clipboard snapshot exceeds byte quota"); bytes += data->size();
        }
    }
    std::vector<std::string> formats;
    for (const auto& [mime, unused] : content) { (void)unused; formats.push_back(mime); }
    local_ = std::move(content); owner_changed(formats, true);
    transport_.offer_mimes(formats);
}
void MimeClipboard::transfer(std::uint32_t serial, const std::string& mime) {
    require(serials_.size() < limits_.transfers && !serials_.contains(serial), "duplicate or excessive clipboard transfer serial");
    const auto found = local_.find(mime);
    serials_.insert(serial); requests_.push_back({serial, found == local_.end() ? MimeBytes{} : found->second});
}
void MimeClipboard::finish(std::uint32_t serial, bool success) {
    serials_.erase(serial); transport_.finish_mime(serial, success);
}
void MimeClipboard::begin_read() {
    if (incoming_ || reads_.empty()) return;
    auto mime = std::move(reads_.front()); reads_.pop_front();
    try {
        auto fd = transport_.read_mime(mime); require(bool(fd), "native clipboard returned no read descriptor"); fd.nonblocking();
        incoming_.emplace(Incoming{std::move(fd), std::move(mime), {}, Clock::now() + limits_.timeout});
    } catch (const ProtocolError&) { complete_read(); }
}
void MimeClipboard::complete_read() {
    incoming_.reset();
    if (reads_.empty()) { ready_ = std::move(pending_); pending_.clear(); }
}
void MimeClipboard::poll() {
    begin_read();
    if (incoming_) {
        std::size_t budget = limits_.turn_bytes;
        if (Clock::now() >= incoming_->deadline) complete_read();
        else while (incoming_ && budget) {
            auto& input = *incoming_; std::uint8_t buffer[16384];
            const auto n = ::read(input.fd.get(), buffer, std::min(budget, sizeof(buffer)));
            if (n > 0) {
                const auto count = std::size_t(n); budget -= count;
                if (count > limits_.bytes - received_ || count > limits_.bytes - received_ - input.bytes.size()) {
                    complete_read(); break;
                }
                input.bytes.insert(input.bytes.end(), buffer, buffer + n);
                // Absolute deadline bounds slow producers, not just idle producers.
            } else if (n == 0) {
                received_ += input.bytes.size();
                pending_.emplace(input.mime, std::make_shared<const Bytes>(std::move(input.bytes)));
                complete_read();
            } else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else complete_read();
        }
    }
    // One descriptor acquisition per turn. D-Bus calls and callback work remain
    // bounded even when a local application sends a burst of requests.
    if (!requests_.empty()) {
        auto request = std::move(requests_.front()); requests_.pop_front();
        bool accepted = false;
        if (request.bytes) try {
            auto fd = transport_.write_mime(request.serial); require(bool(fd), "native clipboard returned no write descriptor"); fd.nonblocking();
            outgoing_.emplace(request.serial, Outgoing{std::move(fd), request.bytes, 0, Clock::now() + limits_.timeout}); accepted = true;
        } catch (const ProtocolError&) {}
        if (!accepted) finish(request.serial, false);
    }
    // Divide the turn's quota across all writers; a blocked old reader never
    // starves a newer reader, and no writer can monopolize the transport loop.
    std::size_t budget = limits_.turn_bytes;
    const auto slice = outgoing_.empty() ? budget : std::max<std::size_t>(1, budget / outgoing_.size());
    for (auto it = outgoing_.begin(); it != outgoing_.end();) {
        auto& output = it->second; bool failed = Clock::now() >= output.deadline;
        if (!failed && budget && output.offset < output.bytes->size()) {
            const auto count = std::min({slice, budget, output.bytes->size() - output.offset});
            const auto n = pipe_write(output.fd.get(), output.bytes->data() + output.offset, count);
            if (n > 0) { output.offset += std::size_t(n); budget -= std::size_t(n); }
            else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) failed = true;
        }
        if (failed || output.offset == output.bytes->size()) {
            const auto serial = it->first; output.fd.reset(); it = outgoing_.erase(it); finish(serial, !failed);
        } else ++it;
    }
}
void MimeClipboard::clear() noexcept {
    incoming_.reset(); outgoing_.clear(); requests_.clear(); serials_.clear(); reads_.clear();
    ready_.reset(); local_.clear(); pending_.clear(); offered_.clear(); received_ = 0; observed_ = false;
}
std::optional<MimeContent> MimeClipboard::take() {
    auto result = std::move(ready_); ready_.reset(); return result;
}
} // namespace lrdp
