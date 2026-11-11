#pragma once

#include "bgmedia/analysis/AnalysisConfig.hpp"
#include "bgmedia/analysis/AnalysisResult.hpp"

#include <vector>

namespace bgmedia {

struct TempoCandidate {
    double bpm = 0.0;
    // BPM from the least-squares fit over matched onsets (0 if unavailable).
    double fittedBpm = 0.0;
    double score = 0.0;
    double acScore = 0.0;
    double gridScore = 0.0;
    double coverage = 0.0;
    double beatHitRate = 0.0;
    double onsetHitRate = 0.0;
    double precisionAdjusted = 0.0;
    double recallAdjusted = 0.0;
    double pulseScore = 0.0;
    double lowHitRate = 0.0;
    double lowContrast = 0.0;
    double levelSupport = 0.0;
    double prior = 0.0;
    bool octaveRelatedToBest = false;
};

struct TempoEstimate {
    double bpm = 0.0;
    float confidence = 0.0f;
    // Heuristic split of the old single confidence. These are scores, not
    // calibrated probabilities.
    float levelConfidence = 0.0f;
    bool octaveAmbiguous = false;
    std::vector<TempoCandidate> candidates; // sorted, strongest first
};

struct TempoWindow {
    double start = 0.0;
    double end = 0.0;
    double centre = 0.0;
    double bpm = 0.0;
    float confidence = 0.0f;
    double activeFraction = 0.0;
};

struct TempoStructure {
    double bpm = 0.0;
    float confidence = 0.0f;
    float levelConfidence = 0.0f;
    bool stable = true;
    bool octaveAmbiguous = false;
    std::vector<TempoSegment> segments;
    std::vector<TempoTransition> transitions;
    std::vector<double> candidates;
    std::vector<float> candidateScores;
};

// Estimates tempo from a normalised onset envelope. activeFraction is used to
// scale confidence; inactive frames must already be zeroed in env.
[[nodiscard]] TempoEstimate estimateTempo(const std::vector<float>& env,
                                          double fps,
                                          const AnalysisConfig& config,
                                          double activeFraction,
                                          double continuityBpm = 0.0,
                                          const std::vector<float>* lowEnv = nullptr);

[[nodiscard]] TempoStructure estimateTempoStructure(
    const std::vector<float>& env, const std::vector<std::uint8_t>& active,
    double fps, double duration, const AnalysisConfig& config,
    const TempoEstimate& global,
    const std::vector<float>* lowEnv = nullptr);

[[nodiscard]] double bpmAtTime(const TempoStructure& structure, double time,
                               double fallback);

} // namespace bgmedia
