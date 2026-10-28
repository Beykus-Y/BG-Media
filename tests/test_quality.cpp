// BG Motion 3.2 quality pass regression tests: motion composition, masks,
// transitions. Spatial retarget tests live in test_retarget.cpp.
#include "framework.hpp"

#include "bgmedia/motion3/ClipFootwork.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/motion3/MotionPrimitive.hpp"
#include "FootworkTrack.hpp"
#include "motion_metrics.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace bgmedia;
using motion::BodyMask;
using motion::PrimitiveKind;

bool isIdentity(const math::Quat& q) { return std::abs(q.w) > 0.999999f; }

bool handTouched(const motion::HandPose& h) {
    // Untouched primitive hands carry the relaxed resting hand (curl 0.30).
    const motion::HandPose rest = motion::relaxedHandPose(0.30f);
    const auto differs = [](float x, float y) { return std::abs(x - y) > 1.0e-6f; };
    if (differs(h.thumbOpposition, rest.thumbOpposition)) return true;
    for (int i = 0; i < 3; ++i) {
        if (differs(h.wrist[static_cast<std::size_t>(i)], rest.wrist[static_cast<std::size_t>(i)])) {
            return true;
        }
    }
    for (int f = 0; f < 5; ++f) {
        if (differs(h.spread[static_cast<std::size_t>(f)], rest.spread[static_cast<std::size_t>(f)])) {
            return true;
        }
        for (int j = 0; j < 3; ++j) {
            const auto fi = static_cast<std::size_t>(f);
            const auto ji = static_cast<std::size_t>(j);
            if (differs(h.curl[fi][ji], rest.curl[fi][ji])) {
                return true;
            }
        }
    }
    return false;
}

// Everything a primitive modifies, as the body-mask bits needed to let it
// through the compositor.
BodyMask touchedMask(const motion::PrimitivePose& pose) {
    BodyMask touched = motion::body::None;
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        if (!isIdentity(pose.bone[b])) {
            touched |= motion::maskBitForBone(static_cast<vrm::HumanoidBone>(b));
        }
    }
    if (std::abs(pose.hips.x) + std::abs(pose.hips.y) + std::abs(pose.hips.z) > 1.0e-7f) {
        touched |= motion::body::Pelvis;
    }
    if (pose.touchesFeet) {
        for (int f = 0; f < 2; ++f) {
            if (std::abs(pose.footLift[static_cast<std::size_t>(f)]) > 1.0e-7f ||
                std::abs(pose.toePitch[static_cast<std::size_t>(f)]) > 1.0e-7f) {
                touched |= f == 0 ? motion::body::LeftFoot : motion::body::RightFoot;
            }
        }
    }
    for (int sd = 0; sd < 2; ++sd) {
        if (handTouched(pose.hand[static_cast<std::size_t>(sd)])) {
            touched |= sd == 0 ? motion::body::LeftHand : motion::body::RightHand;
        }
    }
    return touched;
}

}  // namespace

TEST_CASE(quality.primitive_touched_channels_fit_the_effective_mask) {
    int offenders = 0;
    for (int k = 0; k < motion::kPrimitiveKindCount; ++k) {
        const auto kind = static_cast<PrimitiveKind>(k);
        motion::PrimitiveCue cue;
        cue.kind = kind;
        for (int side : {-1, 0, 1}) {
            cue.side = side;
            const BodyMask allowed = cue.sideMask();
            BodyMask touched = motion::body::None;
            for (int step = 0; step <= 20; ++step) {
                const float phase = static_cast<float>(step) / 20.0f;
                touched |= touchedMask(motion::samplePrimitive(kind, phase, 1.0f, side));
            }
            const BodyMask lost = touched & ~allowed;
            if (lost != 0u) {
                ++offenders;
                std::cout << "\n            " << motion::primitiveMetadata(kind).id
                          << " side=" << side << " touches 0x" << std::hex << lost
                          << std::dec << " outside its mask";
            }
        }
    }
    CHECK(offenders == 0);
}

namespace {

motion::PrimitiveCue makeCue(PrimitiveKind kind, float beat, float duration, float intensity,
                             int side = -1) {
    motion::PrimitiveCue cue;
    cue.kind = kind;
    cue.beat = beat;
    cue.durationBeats = duration;
    cue.intensity = intensity;
    cue.side = side;
    return cue;
}

float poseDelta(const motion::ClipPose& a, const motion::ClipPose& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        worst = std::max(worst, 1.0f - std::abs(math::dot(a.bone[i], b.bone[i])));
    }
    return worst;
}

}  // namespace

TEST_CASE(quality.cue_selection_ignores_input_order_and_priority_wins) {
    // A low-priority torso cue arrives before a stronger one that claims the
    // same region, plus more cues than the layer cap allows.
    std::vector<motion::PrimitiveCue> cues = {
        makeCue(PrimitiveKind::UpperBodyBounce, 0.0f, 2.0f, 1.0f),   // priority 32
        makeCue(PrimitiveKind::SideLean, 0.0f, 2.0f, 1.0f),          // 35
        makeCue(PrimitiveKind::ChestIsolation, 0.0f, 2.0f, 1.0f),    // 52
        makeCue(PrimitiveKind::HeadNod, 0.0f, 2.0f, 1.0f),           // 34
        makeCue(PrimitiveKind::ArmWave, 0.0f, 2.0f, 1.0f, 0),        // 42
        makeCue(PrimitiveKind::ShoulderAccent, 0.0f, 2.0f, 1.0f, 1), // 62
    };
    auto base = motion::neutralClipPose();
    base.foot[0].contact = 1.0f;
    base.foot[1].contact = 1.0f;
    motion::CompositionOptions options;
    const auto reference = motion::composeMotion(base, cues, 1.0, 8.0, true, options);
    CHECK(reference.diagnostics.acceptedCues > 0);
    // The high-priority chest isolation must have won over the bounce/lean it overlaps.
    CHECK(reference.diagnostics.boneLayers[static_cast<std::size_t>(vrm::HumanoidBone::Chest)] >= 1);

    std::vector<std::size_t> order(cues.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    int permutations = 0;
    do {
        std::vector<motion::PrimitiveCue> shuffled;
        for (std::size_t i : order) shuffled.push_back(cues[i]);
        const auto out = motion::composeMotion(base, shuffled, 1.0, 8.0, true, options);
        CHECK(poseDelta(out.pose, reference.pose) < 1.0e-7f);
        CHECK(out.diagnostics.acceptedCues == reference.diagnostics.acceptedCues);
        CHECK(out.diagnostics.rejectedByConflict == reference.diagnostics.rejectedByConflict);
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK(permutations == 720);
}

TEST_CASE(quality.low_priority_cue_never_blocks_a_higher_one) {
    auto base = motion::neutralClipPose();
    base.foot[0].contact = 1.0f;
    base.foot[1].contact = 1.0f;
    const auto bounce = makeCue(PrimitiveKind::UpperBodyBounce, 0.0f, 2.0f, 1.0f);
    const auto isolation = makeCue(PrimitiveKind::ChestIsolation, 0.0f, 2.0f, 1.0f);
    std::vector<motion::PrimitiveCue> lowFirst = {bounce, isolation};
    std::vector<motion::PrimitiveCue> highFirst = {isolation, bounce};
    const auto a = motion::composeMotion(base, lowFirst, 0.8, 8.0, true);
    const auto b = motion::composeMotion(base, highFirst, 0.8, 8.0, true);
    // Chest isolation (52) beats upper-body bounce (32): the bounce is rejected
    // either way and exactly one cue is active.
    CHECK(a.diagnostics.activeCues == 1);
    CHECK(b.diagnostics.activeCues == 1);
    CHECK(poseDelta(a.pose, b.pose) < 1.0e-7f);
}

TEST_CASE(quality.layer_cap_keeps_the_strongest_cues) {
    auto base = motion::neutralClipPose();
    // Four disjoint upper-body cues, cap 3: the weakest priority must be dropped.
    std::vector<motion::PrimitiveCue> cues = {
        makeCue(PrimitiveKind::HeadNod, 0.0f, 2.0f, 1.0f),            // 34 head
        makeCue(PrimitiveKind::ArmWave, 0.0f, 2.0f, 1.0f, 0),         // 42 left arm
        makeCue(PrimitiveKind::ArmReach, 0.0f, 2.0f, 1.0f, 1),        // 47 right arm
        makeCue(PrimitiveKind::ShoulderAccent, 0.0f, 2.0f, 1.0f, 0),  // 62 shoulders
    };
    motion::CompositionOptions options;
    options.maxUpperBodyLayers = 3;
    const auto out = motion::composeMotion(base, cues, 0.7, 8.0, true, options);
    CHECK(out.diagnostics.activeCues == 3);
    CHECK(out.diagnostics.boneLayers[static_cast<std::size_t>(vrm::HumanoidBone::Head)] == 0);
}

TEST_CASE(quality.disabled_hands_keep_the_arm_gesture) {
    auto base = motion::neutralClipPose();
    std::vector<motion::PrimitiveCue> cues = {
        makeCue(PrimitiveKind::ArmWave, 0.0f, 2.0f, 1.0f, 0)};
    motion::CompositionOptions options;
    options.allowHands = false;
    const auto out = motion::composeMotion(base, cues, 0.7, 8.0, true, options);
    CHECK(out.diagnostics.activeCues == 1);
    CHECK(out.diagnostics.boneLayers[static_cast<std::size_t>(vrm::HumanoidBone::LeftLowerArm)] >= 1);
    CHECK(out.diagnostics.boneLayers[static_cast<std::size_t>(vrm::HumanoidBone::LeftHand)] == 0);
}

TEST_CASE(quality.hand_cue_yields_hands_to_a_stronger_cue_but_keeps_the_arm) {
    auto base = motion::neutralClipPose();
    // PointGesture (68) owns arm+hand of the left side; ArmWave (42) on the same
    // side loses everything and is rejected; WristRoll (44) hands-only is rejected.
    std::vector<motion::PrimitiveCue> cues = {
        makeCue(PrimitiveKind::PointGesture, 0.0f, 2.0f, 1.0f, 0),
        makeCue(PrimitiveKind::ArmWave, 0.0f, 2.0f, 1.0f, 0),
        makeCue(PrimitiveKind::WristRoll, 0.0f, 2.0f, 1.0f, 0),
        makeCue(PrimitiveKind::ArmWave, 0.0f, 2.0f, 1.0f, 1),  // other side is free
    };
    const auto out = motion::composeMotion(base, cues, 0.8, 8.0, true);
    CHECK(out.diagnostics.activeCues == 2);
    CHECK(out.diagnostics.rejectedByConflict == 2);
}

TEST_CASE(quality.every_primitive_visibly_changes_the_composed_pose) {
    int dead = 0;
    for (int k = 0; k < motion::kPrimitiveKindCount; ++k) {
        const auto kind = static_cast<PrimitiveKind>(k);
        auto base = motion::neutralClipPose();
        base.foot[0].contact = 1.0f;
        base.foot[0].phase = motion::FootPhase::Stance;
        base.foot[1].contact = 1.0f;
        base.foot[1].phase = motion::FootPhase::Stance;
        const auto& metadata = motion::primitiveMetadata(kind);
        if (metadata.altersFeet) {
            // Footwork needs a swing foot to act on.
            base.foot[0].contact = 0.0f;
            base.foot[0].phase = motion::FootPhase::Swing;
        }
        float best = 0.0f;
        for (int side : {-1, 0, 1}) {
            for (double beat : {0.6, 1.0, 1.4}) {
                const auto cue = makeCue(kind, 0.0f, 2.0f, 1.0f, side);
                std::vector<motion::PrimitiveCue> cues = {cue};
                const auto out = motion::composeMotion(base, cues, beat, 8.0, true);
                if (out.diagnostics.acceptedCues == 0) continue;
                float change = poseDelta(out.pose, base);  // 1 - |dot|, bones
                change += 20.0f * std::abs(out.pose.hips.x - base.hips.x) +
                          20.0f * std::abs(out.pose.hips.y - base.hips.y);
                for (int f = 0; f < 2; ++f) {
                    const auto fi = static_cast<std::size_t>(f);
                    change += 20.0f * std::abs(out.pose.foot[fi].position.y -
                                               base.foot[fi].position.y) +
                              0.1f * std::abs(out.pose.foot[fi].toePitch - base.foot[fi].toePitch);
                    if (out.pose.detailedHands) {
                        const auto rest = motion::relaxedHandPose(base.hand[fi]);
                        for (int fg = 0; fg < 5; ++fg) {
                            change += 0.05f * std::abs(out.pose.handPose[fi].curl[static_cast<std::size_t>(fg)][1] -
                                                       rest.curl[static_cast<std::size_t>(fg)][1]);
                        }
                    }
                }
                best = std::max(best, change);
            }
        }
        if (best < 2.0e-4f) {
            ++dead;
            std::cout << "\n            " << metadata.id << " contributes " << best;
        }
    }
    CHECK(dead == 0);
}

namespace {

std::filesystem::path dancesDir() {
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances";
}

float maxBoneAngle(const motion::ClipPose& a, const motion::ClipPose& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        const float d = std::abs(math::dot(a.bone[i], b.bone[i]));
        worst = std::max(worst, 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)));
    }
    return worst;
}

float boneAngle(const math::Quat& a, const math::Quat& b) {
    const float d = std::abs(math::dot(a, b));
    return 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f));
}

}  // namespace

TEST_CASE(quality.crossfade_keeps_each_clips_overlays_until_the_blend_ends) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setOverlayMode(motion::OverlayMode::Full);
    REQUIRE(player.selectClip("neonPulse", 0.0, true));
    (void)player.update(0.3);
    REQUIRE(player.selectClip("streetCurrent", 0.8, false));

    // "Legacy" reference: compose the overlays of whichever clip is *shown*
    // after the crossfade, which flips from the outgoing to the incoming clip
    // in the middle of the blend. The new pipeline composes per segment first.
    motion::CompositionOptions options;
    options.maxUpperBodyLayers = 3;
    float newMax = 0.0f;
    float legacyMax = 0.0f;
    double worstAt = 0.0;
    float worstProgress = 0.0f;
    int worstBone = -1;
    bool sawTransition = false;
    motion::ClipPose prevNew;
    motion::ClipPose prevLegacy;
    motion::ClipPose prevPlain;
    float plainMax = 0.0f;
    float steadyMax = 0.0f;
    bool havePrev = false;
    float prevProgress = -1.0f;
    const double dt = 1.0 / 240.0;
    for (int i = 0; i <= 480; ++i) {
        const double t = 0.8 + i * dt;
        const motion::DanceFrame frame = player.update(t);
        if (frame.transitionProgress >= 0.0f) sawTransition = true;
        player.setOverlayMode(motion::OverlayMode::Off);
        const motion::DanceFrame plain = player.update(t);
        player.setOverlayMode(motion::OverlayMode::Full);
        const motion::HumanoidClip* shown = player.library().find(plain.clipId);
        motion::ClipPose legacy = plain.pose;
        if (shown) {
            legacy = motion::composeMotion(plain.pose, player.overlaysFor(plain.clipId),
                                           plain.clipBeat, shown->beats, shown->loop, options)
                         .pose;
        }
        if (havePrev && frame.transitionProgress >= 0.0f && prevProgress >= 0.0f) {
            const float stepAngle = maxBoneAngle(frame.pose, prevNew);
            if (stepAngle > newMax) {
                newMax = stepAngle;
                worstAt = t;
                worstProgress = frame.transitionProgress;
                for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
                    const float d = std::abs(math::dot(frame.pose.bone[b], prevNew.bone[b]));
                    if (2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)) >= stepAngle - 1e-6f) worstBone = static_cast<int>(b);
                }
            }
            legacyMax = std::max(legacyMax, maxBoneAngle(legacy, prevLegacy));
            plainMax = std::max(plainMax, maxBoneAngle(plain.pose, prevPlain));
        }
        if (havePrev && frame.transitionProgress < 0.0f && prevProgress < 0.0f) {
            steadyMax = std::max(steadyMax, maxBoneAngle(frame.pose, prevNew));
        }
        prevProgress = frame.transitionProgress;
        prevNew = frame.pose;
        prevLegacy = legacy;
        prevPlain = plain.pose;
        havePrev = true;
    }
    CHECK(sawTransition);
    std::cout << "\n            max per-step bone rotation over crossfade @240Hz: new="
              << newMax << " legacy=" << legacyMax << " baseOnly=" << plainMax << " steadyOverlay=" << steadyMax << " at t=" << worstAt
              << " progress=" << worstProgress << " bone=" << worstBone;
    CHECK(newMax <= 0.5f * legacyMax);
    // KNOWN LIMITATION (docs/BG_MOTION_3_2_QUALITY_PASS.md): a residual hand pop
    // remains around progress 0.8 (about 4x the steady-state overlay step); it
    // is printed above, not hidden by a loosened threshold.
}

TEST_CASE(quality.time_warp_carries_the_detailed_hand_pose) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setOverlayMode(motion::OverlayMode::Off);
    REQUIRE(player.selectClip("fluidSignal", 0.0, true));
    motion::WarpProfile profile;
    profile.expression = motion::MotionExpression::Expressive;
    player.setWarpProfile(profile);
    const motion::HumanoidClip* clip = player.library().find("fluidSignal");
    REQUIRE(clip != nullptr);
    int checked = 0;
    float worst = 0.0f;
    for (double t = 0.5; t < 10.0; t += 0.0517) {
        const motion::DanceFrame frame = player.update(t);
        if (!frame.warpActive || frame.transitionProgress >= 0.0f ||
            std::abs(frame.warpDeviationBeats) < 0.02) {
            continue;
        }
        const motion::ClipPose expected = motion::sampleClip(*clip, frame.clipBeat);
        if (!expected.detailedHands) continue;
        CHECK(frame.pose.detailedHands);
        ++checked;
        for (int sd = 0; sd < 2; ++sd) {
            for (int fg = 0; fg < 5; ++fg) {
                for (int jt = 0; jt < 3; ++jt) {
                    worst = std::max(
                        worst, std::abs(frame.pose.handPose[static_cast<std::size_t>(sd)]
                                            .curl[static_cast<std::size_t>(fg)][static_cast<std::size_t>(jt)] -
                                        expected.handPose[static_cast<std::size_t>(sd)]
                                            .curl[static_cast<std::size_t>(fg)][static_cast<std::size_t>(jt)]));
                }
            }
            for (int axis = 0; axis < 3; ++axis) {
                worst = std::max(
                    worst, std::abs(frame.pose.handPose[static_cast<std::size_t>(sd)].wrist[static_cast<std::size_t>(axis)] -
                                    expected.handPose[static_cast<std::size_t>(sd)].wrist[static_cast<std::size_t>(axis)]));
            }
        }
    }
    std::cout << "\n            warped frames checked=" << checked << " worst hand delta=" << worst;
    CHECK(checked > 5);
    CHECK(worst < 1.0e-4f);
}

namespace {

// Foot position as seen by the clip baker: stage-space x/z (cm) and yaw.
struct FootSample {
    float x, z, yaw, lift;
};

FootSample sampleFoot(const motion::detail::Footwork& foot, float t) {
    const auto s = foot.at(t);
    return {s.x, s.z, s.yaw, s.lift};
}

float footDistance(const FootSample& a, const FootSample& b) {
    return std::abs(a.x - b.x) + std::abs(a.z - b.z) + std::abs(a.yaw - b.yaw);
}

}  // namespace

TEST_CASE(quality.footwork_close_applies_the_loop_correction) {
    constexpr float kPeriod = 8.0f;
    // The authored pattern ends 3 cm away from where it started (a bug in a
    // pattern, or a rounding drift): the loop must still return home smoothly.
    for (const float lastMoveEnd : {5.0f, 6.5f, 7.4f, 7.97f}) {
        motion::detail::Footwork foot(kPeriod, 0);
        foot.setStart(0.0f, 0.0f, 0.0f);
        foot.step(lastMoveEnd - 1.0f, lastMoveEnd, 3.0f, 1.0f, 10.0f);
        foot.close();
        CHECK(foot.segmentsAreDisjoint());
        const FootSample start = sampleFoot(foot, 0.0f);
        // The last instant of the loop is the start pose: no jump at the seam.
        const FootSample end = sampleFoot(foot, kPeriod - 1.0e-4f);
        CHECK(footDistance(start, end) < 0.02f);
        // And the correction really happens: shortly before the end the foot is
        // still mid-way, not already home and not frozen at the old position.
        const FootSample before = sampleFoot(foot, kPeriod - 0.02f);
        CHECK(footDistance(before, start) < footDistance(sampleFoot(foot, lastMoveEnd + 1.0e-3f), start) + 1.0e-4f);
        // Velocity is bounded around the wrap (no one-frame slide).
        float maxStep = 0.0f;
        const float dt = 1.0f / 64.0f;
        for (float t = 0.0f; t + dt <= kPeriod; t += dt) {
            maxStep = std::max(maxStep, footDistance(sampleFoot(foot, t), sampleFoot(foot, t + dt)));
        }
        const float seamStep = footDistance(sampleFoot(foot, kPeriod - dt), sampleFoot(foot, 0.0f));
        CHECK(seamStep <= maxStep + 1.0e-4f);
    }
}

TEST_CASE(quality.footwork_close_is_a_no_op_for_a_closed_pattern) {
    motion::detail::Footwork foot(8.0f, 1);
    foot.setStart(1.0f, 2.0f, 5.0f);
    foot.step(2.0f, 3.0f, 4.0f, 2.0f, 5.0f);
    foot.step(5.0f, 6.0f, 1.0f, 2.0f, 5.0f);  // back home
    const std::size_t before = foot.segmentCount();
    foot.close();
    CHECK(foot.segmentsAreDisjoint());
    CHECK(foot.segmentCount() == before + 1);  // only the closing hold
    const FootSample end = sampleFoot(foot, 7.99f);
    CHECK_NEAR(end.x, 1.0, 1e-4);
    CHECK_NEAR(end.z, 2.0, 1e-4);
}

TEST_CASE(quality.footwork_arguments_keep_their_meaning) {
    // step(x, z, yaw) used to come back as z <- yaw and yaw <- z.
    motion::detail::Footwork stepper(8.0f, 0);
    stepper.setStart(0.0f, 0.0f, 0.0f);
    stepper.step(1.0f, 2.0f, 5.0f, 3.0f, 40.0f);
    const auto landed = stepper.at(3.0f);
    CHECK_NEAR(landed.x, 5.0, 1e-4);
    CHECK_NEAR(landed.z, 3.0, 1e-4);
    CHECK_NEAR(landed.yaw, 40.0, 1e-4);

    // A pivot turns the foot in place: yaw changes, position does not, and the
    // planted foot is not displaced when the pivot ends.
    motion::detail::Footwork pivoter(8.0f, 1);
    pivoter.setStart(2.0f, 1.0f, 0.0f);
    pivoter.pivot(1.0f, 2.0f, 22.0f);
    const auto mid = pivoter.at(1.5f);
    const auto after = pivoter.at(2.5f);
    CHECK_NEAR(mid.yaw, 11.0, 1e-3);
    CHECK_NEAR(after.yaw, 22.0, 1e-4);
    CHECK_NEAR(after.x, 2.0, 1e-4);
    CHECK_NEAR(after.z, 1.0, 1e-4);
    CHECK_NEAR(pivoter.at(1.999f).z, pivoter.at(2.001f).z, 1e-3);
}

TEST_CASE(quality.showcase_dances_have_no_foot_teleports) {
    for (const auto& dance : motion::ProceduralChoreography::showcase()) {
        float worstStep = 0.0f;
        float worstPlantedStep = 0.0f;
        for (int f = 0; f < 2; ++f) {
            const auto& track = dance.clip.feet[static_cast<std::size_t>(f)];
            for (std::size_t i = 1; i < track.position.size(); ++i) {
                const float step = (track.position[i] - track.position[i - 1]).length();
                worstStep = std::max(worstStep, step);
                // A foot that is on the floor in both keys must not travel at all.
                const bool planted =
                    track.position[i].y < 1.0e-4f && track.position[i - 1].y < 1.0e-4f;
                if (planted) {
                    // Planted-to-planted motion is only legal while a step lands, which
                    // is a smooth arc; a teleport is orders of magnitude larger.
                    worstPlantedStep = std::max(worstPlantedStep, step);
                }
            }
        }
        std::cout << std::fixed << std::setprecision(4) << "\n            " << dance.clip.id
                  << ": max foot step per key=" << worstStep << " (height units)";
        // The old pivot bug moved a planted foot 22 cm (0.1375) in a single key.
        CHECK(worstStep < 0.08f);
    }
}

// Overlays are the only thing that changes between "base clip" and "dancing with
// layers". Their contribution must never jump from one frame to the next, whichever
// cues overlap: a weaker gesture yields to a stronger accent over at least half a
// beat, not over the accent's own two-frame attack. This test measures the per-frame
// change of the overlay contribution (composed pose minus base pose, per bone) over the
// whole loop, including the loop seam, at several tempi and frame rates.
TEST_CASE(quality.overlay_contribution_never_jumps_between_frames) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    float worstEver = 0.0f;
    int violations = 0;
    for (const char* id : {"neonPulse", "streetCurrent", "fluidSignal"}) {
        const motion::HumanoidClip* clip = player.library().find(id);
        REQUIRE(clip != nullptr);
        const auto& cues = player.overlaysFor(id);
        for (double bpm : {80.0, 136.0, 170.0}) {
            for (double fps : {30.0, 60.0, 120.0}) {
                const double beatsPerFrame = bpm / 60.0 / fps;
                std::array<math::Quat, vrm::kHumanoidBoneCount> prevDelta{};
                bool havePrev = false;
                float worst = 0.0f;
                double worstBeat = 0.0;
                std::size_t worstBone = 0;
                for (double beat = 0.0; beat < 2.0 * clip->beats; beat += beatsPerFrame) {
                    const auto base = motion::sampleClip(*clip, beat);
                    const auto out = motion::composeMotion(base, cues, beat, clip->beats, clip->loop);
                    std::array<math::Quat, vrm::kHumanoidBoneCount> delta;
                    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
                        delta[b] = (out.pose.bone[b] * base.bone[b].conjugate()).normalized();
                        // The wrists carry the deliberate HandFlick snap; they are bounded
                        // separately below.
                        const bool isHand = b == static_cast<std::size_t>(vrm::HumanoidBone::LeftHand) ||
                                            b == static_cast<std::size_t>(vrm::HumanoidBone::RightHand);
                        if (havePrev && !isHand) {
                            const float d = std::abs(math::dot(delta[b], prevDelta[b]));
                            const float step = 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f));
                            if (step > worst) {
                                worst = step;
                                worstBeat = beat;
                                worstBone = b;
                            }
                        }
                    }
                    prevDelta = delta;
                    havePrev = true;
                }
                // Same physical limit at every frame rate: rad per second.
                const float radPerSecond = worst * static_cast<float>(fps);
                worstEver = std::max(worstEver, radPerSecond);
                // 20 rad/s for everything but the wrists: the loop-seam yank this test was
                // written for measured 46 rad/s (0.78 rad in one 60 FPS frame) in the arm.
                if (radPerSecond > 20.0f) {
                    ++violations;
                    std::cout << "\n            " << id << " @" << bpm << " BPM, " << fps << " FPS: overlay contribution changes at "
                              << radPerSecond << " rad/s (" << vrm::humanoidBoneName(static_cast<vrm::HumanoidBone>(worstBone))
                              << " at beat " << worstBeat << ")";
                }
            }
        }
    }
    std::cout << std::fixed << std::setprecision(2) << "\n            worst overlay-contribution rate over all runs: " << worstEver << " rad/s";
    CHECK(violations == 0);
}

// Reference-body leg clearance of a pair of foot tracks (cm).
namespace {

float loopMinLegClearance(const motion::detail::Footwork& left, const motion::detail::Footwork& right, float period) {
    float best = 1.0e9f;
    for (float t = 0.0f; t < period; t += 1.0f / 16.0f) {
        const auto l = left.at(t);
        const auto r = right.at(t);
        best = std::min(best, motion::detail::legClearanceCm({l.x, l.lift, l.z}, {r.x, r.lift, r.z}));
    }
    return best;
}

}  // namespace

TEST_CASE(quality.footwork_planner_keeps_one_leg_out_of_the_other) {
    // The Fluid Signal cross-step: one foot lands 24 cm across the other, with no
    // forward/back offset. Authored naively the legs occupy the same space.
    const float period = 16.0f;
    // Case 0: the right foot crosses to the character's left (+x); case 1: the left foot
    // crosses to the right (-x).
    for (int crossing = 0; crossing < 2; ++crossing) {
        const float reach = crossing == 0 ? 24.0f : -24.0f;
        motion::detail::Footwork naiveL(period, 0), naiveR(period, 1);
        naiveL.setStart(0, 0, 0);
        naiveR.setStart(0, 0, 0);
        motion::detail::Footwork& naiveMover = crossing == 0 ? naiveR : naiveL;
        naiveMover.step(2.0f, 3.0f, reach, 0.0f, 0.0f, 10.0f, 0.0f);   // no knowledge of the other foot
        naiveR.hold(period);
        naiveL.hold(period);
        CHECK(loopMinLegClearance(naiveL, naiveR, period) < 0.0f);       // the defect, reproduced

        motion::detail::Footwork planL(period, 0), planR(period, 1);
        planL.setStart(0, 0, 0);
        planR.setStart(0, 0, 0);
        planL.setOther(&planR);
        planR.setOther(&planL);
        motion::detail::Footwork& mover = crossing == 0 ? planR : planL;
        mover.step(2.0f, 3.0f, reach, 0.0f, 0.0f, 10.0f, 0.0f);
        planR.hold(period);
        planL.hold(period);
        CHECK(mover.adjustedSteps() == 1);
        CHECK(loopMinLegClearance(planL, planR, period) >= motion::detail::Footwork::kMinLegGapCm - 0.01f);
        // The planned step is still a step: the foot lands across the other one, in front or behind it.
        const auto landed = mover.at(period - 0.5f);
        CHECK(std::abs(landed.x) > 10.0f);
        CHECK(std::abs(landed.z) > 4.0f);
    }
}

TEST_CASE(quality.generated_dances_never_put_one_leg_through_the_other) {
    int clips = 0;
    float worst = 1.0e9f;
    for (const char* profile : {"neon", "street", "fluid"}) {
        for (int seed = 1; seed <= 40; ++seed) {
            motion::ChoreographyRequest request;
            request.profile = profile;
            request.seed = seed;
            request.bars = 8;
            request.intensity = 0.4f + 0.6f * static_cast<float>(seed % 5) / 4.0f;
            const auto dance = motion::ProceduralChoreography::generate(request);
            const auto& clip = dance.clip;
            for (int k = 0; k < clip.keyCount; ++k) {
                const auto& pl = clip.feet[0].position[static_cast<std::size_t>(k)];
                const auto& pr = clip.feet[1].position[static_cast<std::size_t>(k)];
                // clip positions are in height units (160 cm reference)
                const float clear = motion::detail::legClearanceCm({pl.x * 160.0f, pl.y * 160.0f, pl.z * 160.0f},
                                                                   {pr.x * 160.0f, pr.y * 160.0f, pr.z * 160.0f});
                worst = std::min(worst, clear);
            }
            ++clips;
        }
    }
    std::cout << std::fixed << std::setprecision(2) << "\n            " << clips << " generated clips, worst reference leg clearance " << worst << " cm";
    CHECK(worst >= motion::detail::Footwork::kMinLegGapCm - 0.5f);
}

namespace {

motion::HumanoidClip loadAuthored(const char* name) {
    const auto loaded = motion::loadClipFile(dancesDir() / (std::string(name) + ".json"));
    CHECK(loaded.ok);
    return loaded.clip;
}

float maxFootStep(const motion::HumanoidClip& clip) {
    float worst = 0.0f;
    for (const auto& foot : clip.feet) {
        for (std::size_t i = 1; i < foot.position.size(); ++i) {
            const auto d = foot.position[i] - foot.position[i - 1];
            worst = std::max(worst, d.length());
        }
    }
    return worst;
}

}  // namespace

TEST_CASE(quality.footwork_prepass_clears_the_authored_cross_over_steps) {
    int fixedClips = 0;
    for (const char* name : {"hiphop", "sidestep"}) {
        const motion::HumanoidClip clip = loadAuthored(name);
        motion::FootworkPlanReport report;
        const motion::HumanoidClip planned = motion::planClipFootwork(clip, {}, &report);
        std::cout << "\n            " << name << ": violation keys " << report.violationKeysBefore << " -> "
                  << report.violationKeysAfter << ", min clearance " << report.minClearanceBeforeCm << " -> "
                  << report.minClearanceAfterCm << " cm, " << report.stancesMoved << "/" << report.stances
                  << " stances moved, max shift " << report.maxLandingShiftCm << " cm";
        CHECK(report.violationKeysBefore > 0);  // the authored data really has the defect
        CHECK(report.violationKeysAfter == 0);
        CHECK(report.unresolved == 0);
        CHECK(report.changed);
        ++fixedClips;
    }
    CHECK(fixedClips == 2);
}

TEST_CASE(quality.footwork_prepass_keeps_the_music_and_every_other_track) {
    for (const char* name : {"hiphop", "sidestep"}) {
        const motion::HumanoidClip clip = loadAuthored(name);
        const motion::HumanoidClip planned = motion::planClipFootwork(clip);
        CHECK(planned.keyCount == clip.keyCount);
        CHECK(planned.bpm == clip.bpm);
        CHECK(planned.beats == clip.beats);
        CHECK(planned.accents.size() == clip.accents.size());
        CHECK(planned.contacts.size() == clip.contacts.size());
        for (std::size_t i = 0; i < clip.contacts.size(); ++i) {
            CHECK(planned.contacts[i].start == clip.contacts[i].start);
            CHECK(planned.contacts[i].end == clip.contacts[i].end);
            CHECK(planned.contacts[i].foot == clip.contacts[i].foot);
        }
        for (std::size_t b = 0; b < clip.bones.size(); ++b) {
            CHECK(planned.bones[b].size() == clip.bones[b].size());
        }
        for (std::size_t i = 0; i < clip.root.size(); ++i) {
            CHECK(planned.root[i].x == clip.root[i].x && planned.root[i].y == clip.root[i].y &&
                  planned.root[i].z == clip.root[i].z);
        }
        for (int f = 0; f < 2; ++f) {
            CHECK(planned.feet[static_cast<std::size_t>(f)].yaw == clip.feet[static_cast<std::size_t>(f)].yaw);
            CHECK(planned.feet[static_cast<std::size_t>(f)].toe == clip.feet[static_cast<std::size_t>(f)].toe);
        }
    }
}

TEST_CASE(quality.footwork_prepass_holds_every_stance_without_sliding) {
    for (const char* name : {"hiphop", "sidestep"}) {
        const motion::HumanoidClip clip = loadAuthored(name);
        const motion::HumanoidClip planned = motion::planClipFootwork(clip);
        const int K = clip.keysPerBeat;
        float worstSlideCm = 0.0f;
        for (const motion::ContactSegment& seg : clip.contacts) {
            const int a = static_cast<int>(std::ceil(seg.start * static_cast<float>(K) - 1.0e-3f));
            const int b = std::min(static_cast<int>(std::floor(seg.end * static_cast<float>(K) + 1.0e-3f)),
                                   clip.keyCount - 1);
            const auto& orig = clip.feet[static_cast<std::size_t>(seg.foot)].position;
            const auto& plan = planned.feet[static_cast<std::size_t>(seg.foot)].position;
            // The applied change must be one constant offset over the whole stance.
            const auto base = plan[static_cast<std::size_t>(a)] - orig[static_cast<std::size_t>(a)];
            for (int i = a; i <= b; ++i) {
                const auto d = plan[static_cast<std::size_t>(i)] - orig[static_cast<std::size_t>(i)];
                const float dx = d.x - base.x, dz = d.z - base.z;
                worstSlideCm = std::max(worstSlideCm, std::sqrt(dx * dx + dz * dz) * 160.0f);
            }
        }
        std::cout << "\n            " << name << ": worst change of the stance offset inside a contact "
                  << worstSlideCm << " cm";
        CHECK(worstSlideCm < 0.01f);
    }
}

TEST_CASE(quality.footwork_prepass_is_continuous_deterministic_and_idempotent) {
    for (const char* name : {"hiphop", "sidestep"}) {
        const motion::HumanoidClip clip = loadAuthored(name);
        const motion::HumanoidClip a = motion::planClipFootwork(clip);
        const motion::HumanoidClip b = motion::planClipFootwork(clip);
        for (int f = 0; f < 2; ++f) {
            const auto& pa = a.feet[static_cast<std::size_t>(f)].position;
            const auto& pb = b.feet[static_cast<std::size_t>(f)].position;
            CHECK(pa.size() == pb.size());
            for (std::size_t i = 0; i < pa.size(); ++i) {
                CHECK(pa[i].x == pb[i].x && pa[i].y == pb[i].y && pa[i].z == pb[i].z);
            }
            CHECK(pa.front().x == pa.back().x && pa.front().z == pa.back().z);  // the seam stays closed
        }
        const float before = maxFootStep(clip), after = maxFootStep(a);
        std::cout << "\n            " << name << ": largest foot step per key " << before * 160.0f << " -> "
                  << after * 160.0f << " cm";
        CHECK(after <= before * 1.6f + 0.004f);
        motion::FootworkPlanReport again;
        const motion::HumanoidClip c = motion::planClipFootwork(a, {}, &again);
        CHECK(again.violationKeysBefore == 0);
        CHECK(!again.changed);
        CHECK(c.feet[0].position.size() == a.feet[0].position.size());
    }
}

TEST_CASE(quality.footwork_prepass_leaves_clean_clips_alone) {
    for (const char* name : {"groove", "energetic", "idle"}) {
        const motion::HumanoidClip clip = loadAuthored(name);
        motion::FootworkPlanReport report;
        const motion::HumanoidClip planned = motion::planClipFootwork(clip, {}, &report);
        CHECK(report.violationKeysBefore == 0);
        CHECK(!report.changed);
        for (int f = 0; f < 2; ++f) {
            const auto& po = clip.feet[static_cast<std::size_t>(f)].position;
            const auto& pp = planned.feet[static_cast<std::size_t>(f)].position;
            CHECK(po.size() == pp.size());
            for (std::size_t i = 0; i < po.size(); ++i) {
                CHECK(po[i].x == pp[i].x && po[i].y == pp[i].y && po[i].z == pp[i].z);
            }
        }
    }
}

TEST_CASE(quality.dance_player_serves_planned_authored_clips_and_keeps_the_originals) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    const motion::HumanoidClip original = loadAuthored("hiphop");
    const motion::HumanoidClip* served = player.library().find("hiphop");
    const motion::HumanoidClip* kept = player.authoredClip("hiphop");
    CHECK(served != nullptr && kept != nullptr);
    if (served && kept) {
        bool servedDiffers = false, keptEqual = true;
        for (std::size_t i = 0; i < original.feet[0].position.size(); ++i) {
            const auto& o = original.feet[0].position[i];
            const auto& s = served->feet[0].position[i];
            const auto& k = kept->feet[0].position[i];
            servedDiffers = servedDiffers || std::abs(o.x - s.x) + std::abs(o.z - s.z) > 1.0e-6f;
            keptEqual = keptEqual && o.x == k.x && o.z == k.z && o.y == k.y;
        }
        CHECK(servedDiffers);
        CHECK(keptEqual);
        const motion::FootworkPlanReport* report = player.footworkReport("hiphop");
        CHECK(report != nullptr && report->violationKeysAfter == 0);
        player.setFootworkPlanning(false);  // switching it off serves the authored data again
        const motion::HumanoidClip* raw = player.library().find("hiphop");
        CHECK(raw != nullptr && raw->feet[0].position[100].x == original.feet[0].position[100].x);
    }
}

namespace {

// Ankle travelling along x at 0.10 + 0.10 sin(2 pi t) m/s for three seconds, never
// planted and 0.5 cm off the floor, so the drag gate (> 0.15 m/s) opens and closes
// several times. Sampled at `hz`; the closed form gives the exact position.
std::vector<qa::FootSample> slidingAnkle(double hz) {
    constexpr double kTau = 6.283185307179586;
    std::vector<qa::FootSample> track;
    const int count = static_cast<int>(3.0 * hz);
    for (int i = 0; i <= count; ++i) {
        const double t = i / hz;
        qa::FootSample sample;
        sample.t = t;
        sample.ankle = {static_cast<float>(0.10 * t + 0.10 * (1.0 - std::cos(kTau * t)) / kTau), 0.0f, 0.0f};
        sample.targetY = 0.005f;
        sample.planted = false;
        track.push_back(sample);
    }
    return track;
}

// Ground truth: the same gate applied to the analytic speed on a 10 kHz grid.
qa::GroundDrag analyticDrag() {
    constexpr double kTau = 6.283185307179586;
    qa::GroundDrag truth;
    const double dt = 1.0 / 10000.0;
    for (int i = 0; i < 30000; ++i) {
        const double t = i * dt;
        const double speed = 0.10 + 0.10 * std::sin(kTau * t);
        if (speed > 0.15) {
            truth.distance += speed * dt;
            truth.seconds += dt;
        }
    }
    return truth;
}

} // namespace

TEST_CASE(quality.ground_drag_does_not_depend_on_the_sampling_rate) {
    const qa::GroundDrag truth = analyticDrag();
    CHECK(truth.distance > 0.1 && truth.seconds > 0.5);
    for (double hz : {30.0, 60.0, 120.0, 240.0}) {
        const qa::GroundDrag drag = qa::measureGroundDrag(slidingAnkle(hz), 0.012f, 0.15);
        CHECK(std::abs(drag.distance - truth.distance) < 0.03 * truth.distance);
        CHECK(std::abs(drag.seconds - truth.seconds) < 0.03 * truth.seconds);
    }
}

TEST_CASE(quality.ground_drag_ignores_planted_and_raised_feet) {
    std::vector<qa::FootSample> track = slidingAnkle(120.0);
    for (qa::FootSample& sample : track) {
        sample.planted = true;
    }
    CHECK(qa::measureGroundDrag(track, 0.012f, 0.15).distance == 0.0);
    for (qa::FootSample& sample : track) {
        sample.planted = false;
        sample.targetY = 0.05f;  // 5 cm: a swing, not a drag
    }
    CHECK(qa::measureGroundDrag(track, 0.012f, 0.15).distance == 0.0);
}

TEST_CASE(quality.a_short_cue_performs_a_smaller_gesture_not_a_faster_one) {
    // BG Motion 3.3 quality pass: a cue that is too short for the gesture used
    // to get the full amplitude in less time, i.e. a wrist snap (14 rad/s on
    // neonPulse/Model1). The primitive now keeps its phase (the accent stays on
    // the authored beat) and is performed smaller.
    using motion::PrimitiveKind;
    const auto full = motion::samplePrimitive(PrimitiveKind::HandFlick, 0.5f, 1.0f, 0, 0.0f, 1.0f);
    const auto shortCue = motion::samplePrimitive(PrimitiveKind::HandFlick, 0.5f, 1.0f, 0, 0.0f, 0.18f);
    const auto legacy = motion::samplePrimitive(PrimitiveKind::HandFlick, 0.5f, 1.0f, 0, 0.0f, 0.0f);
    const auto hand = static_cast<std::size_t>(vrm::HumanoidBone::LeftHand);
    const float fullAngle = boneAngle(full.bone[hand], math::Quat::identity());
    const float shortAngle = boneAngle(shortCue.bone[hand], math::Quat::identity());
    const float legacyAngle = boneAngle(legacy.bone[hand], math::Quat::identity());
    std::cout << "\n            hand flick at phase 0.5: full=" << fullAngle
              << " short(0.18 s)=" << shortAngle << " legacy(no duration)=" << legacyAngle;
    CHECK(fullAngle > 0.05f);
    CHECK(shortAngle < 0.75f * fullAngle);
    CHECK(std::abs(legacyAngle - fullAngle) < 1.0e-5f);
    // The gesture still peaks at the same phase: the accent does not move.
    const auto early = motion::samplePrimitive(PrimitiveKind::HandFlick, 0.35f, 1.0f, 0, 0.0f, 0.18f);
    const auto earlyFull = motion::samplePrimitive(PrimitiveKind::HandFlick, 0.35f, 1.0f, 0, 0.0f, 1.0f);
    CHECK(boneAngle(early.bone[hand], math::Quat::identity()) <
          boneAngle(earlyFull.bone[hand], math::Quat::identity()));
}

TEST_CASE(quality.overlay_gestures_stay_below_a_twitch_at_60_fps) {
    // Companion guard at the playback level: no hand or head bone may rotate
    // faster than 12 rad/s (0.20 rad per 1/60 s frame). Before the 3.3 quality
    // pass neonPulse/energetic reached 14-16 rad/s (0.24-0.27 rad/frame) on
    // Model1 while the same clips were at 0-3 rad/s with overlays off, i.e. the
    // overlay layer, not the choreography, was the source.
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setOverlayMode(motion::OverlayMode::Full);
    const std::array<const char*, 4> clips = {"neonPulse", "streetCurrent", "energetic", "sidestep"};
    const std::array<vrm::HumanoidBone, 3> watched = {
        vrm::HumanoidBone::LeftHand, vrm::HumanoidBone::RightHand, vrm::HumanoidBone::Head};
    float worst = 0.0f;
    std::string worstWhere;
    for (const char* id : clips) {
        REQUIRE(player.selectClip(id, 0.0, true));
        motion::ClipPose previous;
        bool have = false;
        double worstAt = 0.0;
        float clipWorst = 0.0f;
        for (int i = 0; i <= 600; ++i) {  // 10 s at 60 FPS
            const double t = static_cast<double>(i) / 60.0;
            const motion::DanceFrame frame = player.update(t);
            if (have) {
                for (vrm::HumanoidBone bone : watched) {
                    const auto b = static_cast<std::size_t>(bone);
                    const float step = boneAngle(frame.pose.bone[b], previous.bone[b]);
                    if (step > clipWorst) {
                        clipWorst = step;
                        worstAt = t;
                    }
                    if (step > worst) {
                        worst = step;
                        worstWhere = std::string(id) + " bone " + std::to_string(b);
                    }
                }
            }
            previous = frame.pose;
            have = true;
        }
        std::cout << "\n            " << id << " worst hand/head step "
                  << 60.0f * clipWorst << " rad/s (t=" << worstAt << ")";
    }
    std::cout << "\n            worst=" << worst << " rad/frame at 60 FPS (" << worstWhere << ")";
    CHECK(worst <= 0.20f);
}

TEST_CASE(quality.generated_clip_wrist_plane_has_no_threshold_step) {
    // The procedural generator used to bake `elev > 80 ? 22 : 9` degrees of
    // wrist roll. Crossing that value between two keys put a 13 deg step in the
    // hand track. The roll is now a ramp and a signed azimuth; the check is the
    // resulting angular speed of the hand bone in the baked track.
    const auto dances = motion::ProceduralChoreography::showcase();
    REQUIRE(!dances.empty());
    float worst = 0.0f;  // deg/s
    for (const motion::GeneratedDance& dance : dances) {
        const double keySeconds = 60.0 / std::max(1.0, dance.clip.bpm) /
                                  std::max(1, dance.clip.keysPerBeat);
        for (std::size_t hand : {static_cast<std::size_t>(vrm::HumanoidBone::LeftHand),
                                 static_cast<std::size_t>(vrm::HumanoidBone::RightHand)}) {
            const auto& track = dance.clip.bones[hand];
            for (std::size_t i = 1; i < track.size(); ++i) {
                const float step = boneAngle(track[i], track[i - 1]);
                worst = std::max(worst,
                                 static_cast<float>(step * 180.0 / 3.14159265 / keySeconds));
            }
        }
    }
    std::cout << "\n            worst hand key speed " << worst << " deg/s";
    CHECK(worst < 300.0f);
}
