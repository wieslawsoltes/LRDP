#include "lrdp/input/touch_injector.hpp"
#include <algorithm>
#include <bitset>

namespace lrdp {
TouchInjector::TouchInjector(std::function<void(std::span<const TouchOperation>)> submit, unsigned capacity)
    : submit_(std::move(submit)), capacity_(capacity) {
    require(bool(submit_) && capacity > 0 && capacity <= 256, "invalid native touch sink");
    slots_.fill(-1);
}
unsigned TouchInjector::active() const {
    return unsigned(std::count_if(slots_.begin(), slots_.end(), [](int slot) { return slot >= 0; }));
}
void TouchInjector::cancel() {
    std::vector<TouchOperation> releases;
    for (const int slot : slots_) if (slot >= 0) releases.push_back({TouchAction::up, unsigned(slot)});
    slots_.fill(-1); // Local ownership is retired even when native teardown fails.
    if (!releases.empty()) submit_(releases);
}
void TouchInjector::apply(std::span<const ExtendedFrame> frames, unsigned width, unsigned height,
                          unsigned logical_width, unsigned logical_height) {
    require(width && height && logical_width && logical_height && width <= 32768 && height <= 32768 &&
            logical_width <= 32768 && logical_height <= 32768, "invalid native touch coordinate space");
    require(frames.size() <= 64, "too many native touch frames");
    auto next = slots_;
    std::bitset<256> occupied, possibly_live;
    for (const int slot : next) if (slot >= 0) occupied.set(unsigned(slot));
    possibly_live = occupied;
    std::vector<TouchOperation> operations;
    unsigned count = 0;
    for (const auto& frame : frames) {
        require(frame.kind == Digitizer::touch, "native backend does not provide pen injection");
        require(frame.contacts.size() <= 1024 - count, "native touch batch exceeds policy");
        count += unsigned(frame.contacts.size());
        std::bitset<256> seen;
        for (const auto& contact : frame.contacts) {
            require(!seen.test(contact.id), "duplicate native touch ID"); seen.set(contact.id);
            require(contact.x >= 0 && contact.y >= 0 && unsigned(contact.x) < width && unsigned(contact.y) < height,
                    "native touch outside captured coordinate space");
            const double x = double(contact.x) * logical_width / width;
            const double y = double(contact.y) * logical_height / height;
            int& slot = next[contact.id];
            if (contact.flags == 25) {
                require(slot < 0, "duplicate native touch down");
                unsigned candidate = 0;
                while (candidate < capacity_ && occupied.test(candidate)) ++candidate;
                require(candidate < capacity_, "native touch slots exhausted");
                slot = int(candidate); occupied.set(candidate); possibly_live.set(candidate);
                operations.push_back({TouchAction::down, candidate, x, y});
            } else if (contact.flags == 26) {
                require(slot >= 0, "native touch motion without down");
                operations.push_back({TouchAction::motion, unsigned(slot), x, y});
            } else if (contact.flags == 4 || contact.flags == 12 || contact.flags == 36) {
                require(slot >= 0, "native touch up without down");
                operations.push_back({TouchAction::up, unsigned(slot)});
                occupied.reset(unsigned(slot)); slot = -1;
            } else {
                require((contact.flags == 10 || contact.flags == 2 || contact.flags == 34) && slot < 0,
                        "invalid native touch hover state");
                // Touch-only native APIs have no proximity/hover operation.
            }
        }
    }
    try {
        if (!operations.empty()) submit_(operations);
        slots_ = next;
    } catch (...) {
        slots_.fill(-1);
        std::vector<TouchOperation> releases;
        for (unsigned slot = 0; slot < capacity_; ++slot)
            if (possibly_live.test(slot)) releases.push_back({TouchAction::up, slot});
        try { if (!releases.empty()) submit_(releases); } catch (...) {}
        throw;
    }
}
} // namespace lrdp
