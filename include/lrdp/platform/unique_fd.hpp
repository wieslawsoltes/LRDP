#pragma once
#include "lrdp/wire.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace lrdp {
class UniqueFd final {
    int fd_ = -1;
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int next = -1) noexcept { const auto old = std::exchange(fd_, next); if (old >= 0) ::close(old); }
    void nonblocking() const {
        const auto flags = fcntl(fd_, F_GETFL);
        require(flags >= 0 && fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(fd_, F_SETFD, FD_CLOEXEC) == 0,
                "cannot configure native file descriptor");
    }
};
} // namespace lrdp
