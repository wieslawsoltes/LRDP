#include "lrdp/drive/client.hpp"
#include <cerrno>
#include <limits>

namespace lrdp::drive {
int status_errno(std::uint32_t status) noexcept {
    switch (status) {
    case success: return 0;
    case denied: return EACCES;
    case invalid_handle: return EBADF;
    case removed: return ENODEV;
    case unsupported: case 0xc0000002: return EOPNOTSUPP;
    case 0xc000000f: case 0xc0000034: case 0xc000003a: return ENOENT;
    case 0xc0000035: return EEXIST;
    case 0xc0000043: return EBUSY;
    case 0xc0000101: return ENOTEMPTY;
    case 0xc0000103: return ENOTDIR;
    case 0xc00000ba: return EISDIR;
    case 0xc000007f: return ENOSPC;
    case 0xc00000a2: return EROFS;
    case 0xc000000d: case 0xc0000033: return EINVAL;
    case 0xc000009a: case 0xc0000017: return ENOMEM;
    case 0xc000011f: return EMFILE;
    case 0xc0000120: return ECANCELED;
    case 0xc0000061: return EPERM;
    default: return EIO;
    }
}
Bridge::Bridge(Limits limits) : limits_(limits) { (void)Protocol(limits); }
Bridge::~Bridge() { disconnect(ENOTCONN); }
void Bridge::owner() const {
    require(std::this_thread::get_id() == owner_, "drive bridge event-loop ownership violation");
}
void Bridge::stop_locked(int error) noexcept {
    if (stopped_) return;
    stopped_ = error ? error : ENOTCONN;
    devices_.clear(); queued_.clear();
    for (auto& [ticket, waiter] : waiters_) { (void)ticket; waiter->ready.notify_all(); }
}
void Bridge::disconnect(int error) noexcept { std::lock_guard lock(mutex_); stop_locked(error); }
int Bridge::stopped() const { std::lock_guard lock(mutex_); return stopped_; }
Reply Bridge::call(Request request) {
    if (std::this_thread::get_id() == owner_) throw IoError(EDEADLK, "native I/O on RDP event loop");
    if (request.data.size() > limits_.transfer || request.path.size() > 4096) throw IoError(E2BIG, "drive request quota");
    try { (void)request_body(request, limits_.transfer); }
    catch (const ProtocolError&) { throw IoError(EINVAL, "invalid native drive request"); }
    auto waiter = std::make_shared<Waiter>();
    const auto deadline = Clock::now() + limits_.timeout;
    std::unique_lock lock(mutex_);
    if (stopped_) throw IoError(stopped_, "drive disconnected");
    if (waiters_.size() >= limits_.outstanding) throw IoError(EAGAIN, "native drive queue full");
    if (next_ticket_ == std::numeric_limits<std::uint64_t>::max()) throw IoError(EOVERFLOW, "native drive tickets exhausted");
    request.ticket = next_ticket_++;
    const auto ticket = request.ticket;
    waiter->request = std::move(request);
    waiters_.emplace(ticket, waiter);
    try { queued_.push_back(ticket); }
    catch (...) { waiters_.erase(ticket); throw; }
    if (!waiter->ready.wait_until(lock, deadline, [&] { return stopped_ || waiter->reply.has_value(); }))
        stop_locked(ETIMEDOUT); // Close the channel/session; a remote open may have succeeded.
    waiters_.erase(ticket);
    if (stopped_) throw IoError(stopped_, "native drive I/O interrupted");
    return std::move(*waiter->reply);
}
std::vector<Device> Bridge::devices() const { std::lock_guard lock(mutex_); return devices_; }
void Bridge::publish(std::vector<Device> devices) {
    owner(); require(devices.size() <= limits_.devices, "drive device snapshot exceeds quota");
    std::lock_guard lock(mutex_); if (!stopped_) devices_ = std::move(devices);
}
std::vector<Request> Bridge::take_requests() {
    owner(); std::lock_guard lock(mutex_); std::vector<Request> requests;
    requests.reserve(queued_.size());
    while (!queued_.empty()) {
        const auto found = waiters_.find(queued_.front());
        if (found != waiters_.end()) requests.push_back(std::move(found->second->request));
        queued_.pop_front();
    }
    return requests;
}
void Bridge::complete(Reply reply) {
    owner(); require(reply.data.size() <= limits_.transfer, "native drive reply exceeds quota");
    std::lock_guard lock(mutex_); if (stopped_) return;
    const auto found = waiters_.find(reply.ticket);
    require(found != waiters_.end() && !found->second->reply, "uncorrelated native drive completion");
    found->second->reply = std::move(reply); found->second->ready.notify_one();
}
} // namespace lrdp::drive
