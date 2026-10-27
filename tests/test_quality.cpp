// BG Motion 3.2 quality pass regression tests: motion composition, masks,
// transitions. Spatial retarget tests live in test_retarget.cpp.
#include "framework.hpp"

#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/motion3/MotionPrimitive.hpp"

#include <algorithm>
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
