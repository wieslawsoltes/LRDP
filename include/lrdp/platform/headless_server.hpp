#pragma once
#include "lrdp/platform/child_process.hpp"
#include <memory>
namespace lrdp {
struct HeadlessOptions {
    std::string xorg = "/usr/lib/xorg/Xorg";
    std::vector<std::string> command{"/usr/bin/xterm"};
};
class HeadlessServer final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit HeadlessServer(const HeadlessOptions& options);
    ~HeadlessServer();
    HeadlessServer(const HeadlessServer&) = delete;
    HeadlessServer& operator=(const HeadlessServer&) = delete;
    const std::string& display() const;
    const std::string& authority() const;
    void start_desktop(const HeadlessOptions& options);
    void check() const;
};
}
