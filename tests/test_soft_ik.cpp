#include "framework.hpp"

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/animation/TwoBoneSolver.hpp"
#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

using namespace bgmedia;
using animation::HumanoidRetargeter;
using animation::RetargetOptions;
using animation::RetargetedPose;
using animation::TwoBoneChain;
using animation::TwoBoneLimits;
using animation::TwoBoneSolution;
using math::Mat4;
using math::Quat;
using math::Vec3;

// Synthetic two-bone chain. The model axes are the default (y up, x right, z forward).
// Rest bend: the knee's natural side is +y here, deliberately not the pole.
TwoBoneChain chainOf(float upper, float lower, Vec3 restBend = {0.0f, 1.0f, 0.0f}) {
    TwoBoneChain chain;
    chain.upper = upper;
    chain.lower = lower;
    chain.restBend = restBend;
    return chain;
}

float distance(Vec3 a, Vec3 b) { return (a - b).length(); }

bool finiteVec(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Checks that a solution is a valid triangle: the bones keep their lengths.
void checkBoneLengths(const TwoBoneSolution& s, Vec3 root, const TwoBoneChain& chain) {
    CHECK(finiteVec(s.knee));
    CHECK(finiteVec(s.end));
    CHECK_NEAR(distance(s.knee, root), chain.upper, 1e-4);
    CHECK_NEAR(distance(s.end, s.knee), chain.lower, 1e-4);
}

// Angle between the two bones (0 = straight), from the solved joints.
float flexionOf(const TwoBoneSolution& s, Vec3 root) {
    const Vec3 thigh = s.knee - root;
    const Vec3 calf = s.end - s.knee;
    const float c = math::dot(thigh, calf) / (thigh.length() * calf.length());
    return std::acos(std::clamp(c, -1.0f, 1.0f));
}

bool sameBits(const Mat4& a, const Mat4& b) {
    return std::memcmp(a.m.data(), b.m.data(), sizeof(a.m)) == 0;
}

bool sameBits(const RetargetedPose& a, const RetargetedPose& b) {
    if (a.nodeGlobals.size() != b.nodeGlobals.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.nodeGlobals.size(); ++i) {
        if (!sameBits(a.nodeGlobals[i], b.nodeGlobals[i])) {
            return false;
        }
    }
    return true;
}

std::filesystem::path fixturePath() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

struct Rig {
    vrm::LoadResult loaded;
    std::unique_ptr<HumanoidRetargeter> retargeter;
};

// Rotates the whole rest pose (every bind transform, the root locals and the model axes)
// about the model's up axis. The output must rotate with it.
void yawRestPose(vrm::VrmModel& model, float radians) {
    const Quat q = math::fromAxisAngle(model.up, radians);
    const Mat4 r = Mat4::fromQuat(q);
    for (Mat4& g : model.bindGlobals) {
        g = r * g;
    }
    for (Quat& rotation : model.bindGlobalRotations) {
        rotation = (q * rotation).normalized();
    }
    for (auto& node : model.nodes) {
        if (node.parent < 0) {
            node.local = r * node.local;
            node.rotation = (q * node.rotation).normalized();
        }
    }
    model.right = math::rotate(q, model.right);
    model.forward = math::rotate(q, model.forward);
}

std::unique_ptr<Rig> makeRig(float yaw = 0.0f) {
    auto rig = std::make_unique<Rig>();
    rig->loaded = vrm::VrmLoader::load(fixturePath());
    if (!rig->loaded.ok()) {
        return nullptr;
    }
    if (yaw != 0.0f) {
        yawRestPose(*rig->loaded.model, yaw);
    }
    rig->retargeter = std::make_unique<HumanoidRetargeter>(*rig->loaded.model);
    if (!rig->retargeter->valid()) {
        return nullptr;
    }
    return rig;
}

CharacterPose bassDip() {
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
    pose.energy = 0.95f;
    return pose;
}

// A clip pose with both feet displaced out of the bind reach, so the legs are driven
// near full extension. contactLeft / contactRight set the planted weight of each foot.
motion::ClipPose stretchedClipPose(float contactLeft, float contactRight) {
    motion::ClipPose pose = motion::neutralClipPose();
    pose.foot[0].position = {0.0f, -0.10f, 0.0f};
    pose.foot[1].position = {0.0f, -0.10f, 0.0f};
    pose.foot[0].contact = contactLeft;
    pose.foot[1].contact = contactRight;
    return pose;
}

} // namespace

// --- pure soft-reach math ---------------------------------------------------

TEST_CASE(soft_ik.soft_reach_is_identity_between_the_knees) {
    const float lo = 0.1f;
    const float hi = 0.9f;
    const float soft = 0.05f;
    for (float d = lo + soft; d <= hi - soft; d += 0.01f) {
        CHECK_NEAR(animation::softReachDistance(d, lo, hi, soft), d, 1e-6);
    }
}

TEST_CASE(soft_ik.soft_reach_is_monotone_and_never_reaches_the_limits) {
    const float lo = 0.1f;
    const float hi = 0.9f;
    const float soft = 0.05f;
    float previous = -1.0f;
    bool monotone = true;
    bool bounded = true;
    for (float d = lo - 1.0f; d <= hi + 2.0f; d += 0.001f) {
        const float value = animation::softReachDistance(d, lo, hi, soft);
        monotone = monotone && value >= previous;
        bounded = bounded && value >= lo && value <= hi;
        previous = value;
    }
    CHECK(monotone);
    CHECK(bounded);
    // Far outside the limit the solved distance approaches it, and stays inside.
    CHECK(animation::softReachDistance(50.0f, lo, hi, soft) <= hi);
    CHECK_NEAR(animation::softReachDistance(50.0f, lo, hi, soft), hi, 1e-5);
    CHECK_NEAR(animation::softReachDistance(-50.0f, lo, hi, soft), lo, 1e-5);
}

TEST_CASE(soft_ik.soft_reach_is_c1_at_both_knees) {
    // The first derivative is 1 on both sides of each knee: the numerical slopes on
    // either side agree with 1 to within the step's second-order error.
    const float lo = 0.1f;
    const float hi = 0.9f;
    const float soft = 0.05f;
    const float h = 0.01f * soft;
    const float knees[2] = {hi - soft, lo + soft};
    for (const float knee : knees) {
        const auto f = [&](float d) { return animation::softReachDistance(d, lo, hi, soft); };
        const float leftSlope = (f(knee) - f(knee - h)) / h;
        const float rightSlope = (f(knee + h) - f(knee)) / h;
        CHECK_NEAR(leftSlope, 1.0f, 0.01);
        CHECK_NEAR(rightSlope, 1.0f, 0.01);
    }
}

TEST_CASE(soft_ik.zero_soft_band_is_the_hard_clamp) {
    CHECK_NEAR(animation::softReachDistance(1.2f, 0.1f, 0.9f, 0.0f), 0.9f, 1e-7);
    CHECK_NEAR(animation::softReachDistance(0.05f, 0.1f, 0.9f, 0.0f), 0.1f, 1e-7);
    CHECK_NEAR(animation::softReachDistance(0.5f, 0.1f, 0.9f, 0.0f), 0.5f, 1e-7);
}

TEST_CASE(soft_ik.distance_at_flexion_spans_fold_to_straight) {
    CHECK_NEAR(animation::distanceAtFlexion(0.3f, 0.3f, 0.0f), 0.6f, 1e-6);
    CHECK_NEAR(animation::distanceAtFlexion(0.3f, 0.3f, math::kPi), 0.0f, 1e-6);
    CHECK_NEAR(animation::distanceAtFlexion(0.3f, 0.3f, math::kPi * 0.5f), std::sqrt(0.18f), 1e-6);
}

// --- two-bone solves --------------------------------------------------------

TEST_CASE(soft_ik.target_inside_just_outside_and_far_outside_reach) {
    const TwoBoneChain chain = chainOf(0.3f, 0.3f);
    TwoBoneLimits limits;
    limits.softReach = 0.036f;  // 6 % of the 0.6 m chain
    limits.stableBend = true;
    const Vec3 root{};
    const Vec3 pole{0.0f, 0.0f, 1.0f};
    const float hi = 0.3f + 0.3f - 0.002f;
    const float knee = hi - limits.softReach;

    // Just inside the knee: the target is reached exactly.
    const TwoBoneSolution inside = animation::solveTwoBone(
        root, Vec3{0.0f, -(knee - 0.01f), 0.0f}, pole, chain, limits);
    CHECK_NEAR(inside.solvedDistance, knee - 0.01f, 1e-6);
    CHECK_NEAR(inside.softShift, 0.0f, 1e-6);
    checkBoneLengths(inside, root, chain);

    // Just outside: pulled in, but still beyond the knee.
    const TwoBoneSolution outside = animation::solveTwoBone(
        root, Vec3{0.0f, -(hi + 0.01f), 0.0f}, pole, chain, limits);
    CHECK(outside.beyondReach);
    CHECK(outside.solvedDistance < hi);
    CHECK(outside.solvedDistance > knee);
    CHECK(outside.softShift < 0.0f);
    checkBoneLengths(outside, root, chain);

    // Far outside: the solved distance is the limit itself, to float precision.
    const TwoBoneSolution far = animation::solveTwoBone(
        root, Vec3{0.0f, -5.0f, 0.0f}, pole, chain, limits);
    CHECK(far.solvedDistance <= hi);
    CHECK_NEAR(far.solvedDistance, hi, 1e-5);
    checkBoneLengths(far, root, chain);

    // The hard clamp, for comparison, is exactly the limit.
    const TwoBoneSolution hard = animation::solveTwoBone(
        root, Vec3{0.0f, -(hi + 0.01f), 0.0f}, pole, chain);
    CHECK_NEAR(hard.solvedDistance, hi, 1e-6);
    CHECK_NEAR(hard.softShift, 0.0f, 1e-6);
}

TEST_CASE(soft_ik.deeply_folded_target_stays_on_a_valid_triangle) {
    const TwoBoneChain chain = chainOf(0.3f, 0.3f);
    TwoBoneLimits limits;
    limits.softReach = 0.036f;
    limits.stableBend = true;
    const Vec3 root{};
    const Vec3 pole{0.0f, 0.0f, 1.0f};
    const float lo = 0.002f;

    for (const float d : {0.001f, 0.0002f}) {
        const TwoBoneSolution folded = animation::solveTwoBone(
            root, Vec3{0.0f, 0.0f, d}, pole, chain, limits);
        CHECK(folded.solvedDistance >= lo);
        CHECK(folded.solvedDistance <= lo + limits.softReach);
        checkBoneLengths(folded, root, chain);
    }

    // A target exactly on the root has no direction: the chain hangs down.
    const TwoBoneSolution onRoot = animation::solveTwoBone(root, root, pole, chain, limits);
    CHECK(finiteVec(onRoot.knee));
    CHECK_NEAR(onRoot.direction.y, -1.0f, 1e-6);
    checkBoneLengths(onRoot, root, chain);
}

TEST_CASE(soft_ik.flexion_window_bounds_a_folded_chain) {
    const TwoBoneChain chain = chainOf(0.3f, 0.3f);
    TwoBoneLimits limits;
    limits.flexWindow = true;
    limits.flexMin = 0.0f;
    limits.flexMax = 1.5f;
    const Vec3 root{};
    const Vec3 pole{0.0f, 0.0f, 1.0f};
    const Vec3 target{0.0f, 0.0f, 0.05f};

    const TwoBoneSolution held = animation::solveTwoBone(root, target, pole, chain, limits);
    CHECK(held.flexBound);
    CHECK_NEAR(flexionOf(held, root), 1.5f, 1e-3);
    checkBoneLengths(held, root, chain);

    // Measured but not enforced (a planted leg): the solve keeps the hard reach.
    limits.flexEnforced = false;
    const TwoBoneSolution reported = animation::solveTwoBone(root, target, pole, chain, limits);
    CHECK(reported.flexBound);
    CHECK_NEAR(reported.solvedDistance, 0.05f, 1e-6);
}

TEST_CASE(soft_ik.near_straight_knee_keeps_its_bend_side_across_a_sweep) {
    // The reach is almost the full chain and the pole swings through the reach line
    // (the pole is parallel to the target direction at theta = pi / 2). The bend side must
    // not flip there: it is handed over to the rest bend.
    const TwoBoneChain chain = chainOf(0.3f, 0.3f);
    const Vec3 root{};
    const Vec3 pole{0.0f, 0.0f, 1.0f};
    const float d = 0.5980f - 0.001f;

    const auto maxStep = [&](bool stable) {
        TwoBoneLimits limits;
        limits.stableBend = stable;
        float worst = 0.0f;
        bool havePrevious = false;
        Vec3 previous{};
        for (int i = 0; i <= 700; ++i) {
            const float theta = 0.2f + 2.7f * static_cast<float>(i) / 700.0f;
            const Vec3 dir{0.0f, -std::cos(theta), std::sin(theta)};
            const TwoBoneSolution s = animation::solveTwoBone(root, dir * d, pole, chain, limits);
            const float along = math::dot(s.knee - root, s.direction);
            Vec3 bend = (s.knee - root) - s.direction * along;
            if (bend.length() < 1.0e-3f) {
                continue;  // straight enough that the side does not matter
            }
            bend = math::normalize(bend);
            if (havePrevious) {
                worst = std::max(worst, std::acos(std::clamp(math::dot(bend, previous), -1.0f, 1.0f)));
            }
            previous = bend;
            havePrevious = true;
        }
        return worst;
    };

    const float stableStep = maxStep(true);
    const float legacyStep = maxStep(false);
    CHECK(stableStep < 0.35f);
    // The BG Motion 3.2 pole-only bend flips across the swing.
    CHECK(legacyStep > 1.0f);
}

TEST_CASE(soft_ik.rotated_rest_pose_gives_the_same_solve) {
    // Rotating the chain's axes, the rest bend, the pole and the target together must
    // rotate the solved joints together: nothing is hardcoded to an axis.
    const float angle = 0.7f;
    const Quat q = math::fromAxisAngle({0.0f, 1.0f, 0.0f}, angle);
    TwoBoneLimits limits;
    limits.softReach = 0.036f;
    limits.stableBend = true;
    limits.flexWindow = true;
    limits.flexMax = 2.2f;
    const Vec3 targets[3] = {{0.20f, -0.45f, 0.10f}, {0.0f, -0.62f, 0.05f}, {-0.30f, -0.05f, 0.40f}};
    for (const Vec3& target : targets) {
        TwoBoneChain chain = chainOf(0.3f, 0.3f, {0.2f, 1.0f, 0.1f});
        const Vec3 pole{0.0f, 0.1f, 1.0f};
        const TwoBoneSolution plain = animation::solveTwoBone({}, target, pole, chain, limits);

        TwoBoneChain turned = chain;
        turned.restBend = math::rotate(q, chain.restBend);
        turned.right = math::rotate(q, chain.right);
        turned.forward = math::rotate(q, chain.forward);
        turned.up = math::rotate(q, chain.up);
        const TwoBoneSolution rotated = animation::solveTwoBone(
            {}, math::rotate(q, target), math::rotate(q, pole), turned, limits);
        CHECK(distance(rotated.knee, math::rotate(q, plain.knee)) < 1.0e-5f);
        CHECK(distance(rotated.end, math::rotate(q, plain.end)) < 1.0e-5f);
        CHECK(plain.bendBlended == rotated.bendBlended);
    }
}

// --- retargeter -------------------------------------------------------------

TEST_CASE(soft_ik.defaults_off_ignore_the_new_options) {
    // With soft IK off, the BG Motion 3.3 fields have no effect at all: the output is the
    // BG Motion 3.2 output bit for bit, whatever the other fields say.
    auto rig = makeRig();
    CHECK(rig != nullptr);
    if (!rig) {
        return;
    }
    RetargetOptions defaults;
    RetargetOptions changedButOff;
    changedButOff.armSoftReach = 0.5f;
    changedButOff.legSoftReach = 0.5f;
    changedButOff.elbowFlexMax = 0.3f;
    changedButOff.kneeFlexMax = 0.3f;
    changedButOff.kneeFlexMin = 0.2f;

    const motion::ClipPose clip = stretchedClipPose(0.0f, 0.0f);
    CHECK(sameBits(rig->retargeter->apply(bassDip(), 1.7, defaults),
                   rig->retargeter->apply(bassDip(), 1.7, changedButOff)));
    CHECK(sameBits(rig->retargeter->applyClip(clip, 0.5, defaults),
                   rig->retargeter->applyClip(clip, 0.5, changedButOff)));
    CHECK(!rig->retargeter->applyClip(clip, 0.5, defaults).stats.softIK);
}

TEST_CASE(soft_ik.free_foot_is_softened_and_planted_foot_is_not) {
    auto rig = makeRig();
    CHECK(rig != nullptr);
    if (!rig) {
        return;
    }
    // Arm collision is off so that the counters below are the leg solves alone.
    RetargetOptions off;
    off.armCollision = false;
    RetargetOptions on;
    on.softIK = true;
    on.armCollision = false;
    on.legSoftReach = 0.06f;
    const auto& retargeter = *rig->retargeter;

    // Planted (contact 1): the soft band never touches the stance target.
    const motion::ClipPose planted = stretchedClipPose(1.0f, 1.0f);
    const RetargetedPose plantedOff = retargeter.applyClip(planted, 0.0, off);
    const RetargetedPose plantedOn = retargeter.applyClip(planted, 0.0, on);
    CHECK(plantedOn.stats.softIK);
    CHECK(plantedOn.stats.softReachSolves == 0);
    CHECK(plantedOn.stats.softReachCompression == 0.0f);
    const auto ankle = [&](const RetargetedPose& pose, vrm::HumanoidBone bone) {
        return pose.bonePosition(bone, *rig->loaded.model);
    };
    CHECK(distance(ankle(plantedOff, vrm::HumanoidBone::LeftFoot),
                   ankle(plantedOn, vrm::HumanoidBone::LeftFoot)) < 1.0e-5f);
    CHECK(distance(ankle(plantedOff, vrm::HumanoidBone::RightFoot),
                   ankle(plantedOn, vrm::HumanoidBone::RightFoot)) < 1.0e-5f);

    // Free (contact 0): the same targets are softened, so the solve differs.
    const motion::ClipPose loose = stretchedClipPose(0.0f, 0.0f);
    const RetargetedPose freeOff = retargeter.applyClip(loose, 0.0, off);
    const RetargetedPose freeOn = retargeter.applyClip(loose, 0.0, on);
    CHECK(freeOn.stats.softReachSolves > 0);
    CHECK(freeOn.stats.softReachCompression > 0.0f);
    CHECK(distance(ankle(freeOff, vrm::HumanoidBone::LeftFoot),
                   ankle(freeOn, vrm::HumanoidBone::LeftFoot)) > 1.0e-4f);
    CHECK(freeOn.stats.valid);
}

TEST_CASE(soft_ik.deterministic_and_independent_of_call_order) {
    auto rig = makeRig();
    CHECK(rig != nullptr);
    if (!rig) {
        return;
    }
    RetargetOptions on;
    on.softIK = true;
    const motion::ClipPose a = stretchedClipPose(0.0f, 1.0f);
    motion::ClipPose b = motion::neutralClipPose();
    b.foot[0].position = {0.05f, 0.0f, 0.12f};
    b.foot[0].contact = 0.0f;

    const RetargetedPose first = rig->retargeter->applyClip(a, 0.0, on);
    const RetargetedPose second = rig->retargeter->applyClip(a, 0.0, on);
    CHECK(sameBits(first, second));

    // Other poses in between, at other times, must not leave any trace.
    (void)rig->retargeter->applyClip(b, 7.3, on);
    (void)rig->retargeter->apply(bassDip(), 3.1, on);
    const RetargetedPose afterOthers = rig->retargeter->applyClip(a, 0.0, on);
    CHECK(sameBits(first, afterOthers));

    // The clip path does not read the media time at all.
    CHECK(sameBits(first, rig->retargeter->applyClip(a, 123.4, on)));

    const RetargetedPose legacy = rig->retargeter->apply(bassDip(), 2.0, on);
    (void)rig->retargeter->applyClip(b, 1.0, on);
    CHECK(sameBits(legacy, rig->retargeter->apply(bassDip(), 2.0, on)));
}

TEST_CASE(soft_ik.rotated_rig_gives_the_same_output) {
    // The same rig, rest pose yawed by an arbitrary angle: every node must come out rotated
    // by that yaw. Arm proxies are calibrated on the unrotated mesh, so they are off here.
    auto plain = makeRig();
    auto turned = makeRig(0.9f);
    CHECK(plain != nullptr);
    CHECK(turned != nullptr);
    if (!plain || !turned) {
        return;
    }
    RetargetOptions on;
    on.softIK = true;
    on.armCollision = false;
    on.legSoftReach = 0.06f;  // exercise the leg band, not only the bend
    const motion::ClipPose clip = stretchedClipPose(0.0f, 0.3f);
    const RetargetedPose a = plain->retargeter->applyClip(clip, 0.0, on);
    const RetargetedPose b = turned->retargeter->applyClip(clip, 0.0, on);
    const Quat q = math::fromAxisAngle(plain->loaded.model->up, 0.9f);
    CHECK(a.nodeGlobals.size() == b.nodeGlobals.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.nodeGlobals.size() && i < b.nodeGlobals.size(); ++i) {
        const Vec3 expected = math::rotate(q, a.nodeGlobals[i].translationPart());
        worst = std::max(worst, distance(expected, b.nodeGlobals[i].translationPart()));
    }
    CHECK(worst < 1.0e-4f);
    CHECK(b.stats.softReachSolves == a.stats.softReachSolves);
}
