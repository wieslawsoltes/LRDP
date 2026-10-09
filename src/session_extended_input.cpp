#include "lrdp/session.hpp"
namespace lrdp {
namespace { constexpr std::uint32_t input_channel = 4; }
void Session::release_all_input() {
    if (extended_) extended_->cancel();
    // Try both classes of release even when a backend call fails. Closing a
    // revoked portal also releases the compositor's device ownership.
    std::exception_ptr failure;
    try { desktop_->cancel_extended_input(); } catch (...) { failure = std::current_exception(); }
    try { desktop_->release_input(); } catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure) std::rethrow_exception(failure);
}
void Session::suspend_extended() {
    if (!extended_) return;
    desktop_->cancel_extended_input();
    if (auto packet = extended_->suspend()) send_dynamic(input_channel, *packet);
}
void Session::synchronize_extended() {
    if (!extended_ || !extended_->ready()) return;
    if (!active() || suppressed_ || active_layout_.monitors != desktop_->layout().monitors) {
        if (!extended_->suspended()) suspend_extended();
    } else if (auto packet = extended_->resume()) send_dynamic(input_channel, *packet);
}
void Session::receive_extended_dynamic(const DvcEvent& event) {
    if (event.kind == DvcEventKind::ready) {
        if (desktop_->extended_capabilities().touches)
            send_channel(*dynamic_channel_, dynamic_.create(input_channel, "Microsoft::Windows::RDS::Input"));
        return;
    }
    if (event.id != input_channel) return;
    if (event.kind == DvcEventKind::opened) {
        extended_.emplace(desktop_->extended_capabilities());
        send_dynamic(input_channel, extended_->start());
    } else if (event.kind == DvcEventKind::closed || event.kind == DvcEventKind::rejected) {
        desktop_->cancel_extended_input(); extended_.reset();
    } else if (event.kind == DvcEventKind::data) {
        require(extended_.has_value(), "input on an uninitialized digitizer channel");
        synchronize_extended();
        try {
            auto result = extended_->receive(event.data, active_layout_.width, active_layout_.height);
            if (result.cancel) desktop_->cancel_extended_input();
            else if (!result.frames.empty()) desktop_->extended_input(result.frames);
            synchronize_extended(); // A late CS_READY may need immediate suspension.
        } catch (...) {
            extended_->cancel();
            try { desktop_->cancel_extended_input(); } catch (...) {}
            throw;
        }
    }
}
} // namespace lrdp
