#include "framework.hpp"

#include "bgmedia/motion3/MusicalTimeline.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace {

using namespace bgmedia;
using motion::AccentKind;
using motion::MusicalTimeline;

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
    const double period = 60.0 / bpm;
    for (int i = 0; i < count; ++i) {
        Beat beat;
        beat.time = first + i * period;
        beat.strength = (i % meter == 0) ? 1.0f : 0.6f;
        beat.confidence = 0.9f;
        beat.beatInBar = i % meter;
        beat.barIndex = i / meter;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
        Onset onset;
        onset.time = beat.time + 0.004;
        onset.strength = beat.strength;
        analysis.onsets.push_back(onset);
    }
    return analysis;
}

} // namespace

TEST_CASE(timeline.constant_grid_phase_bar_and_downbeat) {
    const MusicalTimeline timeline = MusicalTimeline::constant(120.0, 0.5, 4);
    CHECK(timeline.valid());
    CHECK_NEAR(timeline.beatAt(0.5), 0.0, 1e-9);
    CHECK_NEAR(timeline.beatAt(1.0), 1.0, 1e-9);
    CHECK_NEAR(timeline.beatPhaseAt(0.75), 0.5, 1e-6);
    CHECK(timeline.beatIndexAt(0.75) == 0);
    CHECK(timeline.beatInBarAt(0.5) == 0);
    CHECK(timeline.downbeatAt(0.5));
    CHECK(timeline.barAt(0.5) == 0);
    CHECK(timeline.barAt(2.5) == 1);
    CHECK(timeline.downbeatTrusted());
    CHECK_NEAR(timeline.nextDownbeatTime(0.5), 2.5, 1e-9);
    CHECK_NEAR(timeline.nextBarTime(0.5), 2.5, 1e-9);
    CHECK_NEAR(timeline.nextPhraseTime(0.5, 1), 2.5, 1e-9);
    CHECK_NEAR(timeline.nextBeatTime(0.5), 1.0, 1e-9);
}

TEST_CASE(timeline.analysis_phase_is_invertible_and_monotonic) {
    AnalysisResult analysis = makeGrid(120.0, 0.4, 80, 4);
    // Deliberate jitter: a real tracker is never sample-accurate.
    for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
        analysis.beats[i].time += (static_cast<int>(i * 7919 % 41) - 20) * 0.001;
    }
    const MusicalTimeline timeline = MusicalTimeline::fromAnalysis(analysis);
    CHECK(timeline.valid());
    CHECK(timeline.downbeatTrusted());
    double lastBeat = -1.0e9;
    for (double t = 0.0; t < 35.0; t += 0.0137) {
        const double beat = timeline.beatAt(t);
        const double phase = timeline.beatPhaseAt(t);
        CHECK(std::isfinite(beat));
        CHECK(std::isfinite(phase));
        CHECK(phase >= 0.0f && phase < 1.0f);
        CHECK(beat > lastBeat);
        lastBeat = beat;
        CHECK_NEAR(timeline.timeAtBeat(beat), t, 1e-5);
    }
    // The stable grid follows the music, not a free-running counter.
    for (int i = 5; i < 70; ++i) {
        CHECK_NEAR(timeline.timeAtBeat(i), 0.4 + i * 0.5, 0.02);
    }
}

TEST_CASE(timeline.edge_cases_never_divide_by_zero_or_produce_nan) {
    AnalysisResult empty;
    empty.bpm = 0.0;
    empty.info.duration = 10.0;
    const MusicalTimeline emptyTimeline = MusicalTimeline::fromAnalysis(empty);
    CHECK(emptyTimeline.valid());
    CHECK(!emptyTimeline.downbeatTrusted());
    for (double t = -2.0; t < 12.0; t += 0.37) {
        CHECK(std::isfinite(emptyTimeline.beatAt(t)));
        CHECK(std::isfinite(emptyTimeline.bpmAt(t)));
        CHECK(std::isfinite(emptyTimeline.timeAtBeat(emptyTimeline.beatAt(t))));
        CHECK(std::isfinite(emptyTimeline.nextDownbeatTime(t)));
    }

    AnalysisResult broken;
    broken.info.duration = 20.0;
    broken.bpm = 120.0;
    broken.beatsPerBar = 4;
    broken.meterConfidence = 0.9f;
    broken.downbeatConfidence = 0.9f;
    broken.beats = {
        {3.0, 0.4f, 0.5f, 0, 0, false},
        {1.0, 0.7f, 0.8f, 0, 1, false},
        {1.0, 0.7f, 0.8f, 0, 2, false},
        {std::numeric_limits<double>::quiet_NaN(), 0.0f, 0.0f, 0, 0, false},
        {-2.0, 0.0f, 0.0f, 0, 0, false},
        {5.0, 0.5f, 0.6f, 0, 0, false},
        {7.0, 0.5f, 0.6f, 0, 1, false},
    };
    const MusicalTimeline timeline = MusicalTimeline::fromAnalysis(broken);
    for (double t = -5.0; t < 30.0; t += 0.021) {
        const double beat = timeline.beatAt(t);
        CHECK(std::isfinite(beat));
        CHECK(std::isfinite(timeline.timeAtBeat(beat)));
        CHECK(std::isfinite(timeline.bpmAt(t)));
        CHECK(std::isfinite(timeline.nextDownbeatTime(t)));
    }
}

TEST_CASE(timeline.downbeat_is_gated_by_confidence) {
    AnalysisResult weak = makeGrid(120.0, 0.4, 40, 4, 0.20f, 0.80f);
    const MusicalTimeline weakTimeline = MusicalTimeline::fromAnalysis(weak);
    CHECK(!weakTimeline.downbeatTrusted());
    CHECK(!weakTimeline.downbeatAt(0.5));
    CHECK(!weakTimeline.beatIsDownbeat(0));
    // Graceful fallback: the next alignment point is the next beat, not an
    // invented bar.
    CHECK_NEAR(weakTimeline.nextDownbeatTime(1.1), weakTimeline.nextBeatTime(1.1), 1e-9);

    AnalysisResult strong = makeGrid(120.0, 0.4, 40, 4, 0.85f, 0.85f);
    const MusicalTimeline strongTimeline = MusicalTimeline::fromAnalysis(strong);
    CHECK(strongTimeline.downbeatTrusted());
    CHECK(strongTimeline.downbeatAt(2.4));
    CHECK(strongTimeline.beatIsDownbeat(4));
    const double next = strongTimeline.nextDownbeatTime(0.5);
    CHECK_NEAR(next, 2.4, 0.02);
    CHECK(strongTimeline.beatInBarAt(next + 0.01) == 0);
}

TEST_CASE(timeline.accent_map_keeps_real_timestamps_and_bass) {
    AnalysisResult analysis = makeGrid(120.0, 0.4, 24, 4);
    // Add a syncopated onset and a bass attack between beats.
    Onset off;
    off.time = 0.4 + 4 * 0.5 + 0.25;
    off.strength = 0.9f;
    analysis.onsets.push_back(off);
    BassAccent bass;
    bass.time = 3.9;
    bass.strength = 0.8f;
    bass.lowBandRatio = 0.7f;
    bass.confidence = 0.85f;
    bass.nearestBeat = 7;
    bass.offsetFromBeat = 0.0;
    analysis.bassAccents.push_back(bass);

    const MusicalTimeline timeline = MusicalTimeline::fromAnalysis(analysis);
    CHECK(!timeline.accents().empty());
    bool foundOffbeat = false;
    bool foundBass = false;
    for (const motion::AccentEvent& event : timeline.accents()) {
        if (std::abs(event.time - off.time) < 1e-9) {
            foundOffbeat = true;
            CHECK(event.kind == AccentKind::Offbeat);
            CHECK_NEAR(std::abs(event.beatOffset), 0.5f, 0.02);
            CHECK(event.strength > 0.5f);
        }
        if (std::abs(event.time - bass.time) < 0.02) {
            foundBass = true;
            CHECK(event.bass);
        }
        CHECK(event.time >= 0.0);
        CHECK(event.strength >= 0.0f && event.strength <= 1.0f);
        CHECK(event.confidence >= 0.0f && event.confidence <= 1.0f);
    }
    CHECK(foundOffbeat);
    CHECK(foundBass);
    CHECK(timeline.accentAt(off.time, 0.01) > 0.5f);
    CHECK(timeline.accentConfidenceAt(off.time, 0.01) > 0.4f);
}

TEST_CASE(timeline.subdivision_distinguishes_compound_feel) {
    AnalysisResult ternary = makeGrid(120.0, 0.4, 32, 4, 0.90f, 0.90f);
    // Replace the straight eighths with a clean triplet grid.
    ternary.onsets.clear();
    for (const Beat& beat : ternary.beats) {
        for (int sub = 0; sub < 3; ++sub) {
            Onset onset;
            onset.time = beat.time + sub * 0.5 / 3.0;
            onset.strength = sub == 0 ? 0.8f : 0.55f;
            ternary.onsets.push_back(onset);
        }
    }
    const MusicalTimeline compound = MusicalTimeline::fromAnalysis(ternary);
    CHECK(compound.subdivision() == 3);
    CHECK(compound.compound());
    CHECK(compound.meterLabel().find("12/8") != std::string::npos);

    AnalysisResult weak = ternary;
    weak.meterConfidence = 0.20f;
    weak.downbeatConfidence = 0.20f;
    const MusicalTimeline ambiguous = MusicalTimeline::fromAnalysis(weak);
    CHECK(ambiguous.subdivision() == 3);
    CHECK(!ambiguous.compound());
    CHECK(ambiguous.meterLabel().find("12/8") == std::string::npos);
}

TEST_CASE(timeline.silence_is_reported_without_inventing_music) {
    AnalysisResult analysis = makeGrid(120.0, 0.4, 32, 4);
    SilenceRegion silence;
    silence.start = 3.0;
    silence.end = 5.0;
    silence.confidence = 0.9f;
    analysis.silences.push_back(silence);
    const MusicalTimeline timeline = MusicalTimeline::fromAnalysis(analysis);
    CHECK(timeline.silentAt(4.0));
    CHECK(!timeline.silentAt(2.0));
    CHECK(!timeline.silentAt(6.0));
    CHECK_NEAR(timeline.silenceWeightAt(4.0, 0.12), 0.0, 1e-6);
    const float edgeWeight = timeline.silenceWeightAt(2.94, 0.12);
    CHECK(edgeWeight > 0.0f && edgeWeight < 1.0f);
    CHECK_NEAR(timeline.silenceWeightAt(2.0, 0.12), 1.0, 1e-6);
}

TEST_CASE(timeline.tempo_change_keeps_the_grid_finite_and_monotonic) {
    AnalysisResult analysis;
    analysis.info.duration = 30.0;
    analysis.bpm = 100.0;
    analysis.bpmConfidence = 0.7f;
    analysis.beatPhaseConfidence = 0.7f;
    analysis.meterConfidence = 0.7f;
    analysis.downbeatConfidence = 0.7f;
    analysis.beatsPerBar = 4;
    analysis.tempoStable = false;
    double time = 0.4;
    for (int i = 0; i < 60; ++i) {
        Beat beat;
        beat.time = time;
        beat.strength = i % 4 == 0 ? 0.9f : 0.5f;
        beat.confidence = 0.7f;
        beat.beatInBar = i % 4;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
        time += i < 30 ? 0.60 : 0.50;
    }
    TempoSegment a;
    a.start = 0.0;
    a.end = 18.0;
    a.bpm = 100.0;
    a.confidence = 0.8f;
    TempoSegment b;
    b.start = 18.0;
    b.end = 30.0;
    b.bpm = 120.0;
    b.confidence = 0.8f;
    analysis.tempoSegments = {a, b};

    const MusicalTimeline timeline = MusicalTimeline::fromAnalysis(analysis);
    double last = -1.0e9;
    for (double t = 0.0; t < 30.0; t += 0.01) {
        const double beat = timeline.beatAt(t);
        CHECK(std::isfinite(beat));
        CHECK(beat > last);
        last = beat;
        CHECK(std::isfinite(timeline.bpmAt(t)));
    }
}

TEST_CASE(timeline.describe_is_honest_about_confidence) {
    const MusicalTimeline timeline = MusicalTimeline::constant(100.0, 0.0, 4);
    CHECK(timeline.describe().find("4/4") != std::string::npos);
    AnalysisResult weak = makeGrid(100.0, 0.0, 16, 4, 0.1f, 0.1f);
    const MusicalTimeline honest = MusicalTimeline::fromAnalysis(weak);
    CHECK(honest.describe().find("not trusted") != std::string::npos);
}
