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
    history_.fill(0.0f);
    prefilterPrev_.fill(0.0f);
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

    std::span<const float> src;
    if (inputRate_ > outputRate_) {
        // 3-tap [0.25, 0.5, 0.25] anti-aliasing prefilter when downsampling.
        filteredScratch_.resize(inFrames * 2);
        float p2L = prefilterPrev_[0];
        float p2R = prefilterPrev_[1];
        float p1L = prefilterPrev_[2];
        float p1R = prefilterPrev_[3];
        for (std::size_t i = 0; i < inFrames; ++i) {
            const float xL = inputInterleaved[i * 2];
            const float xR = inputInterleaved[i * 2 + 1];
            filteredScratch_[i * 2] = 0.25f * p2L + 0.5f * p1L + 0.25f * xL;
            filteredScratch_[i * 2 + 1] = 0.25f * p2R + 0.5f * p1R + 0.25f * xR;
            p2L = p1L;
            p2R = p1R;
            p1L = xL;
            p1R = xR;
        }
        prefilterPrev_[0] = p2L;
        prefilterPrev_[1] = p2R;
        prefilterPrev_[2] = p1L;
        prefilterPrev_[3] = p1R;
        src = filteredScratch_;
    } else {
        src = inputInterleaved.subspan(0, inFrames * 2);
    }

    auto sampleAt = [&](std::size_t frameIdx, std::size_t ch) noexcept -> float {
        if (frameIdx < 4) {
            return history_[frameIdx * 2 + ch];
        }
        return src[(frameIdx - 4) * 2 + ch];
    };

    const std::size_t totalFrames = 4 + inFrames;
    const double step =
        static_cast<double>(inputRate_) / static_cast<double>(outputRate_);

    // Keep at least 2 lookahead frames for cubic interpolation.
    double pos = phase_ + 1.0; // offset by 1 so i-1 is valid
    while (pos + 2.0 < static_cast<double>(totalFrames)) {
        const std::size_t idx = static_cast<std::size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(idx));

        for (std::size_t c = 0; c < 2; ++c) {
            const float y0 = sampleAt(idx - 1, c);
            const float y1 = sampleAt(idx, c);
            const float y2 = sampleAt(idx + 1, c);
            const float y3 = sampleAt(idx + 2, c);
            const float sample =
                std::clamp(catmullRom(y0, y1, y2, y3, frac), -1.0f, 1.0f);
            outputInterleaved.push_back(sample);
        }
        pos += step;
    }

    // Keep the last 4 virtual frames in fixed history_ without heap allocation.
    phase_ = (pos - 1.0) - static_cast<double>(inFrames);
    if (inFrames >= 4) {
        const std::size_t tailOffset = (inFrames - 4) * 2;
        std::copy_n(src.data() + tailOffset, 8, history_.data());
    } else {
        const std::size_t keepHistFrames = 4 - inFrames;
        std::move(history_.begin() + static_cast<std::ptrdiff_t>(inFrames * 2),
                  history_.end(), history_.begin());
        std::copy_n(src.data(), inFrames * 2,
                    history_.data() + keepHistFrames * 2);
    }
}

} // namespace bgmedia::wave
