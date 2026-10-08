#include "lrdp/platform/capture_frame.hpp"
#include <iostream>
#include <limits>
#include <random>
using namespace lrdp;
namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F action) { try { action(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected capture rejection"); }
}
int main() {
    try {
        Bytes bytes(40, 0xee);
        for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 3; ++x) {
            const auto p = y * 20 + x * 4; bytes[p] = std::uint8_t(x + 1); bytes[p + 1] = std::uint8_t(y + 10); bytes[p + 2] = 77; bytes[p + 3] = 0;
        }
        const auto frame = copy_capture_frame(bytes, 3, 2, 20, PackedFormat::rgba);
        check(frame.width == 3 && frame.height == 2 && frame.bgra[0] == 77 && frame.bgra[2] == 1 && frame.bgra[3] == 255, "RGBA swizzle and alpha");
        check(frame.bgra[12 + 1] == 11 && frame.bgra[12 + 2] == 1, "padded input stride");
        const auto crop = copy_capture_frame(bytes, 3, 2, 20, PackedFormat::bgrx, CaptureCrop{1, 1, 2, 1});
        check(crop.width == 2 && crop.height == 1 && crop.bgra[0] == 2 && crop.bgra[4] == 3, "privacy crop limits output");
        rejects([&] { (void)copy_capture_frame(bytes, 3, 2, -20, PackedFormat::bgra); });
        rejects([&] { (void)copy_capture_frame(View(bytes).first(31), 3, 2, 20, PackedFormat::bgra); });
        rejects([&] { (void)copy_capture_frame(bytes, 3, 2, 11, PackedFormat::bgra); });
        rejects([&] { (void)copy_capture_frame(bytes, 3, 2, 20, PackedFormat::bgra, CaptureCrop{2, 0, 2, 2}); });
        rejects([&] { (void)copy_capture_frame(bytes, 0xffffffff, 0xffffffff, std::numeric_limits<std::int32_t>::max(), PackedFormat::bgra); });
        check(rdp_evdev_key(0x1e, 0) == 30 && rdp_evdev_key(0x1d, 0x100) == 97 && rdp_evdev_key(0x48, 0x100) == 103, "native scan code mappings");
        check(rdp_evdev_key(0xffff, 0) == 0 && rdp_evdev_key(0x45, 0x200) == 119, "extended key safety");
        std::mt19937 random(0x50495045);
        for (unsigned i = 0; i < 10000; ++i) {
            try { (void)copy_capture_frame(bytes, random(), random(), std::int32_t(random()), PackedFormat::bgra); }
            catch (const ProtocolError&) {}
        }
        std::cout << "PASS: captured row bounds, channel order, alpha, crop privacy, key mappings and 10000 malformed capture layouts\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
