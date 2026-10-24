#include "framework.hpp"

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>

namespace {

using bgmedia::CharacterPose;
using bgmedia::MoveKind;
using bgmedia::animation::HumanoidRetargeter;
using bgmedia::animation::RetargetOptions;
using bgmedia::vrm::HumanoidBone;
using bgmedia::vrm::VrmLoader;
using bgmedia::vrm::VrmModel;

std::filesystem::path fixturePath() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

std::filesystem::path realModelPath() {
    if (const char* env = std::getenv("BGMEDIA_VRM_TEST_MODEL")) {
        return std::filesystem::path(env);
    }
#ifdef BGMEDIA_SOURCE_DIR
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "models" / "Model1.vrm";
#else
    return {};
#endif
}

bool matrixFinite(const bgmedia::math::Mat4& matrix) {
    return matrix.finite();
}

bgmedia::math::Vec3 bonePosition(const bgmedia::animation::RetargetedPose& pose,
                                 const VrmModel& model, HumanoidBone bone) {
    return pose.bonePosition(bone, model);
}

CharacterPose bouncePose() {
    CharacterPose pose;
    pose.move = MoveKind::Bounce;
    pose.bob = -0.72f;
    pose.lean = 0.11f;
    pose.armLeft = -0.40f;
    pose.armRight = 0.40f;
    pose.kneeLeft = 0.62f;
    pose.kneeRight = 0.62f;
    pose.headNod = 0.16f;
    pose.energy = 0.9f;
    return pose;
}

CharacterPose bassDipPose() {
    CharacterPose pose;
    pose.move = MoveKind::BassDip;
    pose.bob = -0.82f;
    pose.lean = 0.10f;
    pose.kneeLeft = 0.72f;
    pose.kneeRight = 0.66f;
    pose.hipShift = 0.15f;
    pose.armLeft = -0.42f;
    pose.armRight = 0.42f;
    pose.elbowLeft = 0.30f;
    pose.elbowRight = 0.30f;
    pose.headNod = -0.42f;
    pose.accented = true;
    pose.impact = 1.0f;
    pose.energy = 0.95f;
    return pose;
}

CharacterPose armWavePose() {
    CharacterPose pose;
    pose.move = MoveKind::ArmWave;
    pose.armLeft = -0.82f;
    pose.armRight = 0.02f;
    pose.elbowLeft = 0.62f;
    pose.elbowRight = 0.02f;
    pose.lean = 0.22f;
    pose.headTurn = 0.18f;
    pose.chestTwist = 0.18f;
    pose.energy = 0.8f;
    return pose;
}

struct RigFixture {
    bgmedia::vrm::LoadResult loaded;
    std::unique_ptr<HumanoidRetargeter> rig;
};

std::unique_ptr<RigFixture> makeFixtureRig() {
    auto fixture = std::make_unique<RigFixture>();
    fixture->loaded = VrmLoader::load(fixturePath());
    if (!fixture->loaded.ok()) {
        return nullptr;
    }
    fixture->rig =
        std::make_unique<HumanoidRetargeter>(*fixture->loaded.model);
    if (!fixture->rig->valid()) {
        return nullptr;
    }
    return fixture;
}

} // namespace

TEST_CASE(retarget.neutral_pose_is_not_tpose) {
    auto fixture = makeFixtureRig();
    CHECK(fixture != nullptr);
    if (!fixture) {
        return;
    }
    const VrmModel& model = *fixture->loaded.model;
    RetargetOptions options;
    options.idleMotion = false;
    CharacterPose neutral;
    const auto pose = fixture->rig->apply(neutral, 0.0, options);
    CHECK(pose.stats.valid);
    CHECK(pose.nodeGlobals.size() == model.nodes.size());
    for (const auto& matrix : pose.nodeGlobals) {
        CHECK(matrixFinite(matrix));
    }

    const auto shoulder = bonePosition(pose, model, HumanoidBone::RightUpperArm);
    const auto hand = bonePosition(pose, model, HumanoidBone::RightHand);
    CHECK(hand.y < shoulder.y - 0.20f);
    CHECK(std::abs(hand.x - shoulder.x) > 0.05f);

    const auto leftFoot = bonePosition(pose, model, HumanoidBone::LeftFoot);
    const auto rightFoot = bonePosition(pose, model, HumanoidBone::RightFoot);
    CHECK_NEAR(leftFoot.y, model.boneBindPosition(HumanoidBone::LeftFoot).y, 0.005);
    CHECK_NEAR(rightFoot.y, model.boneBindPosition(HumanoidBone::RightFoot).y, 0.005);
}

TEST_CASE(retarget.bounce_and_bassdip_keep_feet_planted) {
    auto fixture = makeFixtureRig();
    CHECK(fixture != nullptr);
    if (!fixture) {
        return;
    }
    const VrmModel& model = *fixture->loaded.model;
    RetargetOptions options;
    options.idleMotion = false;
    CharacterPose upwardBounce = bouncePose();
    upwardBounce.bob = 0.72f;
    upwardBounce.kneeLeft = 0.0f;
    upwardBounce.kneeRight = 0.0f;
    const CharacterPose poses[] = {bouncePose(), bassDipPose(), upwardBounce};
    for (std::size_t poseIndex = 0; poseIndex < std::size(poses); ++poseIndex) {
        const CharacterPose& input = poses[poseIndex];
        const auto pose = fixture->rig->apply(input, 1.0, options);
        CHECK(pose.stats.valid);
        CHECK(pose.stats.footIK);
        for (const auto& matrix : pose.nodeGlobals) {
            CHECK(matrixFinite(matrix));
        }
        const auto leftFoot = bonePosition(pose, model, HumanoidBone::LeftFoot);
        const auto rightFoot = bonePosition(pose, model, HumanoidBone::RightFoot);
        const auto leftBind = model.boneBindPosition(HumanoidBone::LeftFoot);
        const auto rightBind = model.boneBindPosition(HumanoidBone::RightFoot);
        CHECK_NEAR(leftFoot.x, leftBind.x, 0.002);
        CHECK_NEAR(leftFoot.z, leftBind.z, 0.002);
        CHECK_NEAR(rightFoot.x, rightBind.x, 0.002);
        CHECK_NEAR(rightFoot.z, rightBind.z, 0.002);
        CHECK_NEAR(leftFoot.y, leftBind.y, 0.006);
        CHECK_NEAR(rightFoot.y, rightBind.y, 0.006);
        CHECK(pose.stats.maxFootError < 0.005f);
        // The hips actually move down for the grounded bounce/dip, and the
        // knees absorb it. An upward bob must stop at the bind height instead
        // of lifting the feet.
        const auto hip = bonePosition(pose, model, HumanoidBone::Hips);
        if (poseIndex < 2) {
            CHECK(hip.y < model.boneBindPosition(HumanoidBone::Hips).y - 0.02f);
        } else {
            CHECK(hip.y >= model.boneBindPosition(HumanoidBone::Hips).y - 0.003f);
        }
    }
}

TEST_CASE(retarget.arms_head_and_torso_are_articulated) {
    auto fixture = makeFixtureRig();
    CHECK(fixture != nullptr);
    if (!fixture) {
        return;
    }
    const VrmModel& model = *fixture->loaded.model;
    RetargetOptions options;
    options.idleMotion = false;
    const CharacterPose neutral;
    const auto base = fixture->rig->apply(neutral, 0.0, options);
    const auto wave = fixture->rig->apply(armWavePose(), 0.0, options);
    CHECK(base.stats.valid);
    CHECK(wave.stats.valid);

    const auto baseHand = bonePosition(base, model, HumanoidBone::LeftHand);
    const auto waveHand = bonePosition(wave, model, HumanoidBone::LeftHand);
    CHECK((waveHand - baseHand).length() > 0.15f);
    const auto waveHead = bonePosition(wave, model, HumanoidBone::Head);
    const auto baseHead = bonePosition(base, model, HumanoidBone::Head);
    CHECK((waveHead - baseHead).length() > 0.001f);

    CharacterPose nod;
    nod.headNod = -0.7f;
    nod.headTurn = 0.4f;
    nod.headTilt = 0.25f;
    const auto nodded = fixture->rig->apply(nod, 0.0, options);
    const auto nodHead = bonePosition(nodded, model, HumanoidBone::Head);
    CHECK((nodHead - baseHead).length() > 0.001f);

    CharacterPose sway;
    sway.lean = 0.8f;
    sway.chestTwist = 0.5f;
    sway.hipShift = -0.5f;
    const auto swayed = fixture->rig->apply(sway, 0.0, options);
    const auto swayHip = bonePosition(swayed, model, HumanoidBone::Hips);
    CHECK(std::abs(swayHip.x - model.boneBindPosition(HumanoidBone::Hips).x) > 0.01f);
    CHECK(swayed.stats.valid);
}

TEST_CASE(retarget.is_deterministic_and_frame_rate_independent) {
    auto fixture = makeFixtureRig();
    CHECK(fixture != nullptr);
    if (!fixture) {
        return;
    }
    const VrmModel& model = *fixture->loaded.model;
    RetargetOptions options;
    options.idleMotion = false;
    const CharacterPose pose = armWavePose();

    const auto first = fixture->rig->apply(pose, 2.25, options);
    const auto second = fixture->rig->apply(pose, 2.25, options);
    CHECK(first.nodeGlobals.size() == second.nodeGlobals.size());
    for (std::size_t i = 0; i < first.nodeGlobals.size(); ++i) {
        CHECK(std::memcmp(first.nodeGlobals[i].m.data(),
                          second.nodeGlobals[i].m.data(),
                          sizeof(first.nodeGlobals[i].m)) == 0);
    }

    // Advancing and rewinding media time must reproduce exactly the same pose.
    (void)fixture->rig->apply(bouncePose(), 10.0, options);
    (void)fixture->rig->apply(bassDipPose(), 3.0, options);
    const auto afterSeek = fixture->rig->apply(pose, 2.25, options);
    for (std::size_t i = 0; i < first.nodeGlobals.size(); ++i) {
        CHECK(std::memcmp(first.nodeGlobals[i].m.data(),
                          afterSeek.nodeGlobals[i].m.data(),
                          sizeof(first.nodeGlobals[i].m)) == 0);
    }

    // An animated character can be sampled at any time without exploding.
    for (double time = 0.0; time < 30.0; time += 0.37) {
        const auto sample = fixture->rig->apply(bassDipPose(), time, options);
        CHECK(sample.stats.valid);
        for (const auto& matrix : sample.nodeGlobals) {
            CHECK(matrixFinite(matrix));
        }
    }
    (void)model;
}

TEST_CASE(retarget.model1_if_available) {
    const std::filesystem::path path = realModelPath();
    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }
    bgmedia::vrm::LoadResult loaded = VrmLoader::load(path);
    CHECK(loaded.ok());
    if (!loaded.ok()) {
        std::cout << "\n            " << loaded.error;
        return;
    }
    const VrmModel& model = *loaded.model;
    HumanoidRetargeter rig(model);
    CHECK(rig.valid());
    if (!rig.valid()) {
        std::cout << "\n            " << rig.error();
        return;
    }
    RetargetOptions options;
    options.idleMotion = false;
    CharacterPose poses[] = {bouncePose(), bassDipPose(), armWavePose()};
    for (const CharacterPose& input : poses) {
        const auto pose = rig.apply(input, 4.0, options);
        CHECK(pose.stats.valid);
        CHECK(pose.stats.footIK);
        CHECK(pose.nodeGlobals.size() == model.nodes.size());
        for (const auto& matrix : pose.nodeGlobals) {
            CHECK(matrixFinite(matrix));
        }
        CHECK(pose.stats.maxFootError < 0.01f);
        const auto leftFoot = bonePosition(pose, model, HumanoidBone::LeftFoot);
        const auto rightFoot = bonePosition(pose, model, HumanoidBone::RightFoot);
        CHECK_NEAR(leftFoot.x, model.boneBindPosition(HumanoidBone::LeftFoot).x, 0.004);
        CHECK_NEAR(leftFoot.z, model.boneBindPosition(HumanoidBone::LeftFoot).z, 0.004);
        CHECK_NEAR(rightFoot.x, model.boneBindPosition(HumanoidBone::RightFoot).x, 0.004);
        CHECK_NEAR(rightFoot.z, model.boneBindPosition(HumanoidBone::RightFoot).z, 0.004);
    }
    const auto neutral = rig.apply(CharacterPose{}, 0.0, options);
    const auto hand = bonePosition(neutral, model, HumanoidBone::RightHand);
    const auto shoulder = bonePosition(neutral, model, HumanoidBone::RightUpperArm);
    CHECK(hand.y < shoulder.y - 0.25f);
    std::cout << "\n            Model1 retarget: height=" << neutral.stats.height
              << " footError=" << neutral.stats.maxFootError
              << " bones=" << neutral.stats.resolvedBones;
}
