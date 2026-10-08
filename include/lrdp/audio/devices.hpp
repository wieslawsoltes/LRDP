#pragma once
#include "format.hpp"
#include <memory>
#include <optional>

namespace lrdp {
struct AudioOptions { bool playback = false, microphone = false; };
class AudioDevices {
public:
    virtual ~AudioDevices() = default;
    virtual bool playback_available() const = 0;
    virtual bool microphone_available() const = 0;
    virtual void check() const = 0;
    virtual void enable_playback(bool enabled) = 0;
    virtual void enable_microphone(bool enabled) = 0;
    virtual std::optional<Bytes> take_playback(unsigned frames) = 0;
    virtual void feed_microphone(View pcm) = 0;
    virtual std::string description() const = 0;
};
std::unique_ptr<AudioDevices> make_pipewire_audio(AudioOptions options);
} // namespace lrdp
