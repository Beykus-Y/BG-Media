#pragma once

// BG Mix 0.3 — Time-local musical feature map for context-aware Smart AutoMix.
// Built on top of BG Beat's AnalysisResult (never duplicating beat tracking or
// tempo estimation) and designed so pre-analysis, caching and candidate
// discovery run completely off the real-time audio thread.

#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/audio/AudioData.hpp"
#include "bgmedia/mix/PcmTrack.hpp"
#include "bgmedia/version.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bgmedia::mix {

enum class VocalDetectorKind : std::uint8_t {
    None = 0,          // Vocal detection disabled or not run; vocalConfidence = 0
    SpectralProxy,     // Lightweight harmonic/formant mid-band proxy (never claimed as ML vocal)
    MusicVocalModel,   // Music-specific vocal detector model
    ExternalStem       // Pre-separated vocal stem
};

[[nodiscard]] const char* vocalDetectorKindName(VocalDetectorKind kind) noexcept;

enum class CueRole : std::uint8_t {
    MixOut = 0,
    MixIn = 1
};

enum class CueKind : std::uint8_t {
    AudibleBoundary = 0,
    Downbeat,
    PhraseBoundary,
    SectionBoundary,
    BreakdownEntry,
    EnergyPeakOrDrop,
    OutroStart,
    SilenceEdge
};

[[nodiscard]] const char* cueKindName(CueKind kind) noexcept;

struct MixFeatureConfig {
    // Frame step for the time-aligned feature series (0.10 s = 10 Hz default).
    double frameStepSeconds = 0.10;
    double lowBandMaxHz = 180.0;
    double midBandMaxHz = 3500.0;

    // Adaptive audible start/end hysteresis relative to estimated noise floor
    // and active reference level (never a global hard-coded -55 dBFS cut).
    double adaptiveMarginAboveNoiseDb = 9.0;
    double adaptiveHysteresisDb = 4.5;

    // Candidate cue search bounds.
    int maxMixOutCandidates = 12;
    int maxMixInCandidates = 12;

    // Optional lightweight spectral proxy or vocal/harmonic passes (PR 2).
    VocalDetectorKind vocalDetector = VocalDetectorKind::None;
    std::uint32_t vocalModelVersion = 0;
    std::uint64_t vocalModelChecksum = 0;
    std::uint32_t harmonicAnalyzeVersion = 0;

    [[nodiscard]] std::uint64_t fingerprint() const noexcept;
};

struct MixFeatureInfo {
    std::uint32_t schemaVersion = kMixFeatureSchemaVersion;
    std::uint32_t extractorVersion = kMixFeatureExtractorVersion;
    std::uint32_t analyzerVersion = kAnalyzerVersion;
    std::uint64_t contentHash = 0;
    std::uint64_t analysisConfigHash = 0;
    std::uint64_t featureConfigHash = 0;
    VocalDetectorKind vocalDetectorKind = VocalDetectorKind::None;
    std::uint32_t vocalModelVersion = 0;
    std::uint64_t vocalModelChecksum = 0;
    std::uint32_t harmonicAnalyzeVersion = 0;
    double duration = 0.0;
    std::uint32_t sampleRate = 0;
    std::uint32_t sourceChannels = 0;
    double frameStepSeconds = 0.10;
    // True when extracted from PCM + AnalysisResult; false when synthesized
    // as a deterministic downgrade fallback from AnalysisResult alone.
    bool fromPcm = false;
};

struct LoudnessSummary {
    // BS.1770-4 compatible K-weighted loudness (distinguished from plain RMS).
    float integratedLufs = -70.0f;
    float shortTermMaxLufs = -70.0f;
    float momentaryMaxLufs = -70.0f;
    float loudnessRangeLu = 0.0f;
    // Plain unweighted RMS and sample peak in dBFS.
    float meanRmsDbfs = -90.0f;
    float samplePeakDbfs = -90.0f;
    bool gatedValid = false;
};

struct AudibleBounds {
    double audibleStart = 0.0;
    double fullBodyStart = 0.0;
    double tailStart = 0.0;
    double audibleEnd = 0.0;
    float noiseFloorDbfs = -90.0f;
    float referenceDbfs = -14.0f;
    float enterThresholdDbfs = -60.0f;
    float exitThresholdDbfs = -64.5f;
    bool hasFadeIn = false;
    bool hasFadeOut = false;
    bool hasReverbTail = false;
    float confidence = 0.0f;
};

struct MixFeatureFrame {
    double time = 0.0;
    float rms = 0.0f;
    float momentaryLufs = -70.0f;
    float shortTermLufs = -70.0f;
    float energy = 0.0f;         // 0..1 relative to track's 95th percentile
    float lowEnergy = 0.0f;      // linear band power < lowBandMaxHz
    float midEnergy = 0.0f;      // linear band power [lowBandMaxHz, midBandMaxHz]
    float highEnergy = 0.0f;     // linear band power > midBandMaxHz
    float lowShare = 0.0f;       // 0..1 share of frame spectral power
    float midShare = 0.0f;       // 0..1 share of frame spectral power
    float highShare = 0.0f;      // 0..1 share of frame spectral power
    float bassDominance = 0.0f;  // 0..1 low-end weight + bass accent support
    float onsetDensity = 0.0f;   // 0..1 local percussion/onset activity
    float spectralNovelty = 0.0f;// 0..1 local timbral/spectral change
    float boundaryStrength = 0.0f;// 0..1 combined novelty + energy + rhythm boundary
    // Forward-compatible slots populated by PR 2 vocal/harmonic analyzers.
    // When vocalDetectorKind == None, vocalConfidence is 0.0 so callers never
    // confuse "unknown vocal state" with "verified instrumental".
    float vocalScore = 0.0f;
    float vocalConfidence = 0.0f;
    float harmonicSalience = 0.0f;
    std::int8_t dominantPitchClass = -1;
    bool active = false;
};

struct BarFeatureSummary {
    int barIndex = 0;
    double start = 0.0;
    double end = 0.0;
    int beatCount = 0;
    double downbeatTime = 0.0;
    float downbeatConfidence = 0.0f;
    float meanEnergy = 0.0f;
    float momentaryLufs = -70.0f;
    float lowShare = 0.0f;
    float midShare = 0.0f;
    float highShare = 0.0f;
    float bassDominance = 0.0f;
    float onsetDensity = 0.0f;
    float spectralNovelty = 0.0f;
    float boundaryStrength = 0.0f;
    float vocalScore = 0.0f;
    float vocalConfidence = 0.0f;
    SectionKind sectionKind = SectionKind::Unknown;
};

struct PhraseSpan {
    double start = 0.0;
    double end = 0.0;
    int barStart = 0;
    int barCount = 0;
    int beatCount = 0;
    SectionKind sectionKind = SectionKind::Unknown;
    float meanEnergy = 0.0f;
    float energySlope = 0.0f;
    float bassDominance = 0.0f;
    float boundaryStrengthStart = 0.0f;
    float boundaryStrengthEnd = 0.0f;
    float confidence = 0.0f;
    bool regularMeter = true;
};

struct CuePointCandidate {
    double time = 0.0;
    int barIndex = -1;
    int beatIndex = -1;
    CueRole role = CueRole::MixIn;
    CueKind kind = CueKind::Downbeat;
    SectionKind sectionKind = SectionKind::Unknown;
    float boundaryStrength = 0.0f;
    float rhythmConfidence = 0.0f;
    float phraseConfidence = 0.0f;
    float energyBefore = 0.0f;
    float energyAfter = 0.0f;
    float bassDominance = 0.0f;
    double availableWindowSeconds = 0.0;
    float vocalBefore = 0.0f;
    float vocalAfter = 0.0f;
    float vocalConfidence = 0.0f;
    float harmonicStability = 0.0f;
    float priorScore = 0.0f;
    std::string reason;
};

struct VocalRegion {
    double start = 0.0;
    double end = 0.0;
    float score = 0.0f;
    float confidence = 0.0f;
    bool leadLikely = false;
};

struct VocalTrackSummary {
    bool available = false;
    VocalDetectorKind detectorKind = VocalDetectorKind::None;
    std::uint32_t modelVersion = 0;
    std::uint64_t modelChecksum = 0;
    float meanConfidence = 0.0f;
    float activeRatio = 0.0f;
    std::vector<VocalRegion> regions;
};

struct HarmonicSegment {
    double start = 0.0;
    double end = 0.0;
    int keyIndex = -1; // -1 = unknown, 0..11 = C..B
    bool isMinor = false;
    float confidence = 0.0f;
    float tonalness = 0.0f;
    std::array<float, 12> chroma{};
};

struct HarmonicTrackSummary {
    bool available = false;
    int keyIndex = -1;
    bool isMinor = false;
    std::string camelotCode; // Presentation helper only, never a hard rule
    float keyConfidence = 0.0f;
    float tonalness = 0.0f;
    std::array<float, 12> globalChroma{};
    std::vector<HarmonicSegment> segments;
};

struct WindowFeatureSummary {
    double start = 0.0;
    double end = 0.0;
    double duration = 0.0;
    float meanLufs = -70.0f;
    float peakLufs = -70.0f;
    float meanRms = 0.0f;
    float meanEnergy = 0.0f;
    float energySlope = 0.0f;
    float lowShare = 0.0f;
    float midShare = 0.0f;
    float highShare = 0.0f;
    float bassDominance = 0.0f;
    float onsetDensity = 0.0f;
    float spectralNovelty = 0.0f;
    float activeRatio = 0.0f;
    float vocalScore = 0.0f;
    float vocalConfidence = 0.0f;
    float harmonicSalience = 0.0f;
    double localBpm = 0.0;
    bool tempoStable = true;
    SectionKind dominantSection = SectionKind::Unknown;
    float confidence = 0.0f;
};

struct MixFeatureMap {
    MixFeatureInfo info;
    LoudnessSummary loudness;
    AudibleBounds audible;

    std::vector<MixFeatureFrame> frames;
    std::vector<BarFeatureSummary> bars;
    std::vector<PhraseSpan> phrases;
    std::vector<CuePointCandidate> mixOutCandidates;
    std::vector<CuePointCandidate> mixInCandidates;

    VocalTrackSummary vocal;
    HarmonicTrackSummary harmony;

    // Preserved context from BG Beat so window queries can evaluate local
    // tempo stability and sections without re-running BeatTracker/TempoEstimator.
    double globalBpm = 0.0;
    float bpmConfidence = 0.0f;
    float beatPhaseConfidence = 0.0f;
    float downbeatConfidence = 0.0f;
    int beatsPerBar = 4;
    float meterConfidence = 0.0f;
    bool tempoStable = true;
    std::vector<TempoSegment> tempoSegments;
    std::vector<TempoTransition> tempoTransitions;
    std::vector<Section> sections;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return info.duration <= 0.0 || frames.empty(); }

    // Summarizes the time interval [start, end] in source-media seconds.
    [[nodiscard]] WindowFeatureSummary summarizeWindow(double start, double end) const noexcept;

    // Interpolates or fetches the nearest frame at source-media time `t`.
    [[nodiscard]] MixFeatureFrame sampleAt(double time) const noexcept;

    // Local tempo at `time` from BG Beat's tempoSegments/tempoTransitions.
    [[nodiscard]] double localBpmAt(double time, bool* stableOut = nullptr) const noexcept;

    // Section containing `time` (Intro/Build/Main/Breakdown/Outro/Unknown).
    [[nodiscard]] SectionKind sectionAt(double time, float* confidenceOut = nullptr) const noexcept;
};

class MixFeatureExtractor {
public:
    explicit MixFeatureExtractor(MixFeatureConfig config = {});

    [[nodiscard]] const MixFeatureConfig& config() const noexcept { return config_; }

    // Full extraction from mono PcmBuffer + BG Beat AnalysisResult.
    [[nodiscard]] MixFeatureMap extract(const PcmBuffer& pcm,
                                        const AnalysisResult& analysis) const;

    // Convenience overload for mixer PcmTrack (downmixes to mono for analysis
    // while preserving contentHash from AnalysisResult when non-zero).
    [[nodiscard]] MixFeatureMap extractFromTrack(const PcmTrack& track,
                                                 const AnalysisResult& analysis) const;

    // Deterministic downgrade fallback built from AnalysisResult alone (used
    // when callers only have AnalysisResult, e.g. legacy 0.2 planner callers).
    [[nodiscard]] MixFeatureMap buildFromAnalysisOnly(
        const AnalysisResult& analysis) const;

private:
    MixFeatureConfig config_;
};

} // namespace bgmedia::mix
