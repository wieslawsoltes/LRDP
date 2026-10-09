#pragma once
#include "lrdp/drive/client.hpp"
#include <memory>
namespace lrdp {
class DriveMount final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    DriveMount(std::shared_ptr<drive::Bridge> bridge, const std::string& private_root);
    ~DriveMount();
    DriveMount(const DriveMount&) = delete;
    DriveMount& operator=(const DriveMount&) = delete;
    const std::string& path() const;
};
} // namespace lrdp
