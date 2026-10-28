#include "bgmedia/motion3/TaskSpaceIntent.hpp"

#include <cstddef>

namespace bgmedia::motion {
namespace {

using vrm::HumanoidBone;

// Reference skeleton in height units. Every bind rotation is identity, so a
// clip quaternion (relative to the T-pose bind) composes directly along the
// chain. Left is +X; the right arm mirrors X. The offsets are a canonical
// adult proportion set and are deliberately NOT read from any model.
constexpr Vec3 kHipsBind{0.0f, 0.53f, 0.0f};
constexpr Vec3 kSpineOffset{0.0f, 0.05f, 0.0f};
constexpr Vec3 kChestOffset{0.0f, 0.08f, 0.0f};
constexpr Vec3 kUpperChestOffset{0.0f, 0.08f, 0.0f};
constexpr float kShoulderX = 0.04f;
constexpr float kUpperArmX = 0.06f;
constexpr float kUpperArmLength = 0.17f;  // shoulder joint to elbow
constexpr float kForearmLength = 0.15f;   // elbow to wrist
static_assert(kUpperArmLength + kForearmLength == kReferenceArmReach,
              "reach must equal the sum of the arm bones");

// Below this elbow offset from the shoulder-wrist axis the bend plane is undefined.
constexpr float kDegenerateBend = 1e-3f;
constexpr double kVelocityHalfStep = 1.0 / 64.0;  // beats

// A joint's world-space position and orientation, both in the clip frame.
struct Joint {
    Vec3 position{};
    Quat rotation{};
};

// Child joint: its offset is carried by the parent's accumulated rotation, and
// its own clip rotation is applied on the right (parent * local).
Joint attach(const Joint& parent, Vec3 offset, const Quat& local) {
    return {parent.position + rotate(parent.rotation, offset),
            (parent.rotation * local).normalized()};
}

// Detailed wrist layer. Same rotation and scales as HumanoidRetargeter::applyClip, which
// applies it after the hand bone, so the orientation describes the whole pre-IK pose.
Quat detailedWristRotation(const HandPose& pose) {
    return (math::fromAxisAngle({-1.0f, 0.0f, 0.0f}, pose.wrist[0] * 0.26f) *
            math::fromAxisAngle({0.0f, 1.0f, 0.0f}, pose.wrist[1] * 0.30f) *
            math::fromAxisAngle({0.0f, 0.0f, 1.0f}, pose.wrist[2] * 0.34f))
        .normalized();
}

ArmIntent armIntent(const Joint& chest, const Quat& chestInverse, Vec3 shoulder,
                    Vec3 elbow, const Joint& hand) {
    ArmIntent intent;
    intent.valid = true;
    intent.source = ArmIntentSource::Extracted;
    intent.wrist = rotate(chestInverse, hand.position - chest.position);
    intent.shoulder = rotate(chestInverse, shoulder - chest.position);
    intent.wristOrientation = (chestInverse * hand.rotation).normalized();

    const Vec3 reachVector = hand.position - shoulder;
    const float reachLength = reachVector.length();
    intent.reach = reachLength / kReferenceArmReach;

    // The bend direction is the elbow offset with the shoulder-wrist axis removed.
    Vec3 pole = elbow - shoulder;
    if (reachLength > kDegenerateBend) {
        const Vec3 axis = reachVector / reachLength;
        pole = pole - axis * dot(pole, axis);
    }
    const float poleLength = pole.length();
    if (poleLength > kDegenerateBend) {
        intent.elbowPole = rotate(chestInverse, pole / poleLength);
        intent.confidence = 1.0f;
    } else {
        intent.elbowPole = {0.0f, -1.0f, 0.0f};
        intent.confidence = 0.5f;
    }
    return intent;
}

} // namespace

math::Vec3 referenceShoulder(std::size_t side) {
    // Rest chest frame: the upper arm root is at the chest level plus the upper
    // chest, shoulder and upper arm offsets, mirrored for the right side.
    const Joint chest{kHipsBind + kSpineOffset + kChestOffset, Quat::identity()};
    const Joint upperChest = attach(chest, kUpperChestOffset, Quat::identity());
    const float sign = side == 0 ? 1.0f : -1.0f;
    const Joint shoulder = attach(upperChest, {sign * kShoulderX, 0.0f, 0.0f}, Quat::identity());
    const Joint upper = attach(shoulder, {sign * kUpperArmX, 0.0f, 0.0f}, Quat::identity());
    return upper.position - chest.position;
}

TaskSpaceIntent extractTaskSpaceIntent(const ClipPose& pose) {
    const auto boneRotation = [&pose](HumanoidBone bone) {
        return pose.bone[static_cast<std::size_t>(bone)].normalized();
    };

    // Root translation and yaw (pose.rootX / rootZ / rootYaw) are not used: the
    // intent describes the body, not where the body stands on stage.
    const Joint hips{kHipsBind + pose.hips, boneRotation(HumanoidBone::Hips)};
    const Joint spine = attach(hips, kSpineOffset, boneRotation(HumanoidBone::Spine));
    const Joint chest = attach(spine, kChestOffset, boneRotation(HumanoidBone::Chest));
    const Joint upperChest =
        attach(chest, kUpperChestOffset, boneRotation(HumanoidBone::UpperChest));
    const Quat chestInverse = chest.rotation.conjugate();

    TaskSpaceIntent out;
    for (std::size_t side = 0; side < 2; ++side) {
        const float sign = side == 0 ? 1.0f : -1.0f;
        const HumanoidBone shoulderBone =
            side == 0 ? HumanoidBone::LeftShoulder : HumanoidBone::RightShoulder;
        const HumanoidBone upperBone =
            side == 0 ? HumanoidBone::LeftUpperArm : HumanoidBone::RightUpperArm;
        const HumanoidBone lowerBone =
            side == 0 ? HumanoidBone::LeftLowerArm : HumanoidBone::RightLowerArm;
        const HumanoidBone handBone =
            side == 0 ? HumanoidBone::LeftHand : HumanoidBone::RightHand;

        const Joint shoulder = attach(upperChest, {sign * kShoulderX, 0.0f, 0.0f},
                                      boneRotation(shoulderBone));
        const Joint upper =
            attach(shoulder, {sign * kUpperArmX, 0.0f, 0.0f}, boneRotation(upperBone));
        const Joint lower = attach(upper, {sign * kUpperArmLength, 0.0f, 0.0f},
                                   boneRotation(lowerBone));
        Joint hand =
            attach(lower, {sign * kForearmLength, 0.0f, 0.0f}, boneRotation(handBone));
        if (pose.detailedHands) {
            hand.rotation =
                (hand.rotation * detailedWristRotation(pose.handPose[side])).normalized();
        }

        out.arm[side] = armIntent(chest, chestInverse, upper.position, lower.position, hand);
    }
    return out;
}

TaskSpaceIntent extractClipTaskSpaceIntent(const HumanoidClip& clip, double clipBeat) {
    ClipPose pose;
    sampleClip(clip, clipBeat, pose);
    TaskSpaceIntent out = extractTaskSpaceIntent(pose);

    sampleClip(clip, clipBeat - kVelocityHalfStep, pose);
    const TaskSpaceIntent before = extractTaskSpaceIntent(pose);
    sampleClip(clip, clipBeat + kVelocityHalfStep, pose);
    const TaskSpaceIntent after = extractTaskSpaceIntent(pose);

    const float perBeat = static_cast<float>(1.0 / (2.0 * kVelocityHalfStep));
    for (std::size_t side = 0; side < 2; ++side) {
        out.arm[side].wristVelocity =
            (after.arm[side].wrist - before.arm[side].wrist) * perBeat;
    }
    return out;
}

ArmIntent resolveArmIntent(const ArmIntent& explicitIntent, const ArmIntent& extracted) {
    if (explicitIntent.valid && explicitIntent.source == ArmIntentSource::Explicit) {
        return explicitIntent;
    }
    return extracted;
}

} // namespace bgmedia::motion
