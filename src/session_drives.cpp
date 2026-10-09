#include "lrdp/session.hpp"
#include <cerrno>

namespace lrdp {
void Session::configure_drives(std::shared_ptr<drive::Bridge> bridge) {
    require(phase_ == SessionPhase::connect && bridge && !drive_bridge_, "drive configuration must precede connection");
    drives_.emplace(bridge->limits()); drive_bridge_ = std::move(bridge);
}
void Session::start_drives() {
    if (!drive_bridge_) return;
    const auto channel = settings_.channels.find("rdpdr");
    if (channel == settings_.channels.end()) { drive_bridge_->disconnect(ENOTSUP); drives_.reset(); return; }
    drive_channel_ = channel->second; drives_->start();
    for (const auto& message : drives_->drain()) send_channel(*drive_channel_, message);
}
bool Session::receive_drives(std::uint16_t channel, View payload) {
    if (!drive_channel_ || channel != *drive_channel_) return false;
    auto [assembler, inserted] = assemblers_.try_emplace(channel, std::size_t(drives_->limits().transfer) + 1024*1024);
    (void)inserted;
    if (const auto message = assembler->second.accept(payload)) {
        drives_->receive(*message);
        drive_bridge_->publish(drives_->devices());
        for (auto& reply : drives_->take_replies()) drive_bridge_->complete(std::move(reply));
        for (const auto& output : drives_->drain()) send_channel(channel, output);
    }
    return true;
}
void Session::tick_drives() {
    if (!drive_channel_) return;
    require(!drive_bridge_->stopped(), "native drive endpoint stopped; closing to retire remote handles");
    if (!drives_->ready()) return;
    drives_->tick();
    for (auto& request : drive_bridge_->take_requests()) drive_requests_.push_back(std::move(request));
    require(drive_requests_.size() <= drives_->limits().outstanding, "native drive submission backlog exceeds policy");
    // Try every queued handle once; a busy file cannot starve unrelated files.
    const auto count = drive_requests_.size();
    for (std::size_t i = 0; i < count; ++i) {
        auto request = std::move(drive_requests_.front()); drive_requests_.pop_front();
        if (!drives_->submit(request)) drive_requests_.push_back(std::move(request));
        for (auto& reply : drives_->take_replies()) drive_bridge_->complete(std::move(reply));
    }
    for (const auto& output : drives_->drain()) send_channel(*drive_channel_, output);
}
} // namespace lrdp
