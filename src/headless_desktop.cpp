#include "lrdp/platform/headless_desktop.hpp"
#include "lrdp/platform/randr_outputs.hpp"
#include "lrdp/platform/x11_managed.hpp"
#include <cstdlib>
namespace lrdp {
namespace {
// The factory runs on the session thread before starting media workers. The
// parent's environment and physical desktop are unchanged (session is forked).
class AuthorityScope {
    std::optional<std::string> previous_;
public:
    explicit AuthorityScope(const std::string& value) {
        if (const auto* old = std::getenv("XAUTHORITY")) previous_ = old;
        require(setenv("XAUTHORITY",value.c_str(),1) == 0,"cannot select private Xauthority");
    }
    ~AuthorityScope() { if (previous_) setenv("XAUTHORITY",previous_->c_str(),1); else unsetenv("XAUTHORITY"); }
};
class HeadlessDesktop final : public Desktop {
    HeadlessServer server_;
    std::shared_ptr<RandrOutputs> outputs_ = std::make_shared<RandrOutputs>();
    std::unique_ptr<Desktop> native_;
public:
    explicit HeadlessDesktop(const HeadlessOptions& options) : server_(options) {
        AuthorityScope authority(server_.authority());
        native_ = make_managed_x11_desktop(server_.display(),[outputs=outputs_](auto* display,const Layout& layout) { return outputs->apply(display,layout); });
        server_.start_desktop(options);
    }
    void pump() override { server_.check(); native_->pump(); }
    Layout layout() const override { return native_->layout(); }
    bool resizable() const override { return true; }
    bool resize(const Layout& layout) override { server_.check(); return native_->resize(layout); }
    std::shared_ptr<const PointerShape> pointer_shape() override { return native_->pointer_shape(); }
    Frame capture() override { server_.check(); return native_->capture(); }
    void input(const InputEvent& event) override { native_->input(event); }
    bool enable_rich_clipboard() override { return native_->enable_rich_clipboard(); }
    void set_clipboard_rich(RichClipboard content) override { native_->set_clipboard_rich(std::move(content)); }
    std::optional<RichClipboard> poll_clipboard_rich() override { return native_->poll_clipboard_rich(); }
    bool enable_file_clipboard() override { return native_->enable_file_clipboard(); }
    void set_clipboard_files(std::vector<std::string> paths) override { native_->set_clipboard_files(std::move(paths)); }
    std::optional<std::vector<std::string>> poll_clipboard_files() override { return native_->poll_clipboard_files(); }
    void set_clipboard(std::string text) override { native_->set_clipboard(std::move(text)); }
    std::optional<std::string> poll_clipboard() override { return native_->poll_clipboard(); }
    void release_input() override { native_->release_input(); }
};
}
std::unique_ptr<Desktop> make_headless_desktop(const HeadlessOptions& options) { return std::make_unique<HeadlessDesktop>(options); }
}
