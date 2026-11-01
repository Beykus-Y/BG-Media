#pragma once

// BG Mix 0.3 — Music-specific vocal activity detection and overlap vocal
// collision / handoff scoring.
//
// Designed so vocal detection runs during track preparation (never on the
// real-time audio thread), supports pluggable external stem/ML detectors, and
// distinguishes harmonic multi-formant singing voice from percussive transients
// or pure single-tone instruments.

#include "bgmedia/mix/MixFeatureMap.hpp"
#include "bgmedia/mix/MixTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bgmedia::mix {

struct VocalDetectionOutput {
    bool available = false;
    VocalDetectorKind detectorKind = VocalDetectorKind::None;
    std::uint32_t modelVersion = 0;
    std::uint64_t modelChecksum = 0;
    float meanConfidence = 0.0f;
    float activeRatio = 0.0f;
    std::vector<float> frameScores;
    std::vector<float> frameConfidences;
    std::vector<VocalRegion> regions;
};

class IVocalActivityDetector {
public:
    virtual ~IVocalActivityDetector() = default;

    [[nodiscard]] virtual VocalDetectorKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t modelVersion() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t modelChecksum() const noexcept = 0;

    [[nodiscard]] virtual VocalDetectionOutput detect(
        const PcmBuffer& monoPcm,
        InterleavedAudioView stereoAudio,
        const AnalysisResult& analysis,
        const std::vector<MixFeatureFrame>& frames,
        double frameStepSeconds) const = 0;
};

// Built-in music-specific vocal activity detector. Combines harmonic pitch
// salience in the human fundamental range (95..880 Hz), multi-formant harmonic
// richness across F1/F2/F3 (300..3400 Hz), harmonic-to-percussive stability,
// syllabic envelope continuity, and stereo center (Mid/Side) focus.
class MusicVocalDetector final : public IVocalActivityDetector {
public:
    static constexpr std::uint32_t kModelVersion = 1;
    static constexpr std::uint64_t kModelChecksum = 0x4247564F434D444CULL; // "BGVOCMDL"

    [[nodiscard]] VocalDetectorKind kind() const noexcept override {
        return VocalDetectorKind::MusicVocalModel;
    }
    [[nodiscard]] std::uint32_t modelVersion() const noexcept override {
        return kModelVersion;
    }
    [[nodiscard]] std::uint64_t modelChecksum() const noexcept override {
        return kModelChecksum;
    }

    [[nodiscard]] VocalDetectionOutput detect(
        const PcmBuffer& monoPcm,
        InterleavedAudioView stereoAudio,
        const AnalysisResult& analysis,
        const std::vector<MixFeatureFrame>& frames,
        double frameStepSeconds) const override;
};

// Computes vocal activity directly from a pre-separated vocal stem buffer.
class ExternalStemVocalDetector final : public IVocalActivityDetector {
public:
    explicit ExternalStemVocalDetector(const PcmBuffer& vocalStem,
                                       std::uint64_t stemChecksum = 0);

    [[nodiscard]] VocalDetectorKind kind() const noexcept override {
        return VocalDetectorKind::ExternalStem;
    }
    [[nodiscard]] std::uint32_t modelVersion() const noexcept override { return 1; }
    [[nodiscard]] std::uint64_t modelChecksum() const noexcept override {
        return stemChecksum_;
    }

    [[nodiscard]] VocalDetectionOutput detect(
        const PcmBuffer& monoPcm,
        InterleavedAudioView stereoAudio,
        const AnalysisResult& analysis,
        const std::vector<MixFeatureFrame>& frames,
        double frameStepSeconds) const override;

private:
    const PcmBuffer& vocalStem_;
    std::uint64_t stemChecksum_ = 0;
};

// Applies a VocalDetectionOutput onto a MixFeatureMap (updating frame vocal
// scores, bar/phrase/cue vocal summaries, and map.vocal).
void applyVocalDetection(MixFeatureMap& map, const VocalDetectionOutput& output);

struct VocalOverlapAssessment {
    bool available = false;
    float confidence = 0.0f;

    // 0..1 score for simultaneous audible lead vocals (double-vocal clash).
    float vocalClashScore = 0.0f;
    double clashDurationSeconds = 0.0;
    double leadVocalClashDurationSeconds = 0.0;
    bool severeLeadVocalClash = false;
    float maxSimultaneousVocalScore = 0.0f;
    bool lowConfidenceConservative = false;

    // 0..1 score for singing over an instrumental bed (clean vocal-over-instrumental).
    float vocalOverInstrumentalScore = 0.0f;

    // Mean vocal presence in the outgoing and incoming overlap windows.
    float outgoingVocalMean = 0.0f;
    float incomingVocalMean = 0.0f;

    // Boundary diagnostics: cutting out or entering in the middle of an active
    // vocal phrase vs handing off between vocal phrases.
    bool cutsMidVocalPhraseA = false;
    bool entersMidVocalPhraseB = false;
    bool cleanVocalHandoff = false;

    std::string summary;
};

// Evaluates vocal collision and handoff quality over an overlap of `duration`
// output seconds starting at `startA` (rate `rateA`) and `startB` (rate `rateB`).
[[nodiscard]] VocalOverlapAssessment evaluateVocalOverlap(
    const MixFeatureMap& featuresA,
    double startA,
    double rateA,
    const MixFeatureMap& featuresB,
    double startB,
    double rateB,
    double duration,
    const std::vector<GainPoint>& gainCurveOut = {},
    const std::vector<GainPoint>& gainCurveIn = {},
    const std::vector<EqGainPoint>& eqCurveOut = {},
    const std::vector<EqGainPoint>& eqCurveIn = {});

} // namespace bgmedia::mix
