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

[[nodiscard]] ImplementationStatus transitionStatus(TransitionType type) noexcept;
[[nodiscard]] const char* transitionTypeName(TransitionType type) noexcept;
[[nodiscard]] const char* transitionTypeShortName(TransitionType type) noexcept;
[[nodiscard]] const char* transitionStatusLabel(TransitionType type) noexcept;
// Types the UI may offer as setup choices (working + experimental).
[[nodiscard]] std::vector<TransitionType> selectableTransitionTypes();

struct TempoPoint {
    double time = 0.0;   // seconds from the start of the transition
    double rate = 1.0;   // playback speed of the outgoing deck
    double bpm = 0.0;    // resulting local BPM of the outgoing deck
};

struct GainPoint {
    double time = 0.0;
    float gain = 0.0f;
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

    double duration = 0.0;          // seconds
    double transitionStartA = 0.0;  // outgoing media time when the mix starts
    double transitionEndA = 0.0;    // outgoing media time when the mix ends
    double startB = 0.0;            // incoming media time at mix start
    double endB = 0.0;              // incoming media time at mix end

    double bpmA = 0.0;
    double bpmB = 0.0;
    double targetBpm = 0.0;
    double localBpmA = 0.0;
    double tempoRampPercent = 0.0;

    bool beatAligned = false;
    bool downbeatAligned = false;
    double alignmentErrorMs = 0.0;
    double handoverProgress = 0.5;  // where the musical master moves to B

    std::vector<TempoPoint> tempoCurve;
    std::vector<GainPoint> gainCurveOut;
    std::vector<GainPoint> gainCurveIn;

    float confidence = 0.0f;
    bool experimental = false;
    bool pitchShiftWarning = false;
    std::string fallbackReason;
    std::vector<std::string> reasons;

    [[nodiscard]] bool crossfades() const noexcept {
        return type != TransitionType::DirectCut &&
               type != TransitionType::Off;
    }
};

struct PlannerConfig {
    bool adaptive = true;
    bool allowTempoRamp = false;      // experimental, off by default
    double maxTempoDeltaPercent = 6.0;
    double tempoRampBars = 8.0;
    double defaultCrossfadeSeconds = 8.0;
    double minCrossfadeSeconds = 2.0;
    double maxCrossfadeSeconds = 16.0;
    double minConfidence = 0.45;
    bool trustDownbeats = true;
    bool preferPhraseBoundaries = true;

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
};

enum class TransitionPhase : std::uint8_t {
    Idle,
    Running,
    Cancelling,
    Completed
};

struct MixSnapshot {
    DeckStatus decks[2];
    DeckId master = DeckId::A;
    TransitionPhase phase = TransitionPhase::Idle;
    TransitionType transitionType = TransitionType::Off;
    double transitionProgress = 0.0;
    double transitionDuration = 0.0;
    double transitionElapsed = 0.0;
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
    bool transitionAvailable = false;
    bool preparingNext = false;
    std::string preparingName;
    float preparingProgress = 0.0f;
    MixPlan preview;
    std::string status;
    std::string lastError;
};

} // namespace bgmedia::mix
