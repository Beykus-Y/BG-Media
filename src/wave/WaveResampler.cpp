#include "bgmedia/wave/WaveResampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace bgmedia::wave {
namespace {

float catmullRom(float y0, float y1, float y2, float y3, float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float a2 = -0.5f * y0 + 0.5f * y2;
    const float a3 = y1;
    return a0 * t3 + a1 * t2 + a2 * t + a3;
}

} // namespace

void WaveStereoResampler::configure(std::uint32_t inputRate,
                                    std::uint32_t outputRate) {
    const std::uint32_t newIn = inputRate ? inputRate : 48000;
    const std::uint32_t newOut = outputRate ? outputRate : 48000;
    if (newIn != inputRate_ || newOut != outputRate_) {
        inputRate_ = newIn;
        outputRate_ = newOut;
        reset();
    }
}

void WaveStereoResampler::reset() {
    phase_ = 0.0;
    history_.assign(8, 0.0f); // 4 stereo frames of history
}

void WaveStereoResampler::process(std::span<const float> inputInterleaved,
                                  std::vector<float>& outputInterleaved) {
    const std::size_t inFrames = inputInterleaved.size() / 2;
    if (inFrames == 0) {
        return;
    }
    if (inputRate_ == outputRate_) {
        outputInterleaved.insert(outputInterleaved.end(),
                                 inputInterleaved.begin(),
                                 inputInterleaved.begin() +
                                     static_cast<std::ptrdiff_t>(inFrames * 2));
        return;
    }

    if (history_.size() < 8) {
        history_.assign(8, 0.0f);
    }

    const std::size_t histFrames = history_.size() / 2;
    history_.insert(history_.end(), inputInterleaved.begin(),
                    inputInterleaved.begin() +
                        static_cast<std::ptrdiff_t>(inFrames * 2));

    const std::size_t totalFrames = histFrames + inFrames;
    const double step =
        static_cast<double>(inputRate_) / static_cast<double>(outputRate_);

    // Keep at least 2 lookahead frames for cubic interpolation.
    double pos = phase_ + 1.0; // offset by 1 so i-1 is valid
    while (pos + 2.0 < static_cast<double>(totalFrames)) {
        const std::size_t idx = static_cast<std::size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(idx));

        for (int c = 0; c < 2; ++c) {
            const float y0 = history_[(idx - 1) * 2 + static_cast<std::size_t>(c)];
            const float y1 = history_[idx * 2 + static_cast<std::size_t>(c)];
            const float y2 = history_[(idx + 1) * 2 + static_cast<std::size_t>(c)];
            const float y3 = history_[(idx + 2) * 2 + static_cast<std::size_t>(c)];
            const float sample = std::clamp(catmullRom(y0, y1, y2, y3, frac), -1.0f, 1.0f);
            outputInterleaved.push_back(sample);
        }
        pos += step;
    }

    // Keep the last 4 frames as history for the next call.
    const std::size_t keepFrom = totalFrames > 4 ? (totalFrames - 4) : 0;
    phase_ = (pos - 1.0) - static_cast<double>(keepFrom);
    std::vector<float> nextHist(
        history_.begin() + static_cast<std::ptrdiff_t>(keepFrom * 2),
        history_.end());
    history_.swap(nextHist);
}

} // namespace bgmedia::wave
