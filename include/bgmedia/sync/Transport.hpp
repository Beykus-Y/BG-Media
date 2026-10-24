#pragma once

#include <cstdint>

namespace bgmedia {

// Time is expressed in media seconds of a single source timeline. Annotation:
// BGMEDIA_TIME_SECONDS. Choreography never sees wall-clock time or FPS; it is
// sampled at a media timestamp. A future video track plugs into the same model.
struct TransportSnapshot {
    // Source position of the next frame the player will render. This is the
    // player's raw cursor, not necessarily what the listener hears right now.
    double sourcePosition = 0.0;
    double duration = 0.0;

    bool playing = false;
    double rate = 1.0;

    // Seconds of already-rendered audio that have not reached the speakers
    // yet (device buffer + backend latency). Subtracted by PlaybackClock.
    double outputLatency = 0.0;

    // Incremented by the host on seek, stop, or any discontinuity that must
    // snap the visual clock rather than smooth across it.
    std::uint64_t discontinuity = 0;

    // Monotonic host time in seconds. Must come from the same clock for every
    // snapshot; std::chrono::steady_clock is a good choice.
    double hostTime = 0.0;
};

// Adapter implemented by the host (desktop miniaudio, later AAudio/AVAudio-
// Engine). The engine only reads snapshots; transport commands go through this
// interface so choreography/UI code is independent of the audio backend.
class IPlaybackSource {
public:
    virtual ~IPlaybackSource() = default;
    [[nodiscard]] virtual TransportSnapshot snapshot() const = 0;
};

class IPlaybackController : public IPlaybackSource {
public:
    ~IPlaybackController() override = default;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void seek(double mediaSeconds) = 0;
    virtual void setRate(double rate) = 0;
};

// Converts jittery adapter snapshots into a smooth, monotonic media clock and
// compensates output latency. Discontinuities (seek, loop, rate change) snap.
class PlaybackClock {
public:
    struct Options {
        // Difference above which the visual clock snaps rather than glides.
        double seekThresholdSeconds = 0.12;
        // Time constant for small correction; keeps the cursor fluid at
        // 30-240 FPS without adding audible-sync drift.
        double smoothingSeconds = 0.025;
        // Do not extrapolate further than this when snapshots stall.
        double maxExtrapolationSeconds = 0.25;
    };

    PlaybackClock() = default;
    explicit PlaybackClock(Options options) : options_(options) {}

    void reset();

    // Call once per rendered frame. Returns the audible media position.
    [[nodiscard]] double update(const TransportSnapshot& snapshot);

    [[nodiscard]] double position() const noexcept { return position_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] double rate() const noexcept { return rate_; }
    // Raw latency-compensated target from the last snapshot, for diagnostics.
    [[nodiscard]] double targetPosition() const noexcept { return targetPosition_; }

private:
    [[nodiscard]] double rawAudible(const TransportSnapshot& s) const noexcept;

    Options options_{};
    bool initialised_ = false;
    bool playing_ = false;
    double rate_ = 1.0;
    double position_ = 0.0;
    double targetPosition_ = 0.0;
    double duration_ = 0.0;
    double lastUpdateHostTime_ = 0.0;
    std::uint64_t discontinuity_ = 0;
};

} // namespace bgmedia
