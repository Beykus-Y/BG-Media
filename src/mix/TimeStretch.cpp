#include "bgmedia/mix/ITimeStretchProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bgmedia::mix {

void LinearResampleStretcher::render(const PcmTrack& track,
                                     double& positionFrames, double tempoRatio,
                                     float* out,
                                     std::size_t outFrames) const noexcept {
    if (!track.valid() || !out) {
        if (out) {
            std::fill_n(out, outFrames * 2, 0.0f);
        }
        return;
    }
    const std::uint32_t channels = track.channels;
    const std::size_t sourceFrames = track.frameCount();
    const double step = std::clamp(tempoRatio, 0.05, 8.0);
    for (std::size_t i = 0; i < outFrames; ++i) {
        if (positionFrames < 0.0 ||
            positionFrames >= static_cast<double>(sourceFrames) - 1.0) {
            out[i * 2] = 0.0f;
            out[i * 2 + 1] = 0.0f;
            positionFrames += step;
            continue;
        }
        const auto index = static_cast<std::size_t>(positionFrames);
        const float frac =
            static_cast<float>(positionFrames - static_cast<double>(index));
        if (channels == 1) {
            const float a = track.samples[index];
            const float b = track.samples[std::min(index + 1, sourceFrames - 1)];
            const float value = a + (b - a) * frac;
            out[i * 2] = value;
            out[i * 2 + 1] = value;
        } else {
            const float a0 = track.samples[index * 2];
            const float a1 = track.samples[index * 2 + 1];
            const std::size_t next = std::min(index + 1, sourceFrames - 1);
            const float b0 = track.samples[next * 2];
            const float b1 = track.samples[next * 2 + 1];
            out[i * 2] = a0 + (b0 - a0) * frac;
            out[i * 2 + 1] = a1 + (b1 - a1) * frac;
        }
        positionFrames += step;
    }
}

void WsolaStretcher::render(const PcmTrack&, double&, double, float* out,
                            std::size_t outFrames) const noexcept {
    // Deliberately silent and unavailable; the engine only selects this class
    // when a real implementation replaces it.
    if (out) {
        std::memset(out, 0, outFrames * 2 * sizeof(float));
    }
}

} // namespace bgmedia::mix
