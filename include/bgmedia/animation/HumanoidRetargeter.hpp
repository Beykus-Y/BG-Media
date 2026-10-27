#pragma once

// BG Motion -> humanoid bone retargeting.
//
// The input is the existing backend-independent CharacterPose. The output is a
// complete set of node globals in character space. Nothing here knows about a
// particular VRM model's node indices or proportions: every axis, bone length
// and bind transform is derived from the VRM humanoid mapping and the bind
// pose by HumanoidRetargeter's constructor.

#include "bgmedia/choreography/Choreography.hpp"
#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bgmedia::animation {

using vrm::HumanoidBone;
using math::Mat4;
using math::Quat;
using math::Vec3;

struct RetargetOptions {
    // Arms hang down from a VRM T-pose by this angle (radians). Positive
    // CharacterPose arm values swing toward the body; negative values swing
    // outward, matching the 2D BG Motion debug rig.
    float armHang = 0.32f;
    float armSwingGain = 1.05f;
    float elbowBase = 0.13f;
    float elbowGain = 0.85f;

    float hipBobScale = 0.055f;   // metres per unit bob, scaled by height
    float kneeDropScale = 0.025f; // extra hip drop for bent knees
    float leanMax = 0.17f;        // radians at |lean| = 1
    float twistMax = 0.17f;
    float headNodMax = 0.34f;
    float headTiltMax = 0.22f;
    float headTurnMax = 0.36f;

    bool footIK = true;
    bool idleMotion = true;
};

struct RetargetStats {
    bool valid = false;
    bool footIK = false;
    double retargetMilliseconds = 0.0;
    float height = 0.0f;
    float hipDrop = 0.0f;
    // Largest distance, on the *evaluated* skeleton, between a foot joint and
    // its requested target (reach deficit and solver error combined).
    float maxFootError = 0.0f;
    // Largest distance between the evaluated foot joint and the closest point
    // the leg chain can reach: error attributable to the solver/rig alone.
    float footSolverError = 0.0f;
    float lowestFootY = 0.0f;
    int resolvedBones = 0;
    int missingBones = 0;
    int fingerBones = 0;       // finger nodes present on the model
    bool detailedHands = false;  // per-finger pose was applied
    bool secondaryApplied = false;

    // BG Motion 3.0 (applyClip) diagnostics.
    bool clipDriven = false;
    float requestedHipDrop = 0.0f;  // authored, metres
    float reachHipDrop = 0.0f;      // extra drop added so planted feet are reachable
    float footReachDeficit = 0.0f;  // metres a target lay beyond the legs' reach (0 = ok)
};

struct RetargetedPose {
    // Character-space node globals. The renderer applies its own placement
    // (user scale/offset) and the model transform below.
    std::vector<Mat4> nodeGlobals;
    // Whole-character yaw from CharacterPose::rotation, in turns.
    Mat4 modelTransform = Mat4::identity();
    // BG Motion 3.0: the character root. The renderer places the model as
    //   T(rootOffset * scale) * T(rootPivot * scale) * modelTransform *
    //   T(-rootPivot * scale) * S
    // so root translation follows the user's model scale. Legacy poses leave
    // both at zero.
    Vec3 rootOffset{};
    Vec3 rootPivot{};
    RetargetStats stats;

    [[nodiscard]] Vec3 bonePosition(HumanoidBone bone,
                                    const vrm::VrmModel& model) const;
};

class HumanoidRetargeter {
public:
    explicit HumanoidRetargeter(const vrm::VrmModel& model);
    ~HumanoidRetargeter();
    HumanoidRetargeter(HumanoidRetargeter&&) noexcept;
    HumanoidRetargeter& operator=(HumanoidRetargeter&&) noexcept;
    HumanoidRetargeter(const HumanoidRetargeter&) = delete;
    HumanoidRetargeter& operator=(const HumanoidRetargeter&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] const std::string& error() const;

    // Deterministic: the output depends only on pose, mediaTime and options,
    // never on call order, wall time or render FPS.
    [[nodiscard]] RetargetedPose apply(
        const CharacterPose& pose, double mediaTime,
        const RetargetOptions& options = {}) const;

    // BG Motion 3.0: poses a model from a sampled dance clip. Foot targets are
    // stage-space, so planted feet stay planted while the root turns or moves.
    [[nodiscard]] RetargetedPose applyClip(
        const motion::ClipPose& pose, double mediaTime,
        const RetargetOptions& options = {}) const;

    // Stage-space (world, before the renderer's user scale) position of a node
    // of a pose produced by applyClip(): includes root yaw and translation.
    [[nodiscard]] static Vec3 stagePosition(const RetargetedPose& pose, int node);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bgmedia::animation
