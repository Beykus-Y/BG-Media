#include "framework.hpp"

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/math/Math.hpp"
#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/motion3/TaskSpaceIntent.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <cmath>
#include <filesystem>
#include <vector>

namespace {

using namespace bgmedia;
using animation::HumanoidRetargeter;
using animation::RetargetOptions;
using math::Quat;
using math::Vec3;
using motion::ArmIntent;
using motion::ArmIntentSource;
using motion::ClipPose;
using motion::TaskSpaceIntent;
using vrm::HumanoidBone;
using vrm::VrmLoader;

std::filesystem::path dancesDir() {
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances";
}
std::filesystem::path minimalVrm() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

bool isFinite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float distance(Vec3 a, Vec3 b) { return (a - b).length(); }

Quat rotationZ(float radians) { return math::fromAxisAngle({0.0f, 0.0f, 1.0f}, radians); }

Quat& bone(ClipPose& pose, HumanoidBone which) {
    return pose.bone[static_cast<std::size_t>(which)];
}

} // namespace

TEST_CASE(intent.reference_bind_pose) {
    const TaskSpaceIntent intent = motion::extractTaskSpaceIntent(motion::neutralClipPose());
    const ArmIntent& left = intent.arm[0];
    CHECK(left.valid);
    CHECK(left.source == ArmIntentSource::Extracted);
    // Chest at 0.66, shoulder joint at 0.74 (0.08 above the chest), wrist 0.42 out.
    CHECK_NEAR(left.wrist.x, 0.42f, 1e-5);
    CHECK_NEAR(left.wrist.y, 0.08f, 1e-5);
    CHECK_NEAR(left.wrist.z, 0.0f, 1e-5);
    // A T-pose arm is straight: full reach, undefined bend, fallback pole.
    CHECK_NEAR(left.reach, 1.0f, 1e-4);
    CHECK_NEAR(left.confidence, 0.5f, 1e-5);
    CHECK_NEAR(left.elbowPole.y, -1.0f, 1e-5);
    CHECK_NEAR(intent.arm[1].wrist.x, -0.42f, 1e-5);
    CHECK_NEAR(intent.arm[1].wrist.y, 0.08f, 1e-5);
}

TEST_CASE(intent.arm_raise_moves_wrist_up) {
    ClipPose pose = motion::neutralClipPose();
    bone(pose, HumanoidBone::LeftUpperArm) = rotationZ(math::kPi * 0.5f);
    const ArmIntent left = motion::extractTaskSpaceIntent(pose).arm[0];
    // The whole arm swings 90 degrees about the shoulder: the wrist goes straight up.
    CHECK_NEAR(left.wrist.x, 0.10f, 1e-4);
    CHECK_NEAR(left.wrist.y, 0.40f, 1e-4);
    CHECK_NEAR(left.reach, 1.0f, 1e-4);
    // The right arm is untouched.
    CHECK_NEAR(motion::extractTaskSpaceIntent(pose).arm[1].wrist.x, -0.42f, 1e-5);
}

TEST_CASE(intent.elbow_bend_defines_pole) {
    ClipPose pose = motion::neutralClipPose();
    bone(pose, HumanoidBone::LeftLowerArm) = rotationZ(-math::kPi * 0.5f);
    const ArmIntent left = motion::extractTaskSpaceIntent(pose).arm[0];
    // Forearm points down: the wrist is below the shoulder, the arm is bent.
    CHECK(left.wrist.y < 0.0f);
    CHECK_NEAR(left.confidence, 1.0f, 1e-5);
    CHECK_NEAR(left.elbowPole.length(), 1.0f, 1e-4);
    // The pole is perpendicular to the shoulder-wrist axis.
    const Vec3 axis = left.wrist - Vec3{0.10f, 0.08f, 0.0f};
    CHECK_NEAR(math::dot(left.elbowPole, math::normalize(axis)), 0.0f, 1e-4);
    // The elbow lifts up and out (away from the body), so the pole points up.
    CHECK(left.elbowPole.y > 0.5f);
}

TEST_CASE(intent.detailed_wrist_is_part_of_orientation) {
    ClipPose pose = motion::neutralClipPose();
    pose.handPose[0].wrist = {1.0f, 0.0f, 0.0f};
    // Without the detailed hand layer the wrist adds nothing.
    CHECK(motion::extractTaskSpaceIntent(pose).arm[0].wristOrientation.w > 0.9999f);

    pose.detailedHands = true;
    const Quat expected = math::fromAxisAngle({-1.0f, 0.0f, 0.0f}, 0.26f);
    const Quat actual = motion::extractTaskSpaceIntent(pose).arm[0].wristOrientation;
    CHECK_NEAR(actual.x, expected.x, 1e-5);
    CHECK_NEAR(actual.y, expected.y, 1e-5);
    CHECK_NEAR(actual.z, expected.z, 1e-5);
    CHECK_NEAR(actual.w, expected.w, 1e-5);
    // The right hand has its own (here neutral) wrist value.
    CHECK(motion::extractTaskSpaceIntent(pose).arm[1].wristOrientation.w > 0.9999f);
}

TEST_CASE(intent.chest_relative_ignores_torso_and_stage) {
    const ArmIntent reference =
        motion::extractTaskSpaceIntent(motion::neutralClipPose()).arm[0];

    // Bending the spine carries the chest and the arm together: the chest-relative
    // wrist does not move, so a gesture in front of the sternum stays there.
    ClipPose bent = motion::neutralClipPose();
    bone(bent, HumanoidBone::Spine) = rotationZ(0.5f);
    bone(bent, HumanoidBone::Chest) = rotationZ(0.2f);
    const ArmIntent withSpine = motion::extractTaskSpaceIntent(bent).arm[0];
    CHECK(distance(withSpine.wrist, reference.wrist) < 1e-5f);
    CHECK(distance(withSpine.elbowPole, reference.elbowPole) < 1e-5f);

    // Hip translation and the stage root (translation and yaw) are not part of the intent.
    ClipPose staged = motion::neutralClipPose();
    staged.hips = {0.02f, -0.01f, 0.03f};
    staged.rootX = 0.3f;
    staged.rootZ = -0.2f;
    staged.rootYaw = 0.25f;
    const ArmIntent placed = motion::extractTaskSpaceIntent(staged).arm[0];
    CHECK(distance(placed.wrist, reference.wrist) < 1e-5f);
    CHECK(distance(placed.elbowPole, reference.elbowPole) < 1e-5f);
}

TEST_CASE(intent.clip_extraction_is_deterministic_and_order_free) {
    const auto loaded = motion::loadClipFile(dancesDir() / "groove.json");
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }
    const TaskSpaceIntent first = motion::extractClipTaskSpaceIntent(loaded.clip, 5.5);
    // Sampling other beats first must not change the result: the intent is a
    // pure function of the beat, not of call history or frame rate.
    for (double beat = 0.0; beat < 8.0; beat += 0.37) {
        (void)motion::extractClipTaskSpaceIntent(loaded.clip, beat);
    }
    const TaskSpaceIntent second = motion::extractClipTaskSpaceIntent(loaded.clip, 5.5);
    for (std::size_t side = 0; side < 2; ++side) {
        CHECK(distance(first.arm[side].wrist, second.arm[side].wrist) == 0.0f);
        CHECK(distance(first.arm[side].wristVelocity, second.arm[side].wristVelocity) == 0.0f);
        CHECK(first.arm[side].wristOrientation.w == second.arm[side].wristOrientation.w);
    }
}

TEST_CASE(intent.all_authored_clips_are_extractable) {
    const std::vector<std::string> names = {"energetic.json", "groove.json", "hiphop.json",
                                            "idle.json", "sidestep.json"};
    for (const std::string& name : names) {
        const auto loaded = motion::loadClipFile(dancesDir() / name);
        CHECK(loaded.ok);
        if (!loaded.ok) {
            continue;
        }
        for (double beat = 0.0; beat < loaded.clip.beats; beat += 0.25) {
            const TaskSpaceIntent intent = motion::extractClipTaskSpaceIntent(loaded.clip, beat);
            for (const ArmIntent& arm : intent.arm) {
                CHECK(arm.valid);
                CHECK(isFinite(arm.wrist));
                CHECK(isFinite(arm.elbowPole));
                CHECK(isFinite(arm.wristVelocity));
                CHECK(std::isfinite(arm.reach));
                // Two rigid bones cannot stretch past their full length.
                CHECK(arm.reach <= 1.0f + 1e-4f);
                CHECK(arm.confidence >= 0.5f && arm.confidence <= 1.0f);
            }
        }
    }
}

TEST_CASE(intent.explicit_intent_overrides_extracted) {
    const ArmIntent extracted =
        motion::extractTaskSpaceIntent(motion::neutralClipPose()).arm[0];

    ArmIntent explicitTarget;
    explicitTarget.valid = true;
    explicitTarget.source = ArmIntentSource::Explicit;
    explicitTarget.wrist = {0.2f, 0.5f, 0.1f};

    const ArmIntent chosen = motion::resolveArmIntent(explicitTarget, extracted);
    CHECK(chosen.source == ArmIntentSource::Explicit);
    CHECK(distance(chosen.wrist, explicitTarget.wrist) == 0.0f);

    // No explicit intent: the extracted fallback is used.
    const ArmIntent fallback = motion::resolveArmIntent(ArmIntent{}, extracted);
    CHECK(fallback.source == ArmIntentSource::Extracted);
    CHECK(distance(fallback.wrist, extracted.wrist) == 0.0f);

    // An extracted intent never counts as explicit.
    const ArmIntent stillExtracted = motion::resolveArmIntent(extracted, extracted);
    CHECK(stillExtracted.source == ArmIntentSource::Extracted);
}

TEST_CASE(intent.matches_model_fk_direction) {
    // The retargeter evaluates the same clip pose on a real VRM skeleton. The two
    // skeletons differ in proportions, so compare directions: the chest-to-wrist
    // vector must point the same way in both, after mapping the clip frame to the
    // model's axes (clip +X = character left, +Y = up, +Z = forward).
    const auto loaded = VrmLoader::load(minimalVrm());
    CHECK(loaded.ok());
    if (!loaded.ok()) {
        return;
    }
    const vrm::VrmModel& model = *loaded.model;
    const HumanoidRetargeter rig(model);
    CHECK(rig.valid());
    if (!rig.valid()) {
        return;
    }

    RetargetOptions options;
    options.idleMotion = false;
    options.armCollision = false;  // compare the raw FK, not the reactive correction
    options.footIK = false;

    const std::vector<Quat> rotations = {
        rotationZ(math::kPi * 0.25f),
        rotationZ(math::kPi * 0.5f),
        math::fromAxisAngle({1.0f, 0.0f, 0.0f}, 0.6f),
    };
    const Vec3 left = -model.right;
    for (const Quat& upper : rotations) {
        ClipPose pose = motion::neutralClipPose();
        bone(pose, HumanoidBone::LeftUpperArm) = upper;
        bone(pose, HumanoidBone::LeftLowerArm) = rotationZ(-0.4f);

        const ArmIntent intent = motion::extractTaskSpaceIntent(pose).arm[0];
        const animation::RetargetedPose out = rig.applyClip(pose, 0.0, options);
        CHECK(out.stats.valid);

        const Vec3 modelChest = out.bonePosition(HumanoidBone::Chest, model);
        const Vec3 modelWrist = out.bonePosition(HumanoidBone::LeftHand, model) - modelChest;
        // Clip direction -> model axes (the same mapping applyClip uses).
        const Vec3 clipWrist = intent.wrist;
        const Vec3 expected = left * clipWrist.x + model.up * clipWrist.y +
                              model.forward * clipWrist.z;
        const float cosine = math::dot(math::normalize(modelWrist), math::normalize(expected));
        // Within about 0.3 rad between the two skeletons.
        CHECK(cosine > 0.95f);
    }
}
