#pragma once
#include "display.hpp"
#include <map>

namespace lrdp {
inline constexpr std::uint16_t client_user = 1001, server_user = 1002, global_channel = 1003;
inline constexpr std::uint32_t share_id = 0x000103ea;
struct ClientSettings {
    std::uint16_t width = 1280, height = 720, depth = 24;
    std::uint32_t keyboard_layout = 0;
    std::uint16_t early_caps = 0;
    std::optional<Layout> monitors;
    std::map<std::string, std::uint16_t> channels;
    std::vector<std::uint16_t> channel_ids;
};
ClientSettings connect_initial(View payload, std::uint32_t selected_protocol);
Bytes connect_response(const ClientSettings& settings, std::uint32_t requested_protocols);
Bytes mcs_data(std::uint16_t channel, View payload);
Bytes share_control(std::uint16_t type, View payload, std::uint16_t source = server_user);
Bytes share_data(std::uint8_t type, View payload, std::uint16_t source = server_user);
Bytes valid_client_license();
Bytes demand_active(std::uint16_t width, std::uint16_t height, std::uint16_t depth, bool resize, bool unicode_input = true);
} // namespace lrdp
