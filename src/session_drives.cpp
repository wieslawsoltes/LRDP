#include "lrdp/session.hpp"
#include <algorithm>
#include <cerrno>

namespace lrdp {
void Session::configure_drives(std::shared_ptr<drive::Bridge> bridge) {
    require(phase_ == SessionPhase::connect && bridge && !drive_bridge_, "drive configuration must precede connection");
    drive_bridge_ = std::move(bridge);
}
void Session::configure_printers(std::unique_ptr<printing::Endpoint> endpoint) {
    require(phase_ == SessionPhase::connect && endpoint && !printers_, "printer configuration must precede connection");
    printers_ = std::move(endpoint);
}
void Session::start_drives() {
    if (!drive_bridge_ && !printers_) return;
    const auto channel = settings_.channels.find("rdpdr");
    if (channel == settings_.channels.end()) {
        if (drive_bridge_) drive_bridge_->disconnect(ENOTSUP);
        if (printers_) printers_->disconnect();
        return;
    }
    auto limits = drive_bridge_ ? drive_bridge_->limits() : drive::Limits{};
    limits.files = bool(drive_bridge_); limits.printers = bool(printers_);
    drives_.emplace(limits); drive_channel_ = channel->second; drives_->start();
    for (const auto& message : drives_->drain()) send_channel(*drive_channel_, message);
}
void Session::complete_device_replies() {
    for (auto& reply : drives_->take_replies()) {
        if (reply.purpose == drive::Purpose::printer) {
            require(printers_ != nullptr, "printer reply without configured endpoint");
            printers_->complete(reply);
        } else {
            require(drive_bridge_ != nullptr, "filesystem reply without configured bridge");
            drive_bridge_->complete(std::move(reply));
        }
    }
}
bool Session::receive_drives(std::uint16_t channel, View payload) {
    if (!drive_channel_ || channel != *drive_channel_) return false;
    auto [assembler, inserted] = assemblers_.try_emplace(channel, std::size_t(drives_->limits().transfer) + 1024*1024);
    (void)inserted;
    if (const auto message = assembler->second.accept(payload)) {
        drives_->receive(*message);
        auto devices = drives_->devices();
        if (printers_) printers_->publish(devices);
        if (drive_bridge_) {
            std::erase_if(devices, [](const auto& d) { return d.printer.has_value(); });
            drive_bridge_->publish(std::move(devices));
        }
        complete_device_replies();
        for (const auto& output : drives_->drain()) send_channel(channel, output);
    }
    return true;
}
void Session::tick_drives() {
    if (printers_) printers_->pump();
    if (!drive_channel_) return;
    require(!drive_bridge_ || !drive_bridge_->stopped(), "native drive endpoint stopped; closing to retire remote handles");
    if (!drives_->ready()) return;
    drives_->tick();
    if (drive_bridge_) for (auto& request : drive_bridge_->take_requests()) drive_requests_.push_back(std::move(request));
    if (printers_) for (auto& request : printers_->take_requests(std::min<std::uint32_t>(drives_->limits().transfer,65536))) drive_requests_.push_back(std::move(request));
    require(drive_requests_.size() <= drives_->limits().outstanding + printing::Jobs::job_limit, "native device backlog exceeds policy");
    // Each domain has its own trusted tickets, sharing bounded wire completion IDs.
    // Try every queued handle once; a busy file or printer cannot starve others.
    const auto count = drive_requests_.size();
    for (std::size_t i = 0; i < count; ++i) {
        auto request = std::move(drive_requests_.front()); drive_requests_.pop_front();
        if (!drives_->submit(request)) drive_requests_.push_back(std::move(request));
        complete_device_replies();
    }
    for (const auto& output : drives_->drain()) send_channel(*drive_channel_, output);
}
} // namespace lrdp
