#include "framework.hpp"

#include "bgmedia/sync/Transport.hpp"

#include <cmath>
#include <cstdint>

namespace {

bgmedia::TransportSnapshot snapshot(double source, double duration, bool playing,
                                    double rate, double latency,
                                    std::uint64_t discontinuity, double hostTime) {
    bgmedia::TransportSnapshot s;
    s.sourcePosition = source;
    s.duration = duration;
    s.playing = playing;
    s.rate = rate;
    s.outputLatency = latency;
    s.discontinuity = discontinuity;
    s.hostTime = hostTime;
    return s;
}

} // namespace

TEST_CASE(sync.latency_and_playback) {
    using namespace bgmedia;
    PlaybackClock clock;
    constexpr double duration = 30.0;
    constexpr double latency = 0.020;
    constexpr double frame = 1.0 / 60.0;

    CHECK_NEAR(clock.update(snapshot(latency, duration, true, 1.0, latency, 0, 0.0)),
               0.0, 1e-9);

    double last = 0.0;
    for (double t = frame; t <= 1.0 + 1e-9; t += frame) {
        const double value =
            clock.update(snapshot(latency + t, duration, true, 1.0, latency, 0, t));
        CHECK(value + 1e-9 >= last);
        last = value;
    }
    CHECK_NEAR(last, 1.0, 0.035);
}

TEST_CASE(sync.pause_holds_and_seek_snaps) {
    using namespace bgmedia;
    PlaybackClock clock;
    constexpr double duration = 30.0;
    constexpr double latency = 0.02;

    (void)clock.update(snapshot(5.02, duration, true, 1.0, latency, 0, 0.0));
    for (double t = 0.016; t <= 1.0; t += 0.016) {
        (void)clock.update(snapshot(5.02 + t, duration, true, 1.0, latency, 0, t));
    }

    const double paused =
        clock.update(snapshot(6.02, duration, false, 1.0, latency, 0, 1.1));
    CHECK_NEAR(paused, 6.0, 0.03);
    const double held =
        clock.update(snapshot(6.02, duration, false, 1.0, latency, 0, 1.8));
    CHECK_NEAR(held, 6.0, 0.001);
    CHECK(!clock.playing());

    const double sought =
        clock.update(snapshot(12.02, duration, false, 1.0, latency, 1, 2.0));
    CHECK_NEAR(sought, 12.0, 0.03);
    CHECK_NEAR(clock.position(), 12.0, 0.03);
}

TEST_CASE(sync.rate_change) {
    using namespace bgmedia;
    PlaybackClock clock;
    constexpr double duration = 30.0;
    constexpr double latency = 0.03;
    constexpr double rate = 1.5;

    (void)clock.update(snapshot(2.045, duration, true, rate, latency, 0, 0.0));
    double t = 0.0;
    for (int i = 1; i <= 60; ++i) {
        t = static_cast<double>(i) / 60.0;
        (void)clock.update(snapshot(2.045 + t * rate, duration, true, rate, latency, 0, t));
    }
    CHECK_NEAR(clock.position(), 2.0 + t * rate, 0.05);
    CHECK_NEAR(clock.rate(), rate, 1e-9);
}

TEST_CASE(sync.clamps_to_duration) {
    using namespace bgmedia;
    PlaybackClock clock;
    (void)clock.update(snapshot(9.95, 10.0, true, 1.0, 0.02, 0, 0.0));
    (void)clock.update(snapshot(10.0, 10.0, true, 1.0, 0.0, 0, 0.1));
    CHECK(clock.position() <= 10.0);
    const double atEnd = clock.update(snapshot(10.0, 10.0, false, 1.0, 0.0, 0, 0.2));
    CHECK_NEAR(atEnd, 10.0, 1e-9);
}

TEST_CASE(sync.smooths_small_jitter_monotonically) {
    using namespace bgmedia;
    PlaybackClock clock;
    PlaybackClock::Options options;
    options.seekThresholdSeconds = 0.2;
    clock = PlaybackClock(options);

    (void)clock.update(snapshot(0.0, 20.0, true, 1.0, 0.0, 0, 0.0));
    double last = 0.0;
    for (int i = 1; i <= 120; ++i) {
        const double t = static_cast<double>(i) / 60.0;
        const double jitter = (i % 3 == 0) ? 0.004 : -0.002;
        const double value =
            clock.update(snapshot(t + jitter, 20.0, true, 1.0, 0.0, 0, t));
        CHECK(value + 1e-9 >= last);
        last = value;
        CHECK(std::abs(value - t) < 0.10);
    }
}
