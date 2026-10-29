#include "bgmedia/motion3/AliveLayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace bgmedia::motion {
namespace {

// Written out instead of M_PI: M_PI is not part of the standard and is not
// defined by MSVC unless _USE_MATH_DEFINES is set.
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kDegree = kTwoPi / 360.0;

// Clip hips are fractions of the model height. Centimetre figures below use the
// same 1.6 m reference body as the footwork planner.
constexpr double kReferenceHeightCm = 160.0;

// Breathing: a fixed 4 s period. An energy-dependent period would re-phase
// t / period every frame while energy moves and make the chest jitter.
constexpr double kBreathPeriodSeconds = 4.0;
constexpr double kUpperChestPitchDeg = 1.5;
constexpr double kChestPitchDeg = 0.75;
constexpr double kShoulderLiftDeg = 1.0;
constexpr double kBreathHipsCm = 0.3;

// Weight shift: hips x sway of 1 cm with a period between 7 and 11 s.
constexpr double kWeightShiftCm = 1.0;
constexpr double kWeightShiftMinSeconds = 7.0;
constexpr double kWeightShiftSpanSeconds = 4.0;

// Foot handover of the dance -> Idle blend. Foot positions are in model heights.
constexpr double kFootLiftCapHeights = 0.03;                      // about 4.8 cm at 1.6 m
constexpr double kFootLiftMinTravel = 1.0 / kReferenceHeightCm;   // 1 cm
constexpr double kFootLiftFraction = 0.5;                         // lift = 0.5 * travel, capped
// Half the distance between the bind ankles, in height units (left foot at +x). The
// retargeter turns foot targets around the ankle midpoint; the real width is model
// dependent, this typical value keeps the re-anchored stance within a few mm.
constexpr double kBindAnkleHalfWidth = 0.06;

// Head look-around: total yaw +-4 deg and pitch +-2 deg, split neck 40 % / head 60 %.
constexpr double kLookYawDeg = 4.0;
constexpr double kLookPitchDeg = 2.0;
constexpr double kNeckShare = 0.4;

// Look-around as saccades: the gaze holds a target for 1.5..4 s, then turns to the next
// one in 0.12..0.20 s. Saccade n starts at n * slot + offset(n), with offset(n) in
// 0..1 s, so the start times are closed form and the layer stays a pure function of
// wall time. Between two saccades the fixation is slot + offset(n+1) - offset(n) -
// duration(n), that is 1.55..3.63 s. Past kMaxWallTime the gaze freezes, so the slot
// index stays exact in an int64 (about 30 000 years).
constexpr double kSaccadeSlotSeconds = 2.75;
constexpr double kSaccadeOffsetSeconds = 1.0;
constexpr double kSaccadeMinSeconds = 0.12;
constexpr double kSaccadeSpanSeconds = 0.08;
constexpr double kMaxWallTime = 1.0e12;

// Head stabilisation: below the dancing level the head keeps (1 - share) of the torso's
// world yaw and pitch. The share is 50 % up to energy 0.35 and fades to 0 at 0.5.
constexpr double kStabiliseShare = 0.5;
constexpr double kStabiliseFullEnergy = 0.35;
constexpr double kStabiliseOffEnergy = 0.5;

// Spotting (head turns ahead of the body after a turn over 90 deg) is not in this
// layer: it is an event at the start of the turn, and the layer sees only the current
// pose, so a stateless spot cannot be derived from root yaw. The stabilisation above
// already holds half of a 180 deg spin in world space.

std::size_t boneIndex(HumanoidBone bone) {
    return static_cast<std::size_t>(bone);
}

bool finiteQuat(const math::Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
           std::isfinite(q.w);
}

// Integer hash (lowbias32). Maps a seed and a salt to a stable value in [0, 1).
std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

double unitHash(std::uint32_t seed, std::uint32_t salt) {
    const std::uint32_t mixed = hash32(seed ^ hash32(salt * 0x9e3779b9U + 0x7f4a7c15U));
    return static_cast<double>(mixed) / 4294967296.0;
}

// Fractional part of a cycle count, in [0, 1). Keeps the phase exact for very
// large wall times, where sin(2 pi t f) would lose all precision.
double wrapCycles(double cycles) {
    if (!std::isfinite(cycles)) {
        return 0.0;
    }
    double fraction = std::fmod(cycles, 1.0);
    if (fraction < 0.0) {
        fraction += 1.0;
    }
    return fraction >= 1.0 ? 0.0 : fraction;
}

// sin(2 pi (cycles + phase)) for a cycle count and a phase in cycles.
double wave(double cycles, double phase) {
    return std::sin(kTwoPi * (wrapCycles(cycles) + phase));
}

float smoothstep01(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Ease over [0, 1] with zero first and second derivatives at both ends. Exactly 0 at 0
// and exactly 1 at 1, so an endpoint test can use ==.
float smootherstep01(float x) {
    const float t = std::clamp(x, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// a * (1 - t) + b * t: exact at both ends (a at t = 0, b at t = 1). The usual
// a + (b - a) * t is not exact at t = 1 in floating point.
float exactLerp(float a, float b, float t) { return a * (1.0f - t) + b * t; }

// Shortest-arc lerp of a turn value. Exact at t = 0.
float turnLerp(float from, float to, float t) {
    float d = to - from;
    d -= std::round(d);
    return from + d * t;
}

math::Quat pitchQuat(double degrees) {
    return math::fromAxisAngle({1.0f, 0.0f, 0.0f},
                               static_cast<float>(degrees * kDegree));
}

math::Quat yawQuat(double degrees) {
    return math::fromAxisAngle({0.0f, 1.0f, 0.0f},
                               static_cast<float>(degrees * kDegree));
}

math::Quat rollQuat(double degrees) {
    return math::fromAxisAngle({0.0f, 0.0f, 1.0f},
                               static_cast<float>(degrees * kDegree));
}

// bone = delta * bone, the same composition the other overlays use.
void composeBone(ClipPose& pose, HumanoidBone bone, const math::Quat& delta) {
    math::Quat& target = pose.bone[boneIndex(bone)];
    if (!finiteQuat(target)) {
        return;
    }
    target = (delta * target).normalized();
}

// Hash in [0, 1) of a saccade index (an int64 slot number), a seed and a channel:
// 0 yaw target, 1 pitch target, 2 start offset, 3 duration.
double indexHash(std::uint32_t seed, std::int64_t index, std::uint32_t channel) {
    const auto u = static_cast<std::uint64_t>(index);
    const std::uint32_t folded =
        hash32(static_cast<std::uint32_t>(u) ^
               hash32(static_cast<std::uint32_t>(u >> 32) ^ 0x3c6ef372U));
    return unitHash(seed ^ folded, channel);
}

// A gaze target in the unit square: -1..1 of the yaw and pitch limits.
struct Gaze {
    double yaw = 0.0;
    double pitch = 0.0;
};

Gaze saccadeTarget(std::uint32_t seed, std::int64_t index) {
    return {2.0 * indexHash(seed, index, 0) - 1.0, 2.0 * indexHash(seed, index, 1) - 1.0};
}

double saccadeStart(std::uint32_t seed, std::int64_t index) {
    return static_cast<double>(index) * kSaccadeSlotSeconds +
           kSaccadeOffsetSeconds * indexHash(seed, index, 2);
}

double saccadeSpan(std::uint32_t seed, std::int64_t index) {
    return kSaccadeMinSeconds + kSaccadeSpanSeconds * indexHash(seed, index, 3);
}

// Gaze at a wall time. The latest saccade that has started is either the one of this
// slot or, when its offset is still ahead, the one of the slot before (a saccade ends
// before the next slot starts). The turn is a smoothstep from the previous target.
Gaze gazeAt(double wallTime, std::uint32_t seed) {
    const double t = std::clamp(wallTime, -kMaxWallTime, kMaxWallTime);
    const auto slot = static_cast<std::int64_t>(std::floor(t / kSaccadeSlotSeconds));
    const std::int64_t index = saccadeStart(seed, slot) > t ? slot - 1 : slot;
    const Gaze target = saccadeTarget(seed, index);
    const double progress =
        std::clamp((t - saccadeStart(seed, index)) / saccadeSpan(seed, index), 0.0, 1.0);
    if (progress >= 1.0) {
        return target;
    }
    const Gaze from = saccadeTarget(seed, index - 1);
    const double eased = progress * progress * (3.0 - 2.0 * progress);
    return {from.yaw + (target.yaw - from.yaw) * eased,
            from.pitch + (target.pitch - from.pitch) * eased};
}

// Yaw about +Y and pitch about +X (positive tilts the top forward) of a rotation, in
// degrees, read from its forward axis. Exact for yaw * pitch.
struct YawPitch {
    double yaw = 0.0;
    double pitch = 0.0;
};

YawPitch yawPitchOf(const math::Quat& q) {
    const math::Vec3 forward = math::rotate(q, {0.0f, 0.0f, 1.0f});
    return {std::atan2(static_cast<double>(forward.x), static_cast<double>(forward.z)) / kDegree,
            std::asin(std::clamp(-static_cast<double>(forward.y), -1.0, 1.0)) / kDegree};
}

// The `share` power of a rotation: the part of it a bone takes when the head is split
// between neck and head. Shortest arc, so the identity sits at 0.
math::Quat shareOf(const math::Quat& q, double share) {
    const math::Quat shortest = q.w < 0.0f ? math::Quat{-q.x, -q.y, -q.z, -q.w} : q;
    return math::slerp(math::Quat::identity(), shortest, static_cast<float>(share));
}

// Torso world orientation: the body-frame chain Hips, Spine, Chest, UpperChest, turned by
// the root yaw (the retargeter turns the whole model by rootYaw about up).
math::Quat torsoBodyRotation(const ClipPose& pose) {
    const math::Quat chain = pose.bone[boneIndex(HumanoidBone::Hips)] *
                             pose.bone[boneIndex(HumanoidBone::Spine)] *
                             pose.bone[boneIndex(HumanoidBone::Chest)] *
                             pose.bone[boneIndex(HumanoidBone::UpperChest)];
    return chain.normalized();
}

// Counter-rotates the head so it keeps (1 - amount) of the torso's world yaw and pitch.
// The correction is worked out in the torso frame, where the bones live, and split
// 40 % neck / 60 % head like the look, so the neck and head shares of the whole head
// motion stay the same. Does nothing when the torso or the root yaw is not finite.
void stabiliseHead(ClipPose& pose, double amount) {
    const math::Quat torso = torsoBodyRotation(pose);
    const double rootYaw = std::isfinite(pose.rootYaw) ? static_cast<double>(pose.rootYaw) : 0.0;
    if (!finiteQuat(torso) || amount <= 0.0) {
        return;
    }
    const math::Quat root = yawQuat(rootYaw * 360.0);
    const YawPitch world = yawPitchOf(root * torso);
    // Stage-frame counter-rotation, then conjugated into the torso frame:
    // torso^-1 * root^-1 * W * root * torso.
    const math::Quat stage = yawQuat(-amount * world.yaw) * pitchQuat(-amount * world.pitch);
    const math::Quat correction =
        (torso.conjugate() * (root.conjugate() * stage * root) * torso).normalized();
    composeBone(pose, HumanoidBone::Neck, shareOf(correction, kNeckShare));
    composeBone(pose, HumanoidBone::Head, shareOf(correction, 1.0 - kNeckShare));
}

} // namespace

float aliveEnergyScale(float energy) {
    const float e = std::isfinite(energy) ? std::clamp(energy, 0.0f, 1.0f) : 0.0f;
    return 1.0f - 0.7f * smoothstep01(0.02f, 0.4f, e);
}

void applyAlive(ClipPose& pose, double wallTime, float energy, std::uint32_t seed,
                float weight, float bodyWeight, float headWeight) {
    const float strength = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    if (strength <= 0.0f) {
        return;  // weight 0 is exactly the identity
    }
    const float bodyScale =
        std::isfinite(bodyWeight) ? std::clamp(bodyWeight, 0.0f, 1.0f) : 0.0f;
    const float headScale =
        std::isfinite(headWeight) ? std::clamp(headWeight, 0.0f, 1.0f) : 0.0f;
    const double energyScale = aliveEnergyScale(energy);
    // Breathing and weight shift, and the head look, get separate amplitudes. With the
    // defaults both are strength * energyScale, exactly as before.
    const double bodyAmplitude = static_cast<double>(strength) * bodyScale * energyScale;
    const double headAmplitude = static_cast<double>(strength) * headScale * energyScale;
    const double t = std::isfinite(wallTime) ? wallTime : 0.0;

    // --- breathing -----------------------------------------------------------
    if (bodyAmplitude > 0.0) {
        const double amplitude = bodyAmplitude;
        // Energy only scales the amplitude; the phase stays a plain function of wall time.
        const double breathPeriod = kBreathPeriodSeconds;
        // -1..1, inhale = positive. Positive pitch about +X tilts the top of the torso
        // forward, so the inhale (chest lifts and tips back) uses the negative sign.
        const double breath = wave(t / breathPeriod, unitHash(seed, 1));
        composeBone(pose, HumanoidBone::UpperChest,
                    pitchQuat(-kUpperChestPitchDeg * amplitude * breath));
        composeBone(pose, HumanoidBone::Chest,
                    pitchQuat(-kChestPitchDeg * amplitude * breath));
        // Shoulders lift together on the inhale: +Z roll lifts the left (+X) shoulder,
        // so the right (-X) shoulder takes the opposite sign.
        const double shoulder = kShoulderLiftDeg * amplitude * breath;
        composeBone(pose, HumanoidBone::LeftShoulder, rollQuat(shoulder));
        composeBone(pose, HumanoidBone::RightShoulder, rollQuat(-shoulder));
        if (std::isfinite(pose.hips.x) && std::isfinite(pose.hips.y) &&
            std::isfinite(pose.hips.z)) {
            const double hipsY = kBreathHipsCm / kReferenceHeightCm * amplitude * breath;
            pose.hips.y += static_cast<float>(hipsY);
        }

        // --- slow weight shift -------------------------------------------------
        // Hips only. The feet are IK targets in stage space (footIK), so the pelvis
        // moves over planted feet instead of dragging them.
        const double shiftPeriod =
            kWeightShiftMinSeconds + kWeightShiftSpanSeconds * unitHash(seed, 2);
        const double shift = wave(t / shiftPeriod, unitHash(seed, 3));
        if (std::isfinite(pose.hips.x)) {
            const double hipsX = kWeightShiftCm / kReferenceHeightCm * amplitude * shift;
            pose.hips.x += static_cast<float>(hipsX);
        }
    }

    // --- head look-around: saccades -------------------------------------------
    if (headAmplitude > 0.0) {
        const Gaze gaze = gazeAt(t, seed);
        const double yaw = kLookYawDeg * headAmplitude * gaze.yaw;
        const double pitch = kLookPitchDeg * headAmplitude * gaze.pitch;
        const math::Quat neck = yawQuat(kNeckShare * yaw) * pitchQuat(kNeckShare * pitch);
        const math::Quat head = yawQuat((1.0 - kNeckShare) * yaw) *
                                pitchQuat((1.0 - kNeckShare) * pitch);
        composeBone(pose, HumanoidBone::Neck, neck);
        composeBone(pose, HumanoidBone::Head, head);
    }

    // --- head stabilisation ---------------------------------------------------
    // Only while calm (energy below 0.5): when dancing the head follows the body. It is
    // part of the head, so the head weight scales it, not the energy amplitude.
    const float calmEnergy = std::isfinite(energy) ? std::clamp(energy, 0.0f, 1.0f) : 0.0f;
    const double calm = 1.0 - static_cast<double>(smoothstep01(
                                  static_cast<float>(kStabiliseFullEnergy),
                                  static_cast<float>(kStabiliseOffEnergy), calmEnergy));
    const double stabiliseAmount =
        kStabiliseShare * calm * static_cast<double>(strength) * static_cast<double>(headScale);
    stabiliseHead(pose, stabiliseAmount);
}

double coastTime(double stoppedMediaTime, double sinceStop) {
    // Not in the past: a time before the stop (or NaN) reads as the stop itself.
    if (!(sinceStop > 0.0)) {
        return stoppedMediaTime;
    }
    return stoppedMediaTime +
           kCoastStartSpeed * kCoastTauSeconds * (1.0 - std::exp(-sinceStop / kCoastTauSeconds));
}

double coastLagAfterResume(double lagAtResume, double sinceResume) {
    if (!(sinceResume > 0.0)) {
        return lagAtResume;
    }
    return lagAtResume * std::exp(-sinceResume / kCoastTauSeconds);
}

std::array<ClipPose::Foot, 2> idleFeetAtDanceRoot(const ClipPose& dance, const ClipPose& idle) {
    std::array<ClipPose::Foot, 2> idleFeet = idle.foot;
    if (dance.rootX == idle.rootX && dance.rootZ == idle.rootZ && dance.rootYaw == idle.rootYaw) {
        return idleFeet;  // same root: exactly the Idle feet
    }
    const double turn = (static_cast<double>(dance.rootYaw) - idle.rootYaw) * kTwoPi;
    const double c = std::cos(turn);
    const double sn = std::sin(turn);
    for (std::size_t f = 0; f < 2; ++f) {
        const double ankle = f == 0 ? kBindAnkleHalfWidth : -kBindAnkleHalfWidth;
        // Offset from the root, measured from the ankle midpoint like the retargeter.
        const double x = ankle + idle.foot[f].position.x - idle.rootX;
        const double z = static_cast<double>(idle.foot[f].position.z) - idle.rootZ;
        // Rotation about +y in clip axes (x left, y up, z forward; right handed).
        const double rx = c * x + sn * z;
        const double rz = -sn * x + c * z;
        if (std::isfinite(rx) && std::isfinite(rz)) {
            idleFeet[f].position.x = static_cast<float>(rx + dance.rootX - ankle);
            idleFeet[f].position.z = static_cast<float>(rz + dance.rootZ);
            idleFeet[f].yaw = idle.foot[f].yaw - idle.rootYaw + dance.rootYaw;
        }
    }
    return idleFeet;
}

int airborneFootFirst(const ClipPose& dance) {
    return dance.foot[1].contact < dance.foot[0].contact ? 1 : 0;
}

ClipPose blendToIdleKeepingFeet(const ClipPose& dance, const ClipPose& idle, float mix,
                                int firstFoot, const ClipPose* anchor) {
    const float m = std::isfinite(mix) ? std::clamp(mix, 0.0f, 1.0f) : 0.0f;
    if (m <= 0.0f) {
        return dance;
    }
    // Body: plain crossfade. The root is the dance root on both sides, so blendPoses
    // leaves it unchanged at every mix and the Idle origin never pulls a travelled
    // dancer back.
    ClipPose idleBody = idle;
    idleBody.rootX = dance.rootX;
    idleBody.rootZ = dance.rootZ;
    idleBody.rootYaw = dance.rootYaw;
    ClipPose out = blendPoses(dance, idleBody, m);

    // Idle's own targets sit around Idle's root; move them to the kept dance root so the
    // stance does not lean back to the origin or twist against the body's heading.
    // `anchor` (the pose the dancer stopped in, latched by the caller) keeps the Idle
    // stance fixed while a resumed dance root moves on.
    const std::array<ClipPose::Foot, 2> idleFeet =
        idleFeetAtDanceRoot(anchor ? *anchor : dance, idle);

    // Feet: one at a time. The foot with less contact in the dance pose (the airborne one)
    // lands first, over mix 0..0.5; the supporting foot moves over 0.5..1, once the other
    // foot is down. Ties go left first. At every mix one of the two feet has p = 0 or p = 1,
    // so it sits exactly on an endpoint.
    // The caller latches the order for a whole transition (firstFoot 0 or 1): a dance pose
    // that keeps moving during a resume must not swap the feet's intervals mid-way.
    const std::size_t first = static_cast<std::size_t>(
        firstFoot == 0 || firstFoot == 1 ? firstFoot : airborneFootFirst(dance));
    for (std::size_t f = 0; f < 2; ++f) {
        const float start = f == first ? 0.0f : 0.5f;
        const float p = smootherstep01((m - start) / 0.5f);
        // Guarded: sin(pi) is not exactly 0 in floating point, and the endpoints must stay exact.
        const float arch = (p > 0.0f && p < 1.0f)
                               ? static_cast<float>(std::sin(kTwoPi * 0.5 * p))
                               : 0.0f;
        const ClipPose::Foot& from = dance.foot[f];
        const ClipPose::Foot& to = idleFeet[f];
        ClipPose::Foot& foot = out.foot[f];

        Vec3 position{exactLerp(from.position.x, to.position.x, p),
                      exactLerp(from.position.y, to.position.y, p),
                      exactLerp(from.position.z, to.position.z, p)};
        const double dx = static_cast<double>(to.position.x) - from.position.x;
        const double dz = static_cast<double>(to.position.z) - from.position.z;
        const double travel = std::sqrt(dx * dx + dz * dz);
        // A foot that is already in the air just comes down; only a planted foot steps.
        // Both gates are smooth: on resume the dance pose moves again, so its contact and
        // the travel change under a foot that is mid hand-over, and a switch would pop.
        if (arch > 0.0f) {
            const double planted = smoothstep01(0.25f, 0.75f, from.contact);
            const double far = smoothstep01(static_cast<float>(0.5 * kFootLiftMinTravel),
                                             static_cast<float>(1.5 * kFootLiftMinTravel),
                                             static_cast<float>(travel));
            const double lift = std::min(kFootLiftCapHeights, kFootLiftFraction * travel);
            position.y += static_cast<float>(lift * planted * far) * arch;
        }
        foot.position = position;
        foot.yaw = turnLerp(from.yaw, to.yaw, p);
        foot.toePitch = exactLerp(from.toePitch, to.toePitch, p);
        foot.contact = std::min(exactLerp(from.contact, to.contact, p), 1.0f - arch);
        foot.phase = p < 0.5f ? from.phase : to.phase;
    }
    return out;
}

void IdleCrossfade::update(bool running, double wallNow) {
    if (running) {
        running_ = true;
    } else if (running_) {
        running_ = false;
        stoppedAt_ = wallNow;
    }
    const float target =
        (!running_ && wallNow - stoppedAt_ >= kHoldSeconds) ? 1.0f : 0.0f;
    if (target != to_) {
        from_ = mix(wallNow);
        to_ = target;
        start_ = wallNow;
        duration_ = to_ > 0.5f ? kToIdleSeconds : kToDanceSeconds;
    }
}

float IdleCrossfade::mix(double wallNow) const {
    if (!std::isfinite(wallNow) || duration_ <= 0.0) {
        return to_;
    }
    const double progress = std::clamp((wallNow - start_) / duration_, 0.0, 1.0);
    const auto s = static_cast<float>(progress * progress * (3.0 - 2.0 * progress));
    return from_ + (to_ - from_) * s;
}

} // namespace bgmedia::motion
