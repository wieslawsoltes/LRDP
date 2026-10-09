#pragma once
#include "lrdp/platform/unique_fd.hpp"
#include "lrdp/clipboard/rich_content.hpp"
#include <memory>
#include <optional>

namespace lrdp {
struct PortalStream {
    std::uint32_t node = 0;
    std::optional<std::uint64_t> serial;
    unsigned logical_width = 0, logical_height = 0;
    bool embedded_cursor = false;
};
// One consent-scoped remote desktop session. All methods run on the creating
// thread. No arbitrary global capture, input device, or clipboard access is used.
class PortalSession final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PortalSession();
    ~PortalSession();
    PortalSession(const PortalSession&) = delete;
    PortalSession& operator=(const PortalSession&) = delete;
    const PortalStream& stream() const;
    const std::string& path() const;
    bool clipboard_available() const;
    UniqueFd open_pipewire();
    void poll();
    void flush();
    void pointer(double x, double y);
    void button(unsigned evdev_button, bool down);
    void key(unsigned evdev_key, bool down);
    void keysym(std::uint32_t symbol, bool down);
    void wheel(bool horizontal, int steps);
    bool enable_rich_clipboard();
    bool enable_file_clipboard();
    void set_clipboard_rich(RichClipboard content);
    std::optional<RichClipboard> take_clipboard_rich();
    void set_clipboard_files(std::vector<std::string> paths);
    std::optional<std::vector<std::string>> take_clipboard_files();
    void set_clipboard(std::string text);
    std::optional<std::string> take_clipboard();
};
} // namespace lrdp
