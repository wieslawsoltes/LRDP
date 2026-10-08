#pragma once
#include "file_store.hpp"
#include <chrono>
#include <map>

namespace lrdp {
// MS-RDPECLIP 2.2.4, 2.2.5.2.3, 2.2.5.3/4. All methods run on the session
// loop. File callbacks are bounded synchronous I/O, not a zero-copy pipeline.
class ClipboardFiles {
    using Clock = std::chrono::steady_clock;
    std::shared_ptr<ClipboardFileStore> store_;
    FileClipboardLimits limits_;
    std::shared_ptr<const ClipboardFileSource> published_;
    std::map<std::uint32_t, std::shared_ptr<const ClipboardFileSource>> locked_;
    bool peer_stream_ = false, peer_lock_ = false, awaiting_list_ = false, receiving_ = false;
    std::uint32_t next_lock_ = 0, next_stream_ = 0;
    std::optional<std::uint32_t> incoming_lock_;
    Clock::time_point deadline_{};
    std::vector<ClipboardFile> incoming_;
    std::unique_ptr<ClipboardFileSink> sink_;
    struct Read { std::size_t index; std::uint64_t offset; std::uint32_t count; bool size; };
    std::map<std::uint32_t, Read> pending_;
    std::vector<std::uint64_t> scheduled_;
    std::vector<bool> queried_;
    std::vector<Bytes> outbound_;
    std::optional<std::vector<std::string>> completed_;
    std::optional<std::string> error_;
    void request(Read read);
    void advance();
    void unlock();
    void fail(const std::string& message);
    void response(std::uint16_t flags, View payload);
    void serve(View payload);
public:
    explicit ClipboardFiles(std::shared_ptr<ClipboardFileStore> store, FileClipboardLimits limits = {});
    void negotiate(std::uint32_t flags);
    bool supported() const { return peer_stream_; }
    bool awaiting_list() const { return awaiting_list_; }
    std::shared_ptr<const ClipboardFileSource> offer(const std::vector<std::string>& paths);
    void publish(std::shared_ptr<const ClipboardFileSource> source) { published_ = std::move(source); }
    Bytes file_list() const;
    void begin_remote();
    void list(View payload);
    void cancel();
    void accept(std::uint16_t type, std::uint16_t flags, View payload);
    void tick(Clock::time_point now = Clock::now());
    std::vector<Bytes> drain();
    std::optional<std::vector<std::string>> take_completed();
    std::optional<std::string> take_error();
    std::size_t pending() const { return pending_.size(); }
};
} // namespace lrdp
