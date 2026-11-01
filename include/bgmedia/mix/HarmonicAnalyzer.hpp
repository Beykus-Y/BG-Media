#pragma once

// BG Mix 0.3 — Harmonic pitch-class chroma, global/local key & tonalness
// estimation, pitch-shift-aware chroma rotation, and overlap harmonic /
// spectral masking compatibility scoring.
//
// Camelot codes (1A..12B) are provided strictly as a UI/diagnostic presentation
// helper; transition scoring uses continuous chroma similarity, circle-of-fifths
// / relative mode distance, tonalness weighting, pitch-shift detune, and
// spectral crowding.

#include "bgmedia/mix/MixFeatureMap.hpp"
#include "bgmedia/mix/MixTypes.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bgmedia::mix {

class HarmonicAnalyzer {
public:
    static constexpr std::uint32_t kVersion = 1;

    // Extracts per-frame dominant pitch class & harmonic salience, local
    // phrase/bar harmonic segments, and global key/mode/tonalness summary.
    void analyze(const PcmBuffer& monoPcm,
                 MixFeatureMap& map) const;

    // Correlates a 12-bin chroma vector against 24 major/minor tonal profiles.
    static void estimateKeyFromChroma(const std::array<float, 12>& chroma,
                                      float tonalness,
                                      int* keyIndexOut,
                                      bool* isMinorOut,
                                      float* confidenceOut) noexcept;

    // Computes tonalness (0..1) from a 12-bin chroma distribution (0 = flat/atonal
    // drum/noise spectrum, 1 = strongly pitched harmonic content).
    [[nodiscard]] static float computeChromaTonalness(
        const std::array<float, 12>& chroma) noexcept;

    // Rotates a 12-bin chroma vector by `semitones` (supports fractional semitones
    // when pitchLock == false and playbackRate != 1.0).
    [[nodiscard]] static std::array<float, 12> shiftChroma(
        const std::array<float, 12>& chroma,
        double semitones) noexcept;

    // Cosine similarity in [0, 1] between two 12-bin chroma vectors.
    [[nodiscard]] static float chromaCosineSimilarity(
        const std::array<float, 12>& a,
        const std::array<float, 12>& b) noexcept;

    // Continuous key-relationship compatibility in [0, 1] (unison, relative
    // major/minor, perfect fifth/fourth, parallel major/minor, etc.).
    [[nodiscard]] static float keyRelationshipScore(
        int keyA, bool minorA,
        int keyB, bool minorB) noexcept;

    // Presentation helpers (Camelot is never used as a hard gating rule).
    [[nodiscard]] static std::string toCamelotCode(int keyIndex, bool isMinor);
    [[nodiscard]] static std::string keyName(int keyIndex, bool isMinor);
};

struct HarmonicCompatibility {
    bool available = false;
    float confidence = 0.0f;

    float tonalnessA = 0.0f;
    float tonalnessB = 0.0f;
    // True when at least one overlap window is percussive/atonal (tonalness < 0.25),
    // neutralizing harmonic key clashes.
    bool percussiveNeutral = false;

    // Effective semitone shift applied to A and B when pitchLock == false.
    double semitoneShiftA = 0.0;
    double semitoneShiftB = 0.0;
    // Penalty (0..1) for fractional-semitone tuning mismatch when pitchLock == false.
    float detunePenalty = 0.0f;

    float chromaSimilarity = 0.0f;
    float keyRelationScore = 0.0f;
    // Simultaneous mid-band / low-band spectral crowding in [0, 1].
    float spectralCrowdScore = 0.0f;

    // Overall harmonic compatibility (0..1, higher is better) and clash risk (0..1).
    float compatibilityScore = 0.5f;
    float harmonicClashScore = 0.0f;

    std::string summary;
};

// Evaluates harmonic compatibility and spectral masking over the transition
// overlap window `[startA, startA + duration * rateA]` and
// `[startB, startB + duration * rateB]`, accounting for `pitchLock`.
[[nodiscard]] HarmonicCompatibility evaluateHarmonicCompatibility(
    const MixFeatureMap& featuresA,
    double startA,
    double rateA,
    const MixFeatureMap& featuresB,
    double startB,
    double rateB,
    double duration,
    bool pitchLock = true,
    const std::vector<GainPoint>& gainCurveOut = {},
    const std::vector<GainPoint>& gainCurveIn = {},
    const std::vector<EqGainPoint>& eqCurveOut = {},
    const std::vector<EqGainPoint>& eqCurveIn = {});

} // namespace bgmedia::mix
