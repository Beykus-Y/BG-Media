#pragma once

// BG Motion 3.3 continuous elbow swivel solver (issue #6, synthetic-rig scope).
//
// For a shoulder S and a wrist W, the elbow lies on a circle (the swivel circle)
// around the S-W axis. Its angle psi around that axis is the free parameter. Each
// elbow angle is measured from the preferred bend direction (ArmIntent::elbowPole),
// so psi = 0 bends the way the intent asks for.
//
// The solver does not look at the previous rendered frame. It plans a whole curve
// offline, over a list of samples with known beats, by dynamic programming (Viterbi)
// over a fixed candidate set per sample. The cost combines:
//   * preference: bending away from the preferred pole;
//   * clearance: the upper arm and forearm must stay clear of the torso proxy (the
//     same synthetic torso as WristPlanner); the shoulder joint itself is excluded;
//   * continuity: elbow direction changes between samples are penalised, and changes
//     above pi/2 (flips) cost extra.
// The curve is a pure function of its inputs: it is the same at any frame rate, for
// any seek and in any call order. Sampling a curve interpolates stored values only.
//
// Degenerate cases are explicit, not silent: a fully extended or folded arm, a pole
// collinear with the S-W axis, and a wrist beyond reach. Each is counted in the report.
// The planner's reach sphere is the reference arm length (kReferenceArmReach).
//
// Units: height units, chest-relative, as in TaskSpaceIntent and WristPlanner.

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/math/Math.hpp"
#include "bgmedia/motion3/TaskSpaceIntent.hpp"

#include <vector>

namespace bgmedia::motion {

struct ElbowSample {
    double beat = 0.0;
    math::Vec3 shoulder{};  // chest-relative shoulder joint
    math::Vec3 wrist{};     // chest-relative wrist, on the planned path
    math::Vec3 pole{};      // preferred bend direction (ArmIntent::elbowPole)
};

struct ElbowSolveOptions {
    int candidates = 16;            // swivel angles tried per sample
    float armRadius = 0.03f;        // upper arm and forearm radius (height units)
    float margin = 0.01f;           // extra gap kept from the torso surface
    int armSamples = 4;             // clearance sample points per bone (shoulder excluded)
    float poleWeight = 0.2f;        // cost of bending away from the preferred pole
    float clearanceWeight = 40.0f;  // cost per unit of penetration
    float smoothWeight = 1.0f;      // cost per radian of elbow direction change
    float flipWeight = 4.0f;        // extra cost for a change above pi/2
    float upperArm = kReferenceUpperArm;
    float forearm = kReferenceForearm;
};

struct ElbowSolveReport {
    int samples = 0;
    int degenerateSamples = 0;     // straight, folded or pole-collinear: swivel undefined
    int clampedSamples = 0;        // wrist outside the reach sphere, moved onto it
    float minClearanceBefore = 0.0f;  // pole-only elbow (psi = 0 each sample)
    float minClearanceAfter = 0.0f;   // the solved curve
    float maxSwivelStep = 0.0f;       // largest elbow direction change between samples (rad)
    float maxAngularSpeed = 0.0f;     // largest change per beat (rad / beat)
    int flips = 0;                    // changes above pi/2
};

// Elbow position for swivel angle `swivel` (radians) on the two-bone locus. The result
// keeps both bone lengths exactly; `degenerate` is set when the swivel is undefined.
[[nodiscard]] math::Vec3 elbowPosition(math::Vec3 shoulder, math::Vec3 wrist, float swivel,
                                       math::Vec3 pole, float upperArm, float forearm,
                                       bool* degenerate = nullptr);

// Solved elbow curve. Stores only the bend direction (unit, chest frame) per sample: the
// side the elbow bends to. The elbow itself is rebuilt on the two-bone locus for the wrist
// the consumer plans at that beat, so both bones keep their length at every frame.
class ElbowSwivelCurve {
public:
    std::vector<double> beats;
    std::vector<math::Vec3> bend;

    // Bend direction at a beat. Interpolated on the sphere; antipodal neighbours turn
    // along a fixed rotation path, so the direction never collapses to zero.
    [[nodiscard]] math::Vec3 bendDirection(double beat) const;

    // Elbow position for the wrist at that beat. Both bone lengths are exact whenever the
    // wrist is within reach (out-of-reach wrists are clamped, as in elbowPosition).
    [[nodiscard]] math::Vec3 elbow(double beat, math::Vec3 shoulder, math::Vec3 wrist,
                                   float upperArm, float forearm) const;
};

[[nodiscard]] ElbowSwivelCurve solveElbowSwivel(const animation::CollisionProxyInfo& torso,
                                                const std::vector<ElbowSample>& samples,
                                                const ElbowSolveOptions& options = {},
                                                ElbowSolveReport* report = nullptr);

} // namespace bgmedia::motion
