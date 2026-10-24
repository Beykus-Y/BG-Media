#include "framework.hpp"

#include "bgmedia/analysis/Analyzer.hpp"
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
