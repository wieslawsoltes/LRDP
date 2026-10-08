#pragma once
#include "lrdp/platform/portal_session.hpp"
#include "lrdp/desktop.hpp"

namespace lrdp {
struct CaptureSnapshot {
    std::shared_ptr<const Frame> frame;
    std::uint64_t sequence = 0;
};
class PipeWireCapture final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PipeWireCapture(UniqueFd remote, const PortalStream& stream);
    ~PipeWireCapture();
    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;
    CaptureSnapshot latest() const;
};
} // namespace lrdp
