#pragma once

#include <cstdint>
#include <string>

namespace bgmedia {

// Tunable analysis contract. Anything that changes cached results must be
// represented here (or by kAnalyzerVersion) because fingerprint() is part of
// the cache key.
struct AnalysisConfig {
    // Tempo search range. 55..200 covers the test corpus and most dance music.
    // The estimator also considers metric relatives (1/3, 1/2, 2/3, 3/4, 4/3,
    // 3/2, 2, 3); preferredBpm is only a weak prior, never a hard target.
    double minBpm = 55.0;
    double maxBpm = 200.0;
    double preferredBpm = 120.0;

    // 0 selects a sample-rate-aware default (2048 / 512 at 44.1-48 kHz).
    // hopSize 0 selects stftSize / 4.
    int stftSize = 0;
    int hopSize = 0;

    // Number of log-spaced spectral bands used by onset and bass detection.
    int melBands = 80;

    // >1 increases onset sensitivity (lower adaptive threshold).
    double onsetSensitivity = 1.0;
    // >1 makes bass accent classification more permissive.
    double bassAccentSensitivity = 1.0;

    // Silence shorter than this is not reported; beats are not extrapolated
    // through longer silent gaps.
    double minSilenceSeconds = 0.8;
    // Silence threshold relative to the track's 95th percentile RMS:
    // value is dBFS below that percentile, clamped to an absolute floor.
    double silenceThresholdDb = -42.0;

    int beatsPerBarHint = 4;

    // Tempo change detection. Stable segments are only reported when at least
    // this many consistent windows agree. A transition is reported separately
    // and never promoted to a confident BPM.
    bool detectTempoChanges = true;
    double tempoWindowSeconds = 6.0;
    double tempoHopSeconds = 1.5;

    [[nodiscard]] std::uint64_t fingerprint() const noexcept;
    [[nodiscard]] std::string toJson() const;
};

} // namespace bgmedia
