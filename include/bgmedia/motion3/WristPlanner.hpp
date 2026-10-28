#pragma once

// BG Motion 3.3 collision-free wrist trajectory planner (issue #5, synthetic-rig scope).
//
// The planner works in the chest frame used by TaskSpaceIntent: origin at the chest
// joint, height units. Input is a list of wrist keys (beat, chest-relative position).
// Output is a wrist path that keeps the hand sphere clear of a torso proxy and inside
// the arm's reach. Each segment between keys is checked by dense sampling (a swept
// test, not only its endpoints). A segment that penetrates the torso is replaced by a
// deterministic detour through one waypoint. If no detour is clear, the segment is
// shortened to its last clear point and flagged, so a constraint conflict is reported
// instead of hidden.
//
// Scope: the torso is a five-band elliptical cylinder (the same layout as
// CollisionProxyInfo). The hand is a sphere. Forearms, hair and legs are not checked
// yet. The planner does not modify any authored clip, primitive or source track. The
// result is a new path that a consumer samples by beat.
//
// Units: the torso proxy and the hand radius must use the same units as the keys
// (height units here). A calibrated proxy from HumanoidRetargeter is in metres and must
// be scaled by 1 / model height before it is used with this planner.

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/math/Math.hpp"

#include <cstddef>
#include <vector>

namespace bgmedia::motion {

// Torso band heights, chest-relative (waist to shoulders), in height units.
inline constexpr float kTorsoBottomY = -0.12f;
inline constexpr float kTorsoTopY = 0.08f;

// Synthetic torso proxy for tests and offline QA on the reference skeleton. Band
// values are in height units. A real model proxy must be scaled first (see above).
[[nodiscard]] animation::CollisionProxyInfo syntheticTorsoProxy();

struct WristKey {
    double beat = 0.0;
    math::Vec3 wrist{};     // chest-relative, height units
    math::Vec3 shoulder{};  // chest-relative shoulder joint at this key (reach sphere centre)
};

struct WristPlanOptions {
    float handRadius = 0.03f;       // hand sphere radius (height units)
    float margin = 0.01f;           // extra gap kept from the torso surface
    float sampleStep = 0.01f;       // spacing of the swept clearance test
    float reachLimit = 0.32f;       // shoulder-to-wrist limit (kReferenceArmReach)
    float detourOffset = 0.12f;     // first waypoint offset from the segment midpoint
    float detourCostPerMeter = 0.5f;  // penalty per unit of waypoint offset
    float directionHysteresis = 0.02f;  // cost of changing the detour direction between segments
};

// Approximate signed distance from a chest-relative point to the torso proxy surface
// (positive = outside). Horizontal section is a superellipse, interpolated between the
// bands; outside the band range the vertical excess is added.
[[nodiscard]] float torsoSignedDistance(const animation::CollisionProxyInfo& torso,
                                        math::Vec3 point);

// Smallest gap between the hand sphere and the torso along a polyline, sampled at
// `step`. Negative = penetration. Returns a large value when the torso is invalid.
[[nodiscard]] float polylineClearance(const animation::CollisionProxyInfo& torso,
                                      const std::vector<math::Vec3>& points, float step,
                                      float handRadius);

struct WristSegmentPlan {
    std::vector<math::Vec3> points;  // start, optional waypoint, end (or last clear point)
    bool detoured = false;
    bool attenuated = false;   // no clear route: the segment stops at its last clear point
    bool conflict = false;     // the start pose itself penetrates the torso
    int directionIndex = -1;   // candidate direction chosen for the detour, or -1
    float minClearance = 0.0f;
};

// Plans one segment from `start` to `end` around the torso. The shoulder moves linearly
// from `shoulderStart` to `shoulderEnd` along the segment. `previousDirection` is the
// detour direction chosen for the segment before (-1 = none); changing it costs
// `directionHysteresis`, which prevents flipping between equally valid detours.
[[nodiscard]] WristSegmentPlan planWristSegment(const animation::CollisionProxyInfo& torso,
                                                math::Vec3 shoulderStart,
                                                math::Vec3 shoulderEnd, math::Vec3 start,
                                                math::Vec3 end,
                                                const WristPlanOptions& options = {},
                                                int previousDirection = -1);

struct WristPlanReport {
    int segments = 0;
    int detoured = 0;
    int attenuated = 0;
    int conflicts = 0;
    int reachClamped = 0;        // keys moved onto the reach sphere before planning
    float directClearance = 0.0f;   // min clearance of the straight key polyline
    float plannedClearance = 0.0f;  // min clearance of the planned polyline
    float maxDetourOffset = 0.0f;   // largest waypoint distance from its segment
};

// Plans a whole key sequence. The returned keys keep the original beats; inserted
// waypoints get beats split by arc length within their segment. Attenuated segments
// end at their last clear point, and the next segment starts from there.
class WristPath {
public:
    std::vector<WristKey> keys;

    // Piecewise-linear sample at a beat. Clamps outside the key range.
    [[nodiscard]] math::Vec3 sample(double beat) const;
};

[[nodiscard]] WristPath planWristPath(const animation::CollisionProxyInfo& torso,
                                      const std::vector<WristKey>& keys,
                                      const WristPlanOptions& options = {},
                                      WristPlanReport* report = nullptr);

} // namespace bgmedia::motion
