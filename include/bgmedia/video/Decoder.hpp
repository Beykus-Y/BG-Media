#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <variant>

namespace bgmedia::video {

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0;
    double duration = 0; // seconds
    int sourceSampleRate = 0;
    int sourceChannels = 0;
};

struct VideoFrame {
    double pts = 0; // seconds relative to container start
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct AudioFrame {
    static constexpr int sampleRate = 48000;
    static constexpr int channels = 2;
    double pts = 0;
    std::vector<float> samples; // interleaved stereo
};

struct EndOfStream {};
// monostate means the decoder made progress without producing a frame.
using Decoded = std::variant<std::monostate, VideoFrame, AudioFrame, EndOfStream>;

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual VideoInfo info() const = 0;
    virtual Decoded step() = 0;
    virtual void seek(double seconds) = 0;
    // Bytes in converted frames held inside this adapter. Codec and demuxer
    // working memory are measured separately via process RSS.
    virtual std::size_t bufferedBytes() const noexcept = 0;
};

}
