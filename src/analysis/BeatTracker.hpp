#pragma once

#include "bgmedia/analysis/AnalysisConfig.hpp"
#include "bgmedia/analysis/AnalysisResult.hpp"
#include "analysis/SpectralAnalysis.hpp"
#include "analysis/TempoEstimator.hpp"

#include <vector>

namespace bgmedia {

struct BeatTrackingResult {
    std::vector<Onset> onsets;
    std::vector<Beat> beats;
    int beatsPerBar = 4;
    float meterConfidence = 0.0f;
    float beatPhaseConfidence = 0.0f;
    float downbeatConfidence = 0.0f;
    // BG Motion 3.1: a stable track may be tracked at a slower metric level
    // (for example a 180 BPM pulse locked to every third beat). When the
    // evidence supports a faster level, the beats are refined and this
    // multiplier tells the caller how to rescale the reported tempo.
    int metricMultiplier = 1;
    double refinedBpm = 0.0;
};

// Extracts onset peaks from the sharp onset envelope. Silence regions are
// excluded with a small safety margin.
[[nodiscard]] std::vector<Onset> detectOnsets(
    const SpectralFeatures& features, const std::vector<SilenceRegion>& silence,
    double duration, const AnalysisConfig& config);

// Ellis-style dynamic-programming beat tracker with a time-varying local
// period. It runs separately on contiguous non-silent regions, then
// extrapolates the grid only across short internal gaps. Beats that continue a
// grid without direct evidence are flagged and carry lower confidence.
[[nodiscard]] BeatTrackingResult trackBeats(
    const SpectralFeatures& features, const std::vector<std::uint8_t>& active,
    const std::vector<SilenceRegion>& silence, const TempoStructure& tempo,
    double duration, const AnalysisConfig& config);

} // namespace bgmedia
