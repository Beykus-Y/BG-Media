#include "framework.hpp"

#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/motion3/TaskSpaceIntent.hpp"
#include "bgmedia/motion3/WristPlanner.hpp"

#include <cmath>
#include <filesystem>
#include <vector>

namespace {

using namespace bgmedia;
using math::Vec3;
using motion::WristKey;
using motion::WristPlanOptions;
using motion::WristPlanReport;

std::filesystem::path dancesDir() {
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances";
}

float distance(Vec3 a, Vec3 b) { return (a - b).length(); }

// Keys in the left arm's chest frame: out, across the chest to the far shoulder, back.
std::vector<WristKey> acrossChestKeys() {
    const Vec3 shoulder = motion::referenceShoulder(0);
    return {{0.0, {0.42f, 0.08f, 0.0f}, shoulder},
            {2.0, {-0.15f, 0.08f, 0.0f}, shoulder},
            {4.0, {0.42f, 0.08f, 0.0f}, shoulder}};
}

} // namespace

TEST_CASE(planner.torso_distance_signs) {
    const auto torso = motion::syntheticTorsoProxy();
    CHECK(motion::torsoSignedDistance(torso, {0.0f, 0.0f, 0.0f}) < 0.0f);   // inside
    CHECK(motion::torsoSignedDistance(torso, {0.0f, 0.0f, 0.3f}) > 0.0f);   // in front
    CHECK(motion::torsoSignedDistance(torso, {0.3f, 0.0f, 0.0f}) > 0.0f);   // to the side
    CHECK(motion::torsoSignedDistance(torso, {0.0f, 0.2f, 0.0f}) > 0.0f);   // above the torso
    CHECK(motion::torsoSignedDistance(torso, {0.0f, -0.3f, 0.0f}) > 0.0f);  // below the torso
    CHECK(motion::torsoSignedDistance(torso, {0.0f, 0.0f, 0.3f}) >
          motion::torsoSignedDistance(torso, {0.0f, 0.0f, 0.2f}));           // grows outward
}

TEST_CASE(planner.reference_shoulder_matches_intent_rig) {
    const Vec3 left = motion::referenceShoulder(0);
    const Vec3 right = motion::referenceShoulder(1);
    CHECK_NEAR(left.x, 0.10f, 1e-6);
    CHECK_NEAR(left.y, 0.08f, 1e-6);
    CHECK_NEAR(right.x, -0.10f, 1e-6);
    // The rest wrist sits exactly one full reach away on the same level.
    const motion::ArmIntent rest =
        motion::extractTaskSpaceIntent(motion::neutralClipPose()).arm[0];
    CHECK_NEAR(distance(rest.wrist, left), motion::kReferenceArmReach, 1e-5);
}

TEST_CASE(planner.across_chest_touch_detours_around_torso) {
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder = motion::referenceShoulder(0);
    const Vec3 start{0.42f, 0.08f, 0.0f};
    const Vec3 end{-0.15f, 0.08f, 0.0f};
    // The straight line crosses the sternum: a real penetration.
    CHECK(motion::polylineClearance(torso, {start, end}, 0.01f, 0.03f) < 0.0f);

    const motion::WristSegmentPlan plan =
        motion::planWristSegment(torso, shoulder, shoulder, start, end);
    CHECK(plan.detoured);
    CHECK(!plan.attenuated);
    CHECK(!plan.conflict);
    CHECK(plan.points.size() == 3);
    CHECK(plan.points.front().x == start.x);
    CHECK(plan.points.back().x == end.x);
    CHECK(plan.minClearance >= 0.01f - 1e-4f);
    // The chosen route goes in front of the sternum.
    CHECK(plan.points[1].z > 0.0f);
}

TEST_CASE(planner.relaxed_swing_is_untouched) {
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder = motion::referenceShoulder(0);
    const Vec3 start{0.36f, -0.05f, 0.05f};
    const Vec3 end{0.25f, -0.18f, 0.02f};
    const motion::WristSegmentPlan plan =
        motion::planWristSegment(torso, shoulder, shoulder, start, end);
    CHECK(!plan.detoured);
    CHECK(!plan.attenuated);
    CHECK(plan.points.size() == 2);
    CHECK(distance(plan.points[1], end) == 0.0f);
}

TEST_CASE(planner.overhead_gesture_is_direct) {
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder = motion::referenceShoulder(0);
    // Above the torso top the proxy is clear, so the straight raise is kept.
    const motion::WristSegmentPlan plan =
        motion::planWristSegment(torso, shoulder, shoulder, {0.42f, 0.08f, 0.0f}, {0.08f, 0.36f, 0.0f});
    CHECK(!plan.detoured);
    CHECK(!plan.attenuated);
    CHECK(plan.minClearance >= 0.01f - 1e-4f);
}

TEST_CASE(planner.behind_to_front_sweep_is_planned_or_flagged) {
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder = motion::referenceShoulder(0);
    const Vec3 start{0.0f, -0.02f, -0.20f};
    const Vec3 end{0.0f, -0.02f, 0.20f};
    const motion::WristSegmentPlan plan =
        motion::planWristSegment(torso, shoulder, shoulder, start, end);
    CHECK(!plan.conflict);
    // Either a clear route was found, or the gesture was shortened and flagged.
    // It is never reported as clear while it still penetrates.
    CHECK(plan.detoured || plan.attenuated);
    if (!plan.attenuated) {
        CHECK(plan.minClearance >= 0.01f - 1e-4f);
    }
}

TEST_CASE(planner.key_path_is_clear_reachable_and_continuous) {
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder = motion::referenceShoulder(0);
    WristPlanReport report;
    const motion::WristPath path =
        motion::planWristPath(torso, acrossChestKeys(), {}, &report);

    CHECK(report.segments == 2);
    CHECK(report.detoured == 2);
    CHECK(report.attenuated == 0);
    CHECK(report.conflicts == 0);
    CHECK(report.directClearance < 0.0f);
    CHECK(report.plannedClearance >= 0.01f - 1e-4f);

    // Dense sampling: clear, reachable, and no jumps between samples.
    const double dt = 0.01;
    Vec3 previous = path.sample(0.0);
    float worstStep = 0.0f;
    float worstClearance = 1.0e6f;
    for (double beat = 0.0; beat <= 4.0 + 1e-9; beat += dt) {
        const Vec3 p = path.sample(beat);
        worstStep = std::max(worstStep, distance(p, previous));
        previous = p;
        worstClearance = std::min(worstClearance,
                                  motion::torsoSignedDistance(torso, p) - 0.03f);
        CHECK((p - shoulder).length() <= motion::WristPlanOptions{}.reachLimit + 1e-4f);
    }
    CHECK(worstClearance >= 0.01f - 1e-3f);
    CHECK(worstStep < 0.02f);

    // The planned path still hits its keys.
    CHECK(distance(path.sample(0.0), {0.42f, 0.08f, 0.0f}) < 1e-6f);
    CHECK(distance(path.sample(2.0), {-0.15f, 0.08f, 0.0f}) < 1e-6f);
    CHECK(distance(path.sample(4.0), {0.42f, 0.08f, 0.0f}) < 1e-6f);
}

TEST_CASE(planner.is_deterministic) {
    const auto torso = motion::syntheticTorsoProxy();
    const motion::WristPath a = motion::planWristPath(torso, acrossChestKeys());
    const motion::WristPath b = motion::planWristPath(torso, acrossChestKeys());
    CHECK(a.keys.size() == b.keys.size());
    for (std::size_t i = 0; i < a.keys.size() && i < b.keys.size(); ++i) {
        CHECK(a.keys[i].beat == b.keys[i].beat);
        CHECK(distance(a.keys[i].wrist, b.keys[i].wrist) == 0.0f);
    }
}

TEST_CASE(planner.repeated_crossings_keep_one_detour_side) {
    // Back-and-forth across the sternum: every detour should pick the same side
    // (front), so the path never flips between equally valid routes.
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder = motion::referenceShoulder(0);
    std::vector<WristKey> keys;
    for (int i = 0; i < 6; ++i) {
        const float x = i % 2 == 0 ? 0.42f : -0.15f;
        keys.push_back({static_cast<double>(i), {x, 0.08f, 0.0f}, shoulder});
    }
    WristPlanReport report;
    const motion::WristPath path = motion::planWristPath(torso, keys, {}, &report);
    CHECK(report.detoured == 5);
    CHECK(report.attenuated == 0);
    for (const WristKey& key : path.keys) {
        CHECK(key.wrist.z >= -1e-6f);
    }
}

TEST_CASE(planner.reach_is_clamped_before_planning) {
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder = motion::referenceShoulder(0);
    WristPlanReport report;
    const std::vector<WristKey> keys = {{0.0, {0.1f, 0.08f, 0.0f}, shoulder},
                                        {1.0, {0.1f, 0.90f, 0.0f}, shoulder}};
    const motion::WristPath path =
        motion::planWristPath(torso, keys, {}, &report);
    CHECK(report.reachClamped == 1);
    CHECK((path.sample(1.0) - shoulder).length() <= motion::WristPlanOptions{}.reachLimit + 1e-4f);
}

TEST_CASE(planner.authored_clips_are_never_made_worse) {
    // Offline QA on the reference skeleton: the planned wrist path must never be closer
    // to the synthetic torso than the straight authored path, and must stay reachable.
    const auto torso = motion::syntheticTorsoProxy();
    const std::vector<std::string> names = {"energetic.json", "groove.json", "hiphop.json",
                                            "idle.json", "sidestep.json"};
    for (const std::string& name : names) {
        const auto loaded = motion::loadClipFile(dancesDir() / name);
        CHECK(loaded.ok);
        if (!loaded.ok) {
            continue;
        }
        for (std::size_t side = 0; side < 2; ++side) {
            std::vector<WristKey> keys;
            for (double beat = 0.0; beat <= loaded.clip.beats + 1e-9; beat += 0.125) {
                const motion::TaskSpaceIntent intent =
                    motion::extractClipTaskSpaceIntent(loaded.clip, beat);
                keys.push_back({beat, intent.arm[side].wrist, intent.arm[side].shoulder});
            }
            WristPlanReport report;
            const motion::WristPath path =
                motion::planWristPath(torso, keys, {}, &report);
            CHECK(report.plannedClearance >= report.directClearance - 1e-4f);
            for (double beat = 0.0; beat <= loaded.clip.beats + 1e-9; beat += 1.0 / 32.0) {
                const Vec3 shoulderAtBeat =
                    motion::extractClipTaskSpaceIntent(loaded.clip, beat).arm[side].shoulder;
                CHECK((path.sample(beat) - shoulderAtBeat).length() <=
                      motion::WristPlanOptions{}.reachLimit + 1e-4f);
            }
        }
    }
}
