#include "bgmedia/motion3/MotionPrimitive.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>

namespace bgmedia::motion {
namespace {

using math::Quat;
using math::Vec3;
using vrm::HumanoidBone;

constexpr float kDeg = 3.14159265358979323846f / 180.0f;
constexpr float kTiny = 1.0e-6f;

constexpr Vec3 kX{1.0f, 0.0f, 0.0f};
constexpr Vec3 kY{0.0f, 1.0f, 0.0f};
constexpr Vec3 kZ{0.0f, 0.0f, 1.0f};

float clamp01(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

float smoothstep(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

float bell(float phase, float attack, float release) {
    const float a = attack > kTiny ? smoothstep(phase / attack) : 1.0f;
    const float r = release > kTiny ? smoothstep((1.0f - phase) / release) : 1.0f;
    return std::max(0.0f, std::min(a, r));
}

float wrapPositive(float value, float period) {
    if (!(period > kTiny)) {
        return 0.0f;
    }
    float wrapped = std::fmod(value, period);
    if (wrapped < 0.0f) {
        wrapped += period;
    }
    return wrapped;
}

std::size_t indexOf(HumanoidBone bone) {
    return static_cast<std::size_t>(bone);
}

Quat axisAngle(Vec3 axis, float degrees) {
    return math::fromAxisAngle(axis, degrees * kDeg);
}

Quat addDelta(Quat base, Quat delta, float weight) {
    const Quat d = math::slerp(Quat::identity(), delta, clamp01(weight));
    return (d * base).normalized();
}

int hashString(const std::string& text) {
    std::uint32_t hash = 2166136261u;
    for (char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619u;
    }
    return static_cast<int>(hash & 0x7fffffffu);
}

struct Rng {
    std::uint32_t state = 1u;
    explicit Rng(std::uint32_t seed) : state(seed ? seed : 1u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    float unit() { return static_cast<float>(next() >> 8) / 16777215.0f; }
    int range(int lo, int hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<int>(next() % static_cast<std::uint32_t>(hi - lo + 1));
    }
    bool chance(float p) { return unit() < p; }
};

// --- metadata --------------------------------------------------------------

const std::vector<PrimitiveMetadata>& buildLibrary() {
    static const std::vector<PrimitiveMetadata> library = [] {
    std::vector<PrimitiveMetadata> v = {
        {PrimitiveKind::ArmWave, "armWave", "Arm wave", MotionCategory::Arm,
         body::Arms | body::Hands, BlendMode::Additive, 2.0f, 0.75f, 4.0f,
         0.25f, 1.0f, 42, true, false, false, false, false,
         "A travelling wave from shoulder to wrist, one arm leading."},
        {PrimitiveKind::AlternatingArmSwing, "armSwing", "Alternating arm swing",
         MotionCategory::Arm, body::LeftArm | body::RightArm, BlendMode::Additive,
         2.0f, 1.0f, 4.0f, 0.2f, 1.0f, 38, true, false, false, false, false,
         "Counter-swinging arms with a soft elbow follow-through."},
        {PrimitiveKind::HandFlick, "handFlick", "Hand flick", MotionCategory::Hand,
         body::LeftHand | body::RightHand, BlendMode::Additive, 0.5f, 0.25f, 1.0f,
         0.25f, 1.0f, 56, true, true, false, false, false,
         "A short wrist snap that starts and ends relaxed."},
        {PrimitiveKind::WristRoll, "wristRoll", "Wrist roll", MotionCategory::Hand,
         body::LeftHand | body::RightHand, BlendMode::Additive, 1.0f, 0.5f, 2.0f,
         0.2f, 1.0f, 44, true, false, false, false, false,
         "A continuous roll around the forearm axis."},
        {PrimitiveKind::ArmReach, "armReach", "Arm reach", MotionCategory::Arm,
         body::Arms | body::Hands, BlendMode::Additive, 1.5f, 0.75f, 3.0f,
         0.25f, 1.0f, 47, false, false, false, false, false,
         "A prepared diagonal reach with a controlled elbow and open hand."},
        {PrimitiveKind::ShoulderAccent, "shoulderAccent", "Shoulder accent",
         MotionCategory::Shoulder, body::Shoulders | body::Chest, BlendMode::Additive, 0.5f, 0.25f,
         1.5f, 0.2f, 1.0f, 62, true, true, false, false, false,
         "A single shoulder lift/drop answering a bass hit."},
        {PrimitiveKind::ShoulderRoll, "shoulderRoll", "Shoulder roll",
         MotionCategory::Shoulder, body::Shoulders | body::Chest, BlendMode::Additive, 1.5f, 0.75f,
         3.0f, 0.2f, 1.0f, 40, true, false, false, false, false,
         "A circular shoulder roll with a delayed chest answer."},
        {PrimitiveKind::ChestIsolation, "chestIsolation", "Chest isolation",
         MotionCategory::Torso, body::Spine | body::Chest | body::Pelvis, BlendMode::Additive, 0.75f,
         0.5f, 2.0f, 0.2f, 1.0f, 52, true, true, false, false, false,
         "The ribcage moves while the pelvis stays put."},
        {PrimitiveKind::TorsoTwist, "torsoTwist", "Torso twist", MotionCategory::Torso,
         body::Torso | body::Head, BlendMode::Additive, 1.5f, 0.75f, 3.0f, 0.2f, 1.0f, 46, true,
         false, false, false, false, "Pelvis and chest counter-rotate."},
        {PrimitiveKind::BodyWave, "bodyWave", "Body wave", MotionCategory::Torso,
         body::Pelvis | body::Spine | body::Chest | body::Head, BlendMode::Additive, 2.0f, 1.0f,
         4.0f, 0.2f, 1.0f, 48, false, false, false, false, false,
         "A wave travelling through pelvis, spine and chest with a lag."},
        {PrimitiveKind::SideLean, "sideLean", "Side lean", MotionCategory::Torso,
         body::Spine | body::Chest | body::Head, BlendMode::Additive, 1.0f, 0.5f, 2.0f, 0.2f, 1.0f,
         35, true, false, false, false, false,
         "A lateral lean with a small shoulder counter-tilt."},
        {PrimitiveKind::UpperBodyBounce, "upperBodyBounce", "Upper-body bounce",
         MotionCategory::Torso, body::Pelvis | body::Spine | body::Chest,
         BlendMode::Additive, 0.5f, 0.25f, 2.0f, 0.15f, 1.0f, 32, true, false, false,
         false, false, "A small, springy bounce that never moves a planted foot."},
        {PrimitiveKind::HipSway, "hipSway", "Hip sway", MotionCategory::FullBody,
         body::Pelvis | body::Spine, BlendMode::Additive, 1.0f, 0.5f, 2.0f, 0.2f, 1.0f, 36, true,
         false, false, false, false, "Lateral pelvis shift with a soft roll."},
        {PrimitiveKind::WeightShift, "weightShift", "Weight shift",
         MotionCategory::FullBody, body::Pelvis | body::Spine | body::Chest | body::Legs,
         BlendMode::Additive, 1.0f, 0.75f, 2.0f, 0.25f, 1.0f, 54, true, false, true, false, false,
         "Transfers the body over the loaded leg; both feet stay planted."},
        {PrimitiveKind::SideStep, "sideStep", "Side step", MotionCategory::Footwork,
         body::Pelvis | body::Spine | body::Legs | body::Feet, BlendMode::Additive, 1.0f, 0.75f, 2.0f,
         0.3f, 1.0f, 66, true, false, true, true, false,
         "A lateral step with a knee lift and a hip-led landing."},
        {PrimitiveKind::StepTouch, "stepTouch", "Step touch", MotionCategory::Footwork,
         body::Pelvis | body::Legs | body::Feet, BlendMode::Additive, 1.0f, 0.75f, 2.0f,
         0.3f, 1.0f, 64, true, false, true, true, false,
         "Step out and close with the free foot touching down."},
        {PrimitiveKind::HeelTap, "heelTap", "Heel tap", MotionCategory::Footwork,
         body::LeftLeg | body::RightLeg | body::LeftFoot | body::RightFoot,
         BlendMode::Additive, 0.75f, 0.5f, 1.5f, 0.25f, 1.0f, 60, true, false, true,
         true, false, "The heel leads, the toes lift, then the foot settles."},
        {PrimitiveKind::ToeTap, "toeTap", "Toe tap", MotionCategory::Footwork,
         body::LeftLeg | body::RightLeg | body::LeftFoot | body::RightFoot,
         BlendMode::Additive, 0.75f, 0.5f, 1.5f, 0.25f, 1.0f, 58, true, false, true,
         true, false, "A light toe tap with a tiny knee release."},
        {PrimitiveKind::KneeBounce, "kneeBounce", "Knee bounce", MotionCategory::Footwork,
         body::Pelvis | body::Legs, BlendMode::Additive, 0.5f, 0.25f, 2.0f, 0.15f, 1.0f,
         30, true, false, true, false, false,
         "A compliant knee bounce; the feet never change their target."},
        {PrimitiveKind::HeadNod, "headNod", "Head nod", MotionCategory::Head,
         body::Head, BlendMode::Additive, 0.5f, 0.25f, 1.0f, 0.15f, 1.0f, 34, true,
         false, false, false, false, "A relaxed nod with a small neck lead."},
        {PrimitiveKind::HeadTilt, "headTilt", "Head tilt", MotionCategory::Head,
         body::Head, BlendMode::Additive, 1.0f, 0.5f, 2.0f, 0.2f, 1.0f, 33, true, false,
         false, false, false, "A lateral head tilt with a delayed neck answer."},
        {PrimitiveKind::LookDirection, "lookDirection", "Look direction",
         MotionCategory::Head, body::Head, BlendMode::Additive, 1.5f, 0.75f, 3.0f, 0.2f,
         1.0f, 37, true, false, false, false, false,
         "Turns the gaze first, then lets the head follow."},
        {PrimitiveKind::HeadAccent, "headAccent", "Head accent", MotionCategory::Head,
         body::Head | body::Chest, BlendMode::Additive, 0.4f, 0.25f, 1.0f, 0.2f, 1.0f, 64, true, true,
         false, false, false, "A fast, short head accent on a musical hit."},
        {PrimitiveKind::FingerRipple, "fingerRipple", "Finger ripple",
         MotionCategory::Hand, body::LeftHand | body::RightHand, BlendMode::Additive,
         1.0f, 0.5f, 2.0f, 0.2f, 1.0f, 55, true, false, false, false, true,
         "A small wave through the fingers; each finger lags the previous one."},
        {PrimitiveKind::FingerSpread, "fingerSpread", "Finger spread", MotionCategory::Hand,
         body::LeftHand | body::RightHand, BlendMode::Additive, 0.75f, 0.5f, 2.0f, 0.2f,
         1.0f, 50, true, false, false, false, true,
         "Opens the hand from the middle finger outward."},
        {PrimitiveKind::FistPulse, "fistPulse", "Fist pulse", MotionCategory::Hand,
         body::LeftHand | body::RightHand, BlendMode::Additive, 0.4f, 0.25f, 1.0f, 0.2f,
         1.0f, 61, true, true, false, false, true,
         "Closes the fingers on the hit, then releases them."},
        {PrimitiveKind::PointGesture, "pointGesture", "Point gesture",
         MotionCategory::Hand, body::LeftArm | body::RightArm | body::LeftHand |
                                      body::RightHand,
         BlendMode::Additive, 1.25f, 0.75f, 2.5f, 0.25f, 1.0f, 68, false, false, false,
         false, true, "Raises the arm and extends one finger while the rest relax."},
    };
    // `side` only picks the lead/direction of these; they always involve both
    // limbs, so the side must not narrow their mask.
    for (PrimitiveMetadata& m : v) {
        m.bilateral = m.kind == PrimitiveKind::WeightShift ||
                      m.kind == PrimitiveKind::KneeBounce ||
                      m.kind == PrimitiveKind::StepTouch;
    }
    return v;
    }();
    return library;
}

// --- primitive sampling ----------------------------------------------------

void setBone(PrimitivePose& pose, HumanoidBone bone, Quat delta) {
    pose.bone[indexOf(bone)] = (delta * pose.bone[indexOf(bone)]).normalized();
}

void addBoneParent(PrimitivePose& pose, HumanoidBone bone, Quat delta) {
    pose.bone[indexOf(bone)] = (delta * pose.bone[indexOf(bone)]).normalized();
}

struct ArmSet {
    HumanoidBone upper;
    HumanoidBone lower;
    HumanoidBone hand;
    HumanoidBone shoulder;
    int sd;
};

ArmSet armSet(int sd) {
    if (sd == 0) {
        return {HumanoidBone::LeftUpperArm, HumanoidBone::LeftLowerArm,
                HumanoidBone::LeftHand, HumanoidBone::LeftShoulder, 0};
    }
    return {HumanoidBone::RightUpperArm, HumanoidBone::RightLowerArm,
            HumanoidBone::RightHand, HumanoidBone::RightShoulder, 1};
}

float sideSign(int sd) { return sd == 0 ? -1.0f : 1.0f; }

void raiseArm(PrimitivePose& pose, const ArmSet& arm, float degrees) {
    setBone(pose, arm.upper, axisAngle(kZ, -sideSign(arm.sd) * degrees));
}

void swingArm(PrimitivePose& pose, const ArmSet& arm, float degrees) {
    setBone(pose, arm.upper, axisAngle(kY, sideSign(arm.sd) * degrees));
}

void bendElbow(PrimitivePose& pose, const ArmSet& arm, float degrees) {
    setBone(pose, arm.lower, axisAngle(kY, sideSign(arm.sd) * degrees));
}

void rollWrist(PrimitivePose& pose, const ArmSet& arm, float degrees) {
    setBone(pose, arm.hand, axisAngle(kZ, -sideSign(arm.sd) * degrees));
}

void setFinger(PrimitivePose& pose, int sd, int finger, float curl,
               float spread = 0.0f, float opposition = 0.0f) {
    HandPose& hand = pose.hand[static_cast<std::size_t>(sd)];
    for (int joint = 0; joint < 3; ++joint) {
        hand.curl[static_cast<std::size_t>(finger)][static_cast<std::size_t>(joint)] =
            clamp01(curl);
    }
    if (finger == 0) {
        hand.thumbOpposition = clamp01(opposition);
    } else {
        hand.spread[static_cast<std::size_t>(finger)] = std::clamp(spread, -1.0f, 1.0f);
    }
}

void relaxHands(PrimitivePose& pose, float curl = 0.3f) {
    pose.hand[0] = relaxedHandPose(curl);
    pose.hand[1] = relaxedHandPose(curl);
}

}  // namespace

const std::vector<PrimitiveMetadata>& primitiveLibrary() { return buildLibrary(); }

const PrimitiveMetadata& primitiveMetadata(PrimitiveKind kind) {
    const auto& library = primitiveLibrary();
    const std::size_t index = static_cast<std::size_t>(kind);
    return library[std::min(index, library.size() - 1)];
}

const PrimitiveMetadata* findPrimitive(const std::string& id) {
    for (const PrimitiveMetadata& metadata : primitiveLibrary()) {
        if (id == metadata.id) {
            return &metadata;
        }
    }
    return nullptr;
}

PrimitiveKind primitiveFromId(const std::string& id, bool* found) {
    if (const PrimitiveMetadata* metadata = findPrimitive(id)) {
        if (found) *found = true;
        return metadata->kind;
    }
    if (found) *found = false;
    return PrimitiveKind::ArmWave;
}

const char* categoryName(MotionCategory category) {
    switch (category) {
    case MotionCategory::Arm: return "arm";
    case MotionCategory::Hand: return "hand";
    case MotionCategory::Torso: return "torso";
    case MotionCategory::Shoulder: return "shoulder";
    case MotionCategory::Head: return "head";
    case MotionCategory::Footwork: return "footwork";
    case MotionCategory::FullBody: return "fullbody";
    }
    return "unknown";
}

const char* blendModeName(BlendMode mode) {
    switch (mode) {
    case BlendMode::Additive: return "additive";
    case BlendMode::Override: return "override";
    case BlendMode::Masked: return "masked";
    }
    return "additive";
}

BodyMask maskBitForBone(HumanoidBone bone) {
    switch (bone) {
    case HumanoidBone::Hips: return body::Pelvis;
    case HumanoidBone::Spine: return body::Spine;
    case HumanoidBone::Chest:
    case HumanoidBone::UpperChest: return body::Chest;
    case HumanoidBone::Neck:
    case HumanoidBone::Head: return body::Head;
    case HumanoidBone::LeftShoulder:
    case HumanoidBone::RightShoulder: return body::Shoulders;
    case HumanoidBone::LeftUpperArm:
    case HumanoidBone::LeftLowerArm: return body::LeftArm;
    case HumanoidBone::RightUpperArm:
    case HumanoidBone::RightLowerArm: return body::RightArm;
    case HumanoidBone::LeftHand: return body::LeftHand;
    case HumanoidBone::RightHand: return body::RightHand;
    case HumanoidBone::LeftUpperLeg:
    case HumanoidBone::LeftLowerLeg: return body::LeftLeg;
    case HumanoidBone::RightUpperLeg:
    case HumanoidBone::RightLowerLeg: return body::RightLeg;
    case HumanoidBone::LeftFoot:
    case HumanoidBone::LeftToes: return body::LeftFoot;
    case HumanoidBone::RightFoot:
    case HumanoidBone::RightToes: return body::RightFoot;
    default: return body::None;
    }
}

BodyMask narrowMaskToSide(BodyMask mask, int side) {
    if (side == 0) {
        return mask & ~(body::RightArm | body::RightHand | body::RightLeg |
                        body::RightFoot);
    }
    if (side == 1) {
        return mask & ~(body::LeftArm | body::LeftHand | body::LeftLeg |
                        body::LeftFoot);
    }
    return mask;
}

bool masksConflict(BodyMask a, BodyMask b) {
    const BodyMask shared = a & b;
    // Hands/arms are separate layers by design. A torso cue and an arm cue can
    // coexist, but two cues that drive the same limb or the same torso segment
    // may not run at the same time.
    static constexpr BodyMask kExclusive =
        body::Pelvis | body::Spine | body::Chest | body::Shoulders | body::Head |
        body::LeftArm | body::RightArm | body::LeftHand | body::RightHand |
        body::LeftLeg | body::RightLeg | body::LeftFoot | body::RightFoot;
    return (shared & kExclusive) != 0u;
}

PrimitivePose samplePrimitive(PrimitiveKind kind, float phase, float intensity,
                              int side, float accent) {
    PrimitivePose pose;
    pose.bone.fill(Quat::identity());
    relaxHands(pose, 0.30f);
    pose.mask = primitiveMetadata(kind).mask;
    pose.weight = 1.0f;
    phase = clamp01(phase);
    intensity = clamp01(intensity);
    accent = clamp01(accent);
    pose.localPhase = phase;

    const float amp = intensity * (1.0f + 0.28f * accent);
    const float env = bell(phase, 0.18f - 0.10f * accent, 0.28f);
    const float snap = bell(phase, 0.08f, 0.45f);
    const float wave = std::sin(phase * 6.2831853f);
    const float sway = std::sin(phase * 3.14159265f);
    const int first = side == 0 || side == 1 ? side : 0;

    switch (kind) {
    case PrimitiveKind::ArmWave: {
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            const float lag = sd == 0 ? 0.0f : 0.18f;
            const float p = std::fmod(phase + lag, 1.0f);
            const float e = bell(p, 0.20f, 0.30f);
            const ArmSet arm = armSet(sd);
            raiseArm(pose, arm, (18.0f + 26.0f * e) * amp);
            swingArm(pose, arm, 10.0f * std::sin(p * 6.2831853f) * amp);
            bendElbow(pose, arm, (14.0f + 32.0f * e) * amp);
            rollWrist(pose, arm, 26.0f * std::sin((p + 0.22f) * 6.2831853f) * amp);
            for (int fg = 1; fg < 5; ++fg) {
                setFinger(pose, sd, fg, 0.22f + 0.22f * e, 0.15f * e);
            }
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftArm | body::LeftHand
                                           : body::RightArm | body::RightHand)
                              : body::LeftArm | body::RightArm;
        break;
    }
    case PrimitiveKind::AlternatingArmSwing: {
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            const float sign = sd == 0 ? 1.0f : -1.0f;
            const ArmSet arm = armSet(sd);
            swingArm(pose, arm, (28.0f * wave * sign) * amp);
            raiseArm(pose, arm, (10.0f + 8.0f * std::abs(wave)) * amp);
            bendElbow(pose, arm, (26.0f + 14.0f * std::max(0.0f, wave * sign)) * amp);
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftArm : body::RightArm)
                              : body::LeftArm | body::RightArm;
        break;
    }
    case PrimitiveKind::HandFlick: {
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            const ArmSet arm = armSet(sd);
            // Attack widened from 0.10 to 0.35 of the cue (about 0.15-0.2 beat for the
            // usual 0.5-beat accents): the wrist snap no longer peaks at 25+ rad/s.
            const float s = bell(phase, 0.35f, 0.30f);
            rollWrist(pose, arm, (40.0f * s) * amp);
            setBone(pose, arm.hand, axisAngle(kY, -18.0f * s * amp));
            for (int fg = 1; fg < 5; ++fg) {
                setFinger(pose, sd, fg, 0.18f + 0.38f * s, 0.3f * s);
            }
            setFinger(pose, sd, 0, 0.25f + 0.2f * s, 0.0f, 0.3f * s);
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftHand : body::RightHand)
                              : body::LeftHand | body::RightHand;
        break;
    }
    case PrimitiveKind::WristRoll: {
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            const ArmSet arm = armSet(sd);
            const float angle = 360.0f * phase * amp;
            rollWrist(pose, arm, 24.0f * std::sin(angle * kDeg));
            setBone(pose, arm.hand, axisAngle(kY, 20.0f * std::cos(angle * kDeg) * amp));
            for (int fg = 1; fg < 5; ++fg) {
                setFinger(pose, sd, fg, 0.22f + 0.18f * (0.5f + 0.5f * std::sin(angle * kDeg)),
                          0.12f);
            }
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftHand : body::RightHand)
                              : body::LeftHand | body::RightHand;
        break;
    }
    case PrimitiveKind::ArmReach: {
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            const ArmSet arm = armSet(sd);
            const float e = bell(phase, 0.30f, 0.30f);
            raiseArm(pose, arm, (34.0f + 36.0f * e) * amp);
            swingArm(pose, arm, (12.0f + 38.0f * e) * amp);
            bendElbow(pose, arm, (48.0f - 30.0f * e) * amp);
            rollWrist(pose, arm, 18.0f * e * amp);
            for (int fg = 1; fg < 5; ++fg) {
                setFinger(pose, sd, fg, 0.16f + 0.08f * e, 0.55f * e);
            }
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftArm | body::LeftHand
                                           : body::RightArm | body::RightHand)
                              : body::LeftArm | body::RightArm;
        break;
    }
    case PrimitiveKind::ShoulderAccent: {
        const float up = snap * amp;
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            const float sign = sd == 0 ? 1.0f : -1.0f;
            setBone(pose,
                    sd == 0 ? HumanoidBone::LeftShoulder : HumanoidBone::RightShoulder,
                    axisAngle(kZ, -sign * 12.0f * up));
            // The arm hangs from the shoulder and inherits this lift; rotating
            // the upper arm as well would write outside the Shoulders|Chest mask.
        }
        addBoneParent(pose, HumanoidBone::Chest, axisAngle(kY, 4.0f * up * (first == 0 ? 1.0f : -1.0f)));
        pose.mask = body::Shoulders;
        break;
    }
    case PrimitiveKind::ShoulderRoll: {
        const float angle = phase * 360.0f;
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            const float sign = sd == 0 ? 1.0f : -1.0f;
            const float lift = 10.0f * sway * amp;
            const float fwd = 12.0f * std::cos(angle * kDeg) * amp;
            setBone(pose,
                    sd == 0 ? HumanoidBone::LeftShoulder : HumanoidBone::RightShoulder,
                    (axisAngle(kZ, -sign * lift) * axisAngle(kY, sign * fwd))
                        .normalized());
        }
        addBoneParent(pose, HumanoidBone::Chest,
                      axisAngle(kY, 5.0f * std::sin(angle * kDeg) * amp));
        pose.mask = body::Shoulders;
        break;
    }
    case PrimitiveKind::ChestIsolation: {
        const float sideMove = wave * amp;
        const float fwd = snap * amp;
        addBoneParent(pose, HumanoidBone::Spine, axisAngle(kZ, 4.0f * sideMove));
        addBoneParent(pose, HumanoidBone::Chest, axisAngle(kZ, 8.0f * sideMove));
        addBoneParent(pose, HumanoidBone::Chest, axisAngle(kX, 3.5f * fwd));
        addBoneParent(pose, HumanoidBone::UpperChest, axisAngle(kX, 2.0f * fwd));
        addBoneParent(pose, HumanoidBone::UpperChest, axisAngle(kZ, 4.0f * sideMove));
        addBoneParent(pose, HumanoidBone::Hips, axisAngle(kZ, -2.0f * sideMove));
        pose.mask = body::Spine | body::Chest | body::Shoulders;
        break;
    }
    case PrimitiveKind::TorsoTwist: {
        const float twist = wave * amp;
        addBoneParent(pose, HumanoidBone::Hips, axisAngle(kY, 6.0f * twist));
        addBoneParent(pose, HumanoidBone::Spine, axisAngle(kY, -10.0f * twist));
        addBoneParent(pose, HumanoidBone::Chest, axisAngle(kY, -12.0f * twist));
        addBoneParent(pose, HumanoidBone::UpperChest, axisAngle(kY, -8.0f * twist));
        addBoneParent(pose, HumanoidBone::Neck, axisAngle(kY, 8.0f * twist));
        addBoneParent(pose, HumanoidBone::Head, axisAngle(kY, 12.0f * twist));
        pose.mask = body::Pelvis | body::Spine | body::Chest;
        break;
    }
    case PrimitiveKind::BodyWave: {
        const float p = phase * 6.2831853f;
        addBoneParent(pose, HumanoidBone::Hips, axisAngle(kX, 4.0f * std::sin(p) * amp));
        addBoneParent(pose, HumanoidBone::Spine,
                      axisAngle(kX, 7.0f * std::sin(p - 0.35f) * amp));
        addBoneParent(pose, HumanoidBone::Chest,
                      axisAngle(kX, 9.0f * std::sin(p - 0.70f) * amp));
        addBoneParent(pose, HumanoidBone::UpperChest,
                      axisAngle(kX, 6.0f * std::sin(p - 1.05f) * amp));
        addBoneParent(pose, HumanoidBone::Neck,
                      axisAngle(kX, -3.0f * std::sin(p - 1.25f) * amp));
        pose.mask = body::Pelvis | body::Spine | body::Chest;
        break;
    }
    case PrimitiveKind::SideLean: {
        const float lean = sway * amp;
        addBoneParent(pose, HumanoidBone::Spine, axisAngle(kZ, 9.0f * lean));
        addBoneParent(pose, HumanoidBone::Chest, axisAngle(kZ, 8.0f * lean));
        addBoneParent(pose, HumanoidBone::UpperChest, axisAngle(kZ, 4.0f * lean));
        addBoneParent(pose, HumanoidBone::Neck, axisAngle(kZ, -3.0f * lean));
        addBoneParent(pose, HumanoidBone::Head, axisAngle(kZ, -4.0f * lean));
        pose.mask = body::Spine | body::Chest;
        break;
    }
    case PrimitiveKind::UpperBodyBounce: {
        const float bounce = -std::abs(std::sin(phase * 3.14159265f)) * amp;
        pose.hips.y = -0.018f * amp * (0.5f - 0.5f * std::cos(phase * 6.2831853f));
        addBoneParent(pose, HumanoidBone::Spine, axisAngle(kX, 3.0f * bounce));
        addBoneParent(pose, HumanoidBone::Chest, axisAngle(kX, 2.0f * bounce));
        pose.mask = body::Pelvis | body::Spine | body::Chest;
        break;
    }
    case PrimitiveKind::HipSway: {
        pose.hips.x = 0.020f * std::sin(phase * 6.2831853f) * amp;
        addBoneParent(pose, HumanoidBone::Hips,
                      axisAngle(kZ, 7.0f * std::sin(phase * 6.2831853f) * amp));
        addBoneParent(pose, HumanoidBone::Spine, axisAngle(kZ, -3.0f * std::sin(phase * 6.2831853f) * amp));
        pose.mask = body::Pelvis;
        break;
    }
    case PrimitiveKind::WeightShift: {
        const float s = sway * amp;
        pose.hips.x = 0.026f * s;
        addBoneParent(pose, HumanoidBone::Hips, axisAngle(kZ, 7.0f * s));
        addBoneParent(pose, HumanoidBone::Spine, axisAngle(kZ, -4.0f * s));
        addBoneParent(pose, HumanoidBone::Chest, axisAngle(kZ, -2.0f * s));
        addBoneParent(pose, HumanoidBone::LeftUpperLeg, axisAngle(kZ, -2.0f * s));
        addBoneParent(pose, HumanoidBone::RightUpperLeg, axisAngle(kZ, -2.0f * s));
        pose.mask = body::Pelvis | body::Legs;
        break;
    }
    case PrimitiveKind::SideStep: {
        const float lift = std::sin(phase * 3.14159265f);
        const int sd = side >= 0 ? side : first;
        pose.hips.x = (sd == 0 ? 0.024f : -0.024f) * lift * amp;
        addBoneParent(pose, HumanoidBone::Hips,
                      axisAngle(kZ, (sd == 0 ? 8.0f : -8.0f) * lift * amp));
        addBoneParent(pose, HumanoidBone::Spine,
                      axisAngle(kZ, (sd == 0 ? -4.0f : 4.0f) * lift * amp));
        if (pose.footLift[static_cast<std::size_t>(sd)] < 1.0e-5f) {
            pose.footLift[static_cast<std::size_t>(sd)] = 0.055f * lift * amp;
            pose.toePitch[static_cast<std::size_t>(sd)] = 0.35f * lift * amp;
        }
        pose.touchesFeet = true;
        pose.mask = body::Pelvis | body::Legs | body::Feet;
        break;
    }
    case PrimitiveKind::StepTouch: {
        const float lift = std::sin(phase * 3.14159265f);
        const int sd = side >= 0 ? side : first;
        const int other = 1 - sd;
        pose.hips.x = (sd == 0 ? 0.018f : -0.018f) * lift * amp;
        addBoneParent(pose, HumanoidBone::Hips,
                      axisAngle(kY, (sd == 0 ? 6.0f : -6.0f) * lift * amp));
        pose.footLift[static_cast<std::size_t>(sd)] = 0.040f * lift * amp;
        pose.toePitch[static_cast<std::size_t>(other)] = -0.18f * lift * amp;
        pose.touchesFeet = true;
        pose.mask = body::Pelvis | body::Legs | body::Feet;
        break;
    }
    case PrimitiveKind::HeelTap: {
        const float lift = std::sin(phase * 3.14159265f);
        const int sd = side >= 0 ? side : first;
        pose.footLift[static_cast<std::size_t>(sd)] = 0.018f * lift * amp;
        pose.toePitch[static_cast<std::size_t>(sd)] = 0.75f * lift * amp;
        addBoneParent(pose, sd == 0 ? HumanoidBone::LeftUpperLeg : HumanoidBone::RightUpperLeg,
                      axisAngle(kX, 2.0f * lift * amp));
        pose.touchesFeet = true;
        pose.mask = body::LeftLeg | body::RightLeg | body::LeftFoot | body::RightFoot;
        break;
    }
    case PrimitiveKind::ToeTap: {
        const float lift = std::sin(phase * 3.14159265f);
        const int sd = side >= 0 ? side : first;
        pose.footLift[static_cast<std::size_t>(sd)] = 0.030f * lift * amp;
        pose.toePitch[static_cast<std::size_t>(sd)] = -0.55f * lift * amp;
        pose.touchesFeet = true;
        pose.mask = body::LeftFoot | body::RightFoot | body::LeftLeg | body::RightLeg;
        break;
    }
    case PrimitiveKind::KneeBounce: {
        const float dip = 0.5f - 0.5f * std::cos(phase * 6.2831853f);
        pose.hips.y = -0.026f * dip * amp;
        addBoneParent(pose, HumanoidBone::LeftUpperLeg, axisAngle(kX, 2.5f * dip * amp));
        addBoneParent(pose, HumanoidBone::RightUpperLeg, axisAngle(kX, 2.5f * dip * amp));
        pose.mask = body::Pelvis | body::Legs;
        break;
    }
    case PrimitiveKind::HeadNod: {
        const float nod = std::sin(phase * 6.2831853f) * amp;
        addBoneParent(pose, HumanoidBone::Neck, axisAngle(kX, 4.0f * nod));
        addBoneParent(pose, HumanoidBone::Head, axisAngle(kX, 9.0f * nod));
        pose.mask = body::Head;
        break;
    }
    case PrimitiveKind::HeadTilt: {
        const float tilt = sway * amp;
        addBoneParent(pose, HumanoidBone::Neck, axisAngle(kZ, 3.0f * tilt));
        addBoneParent(pose, HumanoidBone::Head, axisAngle(kZ, 8.0f * tilt));
        pose.mask = body::Head;
        break;
    }
    case PrimitiveKind::LookDirection: {
        const float turn = std::sin(phase * 6.2831853f) * amp;
        addBoneParent(pose, HumanoidBone::Neck, axisAngle(kY, 8.0f * turn));
        addBoneParent(pose, HumanoidBone::Head, axisAngle(kY, 16.0f * turn));
        addBoneParent(pose, HumanoidBone::Head, axisAngle(kX, -2.0f * std::abs(turn)));
        pose.mask = body::Head;
        break;
    }
    case PrimitiveKind::HeadAccent: {
        const float hit = snap * amp;
        addBoneParent(pose, HumanoidBone::Neck, axisAngle(kY, 5.0f * hit));
        addBoneParent(pose, HumanoidBone::Head, axisAngle(kY, 12.0f * hit));
        addBoneParent(pose, HumanoidBone::Head, axisAngle(kX, -4.0f * hit));
        addBoneParent(pose, HumanoidBone::Chest, axisAngle(kY, -3.0f * hit));
        pose.mask = body::Head;
        break;
    }
    case PrimitiveKind::FingerRipple: {
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            for (int fg = 1; fg < 5; ++fg) {
                const float p = std::fmod(phase + static_cast<float>(fg) * 0.12f, 1.0f);
                const float c = 0.15f + 0.42f * bell(p, 0.15f, 0.35f) * amp;
                setFinger(pose, sd, fg, c, 0.20f * bell(p, 0.2f, 0.4f) * amp);
            }
            setFinger(pose, sd, 0, 0.22f + 0.2f * env * amp, 0.0f, 0.25f * env * amp);
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftHand : body::RightHand)
                              : body::LeftHand | body::RightHand;
        break;
    }
    case PrimitiveKind::FingerSpread: {
        const float open = bell(phase, 0.25f, 0.30f) * amp;
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            for (int fg = 1; fg < 5; ++fg) {
                setFinger(pose, sd, fg, 0.12f + 0.10f * (1.0f - open),
                          (static_cast<float>(fg) - 2.0f) * 0.35f * open);
            }
            setFinger(pose, sd, 0, 0.18f, 0.0f, 0.35f * open);
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftHand : body::RightHand)
                              : body::LeftHand | body::RightHand;
        break;
    }
    case PrimitiveKind::FistPulse: {
        const float close = snap * amp;
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            for (int fg = 0; fg < 5; ++fg) {
                setFinger(pose, sd, fg, 0.20f + 0.72f * close,
                          -0.15f * close, 0.35f * close);
            }
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftHand : body::RightHand)
                              : body::LeftHand | body::RightHand;
        break;
    }
    case PrimitiveKind::PointGesture: {
        for (int sd = 0; sd < 2; ++sd) {
            if (side >= 0 && sd != side) continue;
            const ArmSet arm = armSet(sd);
            const float e = bell(phase, 0.25f, 0.30f) * amp;
            raiseArm(pose, arm, (24.0f + 36.0f * e));
            swingArm(pose, arm, (18.0f + 42.0f * e));
            bendElbow(pose, arm, 30.0f - 12.0f * e);
            for (int fg = 1; fg < 5; ++fg) {
                setFinger(pose, sd, fg, fg == 1 ? 0.05f : 0.75f);
            }
            setFinger(pose, sd, 0, 0.55f, 0.0f, 0.35f);
            pose.hand[static_cast<std::size_t>(sd)].wrist[2] =
                (sd == 0 ? -0.20f : 0.20f) * e;
        }
        pose.mask = side >= 0 ? (side == 0 ? body::LeftArm | body::LeftHand
                                           : body::RightArm | body::RightHand)
                              : body::LeftArm | body::RightArm | body::LeftHand |
                                    body::RightHand;
        break;
    }
    default:
        break;
    }
    pose.envelope = env;
    return pose;
}

namespace {

constexpr int kRegionCount = 13;  // one per body:: bit

// Minimum time (in beats) over which a stronger cue may take a region from a weaker one.
constexpr float kYieldBeats = 0.5f;

struct ActiveCue {
    const PrimitiveCue* cue = nullptr;
    PrimitivePose pose;
    float weight = 0.0f;
    // Support and contact gate (1 = open). Scales the cue's body layers; the foot lift is
    // scaled per foot by its own swing weight instead.
    float gate = 1.0f;
    // How strongly this cue makes weaker cues yield. It follows the cue's weight but never
    // faster than kYieldBeats at either end: a one-beat accent with a 2-frame attack must
    // not pull a large arm gesture back by 45 degrees in 2 frames.
    float influence = 0.0f;
    BodyMask mask = body::None;
    // Per-region share of the cue that survives stronger cues. 1 = untouched.
    // Conflicts fade a weaker cue out by (1 - winner weight) instead of cutting
    // it, so a stronger cue starting at weight ~0 cannot pop the weaker one.
    std::array<float, kRegionCount> share{};
    ActiveCue() { share.fill(1.0f); }
};

float regionShare(const ActiveCue& cue, BodyMask bit) {
    if (bit == body::None) {
        return 0.0f;
    }
    return cue.share[static_cast<std::size_t>(std::countr_zero(bit))];
}

// How planted a foot is, 0 = swinging .. 1 = planted (BG Motion 3.3, A6). Clip poses carry
// a continuous contact, which is ramped so overlays fade with the foot instead of switching
// on a threshold. A contact of exactly zero is a real value (a swing, or the first and last
// sample of a contact segment), so only a stance or pivot phase with no contact data (a
// hand-built pose) counts as planted without one.
float plantedWeight(const ClipPose& pose, int foot) {
    const auto& f = pose.foot[static_cast<std::size_t>(foot)];
    if (f.contact > 0.0f) {
        const float t = clamp01((f.contact - 0.2f) / 0.3f);
        return t * t * (3.0f - 2.0f * t);
    }
    return f.phase == FootPhase::Stance || f.phase == FootPhase::Pivot ? 1.0f : 0.0f;
}

}  // namespace

ComposedPose composeMotion(const ClipPose& base, std::span<const PrimitiveCue> cues,
                           double clipBeat, double clipBeats, bool looping,
                           const CompositionOptions& options) {
    ComposedPose out;
    out.pose = base;
    if (!out.pose.detailedHands) {
        out.pose.handPose[0] = relaxedHandPose(out.pose.hand[0]);
        out.pose.handPose[1] = relaxedHandPose(out.pose.hand[1]);
    }
    if (cues.empty() || !(clipBeats > 0.0)) {
        return out;
    }
    const float beats = static_cast<float>(clipBeats);
    float beat = static_cast<float>(clipBeat);
    if (looping) {
        beat = wrapPositive(beat, beats);
    }

    // --- 1. candidates -------------------------------------------------
    // Every cue that is musically active and mechanically allowed, with its
    // effective (side- and option-narrowed) mask.
    std::vector<ActiveCue> candidates;
    candidates.reserve(cues.size());
    for (const PrimitiveCue& cue : cues) {
        if (!(cue.durationBeats > 1.0e-4f) || !(cue.intensity > 1.0e-4f)) {
            continue;
        }
        float local = beat - cue.beat;
        if (looping) {
            local = wrapPositive(local, beats);
        } else if (local < 0.0f || local > cue.durationBeats) {
            continue;
        }
        if (local > cue.durationBeats) {
            continue;
        }
        const PrimitiveMetadata& metadata = cue.metadata();
        BodyMask mask = cue.sideMask();
        // Options remove regions instead of discarding the whole gesture: an arm
        // wave with hands disabled is still an arm wave.
        if (!options.allowFootwork) mask &= ~body::Legs;
        if (!options.allowHands) mask &= ~body::Hands;
        if (!options.allowTorso) mask &= ~body::Torso;
        if (!options.allowHead) mask &= ~body::Head;
        if (mask == body::None || (metadata.category == MotionCategory::Footwork &&
                                   (mask & body::Legs) == 0u)) {
            ++out.diagnostics.rejectedByMask;
            continue;
        }
        // Support and contact are gates on the cue's weight, not switches. A cue is rejected
        // only when the gate is closed (no planted foot to support it, or a lifted foot is
        // fully planted); between those the cue fades with the contact instead of popping.
        float gate = 1.0f;
        if (metadata.needsSupport) {
            const float support = std::max(plantedWeight(base, 0), plantedWeight(base, 1));
            if (support <= 1.0e-4f) {
                ++out.diagnostics.rejectedBySupport;
                continue;
            }
            gate *= support;
        }
        if (metadata.altersFeet) {
            // A primitive may animate a swing foot, never a planted one. The
            // base choreography owns contact. Only the feet it actually lifts
            // count; the application loop lifts each foot by its own swing weight.
            const PrimitivePose probe =
                samplePrimitive(cue.kind, 0.5f, 1.0f, cue.side, 0.0f);
            float swingGate = 1.0f;
            for (int f = 0; f < 2; ++f) {
                const bool lifts =
                    std::abs(probe.footLift[static_cast<std::size_t>(f)]) > 1.0e-6f;
                if (lifts && (mask & (f == 0 ? body::LeftFoot : body::RightFoot)) != 0u) {
                    swingGate = std::min(swingGate, 1.0f - plantedWeight(base, f));
                }
            }
            if (swingGate <= 1.0e-4f) {
                ++out.diagnostics.rejectedByContact;
                continue;
            }
            gate *= swingGate;
        }
        const float phase = clamp01(local / cue.durationBeats);
        const float envelope = bell(phase, 0.16f, 0.24f);
        const float weight = envelope * clamp01(cue.intensity);
        if (weight <= 1.0e-4f) {
            continue;
        }
        ActiveCue entry;
        entry.cue = &cue;
        entry.pose = samplePrimitive(cue.kind, phase, cue.intensity, cue.side, 0.0f);
        entry.weight = weight;
        entry.gate = gate;
        {
            const auto smooth = [](float x) {
                x = std::clamp(x, 0.0f, 1.0f);
                return x * x * (3.0f - 2.0f * x);
            };
            const float into = smooth(local / kYieldBeats);
            const float outOf = smooth((cue.durationBeats - local) / kYieldBeats);
            entry.influence = std::min(weight, std::min(into, outOf));
        }
        entry.mask = mask;
        entry.pose.mask = mask;
        candidates.push_back(std::move(entry));
    }

    // --- 2. deterministic selection ---------------------------------------
    // Highest priority first; ties are broken by the cue's own data only, never
    // by its position in the input array or by its current weight (weight order
    // would flip mid-cue and pop the layers), so permuting the cues cannot change
    // the outcome. A lower-priority cue never blocks a higher-priority one.
    const auto cueOrder = [](const ActiveCue& a, const ActiveCue& b) {
        const PrimitiveCue& ca = *a.cue;
        const PrimitiveCue& cb = *b.cue;
        if (ca.effectivePriority() != cb.effectivePriority()) {
            return ca.effectivePriority() > cb.effectivePriority();
        }
        if (ca.kind != cb.kind) return ca.kind < cb.kind;
        if (ca.side != cb.side) return ca.side < cb.side;
        if (ca.beat != cb.beat) return ca.beat < cb.beat;
        if (ca.durationBeats != cb.durationBeats) return ca.durationBeats < cb.durationBeats;
        if (ca.intensity != cb.intensity) return ca.intensity > cb.intensity;
        if (ca.blend != cb.blend) return ca.blend < cb.blend;
        if (ca.phaseOffset != cb.phaseOffset) return ca.phaseOffset < cb.phaseOffset;
        return a.mask < b.mask;
    };
    std::sort(candidates.begin(), candidates.end(), cueOrder);

    // A weaker cue does not vanish when a stronger one claims a shared region:
    // its contribution there fades by (1 - winner weight). The cue is dropped
    // only when almost nothing of it would remain anywhere.
    constexpr float kMinShare = 0.02f;
    std::vector<ActiveCue> active;
    active.reserve(candidates.size());
    float upperLayerLoad = 0.0f;
    for (ActiveCue& entry : candidates) {
        bool conflicted = false;
        for (const ActiveCue& other : active) {
            const BodyMask shared = entry.mask & other.mask;
            if (!masksConflict(entry.mask, other.mask)) {
                continue;
            }
            for (int r = 0; r < kRegionCount; ++r) {
                const BodyMask bit = 1u << r;
                if ((shared & bit) == 0u) {
                    continue;
                }
                const float winner = std::clamp(other.influence * regionShare(other, bit), 0.0f, 1.0f);
                entry.share[static_cast<std::size_t>(r)] *= 1.0f - winner;
                conflicted = true;
            }
        }
        if (conflicted) {
            float best = 0.0f;
            for (int r = 0; r < kRegionCount; ++r) {
                if ((entry.mask & (1u << r)) != 0u) {
                    best = std::max(best, entry.share[static_cast<std::size_t>(r)]);
                }
            }
            if (best < kMinShare) {
                ++out.diagnostics.rejectedByConflict;
                continue;
            }
            ++out.diagnostics.narrowedByConflict;
        }
        if ((entry.mask & body::UpperBody) != 0u) {
            // Layer cap as a *capacity*: every upper-body cue consumes its own
            // weight, and a cue only gets what is left. A rising high-priority
            // cue therefore squeezes weaker ones out gradually instead of
            // evicting one of them in a single frame.
            const float capacity = std::clamp(
                static_cast<float>(options.maxUpperBodyLayers) - upperLayerLoad, 0.0f, 1.0f);
            if (capacity < kMinShare) {
                ++out.diagnostics.rejectedByConflict;
                continue;
            }
            if (capacity < 1.0f) {
                for (float& share : entry.share) {
                    share *= capacity;
                }
                ++out.diagnostics.narrowedByConflict;
            }
            float best = 0.0f;
            for (int r = 0; r < kRegionCount; ++r) {
                if ((entry.mask & (1u << r)) != 0u) {
                    best = std::max(best, entry.share[static_cast<std::size_t>(r)]);
                }
            }
            upperLayerLoad += entry.influence * best;
        }
        active.push_back(std::move(entry));
    }
    // Apply low priority first so a stronger override lands on top.
    std::reverse(active.begin(), active.end());

    for (const ActiveCue& entry : active) {
        const PrimitivePose& primitive = entry.pose;
        const float w = entry.weight * entry.gate;
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            const Quat& delta = primitive.bone[b];
            if (std::abs(delta.w) > 0.999999f) {
                continue;
            }
            const HumanoidBone bone = static_cast<HumanoidBone>(b);
            const BodyMask boneBit = maskBitForBone(bone);
            if ((entry.mask & boneBit) == 0u) {
                continue;
            }
            const float wb = w * regionShare(entry, boneBit);
            if (wb <= 1.0e-5f) {
                continue;
            }
            if (entry.cue->effectiveBlend() == BlendMode::Override) {
                // Replace the masked bone: the cue's delta becomes the local
                // rotation, blended from the current base by the cue weight.
                out.pose.bone[b] = math::slerp(out.pose.bone[b], delta, wb);
            } else {
                out.pose.bone[b] = addDelta(out.pose.bone[b], delta, wb);
            }
            ++out.diagnostics.boneLayers[b];
        }
        out.pose.hips = out.pose.hips + primitive.hips * (w * regionShare(entry, body::Pelvis));
        if (options.allowFootwork && primitive.touchesFeet) {
            for (int f = 0; f < 2; ++f) {
                // Each foot follows its own swing weight, so a foot that is coming off the
                // floor lifts in step with its contact and never switches on at once.
                const float swing = 1.0f - plantedWeight(base, f);
                if (swing > 0.0f) {
                    const float wf =
                        entry.weight * swing *
                        regionShare(entry, f == 0 ? body::LeftFoot : body::RightFoot);
                    out.pose.foot[static_cast<std::size_t>(f)].position.y +=
                        primitive.footLift[static_cast<std::size_t>(f)] * wf;
                    out.pose.foot[static_cast<std::size_t>(f)].toePitch +=
                        primitive.toePitch[static_cast<std::size_t>(f)] * wf;
                }
            }
        }
        if (options.allowHands && (entry.mask & body::Hands) != 0u) {
            for (int sd = 0; sd < 2; ++sd) {
                if ((entry.mask & (sd == 0 ? body::LeftHand : body::RightHand)) == 0u) {
                    continue;
                }
                out.pose.handPose[static_cast<std::size_t>(sd)] = blendHandPose(
                    out.pose.handPose[static_cast<std::size_t>(sd)],
                    primitive.hand[static_cast<std::size_t>(sd)],
                    w * regionShare(entry, sd == 0 ? body::LeftHand : body::RightHand));
                out.pose.detailedHands = true;
            }
        }
        ++out.diagnostics.acceptedCues;
    }
    out.diagnostics.activeCues = static_cast<int>(active.size());
    for (const ActiveCue& entry : active) {
        out.diagnostics.maxWeight =
            std::max(out.diagnostics.maxWeight, entry.weight);
    }
    return out;
}

std::vector<PrimitiveCue> planPrimitiveOverlays(const HumanoidClip& clip,
                                                std::uint32_t seed, float density,
                                                bool includeHands) {
    std::vector<PrimitiveCue> cues;
    if (!(clip.beats >= 2.0f) || clip.keyCount < 2) {
        return cues;
    }
    density = std::clamp(density, 0.0f, 1.5f);
    if (density <= 1.0e-4f) {
        return cues;
    }
    Rng rng(static_cast<std::uint32_t>(hashString(clip.id)) ^ (seed * 2654435761u));
    const float beats = clip.beats;
    const int bar = std::max(1, clip.beatsPerBar);
    const float intensity = std::clamp(0.35f + clip.energy * 0.75f, 0.35f, 1.0f);
    int barIndex = 0;
    for (float b = 0.0f; b < beats - 1.0e-4f; b += static_cast<float>(bar), ++barIndex) {
        // Strong bar start: torso or shoulder accent. Which one alternates so
        // four bars are not four copies of the same gesture.
        if (rng.chance(0.55f + 0.25f * density)) {
            PrimitiveCue cue;
            cue.kind = (barIndex % 3 == 0) ? PrimitiveKind::ShoulderAccent
                                           : PrimitiveKind::ChestIsolation;
            cue.beat = b;
            cue.durationBeats = cue.metadata().defaultBeats;
            cue.intensity = intensity;
            cue.side = (barIndex % 2 == 0) ? 0 : 1;
            cue.reason = "bar accent";
            cues.push_back(cue);
        }
        // Off-beat gestures: hands and head, deliberately not on every beat.
        for (int beat = 0; beat < bar; ++beat) {
            const float at = b + static_cast<float>(beat);
            if (at >= beats) break;
            if (includeHands && rng.chance(0.32f + 0.30f * density)) {
                PrimitiveCue cue;
                const int pick = rng.range(0, 3);
                cue.kind = pick == 0 ? PrimitiveKind::HandFlick
                           : pick == 1 ? PrimitiveKind::FingerRipple
                           : pick == 2 ? PrimitiveKind::WristRoll
                                       : PrimitiveKind::FingerSpread;
                cue.beat = at + 0.5f;
                cue.durationBeats = cue.metadata().defaultBeats;
                cue.intensity = intensity * (0.7f + 0.3f * rng.unit());
                cue.side = (beat + barIndex) % 2;
                cue.reason = "off-beat hand layer";
                cues.push_back(cue);
            }
            if (rng.chance(0.22f + 0.20f * density)) {
                PrimitiveCue cue;
                cue.kind = rng.chance(0.5f) ? PrimitiveKind::HeadTilt
                                             : PrimitiveKind::LookDirection;
                cue.beat = at + 1.0f;
                cue.durationBeats = cue.metadata().defaultBeats;
                cue.intensity = intensity * 0.8f;
                cue.reason = "head answer";
                cues.push_back(cue);
            }
        }
        // A reach or wave every other bar, with anticipation before the bar.
        if (barIndex % 2 == 1 && rng.chance(0.5f + 0.2f * density)) {
            PrimitiveCue cue;
            cue.kind = rng.chance(0.6f) ? PrimitiveKind::ArmWave : PrimitiveKind::ArmReach;
            cue.beat = b + static_cast<float>(bar) - 0.5f;
            cue.durationBeats = cue.metadata().defaultBeats;
            cue.intensity = intensity * 0.9f;
            cue.side = (barIndex % 2 == 0) ? 1 : 0;
            cue.reason = "phrase arm gesture";
            cues.push_back(cue);
        }
    }
    // Authored accents are the strongest musical evidence. Put a short head or
    // chest accent exactly on them.
    for (const AccentMark& mark : clip.accents) {
        if (mark.strength < 0.55f) continue;
        PrimitiveCue cue;
        cue.kind = (mark.strength > 0.85f) ? PrimitiveKind::HeadAccent
                                           : PrimitiveKind::FistPulse;
        cue.beat = mark.beat;
        cue.durationBeats = cue.metadata().defaultBeats;
        cue.intensity = std::clamp(mark.strength * mark.confidence, 0.3f, 1.0f);
        cue.reason = "authored accent: " + mark.kind;
        cues.push_back(cue);
    }
    std::sort(cues.begin(), cues.end(), [](const PrimitiveCue& a, const PrimitiveCue& b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        return a.effectivePriority() < b.effectivePriority();
    });
    return cues;
}

}  // namespace bgmedia::motion
