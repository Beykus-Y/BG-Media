#include "framework.hpp"

#include "bgmedia/analysis/Analyzer.hpp"
#include "analysis/EventDetection.hpp"
#include "analysis/TempoEstimator.hpp"
#include "bgmedia/motion3/MusicalTimeline.hpp"
#include "support/SyntheticSignals.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {

struct AnalysisRun {
    bgmedia::PcmBuffer pcm;
    bgmedia::AnalysisResult result;
    bgmedia::test::SyntheticTrack track;
};

AnalysisRun analyzeTrack(const bgmedia::test::SyntheticTrack& track,
                         bgmedia::AnalysisConfig config = {}) {
    using namespace bgmedia;
    AnalysisRun run;
    run.track = track;
    std::string error;
    run.pcm = PcmBuffer::fromAudio(track.audio, &error);
    if (!run.pcm.valid()) {
        throw test::Failure("failed to create PCM: " + error);
    }
    Analyzer analyzer(config);
    AnalysisReport report = analyzer.analyze(run.pcm);
    if (!report.ok()) {
        throw test::Failure("analysis failed: " + report.message);
    }
    run.result = std::move(report.result);
    return run;
}

struct Alignment {
    int matched = 0;
    int total = 0;
    double meanError = 0.0;
    double maxError = 0.0;
};

Alignment alignBeats(const std::vector<double>& truth,
                     const std::vector<bgmedia::Beat>& detected,
                     double tolerance, double fromTime, double toTime) {
    Alignment alignment;
    bool hasError = false;
    for (double expected : truth) {
        if (expected < fromTime || expected > toTime) {
            continue;
        }
        ++alignment.total;
        double best = 1e9;
        for (const auto& beat : detected) {
            const double error = std::abs(beat.time - expected);
            if (error < best) {
                best = error;
            }
        }
        if (best <= tolerance) {
            ++alignment.matched;
            alignment.meanError += best;
            alignment.maxError = std::max(alignment.maxError, best);
            hasError = true;
        }
    }
    if (hasError && alignment.matched > 0) {
        alignment.meanError /= alignment.matched;
    }
    return alignment;
}

double nearestBeat(const std::vector<bgmedia::Beat>& beats, double time) {
    double best = 1e9;
    for (const auto& beat : beats) {
        best = std::min(best, std::abs(beat.time - time));
    }
    return best;
}

struct BeatAgreement {
    int matched = 0;
    int detected = 0;
    int reference = 0;
    double precision = 0.0;
    double recall = 0.0;
    double fMeasure = 0.0;
    double meanSignedError = 0.0;
    double p95AbsoluteError = 0.0;
    double medianAbsMs = 0.0;
    double p95AbsMs = 0.0;
};

BeatAgreement scoreBeats(const std::vector<double>& truth,
                         const std::vector<bgmedia::Beat>& detected,
                         double tolerance, double fromTime, double toTime) {
    BeatAgreement score;
    std::vector<double> detectedTimes;
    for (const auto& beat : detected) {
        if (beat.time >= fromTime && beat.time <= toTime) {
            detectedTimes.push_back(beat.time);
        }
    }
    std::vector<double> referenceTimes;
    for (double time : truth) {
        if (time >= fromTime && time <= toTime) {
            referenceTimes.push_back(time);
        }
    }
    std::vector<bool> used(detectedTimes.size(), false);
    std::vector<double> errors;
    for (double expected : referenceTimes) {
        int bestIndex = -1;
        double bestError = tolerance + 1e-12;
        for (std::size_t i = 0; i < detectedTimes.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const double error = std::abs(detectedTimes[i] - expected);
            if (error <= bestError) {
                bestError = error;
                bestIndex = static_cast<int>(i);
            }
        }
        if (bestIndex >= 0) {
            used[static_cast<std::size_t>(bestIndex)] = true;
            ++score.matched;
            errors.push_back(detectedTimes[static_cast<std::size_t>(bestIndex)] - expected);
        }
    }
    score.detected = static_cast<int>(detectedTimes.size());
    score.reference = static_cast<int>(referenceTimes.size());
    score.precision = score.detected > 0
                          ? static_cast<double>(score.matched) / score.detected
                          : 0.0;
    score.recall = score.reference > 0
                       ? static_cast<double>(score.matched) / score.reference
                       : 0.0;
    score.fMeasure = score.precision + score.recall > 0.0
                         ? 2.0 * score.precision * score.recall /
                               (score.precision + score.recall)
                         : 0.0;
    if (!errors.empty()) {
        double sum = 0.0;
        std::vector<double> absolute;
        absolute.reserve(errors.size());
        for (double error : errors) {
            sum += error;
            absolute.push_back(std::abs(error));
        }
        score.meanSignedError = sum / static_cast<double>(errors.size());
        std::sort(absolute.begin(), absolute.end());
        const std::size_t index = static_cast<std::size_t>(
            std::ceil(0.95 * static_cast<double>(absolute.size())) - 1.0);
        score.p95AbsoluteError = absolute[std::min(index, absolute.size() - 1)];
        score.medianAbsMs = absolute[absolute.size() / 2] * 1000.0;
        score.p95AbsMs = absolute[std::min(index, absolute.size() - 1)] * 1000.0;
    }
    return score;
}

BeatAgreement scoreDownbeats(const std::vector<double>& truth,
                             const std::vector<bgmedia::Beat>& detected,
                             double tolerance, double fromTime, double toTime) {
    std::vector<bgmedia::Beat> downbeats;
    for (const auto& beat : detected) {
        if (beat.downbeat) {
            downbeats.push_back(beat);
        }
    }
    return scoreBeats(truth, downbeats, tolerance, fromTime, toTime);
}

} // namespace

TEST_CASE(analysis.bpm_92) {
    using namespace bgmedia;
    test::TrackOptions options;
    options.bpm = 92.0;
    options.duration = 20.0;
    const auto run = analyzeTrack(test::makeClickTrack(options));

    CHECK_NEAR(run.result.bpm, 92.0, 1.5);
    CHECK(run.result.bpmConfidence > 0.55f);
    CHECK(run.result.tempoStable);
    const Alignment alignment = alignBeats(run.track.beatTimes, run.result.beats, 0.06, 1.0, 19.0);
    CHECK(alignment.total >= 20);
    CHECK(alignment.matched >= static_cast<int>(alignment.total * 0.9));
    CHECK(alignment.meanError < 0.04);
    CHECK(alignment.maxError < 0.08);
}

TEST_CASE(analysis.bpm_123) {
    using namespace bgmedia;
    test::TrackOptions options;
    options.bpm = 123.0;
    options.duration = 20.0;
    const auto run = analyzeTrack(test::makeClickTrack(options));
    CHECK_NEAR(run.result.bpm, 123.0, 1.5);
    CHECK(run.result.bpmConfidence > 0.55f);
    const Alignment alignment = alignBeats(run.track.beatTimes, run.result.beats, 0.06, 1.0, 19.0);
    CHECK(alignment.total >= 25);
    CHECK(alignment.matched >= static_cast<int>(alignment.total * 0.9));
    CHECK(alignment.meanError < 0.04);
}

TEST_CASE(analysis.bpm_163) {
    using namespace bgmedia;
    test::TrackOptions options;
    options.bpm = 163.0;
    options.duration = 20.0;
    const auto run = analyzeTrack(test::makeClickTrack(options));
    CHECK_NEAR(run.result.bpm, 163.0, 2.0);
    CHECK(run.result.bpmConfidence > 0.5f);
    const Alignment alignment = alignBeats(run.track.beatTimes, run.result.beats, 0.06, 1.0, 19.0);
    CHECK(alignment.total >= 30);
    CHECK(alignment.matched >= static_cast<int>(alignment.total * 0.88));
}

TEST_CASE(analysis.straight_tempo_corpus) {
    using namespace bgmedia;
    for (const double expectedBpm : {80.0, 92.0, 100.0, 123.0, 125.0, 163.0}) {
        test::TrackOptions options;
        options.bpm = expectedBpm;
        options.duration = 18.0;
        const auto run = analyzeTrack(test::makeClickTrack(options));
        const double tolerance = std::max(1.5, expectedBpm * 0.02);
        CHECK_NEAR(run.result.bpm, expectedBpm, tolerance);
        const BeatAgreement agreement =
            scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 17.0);
        CHECK(agreement.reference >= 20);
        CHECK(agreement.fMeasure > 0.95);
        CHECK(agreement.meanSignedError < 0.02);
        CHECK(run.result.bpmConfidence > 0.30f);
        CHECK(run.result.bpmLevelConfidence >= 0.0f);
        CHECK(run.result.bpmLevelConfidence <= 1.0f);
    }
}

TEST_CASE(analysis.metric_level_offbeat_is_not_double_time) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeOffbeatTrack(100.0, 18.0));
    CHECK_NEAR(run.result.bpm, 100.0, 2.0);
    CHECK(std::abs(run.result.bpm - 200.0) > 5.0);
    const BeatAgreement agreement =
        scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 17.0);
    CHECK(agreement.fMeasure > 0.95);
    CHECK(agreement.meanSignedError < 0.02);
    const BeatAgreement downbeats =
        scoreDownbeats(run.track.downbeatTimes, run.result.beats, 0.07, 1.0, 17.0);
    CHECK(downbeats.fMeasure > 0.85);
}

TEST_CASE(analysis.metric_level_ternary_is_not_tatum) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeTernaryTrack(66.2, 24.0));
    CHECK_NEAR(run.result.bpm, 66.2, 1.8);
    CHECK(std::abs(run.result.bpm - 198.6) > 5.0);
    const BeatAgreement agreement =
        scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 23.0);
    CHECK(agreement.fMeasure > 0.95);
    CHECK(agreement.meanSignedError < 0.02);
    const BeatAgreement downbeats =
        scoreDownbeats(run.track.downbeatTimes, run.result.beats, 0.07, 1.0, 23.0);
    CHECK(downbeats.fMeasure > 0.85);
    CHECK(run.result.octaveAmbiguous);
}

TEST_CASE(analysis.metric_level_halftime_is_not_double_time) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeHalfTimeTrack(80.0, 18.0));
    CHECK_NEAR(run.result.bpm, 80.0, 1.8);
    CHECK(std::abs(run.result.bpm - 160.0) > 5.0);
    const BeatAgreement agreement =
        scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 17.0);
    CHECK(agreement.fMeasure > 0.95);
    const BeatAgreement downbeats =
        scoreDownbeats(run.track.downbeatTimes, run.result.beats, 0.07, 1.0, 17.0);
    CHECK(downbeats.fMeasure > 0.85);
}

TEST_CASE(analysis.shuffle_and_dense_hats_keep_pulse) {
    using namespace bgmedia;
    const auto shuffle = analyzeTrack(test::makeShuffleTrack(80.0, 18.0));
    CHECK_NEAR(shuffle.result.bpm, 80.0, 2.0);
    CHECK(scoreBeats(shuffle.track.beatTimes, shuffle.result.beats, 0.07, 1.0, 17.0)
              .fMeasure > 0.95);
    const auto dense = analyzeTrack(test::makeDenseHatTrack(99.0, 18.0));
    CHECK_NEAR(dense.result.bpm, 99.0, 2.0);
    CHECK(scoreBeats(dense.track.beatTimes, dense.result.beats, 0.07, 1.0, 17.0)
              .fMeasure > 0.95);
}

TEST_CASE(analysis.drift_tempo_keeps_pulse) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeDriftTrack(80.0, 18.0));
    CHECK_NEAR(run.result.bpm, 80.0, 3.5);
    const BeatAgreement agreement =
        scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 17.0);
    CHECK(agreement.fMeasure > 0.90);
    CHECK(agreement.meanSignedError < 0.025);
}

TEST_CASE(analysis.onset_timestamp_is_not_systematically_early) {
    using namespace bgmedia;
    test::TrackOptions options;
    options.bpm = 123.0;
    options.duration = 18.0;
    const auto run = analyzeTrack(test::makeClickTrack(options));
    const BeatAgreement agreement =
        scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 17.0);
    CHECK(agreement.matched > 20);
    CHECK(std::abs(agreement.meanSignedError) < 0.015);
    CHECK(agreement.p95AbsoluteError < 0.025);
}

TEST_CASE(analysis.syncopated_bass_is_not_forced_onto_beat) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeSyncopatedBassTrack(100.0, 18.0));
    CHECK_NEAR(run.result.bpm, 100.0, 2.0);
    CHECK(run.result.bassAccents.size() >= 4);
    int offbeat = 0;
    for (const BassAccent& accent : run.result.bassAccents) {
        CHECK(accent.confidence >= 0.0f);
        CHECK(accent.confidence <= 1.0f);
        CHECK(accent.strength >= 0.0f);
        if (accent.nearestBeat < 0) {
            ++offbeat;
        }
    }
    CHECK(offbeat >= 1);
}

TEST_CASE(analysis.confidence_channels_are_heuristic_but_bounded) {
    using namespace bgmedia;
    test::TrackOptions options;
    options.bpm = 92.0;
    options.duration = 18.0;
    const auto run = analyzeTrack(test::makeClickTrack(options));
    CHECK(run.result.bpmConfidence >= 0.0f && run.result.bpmConfidence <= 1.0f);
    CHECK(run.result.bpmLevelConfidence >= 0.0f &&
          run.result.bpmLevelConfidence <= 1.0f);
    CHECK(run.result.beatPhaseConfidence >= 0.0f &&
          run.result.beatPhaseConfidence <= 1.0f);
    CHECK(run.result.downbeatConfidence >= 0.0f &&
          run.result.downbeatConfidence <= 1.0f);
    CHECK(run.result.meterConfidence >= 0.0f && run.result.meterConfidence <= 1.0f);
}

TEST_CASE(analysis.tempo_change_reports_transition) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeTempoChangeTrack(92.0, 123.0, 12.0, 24.0));
    CHECK(run.result.hasTempoChange());
    CHECK(!run.result.tempoStable);
    CHECK(run.result.bpmConfidence < 0.8f);
    CHECK(run.result.tempoSegments.size() >= 2);
    CHECK(!run.result.tempoTransitions.empty());

    bool found92 = false;
    bool found123 = false;
    for (const auto& segment : run.result.tempoSegments) {
        if (std::abs(segment.bpm - 92.0) < 2.0) {
            found92 = true;
            CHECK(segment.end < 12.5);
        }
        if (std::abs(segment.bpm - 123.0) < 2.0) {
            found123 = true;
            CHECK(segment.start > 10.5);
        }
    }
    CHECK(found92);
    CHECK(found123);

    bool transitionCoversChange = false;
    for (const auto& transition : run.result.tempoTransitions) {
        if (transition.start <= 12.0 && transition.end >= 12.0) {
            transitionCoversChange = true;
            CHECK(transition.confidence <= 0.5f);
        }
    }
    CHECK(transitionCoversChange);
}

TEST_CASE(analysis.silence_gap_excludes_beats) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeSilenceTrack());
    CHECK_NEAR(run.result.bpm, 92.0, 2.0);
    CHECK(!run.result.silences.empty());
    const auto& silence = run.result.silences.front();
    CHECK_NEAR(silence.start, 4.0, 0.35);
    CHECK_NEAR(silence.end, 9.0, 0.35);
    CHECK(run.result.activeRatio > 0.5);
    CHECK(run.result.activeRatio < 0.75);
    for (const auto& beat : run.result.beats) {
        CHECK(beat.time < silence.start - 0.05 || beat.time > silence.end + 0.05);
    }
}

TEST_CASE(analysis.fully_silent_does_not_invent_music) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeFullySilentTrack(6.0));
    CHECK_NEAR(run.result.bpm, 0.0, 1e-9);
    CHECK(run.result.beats.empty());
    CHECK(run.result.bassAccents.empty());
    CHECK(run.result.activeRatio < 0.01);
    CHECK(!run.result.silences.empty());
}

TEST_CASE(analysis.bass_accents_vs_broadband_swells) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeBassAccentTrack());
    CHECK(run.result.bassAccents.size() >= 5);

    int downbeatsHit = 0;
    for (double downbeat : run.track.downbeatTimes) {
        bool hit = false;
        for (const auto& accent : run.result.bassAccents) {
            if (std::abs(accent.time - downbeat) < 0.08) {
                hit = true;
                CHECK(accent.lowBandRatio > 0.3f);
                CHECK(accent.confidence > 0.5f);
                break;
            }
        }
        if (hit) {
            ++downbeatsHit;
        }
    }
    CHECK(downbeatsHit >= static_cast<int>(run.track.downbeatTimes.size()) - 1);

    // Loud high-frequency swells must not be sold as bass accents.
    int falseSwells = 0;
    for (double swell : run.track.swellTimes) {
        for (const auto& accent : run.result.bassAccents) {
            if (std::abs(accent.time - swell) < 0.09) {
                ++falseSwells;
                break;
            }
        }
    }
    CHECK(falseSwells == 0);

    // A few bass-note attacks between drum hits are legitimate low-frequency
    // events, but the map must not explode into one accent per onset.
    CHECK(run.result.bassAccents.size() <= run.track.downbeatTimes.size() + 4);
}

TEST_CASE(analysis.ambiguous_rhythm_is_low_confidence) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeAmbiguousTrack());
    CHECK(run.result.bpmConfidence < 0.65f);
    CHECK(run.result.bpmCandidates.size() >= 2);
    bool hasSlow = false;
    bool hasFast = false;
    for (double candidate : run.result.bpmCandidates) {
        if (std::abs(candidate - 92.0) < 2.5) {
            hasSlow = true;
        }
        if (std::abs(candidate - 138.0) < 2.5) {
            hasFast = true;
        }
    }
    CHECK(hasSlow);
    CHECK(hasFast);
    CHECK(run.result.octaveAmbiguous);
}

TEST_CASE(analysis.cancellation_is_reported) {
    using namespace bgmedia;
    test::TrackOptions options;
    options.bpm = 92.0;
    options.duration = 12.0;
    const auto track = test::makeClickTrack(options);
    std::string error;
    const PcmBuffer pcm = PcmBuffer::fromAudio(track.audio, &error);
    REQUIRE(pcm.valid());
    Analyzer analyzer;
    int calls = 0;
    AnalysisReport report = analyzer.analyze(
        pcm, {}, [&calls]() { return ++calls > 2; });
    CHECK(report.status == AnalysisStatus::Cancelled);
    CHECK(report.message.find("cancel") != std::string::npos);
}

// ---------------------------------------------------------------------------
// BG Motion 3.1 corpus: fast pulses, compound meters and tempo ramps
// ---------------------------------------------------------------------------

TEST_CASE(analysis.fast_four_on_the_floor_is_not_subharmonic) {
    using namespace bgmedia;
    for (double bpm : {150.0, 180.0}) {
        test::TrackOptions options;
        options.bpm = bpm;
        options.duration = 20.0;
        const auto run = analyzeTrack(test::makeClickTrack(options));
        CHECK_NEAR(run.result.bpm, bpm, 2.0);
        const BeatAgreement agreement =
            scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 19.0);
        CHECK(agreement.fMeasure > 0.90);
        CHECK(agreement.medianAbsMs < 20.0);
        CHECK(agreement.p95AbsMs < 30.0);
        CHECK(run.result.beatsPerBar == 4);
    }
}

TEST_CASE(analysis.compound_meter_6_8_and_12_8) {
    using namespace bgmedia;
    {
        const auto run = analyzeTrack(test::makeCompoundTrack(120.0, 2, 20.0, 3));
        CHECK_NEAR(run.result.bpm, 120.0, 2.0);
        CHECK(run.result.beatsPerBar == 2);
        const BeatAgreement agreement =
            scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 19.0);
        CHECK(agreement.fMeasure > 0.90);
        const BeatAgreement downbeats =
            scoreDownbeats(run.track.downbeatTimes, run.result.beats, 0.07, 1.0, 19.0);
        CHECK(downbeats.fMeasure > 0.80);
        const motion::MusicalTimeline timeline =
            motion::MusicalTimeline::fromAnalysis(run.result);
        CHECK(timeline.subdivision() == 3);
        CHECK(timeline.compound());
        CHECK(timeline.meterLabel().find("6/8") != std::string::npos);
    }
    {
        const auto run = analyzeTrack(test::makeCompoundTrack(90.0, 4, 20.0, 3));
        CHECK_NEAR(run.result.bpm, 90.0, 2.0);
        CHECK(run.result.beatsPerBar == 4);
        const motion::MusicalTimeline timeline =
            motion::MusicalTimeline::fromAnalysis(run.result);
        CHECK(timeline.subdivision() == 3);
        CHECK(timeline.compound());
        CHECK(timeline.meterLabel().find("12/8") != std::string::npos);
    }
}

TEST_CASE(analysis.tempo_ramp_reports_a_piecewise_map) {
    using namespace bgmedia;
    const auto run = analyzeTrack(test::makeTempoRampTrack(80.0, 120.0, 24.0));
    CHECK(!run.result.tempoStable);
    CHECK(run.result.tempoSegments.size() >= 3);
    CHECK(!run.result.tempoTransitions.empty());
    const BeatAgreement agreement =
        scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 23.0);
    CHECK(agreement.fMeasure > 0.90);
    CHECK(agreement.medianAbsMs < 20.0);
    CHECK(run.result.tempoSegments.front().bpm < 95.0);
    CHECK(run.result.tempoSegments.back().bpm > 105.0);
}

TEST_CASE(analysis.strong_hats_and_shifted_accents_keep_the_pulse) {
    using namespace bgmedia;
    {
        const auto run = analyzeTrack(test::makeStrongHatWeakKickTrack(128.0, 20.0));
        CHECK_NEAR(run.result.bpm, 128.0, 2.0);
        const BeatAgreement agreement =
            scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 19.0);
        CHECK(agreement.fMeasure > 0.90);
        CHECK(agreement.medianAbsMs < 20.0);
    }
    {
        const auto run = analyzeTrack(test::makeShiftedAccentTrack(100.0, 20.0));
        CHECK_NEAR(run.result.bpm, 100.0, 2.0);
        const BeatAgreement agreement =
            scoreBeats(run.track.beatTimes, run.result.beats, 0.07, 1.0, 19.0);
        CHECK(agreement.fMeasure > 0.90);
        const BeatAgreement downbeats =
            scoreDownbeats(run.track.downbeatTimes, run.result.beats, 0.07, 1.0, 19.0);
        CHECK(downbeats.fMeasure > 0.80);
    }
}

TEST_CASE(analysis.section_detection_resolves_sustained_energy_changes) {
    using namespace bgmedia;
    SpectralFeatures features;
    features.framesPerSecond = 50.0;
    features.rms.resize(1500, 0.06f);
    // A 15-second quiet intro followed by 15 seconds of sustained music.
    for (std::size_t i = 750; i < features.rms.size(); ++i) {
        features.rms[i] = 0.42f;
    }
    const std::vector<Section> sections = detectSections(features, 30.0, {});
    REQUIRE(sections.size() >= 2);
    CHECK_NEAR(sections.front().start, 0.0, 1e-9);
    CHECK_NEAR(sections.back().end, 30.0, 1e-9);
    bool hasBoundary = false;
    for (const Section& section : sections) {
        if (std::abs(section.end - 15.0) < 1.5) {
            hasBoundary = true;
        }
    }
    CHECK(hasBoundary);

    // A static signal must not invent structural transitions.
    std::fill(features.rms.begin(), features.rms.end(), 0.25f);
    const std::vector<Section> stable = detectSections(features, 30.0, {});
    REQUIRE(stable.size() == 1);
    CHECK_NEAR(stable.front().end, 30.0, 1e-9);

    // A repeating periodic drum pattern is not a new song section on each
    // transient. Repeated accent cycles should not create phantom boundaries.
    for (std::size_t i = 0; i < features.rms.size(); ++i) {
        features.rms[i] = (i % 50 < 10) ? 0.55f : 0.03f;
    }
    const std::vector<Section> periodic = detectSections(features, 30.0, {});
    CHECK(periodic.size() <= 2);
}

TEST_CASE(analysis.bass_accent_uses_relative_local_period_window) {
    using namespace bgmedia;
    SpectralFeatures features;
    features.framesPerSecond = 100.0;
    features.onsetTimeOffset = 0.0;
    features.lowEnv.assign(400, 0.0f);
    features.lowRatio.assign(400, 0.0f);
    features.lowEnergyRatio.assign(400, 0.0f);
    features.lowEnv[116] = 0.4f;
    features.lowEnv[117] = 0.9f;
    features.lowEnv[118] = 0.3f;
    features.lowRatio[117] = 0.75f;
    features.lowEnergyRatio[117] = 0.85f;
    Beat a;
    a.time = 1.0;
    Beat b;
    b.time = 2.0;
    const std::vector<BassAccent> accents =
        detectBassAccents(features, {}, {a, b}, 4.0, {});
    REQUIRE(accents.size() == 1);
    CHECK(accents.front().nearestBeat == 0);
    CHECK_NEAR(accents.front().offsetFromBeat, 0.17, 0.01);
}

TEST_CASE(analysis.regression_refinement_stays_in_configured_bpm_range) {
    using namespace bgmedia;
    constexpr double fps = 100.0;
    constexpr double actualBpm = 200.2;
    std::vector<float> envelope(2000, 0.0f);
    for (double time = 0.4; time < 19.8; time += 60.0 / actualBpm) {
        const double frame = time * fps;
        const int centre = static_cast<int>(std::lround(frame));
        for (int delta = -3; delta <= 3; ++delta) {
            const int index = centre + delta;
            if (index < 0 || index >= static_cast<int>(envelope.size())) {
                continue;
            }
            const double distance = (static_cast<double>(index) - frame) / 1.1;
            envelope[static_cast<std::size_t>(index)] +=
                static_cast<float>(std::exp(-0.5 * distance * distance));
        }
    }
    AnalysisConfig config;
    config.minBpm = 190.0;
    config.maxBpm = 200.0;
    config.preferredBpm = 195.0;
    const TempoEstimate estimate = estimateTempo(envelope, fps, config, 1.0);
    REQUIRE(estimate.bpm > 0.0);
    CHECK(estimate.bpm >= config.minBpm);
    CHECK(estimate.bpm <= config.maxBpm);
    for (const TempoCandidate& candidate : estimate.candidates) {
        CHECK(candidate.bpm >= config.minBpm);
        CHECK(candidate.bpm <= config.maxBpm);
    }
}

TEST_CASE(analysis.periodic_120_bpm_envelope_has_no_phantom_sections) {
    using namespace bgmedia;
    SpectralFeatures features;
    features.framesPerSecond = 100.0;
    features.rms.resize(3000);
    for (std::size_t i = 0; i < features.rms.size(); ++i) {
        const double t = static_cast<double>(i) / features.framesPerSecond;
        const double phase = std::fmod(t, 0.5);
        features.rms[i] = static_cast<float>(
            0.03 + 0.5 * std::exp(-phase / 0.06));
    }
    const auto sections = detectSections(features, 30.0, {});
    CHECK(sections.size() <= 2);
}

TEST_CASE(analysis.issue_40_tempo_regressions_fixed) {
    using namespace bgmedia;
    // 1. Fast four-on-the-floor (180 BPM) should estimate ~180 BPM, not subharmonic ~90 BPM
    {
        test::TrackOptions options;
        options.bpm = 180.0;
        options.duration = 20.0;
        const auto run = analyzeTrack(test::makeClickTrack(options));
        CHECK_NEAR(run.result.bpm, 180.0, 2.0);
        CHECK(run.result.bpm > 140.0);
    }
    // 2. Syncopated bassline at 100 BPM should estimate ~100 BPM, not ~66.8 BPM
    {
        const auto run = analyzeTrack(test::makeSyncopatedBassTrack(100.0, 18.0));
        CHECK_NEAR(run.result.bpm, 100.0, 2.0);
        CHECK(run.result.bpm > 85.0);
    }
    // 3. Offbeat track at 120 BPM should estimate ~120 BPM, not subharmonic ~60 BPM
    {
        const auto run = analyzeTrack(test::makeOffbeatTrack(120.0, 18.0));
        CHECK_NEAR(run.result.bpm, 120.0, 2.0);
        CHECK(run.result.bpm > 100.0);
    }
}
