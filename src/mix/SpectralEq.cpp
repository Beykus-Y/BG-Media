#include "bgmedia/mix/SpectralEq.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::mix {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;

float onePoleCoefficient(double cutoffHz, std::uint32_t sampleRate) {
    if (sampleRate == 0 || cutoffHz <= 0.0) {
        return 0.0f;
    }
    const double clamped = std::min(cutoffHz,
                                    static_cast<double>(sampleRate) * 0.45);
    return static_cast<float>(std::exp(-kTwoPi * clamped /
                                      static_cast<double>(sampleRate)));
}

} // namespace

void ThreeBandEq::configure(std::uint32_t sampleRate, float lowCrossoverHz,
                            float highCrossoverHz) noexcept {
    lowHz_ = std::clamp(lowCrossoverHz, 40.0f, 800.0f);
    highHz_ = std::clamp(highCrossoverHz, lowHz_ * 1.5f, 12000.0f);
    lowCoefficient_ = onePoleCoefficient(lowHz_, sampleRate);
    highCoefficient_ = onePoleCoefficient(highHz_, sampleRate);
    reset();
}

void ThreeBandEq::reset() noexcept {
    lowState_[0] = lowState_[1] = 0.0f;
    midState_[0] = midState_[1] = 0.0f;
}

void ThreeBandEq::process(float* interleavedStereo, std::size_t frames,
                          EqGains start, EqGains end) noexcept {
    if (!interleavedStereo || frames == 0) {
        return;
    }
    if (start.low == 1.0f && start.mid == 1.0f && start.high == 1.0f &&
        end.low == 1.0f && end.mid == 1.0f && end.high == 1.0f) {
        // Unity is a mathematical bypass; skipping also keeps the filter
        // state frozen so starting automation later cannot click.
        return;
    }
    const float inverse = frames > 1 ? 1.0f / static_cast<float>(frames - 1)
                                     : 1.0f;
    for (std::size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) * inverse;
        const float lowGain = start.low + (end.low - start.low) * t;
        const float midGain = start.mid + (end.mid - start.mid) * t;
        const float highGain = start.high + (end.high - start.high) * t;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float input = interleavedStereo[i * 2 + channel];
            float& lowState = lowState_[channel];
            lowState = lowCoefficient_ * lowState +
                       (1.0f - lowCoefficient_) * input;
            const float rest = input - lowState;
            float& midState = midState_[channel];
            midState = highCoefficient_ * midState +
                       (1.0f - highCoefficient_) * rest;
            const float high = rest - midState;
            interleavedStereo[i * 2 + channel] =
                lowState * lowGain + midState * midGain + high * highGain;
        }
    }
}

} // namespace bgmedia::mix
