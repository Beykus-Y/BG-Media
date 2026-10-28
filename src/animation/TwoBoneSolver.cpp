#include "bgmedia/animation/TwoBoneSolver.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::animation {
namespace {

constexpr float kTiny = 1.0e-6f;

// Sine of the angle between the pole and the reach line where the stable bend
// starts (pure rest bend) and where it ends (pure pole bend).
constexpr float kStableFrom = 0.15f;
constexpr float kStableTo = 0.40f;
// The same for the knee's lift, as a fraction of the mean bone length: a knee this
// close to straight takes its side from the rest bend, because the pole cannot flip
// it without moving the knee by about twice the lift. The solver stops 2 mm short of a
// full straight chain, so even a straight knee keeps a lift of about 8 % of the bone
// length on the reference rig.
constexpr float kStraightFrom = 0.10f;
constexpr float kStraightTo = 0.25f;

float smoothstep01(float x) {
    const float t = std::clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Unit bend side (perpendicular to direction). The pole decides it. With the stable
// bend, the pole hands over to the rest bend smoothly as the pole nears the reach line
// and as the knee nears straight (straightWeight 0), so the side does not flip on the
// pole's noise. Where the pole and the rest bend oppose each other mid-blend, the mix
// cancels and the rest bend is used: a point of the bend field that no blend can make
// continuous (see docs/BG_MOTION_3_3_SOFT_IK.md, section 5).
math::Vec3 bendSide(math::Vec3 direction, math::Vec3 pole, const TwoBoneChain& chain,
                    bool stable, float straightWeight, float poleEpsilon, bool& blended) {
    const auto perp = [&](math::Vec3 v) {
        return v - direction * math::dot(v, direction);
    };
    if (stable) {
        const math::Vec3 rest = perp(chain.restBend);
        const float restLength2 = rest.lengthSquared();
        const float poleLength = pole.length();
        if (restLength2 > 1.0e-8f && poleLength > kTiny) {
            const math::Vec3 poleOff = perp(pole);
            const float sine = std::sqrt(poleOff.lengthSquared()) / poleLength;
            const float w = smoothstep01((sine - kStableFrom) / (kStableTo - kStableFrom)) *
                            straightWeight;
            if (w < 1.0f) {
                blended = true;
            }
            const math::Vec3 restUnit = rest / std::sqrt(restLength2);
            if (w <= 0.0f) {
                return restUnit;
            }
            if (w >= 1.0f) {
                return math::normalize(poleOff);
            }
            const math::Vec3 mixed = math::normalize(poleOff) * w + restUnit * (1.0f - w);
            return mixed.lengthSquared() > 1.0e-8f ? math::normalize(mixed) : restUnit;
        }
    }
    // BG Motion 3.2 behaviour: the pole, then the right and forward axes.
    math::Vec3 bend = perp(pole);
    if (bend.lengthSquared() < poleEpsilon) {
        bend = math::cross(chain.right, direction);
    }
    if (bend.lengthSquared() < poleEpsilon) {
        bend = math::cross(chain.forward, direction);
    }
    return math::normalize(bend);
}

} // namespace

float softReachDistance(float distance, float lo, float hi, float soft) {
    const float s = std::min(soft, 0.5f * (hi - lo));
    if (!(s > kTiny)) {
        return std::clamp(distance, lo, hi);
    }
    const float kneeHi = hi - s;
    const float kneeLo = lo + s;
    if (distance > kneeHi) {
        return kneeHi + s * (1.0f - std::exp(-(distance - kneeHi) / s));
    }
    if (distance < kneeLo) {
        return kneeLo - s * (1.0f - std::exp(-(kneeLo - distance) / s));
    }
    return distance;
}

float distanceAtFlexion(float upper, float lower, float flexion) {
    const float c = std::cos(std::clamp(flexion, 0.0f, math::kPi));
    return std::sqrt(std::max(0.0f, upper * upper + lower * lower + 2.0f * upper * lower * c));
}

TwoBoneSolution solveTwoBone(math::Vec3 root, math::Vec3 target, math::Vec3 pole,
                             const TwoBoneChain& chain, const TwoBoneLimits& limits) {
    TwoBoneSolution out;
    const float l1 = chain.upper;
    const float l2 = chain.lower;
    const float maxReach = l1 + l2 - 0.002f;
    const float minReach = std::abs(l1 - l2) + 0.002f;

    // A target on the root has no direction: hang the chain down (a unit vector;
    // BG Motion 3.2 divided the fallback by kTiny here, which made it 1e6 long).
    const math::Vec3 reach = target - root;
    float distance = reach.length();
    math::Vec3 direction = distance < kTiny ? -chain.up : reach / distance;
    if (distance < kTiny) {
        distance = kTiny;
    }
    out.direction = direction;
    out.requestedDistance = distance;

    // Hard range, narrowed by the flexion window when it is enforced.
    float lo = minReach;
    float hi = maxReach;
    if (limits.flexWindow) {
        const float windowLo = std::max(minReach, distanceAtFlexion(l1, l2, limits.flexMax));
        const float windowHi = std::min(maxReach, distanceAtFlexion(l1, l2, limits.flexMin));
        if (windowLo <= windowHi) {
            out.flexBound = (distance < windowLo && windowLo > minReach) ||
                            (distance > windowHi && windowHi < maxReach);
            if (limits.flexEnforced) {
                lo = windowLo;
                hi = windowHi;
            }
        }
    }
    out.beyondReach = distance > maxReach;

    const float hard = std::clamp(distance, lo, hi);
    distance = softReachDistance(distance, lo, hi, limits.softReach);
    out.softShift = distance - hard;
    out.solvedDistance = distance;
    out.end = root + direction * distance;

    const float along = (l1 * l1 - l2 * l2 + distance * distance) / (2.0f * distance);
    const float lift = std::sqrt(std::max(0.0f, l1 * l1 - along * along));
    const float meanLength = std::max(kTiny, 0.5f * (l1 + l2));
    const float straightWeight = smoothstep01((lift / meanLength - kStraightFrom) /
                                              (kStraightTo - kStraightFrom));
    const math::Vec3 bend = bendSide(direction, pole, chain, limits.stableBend, straightWeight,
                                     limits.poleEpsilon,
                                     out.bendBlended);
    out.knee = root + direction * along + bend * lift;
    return out;
}

} // namespace bgmedia::animation
