#include "framework.hpp"

#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/motion3/BeatClock.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace bgmedia;
using motion::BeatClock;

// A tempo that wobbles by about +-12 % with 20 ms of detector jitter on top.
// Every beat is therefore a place where a piecewise-linear clock would change
// speed abruptly.
AnalysisResult makeWobblyGrid(int count) {
    AnalysisResult analysis;
    analysis.info.duration = 120.0;
    analysis.bpm = 120.0;
    analysis.tempoStable = false;
    analysis.bpmConfidence = 0.9f;
    analysis.beatPhaseConfidence = 0.9f;
    std::uint32_t state = 11;
    double time = 0.4;
    for (int i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        const double jitter = (static_cast<int>((state >> 10) % 41) - 20) * 0.001;
        Beat beat;
        beat.time = time + jitter;
        beat.beatInBar = i % 4;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
        time += 0.5 * (1.0 + 0.12 * std::sin(0.37 * i));
    }
    return analysis;
}

double relativeDifference(double a, double b) {
    return std::abs(a - b) / std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

} // namespace

TEST_CASE(beat_clock.jittery_grid_has_continuous_tempo_at_beats) {
    const AnalysisResult analysis = makeWobblyGrid(90);
    const BeatClock clock = BeatClock::fromAnalysis(analysis);
    const std::vector<double>& knots = clock.gridTimes();
    CHECK(knots.size() > 40);
    if (knots.size() < 3) {
        return;
    }
    // Speed on either side of every grid time must agree. A piecewise-linear
    // clock differs by several percent here, a C1 spline by numerical noise.
    const double eps = 1e-7;
    double worst = 0.0;
    for (std::size_t k = 1; k + 1 < knots.size(); ++k) {
        const double left = clock.bpmAt(knots[k] - eps);
        const double right = clock.bpmAt(knots[k] + eps);
        worst = std::max(worst, relativeDifference(left, right));
    }
    CHECK(worst < 1e-4);

    // Tempo changes smoothly: over 1 ms the bpm moves by a tiny amount.
    double maxStep = 0.0;
    for (double t = 1.0; t < 40.0; t += 0.0173) {
        maxStep = std::max(maxStep, std::abs(clock.bpmAt(t + 1e-3) - clock.bpmAt(t)));
    }
    CHECK(maxStep < 0.5);
}

TEST_CASE(beat_clock.speed_matches_finite_difference_of_beat_position) {
    const BeatClock clock = BeatClock::fromAnalysis(makeWobblyGrid(90));
    const double h = 1e-5;
    for (double t = 1.0; t < 40.0; t += 0.071) {
        const double numeric = 60.0 * (clock.beatAt(t + h) - clock.beatAt(t - h)) / (2.0 * h);
        CHECK(relativeDifference(clock.bpmAt(t), numeric) < 1e-4);
    }
}

TEST_CASE(beat_clock.time_and_beat_are_exact_inverses) {
    const BeatClock clock = BeatClock::fromAnalysis(makeWobblyGrid(90));
    double lastBeat = -1e300;
    for (double t = 0.0; t < 42.0; t += 0.0097) {
        const double beat = clock.beatAt(t);
        CHECK(beat > lastBeat);
        lastBeat = beat;
        CHECK_NEAR(clock.timeAtBeat(beat), t, 1e-9);
    }
    const double lastGrid = static_cast<double>(clock.gridTimes().size() - 1);
    for (double beat = -2.0; beat < lastGrid + 2.0; beat += 0.0531) {
        CHECK_NEAR(clock.beatAt(clock.timeAtBeat(beat)), beat, 1e-9);
    }
    // Beat boundaries land exactly on the analysed beat times.
    for (std::size_t k = 0; k < clock.gridTimes().size(); ++k) {
        CHECK_NEAR(clock.timeAtBeat(static_cast<double>(k)), clock.gridTimes()[k], 1e-9);
    }
}

TEST_CASE(beat_clock.constant_grid_is_linear_and_unchanged) {
    const BeatClock constant = BeatClock::constant(120.0, 0.5);
    for (double t = -3.0; t < 30.0; t += 0.37) {
        CHECK_NEAR(constant.beatAt(t), (t - 0.5) * 2.0, 1e-12);
    }
    for (double beat = -4.0; beat < 40.0; beat += 0.61) {
        CHECK_NEAR(constant.timeAtBeat(beat), 0.5 + beat * 0.5, 1e-12);
    }
    CHECK_NEAR(constant.bpmAt(7.0), 120.0, 1e-12);

    // A perfectly regular analysed grid is a straight line as well.
    AnalysisResult analysis;
    analysis.info.duration = 40.0;
    analysis.bpm = 120.0;
    analysis.tempoStable = true;
    analysis.bpmConfidence = 0.9f;
    analysis.beatPhaseConfidence = 0.9f;
    for (int i = 0; i < 80; ++i) {
        Beat beat;
        beat.time = 0.4 + i * 0.5;
        beat.beatInBar = i % 4;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
    }
    const BeatClock grid = BeatClock::fromAnalysis(analysis);
    for (double t = 1.0; t < 38.0; t += 0.123) {
        CHECK_NEAR(grid.beatAt(t), (t - 0.4) * 2.0, 1e-9);
        CHECK_NEAR(grid.bpmAt(t), 120.0, 1e-7);
    }
}
