#pragma once

// BG Motion 3.2 stance-level footwork pre-pass for AUTHORED clips.
//
// An authored clip stores stage-space foot tracks and a contact schedule. On some
// stretches the two disagree with the body: a foot is planted where the other leg is,
// or a swing foot is dragged straight through the standing leg. The pre-pass finds those
// stretches on the baked tracks and re-plans them as real steps:
//   * the swing/stance structure comes from the contact schedule (cyclic, so a stance
//     that spans the loop seam is one stance);
//   * for each swing the landing point, the swing path (a side bend) and the lift are
//     chosen so the leg proxy of the swinging leg clears the other leg for the whole
//     swing and for the whole stance that follows;
//   * every stance gets ONE offset that is held for its entire duration, so a planted foot
//     cannot slide; the offset is blended in only while the foot is in the air, with
//     smootherstep (zero velocity and acceleration at both ends) and a sin^2 side bend,
//     so position and velocity are continuous at take-off and landing.
// Contact times, beats and every non-foot track are untouched, so musical markers and the
// clip phase are preserved. Clips whose legs never touch are returned unchanged. The
// result is a copy; the authored clip is never modified. Deterministic.

#include "bgmedia/motion3/DanceClip.hpp"

#include <vector>

namespace bgmedia::motion {

// Lower body in centimetres at a 160 cm reference height (same units as the procedural
// footwork planner). Scale a model's measurements by 160 / height.
struct LegGeometry {
    float hipHalf = 8.5f;   // hip joint to the pelvis centre
    float hipY = 88.0f;     // hip height above the floor
    float ankleHalf = 7.0f; // bind ankle to the pelvis centre
    float ankleY = 7.0f;    // ankle height above the floor
    float thighR = 7.0f;
    float kneeR = 5.4f;
    float ankleR = 4.4f;
    float footLen = 13.0f;  // ankle to toe
    float footR = 3.6f;
    float legLen = 0.0f;    // thigh + shin bone length; 0 = unknown (straight-leg estimate)
};

struct FootworkPlanOptions {
    LegGeometry legs;
    float minGapCm = 2.5f;       // required clearance between the legs
    float maxShiftCm = 26.0f;    // largest landing change
    int maxSweeps = 4;
};

struct FootworkPlanReport {
    bool changed = false;
    int keys = 0;
    int violationKeysBefore = 0;   // keys whose clearance is below minGapCm
    int violationKeysAfter = 0;
    float minClearanceBeforeCm = 0.0f;
    float minClearanceAfterCm = 0.0f;
    int swings = 0;
    int stances = 0;
    int stancesMoved = 0;
    int unresolved = 0;            // violations that no candidate cleared
    float maxLandingShiftCm = 0.0f;
    float maxSideBendCm = 0.0f;
    float maxExtraLiftCm = 0.0f;
    float maxPelvisShiftCm = 0.0f;   // weight-transfer shift of the pelvis
    std::vector<int> unresolvedKeys;  // key indices still below the gap (diagnostics)
};

[[nodiscard]] HumanoidClip planClipFootwork(const HumanoidClip& clip,
                                            const FootworkPlanOptions& options = {},
                                            FootworkPlanReport* report = nullptr);

}  // namespace bgmedia::motion
