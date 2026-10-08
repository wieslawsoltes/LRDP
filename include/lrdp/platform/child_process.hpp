#pragma once
#include "lrdp/wire.hpp"
#include <map>
#include <sys/types.h>
namespace lrdp {
// A process group owned until destruction. Child setup uses no allocations or
// locks after fork, and closes every unrelated descriptor before exec.
class ChildProcess final {
    pid_t pid_ = -1;
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    void start(const std::vector<std::string>& argv, const std::map<std::string,std::string>& environment,
               const std::vector<std::string>& remove, int log_fd, int display_fd = -1);
    bool alive() const;
    void stop() noexcept;
    pid_t pid() const noexcept { return pid_; }
};
}
