#pragma once

// BeatClock: the stable media-time <-> musical-beat mapping used by BG Motion.
//
// It is deliberately independent from the analyser and from rendering. A clock
// can be built from a BG Beat analysis (a monotone cubic Hermite through the
// smoothed beat timestamps, so the tempo changes smoothly across every beat),
// or from a constant Motion Lab tempo. The clock never divides by a zero interval and never extrapolates with
// a non-finite period; the edge cases are handled explicitly below.

#include "bgmedia/analysis/AnalysisResult.hpp"

#include <vector>

namespace bgmedia::motion {

class BeatClock {
public:
    BeatClock() = default;

    [[nodiscard]] static BeatClock constant(double bpm, double firstBeatTime = 0.0,
                                            int beatsPerBar = 4);
    // Smoothed grid from BG Beat. Falls back to the analysed BPM when the beat
    // list is too short. Non-finite, duplicated and out-of-order beat events
    // are sanitised before fitting.
    [[nodiscard]] static BeatClock fromAnalysis(const AnalysisResult& analysis,
                                                double fallbackBpm = 100.0);

    [[nodiscard]] double beatAt(double mediaTime) const;
    [[nodiscard]] double timeAtBeat(double beat) const;
    [[nodiscard]] double bpmAt(double mediaTime) const;
    // Beat index within the bar (0 = downbeat) of the beat containing `beat`.
    [[nodiscard]] int beatInBar(double beat) const;
    [[nodiscard]] int beatsPerBar() const { return beatsPerBar_; }
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] float confidence() const { return confidence_; }
    [[nodiscard]] bool constantGrid() const { return times_.empty(); }
    [[nodiscard]] double nominalBpm() const { return bpm_; }
    [[nodiscard]] int barOffset() const { return barOffset_; }

    // --- phase helpers -----------------------------------------------------
    // Fractional position inside the beat that contains `mediaTime`.
    [[nodiscard]] float phaseAt(double mediaTime) const;
    // Duration of the beat interval that contains `mediaTime` (seconds).
    [[nodiscard]] double beatPeriodAt(double mediaTime) const;
    // Media time of the beat boundary with the requested index. Negative
    // indices extrapolate backwards with the head period.
    [[nodiscard]] double beatTime(int index) const;
    [[nodiscard]] double nextBeatTime(double mediaTime) const;
    [[nodiscard]] double previousBeatTime(double mediaTime) const;

    // Smoothed grid timestamps. Empty for a constant grid.
    [[nodiscard]] const std::vector<double>& gridTimes() const { return times_; }
    [[nodiscard]] double firstGridTime() const { return times_.empty() ? 0.0 : times_.front(); }
    [[nodiscard]] double lastGridTime() const { return times_.empty() ? 0.0 : times_.back(); }

private:
    bool valid_ = false;
    double bpm_ = 100.0;
    double first_ = 0.0;
    int beatsPerBar_ = 4;
    int barOffset_ = 0;
    float confidence_ = 1.0f;
    std::vector<double> times_;  // smoothed beat times; empty = constant grid
    std::vector<double> tangents_;  // d(beat)/dt at each grid time (monotone)
    double headPeriod_ = 0.6;
    double tailPeriod_ = 0.6;
};

} // namespace bgmedia::motion
