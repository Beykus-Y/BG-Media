#pragma once

// Independent three-band EQ for one BG Mix deck. It uses complementary
// one-pole splits: low + (mid + high) reconstructs the input exactly, so a
// unity gain curve is a true bypass with no crossover bump, no resonance and
// no level step when automation starts or ends. Coefficients are derived from
// the actual sample rate, and gain changes are interpolated per sample.

#include <cstddef>
#include <cstdint>

namespace bgmedia::mix {

struct EqGains {
    float low = 1.0f;
    float mid = 1.0f;
    float high = 1.0f;
};

class ThreeBandEq {
public:
    void configure(std::uint32_t sampleRate,
                   float lowCrossoverHz = 160.0f,
                   float highCrossoverHz = 3200.0f) noexcept;
    void reset() noexcept;

    // Interleaved stereo, in place. `start` is the gain at the first sample
    // and `end` at the last, so callers can automate across a callback block.
    void process(float* interleavedStereo, std::size_t frames, EqGains start,
                 EqGains end) noexcept;

    [[nodiscard]] float lowCutoff() const noexcept { return lowHz_; }
    [[nodiscard]] float highCutoff() const noexcept { return highHz_; }

private:
    float lowCoefficient_ = 0.98f;
    float highCoefficient_ = 0.7f;
    float lowState_[2] = {0.0f, 0.0f};
    float midState_[2] = {0.0f, 0.0f};
    float lowHz_ = 160.0f;
    float highHz_ = 3200.0f;
};

} // namespace bgmedia::mix
