#pragma once

// BG Motion -> humanoid bone retargeting.
//
// The input is the existing backend-independent CharacterPose. The output is a
// complete set of node globals in character space. Nothing here knows about a
// particular VRM model's node indices or proportions: every axis, bone length
// and bind transform is derived from the VRM humanoid mapping and the bind
// pose by HumanoidRetargeter's constructor.

#include "bgmedia/choreography/Choreography.hpp"
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
    float maxFootError = 0.0f;
    float lowestFootY = 0.0f;
    int resolvedBones = 0;
    int missingBones = 0;
};

struct RetargetedPose {
    // Character-space node globals. The renderer applies its own placement
    // (user scale/offset) and the model transform below.
    std::vector<Mat4> nodeGlobals;
    // Whole-character yaw from CharacterPose::rotation, in turns.
    Mat4 modelTransform = Mat4::identity();
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bgmedia::animation
