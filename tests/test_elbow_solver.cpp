#include "framework.hpp"

#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/motion3/ElbowSolver.hpp"
#include "bgmedia/motion3/TaskSpaceIntent.hpp"
#include "bgmedia/motion3/WristPlanner.hpp"

#include <cmath>
#include <filesystem>
#include <vector>

namespace {

using namespace bgmedia;
using math::Vec3;
using motion::ElbowSample;
using motion::ElbowSolveOptions;
using motion::ElbowSolveReport;

std::filesystem::path dancesDir() {
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances";
}

float distance(Vec3 a, Vec3 b) { return (a - b).length(); }

bool isFinite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

TEST_CASE(elbow.locus_keeps_bone_lengths_for_every_swivel) {
    const Vec3 shoulder = motion::referenceShoulder(0);
    const Vec3 wrist{0.30f, 0.08f, 0.20f};
    const Vec3 pole{0.0f, -1.0f, 0.0f};
    for (int i = 0; i < 16; ++i) {
        const float swivel = 2.0f * math::kPi * static_cast<float>(i) / 16.0f;
        bool degenerate = true;
        const Vec3 elbow = motion::elbowPosition(shoulder, wrist, swivel, pole,
                                                 motion::kReferenceUpperArm,
                                                 motion::kReferenceForearm, &degenerate);
        CHECK(!degenerate);
        CHECK_NEAR(distance(elbow, shoulder), motion::kReferenceUpperArm, 1e-4);
        CHECK_NEAR(distance(elbow, wrist), motion::kReferenceForearm, 1e-4);
    }
}

TEST_CASE(elbow.zero_swivel_bends_towards_the_pole) {
    const Vec3 shoulder = motion::referenceShoulder(0);
    const Vec3 wrist{0.30f, 0.08f, 0.20f};
    const Vec3 down{0.0f, -1.0f, 0.0f};
    const Vec3 towardsPole =
        motion::elbowPosition(shoulder, wrist, 0.0f, down, motion::kReferenceUpperArm,
                              motion::kReferenceForearm) - shoulder;
    const Vec3 opposite =
        motion::elbowPosition(shoulder, wrist, math::kPi, down, motion::kReferenceUpperArm,
                              motion::kReferenceForearm) - shoulder;
    // Swivel 0 hangs the elbow down (the pole); swivel pi lifts it up.
    CHECK(towardsPole.y < 0.0f);
    CHECK(opposite.y > 0.0f);
}

TEST_CASE(elbow.solver_leaves_the_torso_when_the_pole_points_inward) {
    // Wrist low and out to the side, pole pointing inward: the pole-only elbow (swivel 0)
    // penetrates the torso, and a swivel away from the body clears it.
    const auto torso = motion::syntheticTorsoProxy();
    const std::vector<ElbowSample> samples = {
        {0.0, motion::referenceShoulder(0), {0.28f, -0.10f, 0.0f}, {-1.0f, 0.0f, 0.0f}}};
    ElbowSolveReport report;
    const motion::ElbowSwivelCurve curve =
        motion::solveElbowSwivel(torso, samples, {}, &report);
    CHECK(report.minClearanceBefore < 0.0f);
    CHECK(report.minClearanceAfter >= 0.01f - 1e-3f);
    CHECK(curve.elbowOffset.size() == 1);
    CHECK_NEAR(curve.elbowOffset[0].length(), motion::kReferenceUpperArm, 1e-4);
}

TEST_CASE(elbow.swivel_curve_has_no_flips_over_a_sweep) {
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 start{0.10f, -0.24f, 0.0f};
    const Vec3 end{0.25f, -0.05f, 0.22f};
    std::vector<ElbowSample> samples;
    for (int i = 0; i <= 32; ++i) {
        const float t = static_cast<float>(i) / 32.0f;
        samples.push_back({i * 0.125, motion::referenceShoulder(0),
                           math::lerp(start, end, t), {-1.0f, 0.0f, 0.0f}});
    }
    ElbowSolveReport report;
    const motion::ElbowSwivelCurve curve =
        motion::solveElbowSwivel(torso, samples, {}, &report);
    CHECK(report.samples == 33);
    CHECK(report.flips == 0);
    CHECK(report.maxSwivelStep < 0.5f * math::kPi);
    CHECK(report.minClearanceAfter >= report.minClearanceBefore - 1e-4f);
}

TEST_CASE(elbow.curve_sampling_is_order_free_and_keeps_bone_length) {
    const auto torso = motion::syntheticTorsoProxy();
    std::vector<ElbowSample> samples;
    for (int i = 0; i <= 16; ++i) {
        const float t = static_cast<float>(i) / 16.0f;
        samples.push_back({i * 0.25, motion::referenceShoulder(1),
                           math::lerp(Vec3{-0.20f, -0.10f, 0.05f}, Vec3{-0.12f, 0.20f, 0.18f}, t),
                           {1.0f, 0.0f, 0.0f}});
    }
    const motion::ElbowSwivelCurve curve = motion::solveElbowSwivel(torso, samples);
    // Forward and backward reads at the same beats agree exactly.
    // Beats are built from integers, so forward and backward reads hit identical times.
    constexpr int kReads = 240;
    std::vector<Vec3> forward;
    for (int i = 0; i <= kReads; ++i) {
        forward.push_back(curve.sample(i / 60.0));
    }
    for (int i = kReads; i >= 0; --i) {
        CHECK(distance(curve.sample(i / 60.0), forward[static_cast<std::size_t>(i)]) == 0.0f);
    }
    for (const Vec3& offset : forward) {
        CHECK(isFinite(offset));
        CHECK_NEAR(offset.length(), motion::kReferenceUpperArm, 1e-4);
    }
}

TEST_CASE(elbow.degenerate_inputs_are_flagged_and_finite) {
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder = motion::referenceShoulder(0);
    const std::vector<ElbowSample> samples = {
        // Fully extended, straight down.
        {0.0, shoulder, shoulder + Vec3{0.0f, -motion::kReferenceArmReach, 0.0f}, {1.0f, 0.0f, 0.0f}},
        // Beyond reach: clamped onto the sphere.
        {1.0, shoulder, shoulder + Vec3{0.0f, -0.60f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        // Pole collinear with the shoulder-wrist axis (no preferred plane).
        {2.0, shoulder, shoulder + Vec3{0.10f, -0.10f, 0.0f}, {1.0f, -1.0f, 0.0f}},
        // Zero pole vector.
        {3.0, shoulder, shoulder + Vec3{0.20f, 0.0f, 0.10f}, {0.0f, 0.0f, 0.0f}},
    };
    ElbowSolveReport report;
    const motion::ElbowSwivelCurve curve = motion::solveElbowSwivel(torso, samples, {}, &report);
    CHECK(report.clampedSamples == 1);
    CHECK(report.degenerateSamples >= 3);
    CHECK(curve.elbowOffset.size() == samples.size());
    for (const Vec3& offset : curve.elbowOffset) {
        CHECK(isFinite(offset));
        CHECK_NEAR(offset.length(), motion::kReferenceUpperArm, 1e-4);
    }
}

TEST_CASE(elbow.authored_clips_solve_without_flips) {
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
            std::vector<ElbowSample> samples;
            for (double beat = 0.0; beat <= loaded.clip.beats + 1e-9; beat += 0.125) {
                const motion::ArmIntent arm =
                    motion::extractClipTaskSpaceIntent(loaded.clip, beat).arm[side];
                samples.push_back({beat, arm.shoulder, arm.wrist, arm.elbowPole});
            }
            ElbowSolveReport report;
            const motion::ElbowSwivelCurve curve =
                motion::solveElbowSwivel(torso, samples, {}, &report);
            CHECK(report.flips == 0);
            CHECK(report.minClearanceAfter >= report.minClearanceBefore - 1e-4f);
            for (const Vec3& offset : curve.elbowOffset) {
                CHECK(isFinite(offset));
            }
        }
    }
}
