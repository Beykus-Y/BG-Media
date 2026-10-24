#include "bgmedia/sync/Transport.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia {

void PlaybackClock::reset() {
    initialised_ = false;
    playing_ = false;
    rate_ = 1.0;
    position_ = 0.0;
    targetPosition_ = 0.0;
    duration_ = 0.0;
    lastUpdateHostTime_ = 0.0;
    discontinuity_ = 0;
}

double PlaybackClock::rawAudible(const TransportSnapshot& s) const noexcept {
    const double duration = std::max(0.0, s.duration);
    if (duration > 0.0 && s.sourcePosition >= duration - 1e-9) {
        return duration;
    }
    const double latency = std::max(0.0, s.outputLatency);
    const double rate = std::max(0.01, std::abs(s.rate));
    double value = s.sourcePosition - latency * rate;
    value = std::max(0.0, value);
    if (duration > 0.0) {
        value = std::min(value, duration);
    }
    return value;
}

double PlaybackClock::update(const TransportSnapshot& snapshot) {
    const double now = snapshot.hostTime;
    const double duration = std::max(0.0, snapshot.duration);
    const double rate = std::max(0.01, std::abs(snapshot.rate));
    const double raw = rawAudible(snapshot);

    const bool rateChanged = initialised_ && std::abs(rate - rate_) > 1e-6;
    const bool playStateChanged = initialised_ && snapshot.playing != playing_;
    const bool discontinuity = initialised_ && snapshot.discontinuity != discontinuity_;

    if (!initialised_ || discontinuity || rateChanged || playStateChanged) {
        initialised_ = true;
        position_ = raw;
        targetPosition_ = raw;
        playing_ = snapshot.playing;
        rate_ = rate;
        duration_ = duration;
        lastUpdateHostTime_ = now;
        discontinuity_ = snapshot.discontinuity;
        return position_;
    }

    double dt = now - lastUpdateHostTime_;
    if (!std::isfinite(dt) || dt < 0.0) {
        dt = 0.0;
    }
    dt = std::min(dt, options_.maxExtrapolationSeconds);

    if (snapshot.playing) {
        const double previous = position_;
        double predicted = position_ + dt * snapshot.rate;
        const double error = raw - predicted;
        if (std::abs(error) > options_.seekThresholdSeconds) {
            position_ = raw;
        } else if (dt > 0.0) {
            const double alpha = 1.0 - std::exp(-dt / std::max(1e-4, options_.smoothingSeconds));
            position_ = predicted + error * alpha;
            // While playing the visual playhead must not run backwards. Seeks
            // and rate changes reached the snap branch above.
            position_ = std::max(previous, position_);
        }
        if (duration > 0.0) {
            position_ = std::clamp(position_, 0.0, duration);
        }
    } else {
        position_ = raw;
    }

    targetPosition_ = raw;
    playing_ = snapshot.playing;
    rate_ = rate;
    duration_ = duration;
    lastUpdateHostTime_ = now;
    discontinuity_ = snapshot.discontinuity;
    return position_;
}

} // namespace bgmedia
