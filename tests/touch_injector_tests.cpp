#include "lrdp/input/touch_injector.hpp"
#include <cmath>
#include <iostream>
using namespace lrdp;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F f) { try { f(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected rejection"); }
ExtendedFrame frame(unsigned id, unsigned flags, int x = 100, int y = 80) {
    ExtendedContact c; c.id = std::uint8_t(id); c.flags = flags; c.x = x; c.y = y;
    return {Digitizer::touch, 0, 0, {c}};
}
}
int main() {
    try {
        std::vector<TouchOperation> output;
        TouchInjector sink([&](auto events) { output.insert(output.end(), events.begin(), events.end()); }, 2);
        auto apply = [&](std::vector<ExtendedFrame> batch) { sink.apply(batch, 400, 200, 200, 100); };
        apply({frame(250,25),frame(17,25)});
        check(output.size() == 2 && output[0].slot == 0 && output[1].slot == 1 && output[0].x == 50 && output[0].y == 40,
              "sparse IDs map to bounded native slots and logical coordinates");
        rejects([&] { apply({frame(1,25)}); }); check(sink.active() == 2 && output.size() == 2,"slot exhaustion is atomic");
        output.clear(); apply({frame(250,26,200,100),frame(250,4,200,100),frame(1,25)});
        check(output.size() == 3 && output[0].action == TouchAction::motion && output[0].x == 100 &&
              output[1].action == TouchAction::up && output[2].slot == 0,"motion release and slot reuse");
        output.clear(); rejects([&] { apply({frame(17,26),frame(1,26,400,0)}); });
        check(output.empty() && sink.active() == 2,"malformed suffix never injects prefix");
        sink.cancel(); check(output.size() == 2 && sink.active() == 0,"all touch slots released");
        sink.cancel(); check(output.size() == 2,"idempotent release");
        output.clear(); apply({frame(1,10),frame(1,2)}); check(output.empty(),"touch hover does not simulate contact");
        bool fail = true; std::vector<TouchOperation> cleanup;
        TouchInjector broken([&](auto events) {
            if (fail) { fail = false; throw ProtocolError("native sink rejected batch"); }
            cleanup.assign(events.begin(),events.end());
        });
        auto down = frame(255,25); rejects([&] { broken.apply(std::span(&down,1),400,200,200,100); });
        check(broken.active() == 0 && cleanup.size() == 1 && cleanup[0].action == TouchAction::up,
              "partial native failure releases newly allocated slots");
        down.kind = Digitizer::pen; rejects([&] { broken.apply(std::span(&down,1),400,200,200,100); });
        std::cout << "PASS: sparse touch IDs, coordinate scaling, transactional batches, bounded slots, hover and failed-native-call cleanup\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
