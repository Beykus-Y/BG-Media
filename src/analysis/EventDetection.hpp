#pragma once

#include "bgmedia/analysis/AnalysisConfig.hpp"
#include "bgmedia/analysis/AnalysisResult.hpp"
#include "analysis/SpectralAnalysis.hpp"

#include <cstdint>
#include <vector>

namespace bgmedia {

struct SilenceAnalysis {
    std::vector<SilenceRegion> regions;
    std::vector<std::uint8_t> active; // per spectral frame
    double activeRatio = 1.0;
};

[[nodiscard]] SilenceAnalysis detectSilence(const SpectralFeatures& features,
                                            const AnalysisConfig& config,
                                            double duration);

// Bass accents are classified from low-band transient dominance, not from
// broadband loudness. The nearestBeat/offset fields are filled here so the UI
// can explain why an accent was accepted.
[[nodiscard]] std::vector<BassAccent> detectBassAccents(
    const SpectralFeatures& features, const std::vector<SilenceRegion>& silence,
    const std::vector<Beat>& beats, double duration, const AnalysisConfig& config);

// Conservative energy-based section boundaries. Confidence stays low unless
// there is a clear, sustained level change.
[[nodiscard]] std::vector<Section> detectSections(const SpectralFeatures& features,
                                                  double duration,
                                                  const AnalysisConfig& config);

} // namespace bgmedia
