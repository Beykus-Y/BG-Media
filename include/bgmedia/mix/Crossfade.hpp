#pragma once

#include "bgmedia/mix/MixTypes.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::mix {

// Equal-power crossfade. `t` is 0 at the start (outgoing only) and 1 at the end
// (incoming only).
[[nodiscard]] inline float equalPowerOut(double t) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    return static_cast<float>(std::cos(t * 1.57079632679489661923));
}

[[nodiscard]] inline float equalPowerIn(double t) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    return static_cast<float>(std::sin(t * 1.57079632679489661923));
}

// Correlated material can add up to +6 dB even with equal-power gains. The
// engine measures the short-term correlation of the two decks and applies this
// headroom to both channels while a transition is running.
[[nodiscard]] inline float correlationHeadroom(float rho) noexcept {
    rho = std::clamp(rho, 0.0f, 1.0f);
    return 1.0f / std::sqrt(1.0f + rho);
}

// Gain-aware version. Correlation only matters when both decks are actually
// audible: at the very start or end of a fade one gain is near zero, and
// reducing the other deck there would create an audible level dip.
[[nodiscard]] inline float correlationHeadroom(float rho, float gainA,
                                               float gainB) noexcept {
    const float sum = gainA + gainB;
    if (sum <= 1e-6f) {
        return 1.0f;
    }
    const float balance =
        2.0f * std::sqrt(std::max(0.0f, gainA * gainB)) / sum;
    return correlationHeadroom(std::clamp(rho * balance, 0.0f, 1.0f));
}

// Smooth 0..1 ramp used by tempo automation and de-click envelopes.
[[nodiscard]] inline double smoothStep(double t) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

} // namespace bgmedia::mix
