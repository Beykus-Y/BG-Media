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

// 16-bit RIFF/WAVE writer used by tests and the fixture generator.
bool writeWav16(const std::filesystem::path& path, const AudioData& audio,
                std::string* error = nullptr);

} // namespace bgmedia::test
