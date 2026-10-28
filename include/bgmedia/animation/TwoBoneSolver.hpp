#pragma once

// BG Motion 3.3 two-bone solver: the single analytic IK that every leg and the
// arm collision re-solve of HumanoidRetargeter use. It is pure math: no model,
// no frame state, no previous pose. Its output depends only on its arguments.
//
// With default TwoBoneLimits the solve is the BG Motion 3.2 hard clamp, bit for
// bit. The soft reach, the stable bend and the flexion window are opt-in.

#include "bgmedia/math/Math.hpp"

namespace bgmedia::animation {

// Rest-pose geometry of one limb. Everything is derived from the bind pose by the
// retargeter; nothing here is a hardcoded axis.
struct TwoBoneChain {
    float upper = 0.0f;  // first bone (hip -> knee, shoulder -> elbow), metres
    float lower = 0.0f;  // second bone (knee -> ankle, elbow -> wrist), metres
    // Bend direction of the rest pose: the joint's offset from the bind reach line,
    // in model space. Zero when unknown (the solver then uses the pole only).
    math::Vec3 restBend{};
    // Model axes, used only for degenerate fallbacks.
    math::Vec3 up{0.0f, 1.0f, 0.0f};
    math::Vec3 right{1.0f, 0.0f, 0.0f};
    math::Vec3 forward{0.0f, 0.0f, 1.0f};
};

// Opt-in limits of one solve. The defaults reproduce BG Motion 3.2.
struct TwoBoneLimits {
    // Soft band, metres, just inside both reach limits. 0 = hard clamp.
    float softReach = 0.0f;
    // Blend the pole's bend direction toward the rest bend as the pole nears the
    // reach line, so a nearly straight or folded limb keeps a stable bend side.
    bool stableBend = false;
    // Strength of that blend, 0..1. 0 keeps the BG Motion 3.2 pole rule; the arm
    // collision re-solve fades it in with collision depth so contact does not snap
    // the elbow.
    float stableBendWeight = 1.0f;
    // Flexion window: the angle between the two bones (0 = straight), in radians.
    // Converted to a distance window by the law of cosines, so it does not depend
    // on the rig's axes.
    bool flexWindow = false;
    float flexMin = 0.0f;
    float flexMax = 3.14159265f;
    // false: the window is measured and reported (flexBound) but not applied.
    bool flexEnforced = true;
    // Squared length of the pole's projection (perpendicular to the reach line) below
    // which the pole is treated as absent and the fallback axes are used. Legs use 1e-8;
    // the arm collision re-solve uses 1e-10, its BG Motion 3.2 threshold.
    float poleEpsilon = 1.0e-8f;
};

struct TwoBoneSolution {
    math::Vec3 direction{};    // unit, root -> requested target
    math::Vec3 knee{};         // solved middle joint
    math::Vec3 end{};          // solved end joint: root + direction * solvedDistance
    float requestedDistance = 0.0f;
    float solvedDistance = 0.0f;
    float softShift = 0.0f;    // solvedDistance - hard-clamped distance (0 = soft band idle)
    bool beyondReach = false;  // requested distance lies beyond the hard reach
    bool flexBound = false;    // the flexion window bounds the requested distance
    bool bendBlended = false;  // the bend was blended toward the rest bend
};

// Soft reach: maps a requested distance into [lo, hi] (lo <= hi). The map is the
// identity between lo + soft and hi - soft, and beyond those knees it approaches
// the limit exponentially:
//   d' = hi - soft + soft * (1 - exp(-(d - (hi - soft)) / soft))   for d > hi - soft
// The first derivative is 1 at both knees (C1), the map is monotone, and it never
// reaches the limit. soft is clamped to half the interval. soft <= 0 is a hard clamp.
[[nodiscard]] float softReachDistance(float distance, float lo, float hi, float soft);

// Distance between the ends of a chain whose two bones make the given angle.
// flexion 0 is the straight chain (upper + lower), pi is fully folded.
[[nodiscard]] float distanceAtFlexion(float upper, float lower, float flexion);

// Solves the chain from root toward target. pole chooses the side the middle joint
// bends toward. Degenerate targets are handled deterministically: a target at the
// root uses -up, a pole parallel to the reach line uses the rest bend, then the
// model's right and forward axes.
[[nodiscard]] TwoBoneSolution solveTwoBone(math::Vec3 root, math::Vec3 target,
                                           math::Vec3 pole, const TwoBoneChain& chain,
                                           const TwoBoneLimits& limits = {});

} // namespace bgmedia::animation
