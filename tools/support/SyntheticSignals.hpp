#pragma once

#include "bgmedia/audio/AudioData.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace bgmedia::test {

struct TempoPoint {
    double time = 0.0;
    double bpm = 0.0;
};

struct SyntheticTrack {
    AudioData audio;
    double bpm = 0.0;
    std::vector<double> beatTimes;
    std::vector<double> downbeatTimes;
    std::vector<double> kickTimes;
    std::vector<double> swellTimes;
    std::vector<TempoPoint> tempoChanges;
    std::vector<std::pair<double, double>> silenceRegions;
};

struct TrackOptions {
    double bpm = 92.0;
    double duration = 20.0;
    double beatOffset = 0.45;
    int beatsPerBar = 4;
    bool kickEveryBeat = true;
    bool snare = true;
    bool hats = true;
    bool bass = true;
    bool broadbandSwells = false;
    double silenceStart = -1.0;
    double silenceEnd = -1.0;
};

[[nodiscard]] SyntheticTrack makeClickTrack(const TrackOptions& options);

[[nodiscard]] SyntheticTrack makeTempoChangeTrack(double bpmA, double bpmB,
                                                  double splitSeconds,
                                                  double duration = 24.0);

[[nodiscard]] SyntheticTrack makeSilenceTrack();

[[nodiscard]] SyntheticTrack makeFullySilentTrack(double duration = 6.0);

[[nodiscard]] SyntheticTrack makeBassAccentTrack();

[[nodiscard]] SyntheticTrack makeAmbiguousTrack();

// Regression corpus for metric-level and phase robustness. These are
// deliberately not just click tracks: they encode offbeat, ternary, half-time,
// shuffle, dense-hat and syncopated-bass patterns with known ground truth.
[[nodiscard]] SyntheticTrack makeOffbeatTrack(double bpm = 100.0,
                                              double duration = 18.0);
[[nodiscard]] SyntheticTrack makeTernaryTrack(double pulseBpm = 66.2,
                                              double duration = 24.0);
[[nodiscard]] SyntheticTrack makeHalfTimeTrack(double bpm = 80.0,
                                               double duration = 18.0);
[[nodiscard]] SyntheticTrack makeShuffleTrack(double bpm = 80.0,
                                              double duration = 18.0);
[[nodiscard]] SyntheticTrack makeDenseHatTrack(double bpm = 99.0,
                                               double duration = 18.0);
[[nodiscard]] SyntheticTrack makeSyncopatedBassTrack(double bpm = 100.0,
                                                     double duration = 18.0);
[[nodiscard]] SyntheticTrack makeDriftTrack(double bpm = 80.0,
                                            double duration = 18.0);

// Compound-meter material: `pulsesPerBar` dotted-quarter pulses, each divided
// into `subdivision` equal parts. 6/8 = 2 pulses, 12/8 = 4 pulses.
[[nodiscard]] SyntheticTrack makeCompoundTrack(double pulseBpm, int pulsesPerBar,
                                               double duration = 24.0,
                                               int subdivision = 3);
// Loud offbeat hats over a deliberately weak kick: the grid must stay on the
// low pulse, not on the hi-hat layer.
[[nodiscard]] SyntheticTrack makeStrongHatWeakKickTrack(double bpm = 128.0,
                                                        double duration = 20.0);
// Continuous tempo ramp (no sudden segment boundary).
[[nodiscard]] SyntheticTrack makeTempoRampTrack(double bpmA, double bpmB,
                                                double duration = 24.0);
// Syncopated accents: kick only on downbeats, snares/bass shifted off the beat.
[[nodiscard]] SyntheticTrack makeShiftedAccentTrack(double bpm = 100.0,
                                                     double duration = 20.0);

// 16-bit RIFF/WAVE writer used by tests and the fixture generator.
bool writeWav16(const std::filesystem::path& path, const AudioData& audio,
                std::string* error = nullptr);

} // namespace bgmedia::test
