#pragma once

#include "bgmedia/analysis/AnalysisResult.hpp"

#include <cstdint>
#include <vector>

namespace bgmedia {

// Existing moves retain their original values. New BG Motion 2.0 moves are
// appended so code that stores MoveKind numerically keeps working.
enum class MoveKind {
    Idle,
    Pulse,
    Bounce,
    Sway,
    ArmWave,
    Spin,
    Pose,
    Freeze,
    HeadNod,
    BassDip,
    AccentPose
};

// Interpolation profile for a keyframe segment. Cubic preserves position and
// velocity through neighbouring keyframes (C1); the other profiles are useful
// for custom animation tracks and deliberately different timing.
enum class Easing {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    SmoothStep,
    Cubic
};

struct DanceCue {
    double time = 0.0;
    double duration = 0.0;
    MoveKind move = MoveKind::Idle;
    float intensity = 0.0f;
    int beatIndex = -1;
    int barIndex = -1;
    bool bassAccent = false;

    // BG Motion 2.0 scheduling metadata. targetTime is the musical accent the
    // cue is preparing for; anticipationTime is where preparation begins.
    double targetTime = 0.0;
    double anticipationTime = 0.0;
    Easing easing = Easing::Cubic;
};

// Backend-independent pose. Values are deliberately normalised so 2D sprites,
// skeletal rigs and future avatar formats can consume the same choreography.
// Rotations are measured in turns and interpolated on the shortest arc.
struct CharacterPose {
    float bob = 0.0f;         // -1..1 vertical motion
    float lean = 0.0f;        // -1..1
    float armLeft = 0.0f;     // -1..1 shoulder/arm swing
    float armRight = 0.0f;
    float legLeft = 0.0f;     // -1..1 hip swing
    float legRight = 0.0f;
    float rotation = 0.0f;    // -1..1 turns; shortest-arc interpolation
    float energy = 0.0f;      // 0..1

    // Existing fields above keep their original positional order for source
    // compatibility with the first BG Motion release.
    MoveKind move = MoveKind::Idle;
    bool accented = false;

    // BG Motion 2.0 joint detail. All values are normalised -1..1 unless noted.
    float headNod = 0.0f;     // negative = chin down, positive = chin up
    float headTilt = 0.0f;
    float headTurn = 0.0f;
    float chestTwist = 0.0f;
    float hipShift = 0.0f;
    float kneeLeft = 0.0f;    // 0 = straight, 1 = deeply bent
    float kneeRight = 0.0f;
    float elbowLeft = 0.0f;
    float elbowRight = 0.0f;
    float footLeft = 0.0f;    // 0..1 lift, used for step-like accents
    float footRight = 0.0f;

    // Debug/consumer metadata, not part of the physical pose.
    float phase = 0.0f;       // 0..1 inside the current beat
    float blend = 1.0f;       // 0..1 movement blend amount
    float impact = 0.0f;      // 0..1 accent overlay weight
};

struct ChoreographyStyle {
    float intensity = 1.0f;
    float bassInfluence = 0.45f;

    // Fraction of a beat used to cross-fade the previous bar's movement into
    // the next one. The blend ends on the bar boundary, so the accented pose
    // at the boundary belongs to the new movement.
    float movementBlend = 0.35f;

    // Accent preparation / release, as fractions of the local beat duration.
    float anticipation = 0.42f;
    float accentRelease = 0.55f;

    // At fast tempos the same geometric amplitude would require very high
    // angular speed. BG Motion reduces amplitude instead of accelerating.
    bool reduceAmplitudeAtFastTempo = true;
};

struct MotionKeyframe {
    double time = 0.0;
    CharacterPose pose;
    Easing easingToNext = Easing::Cubic;
    bool accent = false;
    int beatIndex = -1;
};

struct MotionClip {
    MoveKind move = MoveKind::Idle;
    double start = 0.0;
    double end = 0.0;
    double anticipationStart = 0.0;
    double accentTime = -1.0;
    float intensity = 0.0f;
    int beatIndex = -1;
    int barIndex = -1;
    bool bassAccent = false;
    Easing easing = Easing::Cubic;
};

// Target pose for a musical beat. It is sampled exactly at `time`, which is
// also stored in the keyframe timeline. This is what the tests use to prove
// that the planned accent pose lands on the musical event.
struct AccentTarget {
    double time = 0.0;
    CharacterPose pose;
    MoveKind move = MoveKind::Idle;
    int beatIndex = -1;
    int barIndex = -1;
    bool bassAccent = false;
    bool downbeat = false;
    float strength = 0.0f;
};

struct Choreography {
    // Legacy per-beat cues remain available for the timeline and for stepped
    // comparison rendering.
    std::vector<DanceCue> cues;

    // BG Motion 2.0 timeline. keyframes is the authoritative smooth timeline;
    // clips and accents are diagnostic/authoring data.
    std::vector<MotionKeyframe> keyframes;
    std::vector<MotionClip> clips;
    std::vector<AccentTarget> accents;

    int beatsPerBar = 4;
    std::uint32_t analyzerVersion = 0;
    double duration = 0.0;
    double bpm = 0.0;
    bool smoothed = true;
};

struct MotionSample {
    CharacterPose pose;
    int keyframeIndex = -1;
    int nextKeyframeIndex = -1;
    double keyframeTime = 0.0;
    double nextKeyframeTime = 0.0;
    float segmentT = 0.0f;
    float movementBlend = 1.0f;
    Easing easing = Easing::Cubic;
    MoveKind move = MoveKind::Idle;
    bool accent = false;
};

// Pure choreography calculation: consumes an AnalysisResult and produces a
// deterministic keyframed animation. No rendering, no SDL, no clock.
class ChoreographyBuilder {
public:
    [[nodiscard]] Choreography build(const AnalysisResult& analysis,
                                     ChoreographyStyle style = {}) const;
};

class ChoreographySampler {
public:
    // Smooth BG Motion 2.0 sampling. The result depends only on choreography
    // and mediaSeconds, never on render FPS or call order.
    [[nodiscard]] CharacterPose sample(const Choreography& choreography,
                                       double mediaSeconds) const;

    // Same sample plus keyframe/blend diagnostics for the desktop debug view.
    [[nodiscard]] MotionSample sampleWithDebug(const Choreography& choreography,
                                               double mediaSeconds) const;

    // Legacy stepped sampler retained for A/B comparison and compatibility.
    [[nodiscard]] CharacterPose sampleStepped(const Choreography& choreography,
                                              double mediaSeconds) const;

    [[nodiscard]] const DanceCue* activeCue(const Choreography& choreography,
                                            double mediaSeconds) const;

    [[nodiscard]] static std::size_t cueCount(const Choreography& choreography) {
        return choreography.cues.size();
    }
};

} // namespace bgmedia
