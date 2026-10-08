#pragma once
#include "lrdp/display.hpp"
#include <memory>
struct _XDisplay;
namespace lrdp {
// Controls exclusively DUMMY outputs; never reconfigures a physical display.
class RandrOutputs final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    RandrOutputs();
    ~RandrOutputs();
    RandrOutputs(const RandrOutputs&) = delete;
    RandrOutputs& operator=(const RandrOutputs&) = delete;
    // Rolls back failures; throws if rollback cannot restore a coherent desktop.
    bool apply(_XDisplay* display, const Layout& layout);
};
}
