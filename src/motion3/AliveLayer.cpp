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
                float weight) {
    const float strength = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    if (strength <= 0.0f) {
        return;  // weight 0 is exactly the identity
    }
    const double amplitude = static_cast<double>(strength) * aliveEnergyScale(energy);
    const double t = std::isfinite(wallTime) ? wallTime : 0.0;

    // --- breathing -----------------------------------------------------------
    // Energy only scales the amplitude; the phase stays a plain function of wall time.
    const double breathPeriod = kBreathPeriodSeconds;
    // -1..1, inhale = positive. Positive pitch about +X tilts the top of the torso
    // forward, so the inhale (chest lifts and tips back) uses the negative sign.
    const double breath = wave(t / breathPeriod, unitHash(seed, 1));
    composeBone(pose, HumanoidBone::UpperChest,
                pitchQuat(-kUpperChestPitchDeg * amplitude * breath));
    composeBone(pose, HumanoidBone::Chest, pitchQuat(-kChestPitchDeg * amplitude * breath));
    // Shoulders lift together on the inhale: +Z roll lifts the left (+X) shoulder,
    // so the right (-X) shoulder takes the opposite sign.
    const double shoulder = kShoulderLiftDeg * amplitude * breath;
    composeBone(pose, HumanoidBone::LeftShoulder, rollQuat(shoulder));
    composeBone(pose, HumanoidBone::RightShoulder, rollQuat(-shoulder));
    if (std::isfinite(pose.hips.x) && std::isfinite(pose.hips.y) && std::isfinite(pose.hips.z)) {
        const double hipsY = kBreathHipsCm / kReferenceHeightCm * amplitude * breath;
        pose.hips.y += static_cast<float>(hipsY);
    }

    // --- slow weight shift ---------------------------------------------------
    // Hips only. The feet are IK targets in stage space (footIK), so the pelvis
    // moves over planted feet instead of dragging them.
    const double shiftPeriod =
        kWeightShiftMinSeconds + kWeightShiftSpanSeconds * unitHash(seed, 2);
    const double shift = wave(t / shiftPeriod, unitHash(seed, 3));
    if (std::isfinite(pose.hips.x)) {
        const double hipsX = kWeightShiftCm / kReferenceHeightCm * amplitude * shift;
        pose.hips.x += static_cast<float>(hipsX);
    }

    // --- head micro look-around ----------------------------------------------
    const double yaw = kLookYawDeg * amplitude * lookSum(kYawFrequencyHz, kYawWeight, t, seed, 16);
    const double pitch =
        kLookPitchDeg * amplitude * lookSum(kPitchFrequencyHz, kPitchWeight, t, seed, 40);
    const math::Quat neck = yawQuat(kNeckShare * yaw) * pitchQuat(kNeckShare * pitch);
    const math::Quat head = yawQuat((1.0 - kNeckShare) * yaw) *
                            pitchQuat((1.0 - kNeckShare) * pitch);
    composeBone(pose, HumanoidBone::Neck, neck);
    composeBone(pose, HumanoidBone::Head, head);
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
