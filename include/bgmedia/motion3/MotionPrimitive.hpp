#pragma once

// BG Motion 3.2 - reusable, parameterised motion primitives.
//
// A primitive is not a pose preset and not a second dance clip. It is a small
// procedural motion with explicit metadata (body mask, duration in musical
// beats, joint set, support-foot requirements, accent affinity, priority and
// blend mode). The compositor combines primitives with a base choreography on
// the musical grid, rejects mechanically incompatible overlaps and applies the
// result with quaternion composition. Nothing here knows about a particular
// VRM model.
//
// All values are expressed in the same "dance frame" as DanceClip: +X is the
// character's left, +Y is up, +Z is forward, rotations are local quaternions
// relative to the bind pose and lengths are fractions of the model height.

#include "bgmedia/motion3/DanceClip.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bgmedia::motion {

using BodyMask = std::uint32_t;

namespace body {
inline constexpr BodyMask None = 0u;
inline constexpr BodyMask Pelvis = 1u << 0;
inline constexpr BodyMask Spine = 1u << 1;
inline constexpr BodyMask Chest = 1u << 2;
inline constexpr BodyMask Shoulders = 1u << 3;
inline constexpr BodyMask Head = 1u << 4;
inline constexpr BodyMask LeftArm = 1u << 5;
inline constexpr BodyMask RightArm = 1u << 6;
inline constexpr BodyMask LeftHand = 1u << 7;
inline constexpr BodyMask RightHand = 1u << 8;
inline constexpr BodyMask LeftLeg = 1u << 9;
inline constexpr BodyMask RightLeg = 1u << 10;
inline constexpr BodyMask LeftFoot = 1u << 11;
inline constexpr BodyMask RightFoot = 1u << 12;

inline constexpr BodyMask Torso = Pelvis | Spine | Chest;
inline constexpr BodyMask UpperBody = Spine | Chest | Shoulders | Head | LeftArm |
                                      RightArm | LeftHand | RightHand;
inline constexpr BodyMask Arms = LeftArm | RightArm;
inline constexpr BodyMask Hands = LeftHand | RightHand;
inline constexpr BodyMask Legs = LeftLeg | RightLeg | LeftFoot | RightFoot;
inline constexpr BodyMask LowerBody = Pelvis | Legs;
inline constexpr BodyMask Feet = LeftFoot | RightFoot;
inline constexpr BodyMask All = UpperBody | LowerBody;
}  // namespace body

enum class MotionCategory : std::uint8_t {
    Arm,
    Hand,
    Torso,
    Shoulder,
    Head,
    Footwork,
    FullBody
};

enum class BlendMode : std::uint8_t {
    Additive,  // add the rotation on top of the base pose (default)
    Override,  // replace the masked bones while active
    Masked     // additive, but the mask is narrowed to the primitive's joints
};

enum class PrimitiveKind : std::uint8_t {
    ArmWave,
    AlternatingArmSwing,
    HandFlick,
    WristRoll,
    ArmReach,
    ShoulderAccent,
    ShoulderRoll,
    ChestIsolation,
    TorsoTwist,
    BodyWave,
    SideLean,
    UpperBodyBounce,
    HipSway,
    WeightShift,
    SideStep,
    StepTouch,
    HeelTap,
    ToeTap,
    KneeBounce,
    HeadNod,
    HeadTilt,
    LookDirection,
    HeadAccent,
    FingerRipple,
    FingerSpread,
    FistPulse,
    PointGesture,
    Count
};

inline constexpr int kPrimitiveKindCount = static_cast<int>(PrimitiveKind::Count);

struct PrimitiveMetadata {
    PrimitiveKind kind = PrimitiveKind::ArmWave;
    const char* id = "";
    const char* name = "";
    MotionCategory category = MotionCategory::Arm;
    BodyMask mask = body::None;
    BlendMode blend = BlendMode::Additive;
    float defaultBeats = 1.0f;
    float minBeats = 0.25f;
    float maxBeats = 4.0f;
    float minIntensity = 0.15f;
    float maxIntensity = 1.0f;
    int priority = 50;           // higher wins when masks overlap
    bool repeatable = true;
    bool accentAffinity = false; // best on a musical accent
    bool needsSupport = false;   // requires at least one planted foot
    bool altersFeet = false;     // may move a swing foot, never a planted one
    bool needsFingers = false;
    const char* description = "";
};

[[nodiscard]] const PrimitiveMetadata& primitiveMetadata(PrimitiveKind kind);
[[nodiscard]] const std::vector<PrimitiveMetadata>& primitiveLibrary();
[[nodiscard]] const PrimitiveMetadata* findPrimitive(const std::string& id);
[[nodiscard]] PrimitiveKind primitiveFromId(const std::string& id, bool* found = nullptr);
[[nodiscard]] const char* categoryName(MotionCategory category);
[[nodiscard]] const char* blendModeName(BlendMode mode);

// A scheduled primitive on the clip's musical axis. `beat` is a clip beat;
// a cue that crosses the loop point is wrapped by the scheduler.
struct PrimitiveCue {
    PrimitiveKind kind = PrimitiveKind::HandFlick;
    float beat = 0.0f;
    float durationBeats = 1.0f;
    float intensity = 0.6f;
    int priorityOverride = -1;
    BodyMask maskOverride = 0;
    BlendMode blend = BlendMode::Additive;
    int side = -1;  // -1 both, 0 left, 1 right
    float phaseOffset = 0.0f;
    std::string reason;

    [[nodiscard]] const PrimitiveMetadata& metadata() const {
        return primitiveMetadata(kind);
    }
    [[nodiscard]] int effectivePriority() const {
        return priorityOverride >= 0 ? priorityOverride : metadata().priority;
    }
    [[nodiscard]] BodyMask effectiveMask() const {
        return maskOverride ? (maskOverride & metadata().mask) : metadata().mask;
    }
    [[nodiscard]] BlendMode effectiveBlend() const { return blend; }
};

struct PrimitivePose {
    std::array<Quat, vrm::kHumanoidBoneCount> bone;
    Vec3 hips{};                 // additive offset, height units
    std::array<float, 2> footLift{};  // additive vertical lift, height units
    std::array<float, 2> toePitch{};  // additive -1..1
    std::array<HandPose, 2> hand;
    BodyMask mask = body::None;
    bool touchesFeet = false;
    float weight = 0.0f;
    float envelope = 0.0f;       // 0..1 attack/release shape
    float localPhase = 0.0f;     // 0..1 through the cue
};

struct CompositionOptions {
    bool allowFootwork = true;
    bool allowHands = true;
    bool allowTorso = true;
    bool allowHead = true;
    int maxUpperBodyLayers = 3;
};

struct CompositionDiagnostics {
    int activeCues = 0;
    int acceptedCues = 0;
    int rejectedByConflict = 0;
    int rejectedByContact = 0;
    int rejectedBySupport = 0;
    int rejectedByMask = 0;
    float maxWeight = 0.0f;
    std::array<int, vrm::kHumanoidBoneCount> boneLayers{};
    std::vector<std::string> notes;
};

struct ComposedPose {
    ClipPose pose;
    CompositionDiagnostics diagnostics;
};

// Evaluates one primitive at a local phase (0..1) and intensity. This is a pure
// function; the scheduler and the compositor are the only callers.
[[nodiscard]] PrimitivePose samplePrimitive(PrimitiveKind kind, float phase,
                                            float intensity, int side,
                                            float accent = 0.0f);

// Combines a base pose with active cues. Deterministic; cue order does not
// affect the result because cues are sorted by priority and beat.
[[nodiscard]] ComposedPose composeMotion(const ClipPose& base,
                                         std::span<const PrimitiveCue> cues,
                                         double clipBeat, double clipBeats,
                                         bool looping,
                                         const CompositionOptions& options = {});

// Deterministic overlay planner. It looks at the clip's accents and bar
// structure and schedules gestures that fit the music instead of firing a
// gesture on every beat.
[[nodiscard]] std::vector<PrimitiveCue> planPrimitiveOverlays(
    const HumanoidClip& clip, std::uint32_t seed, float density,
    bool includeHands = true);

// True when two body masks share a region that cannot be driven twice.
[[nodiscard]] bool masksConflict(BodyMask a, BodyMask b);

}  // namespace bgmedia::motion
