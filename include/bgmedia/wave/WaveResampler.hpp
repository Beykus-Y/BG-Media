#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bgmedia::wave {

// Stateful streaming interleaved-stereo resampler (Catmull-Rom cubic
// interpolation) used off the audio thread when the host sample rate differs
// from 48 kHz. Zero-overhead copy when inputRate == outputRate.
class WaveStereoResampler {
public:
    void configure(std::uint32_t inputRate, std::uint32_t outputRate);
    void reset();

    [[nodiscard]] std::uint32_t inputRate() const noexcept { return inputRate_; }
    [[nodiscard]] std::uint32_t outputRate() const noexcept { return outputRate_; }
    [[nodiscard]] bool isBypass() const noexcept {
        return inputRate_ == outputRate_;
    }

    // Processes `inputInterleaved` (2 * inputFrames floats) and appends
    // resampled interleaved stereo frames to `outputInterleaved`.
    void process(std::span<const float> inputInterleaved,
                 std::vector<float>& outputInterleaved);

private:
    std::uint32_t inputRate_ = 48000;
    std::uint32_t outputRate_ = 48000;
    double phase_ = 0.0;
    std::vector<float> history_; // interleaved stereo carry-over frames
};

} // namespace bgmedia::wave
