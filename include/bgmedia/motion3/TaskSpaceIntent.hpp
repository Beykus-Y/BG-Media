#pragma once

// BG Motion 3.3 task-space gesture intent bridge.
//
// Authored clips and motion primitives describe the arms in JOINT space: one
// quaternion per bone. A planner that routes a hand around the body needs the
// same arm as a TASK: where the wrist is, how the hand is oriented and which way
// the elbow bends. This header extracts that description by forward kinematics
// (FK) on a fixed reference skeleton that does not depend on any VRM model.
//
// The extraction is a pure function of the sampled ClipPose, taken before any
// IK or collision correction, so:
//   * legacy clips and all motion primitives stay playable unchanged; nothing
//     is rewritten and no source track is touched;
//   * the result does not depend on the model, stage position, root yaw or the
//     render frame rate, only on the pose (and, for the clip overload, the beat);
//   * corrected poses are never fed back into intent generation.
//
// Frame: the chest joint is the origin and the axes are the chest's own
// orientation, so a hand held in front of the sternum stays there while the
// spine bends. Lengths are fractions of the model height. Clip frame: +X =
// character left, +Y = up, +Z = forward (right handed).

#include "bgmedia/motion3/DanceClip.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace bgmedia::motion {

// Upper arm plus forearm length of the reference skeleton (height units).
// ArmIntent::reach is the shoulder-to-wrist distance divided by this value.
inline constexpr float kReferenceArmReach = 0.32f;

enum class ArmIntentSource : std::uint8_t {
    None,       // no intent (invalid arm)
    Extracted,  // derived by FK from a sampled clip or primitive pose
    Explicit    // stated directly by a future primitive; overrides Extracted
};

struct ArmIntent {
    bool valid = false;
    ArmIntentSource source = ArmIntentSource::None;

    Vec3 wrist{};            // chest-relative wrist position, height units
    Vec3 shoulder{};         // chest-relative shoulder joint of this pose (moves with the clavicle)
    Quat wristOrientation{}; // hand orientation in the chest frame
    Vec3 elbowPole{0.0f, -1.0f, 0.0f};  // unit: direction the elbow bends
    float reach = 0.0f;      // shoulder-to-wrist distance / kReferenceArmReach
    Vec3 wristVelocity{};    // chest-relative, height units per beat (clip overload only)
    // 1 = a well-defined bend plane; 0.5 = the arm is straight or folded, so the
    // elbow pole is the fallback (0, -1, 0) in the chest frame.
    float confidence = 0.0f;
};

struct TaskSpaceIntent {
    std::array<ArmIntent, 2> arm{};  // [0] left, [1] right
};

// Shoulder joint (upper arm root) of the reference skeleton, chest-relative, in
// height units. side 0 = left, 1 = right. Constant: the chest frame is rigid.
[[nodiscard]] math::Vec3 referenceShoulder(std::size_t side);

// FK on the reference skeleton for one sampled pose. Velocity is left at zero.
[[nodiscard]] TaskSpaceIntent extractTaskSpaceIntent(const ClipPose& pose);

// Samples the clip at `clipBeat` and at +/- half a key around it, so the result
// also carries the chest-relative wrist velocity. Deterministic in the beat.
[[nodiscard]] TaskSpaceIntent extractClipTaskSpaceIntent(const HumanoidClip& clip,
                                                         double clipBeat);

// An explicit intent (a primitive that states its hand target) wins over the
// extracted fallback whenever it is valid. Otherwise the extracted one is kept.
[[nodiscard]] ArmIntent resolveArmIntent(const ArmIntent& explicitIntent,
                                         const ArmIntent& extracted);

} // namespace bgmedia::motion
