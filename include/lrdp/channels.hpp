#pragma once
#include "wire.hpp"
#include <functional>
#include <map>
#include <optional>

namespace lrdp {
class ChannelAssembler {
    std::size_t limit_;
    std::optional<std::uint32_t> expected_;
    Bytes buffer_;
public:
    explicit ChannelAssembler(std::size_t limit = 1024 * 1024) : limit_(limit) {}
    std::optional<Bytes> accept(View pdu);
    void reset() { expected_.reset(); buffer_.clear(); }
};
std::vector<Bytes> channel_fragments(View message, std::size_t chunk_size = 1600);

enum class DvcEventKind { ready, opened, rejected, data, closed };
struct DvcEvent { DvcEventKind kind; std::uint32_t id = 0; Bytes data; };

class DynamicChannels {
    struct Channel { bool open = false; std::optional<std::uint32_t> expected; Bytes partial; };
    std::map<std::uint32_t, Channel> channels_;
    std::size_t buffered_ = 0;
    std::size_t limit_;
    std::uint16_t version_ = 0;
public:
    explicit DynamicChannels(std::size_t limit = 16 * 1024 * 1024) : limit_(limit) {}
    static Bytes capabilities();
    Bytes create(std::uint32_t id, std::string_view listener, unsigned priority = 0);
    std::vector<DvcEvent> accept(View pdu);
    std::vector<Bytes> send(std::uint32_t id, View message) const;
    Bytes close(std::uint32_t id);
    [[nodiscard]] bool ready() const { return version_ != 0; }
    [[nodiscard]] bool is_open(std::uint32_t id) const;
    [[nodiscard]] std::size_t buffered() const { return buffered_; }
};
} // namespace lrdp
