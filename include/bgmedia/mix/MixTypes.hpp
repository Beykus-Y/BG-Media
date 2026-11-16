#pragma once

// BG Mix 0.1 - public value types shared by the planner, the dual-deck engine
// and hosts. This header is deliberately free of SDL, miniaudio and any UI
// dependency so the mixer can be embedded in other applications.

#include <cstdint>
#include <string>
#include <vector>

namespace bgmedia::mix {

enum class DeckId : std::uint8_t {
    A = 0,
    B = 1
};

[[nodiscard]] constexpr DeckId otherDeck(DeckId deck) noexcept {
    return deck == DeckId::A ? DeckId::B : DeckId::A;
}

[[nodiscard]] constexpr int deckIndex(DeckId deck) noexcept {
    return deck == DeckId::A ? 0 : 1;
}

// Transition types. Every value has an explicit implementation status: the UI
// must not present an unimplemented mode as a working feature.
enum class TransitionType : std::uint8_t {
    Off = 0,
    Crossfade,
    SmartCrossfade,
    Beatmatched,
    PhraseAware,
    TempoRamp,
    DualTempoMatch,
    BreakdownBridge,
    DirectCut
};

enum class ImplementationStatus : std::uint8_t {
    Working,
    Experimental,
    NotImplemented
};

// How the planner may use a pre-transition tempo ramp. Manual still requires
// the caller to request TempoRamp/Beatmatched; Auto lets the adaptive planner
// choose it when the material supports it.
enum class TempoRampMode : std::uint8_t {
    Off = 0,
    Auto,
    Manual
};

enum class TempoCurveKind : std::uint8_t {
    Linear = 0,
    Smooth,
    SCurve
};

enum class Aggressiveness : std::uint8_t {
    Gentle = 0,
    Balanced,
    Energetic
};

// Time-stretch quality mode. Quality uses WSOLA and preserves pitch; the
// performance mode is plain linear resampling, where pitch follows tempo.
enum class StretchQuality : std::uint8_t {
    Quality = 0,
    Performance
};

enum class MixMode : std::uint8_t {
    Auto = 0,   // automatic scheduling + adaptive choice
    Manual,     // explicit type, manual start
    Preview     // plan/preview only, never auto-start
};

[[nodiscard]] ImplementationStatus transitionStatus(TransitionType type) noexcept;
[[nodiscard]] const char* transitionTypeName(TransitionType type) noexcept;
[[nodiscard]] const char* transitionTypeShortName(TransitionType type) noexcept;
[[nodiscard]] const char* transitionStatusLabel(TransitionType type) noexcept;
// Types the UI may offer as setup choices (working + experimental).
[[nodiscard]] std::vector<TransitionType> selectableTransitionTypes();

// Beat-rate relationship between the two decks over the same output time:
// `bBeats` incoming beats are played for every `aBeats` outgoing beats. It is
// independent of the audio playback-rate correction (tempoRatioA/B): 60 -> 120
// BPM is 2:1 with both decks at 1.0x. The two concepts must never be derived
// from each other by dividing metric-alternative BPM candidates.
struct MetricRelation {
    int bBeats = 1;
    int aBeats = 1;

    [[nodiscard]] constexpr double ratio() const noexcept {
        return aBeats > 0 ? static_cast<double>(bBeats) / aBeats : 1.0;
    }
    [[nodiscard]] constexpr bool unison() const noexcept {
        return bBeats == aBeats;
    }
    // 1:1, 2:1 and 1:2 are ordinary metric levels; anything else (3:2, 2:3)
    // is a polyrhythm and stays opt-in/experimental.
    [[nodiscard]] constexpr bool polyrhythm() const noexcept {
        return !(unison() || bBeats == 2 * aBeats || aBeats == 2 * bBeats);
    }
    [[nodiscard]] constexpr bool operator==(
        const MetricRelation& other) const noexcept {
        return bBeats == other.bBeats && aBeats == other.aBeats;
    }
};

// How the tempo of the two decks was reconciled. Surfaced to the UI so an
// experimental or fallback result is never presented as a plain beatmatch.
enum class TempoMatchKind : std::uint8_t {
    None = 0,                // no tempo sync (plain crossfade, direct cut)
    ExactMetric,             // metric relation holds at 1.0x on both decks
    MetricWithStretch,       // metric relation + bounded residual rate change
    ExperimentalPolyrhythm,  // opt-in 3:2 / 2:3
    FallbackCrossfade        // tempo sync was wanted but rejected
};

[[nodiscard]] const char* tempoMatchKindName(TempoMatchKind kind) noexcept;

// BG Mix 0.3 high-level musical transition strategy selected by the planner.
enum class MixStrategyKind : std::uint8_t {
    SmoothBlend = 0,   // Phrase/beat-aligned smooth blend with adaptive bass swap
    VocalHandoff,      // Clean vocal handoff / mid-EQ vocal carving
    DropImpactCut,     // Short impact cut at a strong accent/drop boundary
    EnergyBridge,      // Low-energy / breakdown bridge between tracks
    EchoReverbOut      // Outgoing tail / phrase ending with echo/reverb wash-out
};

[[nodiscard]] const char* mixStrategyKindName(MixStrategyKind kind) noexcept;
[[nodiscard]] const char* mixStrategyKindShortName(MixStrategyKind kind) noexcept;

// Transparent multi-objective score breakdown for a transition candidate.
struct MixCandidateScore {
    float totalScore = 0.0f;           // 0..1 weighted overall score
    float phraseBoundaryScore = 0.0f;  // 0..1 phrase/bar/section alignment
    float rhythmBeatScore = 0.0f;      // 0..1 tempo compatibility + beat phase
    float vocalSafetyScore = 0.0f;     // 0..1 freedom from double-vocal clash + clean handoff
    float harmonicScore = 0.0f;        // 0..1 chroma/key compatibility + low spectral crowding
    float energyContinuityScore = 0.0f;// 0..1 energy continuity across handover
    float loudnessBalanceScore = 0.0f; // 0..1 LUFS balance + headroom safety
    float durationFitScore = 0.0f;     // 0..1 structural duration fit
    float continuationScore = 1.0f;    // 0..1 completed phrase/outro vs premature exit from active drop

    float vocalClashRisk = 0.0f;
    float harmonicClashRisk = 0.0f;
    float spectralCrowding = 0.0f;
    bool cutsVocalPhraseA = false;
    bool entersVocalPhraseB = false;
    bool cleanVocalHandoff = false;
    bool severeVocalClash = false;
    bool prematureDropExit = false;
    std::string summary;
};

// Ranked candidate evaluated by the BG Mix 0.3 multi-candidate transition planner.
struct MixCandidate {
    int rank = 0; // 1 = selected winner, 2..N = alternatives
    TransitionType type = TransitionType::SmartCrossfade;
    MixStrategyKind strategy = MixStrategyKind::SmoothBlend;
    double transitionStartA = 0.0;
    double transitionEndA = 0.0;
    double tempoRampStartA = -1.0;
    double startB = 0.0;
    double endB = 0.0;
    double duration = 0.0;
    int durationBars = 0;
    double bassSwapProgress = 0.35;
    bool vocalEqCarve = false;
    float loudnessTrimDbA = 0.0f;
    float loudnessTrimDbB = 0.0f;
    MixCandidateScore score;
    std::string rationale;
};

struct TempoPoint {
    // Seconds from the crossfade start. Negative points belong to the
    // pre-transition tempo ramp, which is exactly the 0.2 behaviour: the
    // outgoing deck reaches the incoming tempo before the overlay begins.
    double time = 0.0;
    double rate = 1.0;   // playback speed of the outgoing deck
    double bpm = 0.0;    // resulting local BPM of the outgoing deck
};

struct GainPoint {
    double time = 0.0;
    float gain = 0.0f;
};

// Independent three-band automation point. Time is relative to the crossfade
// start; gains are linear (1.0 = unity). The engine runs a phase-flat
// complementary crossover, so equal gains are an exact bypass.
struct EqGainPoint {
    double time = 0.0;
    float low = 1.0f;
    float mid = 1.0f;
    float high = 1.0f;
};

// A deterministic, explainable transition plan. The planner never hides a
// fallback: `fallbackReason` is non-empty whenever the requested intelligent
// mode could not be used.
struct MixPlan {
    bool valid = false;
    TransitionType type = TransitionType::Crossfade;
    TransitionType requestedType = TransitionType::Crossfade;
    DeckId from = DeckId::A;
    DeckId to = DeckId::B;

    double duration = 0.0;          // crossfade seconds (excludes pre-roll)
    double preTransitionSeconds = 0.0;  // tempo-ramp seconds before the overlay
    double transitionStartA = 0.0;  // outgoing media time when the crossfade starts
    double transitionEndA = 0.0;    // outgoing media time when the crossfade ends
    double tempoRampStartA = -1.0;  // outgoing media time where the ramp starts
    double startB = 0.0;            // incoming media time at mix start
    double endB = 0.0;              // incoming media time at mix end

    double bpmA = 0.0;
    double bpmB = 0.0;
    double targetBpm = 0.0;
    double localBpmA = 0.0;
    double tempoRampPercent = 0.0;
    double tempoRatioAStart = 1.0;
    double tempoRatioAEnd = 1.0;
    double tempoRatioB = 1.0;

    // Metric-relative matching. `targetBpm` is kept as the incoming (B) BPM
    // the pair meets at; `targetBpmA` is the outgoing equivalent. The two are
    // related by `metricRelation`, not necessarily equal (60 -> 120 BPM).
    MetricRelation metricRelation;
    TempoMatchKind tempoMatchKind = TempoMatchKind::None;
    double targetBpmA = 0.0;

    bool beatAligned = false;
    bool downbeatAligned = false;
    double alignmentErrorMs = 0.0;      // grid snap error at the cue
    double beatPhaseErrorMs = 0.0;      // residual B-vs-A phase after matching
    double handoverProgress = 0.5;  // where the musical master moves to B
    // Where the incoming deck starts returning from its matched rate to 1.0x.
    // Kept late so both decks stay phase-locked while both are audible; the
    // outgoing deck is already well below -12 dB by then.
    double rateReleaseProgress = 0.85;

    // Measured two-track beat-phase residual over the audible overlap, from
    // the real detected beats of both tracks mapped through the actual deck
    // rates. `phaseMeasured` is false when too few beats exist to judge.
    bool phaseMeasured = false;
    int phasePairs = 0;
    double phaseRmsMs = 0.0;
    double phaseP95Ms = 0.0;
    double phaseDriftMs = 0.0;  // |mean error last third - first third|

    bool tempoRampActive = false;
    bool pitchLock = true;
    bool bassSwap = false;
    double bassSwapProgress = 0.35; // 0..1 normalized crossfade progress for bass handover
    bool vocalEqCarve = false;      // mid-band vocal carving active during vocal overlap
    int phraseBars = 0;
    bool phraseBoundary = false;
    float phraseConfidence = 0.0f;

    // BG Mix 0.3 strategy, loudness normalization, tail processor, and candidate scores.
    MixStrategyKind strategy = MixStrategyKind::SmoothBlend;
    float loudnessTrimDbA = 0.0f;   // dB gain trim applied to outgoing track (role from)
    float loudnessTrimDbB = 0.0f;   // dB gain trim applied to incoming track (role to)
    float loudnessGainA = 1.0f;     // linear gain multiplier for outgoing track (role from, headroom-clamped)
    float loudnessGainB = 1.0f;     // linear gain multiplier for incoming track (role to, headroom-clamped)
    float tailSendAmount = 0.0f;    // 0..1 echo/reverb tail send for EchoReverbOut
    double tailDecaySeconds = 0.0;  // tail decay length in seconds
    MixCandidateScore scoreBreakdown;
    std::vector<MixCandidate> alternatives;

    std::vector<TempoPoint> tempoCurve;
    std::vector<GainPoint> gainCurveOut;
    std::vector<GainPoint> gainCurveIn;
    std::vector<EqGainPoint> eqCurveOut;
    std::vector<EqGainPoint> eqCurveIn;

    float confidence = 0.0f;
    bool experimental = false;
    bool pitchShiftWarning = false;
    std::string fallbackReason;
    std::vector<std::string> reasons;

    [[nodiscard]] bool crossfades() const noexcept {
        return type != TransitionType::DirectCut &&
               type != TransitionType::Off;
    }

    [[nodiscard]] double totalDuration() const noexcept {
        return preTransitionSeconds + duration;
    }

    // Media time where the overall transition (ramp + crossfade) begins.
    [[nodiscard]] double overallStartA() const noexcept {
        return tempoRampStartA >= 0.0 ? tempoRampStartA : transitionStartA;
    }
};

struct PlannerConfig {
    bool adaptive = true;
    // Kept for 0.1 API compatibility. true enables the ramp unless
    // tempoRampMode is explicitly Off.
    bool allowTempoRamp = true;
    TempoRampMode tempoRampMode = TempoRampMode::Auto;
    TempoCurveKind tempoCurveKind = TempoCurveKind::SCurve;
    double maxTempoDeltaPercent = 6.0;
    double tempoRampBars = 8.0;
    double defaultCrossfadeSeconds = 8.0;
    double minCrossfadeSeconds = 2.0;
    double maxCrossfadeSeconds = 16.0;
    double minConfidence = 0.45;
    bool trustDownbeats = true;
    bool preferPhraseBoundaries = true;
    bool phraseAlignment = true;
    bool bassSwap = true;
    bool pitchLock = true;
    Aggressiveness aggressiveness = Aggressiveness::Balanced;
    int phraseBars = 4;  // musical phrase length in bars (4 or 8 typical)
    // Residual beat-phase limits for tempo-synced transitions. Exceeding the
    // p95 limit makes the planner fall back to a plain crossfade instead of
    // pretending the decks are aligned.
    double maxPhaseP95Ms = 60.0;
    // Octave-related metric levels (2:1 / 1:2) may be beatmatched at 1.0x when
    // 1:1 is out of range. Needs trustworthy level/phase confidence.
    bool allowMetricRelations = true;
    double minMetricConfidence = 0.5;
    // Opt-in only: 3:2 and 2:3 polyrhythms. Never enabled automatically.
    bool allowPolyrhythm = false;

    // Effective ramp enable: Off wins over the legacy allowTempoRamp flag.
    [[nodiscard]] bool rampEnabled() const noexcept {
        return tempoRampMode != TempoRampMode::Off && allowTempoRamp;
    }

    static PlannerConfig forTransition(TransitionType type);
};

// Immutable description of one deck, as published by the engine. It is a POD
// so hosts can read it without touching engine internals.
struct DeckStatus {
    bool loaded = false;
    bool playing = false;
    bool ended = false;
    std::uint64_t trackId = 0;
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    double rate = 1.0;
    double bpm = 0.0;          // effective BPM = analysed BPM * rate
    double analysedBpm = 0.0;
    float gain = 0.0f;
    float peak = 0.0f;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t seekApplied = 0;
};

enum class TransitionPhase : std::uint8_t {
    Idle,
    Running,
    Cancelling,
    Completed
};

// Musical stage inside a running transition. The engine deliberately keeps
// only three transport phases (Idle/Running/Cancelling) and publishes the
// finer stage for the UI and BG Sync.
enum class TransitionStage : std::uint8_t {
    Idle = 0,
    Preparing,
    TempoRamp,
    BeatAlign,
    Mixing,
    Handover,
    Complete,
    Cancelled
};

struct MixSnapshot {
    DeckStatus decks[2];
    DeckId master = DeckId::A;
    TransitionPhase phase = TransitionPhase::Idle;
    TransitionStage stage = TransitionStage::Idle;
    TransitionType transitionType = TransitionType::Off;
    MixStrategyKind strategy = MixStrategyKind::SmoothBlend;
    double transitionProgress = 0.0;
    double transitionDuration = 0.0;
    double transitionElapsed = 0.0;
    double crossfadeDuration = 0.0;
    double crossfadeElapsed = 0.0;
    double preTransitionSeconds = 0.0;
    double tempoRatioA = 1.0;
    double tempoRatioB = 1.0;
    double beatPhaseErrorMs = 0.0;
    bool pitchLock = false;
    bool bassSwap = false;
    float eqLow[2] = {1.0f, 1.0f};
    float eqMid[2] = {1.0f, 1.0f};
    float eqHigh[2] = {1.0f, 1.0f};
    float loudnessGain[2] = {1.0f, 1.0f};
    float tailWetLevel = 0.0f;
    std::uint64_t completedTransitions = 0;
    std::uint64_t outputFrames = 0;
    double sampleRate = 48000.0;
    double outputLatencySeconds = 0.0;
    float outputPeak = 0.0f;
    float limiterGain = 1.0f;
    bool paused = false;
};

// A queued track's public state, used by the BG Mix UI.
enum class QueueState : std::uint8_t {
    Pending,
    Preparing,
    Ready,
    Failed,
    Playing,
    Played
};

struct QueueItemStatus {
    std::uint64_t id = 0;
    std::string displayName;
    std::string path;
    QueueState state = QueueState::Pending;
    double bpm = 0.0;
    double durationSeconds = 0.0;
    float progress = 0.0f;
    std::string error;
    bool experimentalTempo = false;
};

// Session-level snapshot: queue + deck + plan, safe to copy into the UI.
struct MixSessionSnapshot {
    MixSnapshot engine;
    std::vector<QueueItemStatus> queue;
    // Capacities of prepared queue buffers and unpublished completed jobs;
    // retired deck ownership reported separately (may overlap with queue buffers).
    std::uint64_t preparedPcmBytes = 0;
    std::uint64_t retiredPcmBytes = 0;
    std::uint32_t preparedCount = 0;
    std::uint32_t preparingCount = 0;
    bool transitionAvailable = false;
    bool preparingNext = false;
    std::string preparingName;
    float preparingProgress = 0.0f;
    MixPlan preview;
    MixPlan executed;
    bool hasExecutedPlan = false;
    std::string status;
    std::string lastError;
};

} // namespace bgmedia::mix
