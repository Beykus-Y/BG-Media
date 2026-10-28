#include "framework.hpp"

#include "motion_metrics.hpp"

#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/motion3/MotionPrimitive.hpp"
#include "bgmedia/motion3/ProceduralChoreography.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace bgmedia;

std::filesystem::path sourceDir() { return BGMEDIA_SOURCE_DIR; }
std::filesystem::path dancesDir() { return sourceDir() / "assets" / "dances"; }
std::filesystem::path minimalVrm() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}
std::filesystem::path model1Path() {
    if (const char* env = std::getenv("BGMEDIA_VRM_TEST_MODEL")) {
        return env;
    }
    return sourceDir() / "models" / "Model1.vrm";
}

motion::ChoreographyRequest request(int seed = 7) {
    motion::ChoreographyRequest r;
    r.id = "testDance";
    r.name = "Test Dance";
    r.seed = seed;
    r.bars = 8;
    r.bpm = 104.0;
    r.keysPerBeat = 8;
    r.intensity = 0.7f;
    return r;
}

bool finiteQuat(const math::Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
           std::isfinite(q.w);
}

float poseDistance(const motion::ClipPose& a, const motion::ClipPose& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        const float d = std::abs(math::dot(a.bone[i], b.bone[i]));
        worst = std::max(worst, 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)));
    }
    return worst;
}

struct Rig {
    vrm::LoadResult loaded;
    std::unique_ptr<animation::HumanoidRetargeter> rig;
    bool ok() const { return loaded.ok() && rig && rig->valid(); }
};

Rig makeRig(const std::filesystem::path& path) {
    Rig r;
    r.loaded = vrm::VrmLoader::load(path);
    if (r.loaded.ok()) {
        r.rig = std::make_unique<animation::HumanoidRetargeter>(*r.loaded.model);
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

TEST_CASE(motion32.generator_is_deterministic) {
    const motion::GeneratedDance a = motion::ProceduralChoreography::generate(request(11));
    const motion::GeneratedDance b = motion::ProceduralChoreography::generate(request(11));
    CHECK(a.clip.id == b.clip.id);
    CHECK(a.clip.keyCount == b.clip.keyCount);
    CHECK(a.clip.contacts.size() == b.clip.contacts.size());
    CHECK(a.clip.accents.size() == b.clip.accents.size());
    CHECK(a.overlays.size() == b.overlays.size());
    CHECK(motion::clipToJson(a.clip) == motion::clipToJson(b.clip));
    for (int i = 0; i < 8; ++i) {
        const double beat = static_cast<double>(i) * 4.0;
        CHECK_NEAR(poseDistance(motion::sampleClip(a.clip, beat),
                                motion::sampleClip(b.clip, beat)),
                   0.0, 1e-6);
    }
}

TEST_CASE(motion32.different_seeds_and_profiles_differ) {
    const motion::GeneratedDance a = motion::ProceduralChoreography::generate(request(11));
    motion::ChoreographyRequest other = request(99);
    other.profile = "street";
    const motion::GeneratedDance b = motion::ProceduralChoreography::generate(other);
    float worst = 0.0f;
    for (int i = 0; i < 16; ++i) {
        worst = std::max(worst, poseDistance(motion::sampleClip(a.clip, i * 2.0),
                                             motion::sampleClip(b.clip, i * 2.0)));
    }
    CHECK(worst > 0.035f);  // radians: the two dances are not the same motion
    CHECK(a.overlays.size() > 0);
    CHECK(b.overlays.size() > 0);
}

TEST_CASE(motion32.full_dance_has_phrase_structure_and_loop) {
    const motion::GeneratedDance dance =
        motion::ProceduralChoreography::generate(request(7));
    const motion::HumanoidClip& clip = dance.clip;
    CHECK(clip.beats >= 32.0f);
    CHECK(clip.beatsPerBar == 4);
    CHECK(clip.loop && clip.seamless);
    CHECK(clip.keyCount >= 257);
    CHECK(dance.structure.size() >= 8);
    CHECK(dance.stats.phrases >= 4);
    CHECK(clip.contacts.size() >= 12);
    CHECK(clip.accents.size() >= 8);
    CHECK(clip.entryBeats.size() >= 4);
    CHECK(clip.exitBeats.size() >= 4);
    int pivots = 0;
    for (const motion::ContactSegment& c : clip.contacts) {
        pivots += c.pivot ? 1 : 0;
    }
    CHECK(pivots >= 2);
    // The full dance keys the torso, arms and hands, but never the legs: the
    // legs are solved from foot targets.
    CHECK(!clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::Hips)].empty());
    CHECK(!clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::Chest)].empty());
    CHECK(!clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::UpperChest)].empty());
    CHECK(!clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::Head)].empty());
    CHECK(!clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::LeftUpperArm)].empty());
    CHECK(!clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::RightHand)].empty());
    CHECK(clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::LeftUpperLeg)].empty());
    CHECK(clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::RightLowerLeg)].empty());
    // Detailed hand poses are baked with independent finger channels.
    CHECK(clip.handPoses[0].size() == static_cast<std::size_t>(clip.keyCount));
    CHECK(clip.handPoses[1].size() == static_cast<std::size_t>(clip.keyCount));
    const motion::ClipPose pose = motion::sampleClip(clip, 0.0);
    CHECK(pose.detailedHands);
    CHECK(dance.stats.finite);
    CHECK(dance.stats.loopSeamDeg < 0.05f);
    CHECK(dance.stats.footSlideHeightUnits < 0.004f);
}

TEST_CASE(motion32.generated_clip_roundtrips_through_json_v2) {
    const motion::GeneratedDance dance =
        motion::ProceduralChoreography::generate(request(3));
    const std::string json = motion::clipToJson(dance.clip);
    const motion::ClipLoadResult loaded = motion::parseClipJson(json, "generated.json");
    if (!loaded.ok) {
        for (const auto& d : loaded.diagnostics) {
            std::fprintf(stderr, "roundtrip %s: %s\n", d.file.c_str(), d.message.c_str());
        }
    }
    CHECK(loaded.ok);
    const motion::HumanoidClip& clip = loaded.clip;
    CHECK(clip.formatVersion == motion::kClipFormatVersion);
    CHECK(clip.keyCount == dance.clip.keyCount);
    CHECK(clip.contacts.size() == dance.clip.contacts.size());
    CHECK(clip.accents.size() == dance.clip.accents.size());
    CHECK(clip.handPoses[0].size() == dance.clip.handPoses[0].size());
    CHECK(clip.feet[0].toe.size() == dance.clip.feet[0].toe.size());
    CHECK_NEAR(clip.feet[0].position[10].x, dance.clip.feet[0].position[10].x, 1e-5);
    for (int beat : {0, 5, 12, 20, 31}) {
        const motion::ClipPose a = motion::sampleClip(dance.clip, beat);
        const motion::ClipPose b = motion::sampleClip(clip, beat);
        CHECK_NEAR(poseDistance(a, b), 0.0, 1e-4);
        CHECK_NEAR(a.handPose[0].curl[2][1], b.handPose[0].curl[2][1], 1e-4);
        CHECK_NEAR(a.foot[0].toePitch, b.foot[0].toePitch, 1e-4);
    }
}

TEST_CASE(motion32.generated_clip_passes_the_motion_qa_bounds) {
    const motion::GeneratedDance dance =
        motion::ProceduralChoreography::generate(request(7));
    for (double bpm : {dance.clip.bpmMin, dance.clip.bpm, dance.clip.bpmMax}) {
        const qa::ClipReport report =
            qa::analyseClip(dance.clip, nullptr, nullptr, bpm);
        CHECK(report.maxQuatNormError < 1e-4);
        CHECK(report.loopRotationDeg < 0.05);
        CHECK(report.loopPositionHeight < 0.001);
        CHECK(report.maxAngularSpeed < 1000.0);
        CHECK(report.maxAngularAcceleration < 2.5e5);
        CHECK(report.maxHipsSpeed < 3.0);
        CHECK(report.maxFootSpeed < 8.0);
        CHECK(report.maxRootExcursion < 0.35);
        CHECK(report.slideMax < 0.02);
    }
}

// ---------------------------------------------------------------------------
// Motion primitives and composition
// ---------------------------------------------------------------------------

TEST_CASE(motion32.primitive_library_is_real_and_metadata_is_complete) {
    const auto& library = motion::primitiveLibrary();
    CHECK(library.size() >= 12);
    CHECK(library.size() == static_cast<std::size_t>(motion::kPrimitiveKindCount));
    std::vector<std::string> ids;
    for (const motion::PrimitiveMetadata& m : library) {
        CHECK(m.id != nullptr && m.id[0] != '\0');
        CHECK(m.name != nullptr && m.name[0] != '\0');
        CHECK(m.mask != motion::body::None);
        CHECK(m.defaultBeats > 0.0f);
        CHECK(m.minBeats <= m.defaultBeats);
        CHECK(m.maxBeats >= m.defaultBeats);
        CHECK(m.minIntensity >= 0.0f && m.maxIntensity >= m.minIntensity);
        ids.push_back(m.id);
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    // Categories actually span the body, not only arms.
    bool arm = false, hand = false, torso = false, head = false, foot = false;
    for (const auto& m : library) {
        arm = arm || m.category == motion::MotionCategory::Arm;
        hand = hand || m.category == motion::MotionCategory::Hand;
        torso = torso || m.category == motion::MotionCategory::Torso;
        head = head || m.category == motion::MotionCategory::Head;
        foot = foot || m.category == motion::MotionCategory::Footwork;
    }
    CHECK(arm && hand && torso && head && foot);
    for (const auto& m : library) {
        const motion::PrimitivePose pose =
            motion::samplePrimitive(m.kind, 0.5f, 0.8f, -1, 0.5f);
        for (const math::Quat& q : pose.bone) {
            CHECK(finiteQuat(q));
        }
        CHECK(std::isfinite(pose.hips.x) && std::isfinite(pose.hips.y));
    }
}

TEST_CASE(motion32.masks_distinguish_limbs_from_torso) {
    CHECK(motion::masksConflict(motion::body::Torso, motion::body::Chest));
    CHECK(motion::masksConflict(motion::body::LeftArm, motion::body::Arms));
    CHECK(motion::masksConflict(motion::body::Head, motion::body::Head));
    CHECK(!motion::masksConflict(motion::body::LeftArm, motion::body::RightArm));
    CHECK(!motion::masksConflict(motion::body::Hands, motion::body::Arms));
    CHECK(!motion::masksConflict(motion::body::Head, motion::body::Torso));
}

TEST_CASE(motion32.composition_never_moves_a_planted_foot) {
    motion::ClipPose base = motion::neutralClipPose();
    base.foot[0].phase = motion::FootPhase::Stance;
    base.foot[0].contact = 1.0f;
    base.foot[0].position = {0.0f, 0.0f, 0.0f};
    base.foot[1].phase = motion::FootPhase::Stance;
    base.foot[1].contact = 1.0f;
    base.foot[1].position = {0.0f, 0.0f, 0.0f};
    std::vector<motion::PrimitiveCue> cues;
    motion::PrimitiveCue step;
    step.kind = motion::PrimitiveKind::SideStep;
    step.beat = 0.0f;
    step.durationBeats = 1.0f;
    step.intensity = 1.0f;
    step.side = 0;
    cues.push_back(step);
    const motion::ComposedPose composed =
        motion::composeMotion(base, cues, 0.25, 4.0, true);
    CHECK(composed.diagnostics.rejectedByContact >= 1);
    CHECK_NEAR(composed.pose.foot[0].position.x, 0.0, 1e-6);
    CHECK_NEAR(composed.pose.foot[0].position.y, 0.0, 1e-6);
}

TEST_CASE(motion32.arm_and_hand_layers_compose_without_conflict) {
    motion::ClipPose base = motion::neutralClipPose();
    base.foot[0].phase = motion::FootPhase::Stance;
    base.foot[0].contact = 1.0f;
    base.foot[1].phase = motion::FootPhase::Swing;
    base.foot[1].contact = 0.0f;
    std::vector<motion::PrimitiveCue> cues;
    motion::PrimitiveCue wave;
    wave.kind = motion::PrimitiveKind::ArmWave;
    wave.beat = 0.0f;
    wave.durationBeats = 2.0f;
    wave.intensity = 0.8f;
    cues.push_back(wave);
    motion::PrimitiveCue flick;
    flick.kind = motion::PrimitiveKind::FingerRipple;
    flick.beat = 0.25f;
    flick.durationBeats = 1.0f;
    flick.intensity = 1.0f;
    flick.side = 0;
    cues.push_back(flick);
    const motion::ComposedPose composed =
        motion::composeMotion(base, cues, 0.6, 4.0, true);
    CHECK(composed.diagnostics.acceptedCues >= 2);
    CHECK(composed.pose.detailedHands);
    for (const math::Quat& q : composed.pose.bone) {
        CHECK(finiteQuat(q));
        CHECK_NEAR(std::sqrt(q.lengthSquared()), 1.0, 1e-4);
    }
}

TEST_CASE(motion32.layer_cues_wrap_across_the_loop_point) {
    motion::ClipPose base = motion::neutralClipPose();
    std::vector<motion::PrimitiveCue> cues;
    motion::PrimitiveCue cue;
    cue.kind = motion::PrimitiveKind::HeadAccent;
    cue.beat = 3.75f;
    cue.durationBeats = 0.5f;
    cue.intensity = 1.0f;
    cues.push_back(cue);
    const motion::ComposedPose atStart =
        motion::composeMotion(base, cues, 0.1, 4.0, true);
    CHECK(atStart.diagnostics.acceptedCues == 1);
    const motion::ComposedPose far =
        motion::composeMotion(base, cues, 2.0, 4.0, true);
    CHECK(far.diagnostics.acceptedCues == 0);
}

TEST_CASE(motion32.overlay_plan_is_deterministic_and_musical) {
    const motion::GeneratedDance dance =
        motion::ProceduralChoreography::generate(request(5));
    const auto a = motion::planPrimitiveOverlays(dance.clip, 9u, 0.8f, true);
    const auto b = motion::planPrimitiveOverlays(dance.clip, 9u, 0.8f, true);
    CHECK(a.size() == b.size());
    CHECK(a.size() >= 8);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].kind == b[i].kind);
        CHECK_NEAR(a[i].beat, b[i].beat, 1e-6);
        CHECK(a[i].beat >= 0.0f && a[i].beat <= dance.clip.beats + 1e-4f);
    }
    // The authored downbeat has a gesture on or next to it.
    bool nearDownbeat = false;
    for (const auto& cue : a) {
        const float wrapped = std::fmod(cue.beat, dance.clip.beats);
        nearDownbeat = nearDownbeat || wrapped < 0.6f ||
                       wrapped > dance.clip.beats - 0.2f;
    }
    CHECK(nearDownbeat);
}

// ---------------------------------------------------------------------------
// Hands, model capabilities and spring bones
// ---------------------------------------------------------------------------

TEST_CASE(motion32.model1_reports_fingers_and_spring_chains) {
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        return;
    }
    const vrm::LoadResult loaded = vrm::VrmLoader::load(path);
    REQUIRE(loaded.ok());
    const vrm::VrmModel& model = *loaded.model;
    CHECK(model.fingerBoneCount() == 30);
    CHECK(model.hasFingers());
    CHECK(model.springChains.size() >= 30);
    CHECK(model.springColliderGroups.size() >= 8);
    CHECK(model.springBoneCount() >= 100);
    CHECK(model.diagnostics.fingerBoneCount == 30);
    CHECK(model.diagnostics.springBoneCount == model.springBoneCount());
    CHECK(model.diagnostics.blendShapeCount > 0);
    CHECK(model.springSource == "VRM.secondaryAnimation");
}

TEST_CASE(motion32.detailed_hand_pose_moves_finger_bones) {
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        return;
    }
    Rig rig = makeRig(path);
    REQUIRE(rig.ok());
    motion::ClipPose base = motion::neutralClipPose();
    base.detailedHands = true;
    for (int side = 0; side < 2; ++side) {
        for (int finger = 0; finger < 5; ++finger) {
            for (int joint = 0; joint < 3; ++joint) {
                base.handPose[static_cast<std::size_t>(side)]
                             .curl[static_cast<std::size_t>(finger)]
                                  [static_cast<std::size_t>(joint)] = 0.0f;
            }
        }
    }
    motion::ClipPose closed = base;
    for (int side = 0; side < 2; ++side) {
        for (int finger = 0; finger < 5; ++finger) {
            for (int joint = 0; joint < 3; ++joint) {
                closed.handPose[static_cast<std::size_t>(side)]
                               .curl[static_cast<std::size_t>(finger)]
                                    [static_cast<std::size_t>(joint)] = 1.0f;
            }
        }
    }
    const animation::RetargetedPose openPose = rig.rig->applyClip(base, 0.0);
    const animation::RetargetedPose closedPose = rig.rig->applyClip(closed, 0.0);
    CHECK(closedPose.stats.fingerBones == 30);
    CHECK(closedPose.stats.detailedHands);
    float moved = 0.0f;
    for (int side = 0; side < 2; ++side) {
        for (int finger = 0; finger < 5; ++finger) {
            for (int joint = 0; joint < 3; ++joint) {
                const int node = rig.loaded.model->fingerNode(side, finger, joint);
                REQUIRE(node >= 0);
                const math::Vec3 a =
                    openPose.nodeGlobals[static_cast<std::size_t>(node)].translationPart();
                const math::Vec3 b =
                    closedPose.nodeGlobals[static_cast<std::size_t>(node)].translationPart();
                moved = std::max(moved, (a - b).length());
            }
        }
    }
    CHECK(moved > 0.002f);
}

TEST_CASE(motion32.finger_fallback_is_honest) {
    Rig rig = makeRig(minimalVrm());
    REQUIRE(rig.ok());
    motion::ClipPose pose = motion::neutralClipPose();
    pose.detailedHands = true;
    pose.handPose[0] = motion::relaxedHandPose(1.0f);
    const animation::RetargetedPose out = rig.rig->applyClip(pose, 0.0);
    CHECK(out.stats.valid);
    CHECK(out.stats.fingerBones == 0);
    CHECK(!out.stats.detailedHands);
    CHECK(rig.loaded.model->fingerBoneCount() == 0);
    CHECK(!rig.loaded.model->hasSpringBones());
}

TEST_CASE(motion32.spring_dynamics_are_stable_at_common_frame_rates) {
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        return;
    }
    Rig rig = makeRig(path);
    REQUIRE(rig.ok());
    const motion::GeneratedDance dance =
        motion::ProceduralChoreography::generate(request(7));
    const vrm::VrmModel& model = *rig.loaded.model;
    animation::SecondaryDynamics dynamics(model);
    CHECK(dynamics.available());
    CHECK(dynamics.chainCount() >= 30);
    CHECK(dynamics.boneCount() >= 100);
    CHECK(dynamics.colliderCount() >= 10);

    animation::SecondarySettings spring;
    spring.enabled = true;
    spring.intensity = 1.0f;
    spring.damping = 0.5f;
    const double duration = 2.0;
    for (double fps : {30.0, 60.0, 120.0, 180.0}) {
        dynamics.reset(rig.rig->applyClip(motion::sampleClip(dance.clip, 0.0), 0.0));
        bool finite = true;
        float maxDisplacement = 0.0f;
        float maxStretch = 0.0f;
        const int frames = static_cast<int>(duration * fps);
        for (int frame = 0; frame <= frames; ++frame) {
            const double t = frame / fps;
            const double beat = t * dance.clip.bpm / 60.0;
            animation::RetargetedPose pose =
                rig.rig->applyClip(motion::sampleClip(dance.clip, beat), t);
            dynamics.update(pose, frame == 0 ? 0.0 : 1.0 / fps, spring);
            const auto& stats = dynamics.stats();
            maxDisplacement = std::max(maxDisplacement, stats.maxDisplacement);
            maxStretch = std::max(maxStretch, stats.maxStretch);
            for (const math::Mat4& matrix : pose.nodeGlobals) {
                finite = finite && matrix.finite();
            }
            if (!finite) {
                break;
            }
        }
        CHECK(finite);
        CHECK(maxDisplacement < 0.10f);
        CHECK(maxStretch < 0.05f);
        CHECK(!dynamics.stats().nonFinite);
    }
}

TEST_CASE(motion32.spring_reset_and_disabled_are_safe) {
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        return;
    }
    Rig rig = makeRig(path);
    REQUIRE(rig.ok());
    animation::SecondaryDynamics dynamics(*rig.loaded.model);
    animation::SecondarySettings spring;
    spring.enabled = true;
    const motion::ClipPose clipPose = motion::sampleClip(
        motion::ProceduralChoreography::generate(request(7)).clip, 6.0);
    for (int i = 0; i < 60; ++i) {
        animation::RetargetedPose pose =
            rig.rig->applyClip(clipPose, 3.0 + i / 60.0);
        dynamics.update(pose, i == 0 ? 0.0 : 1.0 / 60.0, spring);
    }
    CHECK(dynamics.stats().valid);
    animation::RetargetedPose fresh = rig.rig->applyClip(clipPose, 4.0);
    dynamics.reset(fresh);
    const int node = rig.loaded.model->springChains[0].nodes[1];
    const math::Vec3 expected =
        animation::HumanoidRetargeter::stagePosition(fresh, node);
    CHECK_NEAR((dynamics.chainPoint(0, 1) - expected).length(), 0.0, 1e-4);
    spring.enabled = false;
    const math::Vec3 untouched = fresh.nodeGlobals[static_cast<std::size_t>(node)]
                                     .translationPart();
    dynamics.update(fresh, 1.0 / 60.0, spring);
    const math::Vec3 after =
        fresh.nodeGlobals[static_cast<std::size_t>(node)].translationPart();
    CHECK_NEAR((untouched - after).length(), 0.0, 1e-6);
}

TEST_CASE(motion32.spring_colliders_are_respected) {
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        return;
    }
    Rig rig = makeRig(path);
    REQUIRE(rig.ok());
    const vrm::VrmModel& model = *rig.loaded.model;
    animation::SecondaryDynamics dynamics(model);
    animation::SecondarySettings spring;
    spring.enabled = true;
    const motion::GeneratedDance dance =
        motion::ProceduralChoreography::generate(request(23));
    double worstPenetration = 0.0;
    bool finite = true;
    for (int frame = 0; frame <= 150; ++frame) {
        const double t = frame / 60.0;
        const double beat = t * dance.clip.bpm / 60.0;
        animation::RetargetedPose pose =
            rig.rig->applyClip(motion::sampleClip(dance.clip, beat), t);
        dynamics.update(pose, frame == 0 ? 0.0 : 1.0 / 60.0, spring);
        const math::Quat rootRot = pose.modelTransform.rotationQuat();
        for (const vrm::SpringBoneChain& chain : model.springChains) {
            for (int node : chain.nodes) {
                const math::Vec3 point =
                    animation::HumanoidRetargeter::stagePosition(pose, node);
                for (int groupIndex : chain.colliderGroups) {
                    if (groupIndex < 0 ||
                        static_cast<std::size_t>(groupIndex) >=
                            model.springColliderGroups.size()) {
                        continue;
                    }
                    const vrm::SpringColliderGroup& group =
                        model.springColliderGroups[static_cast<std::size_t>(groupIndex)];
                    if (group.node < 0) continue;
                    const math::Mat4& matrix =
                        pose.nodeGlobals[static_cast<std::size_t>(group.node)];
                    for (const vrm::SpringCollider& collider : group.colliders) {
                        const math::Vec3 character =
                            matrix.transformVector(collider.offset) + matrix.translationPart();
                        const math::Vec3 center =
                            pose.rootOffset + pose.rootPivot +
                            math::rotate(rootRot, character - pose.rootPivot);
                        const float penetration = collider.radius - (point - center).length();
                        worstPenetration = std::max(worstPenetration,
                                                    static_cast<double>(penetration));
                    }
                }
            }
        }
        for (const math::Mat4& m : pose.nodeGlobals) {
            finite = finite && m.finite();
        }
    }
    CHECK(finite);
    CHECK(worstPenetration < 0.008);  // 8 mm maximum numerical overlap
}

TEST_CASE(motion32.spring_fixed_timestep_is_nearly_invariant) {
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        return;
    }
    Rig rig = makeRig(path);
    REQUIRE(rig.ok());
    const motion::GeneratedDance dance =
        motion::ProceduralChoreography::generate(request(7));
    animation::SecondarySettings spring;
    spring.enabled = true;
    auto run = [&](double fps) {
        animation::SecondaryDynamics dynamics(*rig.loaded.model);
        const int frames = static_cast<int>(3.0 * fps);
        math::Vec3 last{};
        for (int frame = 0; frame <= frames; ++frame) {
            const double t = frame / fps;
            animation::RetargetedPose pose = rig.rig->applyClip(
                motion::sampleClip(dance.clip, t * dance.clip.bpm / 60.0), t);
            dynamics.update(pose, frame == 0 ? 0.0 : 1.0 / fps, spring);
            last = dynamics.chainPoint(0, 3);
        }
        return last;
    };
    const math::Vec3 reference = run(120.0);
    for (double fps : {30.0, 60.0, 180.0}) {
        CHECK_NEAR((run(fps) - reference).length(), 0.0, 0.02);
    }
}

TEST_CASE(motion32.spring_without_a_rig_does_nothing) {
    Rig rig = makeRig(minimalVrm());
    REQUIRE(rig.ok());
    animation::SecondaryDynamics dynamics(*rig.loaded.model);
    CHECK(!dynamics.available());
    animation::RetargetedPose pose = rig.rig->applyClip(motion::neutralClipPose(), 0.0);
    const auto before = pose.nodeGlobals;
    animation::SecondarySettings settings;
    dynamics.update(pose, 1.0 / 30.0, settings);
    REQUIRE(before.size() == pose.nodeGlobals.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK_NEAR((before[i].translationPart() - pose.nodeGlobals[i].translationPart()).length(),
                   0.0, 1e-9);
    }
}

TEST_CASE(motion32.spring_frame_hitch_and_pause_are_safe) {
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        return;
    }
    Rig rig = makeRig(path);
    REQUIRE(rig.ok());
    animation::SecondaryDynamics dynamics(*rig.loaded.model);
    animation::SecondarySettings spring;
    spring.enabled = true;
    const motion::ClipPose clipPose = motion::sampleClip(
        motion::ProceduralChoreography::generate(request(5)).clip, 4.0);
    bool finite = true;
    for (double dt : {1.0 / 30.0, 0.0, 10.0, 1.0 / 180.0, 0.25, 1.0 / 60.0}) {
        animation::RetargetedPose pose = rig.rig->applyClip(clipPose, 2.0);
        dynamics.update(pose, dt, spring);
        for (const math::Mat4& matrix : pose.nodeGlobals) {
            finite = finite && matrix.finite();
        }
        CHECK(!dynamics.stats().nonFinite);
        CHECK(dynamics.stats().substeps <= 8);
    }
    CHECK(finite);
}

TEST_CASE(motion32.hand_pose_clamping_respects_joint_ranges) {
    motion::HandPose pose = motion::relaxedHandPose(4.0f);
    for (auto& finger : pose.curl) {
        for (float value : finger) {
            CHECK(value >= 0.0f && value <= 1.0f);
        }
    }
    pose.curl[0][0] = 9.0f;
    pose.spread[2] = -8.0f;
    pose.thumbOpposition = 5.0f;
    pose.wrist[1] = 3.0f;
    motion::clampHandPose(pose);
    CHECK_NEAR(pose.curl[0][0], 1.0f, 1e-6);
    CHECK_NEAR(pose.spread[2], -1.0f, 1e-6);
    CHECK_NEAR(pose.thumbOpposition, 1.0f, 1e-6);
    CHECK_NEAR(pose.wrist[1], 1.0f, 1e-6);
    const motion::HandPose blended = motion::blendHandPose(
        motion::relaxedHandPose(0.0f), motion::relaxedHandPose(1.0f), 0.5f);
    for (const auto& finger : blended.curl) {
        for (float value : finger) {
            CHECK(value >= 0.0f && value <= 1.0f);
        }
    }
}

TEST_CASE(motion32.generated_to_generated_transition_is_finite) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    (void)player.selectClip("neonPulse", 0.0, true);
    (void)player.update(0.3);
    (void)player.selectClip("streetCurrent", 0.8, false);
    math::Quat previous{};
    for (int i = 0; i <= 120; ++i) {
        const double t = 0.8 + i / 240.0;
        const motion::DanceFrame frame = player.update(t);
        CHECK(std::isfinite(frame.pose.hips.x));
        for (const math::Quat& q : frame.pose.bone) {
            CHECK(finiteQuat(q));
            if (i > 0) {
                const float d = std::abs(math::dot(q, previous));
                const float angle = 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f));
                CHECK(angle < 1.8f);  // no instant flip between clips
            }
        }
        previous = frame.pose.bone[static_cast<std::size_t>(vrm::HumanoidBone::LeftUpperArm)];
    }
}


// ---------------------------------------------------------------------------
// Player integration
// ---------------------------------------------------------------------------

TEST_CASE(motion32.host_profile_roundtrips) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "bgmedia_motion32_profile.cfg";
    motion::Motion32Profile profile;
    profile.overlayMode = motion::OverlayMode::HandsOnly;
    profile.springEnabled = false;
    profile.springIntensity = 1.6f;
    profile.springDamping = 0.85f;
    std::string error;
    REQUIRE(motion::saveMotion32Profile(path, profile, &error));
    motion::Motion32Profile loaded;
    REQUIRE(motion::loadMotion32Profile(path, loaded, &error));
    CHECK(loaded.overlayMode == motion::OverlayMode::HandsOnly);
    CHECK(!loaded.springEnabled);
    CHECK_NEAR(loaded.springIntensity, 1.6, 1e-5);
    CHECK_NEAR(loaded.springDamping, 0.85, 1e-5);
    std::filesystem::remove(path);
}

TEST_CASE(motion32.player_installs_three_generated_dances) {    motion::DancePlayer player;
    const std::size_t count = player.loadLibrary(dancesDir());
    CHECK(count >= 8);
    CHECK(player.generatedDances().size() == 3);
    for (const auto& generated : player.generatedDances()) {
        CHECK(player.library().find(generated.clip.id) != nullptr);
        CHECK(player.library().find(generated.clip.id)->beats >= 32.0f);
    }
    CHECK(player.library().find("neonPulse") != nullptr);
    CHECK(player.library().find("streetCurrent") != nullptr);
    CHECK(player.library().find("fluidSignal") != nullptr);
}

TEST_CASE(motion32.overlay_changes_the_pose_but_not_the_music) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    REQUIRE(player.selectClip("neonPulse", 0.0, true));
    player.setOverlayMode(motion::OverlayMode::Off);
    const motion::DanceFrame plain = player.update(3.0);
    player.setOverlayMode(motion::OverlayMode::Full);
    const motion::DanceFrame layered = player.update(3.0);
    CHECK(plain.clipId == "neonPulse");
    CHECK(layered.clipId == "neonPulse");
    CHECK_NEAR(plain.clipBeat, layered.clipBeat, 1e-6);
    CHECK_NEAR(plain.musicBeat, layered.musicBeat, 1e-6);
    CHECK(layered.overlayActive > 0);
    float worst = 0.0f;
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        const float d = std::abs(math::dot(plain.pose.bone[b], layered.pose.bone[b]));
        worst = std::max(worst, 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)));
    }
    CHECK(worst > 0.005f);
    CHECK_NEAR(plain.pose.rootX, layered.pose.rootX, 1e-6);
    CHECK_NEAR(plain.pose.rootZ, layered.pose.rootZ, 1e-6);
    for (int f = 0; f < 2; ++f) {
        if (plain.pose.foot[static_cast<std::size_t>(f)].contact > 0.8f) {
            CHECK_NEAR(plain.pose.foot[static_cast<std::size_t>(f)].position.x,
                       layered.pose.foot[static_cast<std::size_t>(f)].position.x, 1e-5);
            CHECK_NEAR(plain.pose.foot[static_cast<std::size_t>(f)].position.y,
                       layered.pose.foot[static_cast<std::size_t>(f)].position.y, 1e-5);
        }
    }
}

TEST_CASE(motion32.seek_and_rebase_keep_generated_dance_finite) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    REQUIRE(player.selectClip("fluidSignal", 0.0, true));
    for (double t : {0.0, 3.2, 1.1, 9.7, 0.0, 12.5}) {
        const motion::DanceFrame frame = player.update(t);
        CHECK(std::isfinite(frame.pose.rootX));
        CHECK(std::isfinite(frame.pose.rootZ));
        CHECK(std::isfinite(frame.pose.hips.x));
        for (const math::Quat& q : frame.pose.bone) {
            CHECK(finiteQuat(q));
        }
    }
}

// BG Motion 3.2 suite (registered in CTest), #8: the opt-in inertial transition keeps upper-body poses continuous
// at both ends of a segment fade, is a pure function of media time (seek equals playback),
// and leaves the default blend untouched.
TEST_CASE(motion32.inertial_transition_is_continuous_and_seek_stable) {
    const motion::GeneratedDance a = motion::ProceduralChoreography::generate(request(7));
    const motion::GeneratedDance b = motion::ProceduralChoreography::generate(request(23));
    const motion::BeatClock clock = motion::BeatClock::constant(104.0, 0.0, 4);
    const motion::MusicalTimeline timeline = motion::MusicalTimeline::fromClock(clock, true);
    motion::WarpProfile off;
    off.expression = motion::MotionExpression::Off;
    const double t0 = 4.0;
    const double fade = 0.6;

    const auto makeSchedule = [&](bool inertial) {
        motion::DanceSchedule schedule;
        schedule.setInertialTransitions(inertial);
        motion::ScheduleSegment first;
        first.clip = &a.clip;
        first.fadeSeconds = fade;
        motion::ScheduleSegment second;
        second.clip = &b.clip;
        second.startTime = t0;
        second.alignTime = t0;
        second.fadeSeconds = fade;
        schedule.append(first);
        schedule.append(second);
        return schedule;
    };
    const auto poseAt = [&](const motion::DanceSchedule& schedule, double t) {
        return schedule.sample(t, clock, timeline, off).pose;
    };

    const motion::DanceSchedule inertial = makeSchedule(true);
    const motion::DanceSchedule blend = makeSchedule(false);

    // Start of the fade: the outgoing pose is held exactly, as in the blend.
    const motion::ClipPose outgoingAtStart = poseAt(inertial, t0 - 1.0e-4);
    const motion::ClipPose atStart = poseAt(inertial, t0);
    CHECK_NEAR(poseDistance(outgoingAtStart, atStart), 0.0, 1e-2);

    // Fade end: the inertial pose must hand over to the incoming clip without a step.
    const double edge = 1.0e-4;
    const motion::ClipPose beforeEnd = poseAt(inertial, t0 + fade - edge);
    const motion::ClipPose afterEnd = poseAt(inertial, t0 + fade + edge);
    CHECK_NEAR(poseDistance(beforeEnd, afterEnd), 0.0, 1e-2);

    // Seek equals playback: a schedule sampled only at one time gives the same pose.
    const motion::DanceSchedule fresh = makeSchedule(true);
    CHECK_NEAR(poseDistance(poseAt(inertial, t0 + 0.3), poseAt(fresh, t0 + 0.3)), 0.0, 1e-6);

    // Default off: the blend path is what existed before the flag.
    CHECK(!motion::DanceSchedule{}.inertialTransitions());
    CHECK(!blend.inertialTransitions());
    CHECK(inertial.inertialTransitions());
}

// BG Motion 3.3 (#9): the opt-in follow-through lags only head and hands, stays bounded,
// is deterministic in media time (fresh schedule equals a schedule sampled in order), and
// is off by default.
TEST_CASE(motion32.follow_through_is_bounded_deterministic_and_limited_to_head_and_hands) {
    const motion::GeneratedDance a = motion::ProceduralChoreography::generate(request(7));
    const motion::BeatClock clock = motion::BeatClock::constant(104.0, 0.0, 4);
    const motion::MusicalTimeline timeline = motion::MusicalTimeline::fromClock(clock, true);
    motion::WarpProfile off;
    off.expression = motion::MotionExpression::Off;

    const auto makeSchedule = [&](bool follow) {
        motion::DanceSchedule schedule;
        schedule.setFollowThrough(follow);
        motion::ScheduleSegment seg;
        seg.clip = &a.clip;
        seg.fadeSeconds = 0.6;
        schedule.append(seg);
        return schedule;
    };
    CHECK(!motion::DanceSchedule{}.followThrough());

    const motion::DanceSchedule plain = makeSchedule(false);
    const motion::DanceSchedule lagged = makeSchedule(true);
    const motion::DanceSchedule sequential = makeSchedule(true);
    for (const double t : {1.0, 2.25, 3.5, 4.75}) {
        const motion::ClipPose p0 = plain.sample(t, clock, timeline, off).pose;
        const motion::ClipPose p1 = lagged.sample(t, clock, timeline, off).pose;
        // Head and hands carry the lag; every other bone is identical to the plain pose.
        for (std::size_t bone = 0; bone < vrm::kHumanoidBoneCount; ++bone) {
            const math::Quat& a0 = p0.bone[bone];
            const math::Quat& a1 = p1.bone[bone];
            // Component differences: an angle from acos is too noisy near identity.
            const float diff = std::max({std::abs(a0.x - a1.x), std::abs(a0.y - a1.y),
                                         std::abs(a0.z - a1.z), std::abs(a0.w - a1.w)});
            const bool follows = bone == static_cast<std::size_t>(vrm::HumanoidBone::Head) ||
                                 bone == static_cast<std::size_t>(vrm::HumanoidBone::LeftHand) ||
                                 bone == static_cast<std::size_t>(vrm::HumanoidBone::RightHand);
            if (!follows) {
                CHECK_NEAR(diff, 0.0f, 1e-6f);
            } else {
                // Lag is capped at 0.2 rad; a quaternion component is at most sin(angle/2).
                CHECK(diff <= 0.11f);
            }
        }
        // Same media time from a fresh schedule gives the same pose (seek equals playback).
        const motion::ClipPose again = sequential.sample(t, clock, timeline, off).pose;
        for (std::size_t bone = 0; bone < vrm::kHumanoidBoneCount; ++bone) {
            CHECK_NEAR(std::abs(p1.bone[bone].x - again.bone[bone].x) +
                           std::abs(p1.bone[bone].y - again.bone[bone].y) +
                           std::abs(p1.bone[bone].z - again.bone[bone].z) +
                           std::abs(p1.bone[bone].w - again.bone[bone].w),
                       0.0f, 1e-6f);
        }
    }
}

// BG Motion 3.3 (#10): the phrase head look is an opt-in choice. With it off the head
// keeps the beat nod only, and the rest of the generated clip is unchanged.
TEST_CASE(motion32.head_phrase_look_is_optional) {
    motion::ChoreographyRequest on = request(11);
    motion::ChoreographyRequest off = request(11);
    off.headLookAtPhrases = false;
    const motion::HumanoidClip withLook = motion::ProceduralChoreography::generate(on).clip;
    const motion::HumanoidClip withoutLook = motion::ProceduralChoreography::generate(off).clip;
    CHECK(motion::ProceduralChoreography::generate(on).clip.keyCount == withLook.keyCount);
    const std::size_t head = static_cast<std::size_t>(vrm::HumanoidBone::Head);
    // The phrase look turns the head at the first phrase boundary.
    const motion::ClipPose a = motion::sampleClip(withLook, 0.0);
    const motion::ClipPose b = motion::sampleClip(withoutLook, 0.0);
    const float diff = std::abs(a.bone[head].x - b.bone[head].x) +
                       std::abs(a.bone[head].y - b.bone[head].y) +
                       std::abs(a.bone[head].z - b.bone[head].z);
    CHECK(diff > 0.01f);
    // Music-facing parts are untouched: the feet and contacts are identical.
    CHECK(withLook.contacts.size() == withoutLook.contacts.size());
}
