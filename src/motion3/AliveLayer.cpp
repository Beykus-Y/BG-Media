#include "bgmedia/motion3/AliveLayer.hpp"

#include <algorithm>
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

// Head look-around: total yaw +-4 deg and pitch +-2 deg, split neck 40 % / head 60 %.
constexpr double kLookYawDeg = 4.0;
constexpr double kLookPitchDeg = 2.0;
constexpr double kNeckShare = 0.4;

// Sum of three incommensurate sines per axis. The weights sum to 1, so the
// sum never leaves [-1, 1] and the look angles stay within their limits.
constexpr double kYawFrequencyHz[3] = {0.083, 0.147, 0.229};
constexpr double kYawWeight[3] = {0.5, 0.3, 0.2};
constexpr double kPitchFrequencyHz[3] = {0.113, 0.191, 0.263};
constexpr double kPitchWeight[3] = {0.45, 0.35, 0.2};

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

// Head look-around for one axis: a weighted sum of three sines, in [-1, 1].
double lookSum(const double (&frequencies)[3], const double (&weights)[3],
               double wallTime, std::uint32_t seed, std::uint32_t salt) {
    double sum = 0.0;
    for (std::uint32_t i = 0; i < 3; ++i) {
        // Seed-dependent jitter of +-8 % keeps every character's rhythm distinct
        // while the frequencies stay inside 0.07..0.3 Hz.
        const double jitter = 0.92 + 0.16 * unitHash(seed, salt + i);
        const double frequency = frequencies[i] * jitter;
        const double phase = unitHash(seed, salt + 8 + i);
        sum += weights[i] * wave(wallTime * frequency, phase);
    }
    return sum;
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

    // --- head micro look-around ----------------------------------------------
    if (headAmplitude > 0.0) {
        const double yaw =
            kLookYawDeg * headAmplitude * lookSum(kYawFrequencyHz, kYawWeight, t, seed, 16);
        const double pitch =
            kLookPitchDeg * headAmplitude * lookSum(kPitchFrequencyHz, kPitchWeight, t, seed, 40);
        const math::Quat neck = yawQuat(kNeckShare * yaw) * pitchQuat(kNeckShare * pitch);
        const math::Quat head = yawQuat((1.0 - kNeckShare) * yaw) *
                                pitchQuat((1.0 - kNeckShare) * pitch);
        composeBone(pose, HumanoidBone::Neck, neck);
        composeBone(pose, HumanoidBone::Head, head);
    }
}

ClipPose blendToIdleKeepingFeet(const ClipPose& dance, const ClipPose& idle, float mix) {
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

    // Feet: one at a time. Foot 0 (left) moves over mix 0..0.5, foot 1 (right) over 0.5..1.
    // At every mix one of the two feet has p = 0 or p = 1, so it sits exactly on an endpoint.
    for (std::size_t f = 0; f < 2; ++f) {
        const float start = f == 0 ? 0.0f : 0.5f;
        const float p = smootherstep01((m - start) / 0.5f);
        // Guarded: sin(pi) is not exactly 0 in floating point, and the endpoints must stay exact.
        const float arch = (p > 0.0f && p < 1.0f)
                               ? static_cast<float>(std::sin(kTwoPi * 0.5 * p))
                               : 0.0f;
        const ClipPose::Foot& from = dance.foot[f];
        const ClipPose::Foot& to = idle.foot[f];
        ClipPose::Foot& foot = out.foot[f];

        Vec3 position{exactLerp(from.position.x, to.position.x, p),
                      exactLerp(from.position.y, to.position.y, p),
                      exactLerp(from.position.z, to.position.z, p)};
        const double dx = static_cast<double>(to.position.x) - from.position.x;
        const double dz = static_cast<double>(to.position.z) - from.position.z;
        const double travel = std::sqrt(dx * dx + dz * dz);
        if (travel >= kFootLiftMinTravel && arch > 0.0f) {
            const double lift = std::min(kFootLiftCapHeights, kFootLiftFraction * travel);
            position.y += static_cast<float>(lift) * arch;
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
