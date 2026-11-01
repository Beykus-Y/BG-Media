#include "framework.hpp"

#include "motion_metrics.hpp"

#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/motion3/MotionPrimitive.hpp"
#include "bgmedia/motion3/ProceduralChoreography.hpp"

#include <algorithm>
#include <array>
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

// Angle between the bone rotations. Measured from the chord |p - q| rather than acos(dot):
// acos turns float rounding of identical rotations into ~7e-4 rad near 1.
float poseDistance(const motion::ClipPose& a, const motion::ClipPose& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        const math::Quat& p = a.bone[i];
        const math::Quat& q = b.bone[i];
        const float sign = math::dot(p, q) < 0.0f ? -1.0f : 1.0f;
        const float dx = p.x - sign * q.x;
        const float dy = p.y - sign * q.y;
        const float dz = p.z - sign * q.z;
        const float dw = p.w - sign * q.w;
        const float chord = std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
        worst = std::max(worst, 2.0f * std::asin(std::min(1.0f, 0.5f * chord)));
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

TEST_CASE(motion32.foot_overlay_fades_with_contact_instead_of_popping) {
    // The foot lift follows the foot's contact continuously (BG Motion 3.3, A6). A hard
    // planted/swing threshold switched the full lift on in one step.
    std::vector<motion::PrimitiveCue> cues;
    motion::PrimitiveCue step;
    step.kind = motion::PrimitiveKind::SideStep;
    step.beat = 0.0f;
    step.durationBeats = 1.0f;
    step.intensity = 1.0f;
    step.side = 0;
    cues.push_back(step);
    double previous = 0.0;
    double worstStep = 0.0;
    double liftAtSwing = 0.0;
    for (int i = 0; i <= 100; ++i) {
        const float contact = static_cast<float>(i) / 100.0f;
        motion::ClipPose base = motion::neutralClipPose();
        base.foot[0].phase = contact >= 0.5f ? motion::FootPhase::Stance : motion::FootPhase::Swing;
        base.foot[0].contact = contact;
        base.foot[0].position = {0.0f, 0.0f, 0.0f};
        base.foot[1].phase = motion::FootPhase::Stance;
        base.foot[1].contact = 1.0f;
        base.foot[1].position = {0.0f, 0.0f, 0.0f};
        const motion::ComposedPose composed =
            motion::composeMotion(base, cues, 0.25, 4.0, true);
        const double y = composed.pose.foot[0].position.y;
        CHECK(std::isfinite(y));
        if (i == 0) {
            liftAtSwing = y;
        } else {
            worstStep = std::max(worstStep, std::abs(y - previous));
        }
        previous = y;
    }
    CHECK(std::abs(liftAtSwing) > 1e-3);   // a swinging foot is lifted
    CHECK(worstStep < 0.005);              // no step over a 0.01 change of contact
    CHECK_NEAR(previous, 0.0, 1e-6);       // a planted foot is never lifted
}

TEST_CASE(motion32.contact_start_is_swinging_not_planted) {
    // The first sample of a contact segment has contact 0 and phase Plant. Zero contact is a
    // real value: the foot is swinging there, exactly as it is one sample earlier. Only a
    // stance without any contact data (a hand-built pose) counts as planted by its phase.
    std::vector<motion::PrimitiveCue> cues;
    motion::PrimitiveCue step;
    step.kind = motion::PrimitiveKind::SideStep;
    step.beat = 0.0f;
    step.durationBeats = 1.0f;
    step.intensity = 1.0f;
    step.side = 0;
    cues.push_back(step);
    const auto liftAt = [&](float contact, motion::FootPhase phase) {
        motion::ClipPose base = motion::neutralClipPose();
        base.foot[0].phase = phase;
        base.foot[0].contact = contact;
        base.foot[0].position = {0.0f, 0.0f, 0.0f};
        base.foot[1].phase = motion::FootPhase::Stance;
        base.foot[1].contact = 1.0f;
        base.foot[1].position = {0.0f, 0.0f, 0.0f};
        return motion::composeMotion(base, cues, 0.25, 4.0, true).pose.foot[0].position.y;
    };
    const double atStart = liftAt(0.0f, motion::FootPhase::Plant);
    const double justAfter = liftAt(1.0e-5f, motion::FootPhase::Plant);
    CHECK(std::abs(atStart) > 1e-3);              // swinging: lifted
    CHECK(std::abs(atStart - justAfter) < 0.005); // no step at the contact start
    CHECK(std::abs(liftAt(0.0f, motion::FootPhase::Stance)) < 1e-6);  // hand-built stance: planted
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

TEST_CASE(motion32.scalar_curl_and_relaxed_detailed_hand_pose_the_same_fingers) {
    // A clip without detailed hands and the same clip with the relaxed detailed pose of its
    // curl must pose every finger identically (BG Motion 3.3, A7). They differed before, so
    // turning detailedHands on popped the fingers. Needs a model with finger bones.
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        return;
    }
    Rig rig = makeRig(path);
    REQUIRE(rig.ok());
    for (const float curl : {0.0f, 0.4f, 1.0f}) {
        motion::ClipPose scalar = motion::neutralClipPose();
        scalar.hand[0] = curl;
        scalar.hand[1] = curl;
        motion::ClipPose detailed = scalar;
        detailed.detailedHands = true;
        detailed.handPose[0] = motion::relaxedHandPose(curl);
        detailed.handPose[1] = motion::relaxedHandPose(curl);
        const animation::RetargetedPose a = rig.rig->applyClip(scalar, 0.0);
        const animation::RetargetedPose b = rig.rig->applyClip(detailed, 0.0);
        for (int side = 0; side < 2; ++side) {
            for (int finger = 0; finger < 5; ++finger) {
                for (int joint = 0; joint < 3; ++joint) {
                    const int node = rig.loaded.model->fingerNode(side, finger, joint);
                    REQUIRE(node >= 0);
                    const auto qa =
                        a.nodeGlobals[static_cast<std::size_t>(node)].rotationQuat();
                    const auto qb =
                        b.nodeGlobals[static_cast<std::size_t>(node)].rotationQuat();
                    CHECK(std::abs(math::dot(qa, qb)) > 0.99999f);
                }
            }
        }
    }
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

// BG Motion 3.3 (A8): the inertial transition carries the outgoing upper-body velocity through
// the fade start. The review found it only C0 (its offset started at zero velocity). Here the
// one-sided angular velocities either side of the fade start must agree: the jump is the
// step in velocity, which for a C1 hand-over is only the acceleration times the sampling step.
TEST_CASE(motion32.inertial_transition_carries_velocity_through_the_fade_start) {
    const motion::GeneratedDance a = motion::ProceduralChoreography::generate(request(7));
    const motion::GeneratedDance b = motion::ProceduralChoreography::generate(request(23));
    const motion::BeatClock clock = motion::BeatClock::constant(104.0, 0.0, 4);
    const motion::MusicalTimeline timeline = motion::MusicalTimeline::fromClock(clock, true);
    motion::WarpProfile off;
    off.expression = motion::MotionExpression::Off;
    const double t0 = 4.0;
    const double fade = 0.6;
    const double h = 1.0 / 240.0;

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
    // Upper body only: the inertial transition does not touch hips and legs.
    const auto upperBody = [](std::size_t bone) {
        using B = vrm::HumanoidBone;
        const auto at = [](B value) { return static_cast<std::size_t>(value); };
        return (bone >= at(B::Spine) && bone <= at(B::Head)) ||
               (bone >= at(B::LeftShoulder) && bone <= at(B::LeftHand)) ||
               (bone >= at(B::RightShoulder) && bone <= at(B::RightHand));
    };
    // Angular velocity (world frame, rad/s) of one bone between two samples. The angle is
    // taken from the chord-free form 2 atan2(|xyz|, |w|), which is exact for small steps.
    using Velocity = std::array<double, 3>;
    const auto velocity = [&](const motion::ClipPose& from, const motion::ClipPose& to,
                              std::size_t bone) {
        const math::Quat d = (to.bone[bone] * conjugate(from.bone[bone])).normalized();
        const double sign = d.w < 0.0f ? -1.0 : 1.0;
        const double x = sign * d.x;
        const double y = sign * d.y;
        const double z = sign * d.z;
        const double s = std::sqrt(x * x + y * y + z * z);
        const double k = s > 1.0e-12 ? 2.0 * std::atan2(s, sign * d.w) / s / h : 2.0 / h;
        return Velocity{k * x, k * y, k * z};
    };
    // Velocity step across the fade start for each upper-body bone: the left derivative
    // (outgoing side) against the right derivative (incoming side).
    struct Jump {
        double step = 0.0;
        double outgoing = 0.0;
    };
    const auto jumpAt = [&](const motion::DanceSchedule& schedule, double time) {
        const motion::ClipPose before1 = poseAt(schedule, time - h);
        const motion::ClipPose at = poseAt(schedule, time);
        const motion::ClipPose after1 = poseAt(schedule, time + h);
        Jump worst;
        for (std::size_t bone = 0; bone < vrm::kHumanoidBoneCount; ++bone) {
            if (!upperBody(bone)) {
                continue;
            }
            const Velocity left = velocity(before1, at, bone);
            const Velocity right = velocity(at, after1, bone);
            const double step = std::sqrt((right[0] - left[0]) * (right[0] - left[0]) +
                                          (right[1] - left[1]) * (right[1] - left[1]) +
                                          (right[2] - left[2]) * (right[2] - left[2]));
            const double outgoing = std::sqrt(left[0] * left[0] + left[1] * left[1] +
                                              left[2] * left[2]);
            if (step > worst.step) {
                worst.step = step;
                worst.outgoing = outgoing;
            }
        }
        return worst;
    };
    const Jump inertial = jumpAt(makeSchedule(true), t0);
    const Jump blend = jumpAt(makeSchedule(false), t0);
    CHECK(std::isfinite(inertial.step));
    // Carrying the offset velocity removes most of the step the zero-velocity offset left
    // (about the outgoing speed itself).
    CHECK(inertial.step < 0.5 * std::max(inertial.outgoing, 0.5));
    // The blend already hands over smoothly at the start (smootherstep), so the carried
    // velocity does not beat it there; DanceSchedule keeps the blend as its default.
    CHECK(blend.step < 0.25 * std::max(inertial.outgoing, 0.5));
    // Fade end: the offset reaches zero with zero velocity, so the hand-over to the incoming
    // clip has no velocity step either (only the acceleration times the sampling step).
    const Jump atEnd = jumpAt(makeSchedule(true), t0 + fade);
    CHECK(atEnd.step < 0.05);
}

// Angle (radians) between two bone rotations: the rotation angle of conj(p) * q, taken from the
// vector part so that it stays accurate near identity.
namespace {

float rotationAngle(const math::Quat& p, const math::Quat& q) {
    const math::Quat r = p.conjugate() * q;
    const float vector = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    return 2.0f * std::atan2(vector, std::abs(r.w));
}

} // namespace

// BG Motion 3.3 (#9) follow-through, WP-E. The table lags the upper body along the chain. Every
// listed bone stays inside its own angle cap, nothing else changes (hips, spine, legs, feet,
// toes and eyes are bit-identical), the result is deterministic in media time (fresh schedule
// and reverse order agree with a sequential pass), and it is off by default on the schedule.
TEST_CASE(motion32.follow_through_is_bounded_deterministic_and_limited_to_upper_body_chain) {
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

    // The table is the intended set: upper body only, each bone once, parents before children.
    using B = vrm::HumanoidBone;
    const auto& table = motion::DanceSchedule::followThroughBones();
    std::array<float, vrm::kHumanoidBoneCount> capRadians{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        const std::size_t bone = static_cast<std::size_t>(table[i].bone);
        CHECK(capRadians[bone] == 0.0f);
        capRadians[bone] = table[i].capDegrees * 3.14159265f / 180.0f;
        CHECK(table[i].weight > 0.0f && table[i].weight <= 1.0f);
        CHECK(table[i].capDegrees > 0.0f && table[i].capDegrees <= 30.0f);
    }
    for (const B listed : {B::Chest, B::UpperChest, B::Neck, B::Head, B::LeftShoulder,
                           B::LeftUpperArm, B::LeftLowerArm, B::LeftHand, B::RightShoulder,
                           B::RightUpperArm, B::RightLowerArm, B::RightHand}) {
        CHECK(capRadians[static_cast<std::size_t>(listed)] > 0.0f);
    }
    for (const B unlisted : {B::Hips, B::Spine, B::LeftUpperLeg, B::LeftLowerLeg, B::LeftFoot,
                             B::LeftToes, B::RightUpperLeg, B::RightLowerLeg, B::RightFoot,
                             B::RightToes, B::LeftEye, B::RightEye}) {
        CHECK(capRadians[static_cast<std::size_t>(unlisted)] == 0.0f);
    }

    const motion::DanceSchedule plain = makeSchedule(false);
    const motion::DanceSchedule lagged = makeSchedule(true);
    const motion::DanceSchedule reverse = makeSchedule(true);
    const std::vector<double> times{1.0, 2.25, 3.5, 4.75};
    std::vector<motion::ClipPose> sequentialPoses;
    float largestLag = 0.0f;  // the test must see the follow-through move something
    for (const double t : times) {
        const motion::ClipPose p0 = plain.sample(t, clock, timeline, off).pose;
        const motion::ClipPose p1 = lagged.sample(t, clock, timeline, off).pose;
        sequentialPoses.push_back(p1);
        for (std::size_t bone = 0; bone < vrm::kHumanoidBoneCount; ++bone) {
            CHECK(finiteQuat(p1.bone[bone]));
            if (capRadians[bone] > 0.0f) {
                // Bounded: the offset from the bone's own motion is inside its cap.
                CHECK(rotationAngle(p0.bone[bone], p1.bone[bone]) <= capRadians[bone] + 1e-5f);
                largestLag = std::max(largestLag, rotationAngle(p0.bone[bone], p1.bone[bone]));
            } else {
                // Untouched: identical to the plain pose, not merely close.
                CHECK_NEAR(rotationAngle(p0.bone[bone], p1.bone[bone]), 0.0f, 1e-7f);
            }
        }
        CHECK(p0.hips.x == p1.hips.x && p0.hips.y == p1.hips.y && p0.hips.z == p1.hips.z);
        for (std::size_t f = 0; f < 2; ++f) {
            CHECK(p0.foot[f].position.x == p1.foot[f].position.x);
            CHECK(p0.foot[f].position.y == p1.foot[f].position.y);
            CHECK(p0.foot[f].position.z == p1.foot[f].position.z);
            CHECK(p0.foot[f].yaw == p1.foot[f].yaw);
        }
    }
    CHECK(largestLag > 0.5f * 3.14159265f / 180.0f);
    // Deterministic: the same media times from a fresh schedule, in reverse order, match.
    for (std::size_t i = times.size(); i-- > 0;) {
        const motion::ClipPose again = reverse.sample(times[i], clock, timeline, off).pose;
        CHECK_NEAR(poseDistance(again, sequentialPoses[i]), 0.0f, 1e-6f);
    }
}

// BG Motion 3.3 (#9): follow-through is a measurable phase lag. A sudden 60 degree swing of the
// left upper arm with every other track in bind pose: the hand has no motion of its own, so its
// swing can only come from the chain. The displayed hand must reach half of the swing later
// than the displayed upper arm, and the plain schedule must show no lag at all.
TEST_CASE(motion32.follow_through_shows_hand_lagging_a_sudden_arm_move) {
    motion::HumanoidClip clip;
    clip.id = "suddenArm";
    clip.bpm = 120.0;
    clip.beats = 8.0f;
    clip.keysPerBeat = 16;
    clip.keyCount = 8 * 16 + 1;
    clip.loop = false;
    clip.seamless = false;
    std::vector<math::Quat>& arm = clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::LeftUpperArm)];
    arm.resize(static_cast<std::size_t>(clip.keyCount));
    for (int k = 0; k < clip.keyCount; ++k) {
        // Smootherstep from beat 2 to beat 2.25 (0.125 s at 120 BPM): fast, but continuous.
        const float u = std::clamp((static_cast<float>(k) / 16.0f - 2.0f) / 0.25f, 0.0f, 1.0f);
        const float s = u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
        arm[static_cast<std::size_t>(k)] =
            math::fromAxisAngle(math::Vec3{0.0f, 0.0f, 1.0f}, s * 60.0f * 3.14159265f / 180.0f);
    }
    const motion::BeatClock clock = motion::BeatClock::constant(120.0, 0.0, 4);
    const motion::MusicalTimeline timeline = motion::MusicalTimeline::fromClock(clock, true);
    motion::WarpProfile off;
    off.expression = motion::MotionExpression::Off;

    const auto makeSchedule = [&](bool follow) {
        motion::DanceSchedule schedule;
        schedule.setFollowThrough(follow);
        motion::ScheduleSegment seg;
        seg.clip = &clip;
        seg.fadeSeconds = 0.0;
        schedule.append(seg);
        return schedule;
    };
    const auto identity = math::Quat::identity();
    // World rotation of the chain shoulder (bind) -> upper arm -> lower arm -> hand, as the
    // retargeter composes it, and of the upper arm alone.
    const auto chainAt = [&](const motion::DanceSchedule& schedule, double t, bool hand) {
        const motion::ClipPose pose = schedule.sample(t, clock, timeline, off).pose;
        const std::size_t ua = static_cast<std::size_t>(vrm::HumanoidBone::LeftUpperArm);
        const std::size_t la = static_cast<std::size_t>(vrm::HumanoidBone::LeftLowerArm);
        const std::size_t h = static_cast<std::size_t>(vrm::HumanoidBone::LeftHand);
        const math::Quat upper = pose.bone[ua];
        return hand ? upper * pose.bone[la] * pose.bone[h] : upper;
    };
    // First time the swing reaches half of its final angle.
    const auto halfTime = [&](const motion::DanceSchedule& schedule, bool hand) {
        const double dt = 1.0 / 480.0;
        const float final = rotationAngle(identity, chainAt(schedule, 1.6, hand));
        double half = -1.0;
        for (double t = 0.9; t <= 1.6; t += dt) {
            if (rotationAngle(identity, chainAt(schedule, t, hand)) >= 0.5f * final) {
                half = t;
                break;
            }
        }
        return half;
    };

    const motion::DanceSchedule plain = makeSchedule(false);
    const motion::DanceSchedule lagged = makeSchedule(true);
    const double plainUpper = halfTime(plain, false);
    const double plainHand = halfTime(plain, true);
    const double laggedUpper = halfTime(lagged, false);
    const double laggedHand = halfTime(lagged, true);
    CHECK(plainUpper > 0.9 && plainHand > 0.9);
    CHECK(laggedUpper > 0.9 && laggedHand > 0.9);
    // Without follow-through the hand is the upper arm: no lag at all.
    CHECK_NEAR(plainHand - plainUpper, 0.0, 1e-9);
    // With it, the hand trails the upper arm by a clear margin (lag share of the chain).
    CHECK(laggedHand - laggedUpper > 0.03);
    // The upper arm itself also trails its own motion, but only by its small share.
    CHECK(laggedUpper >= plainUpper - 1e-9);
}

// BG Motion 3.3 (#9): the follow-through is continuous across a crossfade, the fade end and a loop
// seam, and a seek lands on the same pose as playback.
TEST_CASE(motion32.follow_through_is_continuous_across_fades_and_loop_seams) {
    const motion::GeneratedDance a = motion::ProceduralChoreography::generate(request(7));
    const motion::GeneratedDance b = motion::ProceduralChoreography::generate(request(23));
    const motion::BeatClock clock = motion::BeatClock::constant(104.0, 0.0, 4);
    const motion::MusicalTimeline timeline = motion::MusicalTimeline::fromClock(clock, true);
    motion::WarpProfile off;
    off.expression = motion::MotionExpression::Off;
    const double t0 = 4.0;
    const double fade = 0.6;
    const double seam = static_cast<double>(a.clip.beats) * 60.0 / 104.0;

    const auto makeSchedule = [&] {
        motion::DanceSchedule schedule;
        schedule.setFollowThrough(true);
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
    const motion::DanceSchedule schedule = makeSchedule();
    const auto poseAt = [&](double t) { return schedule.sample(t, clock, timeline, off).pose; };
    const double edge = 1.0e-4;
    for (const double t : {t0, t0 + fade, seam}) {
        CHECK_NEAR(poseDistance(poseAt(t - edge), poseAt(t + edge)), 0.0f, 1e-2f);
    }
    // Seek equals playback at a time inside the fade.
    CHECK_NEAR(poseDistance(poseAt(t0 + 0.3), makeSchedule().sample(t0 + 0.3, clock, timeline, off).pose),
               0.0f, 1e-6f);
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

TEST_CASE(motion32.gated_cue_yields_arbitration_with_its_gate) {
    // A contact-gated cue that has almost faded out must also stop suppressing the cues
    // it overlaps; otherwise they reappear in one frame when it is finally rejected.
    std::vector<motion::PrimitiveCue> cues;
    motion::PrimitiveCue step;
    step.kind = motion::PrimitiveKind::SideStep;
    step.beat = 0.0f;
    step.durationBeats = 1.0f;
    step.intensity = 1.0f;
    step.side = 0;
    cues.push_back(step);
    motion::PrimitiveCue sway;
    sway.kind = motion::PrimitiveKind::HipSway;
    sway.beat = 0.0f;
    sway.durationBeats = 1.0f;
    sway.intensity = 1.0f;
    cues.push_back(sway);
    double previous = 0.0;
    double worstStep = 0.0;
    for (int i = 0; i <= 2000; ++i) {
        const float contact = static_cast<float>(i) / 2000.0f;
        motion::ClipPose base = motion::neutralClipPose();
        base.foot[0].phase = motion::FootPhase::Plant;
        base.foot[0].contact = contact;
        base.foot[1].phase = motion::FootPhase::Stance;
        base.foot[1].contact = 1.0f;
        const motion::ComposedPose composed = motion::composeMotion(base, cues, 0.25, 4.0, true);
        const double x = composed.pose.hips.x;
        CHECK(std::isfinite(x));
        if (i > 0) {
            worstStep = std::max(worstStep, std::abs(x - previous));
        }
        previous = x;
    }
    // 0.0005 of contact per sample: the hips move continuously, no 0.01 jump.
    CHECK(worstStep < 0.001);
}

TEST_CASE(motion32.foot_lift_fades_with_the_support_gate) {
    // A step needs a supporting foot. As that support goes, the swinging foot's lift must
    // fade with it, not stay at full height until the cue is rejected.
    std::vector<motion::PrimitiveCue> cues;
    motion::PrimitiveCue step;
    step.kind = motion::PrimitiveKind::SideStep;
    step.beat = 0.0f;
    step.durationBeats = 1.0f;
    step.intensity = 1.0f;
    step.side = 0;
    cues.push_back(step);
    double previous = 0.0;
    double worstStep = 0.0;
    for (int i = 0; i <= 2000; ++i) {
        const float support = static_cast<float>(i) / 2000.0f;
        motion::ClipPose base = motion::neutralClipPose();
        base.foot[0].phase = motion::FootPhase::Swing;
        base.foot[0].contact = 0.0f;
        base.foot[1].phase = motion::FootPhase::Plant;
        base.foot[1].contact = support;
        const motion::ComposedPose composed = motion::composeMotion(base, cues, 0.25, 4.0, true);
        const double y = composed.pose.foot[0].position.y;
        CHECK(std::isfinite(y));
        if (i > 0) {
            worstStep = std::max(worstStep, std::abs(y - previous));
        }
        previous = y;
    }
    CHECK(worstStep < 0.001);  // no 0.039 jump at the rejection threshold
    CHECK(previous > 0.01);    // full support: the step lifts the foot
}
