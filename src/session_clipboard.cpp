#include "lrdp/session.hpp"
namespace lrdp {
void Session::configure_file_clipboard(std::shared_ptr<ClipboardFileStore> store, FileClipboardLimits limits) {
    require(phase_ == SessionPhase::connect && desktop_->enable_file_clipboard(), "file clipboard unavailable or configured after connection");
    clipboard_.configure_files(std::move(store), limits);
}
void Session::apply_clipboard(ClipboardResult result) {
    require(clipboard_channel_.has_value(), "clipboard channel unavailable");
    for (const auto& pdu : result.outbound) send_channel(*clipboard_channel_, pdu);
    if (result.remote_text) desktop_->set_clipboard(std::move(*result.remote_text));
    if (result.remote_files) {
        desktop_->set_clipboard_files(std::move(*result.remote_files)); clipboard_status_ = "File transfer completed";
    }
    if (result.file_error) clipboard_status_ = *result.file_error;
}
void Session::tick_clipboard() {
    if (!clipboard_channel_) return;
    if (auto paths = desktop_->poll_clipboard_files(); clipboard_.files_enabled() && paths) {
        try {
            for (const auto& pdu : clipboard_.set_local_files(*paths)) send_channel(*clipboard_channel_, pdu);
            clipboard_status_ = "Local file offer published";
        } catch (const ProtocolError& error) {
            // Invalid/out-of-root native selections clear the remote offer instead
            // of accidentally leaving the previous file set available for paste.
            clipboard_status_ = error.what();
            for (const auto& pdu : clipboard_.set_local("")) send_channel(*clipboard_channel_, pdu);
        }
    } else if (auto text = desktop_->poll_clipboard()) {
        for (const auto& pdu : clipboard_.set_local(std::move(*text))) send_channel(*clipboard_channel_, pdu);
    }
    apply_clipboard(clipboard_.tick()); // Deadlines continue with graphics suppressed.
}
}
