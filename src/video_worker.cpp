#include "lrdp/video.hpp"

namespace lrdp {
VideoWorker::VideoWorker(VideoFactory factory, VideoCodec codec) : factory_(std::move(factory)), codec_(codec) {
    require(bool(factory_), "video worker requires an encoder factory");
    thread_ = std::thread([this] { run(); });
}
VideoWorker::~VideoWorker() {
    { std::lock_guard lock(mutex_); stopping_ = true; pending_.reset(); }
    available_.notify_one(); if (thread_.joinable()) thread_.join();
}
bool VideoWorker::available() {
    std::lock_guard lock(mutex_); return !stopping_ && !busy_ && !pending_ && !completed_;
}
void VideoWorker::submit(Frame frame, std::uint64_t generation, bool key_frame) {
    frame.validate();
    { std::lock_guard lock(mutex_);
      require(!stopping_ && !busy_ && !pending_ && !completed_, "video worker slot is occupied");
      pending_.emplace(Job{std::move(frame), generation, key_frame}); }
    available_.notify_one();
}
std::optional<VideoCompletion> VideoWorker::take() {
    std::lock_guard lock(mutex_); auto result = std::move(completed_); completed_.reset(); return result;
}
void VideoWorker::run() {
    std::unique_ptr<VideoEncoder> encoder;
    unsigned width = 0, height = 0;
    for (;;) {
        std::optional<Job> job;
        { std::unique_lock lock(mutex_); available_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
          if (stopping_) return;
          job = std::move(pending_); pending_.reset(); busy_ = true; }
        VideoCompletion result; result.generation = job->generation;
        try {
            if (!encoder || job->key_frame || width != job->frame.width || height != job->frame.height) {
                // A refresh may follow a discarded generation. Recreate the codec so
                // its next primary starts a new reference sequence rather than an I
                // picture that could retain references the client never received.
                encoder = factory_(job->frame.width, job->frame.height, codec_);
                require(encoder != nullptr, "encoder factory returned no encoder");
                width = job->frame.width; height = job->frame.height; job->key_frame = true;
            }
            result.frame = encoder->encode(job->frame, job->key_frame);
            require(result.frame->codec == codec_ && result.frame->width == width && result.frame->height == height,
                    "encoder completion does not match its negotiated configuration");
        } catch (const std::exception& error) { result.error = error.what(); encoder.reset(); }
          catch (...) { result.error = "unknown video driver failure"; encoder.reset(); }
        { std::lock_guard lock(mutex_); busy_ = false; if (stopping_) return; completed_ = std::move(result); }
    }
}
} // namespace lrdp
