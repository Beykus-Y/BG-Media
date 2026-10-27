#include "framework.hpp"

#include "motion_metrics.hpp"

#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/motion3/MusicalTimeWarp.hpp"

#include <cmath>
#include <filesystem>
#include <random>

namespace {

using namespace bgmedia;
using motion::MotionExpression;
using motion::MusicalTimeWarp;
using motion::WarpProfile;
using motion::WarpSample;

std::filesystem::path dancesDir() {
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances";
}

std::filesystem::path minimalVrm() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

AnalysisResult makeGrid(double bpm, int count, int meter = 4) {
    AnalysisResult analysis;
    analysis.info.duration = count * 60.0 / bpm;
    analysis.bpm = bpm;
    analysis.bpmConfidence = 0.95f;
    analysis.beatPhaseConfidence = 0.95f;
    analysis.meterConfidence = 0.9f;
    analysis.downbeatConfidence = 0.9f;
    analysis.beatsPerBar = meter;
    analysis.tempoStable = true;
    for (int i = 0; i < count; ++i) {
        Beat beat;
        beat.time = i * 60.0 / bpm;
        beat.strength = i % meter == 0 ? 1.0f : 0.6f;
        beat.confidence = 0.9f;
        beat.beatInBar = i % meter;
        beat.barIndex = i / meter;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
    }
    return analysis;
}

struct Slide {
    double maxMetres = 0.0;
    double footError = 0.0;
};

Slide measureSlide(const motion::DanceDirector& director, const vrm::VrmModel& model,
                   const animation::HumanoidRetargeter& rig, double from, double to) {
    Slide slide;
    const int nodes[2] = {model.nodeFor(vrm::HumanoidBone::LeftFoot),
                          model.nodeFor(vrm::HumanoidBone::RightFoot)};
    const double dt = 1.0 / 120.0;
    for (int f = 0; f < 2; ++f) {
        bool inRun = false;
        math::Vec3 anchor{};
        for (double t = from; t <= to; t += dt) {
            const motion::DanceFrame frame = director.sample(t);
            const auto& foot = frame.pose.foot[static_cast<std::size_t>(f)];
            const animation::RetargetedPose pose = rig.applyClip(frame.pose, t);
            slide.footError = std::max<double>(slide.footError, pose.stats.maxFootError);
            const bool planted =
                (foot.phase == motion::FootPhase::Stance ||
                 foot.phase == motion::FootPhase::Pivot) &&
                foot.contact > 0.999f;
            if (!planted) {
                inRun = false;
                continue;
            }
            const math::Vec3 p = animation::HumanoidRetargeter::stagePosition(pose, nodes[f]);
            if (!inRun) {
                inRun = true;
                anchor = p;
            } else {
                slide.maxMetres = std::max<double>(slide.maxMetres, (p - anchor).length());
            }
        }
    }
    return slide;
}

} // namespace

TEST_CASE(warp.endpoints_are_fixed_and_rate_stays_positive) {
    for (double amplitude : {0.0, 0.05, 0.10, 0.22, 0.45}) {
        CHECK_NEAR(MusicalTimeWarp::deviationAt(0.0, amplitude), 0.0, 1e-12);
        CHECK_NEAR(MusicalTimeWarp::deviationAt(1.0, amplitude), 0.0, 1e-12);
        CHECK_NEAR(MusicalTimeWarp::rateAt(0.0, amplitude), 1.0, 1e-12);
        CHECK_NEAR(MusicalTimeWarp::rateAt(1.0, amplitude), 1.0, 1e-12);
        double previous = -1.0;
        for (int i = 0; i <= 2000; ++i) {
            const double phase = static_cast<double>(i) / 2000.0;
            const double rate = MusicalTimeWarp::rateAt(phase, amplitude);
            CHECK(rate > 0.5);
            const double warped = phase + MusicalTimeWarp::deviationAt(phase, amplitude);
            CHECK(warped >= previous);
            previous = warped;
        }
    }
}

TEST_CASE(warp.derivative_matches_the_rate_curve) {
    for (double amplitude : {0.03, 0.10, 0.22, 0.45}) {
        for (double phase : {0.05, 0.2, 0.5, 0.73, 0.95}) {
            const double h = 1e-6;
            const double derivative =
                (MusicalTimeWarp::deviationAt(phase + h, amplitude) -
                 MusicalTimeWarp::deviationAt(phase - h, amplitude)) /
                (2.0 * h);
            CHECK_NEAR(derivative, MusicalTimeWarp::rateAt(phase, amplitude) - 1.0, 1e-5);
        }
    }
}

TEST_CASE(warp.sample_is_inactive_for_off_silence_and_no_accent) {
    WarpSample input;
    input.musicalBeat = 12.25;
    input.accent = 1.0f;
    input.bass = 0.5f;
    input.confidence = 1.0f;
    WarpProfile off;
    off.expression = MotionExpression::Off;
    const motion::WarpResult offResult = MusicalTimeWarp::sample(input, off);
    CHECK(!offResult.active);
    CHECK_NEAR(offResult.beat, 12.25, 1e-12);

    WarpProfile soft;
    soft.expression = MotionExpression::Soft;
    WarpSample silent = input;
    silent.silent = true;
    CHECK(!MusicalTimeWarp::sample(silent, soft).active);

    WarpSample quiet = input;
    quiet.accent = 0.0f;
    quiet.bass = 0.0f;
    CHECK(!MusicalTimeWarp::sample(quiet, soft).active);
}

TEST_CASE(warp.anticipates_but_lands_exactly_on_the_accent) {
    WarpSample input;
    input.musicalBeat = 7.25;
    input.accent = 1.0f;
    input.confidence = 1.0f;
    WarpProfile expressive;
    expressive.expression = MotionExpression::Expressive;
    const motion::WarpResult mid = MusicalTimeWarp::sample(input, expressive);
    CHECK(mid.active);
    CHECK(mid.beat < input.musicalBeat);
    CHECK(mid.beat > input.musicalBeat - 0.45 / 3.14159265358979);
    CHECK(mid.rate >= 0.5);

    input.musicalBeat = 7.0;
    const motion::WarpResult boundary = MusicalTimeWarp::sample(input, expressive);
    CHECK(boundary.active);
    CHECK_NEAR(boundary.beat, 7.0, 1e-12);
    CHECK_NEAR(boundary.rate, 1.0, 1e-12);
}

TEST_CASE(warp.contact_suppresses_full_body_amplitude_only) {
    WarpSample input;
    input.musicalBeat = 3.4;
    input.accent = 1.0f;
    input.confidence = 1.0f;
    WarpProfile full;
    full.expression = MotionExpression::Expressive;
    full.upperBodyOnly = false;
    full.contactScale = 0.25f;
    const motion::WarpResult free = MusicalTimeWarp::sample(input, full);
    input.footContact = true;
    const motion::WarpResult contact = MusicalTimeWarp::sample(input, full);
    CHECK(contact.amplitude < free.amplitude);
    CHECK_NEAR(contact.amplitude, free.amplitude * 0.25f, 1e-5f);

    WarpProfile upper;
    upper.expression = MotionExpression::Expressive;
    upper.upperBodyOnly = true;
    input.footContact = false;
    const motion::WarpResult upperFree = MusicalTimeWarp::sample(input, upper);
    input.footContact = true;
    const motion::WarpResult upperContact = MusicalTimeWarp::sample(input, upper);
    CHECK_NEAR(upperContact.amplitude, upperFree.amplitude, 1e-6f);
}

TEST_CASE(warp.random_inputs_remain_finite) {
    std::mt19937 rng(0xB6D1A2u);
    std::uniform_real_distribution<double> beat(0.0, 1.0e6);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (int i = 0; i < 20000; ++i) {
        WarpSample input;
        input.musicalBeat = beat(rng);
        input.accent = unit(rng);
        input.bass = unit(rng);
        input.confidence = unit(rng);
        input.footContact = (i & 1) != 0;
        input.silent = (i % 17) == 0;
        const WarpProfile profile{MotionExpression::Expressive, unit(rng), (i & 2) != 0,
                                  0.35f};
        const motion::WarpResult result = MusicalTimeWarp::sample(input, profile);
        CHECK(std::isfinite(result.beat));
        CHECK(std::isfinite(result.deviation));
        CHECK(std::isfinite(result.rate));
        CHECK(result.rate > 0.5 && result.rate < 1.5);
    }
}

TEST_CASE(warp.profile_roundtrip) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "bgmedia_warp_profile_test.cfg";
    WarpProfile profile;
    profile.expression = MotionExpression::Expressive;
    profile.intensity = 0.65f;
    profile.upperBodyOnly = false;
    profile.contactScale = 0.4f;
    std::string error;
    CHECK(motion::saveWarpProfile(path, profile, &error));
    WarpProfile loaded;
    CHECK(motion::loadWarpProfile(path, loaded, &error));
    CHECK(loaded.expression == MotionExpression::Expressive);
    CHECK_NEAR(loaded.intensity, 0.65, 1e-6);
    CHECK(!loaded.upperBodyOnly);
    CHECK_NEAR(loaded.contactScale, 0.4, 1e-6);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE(warp.does_not_move_root_or_foot_targets) {
    motion::DanceDirector director;
    motion::DanceLibrary library;
    library.loadDirectory(dancesDir());
    director.setLibrary(&library);
    director.setMusicalTimeline(motion::MusicalTimeline::fromAnalysis(makeGrid(120.0, 64)));
    director.reset(0.0);
    director.select(motion::DanceStyle::Groove, 0.0, false, true);

    WarpProfile off;
    off.expression = MotionExpression::Off;
    director.setWarpProfile(off);
    std::vector<motion::ClipPose> stable;
    for (double t = 0.0; t < 8.0; t += 1.0 / 120.0) {
        stable.push_back(director.sample(t).pose);
    }
    WarpProfile expressive;
    expressive.expression = MotionExpression::Expressive;
    director.setWarpProfile(expressive);
    int index = 0;
    for (double t = 0.0; t < 8.0; t += 1.0 / 120.0, ++index) {
        const motion::DanceFrame frame = director.sample(t);
        const motion::ClipPose& base = stable[static_cast<std::size_t>(index)];
        CHECK(std::isfinite(frame.pose.hips.y));
        CHECK_NEAR(frame.pose.rootX, base.rootX, 1e-7);
        CHECK_NEAR(frame.pose.rootZ, base.rootZ, 1e-7);
        CHECK_NEAR(frame.pose.rootYaw, base.rootYaw, 1e-7);
        for (std::size_t f = 0; f < 2; ++f) {
            CHECK_NEAR(frame.pose.foot[f].position.x, base.foot[f].position.x, 1e-7);
            CHECK_NEAR(frame.pose.foot[f].position.y, base.foot[f].position.y, 1e-7);
            CHECK_NEAR(frame.pose.foot[f].position.z, base.foot[f].position.z, 1e-7);
        }
    }
}

TEST_CASE(warp.keeps_retargeted_feet_planted) {
    vrm::LoadResult loaded = vrm::VrmLoader::load(minimalVrm());
    REQUIRE(loaded.ok());
    animation::HumanoidRetargeter rig(*loaded.model);
    REQUIRE(rig.valid());

    motion::DanceDirector director;
    motion::DanceLibrary library;
    library.loadDirectory(dancesDir());
    director.setLibrary(&library);
    director.setMusicalTimeline(motion::MusicalTimeline::fromAnalysis(makeGrid(120.0, 64)));
    director.reset(0.0);
    director.select(motion::DanceStyle::Groove, 0.0, false, true);

    WarpProfile off;
    off.expression = MotionExpression::Off;
    director.setWarpProfile(off);
    const Slide baseline = measureSlide(director, *loaded.model, rig, 0.0, 8.0);
    WarpProfile expressive;
    expressive.expression = MotionExpression::Expressive;
    director.setWarpProfile(expressive);
    const Slide warped = measureSlide(director, *loaded.model, rig, 0.0, 8.0);
    CHECK(warped.footError < 0.004);
    CHECK(warped.maxMetres <= baseline.maxMetres + 0.001);
    CHECK(warped.maxMetres < 0.006);
}

TEST_CASE(warp.expressive_mode_changes_timing_but_not_the_accent_pose) {
    motion::DanceDirector director;
    motion::DanceLibrary library;
    library.loadDirectory(dancesDir());
    director.setLibrary(&library);
    director.setMusicalTimeline(motion::MusicalTimeline::constant(120.0, 0.0, 4));
    director.reset(0.0);
    director.select(motion::DanceStyle::Groove, 0.0, false, true);
    WarpProfile expressive;
    expressive.expression = MotionExpression::Expressive;
    director.setWarpProfile(expressive);

    // Beat 4 is an authored downbeat accent: the warp must not move it.
    const motion::DanceFrame atBeat = director.sample(2.0);
    CHECK_NEAR(atBeat.clipBeat, std::round(atBeat.clipBeat), 1e-9);
    CHECK(atBeat.warpActive);
    CHECK_NEAR(atBeat.warpDeviationBeats, 0.0, 1e-9);

    // Half a beat earlier the animation is held back, then accelerates in.
    const motion::DanceFrame before = director.sample(1.75);
    CHECK(before.warpActive);
    CHECK(before.clipBeat < before.unwarpedClipBeat);
    CHECK(std::abs(before.warpDeviationBeats) < 0.45 / 3.14159265358979 + 1e-9);

    const motion::HumanoidClip& groove =
        *library.forStyle(motion::DanceStyle::Groove);
    const motion::ClipPose reference = motion::sampleClip(groove, atBeat.clipBeat);
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        CHECK(qa::angleBetween(atBeat.pose.bone[b], reference.bone[b]) < 1e-4);
    }
}
