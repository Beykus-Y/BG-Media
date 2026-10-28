#include "framework.hpp"

#include "motion_metrics.hpp"

#include "bgmedia/motion3/DancePlayer.hpp"

#include <cmath>
#include <vector>

namespace {

using namespace bgmedia;

std::filesystem::path dancesDir() {
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances";
}

AnalysisResult makeGrid(double bpm, double first, int count, int meter,
                        float meterConfidence = 0.9f,
                        float downbeatConfidence = 0.9f) {
    AnalysisResult analysis;
    analysis.info.duration = first + count * 60.0 / bpm;
    analysis.bpm = bpm;
    analysis.bpmConfidence = 0.95f;
    analysis.beatPhaseConfidence = 0.95f;
    analysis.meterConfidence = meterConfidence;
    analysis.downbeatConfidence = downbeatConfidence;
    analysis.beatsPerBar = meter;
    analysis.tempoStable = true;
    for (int i = 0; i < count; ++i) {
        Beat beat;
        beat.time = first + i * 60.0 / bpm;
        beat.strength = i % meter == 0 ? 1.0f : 0.6f;
        beat.confidence = 0.9f;
        beat.beatInBar = i % meter;
        beat.barIndex = i / meter;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
        Onset onset;
        onset.time = beat.time + 0.003;
        onset.strength = beat.strength;
        analysis.onsets.push_back(onset);
    }
    return analysis;
}

} // namespace

TEST_CASE(sync31.manual_switch_prefers_a_trusted_downbeat) {
    motion::DancePlayer player;
    REQUIRE(player.loadLibrary(dancesDir()) >= 5);
    const AnalysisResult analysis = makeGrid(120.0, 0.4, 64, 4, 0.9f, 0.9f);
    player.setMusicalTimeline(motion::MusicalTimeline::fromAnalysis(analysis), 0.0);
    player.select(motion::DanceStyle::Groove, 0.77);
    const motion::ScheduleSegment& segment = player.director().schedule().segments().back();
    CHECK(segment.clip->id == "groove");
    CHECK(segment.alignedToDownbeat);
    CHECK(segment.barPhaseTrusted);
    const double beat = player.director().beatClock().beatAt(segment.alignTime);
    CHECK_NEAR(beat, std::round(beat), 1e-6);
    CHECK(player.director().beatClock().beatInBar(beat) == 0);
    CHECK(segment.alignTime >= 0.77);
}

TEST_CASE(sync31.low_meter_confidence_does_not_claim_a_downbeat) {
    motion::DancePlayer player;
    REQUIRE(player.loadLibrary(dancesDir()) >= 5);
    const AnalysisResult analysis = makeGrid(120.0, 0.4, 64, 4, 0.2f, 0.2f);
    const motion::MusicalTimeline timeline = motion::MusicalTimeline::fromAnalysis(analysis);
    CHECK(!timeline.downbeatTrusted());
    player.setMusicalTimeline(timeline, 0.0);
    player.select(motion::DanceStyle::Groove, 0.77);
    const motion::ScheduleSegment& segment = player.director().schedule().segments().back();
    CHECK(!segment.alignedToDownbeat);
    CHECK(!segment.barPhaseTrusted);
    // The dance still starts on a beat, not at an arbitrary sample.
    const double beat = player.director().beatClock().beatAt(segment.alignTime);
    CHECK_NEAR(beat, std::round(beat), 1e-6);
}

TEST_CASE(sync31.seek_preserves_bar_phase_and_clears_phase_error) {
    motion::DancePlayer player;
    REQUIRE(player.loadLibrary(dancesDir()) >= 5);
    const AnalysisResult analysis = makeGrid(100.0, 0.0, 200, 4, 0.9f, 0.9f);
    player.setMusicalTimeline(motion::MusicalTimeline::fromAnalysis(analysis), 0.0);
    player.select(motion::DanceStyle::Groove, 0.0, true);
    (void)player.update(5.0);
    for (double t : {2.0, 61.7, 0.0, 120.25, 31.4}) {
        const motion::DanceFrame frame = player.update(t);
        CHECK(frame.clipId == "groove");
        CHECK(std::isfinite(frame.pose.hips.y));
        CHECK(std::isfinite(frame.clipBeat));
        CHECK(std::isfinite(frame.warpRate));
        const double beat = player.director().beatClock().beatAt(t);
        double phase = std::fmod(frame.clipBeat - beat, 4.0);
        if (phase > 2.0) phase -= 4.0;
        if (phase < -2.0) phase += 4.0;
        // A seek no longer reseeds (BG Motion 3.3, A3), so the warp is not re-zeroed at the
        // jump: the phase error is exactly the warp's own correction, which is bounded by
        // A/pi < 0.5/pi beats (deviation = A/(2 pi) (cos - 1), A < 0.5).
        const double warpBound = 0.5 / std::acos(-1.0);
        CHECK(std::abs(phase) < warpBound + 1e-9);
        CHECK_NEAR(phase, frame.warpDeviationBeats, 1e-6);
        CHECK(frame.warpActive || std::abs(frame.warpRate - 1.0) < 1e-9);
    }
}

TEST_CASE(sync31.tempo_change_rebases_without_pose_or_warp_jump) {
    motion::DancePlayer player;
    REQUIRE(player.loadLibrary(dancesDir()) >= 5);
    player.setGrid(motion::BeatClock::constant(100.0, 0.0), 0.0);
    player.select(motion::DanceStyle::SideStep, 0.0, true);
    const motion::DanceFrame before = player.update(9.0);
    player.setGrid(motion::BeatClock::constant(140.0, 0.0), 9.0);
    const motion::DanceFrame after = player.update(9.0);
    double worst = 0.0;
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        worst = std::max(worst, qa::angleBetween(before.pose.bone[b], after.pose.bone[b]));
    }
    CHECK(worst < 0.1);
    CHECK_NEAR(before.clipBeat, after.clipBeat, 1e-6);
    CHECK(std::isfinite(after.warpRate));
    CHECK(std::isfinite(after.effectiveSpeed));
    CHECK(after.warpRate > 0.5 && after.warpRate < 1.5);
}

TEST_CASE(sync31.accent_pose_is_reached_exactly_on_the_target_beat) {
    motion::DancePlayer player;
    REQUIRE(player.loadLibrary(dancesDir()) >= 5);
    const AnalysisResult analysis = makeGrid(120.0, 0.0, 64, 4, 0.9f, 0.9f);
    player.setMusicalTimeline(motion::MusicalTimeline::fromAnalysis(analysis), 0.0);
    player.setExpression(motion::MotionExpression::Expressive);
    player.select(motion::DanceStyle::Groove, 0.0, true);

    const double targetTime = 2.0;  // beat 4, an authored downbeat accent
    const motion::DanceFrame frame = player.update(targetTime);
    const double beat = frame.clipBeat;
    CHECK_NEAR(beat, std::round(beat), 1e-9);
    CHECK_NEAR(frame.warpDeviationBeats, 0.0, 1e-9);

    const motion::HumanoidClip& groove = *player.library().forStyle(motion::DanceStyle::Groove);
    const motion::ClipPose reference = motion::sampleClip(groove, beat);
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        CHECK(qa::angleBetween(frame.pose.bone[b], reference.bone[b]) < 1e-4);
    }

    // One frame before the accent the phase is deliberately held back a little.
    const motion::DanceFrame before = player.update(targetTime - 1.0 / 60.0);
    CHECK(before.clipBeat < before.unwarpedClipBeat);
    CHECK(std::abs(before.warpDeviationBeats) < 0.45 / 3.14159265358979 + 1e-9);
    // The accent map exposes the real event, not a quantised slot.
    CHECK(frame.musicAccent > 0.0f);
}

TEST_CASE(sync31.silence_switches_the_warp_off_predictably) {
    AnalysisResult analysis = makeGrid(120.0, 0.0, 64, 4, 0.9f, 0.9f);
    SilenceRegion silence;
    silence.start = 3.0;
    silence.end = 5.5;
    silence.confidence = 0.9f;
    analysis.silences.push_back(silence);
    motion::DanceDirector director;
    motion::DanceLibrary library;
    library.loadDirectory(dancesDir());
    director.setLibrary(&library);
    director.setMusicalTimeline(motion::MusicalTimeline::fromAnalysis(analysis));
    director.reset(0.0);
    director.select(motion::DanceStyle::Groove, 0.0, false, true);
    director.setExpression(motion::MotionExpression::Expressive);
    const motion::DanceFrame silent = director.sample(4.0);
    CHECK(!silent.warpActive);
    CHECK_NEAR(silent.warpRate, 1.0, 1e-12);
    CHECK(std::isfinite(silent.clipBeat));
    const motion::DanceFrame after = director.sample(6.5);
    CHECK(std::isfinite(after.clipBeat));
}

TEST_CASE(sync31.analysis_timeline_survives_a_grid_refresh) {
    motion::DancePlayer player;
    REQUIRE(player.loadLibrary(dancesDir()) >= 5);
    const AnalysisResult analysis = makeGrid(120.0, 0.4, 64, 4, 0.9f, 0.9f);
    player.setAnalysis(&analysis, 0.0);
    CHECK(!player.timeline().accents().empty());
    CHECK(player.timeline().downbeatTrusted());
    // The legacy UI path used to call setGrid() with the analysis clock; it
    // must not silently discard the accent map / downbeat confidence.
    player.setGrid(motion::BeatClock::fromAnalysis(analysis), 0.0);
    CHECK(!player.timeline().accents().empty());
    CHECK(player.timeline().downbeatTrusted());
    CHECK(player.update(3.0).barPhaseTrusted);
}

TEST_CASE(sync31.auto_phrases_land_on_downbeats_when_meter_is_trusted) {
    AnalysisResult analysis = makeGrid(120.0, 0.5, 400, 4, 0.9f, 0.9f);
    analysis.info.duration = 200.0;
    for (double t = 0.5; t < 199.0; t += 0.25) {
        Onset onset;
        onset.time = t;
        onset.strength = t < 60.0 ? 0.2f : (t < 120.0 ? 0.95f : 0.25f);
        analysis.onsets.push_back(onset);
    }
    const auto section = [&](double start, double end, float energy) {
        Section s;
        s.start = start;
        s.end = end;
        s.energy = energy;
        s.confidence = 0.8f;
        analysis.sections.push_back(s);
    };
    section(0.0, 60.0, 0.15f);
    section(60.0, 120.0, 0.9f);
    section(120.0, 200.0, 0.2f);
    motion::DancePlayer player;
    REQUIRE(player.loadLibrary(dancesDir()) >= 5);
    player.setMode(motion::DanceMode::Auto, 0.0);
    player.setAnalysis(&analysis);
    int checked = 0;
    for (const motion::ScheduleSegment& segment : player.director().schedule().segments()) {
        if (segment.startTime < 0.0) {
            continue;
        }
        const double beat = player.director().beatClock().beatAt(segment.alignTime);
        CHECK_NEAR(beat, std::round(beat), 1e-6);
        if (segment.barPhaseTrusted) {
            CHECK(player.director().beatClock().beatInBar(beat) == 0);
            ++checked;
        }
    }
    CHECK(checked >= 2);
}

