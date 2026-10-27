// BG Motion 3.2 quality pass: spatial correctness (C01, C06, C07, C11) and the
// collision-aware arm refinement. Uses synthetic rigs with re-oriented bone axes
// as well as the optional private local model.
#include "framework.hpp"

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using bgmedia::animation::HumanoidRetargeter;
using bgmedia::animation::RetargetOptions;
using bgmedia::vrm::HumanoidBone;
using bgmedia::vrm::VrmLoader;
using bgmedia::vrm::VrmModel;
namespace vrm = bgmedia::vrm;
namespace animation = bgmedia::animation;
namespace motion = bgmedia::motion;

std::filesystem::path fixturePath() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

std::filesystem::path realModelPath() {
    if (const char* env = std::getenv("BGMEDIA_VRM_TEST_MODEL")) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "models" / "Model1.vrm";
}

}  // namespace

// BG Motion 3.2 quality pass: spatial correctness (C01, C06, C07, C11).
// Included at the end of test_retarget.cpp (shares its helpers and fixtures).

namespace {

using bgmedia::math::Mat4;
using bgmedia::math::Quat;
using bgmedia::math::Vec3;

// Re-orients one node's local axes by `r` while keeping every joint position
// and the orientation of all children unchanged. The skeleton keeps its shape
// but the bone axes (and therefore bind global rotations) differ, which is
// exactly what differs between exporters.
void reorientNode(VrmModel& model, int nodeIndex, const Quat& r) {
    auto& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
    node.rotation = (node.rotation * r).normalized();
    node.local = Mat4::fromTRS(node.translation, node.rotation, node.scale);
    const Quat inv = r.conjugate();
    for (int child : node.children) {
        auto& c = model.nodes[static_cast<std::size_t>(child)];
        c.translation = bgmedia::math::rotate(inv, c.translation);
        c.rotation = (inv * c.rotation).normalized();
        c.local = Mat4::fromTRS(c.translation, c.rotation, c.scale);
    }
}

void rebuildBind(VrmModel& model) {
    std::vector<int> stack(model.sceneRoots.begin(), model.sceneRoots.end());
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        const auto& node = model.nodes[static_cast<std::size_t>(index)];
        const auto i = static_cast<std::size_t>(index);
        if (node.parent >= 0) {
            const auto p = static_cast<std::size_t>(node.parent);
            model.bindGlobals[i] = model.bindGlobals[p] * node.local;
            model.bindGlobalRotations[i] =
                (model.bindGlobalRotations[p] * node.rotation).normalized();
        } else {
            model.bindGlobals[i] = node.local;
            model.bindGlobalRotations[i] = node.rotation.normalized();
        }
        for (int child : node.children) {
            stack.push_back(child);
        }
    }
}

std::unique_ptr<VrmModel> loadMinimal(bool skewedBind) {
    auto result = VrmLoader::load(fixturePath());
    if (!result.ok()) {
        return nullptr;
    }
    auto model = std::move(result.model);
    if (skewedBind) {
        using bgmedia::math::fromAxisAngle;
        using bgmedia::math::normalize;
        const HumanoidBone legs[] = {HumanoidBone::LeftUpperLeg, HumanoidBone::LeftLowerLeg,
                                     HumanoidBone::RightUpperLeg, HumanoidBone::RightLowerLeg,
                                     HumanoidBone::LeftFoot, HumanoidBone::RightFoot};
        float angle = 0.45f;
        for (HumanoidBone bone : legs) {
            reorientNode(*model, model->nodeFor(bone),
                         fromAxisAngle(normalize(Vec3{1.0f, 0.6f, -0.4f}), angle));
            angle += 0.21f;
        }
        rebuildBind(*model);
    }
    return model;
}

Vec3 clipToModel(const VrmModel& model, Vec3 v) {
    return (-model.right) * v.x + model.up * v.y + model.forward * v.z;
}

bgmedia::motion::ClipPose legReachPose() {
    auto pose = bgmedia::motion::neutralClipPose();
    pose.foot[0].position = {0.05f, 0.09f, 0.06f};
    pose.foot[0].contact = 0.0f;
    pose.foot[1].position = {-0.03f, 0.0f, 0.02f};
    pose.foot[1].contact = 1.0f;
    pose.hips = {0.0f, -0.04f, 0.0f};
    return pose;
}

} // namespace

TEST_CASE(spatial.foot_error_is_measured_on_the_evaluated_skeleton) {
    for (const bool skewed : {false, true}) {
        auto model = loadMinimal(skewed);
        CHECK(model != nullptr);
        HumanoidRetargeter rig(*model);
        CHECK(rig.valid());
        for (const float yaw : {0.0f, 0.25f, -0.6f}) {
            auto clip = legReachPose();
            clip.rootYaw = yaw;
            const auto out = rig.applyClip(clip, 0.0);
            CHECK(out.stats.valid);
            const HumanoidBone feet[2] = {HumanoidBone::LeftFoot, HumanoidBone::RightFoot};
            float worst = 0.0f;
            for (int f = 0; f < 2; ++f) {
                const int node = model->nodeFor(feet[f]);
                const Vec3 expected = model->boneBindPosition(feet[f]) +
                                      clipToModel(*model, clip.foot[f].position) *
                                          out.stats.height;
                const Vec3 actual = HumanoidRetargeter::stagePosition(out, node);
                worst = std::max(worst, (actual - expected).length());
            }
            // The reported number is the real, independently measured error.
            CHECK(worst < 0.003f);
            CHECK_NEAR(out.stats.maxFootError, worst, 0.0015);
            CHECK(out.stats.footSolverError < 0.003f);
            CHECK(out.stats.footReachDeficit < 0.003f);
        }
    }
}

TEST_CASE(spatial.unreachable_foot_target_splits_deficit_from_solver_error) {
    auto model = loadMinimal(true);
    CHECK(model != nullptr);
    HumanoidRetargeter rig(*model);
    auto clip = legReachPose();
    clip.foot[0].position = {0.0f, 1.2f, 0.0f};  // far beyond leg length
    const auto out = rig.applyClip(clip, 0.0);
    CHECK(out.stats.valid);
    CHECK(out.stats.footReachDeficit > 0.2f);
    CHECK(out.stats.maxFootError > 0.2f);
    // The leg still lands on the closest reachable point.
    CHECK(out.stats.footSolverError < 0.003f);
}

TEST_CASE(spatial.leg_pose_is_independent_of_bone_axis_orientation) {
    auto plain = loadMinimal(false);
    auto skew = loadMinimal(true);
    CHECK(plain != nullptr && skew != nullptr);
    HumanoidRetargeter a(*plain);
    HumanoidRetargeter b(*skew);
    const auto clip = legReachPose();
    const auto pa = a.applyClip(clip, 0.0);
    const auto pb = b.applyClip(clip, 0.0);
    for (HumanoidBone bone : {HumanoidBone::LeftLowerLeg, HumanoidBone::RightLowerLeg,
                              HumanoidBone::LeftFoot, HumanoidBone::RightFoot}) {
        const Vec3 x = pa.bonePosition(bone, *plain);
        const Vec3 y = pb.bonePosition(bone, *skew);
        // Knee position proves the calf direction is right, not just the ankle.
        CHECK((x - y).length() < 0.003f);
    }
}

TEST_CASE(spatial.apply_clip_honours_foot_ik_option) {
    auto model = loadMinimal(false);
    CHECK(model != nullptr);
    HumanoidRetargeter rig(*model);
    const auto clip = legReachPose();
    RetargetOptions on;
    RetargetOptions off;
    off.footIK = false;
    const auto withIk = rig.applyClip(clip, 0.0, on);
    const auto noIk = rig.applyClip(clip, 0.0, off);
    CHECK(withIk.stats.footIK);
    CHECK(!noIk.stats.footIK);
    CHECK(noIk.stats.valid);
    for (const auto& m : noIk.nodeGlobals) {
        CHECK(m.finite());
    }
    // Without IK the lifted foot is carried by the hips; it must not reach the
    // stage target that IK reaches.
    const int node = model->nodeFor(HumanoidBone::LeftFoot);
    const Vec3 a = HumanoidRetargeter::stagePosition(withIk, node);
    const Vec3 b = HumanoidRetargeter::stagePosition(noIk, node);
    CHECK((a - b).length() > 0.02f);
    CHECK(noIk.stats.maxFootError > withIk.stats.maxFootError);
}

TEST_CASE(spatial.wrist_axes_are_converted_exactly_once) {
    // The model's own axes differ from the clip axes (character faces +X).
    // Wrist roll/pitch authored in the clip frame must rotate the hand about the
    // model's forward / right axes: the axis is converted exactly once.
    auto model = loadMinimal(false);
    CHECK(model != nullptr);
    model->forward = {1.0f, 0.0f, 0.0f};
    model->right = {0.0f, 0.0f, -1.0f};  // up x forward
    HumanoidRetargeter rig(*model);
    auto base = bgmedia::motion::neutralClipPose();
    base.detailedHands = true;
    const auto a = rig.applyClip(base, 0.0);
    const int hand = model->nodeFor(HumanoidBone::RightHand);
    const auto ra = a.nodeGlobals[static_cast<std::size_t>(hand)].rotationQuat();
    const auto axisOf = [&](int channel) {
        auto rolled = base;
        rolled.handPose[1].wrist[static_cast<std::size_t>(channel)] = 1.0f;
        const auto b = rig.applyClip(rolled, 0.0);
        const auto rb = b.nodeGlobals[static_cast<std::size_t>(hand)].rotationQuat();
        const Quat delta = (rb * ra.conjugate()).normalized();
        return bgmedia::math::normalize(Vec3{delta.x, delta.y, delta.z});
    };
    CHECK(std::abs(bgmedia::math::dot(axisOf(0), model->right)) > 0.99f);
    CHECK(std::abs(bgmedia::math::dot(axisOf(1), model->up)) > 0.99f);
    CHECK(std::abs(bgmedia::math::dot(axisOf(2), model->forward)) > 0.99f);
}

// ---------------------------------------------------------------------------
// Collision-aware arm refinement (proxy based; not a mesh intersection test)
// ---------------------------------------------------------------------------

namespace {

bgmedia::motion::ClipPose crossBodyPose(float sweep, bool left = true);

struct ClearanceSweep {
    int frames = 0;
    int violatingFrames = 0;   // proxy penetration before refinement
    int remainingFrames = 0;   // proxy penetration after refinement
    float rawMin = 1.0f;
    float finalMin = 1.0f;
    float maxCorrection = 0.0f;
    int clamped = 0;
    float height = 1.0f;
};

ClearanceSweep sweepClearance(const std::filesystem::path& model, bool useMesh) {
    (void)useMesh;
    ClearanceSweep sweep;
    vrm::LoadResult loaded = vrm::VrmLoader::load(model);
    if (!loaded.ok()) {
        return sweep;
    }
    animation::HumanoidRetargeter rig(*loaded.model);
    if (!rig.valid()) {
        return sweep;
    }
    motion::DancePlayer player;
    player.loadLibrary(std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances");
    player.setOverlayMode(motion::OverlayMode::Full);
    for (const char* id : {"neonPulse", "streetCurrent", "fluidSignal"}) {
        if (!player.selectClip(id, 0.0, true)) continue;
        for (double t = 0.0; t < 40.0; t += 1.0 / 20.0) {
            const motion::DanceFrame frame = player.update(t);
            const auto pose = rig.applyClip(frame.pose, t);
            if (!pose.stats.armProxy) continue;
            ++sweep.frames;
            sweep.height = pose.stats.height;
            sweep.rawMin = std::min(sweep.rawMin, pose.stats.armClearanceRaw);
            sweep.finalMin = std::min(sweep.finalMin, pose.stats.armClearanceMin);
            if (pose.stats.armClearanceRaw < 0.0f) ++sweep.violatingFrames;
            if (pose.stats.armClearanceMin < -0.002f) ++sweep.remainingFrames;
            sweep.maxCorrection = std::max(sweep.maxCorrection, pose.stats.armCorrection);
            if (pose.stats.armCorrectionClamped) ++sweep.clamped;
        }
    }
    return sweep;
}

}  // namespace

TEST_CASE(spatial.model1_arm_proxy_clearance_report) {
    const auto path = realModelPath();
    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }
    vrm::LoadResult loaded = vrm::VrmLoader::load(path);
    REQUIRE(loaded.ok());
    animation::HumanoidRetargeter rig(*loaded.model);
    const auto info = rig.collisionProxy();
    CHECK(info.valid);
    CHECK(info.fromMesh);
    std::cout << std::fixed << std::setprecision(3) << "\n            proxy: halfWidth=" << info.torsoHalfWidth
              << " front=" << info.torsoHalfFront << " back=" << info.torsoHalfBack
              << " upper=" << info.upperArmRadius << " fore=" << info.foreArmRadius
              << " hand=" << info.handRadius;
    const ClearanceSweep sweep = sweepClearance(path, true);
    std::cout << "\n            frames=" << sweep.frames << " violating(raw)=" << sweep.violatingFrames
              << " remaining=" << sweep.remainingFrames << " rawMin=" << sweep.rawMin
              << " finalMin=" << sweep.finalMin << " maxCorrection=" << sweep.maxCorrection
              << " clamped=" << sweep.clamped;
    CHECK(sweep.frames > 100);
    // Cost of the refinement (proxy cached at load; per frame: a few dozen flops).
    const auto clip = crossBodyPose(0.8f);
    RetargetOptions off;
    off.armCollision = false;
    const int runs = 2000;
    const auto time = [&](const RetargetOptions& options) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < runs; ++i) {
            (void)rig.applyClip(clip, 0.0, options);
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / runs;
    };
    const double withMs = time(RetargetOptions{});
    const double withoutMs = time(off);
    std::cout << std::setprecision(4) << "\n            applyClip ms: refinement on=" << withMs
              << " off=" << withoutMs;
}

namespace {

// Forces the left forearm through the chest: swing the upper arm forward and
// across the body, bend the elbow. `sweep` 0..1 scales the whole gesture.
bgmedia::motion::ClipPose crossBodyPose(float sweep, bool left) {
    using bgmedia::math::fromAxisAngle;
    auto pose = bgmedia::motion::neutralClipPose();
    const auto upper = left ? HumanoidBone::LeftUpperArm : HumanoidBone::RightUpperArm;
    const auto lower = left ? HumanoidBone::LeftLowerArm : HumanoidBone::RightLowerArm;
    const float side = left ? 1.0f : -1.0f;
    // Clip frame: +Y up, +Z forward. Bring the arm down (about Z) then forward
    // and across (about Y), then fold the elbow.
    pose.bone[static_cast<std::size_t>(upper)] =
        (fromAxisAngle({0.0f, 1.0f, 0.0f}, side * -1.45f * sweep) *
         fromAxisAngle({0.0f, 0.0f, 1.0f}, -side * 1.15f * sweep))
            .normalized();
    pose.bone[static_cast<std::size_t>(lower)] =
        fromAxisAngle({0.0f, 1.0f, 0.0f}, side * -1.9f * sweep);
    return pose;
}

}  // namespace

TEST_CASE(spatial.arm_refinement_reduces_forced_penetration_smoothly) {
    const auto path = realModelPath();
    if (!std::filesystem::exists(path)) {
        return;
    }
    vrm::LoadResult loaded = vrm::VrmLoader::load(path);
    REQUIRE(loaded.ok());
    animation::HumanoidRetargeter rig(*loaded.model);
    RetargetOptions off;
    off.armCollision = false;
    const int wristNode = loaded.model->nodeFor(HumanoidBone::LeftHand);
    int improved = 0;
    int violating = 0;
    float worstAfter = 1.0f;
    float worstBefore = 1.0f;
    float maxStep = 0.0f;
    float maxPlainStep = 0.0f;
    bgmedia::math::Vec3 prevWrist{};
    bgmedia::math::Vec3 prevPlain{};
    bool havePrev = false;
    for (int i = 0; i <= 200; ++i) {
        const float sweep = static_cast<float>(i) / 200.0f;
        const auto pose = crossBodyPose(sweep);
        const auto fixed = rig.applyClip(pose, 0.0);
        const auto plain = rig.applyClip(pose, 0.0, off);
        CHECK(fixed.stats.valid);
        for (const auto& m : fixed.nodeGlobals) {
            CHECK(m.finite());
        }
        const auto wrist = HumanoidRetargeter::stagePosition(fixed, wristNode);
        const auto plainWrist = HumanoidRetargeter::stagePosition(plain, wristNode);
        if (fixed.stats.armClearanceRaw < -0.002f) {
            ++violating;
            if (fixed.stats.armClearanceMin > fixed.stats.armClearanceRaw + 1.0e-4f) {
                ++improved;
            }
        }
        worstAfter = std::min(worstAfter, fixed.stats.armClearanceMin);
        worstBefore = std::min(worstBefore, fixed.stats.armClearanceRaw);
        if (havePrev) {
            maxStep = std::max(maxStep, (wrist - prevWrist).length());
            maxPlainStep = std::max(maxPlainStep, (plainWrist - prevPlain).length());
        }
        prevWrist = wrist;
        prevPlain = plainWrist;
        havePrev = true;
    }
    std::cout << std::fixed << std::setprecision(4) << "\n            violating frames=" << violating
              << " improved=" << improved << " worst clearance before=" << worstBefore
              << " after=" << worstAfter << " max wrist step (refined)=" << maxStep
              << " (unrefined)=" << maxPlainStep;
    CHECK(violating > 20);  // the gesture really does penetrate the proxy
    CHECK(improved == violating);
    CHECK(worstAfter > worstBefore + 0.01f);
    CHECK(worstAfter > -0.005f);  // within the proxy safety gap
    // The correction is a smooth function of the pose: it may not create a
    // wrist jump beyond a few times the unrefined motion.
    CHECK(maxStep < 3.0f * maxPlainStep + 0.01f);
}

TEST_CASE(spatial.arm_refinement_never_touches_feet_and_is_inert_when_clear) {
    for (const bool useMinimal : {true, false}) {
        auto model = useMinimal ? loadMinimal(false) : [] {
            auto r = VrmLoader::load(realModelPath());
            return std::move(r.model);
        }();
        if (!model) {
            continue;
        }
        HumanoidRetargeter rig(*model);
        RetargetOptions off;
        off.armCollision = false;
        auto clear = bgmedia::motion::neutralClipPose();  // arms out: nothing to correct
        clear.foot[0].position = {0.04f, 0.05f, 0.0f};
        const auto a = rig.applyClip(clear, 0.0);
        const auto b = rig.applyClip(clear, 0.0, off);
        CHECK(a.stats.armCorrection == 0.0f);
        for (std::size_t i = 0; i < a.nodeGlobals.size(); ++i) {
            CHECK((a.nodeGlobals[i].translationPart() - b.nodeGlobals[i].translationPart()).length() < 1.0e-6f);
        }
        // Even with a heavy correction the stage-space feet do not move.
        const auto pose = crossBodyPose(1.0f);
        const auto c = rig.applyClip(pose, 0.0);
        const auto d = rig.applyClip(pose, 0.0, off);
        for (HumanoidBone foot : {HumanoidBone::LeftFoot, HumanoidBone::RightFoot}) {
            const int node = model->nodeFor(foot);
            CHECK((HumanoidRetargeter::stagePosition(c, node) -
                   HumanoidRetargeter::stagePosition(d, node)).length() < 1.0e-5f);
        }
        CHECK(c.stats.footSolverError < 0.003f);
    }
}

// The detailed hand layer and the clip's own hand-bone rotation must agree on
// axis AND sign, on any model basis: a wrist channel is just a clip-frame
// rotation of the hand bone with a fixed gain.
TEST_CASE(spatial.wrist_channels_match_the_clip_bone_path_with_sign) {
    struct Basis {
        const char* name;
        bgmedia::math::Vec3 forward;
        bgmedia::math::Vec3 right;
    };
    const Basis bases[] = {
        {"standard", {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {"faces +X", {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {"faces -Z (Model1)", {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}},
    };
    struct Channel {
        int index;
        bgmedia::math::Vec3 clipAxis;
        float gain;
    };
    const Channel channels[] = {{0, {-1.0f, 0.0f, 0.0f}, 0.26f},  // pitch (established sign)
                                {1, {0.0f, 1.0f, 0.0f}, 0.30f},   // yaw
                                {2, {0.0f, 0.0f, 1.0f}, 0.34f}};  // roll
    for (const Basis& basis : bases) {
        auto model = loadMinimal(false);
        REQUIRE(model != nullptr);
        model->forward = basis.forward;
        model->right = basis.right;
        HumanoidRetargeter rig(*model);
        for (const HumanoidBone handBone : {HumanoidBone::LeftHand, HumanoidBone::RightHand}) {
            const int sd = handBone == HumanoidBone::LeftHand ? 0 : 1;
            const int node = model->nodeFor(handBone);
            for (const Channel& channel : channels) {
                auto viaBone = bgmedia::motion::neutralClipPose();
                viaBone.bone[static_cast<std::size_t>(handBone)] =
                    bgmedia::math::fromAxisAngle(channel.clipAxis, channel.gain);
                auto viaHandPose = bgmedia::motion::neutralClipPose();
                viaHandPose.detailedHands = true;
                viaHandPose.handPose[static_cast<std::size_t>(sd)].wrist[static_cast<std::size_t>(channel.index)] = 1.0f;
                const auto a = rig.applyClip(viaBone, 0.0);
                const auto b = rig.applyClip(viaHandPose, 0.0);
                const auto qa = a.nodeGlobals[static_cast<std::size_t>(node)].rotationQuat();
                const auto qb = b.nodeGlobals[static_cast<std::size_t>(node)].rotationQuat();
                // |dot| = 1 means the same rotation; a flipped sign would give cos(2*angle).
                CHECK(std::abs(bgmedia::math::dot(qa, qb)) > 0.99999f);
            }
        }
    }
}

TEST_CASE(spatial.thumb_opposition_follows_the_curl_axis_and_sign) {
    const bgmedia::math::Vec3 forwards[] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
    const bgmedia::math::Vec3 rights[] = {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}};
    // The minimal fixture has no finger bones, so this needs the optional private
    // model (the basis override only changes the axes the retargeter uses).
    if (!std::filesystem::exists(realModelPath())) {
        return;
    }
    int checkedThumbs = 0;
    for (int basis = 0; basis < 3; ++basis) {
        auto loaded = VrmLoader::load(realModelPath());
        REQUIRE(loaded.ok());
        auto model = std::move(loaded.model);
        model->forward = forwards[basis];
        model->right = rights[basis];
        HumanoidRetargeter rig(*model);
        for (int sd = 0; sd < 2; ++sd) {
            const int thumb = model->fingerNode(sd, 0, 0);
            if (thumb < 0) {
                continue;
            }
            ++checkedThumbs;
            auto neutral = bgmedia::motion::neutralClipPose();
            neutral.detailedHands = true;
            auto curled = neutral;
            curled.handPose[static_cast<std::size_t>(sd)].curl[0] = {1.0f, 1.0f, 1.0f};
            auto opposed = neutral;
            opposed.handPose[static_cast<std::size_t>(sd)].thumbOpposition = 1.0f;
            const auto rest = rig.applyClip(neutral, 0.0).nodeGlobals[static_cast<std::size_t>(thumb)].rotationQuat();
            const auto axisOf = [&](const bgmedia::motion::ClipPose& pose) {
                auto q = (rig.applyClip(pose, 0.0).nodeGlobals[static_cast<std::size_t>(thumb)].rotationQuat() *
                          rest.conjugate()).normalized();
                if (q.w < 0.0f) {
                    q = {-q.x, -q.y, -q.z, -q.w};
                }
                return bgmedia::math::normalize(bgmedia::math::Vec3{q.x, q.y, q.z});
            };
            // Both thumb motions rotate about the model forward axis, same direction.
            CHECK(bgmedia::math::dot(axisOf(curled), axisOf(opposed)) > 0.99f);
            CHECK(std::abs(bgmedia::math::dot(axisOf(opposed), model->forward)) > 0.99f);
        }
    }
    CHECK(checkedThumbs == 6);
}
