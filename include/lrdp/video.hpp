#pragma once
#include "desktop.hpp"
#include "avc444.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace lrdp {
struct VideoOptions {
    std::string backend = "auto";
    std::string device = "/dev/dri/renderD128";
    unsigned fps = 30, qp = 22;
};
struct EncodedVideo {
    Bytes annex_b;
    std::uint32_t width = 0, height = 0;
    bool key_frame = false, hardware = false;
    std::string encoder;
    VideoCodec codec = VideoCodec::avc420;
    Bytes auxiliary; // Second picture in the SAME encoder context, only for AVC444/v2.
    unsigned qp = 22;
};
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    virtual EncodedVideo encode(const Frame&, bool force_key_frame) = 0;
};
using VideoFactory = std::function<std::unique_ptr<VideoEncoder>(unsigned, unsigned, VideoCodec)>;
VideoFactory ffmpeg_video_factory(VideoOptions options);

struct VideoCompletion {
    std::uint64_t generation = 0;
    std::optional<EncodedVideo> frame;
    std::string error;
};
// Single-slot ownership, not an unbounded FIFO. Driver work never runs on the
// TLS/input loop. A completed frame must be consumed before another is captured.
class VideoWorker {
    struct Job { Frame frame; std::uint64_t generation; bool key_frame; };
    VideoFactory factory_;
    VideoCodec codec_;
    std::mutex mutex_;
    std::condition_variable available_;
    bool stopping_ = false, busy_ = false;
    std::optional<Job> pending_;
    std::optional<VideoCompletion> completed_;
    std::thread thread_;
    void run();
public:
    explicit VideoWorker(VideoFactory factory, VideoCodec codec = VideoCodec::avc420);
    ~VideoWorker();
    VideoWorker(const VideoWorker&) = delete;
    VideoWorker& operator=(const VideoWorker&) = delete;
    bool available();
    void submit(Frame frame, std::uint64_t generation, bool key_frame);
    std::optional<VideoCompletion> take();
};
} // namespace lrdp
