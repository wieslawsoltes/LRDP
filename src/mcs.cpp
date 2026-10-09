#include "lrdp/mcs.hpp"
#include "lrdp/monitor_layout.hpp"
#include <algorithm>
#include <array>
#include <set>

namespace lrdp {
namespace {
void prefix(Reader& reader, std::initializer_list<std::uint8_t> expected) {
    const auto bytes = reader.take(expected.size());
    require(std::equal(bytes.begin(), bytes.end(), expected.begin()), "unsupported GCC PER profile");
}
void domain(Reader& reader) {
    Reader in(reader.tlv(0x30));
    for (int i = 0; i < 8; ++i) (void)read_ber_integer(in);
    in.end();
}
void capability(Writer& out, unsigned type, const Writer& body) {
    out.le16(type).le16(unsigned(body.size() + 4)).raw(body.bytes());
}
void block(Writer& out, unsigned type, const Writer& body) { capability(out, type, body); }
}
ClientSettings connect_initial(View payload, std::uint32_t selected_protocol) {
    Reader root(payload); Reader ci(root.tlv(0x7f65)); root.end();
    require(ci.tlv(4).size() <= 16 && ci.tlv(4).size() <= 16, "invalid MCS domain selector");
    require(ci.tlv(1).size() == 1, "invalid MCS upward flag");
    domain(ci); domain(ci); domain(ci);
    Reader gcc(ci.tlv(4)); ci.end();
    prefix(gcc, {0,5,0,0x14,0x7c,0,1});
    const auto gcc_size = gcc.per_length(); require(gcc_size == gcc.remaining(), "GCC request length mismatch");
    prefix(gcc, {0,8,0,0x10,0,1,0xc0,0,'D','u','c','a'});
    Reader blocks(gcc.take(gcc.per_length())); gcc.end();
    ClientSettings settings; bool core_seen = false, security_seen = false, network_seen = false;
    View monitor_data, monitor_attributes;
    std::set<unsigned> seen;
    while (!blocks.empty()) {
        const auto type = blocks.le16(), length = blocks.le16();
        require(length >= 4 && seen.insert(type).second, "duplicate or invalid GCC client block");
        Reader data(blocks.take(length - 4));
        if (type == 0xc001) {
            core_seen = true;
            require(data.remaining() >= 212, "TLS negotiation requires extended Client Core Data");
            require(data.le32() >= 0x00080004, "RDP 5 or newer required");
            settings.width = data.le16(); settings.height = data.le16();
            require(settings.width >= 200 && settings.width <= 16384 && settings.height >= 200 && settings.height <= 16384,
                    "initial desktop dimensions exceed policy");
            data.skip(4); settings.keyboard_layout = data.le32(); data.skip(112);
            const auto post_beta = data.le16(); data.skip(6); const auto high_color = data.le16();
            data.skip(2); settings.early_caps = data.le16(); data.skip(66);
            require(data.le32() == selected_protocol, "TLS protocol selection tampering");
            settings.depth = high_color;
            if (settings.depth != 15 && settings.depth != 16 && settings.depth != 24) {
                settings.depth = post_beta == 0xca02 ? 15 : post_beta == 0xca03 ? 16 : 24;
            }
        } else if (type == 0xc002) {
            require(data.remaining() == 8, "invalid Client Security Data"); security_seen = true;
            // Enhanced RDP Security uses TLS instead of RDP's legacy encryption methods.
        } else if (type == 0xc003) {
            network_seen = true; const auto count = data.le32();
            require(count <= 31 && data.remaining() == count * 12, "invalid static channel count");
            for (std::uint32_t i = 0; i < count; ++i) {
                auto name = data.take(8); const auto zero = std::find(name.begin(), name.end(), 0);
                require(zero != name.end() && zero != name.begin(), "unterminated or empty static channel name");
                std::string text(name.begin(), zero);
                for (unsigned char c : text) require(c >= 32 && c <= 126, "invalid static channel name");
                const auto id = std::uint16_t(global_channel + 1 + i);
                require(settings.channels.emplace(text, id).second, "duplicate static channel name");
                settings.channel_ids.push_back(id); data.skip(4);
            }
        } else if (type == 0xc006) {
            require(data.remaining() == 4 && data.le32() == 0, "invalid Client Message Channel Data");
            settings.message_channel_requested = true;
        } else if (type == 0xc005) monitor_data = data.take(data.remaining());
        else if (type == 0xc008) monitor_attributes = data.take(data.remaining());
        // Unknown optional GCC extensions are length-delimited and not advertised in response.
    }
    require(core_seen && security_seen, "required GCC client data missing");
    (void)network_seen;
    require(!seen.contains(0xc008) || seen.contains(0xc005), "monitor attributes without monitor definitions");
    if (seen.contains(0xc005)) {
        if (seen.contains(0xc008)) require(!monitor_attributes.empty(), "empty monitor attributes");
        settings.monitors = decode_initial_monitors(monitor_data, monitor_attributes);
        // Monitor topology defines the desktop dimensions when supplied.
        settings.width = std::uint16_t(settings.monitors->width); settings.height = std::uint16_t(settings.monitors->height);
    } else require(settings.width <= 8192 && settings.height <= 8192, "single monitor exceeds policy");
    require(std::uint64_t(settings.width) * settings.height <= 16 * 1024 * 1024, "initial framebuffer exceeds quota");
    return settings;
}
Bytes connect_response(const ClientSettings& settings, std::uint32_t requested_protocols) {
    Writer blocks;
    Writer core; core.le32(0x00080004).le32(requested_protocols); block(blocks, 0x0c01, core);
    Writer security; security.le32(0).le32(0); block(blocks, 0x0c02, security);
    Writer net; net.le16(global_channel).le16(unsigned(settings.channel_ids.size()));
    for (auto id : settings.channel_ids) net.le16(id);
    if (settings.channel_ids.size() & 1) net.le16(0);
    block(blocks, 0x0c03, net);
    if (settings.message_channel_requested || settings.message_channel) {
        require(settings.message_channel == 0 ||
                (settings.message_channel > global_channel &&
                 std::find(settings.channel_ids.begin(), settings.channel_ids.end(), settings.message_channel) == settings.channel_ids.end()),
                "message channel collides with a static or user channel");
        Writer message; message.le16(settings.message_channel); block(blocks, 0x0c04, message);
    }
    Writer response; response.raw({0x14,0x76,0x0a,1,1,0,1,0xc0,0,'M','c','D','n'}).per_length(blocks.size()).raw(blocks.bytes());
    Writer gcc; gcc.raw({0,5,0,0x14,0x7c,0,1});
    // T.124 connectPDU length is ignored by RDP clients; emit its actual length.
    gcc.per_length(response.size()).raw(response.bytes());
    Writer parameters;
    for (std::uint32_t value : {settings.message_channel ? 35U : 34U,3U,0U,1U,0U,1U,65528U,2U}) parameters.raw(ber_integer(value));
    Writer body; body.raw(ber_integer(0, 10)).raw(ber_integer(0)).tlv(0x30, parameters.bytes()).tlv(4, gcc.bytes());
    return ber(0x7f66, body.bytes());
}
Bytes mcs_data(std::uint16_t channel, View payload) {
    Writer out; out.u8(0x68).be16(server_user - 1001).be16(channel).u8(0x70).per_length(payload.size()).raw(payload);
    return x224_data(out.bytes());
}
Bytes share_control(std::uint16_t type, View payload, std::uint16_t source) {
    Writer out; out.le16(unsigned(payload.size() + 6)).le16(type | 0x10).le16(source).raw(payload);
    return std::move(out).finish();
}
Bytes share_data(std::uint8_t type, View payload, std::uint16_t source) {
    Writer out; out.le32(share_id).u8(0).u8(2).le16(unsigned(payload.size() + 18)).u8(type).u8(0).le16(0).raw(payload);
    return share_control(7, out.bytes(), source);
}
Bytes valid_client_license() {
    Writer out; out.le16(0x80).le16(0).u8(0xff).u8(3).le16(16).le32(7).le32(2).le16(4).le16(0);
    return std::move(out).finish();
}
} // namespace lrdp
