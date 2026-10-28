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
    CHECK(curve.bend.size() == 1);
    const Vec3 elbow = curve.elbow(0.0, samples[0].shoulder, samples[0].wrist,
                                   motion::kReferenceUpperArm, motion::kReferenceForearm);
    CHECK_NEAR(distance(elbow, samples[0].shoulder), motion::kReferenceUpperArm, 1e-4);
    CHECK_NEAR(distance(elbow, samples[0].wrist), motion::kReferenceForearm, 1e-4);
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
    const Vec3 shoulder = motion::referenceShoulder(1);
    const Vec3 wristStart{-0.20f, -0.10f, 0.05f};
    const Vec3 wristEnd{-0.12f, 0.20f, 0.18f};
    std::vector<ElbowSample> samples;
    for (int i = 0; i <= 16; ++i) {
        const float t = static_cast<float>(i) / 16.0f;
        samples.push_back({i * 0.25, shoulder, math::lerp(wristStart, wristEnd, t), {1.0f, 0.0f, 0.0f}});
    }
    const motion::ElbowSwivelCurve curve = motion::solveElbowSwivel(torso, samples);
    const auto wristAt = [&](double beat) {
        return math::lerp(wristStart, wristEnd, static_cast<float>(beat / 4.0));
    };
    // Beats are built from integers, so forward and backward reads hit identical times.
    constexpr int kReads = 240;
    std::vector<Vec3> forward;
    for (int i = 0; i <= kReads; ++i) {
        forward.push_back(curve.elbow(i / 60.0, shoulder, wristAt(i / 60.0),
                                      motion::kReferenceUpperArm, motion::kReferenceForearm));
    }
    for (int i = kReads; i >= 0; --i) {
        CHECK(distance(curve.elbow(i / 60.0, shoulder, wristAt(i / 60.0),
                                   motion::kReferenceUpperArm, motion::kReferenceForearm),
                       forward[static_cast<std::size_t>(i)]) == 0.0f);
    }
    // Every frame keeps both bones, not only the stored sample beats.
    for (int i = 0; i <= kReads; ++i) {
        const double beat = i / 60.0;
        const Vec3 wrist = wristAt(beat);
        const Vec3& elbow = forward[static_cast<std::size_t>(i)];
        CHECK(isFinite(elbow));
        CHECK_NEAR(distance(elbow, shoulder), motion::kReferenceUpperArm, 1e-4);
        CHECK_NEAR(distance(elbow, wrist), motion::kReferenceForearm, 1e-4);
    }
}

TEST_CASE(elbow.midpoint_keeps_both_bone_lengths) {
    // Two bend directions 90 degrees apart with the wrist fixed: the midpoint elbow must
    // still sit on the locus, so the forearm keeps its full length.
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder{};
    const Vec3 wrist{0.2f, 0.0f, 0.0f};
    const std::vector<ElbowSample> samples = {
        {0.0, shoulder, wrist, {0.0f, 1.0f, 0.0f}},
        {1.0, shoulder, wrist, {0.0f, 0.0f, 1.0f}},
    };
    motion::ElbowSolveOptions options;
    options.candidates = 1;  // pole-only elbow, so the stored bends are exactly the poles
    const motion::ElbowSwivelCurve curve = motion::solveElbowSwivel(torso, samples, options);
    const Vec3 mid = curve.elbow(0.5, shoulder, wrist, motion::kReferenceUpperArm,
                                 motion::kReferenceForearm);
    CHECK(isFinite(mid));
    CHECK_NEAR(distance(mid, shoulder), motion::kReferenceUpperArm, 1e-4);
    CHECK_NEAR(distance(mid, wrist), motion::kReferenceForearm, 1e-4);
}

TEST_CASE(elbow.antipodal_bend_does_not_shrink_the_arm) {
    // Fully extended arm swinging from one side to the other: the bend directions are
    // opposite, and the midpoint must not collapse.
    const auto torso = motion::syntheticTorsoProxy();
    const Vec3 shoulder{};
    const std::vector<ElbowSample> samples = {
        {0.0, shoulder, {0.32f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {1.0, shoulder, {-0.32f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    };
    const motion::ElbowSwivelCurve curve = motion::solveElbowSwivel(torso, samples);
    const Vec3 mid = curve.bendDirection(0.5);
    CHECK_NEAR(mid.length(), 1.0f, 1e-4);
    const Vec3 wrist{0.2f, 0.0f, 0.0f};
    const Vec3 elbow = curve.elbow(0.5, shoulder, wrist, motion::kReferenceUpperArm,
                                   motion::kReferenceForearm);
    CHECK_NEAR(distance(elbow, shoulder), motion::kReferenceUpperArm, 1e-4);
    CHECK_NEAR(distance(elbow, wrist), motion::kReferenceForearm, 1e-4);
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
    CHECK(curve.bend.size() == samples.size());
    for (const ElbowSample& sample : samples) {
        const Vec3 elbow = curve.elbow(sample.beat, sample.shoulder, sample.wrist,
                                       motion::kReferenceUpperArm, motion::kReferenceForearm);
        CHECK(isFinite(elbow));
        CHECK_NEAR(distance(elbow, sample.shoulder), motion::kReferenceUpperArm, 1e-4);
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
            for (const ElbowSample& sample : samples) {
                CHECK(isFinite(curve.elbow(sample.beat, sample.shoulder, sample.wrist,
                                           motion::kReferenceUpperArm,
                                           motion::kReferenceForearm)));
            }
        }
    }
}
