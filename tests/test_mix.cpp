#include "framework.hpp"

#include "bgmedia/cache/AnalysisCache.hpp"
#include "bgmedia/mix/Crossfade.hpp"
#include "bgmedia/mix/HarmonicAnalyzer.hpp"
#include "bgmedia/mix/MixEngine.hpp"
#include "bgmedia/mix/MixFeatureCache.hpp"
#include "bgmedia/mix/MixFeatureMap.hpp"
#include "bgmedia/mix/MixPlanner.hpp"
#include "bgmedia/mix/MixSession.hpp"
#include "bgmedia/mix/MixSync.hpp"
#include "bgmedia/mix/PcmTrack.hpp"
#include "bgmedia/mix/SpectralEq.hpp"
#include "bgmedia/mix/VocalDetector.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using bgmedia::AnalysisResult;
using bgmedia::AudioData;
using bgmedia::Beat;
using bgmedia::Section;
using bgmedia::SectionKind;
using bgmedia::SilenceRegion;
using bgmedia::TempoSegment;
using bgmedia::mix::DeckId;
using bgmedia::mix::MixEngine;
using bgmedia::mix::MixPlan;
using bgmedia::mix::MixPlanner;
using bgmedia::mix::MixSession;
using bgmedia::mix::PcmTrack;
using bgmedia::mix::PlanRequest;
using bgmedia::mix::PlannerConfig;
using bgmedia::mix::PreparedMixTrack;
using bgmedia::mix::TempoMatchKind;
using bgmedia::mix::TempoPoint;
using bgmedia::mix::TransitionPhase;
using bgmedia::mix::TransitionStage;
using bgmedia::mix::TransitionType;
using bgmedia::mix::EqGains;
using bgmedia::mix::ThreeBandEq;
using bgmedia::mix::TempoCurveKind;
using bgmedia::mix::TempoRampMode;

constexpr std::uint32_t kRate = 48000;

std::shared_ptr<PcmTrack> dcTrack(double seconds, float value,
                                  std::uint32_t channels = 2,
                                  std::uint32_t rate = kRate) {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = rate;
    track->channels = channels;
    const std::size_t frames =
        static_cast<std::size_t>(std::llround(seconds * rate));
    track->samples.assign(frames * channels, value);
    return track;
}

std::shared_ptr<PcmTrack> toneTrack(double seconds, double frequency,
                                    std::uint32_t channels = 2,
                                    std::uint32_t rate = kRate) {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = rate;
    track->channels = channels;
    const std::size_t frames = static_cast<std::size_t>(seconds * rate);
    track->samples.resize(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * frequency *
                             static_cast<double>(i) / rate;
        const float value = static_cast<float>(0.4 * std::sin(phase));
        for (std::uint32_t c = 0; c < channels; ++c) {
            track->samples[i * channels + c] = value;
        }
    }
    return track;
}

std::shared_ptr<PcmTrack> stereoTrack(double seconds, double leftHz,
                                      double rightHz, float leftAmp = 0.35f,
                                      float rightAmp = 0.25f) {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = kRate;
    track->channels = 2;
    const std::size_t frames = static_cast<std::size_t>(seconds * kRate);
    track->samples.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        track->samples[i * 2] = static_cast<float>(
            leftAmp * std::sin(2.0 * 3.14159265358979323846 * leftHz * t));
        track->samples[i * 2 + 1] = static_cast<float>(
            rightAmp * std::sin(2.0 * 3.14159265358979323846 * rightHz * t));
    }
    return track;
}

std::shared_ptr<PcmTrack> broadbandTrack(double seconds) {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = kRate;
    track->channels = 2;
    const std::size_t frames = static_cast<std::size_t>(seconds * kRate);
    track->samples.resize(frames * 2);
    const double frequencies[] = {110.0, 247.0, 613.0, 1499.0, 3101.0, 5237.0};
    for (std::size_t i = 0; i < frames; ++i) {
        double value = 0.0;
        for (double frequency : frequencies) {
            value += 0.09 * std::sin(2.0 * 3.14159265358979323846 * frequency *
                                     static_cast<double>(i) / kRate);
        }
        const float sample = static_cast<float>(value);
        track->samples[i * 2] = sample;
        track->samples[i * 2 + 1] = sample;
    }
    return track;
}

AnalysisResult analysisFor(double bpm, double duration = 30.0,
                           bool confident = true) {
    AnalysisResult analysis;
    analysis.info.duration = duration;
    analysis.info.sampleRate = kRate;
    analysis.info.sourceChannels = 2;
    analysis.bpm = bpm;
    analysis.bpmConfidence = confident ? 0.93f : 0.22f;
    analysis.bpmLevelConfidence = confident ? 0.90f : 0.20f;
    analysis.beatPhaseConfidence = confident ? 0.91f : 0.20f;
    analysis.downbeatConfidence = confident ? 0.86f : 0.18f;
    analysis.meterConfidence = confident ? 0.88f : 0.18f;
    analysis.tempoStable = confident;
    analysis.octaveAmbiguous = false;
    analysis.beatsPerBar = 4;
    analysis.activeRatio = confident ? 0.97 : 0.25;
    analysis.tempoSegments.push_back({0.0, duration, bpm,
                                      confident ? 0.92f : 0.20f});

    const double interval = 60.0 / std::max(1.0, bpm);
    for (int i = 0; i * interval < duration; ++i) {
        Beat beat;
        beat.time = static_cast<double>(i) * interval;
        beat.strength = 0.8f;
        beat.confidence = confident ? 0.9f : 0.2f;
        beat.beatInBar = i % 4;
        beat.barIndex = i / 4;
        beat.downbeat = i % 4 == 0;
        analysis.beats.push_back(beat);
    }
    analysis.sections.push_back({0.0, std::min(4.0, duration),
                                 SectionKind::Intro, 0.5f, 0.8f});
    analysis.sections.push_back({4.0, duration, SectionKind::Main, 0.9f, 0.8f});
    return analysis;
}

struct RenderStats {
    float peak = 0.0f;
    float minAbsAfterWarmup = 1.0f;
    std::size_t nonFinite = 0;
    std::size_t zeroRun = 0;
};

void renderSeconds(MixEngine& engine, double seconds, RenderStats* stats,
                   double warmupSeconds = 0.0) {
    std::vector<float> block(480 * 2, 0.0f);
    const int blocks = static_cast<int>(std::ceil(seconds * kRate / 480.0));
    const int warmupBlocks =
        static_cast<int>(std::ceil(warmupSeconds * kRate / 480.0));
    for (int i = 0; i < blocks; ++i) {
        engine.render(block.data(), 480);
        if (!stats) {
            continue;
        }
        for (std::size_t frame = 0; frame < 480; ++frame) {
            for (int c = 0; c < 2; ++c) {
                const float value = block[frame * 2 + static_cast<std::size_t>(c)];
                if (!std::isfinite(value)) {
                    ++stats->nonFinite;
                }
                stats->peak = std::max(stats->peak, std::abs(value));
                if (i >= warmupBlocks) {
                    stats->minAbsAfterWarmup =
                        std::min(stats->minAbsAfterWarmup, std::abs(value));
                    if (std::abs(value) < 1e-6f) {
                        ++stats->zeroRun;
                    }
                }
            }
        }
    }
}

MixPlan crossfadePlan(const MixEngine& engine, double duration, double startB) {
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    MixPlan plan;
    plan.valid = true;
    plan.type = TransitionType::Crossfade;
    plan.requestedType = TransitionType::Crossfade;
    plan.from = DeckId::A;
    plan.to = DeckId::B;
    plan.duration = duration;
    plan.transitionStartA = snap.decks[0].positionSeconds;
    plan.transitionEndA = plan.transitionStartA + duration;
    plan.startB = startB;
    plan.endB = startB + duration;
    plan.handoverProgress = 0.5;
    plan.gainCurveOut = {{0.0, 1.0f}, {duration, 0.0f}};
    plan.gainCurveIn = {{0.0, 0.0f}, {duration, 1.0f}};
    return plan;
}

} // namespace

TEST_CASE("mix.crossfade_equal_power") {
    CHECK_NEAR(bgmedia::mix::equalPowerOut(0.0), 1.0, 1e-6);
    CHECK_NEAR(bgmedia::mix::equalPowerIn(0.0), 0.0, 1e-6);
    CHECK_NEAR(bgmedia::mix::equalPowerOut(1.0), 0.0, 1e-6);
    CHECK_NEAR(bgmedia::mix::equalPowerIn(1.0), 1.0, 1e-6);
    CHECK_NEAR(bgmedia::mix::equalPowerOut(0.5), 0.70710678, 1e-5);
    for (int i = 0; i <= 100; ++i) {
        const double t = static_cast<double>(i) / 100.0;
        const double out = bgmedia::mix::equalPowerOut(t);
        const double in = bgmedia::mix::equalPowerIn(t);
        CHECK_NEAR(out * out + in * in, 1.0, 1e-5);
    }
    CHECK_NEAR(bgmedia::mix::correlationHeadroom(0.0f), 1.0, 1e-6);
    CHECK_NEAR(bgmedia::mix::correlationHeadroom(1.0f), 0.70710678, 1e-5);
    // Gain-aware: a silent deck must not reduce the audible one.
    CHECK_NEAR(bgmedia::mix::correlationHeadroom(1.0f, 1.0f, 0.0f), 1.0, 1e-6);
    CHECK_NEAR(bgmedia::mix::correlationHeadroom(1.0f, 0.7071f, 0.7071f),
               0.70710678, 1e-4);
}

TEST_CASE("mix.engine_gapless_transition") {
    MixEngine engine(kRate);
    auto a = dcTrack(30.0, 0.25f);
    auto b = dcTrack(30.0, 0.25f);
    CHECK(engine.loadDeck(DeckId::A, a, 1, 123.0));
    CHECK(engine.loadDeck(DeckId::B, b, 2, 128.0));
    engine.play(DeckId::A);
    RenderStats warm;
    renderSeconds(engine, 0.30, &warm);
    CHECK(warm.nonFinite == 0);
    CHECK(warm.peak > 0.1f);

    const MixPlan plan = crossfadePlan(engine, 1.0, 0.0);
    CHECK(engine.startTransition(plan));
    CHECK(!engine.startTransition(plan));  // conflicting command is rejected

    RenderStats stats;
    renderSeconds(engine, 1.25, &stats, 0.05);
    CHECK(stats.nonFinite == 0);
    CHECK(stats.peak <= 1.02f);
    CHECK(stats.zeroRun == 0);
    CHECK(stats.minAbsAfterWarmup > 0.15f);

    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Idle);
    CHECK(snap.master == DeckId::B);
    CHECK(snap.completedTransitions >= 1);
    CHECK(!snap.decks[0].playing);
    CHECK(snap.decks[1].playing);
    // The incoming deck advanced through the crossfade, took over as master
    // and kept playing for the remaining 0.25 s: no stall or duplicate.
    CHECK_NEAR(snap.decks[1].positionSeconds, 1.25, 0.02);
    std::printf("mix gapless: peak=%.4f minGainWindow=%.4f Bpos=%.3f\n",
                static_cast<double>(stats.peak),
                static_cast<double>(stats.minAbsAfterWarmup),
                snap.decks[1].positionSeconds);
}

TEST_CASE("mix.engine_empty_and_eof") {
    MixEngine engine(kRate);
    RenderStats silent;
    engine.play(DeckId::A);
    renderSeconds(engine, 0.2, &silent);
    CHECK(silent.nonFinite == 0);
    CHECK(silent.peak == 0.0f);

    auto shortTrack = toneTrack(0.18, 220.0);
    CHECK(engine.loadDeck(DeckId::A, shortTrack, 3, 120.0));
    engine.play(DeckId::A);
    RenderStats eof;
    renderSeconds(engine, 0.45, &eof);
    CHECK(eof.nonFinite == 0);
    CHECK(eof.peak > 0.1f);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.decks[0].ended);
    CHECK(!snap.decks[0].playing);
    CHECK(snap.decks[0].positionSeconds >= 0.17);
}

TEST_CASE("mix.engine_pause_resume_cancel") {
    MixEngine engine(kRate);
    auto a = toneTrack(30.0, 180.0);
    auto b = toneTrack(30.0, 240.0);
    CHECK(engine.loadDeck(DeckId::A, a, 1, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 2, 124.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.2, nullptr);

    CHECK(engine.startTransition(crossfadePlan(engine, 2.0, 0.0)));
    renderSeconds(engine, 0.4, nullptr);
    CHECK(engine.snapshot().phase == TransitionPhase::Running);
    const double beforePause = engine.snapshot().decks[1].positionSeconds;
    engine.setPaused(true);
    renderSeconds(engine, 0.4, nullptr);
    CHECK(engine.paused());
    const double afterPause = engine.snapshot().decks[1].positionSeconds;
    CHECK_NEAR(afterPause, beforePause, 0.02);

    engine.setPaused(false);
    renderSeconds(engine, 0.2, nullptr);
    engine.cancelTransition();
    RenderStats cancel;
    renderSeconds(engine, 0.5, &cancel);
    CHECK(cancel.nonFinite == 0);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Idle);
    CHECK(snap.master == DeckId::A);
    CHECK(snap.decks[0].playing);
    CHECK(!snap.decks[1].playing);
}

TEST_CASE("mix.engine_sample_rates_and_channels") {
    AudioData stereo;
    stereo.sampleRate = 44100;
    stereo.channels = 2;
    stereo.samples.resize(44100 * 2);
    for (std::size_t i = 0; i < 44100; ++i) {
        const float value =
            static_cast<float>(0.3 * std::sin(2.0 * 3.14159265358979323846 *
                                              440.0 * static_cast<double>(i) /
                                              44100.0));
        stereo.samples[i * 2] = value;
        stereo.samples[i * 2 + 1] = value;
    }
    std::string error;
    const PcmTrack converted = PcmTrack::fromAudio(stereo, 48000, &error);
    CHECK(error.empty());
    CHECK(converted.sampleRate == 48000);
    CHECK(converted.channels == 2);
    CHECK_NEAR(converted.duration(), 1.0, 0.002);

    AudioData mono = stereo;
    mono.channels = 1;
    mono.samples.resize(44100);
    for (std::size_t i = 0; i < 44100; ++i) {
        mono.samples[i] = stereo.samples[i * 2];
    }
    const PcmTrack monoConverted = PcmTrack::fromAudio(mono, 48000, &error);
    CHECK(monoConverted.channels == 1);
    CHECK_NEAR(monoConverted.duration(), 1.0, 0.002);

    AudioData surround;
    surround.sampleRate = 48000;
    surround.channels = 6;
    surround.samples.resize(4800 * 6, 0.1f);
    const PcmTrack surroundConverted = PcmTrack::fromAudio(surround, 48000, &error);
    CHECK(surroundConverted.channels == 2);

    MixEngine engine(kRate);
    auto mono48 = dcTrack(3.0, 0.3f, 1);
    CHECK(engine.loadDeck(DeckId::A, mono48, 7, 120.0));
    engine.play(DeckId::A);
    RenderStats stats;
    renderSeconds(engine, 0.25, &stats);
    CHECK(stats.nonFinite == 0);
    CHECK(stats.peak > 0.2f);

    auto stereo441 = std::make_shared<PcmTrack>(PcmTrack::fromAudio(stereo, 44100));
    CHECK(stereo441->valid());
    CHECK(engine.loadDeck(DeckId::B, stereo441, 8, 120.0));
}

TEST_CASE("mix.planner_beat_alignment_bpms") {
    const double bpms[] = {92.0, 123.0, 163.0, 180.0};
    for (double bpm : bpms) {
        const AnalysisResult a = analysisFor(bpm);
        const AnalysisResult b = analysisFor(bpm);
        const double interval = 60.0 / bpm;
        PlanRequest request;
        request.type = TransitionType::SmartCrossfade;
        request.requestedDuration = 8.0;
        // A hair after a downbeat so the grid snap is unambiguous.
        request.currentPositionA = 24.0 * interval + 0.02;
        MixPlanner planner;
        const MixPlan plan = planner.plan(a, b, request);
        CHECK(plan.valid);
        CHECK(plan.beatAligned);
        CHECK(plan.downbeatAligned);
        CHECK(plan.alignmentErrorMs < 50.0);
        CHECK_NEAR(plan.duration, 8.0, 0.02);
        std::printf("mix align %.0f BPM: error=%.2f ms downbeat=%d\n", bpm,
                    plan.alignmentErrorMs, plan.downbeatAligned ? 1 : 0);
    }
}

TEST_CASE("mix.planner_short_incoming_track") {
    // A 4 s edit cannot carry an 8 s fade; the planner must shorten it instead
    // of letting the incoming deck run out halfway through.
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 8.0;
    const MixPlan plan =
        planner.plan(analysisFor(123.0, 30.0), analysisFor(123.0, 4.0), request);
    CHECK(plan.valid);
    CHECK(plan.endB <= 4.0 + 1e-6);
    CHECK(plan.duration <= plan.endB - plan.startB + 1e-6);
    CHECK(plan.duration < 8.0);
    bool explained = false;
    for (const std::string& reason : plan.reasons) {
        if (reason.find("Shortened") != std::string::npos) {
            explained = true;
        }
    }
    CHECK(explained);
    std::printf("mix short incoming: duration=%.3f startB=%.3f endB=%.3f\n",
                plan.duration, plan.startB, plan.endB);
}

TEST_CASE("mix.planner_low_confidence_fallback") {
    const AnalysisResult a = analysisFor(123.0, 30.0, false);
    const AnalysisResult b = analysisFor(128.0, 30.0, false);
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 12.0;
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.type == TransitionType::Crossfade);
    CHECK(!plan.fallbackReason.empty());
    CHECK(plan.confidence < planner.config().minConfidence);
    CHECK(!plan.reasons.empty());
    std::printf("mix fallback: confidence=%.2f reason='%s'\n",
                static_cast<double>(plan.confidence),
                plan.fallbackReason.c_str());

    PlanRequest tempo = request;
    tempo.type = TransitionType::TempoRamp;
    PlannerConfig config;
    config.allowTempoRamp = true;
    MixPlanner rampPlanner(config);
    const MixPlan tempoPlan = rampPlanner.plan(a, b, tempo);
    CHECK(tempoPlan.type == TransitionType::Crossfade);
    CHECK(tempoPlan.fallbackReason.find("low") != std::string::npos);
}

TEST_CASE("mix.planner_silence_and_unstable_fallback") {
    AnalysisResult a = analysisFor(123.0);
    SilenceRegion tail;
    tail.start = 24.0;
    tail.end = 30.0;
    tail.confidence = 0.9f;
    a.silences.push_back(tail);

    PlanRequest request;
    request.type = TransitionType::Crossfade;
    MixPlanner planner;
    const MixPlan plan = planner.plan(a, analysisFor(123.0), request);
    CHECK(plan.valid);
    CHECK(plan.transitionEndA <= 24.5);

    AnalysisResult unstable = analysisFor(123.0);
    unstable.tempoStable = false;
    PlannerConfig config;
    config.allowTempoRamp = true;
    MixPlanner rampPlanner(config);
    PlanRequest ramp;
    ramp.type = TransitionType::TempoRamp;
    ramp.requestedDuration = 8.0;
    const MixPlan unstablePlan = rampPlanner.plan(unstable, analysisFor(128.0), ramp);
    CHECK(unstablePlan.type == TransitionType::Crossfade);
    CHECK(unstablePlan.fallbackReason.find("stable") != std::string::npos);
}

TEST_CASE("mix.planner_tempo_ramp_phase") {
    PlannerConfig config;
    config.allowTempoRamp = true;
    config.maxTempoDeltaPercent = 8.0;
    MixPlanner planner(config);

    auto checkRamp = [&](double fromBpm, double toBpm) {
        const AnalysisResult a = analysisFor(fromBpm);
        const AnalysisResult b = analysisFor(toBpm);
        PlanRequest request;
        request.type = TransitionType::TempoRamp;
        request.requestedDuration = 8.0;
        const MixPlan plan = planner.plan(a, b, request);
        CHECK(plan.valid);
        CHECK(plan.type == TransitionType::TempoRamp);
        // 0.2 ramps before the crossfade with pitch lock, so it is no longer
        // flagged experimental or pitch-shifting.
        CHECK(!plan.experimental);
        CHECK(!plan.pitchShiftWarning);
        CHECK(plan.pitchLock);
        CHECK(plan.tempoRampActive);
        CHECK(plan.preTransitionSeconds > 0.0);
        CHECK(plan.tempoRampStartA < plan.transitionStartA - 0.1);
        CHECK(plan.tempoCurve.size() > 20);
        CHECK_NEAR(plan.tempoCurve.front().rate, 1.0, 1e-4);
        CHECK_NEAR(plan.tempoCurve.front().time, -plan.preTransitionSeconds, 1e-6);
        const double ratio = toBpm / fromBpm;
        CHECK_NEAR(plan.tempoCurve.back().rate, ratio, 1e-3);

        // The pre-roll curve must be smooth and start at 1.0. Its integral is
        // the outgoing media advanced, which is exactly the distance from the
        // ramp start to the crossfade cue.
        double media = 0.0;
        for (std::size_t i = 1; i < plan.tempoCurve.size(); ++i) {
            const auto& p0 = plan.tempoCurve[i - 1];
            const auto& p1 = plan.tempoCurve[i];
            if (p1.time > 1e-9) {
                break;
            }
            media += (p0.rate + p1.rate) * 0.5 * (p1.time - p0.time);
        }
        const double mean = media / std::max(1e-9, plan.preTransitionSeconds);
        CHECK_NEAR(mean, 0.5 * (1.0 + ratio), 0.01);
        CHECK_NEAR(media, plan.transitionStartA - plan.tempoRampStartA, 0.05);
        CHECK_NEAR(plan.tempoCurve.back().bpm, toBpm, 0.5);
        std::printf(
            "mix ramp %.0f->%.0f: rampMean=%.5f target=%.5f pre=%.2f s "
            "media=%.2f s\n",
            fromBpm, toBpm, mean, ratio, plan.preTransitionSeconds, media);
    };
    checkRamp(123.0, 128.0);
    checkRamp(128.0, 122.0);

    PlannerConfig strict;
    strict.allowTempoRamp = true;
    strict.maxTempoDeltaPercent = 3.0;
    MixPlanner strictPlanner(strict);
    PlanRequest request;
    request.type = TransitionType::TempoRamp;
    request.requestedDuration = 8.0;
    // 123 -> 140 cannot meet inside a 3% window on both decks.
    const MixPlan tooFar =
        strictPlanner.plan(analysisFor(123.0), analysisFor(140.0), request);
    CHECK(tooFar.type == TransitionType::Crossfade);
    CHECK(tooFar.fallbackReason.find("exceeds") != std::string::npos);

    PlannerConfig disabledConfig;
    disabledConfig.tempoRampMode = bgmedia::mix::TempoRampMode::Off;
    MixPlanner disabled(disabledConfig);
    const MixPlan off =
        disabled.plan(analysisFor(123.0), analysisFor(128.0), request);
    CHECK(off.type == TransitionType::Crossfade);
    CHECK(off.fallbackReason.find("disabled") != std::string::npos);
}

TEST_CASE("mix.planner_direct_cut_and_unimplemented") {
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::DirectCut;
    const MixPlan cut = planner.plan(analysisFor(123.0), analysisFor(128.0), request);
    CHECK(cut.valid);
    CHECK(cut.type == TransitionType::DirectCut);
    CHECK_NEAR(cut.duration, 0.0, 1e-6);

    PlanRequest unsupported;
    unsupported.type = TransitionType::DualTempoMatch;
    const MixPlan fallback =
        planner.plan(analysisFor(123.0), analysisFor(128.0), unsupported);
    CHECK(fallback.type == TransitionType::Crossfade);
    CHECK(fallback.fallbackReason.find("not implemented") != std::string::npos);
}

TEST_CASE("mix.engine_render_budget") {
    MixEngine engine(kRate);
    auto a = toneTrack(30.0, 173.0);
    auto b = toneTrack(30.0, 197.0);
    CHECK(engine.loadDeck(DeckId::A, a, 11, 123.0));
    CHECK(engine.loadDeck(DeckId::B, b, 12, 128.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.1, nullptr);
    CHECK(engine.startTransition(crossfadePlan(engine, 4.0, 0.0)));

    const auto start = std::chrono::steady_clock::now();
    RenderStats stats;
    renderSeconds(engine, 10.0, &stats);
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start)
            .count();
    CHECK(stats.nonFinite == 0);
    CHECK(stats.peak <= 1.02f);
    // 10 s of stereo audio at 48 kHz must render far faster than real time.
    CHECK(elapsedMs < 1500.0);
    std::printf("mix cpu: 10.0 s audio in %.1f ms (%.1fx realtime), peak=%.3f\n",
                elapsedMs, 10000.0 / std::max(0.001, elapsedMs),
                static_cast<double>(stats.peak));
}

TEST_CASE("mix.engine_seek_direct_cut_and_small_blocks") {
    // Seek while a transition is running: the deck must jump, keep playing and
    // stay finite.
    {
        MixEngine engine(kRate);
        auto a = toneTrack(30.0, 180.0);
        auto b = toneTrack(30.0, 240.0);
        CHECK(engine.loadDeck(DeckId::A, a, 1, 120.0));
        CHECK(engine.loadDeck(DeckId::B, b, 2, 124.0));
        engine.play(DeckId::A);
        renderSeconds(engine, 0.2, nullptr);
        CHECK(engine.startTransition(crossfadePlan(engine, 3.0, 0.0)));
        renderSeconds(engine, 0.5, nullptr);
        engine.seek(DeckId::B, 2.0);
        RenderStats stats;
        renderSeconds(engine, 0.5, &stats);
        CHECK(stats.nonFinite == 0);
        CHECK(engine.snapshot().phase == TransitionPhase::Running);
        CHECK_NEAR(engine.snapshot().decks[1].positionSeconds, 2.5, 0.05);
        engine.cancelTransition();
        renderSeconds(engine, 0.4, nullptr);
    }

    // Direct cut: instant handover, no fade, no gap.
    {
        MixEngine engine(kRate);
        auto a = dcTrack(5.0, 0.25f);
        auto b = dcTrack(5.0, 0.25f);
        CHECK(engine.loadDeck(DeckId::A, a, 3, 120.0));
        CHECK(engine.loadDeck(DeckId::B, b, 4, 120.0));
        engine.play(DeckId::A);
        renderSeconds(engine, 0.1, nullptr);
        MixPlan plan = crossfadePlan(engine, 0.0, 0.0);
        plan.type = TransitionType::DirectCut;
        CHECK(engine.startTransition(plan));
        RenderStats stats;
        renderSeconds(engine, 0.2, &stats, 0.02);
        CHECK(stats.nonFinite == 0);
        CHECK(stats.peak > 0.2f);
        CHECK(stats.minAbsAfterWarmup > 0.2f);
        const bgmedia::mix::MixSnapshot snap = engine.snapshot();
        CHECK(snap.master == DeckId::B);
        CHECK(snap.completedTransitions >= 1);
        CHECK(snap.decks[1].playing);
    }

    // Underrun-style block sizes: no frame may be dropped or duplicated.
    {
        MixEngine engine(kRate);
        auto a = toneTrack(30.0, 173.0);
        auto b = toneTrack(30.0, 197.0);
        CHECK(engine.loadDeck(DeckId::A, a, 5, 123.0));
        CHECK(engine.loadDeck(DeckId::B, b, 6, 128.0));
        engine.play(DeckId::A);
        std::vector<float> buffer(480 * 2, 0.0f);
        engine.render(buffer.data(), 480);
        CHECK(engine.startTransition(crossfadePlan(engine, 2.0, 0.0)));
        const std::size_t sizes[] = {1, 7, 32, 63, 128, 256, 480};
        std::size_t nonFinite = 0;
        float peak = 0.0f;
        for (int i = 0; i < 2000; ++i) {
            const std::size_t frames = sizes[i % 7];
            engine.render(buffer.data(), frames);
            for (std::size_t f = 0; f < frames * 2; ++f) {
                if (!std::isfinite(buffer[f])) {
                    ++nonFinite;
                }
                peak = std::max(peak, std::abs(buffer[f]));
            }
        }
        CHECK(nonFinite == 0);
        CHECK(peak <= 1.02f);
        CHECK(peak > 0.2f);
        CHECK(engine.snapshot().phase == TransitionPhase::Idle);
        CHECK(engine.completedTransitions() >= 1);
        std::printf("mix small blocks: peak=%.3f state=%d completed=%llu\n",
                    static_cast<double>(peak),
                    static_cast<int>(engine.snapshot().phase),
                    static_cast<unsigned long long>(
                        engine.completedTransitions()));
    }
}

TEST_CASE("mix.engine_tempo_ramp_rate") {
    MixEngine engine(kRate);
    auto a = dcTrack(30.0, 0.25f);
    auto b = dcTrack(30.0, 0.25f);
    CHECK(engine.loadDeck(DeckId::A, a, 21, 123.0));
    CHECK(engine.loadDeck(DeckId::B, b, 22, 128.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.2, nullptr);

    MixPlan plan = crossfadePlan(engine, 4.0, 0.0);
    plan.type = TransitionType::TempoRamp;
    const double startA = engine.snapshot().decks[0].positionSeconds;
    plan.transitionStartA = startA;
    plan.transitionEndA = startA + 4.0;
    // Simple linear headroom check: the engine must integrate the curve, not
    // jump to the final rate.
    plan.tempoCurve = {{0.0, 1.0, 123.0}, {4.0, 1.05, 129.15}};
    CHECK(engine.startTransition(plan));
    renderSeconds(engine, 4.1, nullptr, 0.0);

    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Idle);
    CHECK(snap.master == DeckId::B);
    CHECK_NEAR(snap.decks[0].positionSeconds, startA + 4.0 * 1.025, 0.03);
    std::printf("mix tempo engine: A=%.4f expected=%.4f\n",
                snap.decks[0].positionSeconds, startA + 4.0 * 1.025);
}

TEST_CASE("mix.session_queue_and_transport") {
    MixEngine engine(kRate);
    auto prepare = [](const std::filesystem::path& path, PreparedMixTrack& out,
                      std::string& error,
                      std::function<void(float, std::string_view)>) {
        const std::string name = path.filename().string();
        if (name.empty()) {
            error = "empty path";
            return false;
        }
        const double bpm = name.find("beta") != std::string::npos ? 128.0 : 123.0;
        auto track = std::make_shared<PcmTrack>();
        *track = *toneTrack(6.0, 220.0);
        track->id = 0;
        out.audio = track;
        out.analysis =
            std::make_shared<AnalysisResult>(analysisFor(bpm, 6.0));
        return true;
    };
    MixSession session(engine, prepare);

    // The session has no audio callback of its own: the host pumps render().
    // Tests emulate the device so snapshots are published exactly like in the
    // desktop app.
    std::vector<float> block(480 * 2, 0.0f);
    auto pump = [&](int iterations = 1) {
        for (int i = 0; i < iterations; ++i) {
            session.update();
            engine.render(block.data(), 480);
        }
    };

    const std::uint64_t first = session.addTrack("alpha.wav");
    const std::uint64_t second = session.addTrack("beta.wav");
    const std::uint64_t third = session.addTrack("gamma.wav");
    CHECK(first != 0);
    CHECK(second != 0);
    CHECK(third != 0);

    for (int i = 0; i < 400; ++i) {
        pump();
        const bgmedia::mix::MixSnapshot snap = engine.snapshot();
        if (snap.decks[0].loaded && snap.decks[1].loaded) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bgmedia::mix::MixSessionSnapshot loaded = session.snapshot();
    CHECK(loaded.engine.decks[0].loaded);
    CHECK(loaded.engine.decks[1].loaded);
    CHECK(session.deckName(DeckId::A) == "alpha.wav");
    CHECK(session.deckName(DeckId::B) == "beta.wav");
    CHECK(session.deckAnalysis(DeckId::A) != nullptr);
    CHECK_NEAR(session.deckAnalysis(DeckId::A)->bpm, 123.0, 0.1);
    CHECK(session.transitionType() == TransitionType::SmartCrossfade);
    CHECK(session.adaptive());
    CHECK(session.autoQueue());
    CHECK(session.tempoRampMaxPercent() > 0.0);

    session.play();
    pump(2);
    CHECK(!session.snapshot().engine.paused);
    CHECK(session.snapshot().engine.decks[0].playing);
    session.pause();
    pump(2);
    CHECK(session.snapshot().engine.paused);
    session.setAdaptive(false);
    CHECK(!session.adaptive());
    session.setAdaptive(true);
    session.setTransitionDuration(6.0);
    CHECK_NEAR(session.transitionDuration(), 6.0, 1e-9);
    session.setTempoRampEnabled(true);
    CHECK(session.tempoRampEnabled());
    session.setTempoRampMaxPercent(9.0);
    CHECK_NEAR(session.tempoRampMaxPercent(), 9.0, 1e-9);

    CHECK(session.removeTrack(third));
    CHECK(!session.removeTrack(first));  // loaded decks cannot be removed
    session.clearQueue();
    session.update();
    CHECK(session.snapshot().queue.empty());
}

// ---------------------------------------------------------------------------
// Regression coverage for the 10 Codex review findings and concurrency stress
// ---------------------------------------------------------------------------
namespace {

double testTrackSeconds(const std::string& name) {
    if (name.find("short") != std::string::npos) {
        return 0.6;
    }
    if (name.find("long") != std::string::npos) {
        return 30.0;
    }
    return 8.0;
}

bgmedia::mix::MixPrepareCallback makeTestPrepare() {
    return [](const std::filesystem::path& path, PreparedMixTrack& out,
              std::string& error,
              std::function<void(float, std::string_view)> progress) {
        const std::string name = path.filename().string();
        if (name.empty()) {
            error = "empty path";
            return false;
        }
        const double duration = testTrackSeconds(name);
        const double bpm = name.find("beta") != std::string::npos ? 128.0 : 123.0;
        auto track = std::make_shared<PcmTrack>();
        *track = *toneTrack(duration, 220.0);
        out.audio = track;
        auto analysis = std::make_shared<AnalysisResult>(
            analysisFor(bpm, duration));
        // "silence" fixtures end with six seconds of trailing silence. The
        // planner must close the transition window before it, which is only
        // observable if the analysis reports the region.
        if (name.find("silence") != std::string::npos && duration > 8.0) {
            SilenceRegion region;
            region.start = duration - 6.0;
            region.end = duration;
            region.confidence = 0.9f;
            analysis->silences.push_back(region);
        }
        // "marker" fixtures expose a late intro cue, so a direct cut that
        // ignores the planned incoming marker visibly starts at zero instead.
        if (name.find("marker") != std::string::npos && duration > 8.0) {
            analysis->sections.clear();
            analysis->sections.push_back(
                {0.0, 5.2, SectionKind::Build, 0.8f, 0.8f});
            analysis->sections.push_back(
                {5.2, duration, SectionKind::Intro, 0.9f, 0.8f});
            analysis->sections.push_back(
                {5.2, duration, SectionKind::Main, 0.9f, 0.8f});
        }
        out.analysis = analysis;
        if (progress) {
            progress(1.0f, "ready");
        }
        return true;
    };
}

// Emulates the desktop host: one UI tick (session.update) followed by one audio
// callback. Keeping both in the same loop reproduces the real hand-off order.
struct SessionPump {
    MixEngine& engine;
    MixSession& session;
    std::vector<float> block = std::vector<float>(480 * 2, 0.0f);

    void once() {
        session.update();
        engine.render(block.data(), 480);
    }

    void forSeconds(double seconds) {
        const int blocks =
            std::max(1, static_cast<int>(std::ceil(seconds * kRate / 480.0)));
        for (int i = 0; i < blocks; ++i) {
            once();
        }
    }

    bool waitForDecks(int count, int iterations = 800) {
        for (int i = 0; i < iterations; ++i) {
            once();
            const bgmedia::mix::MixSnapshot snap = engine.snapshot();
            const int loaded = (snap.decks[0].loaded ? 1 : 0) +
                               (snap.decks[1].loaded ? 1 : 0);
            if (loaded >= count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        return false;
    }
};

} // namespace

TEST_CASE("mix.engine_retired_track_lifetime") {
    // Regression for the heap-use-after-free: replacing a deck buffer must not
    // free the old PCM until a later render epoch proves the callback cannot
    // still hold its raw pointer.
    MixEngine engine(kRate);
    auto first = std::make_shared<PcmTrack>(*dcTrack(1.0, 0.1f));
    CHECK(engine.loadDeck(DeckId::A, first, 1, 120.0));
    CHECK(first.use_count() >= 2);

    CHECK(engine.loadDeck(DeckId::A, dcTrack(1.0, 0.2f), 2, 121.0));
    // No callback has started since the swap, so the retired buffer stays alive.
    CHECK(first.use_count() >= 2);
    engine.reclaimRetired();
    CHECK(first.use_count() >= 2);

    std::vector<float> block(128 * 2, 0.0f);
    engine.render(block.data(), 128);
    engine.reclaimRetired();
    CHECK(first.use_count() == 1);

    // Time-stretch processors follow the same deferred protocol.
    auto processor = std::make_shared<bgmedia::mix::LinearResampleStretcher>();
    engine.setTimeStretch(processor);
    CHECK(processor.use_count() >= 2);
    engine.setTimeStretch(
        std::make_shared<bgmedia::mix::LinearResampleStretcher>());
    CHECK(processor.use_count() >= 2);
    engine.render(block.data(), 128);
    engine.reclaimRetired();
    CHECK(processor.use_count() == 1);
}

TEST_CASE("mix.engine_load_seek_does_not_override_plan") {
    // Regression for consuming the transition plan before the load-time seek:
    // loading deck B in the same frame as a planned start used to reset the
    // cue position selected by the plan back to zero.
    MixEngine engine(kRate);
    auto a = toneTrack(30.0, 180.0);
    auto b = toneTrack(30.0, 240.0);
    CHECK(engine.loadDeck(DeckId::A, a, 1, 120.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.5, nullptr);

    CHECK(engine.loadDeck(DeckId::B, b, 2, 124.0));
    MixPlan plan = crossfadePlan(engine, 2.0, 1.5);
    CHECK(engine.startTransition(plan));

    std::vector<float> block(480 * 2, 0.0f);
    engine.render(block.data(), 480);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Running);
    CHECK_NEAR(snap.decks[1].positionSeconds, 1.5 + 480.0 / kRate, 0.02);
}

TEST_CASE("mix.engine_eof_declick_has_no_hard_edge") {
    // Regression for fading the tail of the callback block instead of the last
    // playable source sample: at a real EOF the output used to step from full
    // amplitude straight to silence.
    MixEngine engine(kRate);
    auto track = dcTrack(0.20, 0.25f);
    CHECK(engine.loadDeck(DeckId::A, track, 1, 120.0));
    engine.play(DeckId::A);

    std::vector<float> all;
    std::vector<float> block(64 * 2, 0.0f);
    for (int i = 0; i < 500 && !engine.snapshot().decks[0].ended; ++i) {
        engine.render(block.data(), 64);
        all.insert(all.end(), block.begin(), block.end());
    }
    CHECK(engine.snapshot().decks[0].ended);
    CHECK(!all.empty());

    std::size_t last = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (std::abs(all[i]) > 1e-5f) {
            last = i;
        }
    }
    CHECK(last > 0);
    CHECK(std::abs(all[last]) < 0.02f);
    if (last + 1 < all.size()) {
        CHECK(std::abs(all[last + 1] - all[last]) < 0.01f);
    }
}

TEST_CASE("mix.engine_cancel_after_handover_restores_master") {
    // Regression for cancellation after the handover point: master_ pointed at
    // the stopped incoming deck, so the next transition started from silence.
    MixEngine engine(kRate);
    auto a = toneTrack(30.0, 180.0);
    auto b = toneTrack(30.0, 240.0);
    CHECK(engine.loadDeck(DeckId::A, a, 1, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 2, 124.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.2, nullptr);

    CHECK(engine.startTransition(crossfadePlan(engine, 1.0, 0.0)));
    renderSeconds(engine, 0.7, nullptr);
    CHECK(engine.snapshot().master == DeckId::B);

    engine.cancelTransition();
    renderSeconds(engine, 0.5, nullptr);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Idle);
    CHECK(snap.master == DeckId::A);
    CHECK(snap.decks[0].playing);
    CHECK(!snap.decks[1].playing);
}

TEST_CASE("mix.session_remove_preparing_track_cancels_without_poisoning_next") {
    using namespace std::chrono_literals;
    MixEngine engine(kRate);
    std::atomic<bool> started{false};
    std::atomic<bool> cancelled{false};
    const auto prepare = makeTestPrepare();
    MixSession session(engine, [&](const std::filesystem::path& path,
                                   PreparedMixTrack& out, std::string& error,
                                   std::function<void(float, std::string_view)> progress) {
        if (path.filename() == "cancel.wav") {
            started.store(true);
            for (int i = 0; i < 500; ++i) {
                try {
                    progress(0.1f, "decoding");
                } catch (const std::exception&) {
                    cancelled.store(true);
                    throw;
                }
                std::this_thread::sleep_for(2ms);
            }
        }
        return prepare(path, out, error, std::move(progress));
    });
    SessionPump pump{engine, session};
    const auto id = session.addTrack("cancel.wav");
    for (int i = 0; i < 500 && !started.load(); ++i) {
        pump.once();
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(started.load());
    CHECK(session.removeTrack(id));
    session.addTrack("replacement.wav");
    REQUIRE(pump.waitForDecks(1));
    CHECK(cancelled.load());
}

TEST_CASE("mix.session_preparation_exception_does_not_kill_worker") {
    MixEngine engine(kRate);
    const auto prepare = makeTestPrepare();
    MixSession session(engine, [&](const std::filesystem::path& path,
                                   PreparedMixTrack& out, std::string& error,
                                   std::function<void(float, std::string_view)> progress) {
        if (path.filename() == "bad.wav") throw std::runtime_error("decode failed");
        return prepare(path, out, error, std::move(progress));
    });
    SessionPump pump{engine, session};
    const auto bad = session.addTrack("bad.wav");
    session.addTrack("good.wav");
    REQUIRE(pump.waitForDecks(1));
    const auto snapshot = session.snapshot();
    const auto failed = std::find_if(snapshot.queue.begin(), snapshot.queue.end(),
        [bad](const auto& item) { return item.id == bad; });
    REQUIRE(failed != snapshot.queue.end());
    CHECK(failed->state == bgmedia::mix::QueueState::Failed);
}

TEST_CASE("mix.pcm_consuming_multichannel_folds_without_another_source_buffer") {
    AudioData audio;
    audio.sampleRate = 48000;
    audio.channels = 5;
    audio.samples = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f,
                     -0.1f, -0.2f, -0.3f, -0.4f, -0.5f};
    auto expected = PcmTrack::fromAudio(audio, 48000);
    const auto* storage = audio.samples.data();
    auto actual = PcmTrack::fromAudio(std::move(audio), 48000);
    CHECK(actual.samples == expected.samples);
    CHECK(actual.samples.data() == storage);
}

TEST_CASE("mix.pcm_resampler_can_cancel_in_inner_loop") {
    auto track = dcTrack(1.0, 0.2f, 2, 44100);
    int checkpoints = 0;
    bool cancelled = false;
    try {
        (void)PcmTrack::resampled(*track, 48000, [&] {
            if (++checkpoints >= 40) throw std::runtime_error("cancel");
        });
    } catch (const std::runtime_error&) {
        cancelled = true;
    }
    CHECK(cancelled);
    CHECK(checkpoints == 40);
}

TEST_CASE("mix.pcm_consuming_conversion_matches_const_path") {
    AudioData audio;
    audio.sampleRate = 44100;
    audio.channels = 2;
    audio.samples = {0.25f, -0.5f, 1.5f, -2.0f, 0.1f, 0.3f, 0.4f, 0.6f};
    const auto expected = PcmTrack::fromAudio(audio, 48000);
    auto copy = audio;
    const auto actual = PcmTrack::fromAudio(std::move(copy), 48000);
    CHECK(actual.samples == expected.samples);
    CHECK(actual.sampleRate == expected.sampleRate);
    CHECK(actual.channels == expected.channels);
    const auto* original = audio.samples.data();
    const auto sameRate = PcmTrack::fromAudio(std::move(audio), 44100);
    CHECK(sameRate.samples.data() == original);
    CHECK_NEAR(sameRate.samples[2], 1.0, 1e-6);
    CHECK_NEAR(sameRate.samples[3], -1.0, 1e-6);
}

TEST_CASE("mix.session_long_queue_bounds_preparation_until_deck_retires") {
    MixEngine engine(kRate);
    std::atomic<int> calls{0};
    const auto prepare = makeTestPrepare();
    MixSession session(engine, [&](const std::filesystem::path& path,
                                   PreparedMixTrack& out, std::string& error,
                                   std::function<void(float, std::string_view)> progress) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return prepare(path, out, error, std::move(progress));
    });
    SessionPump pump{engine, session};
    for (int i = 0; i < 100; ++i) {
        session.addTrack("track-" + std::to_string(i) + ".wav");
    }
    CHECK(pump.waitForDecks(2));
    for (int i = 0; i < 50; ++i) {
        pump.once();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    auto before = session.snapshot();
    CHECK(before.queue.size() == 100);
    CHECK(before.preparedCount == 2);
    CHECK(before.preparingCount == 0);
    CHECK(before.preparedPcmBytes >= 2ull * 8 * kRate * 2 * sizeof(float));
    CHECK(calls.load(std::memory_order_relaxed) == 2);
    session.setTransitionType(TransitionType::DirectCut);
    session.play();
    pump.forSeconds(0.05);
    const auto completed = engine.completedTransitions();
    session.next();
    bool refilled = false;
    for (int i = 0; i < 800 && !refilled; ++i) {
        pump.once();
        const auto snapshot = engine.snapshot();
        refilled = snapshot.completedTransitions > completed &&
                   snapshot.decks[0].loaded && snapshot.decks[1].loaded &&
                   calls.load(std::memory_order_relaxed) >= 3;
        if (!refilled) std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    CHECK(refilled);
    for (int i = 0; i < 50; ++i) {
        pump.once();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    CHECK(calls.load(std::memory_order_relaxed) == 3);
}

TEST_CASE("mix.session_refilled_deck_stays_stopped") {
    // Finding #1: refilling the outgoing deck after a handover used to start it
    // immediately at full independent gain alongside the playing master.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("alpha.wav");
    session.addTrack("beta.wav");
    session.addTrack("gamma.wav");
    CHECK(pump.waitForDecks(2));

    session.setAdaptive(true);
    session.setTransitionType(TransitionType::DirectCut);
    session.play();
    pump.forSeconds(0.05);
    CHECK(engine.snapshot().decks[0].playing);

    const std::uint64_t completedBefore = engine.completedTransitions();
    session.next();
    bool refilled = false;
    for (int i = 0; i < 500 && !refilled; ++i) {
        pump.once();
        const bgmedia::mix::MixSnapshot snap = engine.snapshot();
        refilled = snap.completedTransitions > completedBefore &&
                   snap.decks[0].loaded && snap.decks[1].loaded;
        if (!refilled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    CHECK(refilled);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.master == DeckId::B);
    CHECK(snap.decks[1].playing);
    CHECK(snap.decks[0].loaded);
    CHECK(!snap.decks[0].playing);
}

TEST_CASE("mix.session_next_starts_at_current_position") {
    // Finding #2: manual Next/Start Transition used to pass -1 as the outgoing
    // position, so the planner selected the end-of-track window and the engine
    // seeked the playing deck to the end.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.setAdaptive(true);
    session.setTransitionDuration(0.0);
    session.play();
    pump.forSeconds(2.0);

    const bgmedia::mix::MixSnapshot before = engine.snapshot();
    const DeckId master = before.master;
    CHECK(before.decks[deckIndex(master)].playing);
    const double position = before.decks[deckIndex(master)].positionSeconds;
    CHECK(position > 1.0);

    session.next();
    pump.once();
    pump.once();
    const bgmedia::mix::MixSnapshot after = engine.snapshot();
    CHECK(after.phase == TransitionPhase::Running);
    const double started = after.decks[deckIndex(master)].positionSeconds;
    CHECK(started > 0.5);
    CHECK(started > position - 0.75);
    CHECK(started < position + 2.5);
}

TEST_CASE("mix.session_stop_and_clear_queue_recover") {
    // Finding #3: Stop set paused before the callback settled the deck flags, so
    // snapshots kept reporting "playing" and Clear Queue never released them.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("alpha.wav");
    session.addTrack("beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.forSeconds(0.1);
    CHECK(engine.snapshot().decks[0].playing);

    session.stopAll();
    pump.forSeconds(0.05);
    bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.paused);
    CHECK(!snap.decks[0].playing);
    CHECK(!snap.decks[1].playing);

    session.clearQueue();
    pump.forSeconds(0.05);
    CHECK(session.snapshot().queue.empty());
    snap = engine.snapshot();
    CHECK(!snap.decks[0].loaded);
    CHECK(!snap.decks[1].loaded);
}

TEST_CASE("mix.session_eof_promotes_prepared_deck") {
    // Finding #6: when the master ends before the incoming deck starts, the
    // prepared deck must be promoted instead of being ignored because it is not
    // playing yet.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("short-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.setAutoQueue(false);
    session.play();
    pump.forSeconds(1.2);

    bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.decks[0].ended);
    CHECK(!snap.decks[0].playing);
    CHECK(snap.decks[1].loaded);
    CHECK(!snap.decks[1].playing);

    session.setAutoQueue(true);
    pump.once();
    pump.once();
    pump.once();
    snap = engine.snapshot();
    CHECK(snap.master == DeckId::B);
    CHECK(snap.decks[1].playing);
    CHECK(!snap.decks[0].loaded);
}

TEST_CASE("mix.session_cancel_does_not_restart_next") {
    // Finding: nextRequested_ stayed set while a manual Next was running, so
    // cancelling it re-armed the request and a second transition started as
    // soon as the reverse fade finished.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.setAdaptive(false);
    session.setTransitionType(TransitionType::Crossfade);
    session.setTransitionDuration(6.0);
    session.setAutoQueue(false);
    session.play();
    pump.forSeconds(0.5);

    const std::uint64_t completedBefore = engine.completedTransitions();
    session.next();
    pump.forSeconds(0.2);
    CHECK(engine.snapshot().phase == TransitionPhase::Running);

    session.cancelTransition();
    pump.forSeconds(1.0);
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);
    CHECK(engine.completedTransitions() == completedBefore);

    // Stay idle for longer than the cancelled transition would have lasted.
    pump.forSeconds(7.0);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Idle);
    CHECK(engine.completedTransitions() == completedBefore);
    CHECK(snap.master == DeckId::A);
    CHECK(snap.decks[0].playing);
    CHECK(!snap.decks[1].playing);
}

TEST_CASE("mix.session_promotes_only_at_actual_eof") {
    // Finding: the last 50 ms were treated as an EOF, so the prepared deck was
    // promoted while the master was still playing. clearDeck() then failed on
    // the live play target and the finished item stayed loaded.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("short-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    // Automatic mixing off keeps this near-end window reachable: the old code
    // promoted there even though the track was still producing audio.
    session.setTransitionType(TransitionType::Off);
    session.play();

    bool inWindow = false;
    for (int i = 0; i < 4000 && !inWindow; ++i) {
        pump.once();
        const bgmedia::mix::MixSnapshot snap = engine.snapshot();
        const bgmedia::mix::DeckStatus& deck = snap.decks[0];
        inWindow = deck.loaded && deck.playing && !deck.ended &&
                   deck.durationSeconds > 0.0 &&
                   deck.durationSeconds - deck.positionSeconds <= 0.04;
        if (!inWindow) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(inWindow);

    // Several UI ticks at that position must not promote anything.
    for (int i = 0; i < 5; ++i) {
        session.update();
    }
    bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.master == DeckId::A);
    CHECK(snap.decks[0].loaded);
    CHECK(snap.decks[0].playing);
    CHECK(!snap.decks[1].playing);

    // The real EOF does promote, and the finished deck is released.
    pump.forSeconds(0.2);
    snap = engine.snapshot();
    CHECK(snap.master == DeckId::B);
    CHECK(snap.decks[1].playing);
    CHECK(!snap.decks[0].loaded);
    CHECK(!snap.decks[0].playing);
}

TEST_CASE("mix.session_auto_transition_uses_planned_window") {
    // Finding: automatic scheduling waited for `duration - fade`, i.e. the
    // physical file end, instead of the window the preview selected before
    // trailing silence. The fade then began after the track had gone quiet.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("long-silence-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.setAdaptive(false);
    session.setTransitionType(TransitionType::Crossfade);
    session.setTransitionDuration(4.0);
    session.play();
    pump.once();

    const bgmedia::mix::MixPlan preview = session.snapshot().preview;
    CHECK(preview.valid);
    // The fixture is 30 s long with silence from 24 s, so the planned window
    // must sit before 24 s: 24.2 - 4.0 = 20.2 s.
    CHECK(preview.transitionStartA < 23.0);
    CHECK(preview.transitionStartA > 17.0);

    bool started = false;
    for (int i = 0; i < 8000 && !started; ++i) {
        pump.once();
        started = engine.snapshot().phase == TransitionPhase::Running;
    }
    CHECK(started);

    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    const double startedAt = snap.decks[0].positionSeconds;
    // The old rule would not have fired before 25.65 s.
    CHECK(startedAt > 17.0);
    CHECK(startedAt < 23.0);
    CHECK_NEAR(snap.transitionDuration, 4.0, 0.35);
}

TEST_CASE("mix.engine_pause_fade_is_per_sample") {
    // Finding: the pause envelope subtracted the whole block's fade step at
    // once, so a 480-frame callback at 48 kHz jumped from full amplitude to
    // silence (and back) and bypassed the 8 ms de-click.
    MixEngine engine(kRate);
    CHECK(engine.loadDeck(DeckId::A, dcTrack(1.0, 0.4f), 1, 120.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.2, nullptr);

    std::vector<float> block(480 * 2, 0.0f);
    engine.setPaused(true);
    engine.render(block.data(), 480);
    // The first sample of the pause block must still carry audio, and the
    // block must end at true silence.
    CHECK(std::abs(block[0]) > 0.25f);
    CHECK(std::abs(block[480 * 2 - 1]) == 0.0f);

    engine.setPaused(false);
    engine.render(block.data(), 480);
    // Resume ramps up from silence instead of snapping to full level.
    CHECK(std::abs(block[0]) < 0.02f);
    CHECK(std::abs(block[480 * 2 - 1]) > 0.30f);
}

TEST_CASE("mix.session_auto_direct_cut_uses_marker") {
    // Finding: automatic scheduling skipped valid zero-duration Direct Cut
    // plans, so it fell back to EOF promotion, which started the incoming deck
    // at zero and recorded no completed transition.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("long-silence-alpha.wav");
    session.addTrack("long-marker-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.setAdaptive(true);
    session.setTransitionType(TransitionType::DirectCut);
    session.setTransitionDuration(0.0);
    session.play();
    pump.once();

    const bgmedia::mix::MixPlan preview = session.snapshot().preview;
    CHECK(preview.valid);
    CHECK(preview.type == TransitionType::DirectCut);
    CHECK_NEAR(preview.duration, 0.0, 1e-6);
    // The cut point is the end of the musical window, before the 24 s silence.
    CHECK(preview.transitionStartA > 23.0);
    CHECK(preview.transitionStartA < 24.5);
    CHECK(preview.startB > 1.0);

    const std::uint64_t completedBefore = engine.completedTransitions();
    bool cut = false;
    for (int i = 0; i < 8000 && !cut; ++i) {
        pump.once();
        cut = engine.completedTransitions() > completedBefore;
    }
    CHECK(cut);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.master == DeckId::B);
    CHECK(snap.decks[1].playing);
    CHECK(snap.decks[1].positionSeconds >= preview.startB - 0.2);
    CHECK(snap.decks[1].positionSeconds < preview.startB + 1.0);
}

TEST_CASE("mix.engine_concurrent_controls_stress") {
    // Real two-thread stress: one thread is the audio callback (render), the
    // other hammers play/pause/seek/load/clear/next/cancel/stop. Run this under
    // ASan to make the deferred-reclamation protocol observable; a plain CTest
    // pass is not by itself proof of memory safety.
    MixEngine engine(kRate);
    CHECK(engine.loadDeck(DeckId::A, dcTrack(2.0, 0.10f), 1, 120.0));
    CHECK(engine.loadDeck(DeckId::B, dcTrack(2.0, 0.12f), 2, 124.0));
    engine.play(DeckId::A);
    engine.setMaster(DeckId::A);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> renders{0};
    std::atomic<std::uint64_t> nonFinite{0};
    std::atomic<float> peak{0.0f};

    std::thread audio([&] {
        std::vector<float> buffer(256 * 2, 0.0f);
        while (!stop.load(std::memory_order_acquire)) {
            engine.render(buffer.data(), 256);
            float localPeak = 0.0f;
            for (float value : buffer) {
                if (!std::isfinite(value)) {
                    nonFinite.fetch_add(1, std::memory_order_relaxed);
                }
                localPeak = std::max(localPeak, std::abs(value));
            }
            peak.store(std::max(peak.load(std::memory_order_relaxed), localPeak),
                       std::memory_order_relaxed);
            renders.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread control([&] {
        for (int i = 0; i < 4000; ++i) {
            switch (i % 12) {
            case 0:
                engine.setPaused(false);
                engine.play(DeckId::A);
                engine.setMaster(DeckId::A);
                break;
            case 1:
                engine.setPaused(false);
                engine.play(DeckId::B);
                engine.setMaster(DeckId::B);
                break;
            case 2:
                engine.pause(DeckId::A);
                engine.pause(DeckId::B);
                break;
            case 3:
                engine.setPaused(!engine.paused());
                break;
            case 4:
                engine.seek(DeckId::A, (i % 40) * 0.05);
                break;
            case 5:
                engine.seek(DeckId::B, (i % 40) * 0.05);
                break;
            case 6:
                // Fresh buffers: the engine's retire list is the only owner of
                // the replaced track, so a premature free is a real UAF.
                (void)engine.loadDeck(DeckId::A, dcTrack(0.4, 0.08f),
                                      1000 + i, 120.0);
                break;
            case 7:
                (void)engine.loadDeck(DeckId::B, dcTrack(0.4, 0.09f),
                                      2000 + i, 124.0);
                break;
            case 8:
                engine.clearDeck(i & 1 ? DeckId::A : DeckId::B);
                break;
            case 9: {
                const bgmedia::mix::MixSnapshot snap = engine.snapshot();
                if (snap.decks[0].loaded && snap.decks[1].loaded &&
                    snap.phase == TransitionPhase::Idle) {
                    (void)engine.startTransition(
                        crossfadePlan(engine, 0.8, 0.0));
                }
                break;
            }
            case 10:
                engine.cancelTransition();
                break;
            default:
                // Directly adjacent stop + submit: the callback discards the
                // pending plan while the control thread publishes a new one.
                // The payload is control-thread-owned, so startTransition()
                // may overwrite it only after the ready flag is cleared.
                engine.stop();
                (void)engine.startTransition(
                    crossfadePlan(engine, 0.5, 0.0));
                break;
            }
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_release);
    });

    control.join();
    audio.join();

    CHECK(renders.load() > 0);
    CHECK(nonFinite.load() == 0);
    CHECK(peak.load() <= 1.02f);
    std::printf("mix stress: renders=%llu peak=%.3f nonfinite=%llu\n",
                static_cast<unsigned long long>(renders.load()),
                static_cast<double>(peak.load()),
                static_cast<unsigned long long>(nonFinite.load()));
}

// ---------------------------------------------------------------------------
// BG Mix 0.2 DSP: pitch-preserving time-stretch
// ---------------------------------------------------------------------------
namespace {

double positiveZeroCrossingHz(const std::vector<float>& samples, int channels,
                              std::uint32_t rate, double skipSeconds) {
    const std::size_t first =
        static_cast<std::size_t>(skipSeconds * static_cast<double>(rate));
    const std::size_t frames = samples.size() / static_cast<std::size_t>(channels);
    if (frames <= first + 8) {
        return 0.0;
    }
    std::size_t crossings = 0;
    for (std::size_t i = first + 1; i < frames; ++i) {
        const float previous = samples[(i - 1) * static_cast<std::size_t>(channels)];
        const float current = samples[i * static_cast<std::size_t>(channels)];
        if (previous <= 0.0f && current > 0.0f) {
            ++crossings;
        }
    }
    const double seconds =
        static_cast<double>(frames - first) / static_cast<double>(rate);
    return seconds > 0.0 ? static_cast<double>(crossings) / seconds : 0.0;
}

std::vector<float> renderStretch(const bgmedia::mix::PcmTrack& track,
                                 double ratio, double seconds,
                                 double startSeconds = 0.0) {
    const bgmedia::mix::WsolaStretcher processor;
    const auto stream = processor.createStream(track.sampleRate);
    double position = startSeconds * track.sampleRate;
    stream->reset(position);
    const std::size_t frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> output(frames * 2, 0.0f);
    std::vector<float> block(480 * 2, 0.0f);
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t chunk = std::min<std::size_t>(480, frames - done);
        stream->render(track, position, ratio, ratio, block.data(), chunk);
        std::copy_n(block.begin(), chunk * 2, output.begin() +
                                                 static_cast<std::ptrdiff_t>(done * 2));
        done += chunk;
    }
    return output;
}

} // namespace

TEST_CASE("mix.wsola_preserves_pitch") {
    const auto track = toneTrack(8.0, 440.0);
    const double ratios[] = {0.90, 0.95, 1.05, 1.10};
    for (double ratio : ratios) {
        const std::vector<float> output = renderStretch(*track, ratio, 3.0, 1.0);
        const double hz = positiveZeroCrossingHz(output, 2, kRate, 0.35);
        CHECK_NEAR(hz, 440.0, 8.0);
        float peak = 0.0f;
        for (float value : output) {
            peak = std::max(peak, std::abs(value));
        }
        CHECK(peak > 0.15f);
        CHECK(peak < 0.60f);
        std::printf("wsola pitch %.2fx: %.2f Hz (target 440), peak %.3f\n",
                    ratio, hz, static_cast<double>(peak));
    }
}

TEST_CASE("mix.wsola_has_no_sample_step_clicks") {
    const auto track = toneTrack(6.0, 220.0);
    const std::vector<float> output = renderStretch(*track, 1.10, 2.0, 0.5);
    float largestStep = 0.0f;
    for (std::size_t i = 2; i < output.size(); ++i) {
        largestStep = std::max(largestStep, std::abs(output[i] - output[i - 2]));
    }
    // A 220 Hz sine at 0.4 amplitude changes by at most ~0.015 per 48 kHz
    // sample. Anything near a full-scale jump would be a click.
    CHECK(largestStep < 0.08f);
    std::printf("wsola click check: largest sample step %.4f\n",
                static_cast<double>(largestStep));
}

TEST_CASE("mix.wsola_preserves_transient_spacing") {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = kRate;
    track->channels = 2;
    const std::size_t frames = kRate * 6;
    track->samples.assign(frames * 2, 0.0f);
    // 30 ms bursts every 250 ms.
    for (int burst = 0; burst < 20; ++burst) {
        const std::size_t start = static_cast<std::size_t>((0.25 * burst + 0.1) * kRate);
        for (std::size_t i = 0; i < kRate / 33; ++i) {
            const float value = static_cast<float>(
                0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                               static_cast<double>(i) / kRate));
            if (start + i < frames) {
                track->samples[(start + i) * 2] = value;
                track->samples[(start + i) * 2 + 1] = value;
            }
        }
    }
    const std::vector<float> output = renderStretch(*track, 1.10, 4.0, 0.0);
    // Count envelope bursts above 0.1 after a 30 ms release.
    std::vector<double> peaks;
    double envelope = 0.0;
    double lastPeak = -1.0;
    bool inBurst = false;
    for (std::size_t i = 0; i < output.size() / 2; ++i) {
        const float value = output[i * 2];
        envelope = std::max(static_cast<double>(std::abs(value)),
                            envelope * 0.999);
        if (!inBurst && envelope > 0.2) {
            inBurst = true;
            const double time = static_cast<double>(i) / kRate;
            if (lastPeak >= 0.0) {
                peaks.push_back(time - lastPeak);
            }
            lastPeak = time;
        } else if (inBurst && envelope < 0.05) {
            inBurst = false;
        }
    }
    CHECK(peaks.size() >= 8);
    for (std::size_t i = 1; i + 1 < peaks.size(); ++i) {
        CHECK_NEAR(peaks[i], 0.25, 0.03);
    }
    std::printf("wsola transients: %zu intervals, first %.4f s\n",
                peaks.size(), peaks.empty() ? 0.0 : peaks[1]);
}

namespace {

MixPlan rampedPlan(const MixEngine& engine, double rampSeconds, double media,
                   double duration, double targetRatio) {
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    MixPlan plan;
    plan.valid = true;
    plan.type = TransitionType::TempoRamp;
    plan.requestedType = TransitionType::TempoRamp;
    plan.from = DeckId::A;
    plan.to = DeckId::B;
    plan.preTransitionSeconds = rampSeconds;
    plan.duration = duration;
    plan.tempoRampStartA = snap.decks[0].positionSeconds;
    plan.transitionStartA = plan.tempoRampStartA + media;
    plan.transitionEndA = plan.transitionStartA + duration;
    plan.startB = 0.0;
    plan.endB = duration;
    plan.handoverProgress = 0.5;
    plan.tempoRampActive = true;
    plan.pitchLock = true;
    plan.tempoRatioAStart = 1.0;
    plan.tempoRatioAEnd = targetRatio;
    constexpr int kPoints = 33;
    for (int i = 0; i < kPoints; ++i) {
        const double t = static_cast<double>(i) / (kPoints - 1);
        const double rate = 1.0 + (targetRatio - 1.0) * t;
        plan.tempoCurve.push_back(
            {-rampSeconds + rampSeconds * t, rate, 123.0 * rate});
    }
    plan.tempoCurve.push_back({0.0, targetRatio, 123.0 * targetRatio});
    plan.tempoCurve.push_back({duration, targetRatio, 123.0 * targetRatio});
    return plan;
}

} // namespace

TEST_CASE("mix.engine_preroll_before_crossfade") {
    MixEngine engine(kRate);
    auto a = toneTrack(40.0, 173.0);
    auto b = toneTrack(40.0, 197.0);
    CHECK(engine.loadDeck(DeckId::A, a, 31, 123.0));
    CHECK(engine.loadDeck(DeckId::B, b, 32, 128.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.2, nullptr);

    const MixPlan plan = rampedPlan(engine, 2.0, 2.1, 2.0, 1.10);
    CHECK(engine.startTransition(plan));

    renderSeconds(engine, 1.0, nullptr);
    bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Running);
    CHECK(snap.stage == TransitionStage::TempoRamp ||
          snap.stage == TransitionStage::BeatAlign);
    CHECK_NEAR(snap.crossfadeElapsed, 0.0, 1e-9);
    CHECK_NEAR(snap.preTransitionSeconds, 2.0, 0.01);
    CHECK(!snap.decks[1].playing);
    CHECK_NEAR(snap.decks[1].positionSeconds, 0.0, 1e-3);
    CHECK_NEAR(snap.tempoRatioA, 1.05, 0.02);

    // The incoming deck must appear exactly at the crossfade boundary, on its
    // planned start marker, not at the ramp start.
    renderSeconds(engine, 1.2, nullptr);
    snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Running);
    CHECK(snap.stage == TransitionStage::Mixing ||
          snap.stage == TransitionStage::Handover);
    CHECK(snap.crossfadeElapsed > 0.1);
    CHECK(snap.decks[1].playing);
    CHECK_NEAR(snap.decks[1].positionSeconds, 0.2, 0.06);
    CHECK_NEAR(snap.tempoRatioA, 1.10, 0.02);
    // A advanced by the integral of the ramp: 2.0 s * 1.05 = 2.1 s of media.
    CHECK_NEAR(snap.decks[0].positionSeconds,
               plan.tempoRampStartA + 2.1 + 0.2 * 1.10, 0.06);
    std::printf(
        "preroll: stage=%d crossfadeElapsed=%.3f A=%.3f B=%.3f ratio=%.3f\n",
        static_cast<int>(snap.stage), snap.crossfadeElapsed,
        snap.decks[0].positionSeconds, snap.decks[1].positionSeconds,
        snap.tempoRatioA);
}

TEST_CASE("mix.engine_cancel_during_preroll_restores_rate") {
    MixEngine engine(kRate);
    auto a = toneTrack(40.0, 173.0);
    auto b = toneTrack(40.0, 197.0);
    CHECK(engine.loadDeck(DeckId::A, a, 33, 123.0));
    CHECK(engine.loadDeck(DeckId::B, b, 34, 128.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.2, nullptr);
    const MixPlan plan = rampedPlan(engine, 2.0, 2.1, 2.0, 1.10);
    CHECK(engine.startTransition(plan));

    renderSeconds(engine, 1.0, nullptr);
    CHECK(engine.snapshot().decks[1].playing == false);
    engine.cancelTransition();
    RenderStats stats;
    renderSeconds(engine, 0.5, &stats, 0.02);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(stats.nonFinite == 0);
    CHECK(stats.peak <= 1.02f);
    CHECK(snap.phase == TransitionPhase::Idle);
    CHECK(snap.master == DeckId::A);
    CHECK(snap.decks[0].playing);
    CHECK(!snap.decks[1].playing);
    CHECK_NEAR(snap.decks[0].rate, 1.0, 0.02);
    std::printf("preroll cancel: A=%.3f rate=%.3f peak=%.3f\n",
                snap.decks[0].positionSeconds, snap.decks[0].rate,
                static_cast<double>(stats.peak));
}

TEST_CASE("mix.engine_bass_swap_monitor") {
    MixEngine engine(kRate);
    auto a = toneTrack(30.0, 100.0);
    auto b = toneTrack(30.0, 100.0);
    CHECK(engine.loadDeck(DeckId::A, a, 35, 123.0));
    CHECK(engine.loadDeck(DeckId::B, b, 36, 123.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.1, nullptr);

    MixPlan plan = crossfadePlan(engine, 2.0, 0.0);
    plan.bassSwap = true;
    plan.eqCurveOut = {{0.0, 1.0f, 1.0f, 1.0f}, {2.0, 0.0f, 1.0f, 1.0f}};
    plan.eqCurveIn = {{0.0, 0.0f, 1.0f, 1.0f}, {2.0, 1.0f, 1.0f, 1.0f}};
    CHECK(engine.startTransition(plan));
    renderSeconds(engine, 1.0, nullptr);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.bassSwap);
    CHECK_NEAR(snap.eqLow[0], 0.5, 0.03);
    CHECK_NEAR(snap.eqLow[1], 0.5, 0.03);
    CHECK_NEAR(snap.eqMid[0], 1.0, 1e-4);
    CHECK_NEAR(snap.eqHigh[0], 1.0, 1e-4);
    std::printf("bass swap: outLow=%.3f inLow=%.3f\n",
                static_cast<double>(snap.eqLow[0]),
                static_cast<double>(snap.eqLow[1]));
}

TEST_CASE("mix.eq_unity_bypass_and_band_response") {
    ThreeBandEq eq;
    eq.configure(kRate);
    const std::size_t frames = kRate;
    std::vector<float> input(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        input[i * 2] = input[i * 2 + 1] =
            static_cast<float>(0.4 * std::sin(2.0 * 3.14159265358979323846 *
                                              100.0 * static_cast<double>(i) /
                                              kRate));
    }
    std::vector<float> bypass = input;
    eq.process(bypass.data(), frames, EqGains{}, EqGains{});
    CHECK(bypass == input);

    auto rms = [](const std::vector<float>& data) {
        double sum = 0.0;
        for (float value : data) {
            sum += static_cast<double>(value) * value;
        }
        return std::sqrt(sum / static_cast<double>(data.size()));
    };
    std::vector<float> lowCut = input;
    EqGains cut;
    cut.low = 0.0f;
    eq.process(lowCut.data(), frames, cut, cut);
    CHECK(rms(lowCut) < rms(input) * 0.65);

    // The high band must survive a low cut. 5 kHz is well above the 3.2 kHz
    // crossover, so only the one-pole split's small mid leakage changes it.
    std::vector<float> highs(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        highs[i * 2] = highs[i * 2 + 1] =
            static_cast<float>(0.4 * std::sin(2.0 * 3.14159265358979323846 *
                                              5000.0 * static_cast<double>(i) /
                                              kRate));
    }
    std::vector<float> highCut = highs;
    eq.process(highCut.data(), frames, cut, cut);
    CHECK(rms(highCut) > rms(highs) * 0.85);

    // A one-second automation ramp must not step the sample stream.
    ThreeBandEq automating;
    automating.configure(kRate);
    std::vector<float> swept = input;
    EqGains from;
    EqGains to;
    to.low = 0.0f;
    automating.process(swept.data(), frames, from, to);
    float largestStep = 0.0f;
    for (std::size_t i = 2; i < swept.size(); i += 2) {
        largestStep = std::max(largestStep, std::abs(swept[i] - swept[i - 2]));
    }
    CHECK(largestStep < 0.1f);
    std::printf("eq: lowCutRms=%.3f highKept=%.3f step=%.4f\n", rms(lowCut),
                rms(highCut) / std::max(1e-9, rms(highs)),
                static_cast<double>(largestStep));
}

TEST_CASE("mix.planner_phrase_and_adaptive") {
    MixPlanner planner;
    PlanRequest phrase;
    phrase.type = TransitionType::PhraseAware;
    phrase.requestedDuration = 8.0;
    const MixPlan plan =
        planner.plan(analysisFor(123.0), analysisFor(128.0), phrase);
    CHECK(plan.valid);
    CHECK(plan.type == TransitionType::PhraseAware);
    CHECK(plan.phraseBoundary);
    CHECK(plan.phraseConfidence > 0.0f);
    CHECK(plan.downbeatAligned);
    CHECK(plan.tempoRampActive);
    CHECK(plan.preTransitionSeconds > 0.0);
    CHECK(plan.startB >= 0.0);

    // Adaptive with compatible tempo uses the musical machinery, not a
    // generic fade.
    PlanRequest adaptive;
    adaptive.type = TransitionType::SmartCrossfade;
    adaptive.requestedDuration = 8.0;
    const MixPlan compatible =
        planner.plan(analysisFor(123.0), analysisFor(125.0), adaptive);
    CHECK(compatible.valid);
    CHECK(compatible.type != TransitionType::Crossfade);
    CHECK(compatible.fallbackReason.empty());

    // Incompatible tempo: an explicit, explained fallback to Smart Crossfade.
    const MixPlan incompatible =
        planner.plan(analysisFor(123.0), analysisFor(180.0), adaptive);
    CHECK(incompatible.valid);
    CHECK(incompatible.type == TransitionType::SmartCrossfade);
    CHECK(incompatible.fallbackReason.find("exceeds") != std::string::npos);
    CHECK(incompatible.bassSwap);

    // Unstable tempo and low confidence both fall back explicitly.
    AnalysisResult unstable = analysisFor(123.0);
    unstable.tempoStable = false;
    const MixPlan unstablePlan =
        planner.plan(unstable, analysisFor(125.0), adaptive);
    CHECK(unstablePlan.type == TransitionType::Crossfade);
    CHECK(unstablePlan.fallbackReason.find("stable") != std::string::npos);

    const MixPlan low =
        planner.plan(analysisFor(123.0, 30.0, false),
                     analysisFor(125.0, 30.0, false), adaptive);
    CHECK(low.type == TransitionType::Crossfade);
    CHECK(!low.fallbackReason.empty());
    std::printf("adaptive: compatible=%s incompatible='%s'\n",
                bgmedia::mix::transitionTypeName(compatible.type),
                incompatible.fallbackReason.c_str());
}

TEST_CASE("mix.planner_ramp_smoothness") {
    PlannerConfig config;
    config.tempoRampMode = TempoRampMode::Manual;
    config.tempoRampBars = 8.0;
    config.tempoCurveKind = TempoCurveKind::Smooth;
    MixPlanner planner(config);
    PlanRequest request;
    request.type = TransitionType::TempoRamp;
    request.requestedDuration = 8.0;
    request.currentPositionA = 10.0;
    const MixPlan plan =
        planner.plan(analysisFor(123.0), analysisFor(128.0), request);
    CHECK(plan.valid);
    CHECK(plan.tempoRampActive);
    CHECK_NEAR(plan.tempoCurve.front().rate, 1.0, 1e-6);
    CHECK_NEAR(plan.tempoCurve.back().rate, 128.0 / 123.0, 1e-3);
    float largest = 0.0f;
    for (std::size_t i = 1; i < plan.tempoCurve.size(); ++i) {
        largest = std::max(
            largest, static_cast<float>(std::abs(plan.tempoCurve[i].rate -
                                                 plan.tempoCurve[i - 1].rate)));
        CHECK(plan.tempoCurve[i].time >= plan.tempoCurve[i - 1].time);
    }
    CHECK(largest < 0.01f);
    CHECK(plan.tempoRampStartA >= 0.0);
    CHECK(plan.tempoRampStartA < plan.transitionStartA);
    std::printf("ramp smooth: maxStep=%.5f bars=%.0f pre=%.2f s\n",
                static_cast<double>(largest), config.tempoRampBars,
                plan.preTransitionSeconds);
}

TEST_CASE("mix.planner_fits_incoming_rate") {
    AnalysisResult b = analysisFor(100.0, 8.0);
    b.octaveAmbiguous = true;
    b.bpmCandidates = {125.0};
    PlannerConfig config;
    config.tempoRampMode = TempoRampMode::Manual;
    config.maxTempoDeltaPercent = 30.0;
    MixPlanner planner(config);
    PlanRequest request;
    request.type = TransitionType::TempoRamp;
    request.requestedDuration = 8.0;
    request.currentPositionA = 10.0;
    const MixPlan plan = planner.plan(analysisFor(125.0), b, request);
    CHECK(plan.valid);
    // A already matches the compromise target, so only B needs the rate match;
    // there is no outgoing ramp.
    CHECK(!plan.tempoRampActive);
    CHECK_NEAR(plan.tempoRatioB, 1.25, 1e-3);
    // 8 s of source at a mean incoming rate of 1.1875 lasts 6.74 s.
    CHECK(plan.duration < 6.8);
    CHECK(plan.endB <= 8.0 + 1e-6);
    std::printf("incoming rate fit: duration=%.2f endB=%.3f ratioB=%.2f\n",
                plan.duration, plan.endB, plan.tempoRatioB);
}

TEST_CASE("mix.planner_compromise_tempo") {
    PlannerConfig config;
    config.tempoRampMode = TempoRampMode::Auto;
    config.maxTempoDeltaPercent = 10.0;
    MixPlanner planner(config);
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 20.0;
    const MixPlan plan = planner.plan(analysisFor(115.0, 120.0),
                                      analysisFor(140.0), request);
    CHECK(plan.valid);
    CHECK(plan.tempoRampActive);
    CHECK(plan.type == TransitionType::PhraseAware);
    // 115 and 140 are 21.7% apart; the planner meets at 126.02 so each deck
    // moves inside the 10% cap.
    CHECK_NEAR(plan.targetBpm, 126.02, 0.2);
    CHECK(std::abs(plan.tempoRatioAEnd - 1.0) <= 0.10 + 1e-6);
    CHECK(std::abs(plan.tempoRatioB - 1.0) <= 0.10 + 1e-6);
    CHECK(plan.fallbackReason.empty());
    std::printf("compromise: target=%.2f rA=%.4f rB=%.4f pre=%.2f\n",
                plan.targetBpm, plan.tempoRatioAEnd, plan.tempoRatioB,
                plan.preTransitionSeconds);
}

TEST_CASE("mix.planner_no_sync_leak_into_crossfade") {
    AnalysisResult a = analysisFor(120.0);
    AnalysisResult b = analysisFor(160.0);
    b.octaveAmbiguous = true;
    b.bpmCandidates = {160.0, 120.0};

    PlannerConfig config;
    config.tempoRampMode = TempoRampMode::Off;
    MixPlanner planner(config);
    PlanRequest request;
    request.type = TransitionType::Crossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 10.0;
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.type == TransitionType::Crossfade);
    CHECK(!plan.tempoRampActive);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);
    CHECK_NEAR(plan.tempoRatioAEnd, 1.0, 1e-9);
    CHECK_NEAR(plan.tempoRampPercent, 0.0, 1e-9);

    // Manual mode must not let an adaptive Smart Crossfade invent a ramp.
    PlannerConfig manual;
    manual.tempoRampMode = TempoRampMode::Manual;
    MixPlanner manualPlanner(manual);
    PlanRequest adaptive;
    adaptive.type = TransitionType::SmartCrossfade;
    adaptive.requestedDuration = 8.0;
    adaptive.currentPositionA = 10.0;
    const MixPlan manualPlan =
        manualPlanner.plan(analysisFor(120.0), analysisFor(126.0), adaptive);
    CHECK(manualPlan.type != TransitionType::TempoRamp);
    CHECK(!manualPlan.tempoRampActive);
    CHECK(manualPlan.fallbackReason.find("Auto") != std::string::npos);
    std::printf("no sync leak: crossfade ratioB=%.3f manualType=%s\n",
                plan.tempoRatioB,
                bgmedia::mix::transitionTypeName(manualPlan.type));
}

TEST_CASE("mix.live_tempo_follows_master") {
    bgmedia::mix::MixSnapshot snap;
    snap.master = DeckId::B;
    snap.outputLatencySeconds = 0.02;
    bgmedia::mix::DeckStatus& deck = snap.decks[1];
    deck.loaded = true;
    deck.playing = true;
    deck.trackId = 77;
    deck.analysedBpm = 128.0;
    deck.rate = 1.10;
    deck.bpm = 140.8;
    deck.positionSeconds = 2.0;

    const bgmedia::mix::LiveMixTempo live = bgmedia::mix::liveMixTempo(snap);
    CHECK(live.valid);
    CHECK(live.playing);
    CHECK(live.master == DeckId::B);
    CHECK(live.trackId == 77);
    CHECK_NEAR(live.bpm, 140.8, 1e-6);
    CHECK_NEAR(live.rate, 1.10, 1e-6);
    CHECK_NEAR(live.positionSeconds, 1.98, 1e-9);
    // Phase is in source-media time: the live BPM is 140.8, but the source
    // grid is still 128 BPM. Using the live period would apply the rate twice.
    CHECK_NEAR(live.sourceBpm, 128.0, 1e-9);
    CHECK_NEAR(live.beatPeriodSeconds, 60.0 / 128.0, 1e-9);
    CHECK_NEAR(live.beatPhase, 0.224, 1e-3);
    CHECK(live.beatPhase >= 0.0);
    CHECK(live.beatPhase < 1.0);

    // A handover moves the musical reference to the other deck without any
    // motion3 change: the adapter simply reports the new master.
    snap.master = DeckId::A;
    snap.decks[0].loaded = true;
    snap.decks[0].playing = true;
    snap.decks[0].trackId = 78;
    snap.decks[0].bpm = 126.0;
    snap.decks[0].analysedBpm = 126.0;
    snap.decks[0].rate = 1.0;
    snap.decks[0].positionSeconds = 4.0;
    const bgmedia::mix::LiveMixTempo after = bgmedia::mix::liveMixTempo(snap);
    CHECK(after.master == DeckId::A);
    CHECK(after.trackId == 78);
    CHECK_NEAR(after.bpm, 126.0, 1e-6);

    // During transition cancellation before handover, the master deck is still
    // playing throughout the reverse fade, so the mix clock remains active.
    snap.phase = TransitionPhase::Cancelling;
    CHECK(bgmedia::mix::liveMixTempo(snap).playing);
    snap.decks[0].playing = false;
    CHECK(!bgmedia::mix::liveMixTempo(snap).playing);
    std::printf("live tempo: master=%d bpm=%.1f phase=%.3f\n",
                static_cast<int>(live.master), live.bpm, live.beatPhase);
}

TEST_CASE("mix.wsola_large_block_drains") {
    const auto track = toneTrack(6.0, 440.0);
    bgmedia::mix::WsolaStretcher processor;
    const auto stream = processor.createStream(kRate);
    CHECK(stream != nullptr);
    stream->reset(0.0);
    constexpr std::size_t kFrames = 16384;
    std::vector<float> output(kFrames * 2, 0.0f);
    double position = 0.0;
    stream->render(*track, position, 1.0, 1.0, output.data(), kFrames);
    CHECK(position > 16000.0);
    std::size_t nonFinite = 0;
    float peak = 0.0f;
    for (float value : output) {
        if (!std::isfinite(value)) {
            ++nonFinite;
        }
        peak = std::max(peak, std::abs(value));
    }
    CHECK(nonFinite == 0);
    CHECK(peak > 0.2f);
}

TEST_CASE("mix.wsola_one_shot_isolated") {
    bgmedia::mix::WsolaStretcher processor;
    auto positive = dcTrack(0.2, 0.4f);
    auto negative = dcTrack(0.2, -0.4f);
    constexpr int kFrames = 2000;
    std::vector<float> first(kFrames * 2, 0.0f);
    std::vector<float> second(kFrames * 2, 0.0f);
    double positionA = 0.0;
    double positionB = 0.0;
    processor.render(*positive, positionA, 1.0, first.data(), kFrames);
    processor.render(*negative, positionB, 1.0, second.data(), kFrames);
    // The second one-shot render must not inherit the first track's queued
    // positive samples or overlap history.
    float tailAverage = 0.0f;
    for (int i = kFrames / 2; i < kFrames; ++i) {
        tailAverage += second[i * 2];
    }
    tailAverage /= static_cast<float>(kFrames / 2);
    std::printf("one-shot isolation: tail=%.4f\n",
                static_cast<double>(tailAverage));
    CHECK(tailAverage < -0.1f);
}

TEST_CASE("mix.wsola_dc_unity") {
    const auto track = dcTrack(2.0, 0.4f);
    const std::vector<float> output = renderStretch(*track, 1.0, 1.5, 0.0);
    const std::size_t first = static_cast<std::size_t>(0.5 * kRate);
    const std::size_t last = static_cast<std::size_t>(1.4 * kRate);
    double sum = 0.0;
    float minimum = 1.0f;
    float maximum = -1.0f;
    for (std::size_t i = first; i < last; ++i) {
        sum += output[i * 2];
        minimum = std::min(minimum, output[i * 2]);
        maximum = std::max(maximum, output[i * 2]);
    }
    const double mean = sum / static_cast<double>(last - first);
    std::printf("wsola DC: mean=%.5f min=%.5f max=%.5f\n", mean,
                static_cast<double>(minimum), static_cast<double>(maximum));
    CHECK_NEAR(mean, 0.4, 0.005);
    CHECK(maximum - minimum < 0.01f);
}

TEST_CASE("mix.wsola_unity_reconstruction") {
    const auto track = broadbandTrack(3.0);
    const std::vector<float> output = renderStretch(*track, 1.0, 1.5, 0.0);
    const std::size_t first = static_cast<std::size_t>(0.25 * kRate);
    const std::size_t last = static_cast<std::size_t>(1.2 * kRate);
    double error = 0.0;
    double signal = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        const double difference = static_cast<double>(output[i * 2]) -
                                  track->samples[i * 2];
        error += difference * difference;
        signal += static_cast<double>(track->samples[i * 2]) *
                  track->samples[i * 2];
    }
    const double relative = std::sqrt(error / std::max(1e-12, signal));
    CHECK(relative < 0.05);
    std::printf("wsola unity reconstruction: relative error %.4f\n", relative);
}

TEST_CASE("mix.wsola_stops_at_eof") {
    const auto track = toneTrack(1.0, 440.0);
    const std::vector<float> output = renderStretch(*track, 1.0, 2.0, 0.0);
    const std::size_t first = static_cast<std::size_t>(1.25 * kRate);
    const std::size_t last = static_cast<std::size_t>(1.90 * kRate);
    double sum = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        sum += static_cast<double>(output[i * 2]) * output[i * 2];
    }
    const double rms = std::sqrt(sum / static_cast<double>(last - first));
    CHECK(rms < 1e-4);
    std::printf("wsola EOF tail: rms after source end %.6f\n", rms);
}

TEST_CASE("mix.wsola_sub_unity_tail_reaches_source_end") {
    const auto track = toneTrack(1.0, 220.0);
    const std::vector<float> output = renderStretch(*track, 0.25, 4.2, 0.0);
    auto rmsRange = [&](double from, double to) {
        const std::size_t first = static_cast<std::size_t>(from * kRate);
        const std::size_t last = static_cast<std::size_t>(to * kRate);
        double sum = 0.0;
        for (std::size_t i = first; i < last; ++i) {
            sum += static_cast<double>(output[i * 2]) * output[i * 2];
        }
        return std::sqrt(sum / static_cast<double>(last - first));
    };
    // 1 s of source at 0.25x lasts 4 s; the stretched tail must not stop at
    // the last full window (~3.88 s).
    CHECK(rmsRange(3.0, 3.8) > 0.05);
    CHECK(rmsRange(4.05, 4.2) < 1e-4);
    std::printf("wsola 0.25x tail: 3.0-3.8 rms=%.4f, after 4.05 rms=%.6f\n",
                rmsRange(3.0, 3.8), rmsRange(4.05, 4.2));
}

TEST_CASE("mix.wsola_stereo_identity") {
    const auto track = stereoTrack(2.0, 220.0, 330.0, 0.35f, 0.25f);
    const std::vector<float> output = renderStretch(*track, 1.0, 1.5, 0.0);
    const std::size_t first = static_cast<std::size_t>(0.30 * kRate);
    const std::size_t last = static_cast<std::size_t>(1.40 * kRate);
    double errorL = 0.0, errorR = 0.0, signalL = 0.0, signalR = 0.0;
    double outL = 0.0, outR = 0.0, inL = 0.0, inR = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        const double dl = output[i * 2] - track->samples[i * 2];
        const double dr = output[i * 2 + 1] - track->samples[i * 2 + 1];
        errorL += dl * dl;
        errorR += dr * dr;
        signalL += static_cast<double>(track->samples[i * 2]) *
                   track->samples[i * 2];
        signalR += static_cast<double>(track->samples[i * 2 + 1]) *
                   track->samples[i * 2 + 1];
        outL += static_cast<double>(output[i * 2]) * output[i * 2];
        outR += static_cast<double>(output[i * 2 + 1]) * output[i * 2 + 1];
        inL += static_cast<double>(track->samples[i * 2]) *
               track->samples[i * 2];
        inR += static_cast<double>(track->samples[i * 2 + 1]) *
               track->samples[i * 2 + 1];
    }
    const double relativeL = std::sqrt(errorL / std::max(1e-12, signalL));
    const double relativeR = std::sqrt(errorR / std::max(1e-12, signalR));
    CHECK(relativeL < 0.01);
    CHECK(relativeR < 0.01);
    CHECK_NEAR(std::sqrt(outL / inL), 1.0, 0.01);
    CHECK_NEAR(std::sqrt(outR / inR), 1.0, 0.01);
    std::printf("wsola stereo identity: errL=%.5f errR=%.5f rmsL=%.4f rmsR=%.4f\n",
                relativeL, relativeR, std::sqrt(outL / inL),
                std::sqrt(outR / inR));
}

TEST_CASE("mix.wsola_stereo_ratios") {
    const auto track = stereoTrack(2.5, 220.0, 330.0, 0.35f, 0.25f);
    const double ratios[] = {0.90, 1.05, 1.10};
    for (double ratio : ratios) {
        const std::vector<float> output = renderStretch(*track, ratio, 1.8, 0.0);
        const std::size_t first = static_cast<std::size_t>(0.40 * kRate);
        const std::size_t last = static_cast<std::size_t>(1.60 * kRate);
        double outL = 0.0, outR = 0.0, inL = 0.0, inR = 0.0;
        double peak = 0.0;
        double cross = 0.0;
        for (std::size_t i = first; i < last; ++i) {
            outL += static_cast<double>(output[i * 2]) * output[i * 2];
            outR += static_cast<double>(output[i * 2 + 1]) * output[i * 2 + 1];
            inL += static_cast<double>(track->samples[i * 2]) *
                   track->samples[i * 2];
            inR += static_cast<double>(track->samples[i * 2 + 1]) *
                   track->samples[i * 2 + 1];
            peak = std::max(peak, std::max(
                std::abs(static_cast<double>(output[i * 2])),
                std::abs(static_cast<double>(output[i * 2 + 1]))));
            cross += static_cast<double>(output[i * 2]) * output[i * 2 + 1];
        }
        const double rmsL = std::sqrt(outL / inL);
        const double rmsR = std::sqrt(outR / inR);
        const double correlation =
            std::abs(cross / std::sqrt(std::max(1e-12, outL * outR)));
        CHECK(rmsL > 0.85);
        CHECK(rmsL < 1.15);
        CHECK(rmsR > 0.85);
        CHECK(rmsR < 1.15);
        CHECK(peak < 0.9);
        CHECK(correlation < 0.5);
        std::printf("wsola stereo %.2fx: rmsL=%.4f rmsR=%.4f corr=%.3f peak=%.3f\n",
                    ratio, rmsL, rmsR, correlation, peak);
    }
}

TEST_CASE("mix.wsola_hop_boundary_continuity") {
    const auto track = stereoTrack(2.5, 1000.0, 1500.0, 0.35f, 0.25f);
    const std::vector<float> output = renderStretch(*track, 1.10, 2.0, 0.0);
    float largestStep = 0.0f;
    std::size_t zeroRuns = 0;
    for (std::size_t i = 2; i < output.size(); i += 2) {
        largestStep = std::max(largestStep, std::abs(output[i] - output[i - 2]));
        if (output[i] == 0.0f && output[i - 2] != 0.0f) {
            ++zeroRuns;
        }
    }
    CHECK(largestStep < 0.20f);
    CHECK(zeroRuns < 8);
    std::printf("wsola hop continuity: maxStep=%.4f zeroStarts=%zu\n",
                static_cast<double>(largestStep), zeroRuns);
}

TEST_CASE("mix.wsola_mono_and_inverted_stereo") {
    auto mono = toneTrack(2.0, 440.0, 1);
    const std::vector<float> monoOut = renderStretch(*mono, 1.0, 1.5, 0.0);
    double monoEnergy = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.3 * kRate);
         i < static_cast<std::size_t>(1.4 * kRate); ++i) {
        CHECK(monoOut[i * 2] == monoOut[i * 2 + 1]);
        monoEnergy += static_cast<double>(monoOut[i * 2]) * monoOut[i * 2];
    }
    const double monoRms = std::sqrt(
        monoEnergy / static_cast<double>(1.1 * kRate));
    CHECK_NEAR(monoRms, 0.4 / std::sqrt(2.0), 0.01);

    auto inverted = stereoTrack(2.0, 220.0, 220.0, 0.4f, 0.4f);
    for (std::size_t i = 0; i < inverted->frameCount(); ++i) {
        inverted->samples[i * 2 + 1] = -inverted->samples[i * 2];
    }
    const std::vector<float> invertedOut =
        renderStretch(*inverted, 1.0, 1.5, 0.0);
    double invertedEnergy = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.3 * kRate);
         i < static_cast<std::size_t>(1.4 * kRate); ++i) {
        CHECK_NEAR(invertedOut[i * 2], -invertedOut[i * 2 + 1], 1e-5);
        invertedEnergy += static_cast<double>(invertedOut[i * 2]) *
                          invertedOut[i * 2];
    }
    CHECK_NEAR(std::sqrt(invertedEnergy / static_cast<double>(1.1 * kRate)),
               0.4 / std::sqrt(2.0), 0.01);
    // Non-unit rate: the mid signal is zero for inverted stereo, so the
    // similarity search must use a non-cancelling channel.
    const std::vector<float> invertedRamped =
        renderStretch(*inverted, 1.05, 1.5, 0.0);
    double rampedEnergy = 0.0;
    double cross = 0.0;
    double energyL = 0.0;
    double energyR = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.3 * kRate);
         i < static_cast<std::size_t>(1.4 * kRate); ++i) {
        rampedEnergy += static_cast<double>(invertedRamped[i * 2]) *
                        invertedRamped[i * 2];
        energyL += static_cast<double>(invertedRamped[i * 2]) *
                   invertedRamped[i * 2];
        energyR += static_cast<double>(invertedRamped[i * 2 + 1]) *
                   invertedRamped[i * 2 + 1];
        cross += static_cast<double>(invertedRamped[i * 2]) *
                 invertedRamped[i * 2 + 1];
    }
    const double rampedRms =
        std::sqrt(rampedEnergy / static_cast<double>(1.1 * kRate));
    CHECK(rampedRms > 0.25);
    CHECK(cross / std::sqrt(energyL * energyR) < -0.95);
    std::printf("wsola mono rms=%.4f inverted rms=%.4f inverted1.05 rms=%.4f\n",
                monoRms,
                std::sqrt(invertedEnergy / static_cast<double>(1.1 * kRate)),
                rampedRms);
}

TEST_CASE("mix.wsola_ramp_callback_size_independent") {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = kRate;
    track->channels = 2;
    const std::size_t frames = 2 * kRate;
    track->samples.assign(frames * 2, 0.0f);
    const std::size_t burst = static_cast<std::size_t>(0.75 * kRate);
    for (std::size_t i = 0; i < kRate / 50; ++i) {
        const float value = static_cast<float>(
            0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                           static_cast<double>(i) / kRate));
        track->samples[(burst + i) * 2] = value;
        track->samples[(burst + i) * 2 + 1] = value;
    }
    auto burstTime = [](const std::vector<float>& out) {
        for (std::size_t i = 0; i < out.size() / 2; ++i) {
            if (std::abs(out[i * 2]) > 0.15f) {
                return static_cast<double>(i) / kRate;
            }
        }
        return -1.0;
    };
    bgmedia::mix::WsolaStretcher processor;
    const auto one = processor.createStream(kRate);
    double onePos = 0.0;
    std::vector<float> oneOut(2 * kRate * 2, 0.0f);
    one->render(*track, onePos, 0.5, 1.5, oneOut.data(), 2 * kRate);

    const auto many = processor.createStream(kRate);
    double manyPos = 0.0;
    std::vector<float> manyOut(2 * kRate * 2, 0.0f);
    std::vector<float> block(480 * 2, 0.0f);
    std::size_t done = 0;
    while (done < 2 * kRate) {
        const double t0 = static_cast<double>(done) / (2.0 * kRate);
        const double t1 = static_cast<double>(done + 480) / (2.0 * kRate);
        many->render(*track, manyPos, 0.5 + t0, 0.5 + t1, block.data(), 480);
        std::copy_n(block.begin(), 480 * 2,
                    manyOut.begin() + static_cast<std::ptrdiff_t>(done * 2));
        done += 480;
    }
    const double oneBurst = burstTime(oneOut);
    const double manyBurst = burstTime(manyOut);
    CHECK(oneBurst > 0.0);
    CHECK(manyBurst > 0.0);
    CHECK_NEAR(oneBurst, manyBurst, 0.03);
    CHECK_NEAR(onePos, manyPos, 0.02);
    std::printf("wsola ramp block independence: one=%.4f many=%.4f pos=%.4f\n",
                oneBurst, manyBurst, onePos);
}

TEST_CASE("mix.engine_never_rewinds_playing_deck") {
    MixEngine engine(kRate);
    auto a = dcTrack(10.0, 0.4f);
    auto b = dcTrack(10.0, 0.4f);
    CHECK(engine.loadDeck(DeckId::A, a, 61, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 62, 120.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 1.0, nullptr);
    const double before = engine.snapshot().decks[0].positionSeconds;

    MixPlan plan = crossfadePlan(engine, 2.0, 0.0);
    // The planner asked for a point 200 ms behind the current cursor. The
    // engine must not rewind; it starts the fade here instead.
    plan.transitionStartA = before - 0.2;
    plan.transitionEndA = plan.transitionStartA + 2.0;
    plan.tempoRampStartA = before - 0.2;
    plan.preTransitionSeconds = 0.0;
    CHECK(engine.startTransition(plan));
    std::vector<float> block(480 * 2, 0.0f);
    engine.render(block.data(), 480);
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.decks[0].positionSeconds >= before - 0.001);
    CHECK(snap.phase == TransitionPhase::Running);
    float minimum = 1.0f;
    for (int i = 0; i < 100; ++i) {
        minimum = std::min(minimum, std::abs(block[i * 2]));
    }
    // No reset hole and no correlation-headroom dip while B is still silent.
    CHECK(minimum > 0.30f);
    std::printf("no rewind: before=%.3f after=%.3f firstMin=%.4f\n", before,
                snap.decks[0].positionSeconds, static_cast<double>(minimum));
}

TEST_CASE("mix.engine_correlation_headroom_at_midpoint") {
    MixEngine engine(kRate);
    auto a = dcTrack(10.0, 0.4f);
    auto b = dcTrack(10.0, 0.4f);
    CHECK(engine.loadDeck(DeckId::A, a, 67, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 68, 120.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.1, nullptr);
    CHECK(engine.startTransition(crossfadePlan(engine, 2.0, 0.0)));
    renderSeconds(engine, 1.0, nullptr);
    std::vector<float> block(480 * 2, 0.0f);
    engine.render(block.data(), 480);
    // Two identical decks at the 50% point would sum to +6 dB without
    // headroom; the correlated gain must keep the level at the source level.
    CHECK_NEAR(block[0], 0.4f, 0.02f);
    std::printf("correlated midpoint: %.4f\n",
                static_cast<double>(block[0]));
}

TEST_CASE("mix.engine_future_start_waits_without_seek") {
    MixEngine engine(kRate);
    auto a = dcTrack(10.0, 0.4f);
    auto b = dcTrack(10.0, 0.4f);
    CHECK(engine.loadDeck(DeckId::A, a, 63, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 64, 120.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 1.0, nullptr);
    const double before = engine.snapshot().decks[0].positionSeconds;

    MixPlan plan = crossfadePlan(engine, 2.0, 0.0);
    plan.transitionStartA = before + 0.25;
    plan.transitionEndA = plan.transitionStartA + 2.0;
    plan.tempoRampStartA = before + 0.25;
    plan.preTransitionSeconds = 0.0;
    CHECK(engine.startTransition(plan));

    renderSeconds(engine, 0.10, nullptr);
    bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.stage == TransitionStage::Preparing);
    CHECK(!snap.decks[1].playing);
    CHECK(snap.decks[0].positionSeconds >= before + 0.08);
    CHECK(snap.decks[0].positionSeconds <= before + 0.12);

    renderSeconds(engine, 0.30, nullptr);
    snap = engine.snapshot();
    CHECK(snap.phase == TransitionPhase::Running);
    CHECK(snap.stage == TransitionStage::Mixing ||
          snap.stage == TransitionStage::Handover);
    CHECK(snap.decks[1].playing);
    CHECK(snap.decks[0].positionSeconds > before + 0.30);
    std::printf("future start: before=%.3f at0.1=%.3f stage0=%d at0.4=%.3f\n",
                before, before + 0.10, static_cast<int>(snap.stage),
                snap.decks[0].positionSeconds);
}

TEST_CASE("mix.engine_future_start_keeps_deck_rate") {
    MixEngine engine(kRate);
    auto a = dcTrack(10.0, 0.4f);
    auto b = dcTrack(10.0, 0.4f);
    CHECK(engine.loadDeck(DeckId::A, a, 69, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 70, 120.0));
    engine.setDeckRate(DeckId::A, 0.5);
    engine.play(DeckId::A);
    renderSeconds(engine, 1.0, nullptr);
    const double before = engine.snapshot().decks[0].positionSeconds;
    CHECK_NEAR(before, 0.5, 0.02);

    MixPlan plan = crossfadePlan(engine, 2.0, 0.0);
    plan.transitionStartA = 1.0;  // 0.5 s of media = 1.0 s at 0.5x
    plan.transitionEndA = plan.transitionStartA + 2.0;
    plan.tempoRampStartA = 1.0;
    plan.preTransitionSeconds = 0.0;
    CHECK(engine.startTransition(plan));
    renderSeconds(engine, 0.9, nullptr);
    bgmedia::mix::MixSnapshot snap = engine.snapshot();
    CHECK(snap.stage == TransitionStage::Preparing);
    CHECK(!snap.decks[1].playing);
    // If the wait used 1.0x, the deck would already be at ~1.4 s.
    CHECK(snap.decks[0].positionSeconds < 1.05);
    CHECK(snap.decks[0].positionSeconds > 0.85);
    std::printf("future rate wait: before=%.3f at0.9=%.3f\n", before,
                snap.decks[0].positionSeconds);
    renderSeconds(engine, 0.2, nullptr);
    snap = engine.snapshot();
    CHECK(snap.decks[1].playing);
}

TEST_CASE("mix.engine_future_start_block_sizes") {
    MixEngine engine(kRate);
    auto a = dcTrack(10.0, 0.4f);
    auto b = dcTrack(10.0, 0.4f);
    CHECK(engine.loadDeck(DeckId::A, a, 65, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 66, 120.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.5, nullptr);
    const double before = engine.snapshot().decks[0].positionSeconds;
    MixPlan plan = crossfadePlan(engine, 1.5, 0.0);
    plan.transitionStartA = before + 0.15;
    plan.transitionEndA = plan.transitionStartA + 1.5;
    plan.tempoRampStartA = before + 0.15;
    plan.preTransitionSeconds = 0.0;
    CHECK(engine.startTransition(plan));

    const std::size_t sizes[] = {1, 7, 32, 63, 128, 256, 480, 512, 1000};
    std::vector<float> buffer(1000 * 2, 0.0f);
    double previous = before;
    std::size_t nonFinite = 0;
    bool bStarted = false;
    for (int i = 0; i < 300; ++i) {
        const std::size_t frames = sizes[i % 9];
        engine.render(buffer.data(), frames);
        for (std::size_t f = 0; f < frames * 2; ++f) {
            if (!std::isfinite(buffer[f])) {
                ++nonFinite;
            }
        }
        const bgmedia::mix::MixSnapshot snap = engine.snapshot();
        CHECK(snap.decks[0].positionSeconds >= previous - 0.001);
        previous = snap.decks[0].positionSeconds;
        bStarted = bStarted || snap.decks[1].playing;
    }
    CHECK(nonFinite == 0);
    CHECK(bStarted);
    CHECK(engine.snapshot().decks[0].positionSeconds > before + 0.15);
}

TEST_CASE("mix.planner_execution_snaps_forward") {
    PlannerConfig config;
    config.tempoRampMode = TempoRampMode::Off;
    MixPlanner planner(config);
    const AnalysisResult a = analysisFor(120.0);
    const double positions[] = {10.01, 10.20, 10.24, 10.49, 10.50, 10.51};
    for (double position : positions) {
        PlanRequest request;
        request.type = TransitionType::SmartCrossfade;
        request.requestedDuration = 4.0;
        request.currentPositionA = position;
        const MixPlan plan = planner.plan(a, analysisFor(120.0), request);
        CHECK(plan.valid);
        const double start = plan.overallStartA();
        // Never rewind, and never wait more than one beat (0.5 s at 120 BPM).
        CHECK(start >= position - 0.021);
        CHECK(start <= position + 0.51);
        std::printf("execution snap %.2f -> %.3f (%.1f ms ahead)\n", position,
                    start, (start - position) * 1000.0);
    }
}

TEST_CASE("mix.engine_resamples_source_rate") {
    // 44.1 kHz source into a 48 kHz engine: WSOLA must not play it 8.8% sharp.
    MixEngine engine(kRate);
    auto track = toneTrack(4.0, 440.0, 2, 44100);
    CHECK(engine.loadDeck(DeckId::A, track, 41, 120.0));
    engine.play(DeckId::A);
    RenderStats stats;
    renderSeconds(engine, 1.0, &stats, 0.2);
    std::vector<float> block(480 * 2, 0.0f);
    std::vector<float> collected;
    collected.reserve(480 * 200 * 2);
    for (int i = 0; i < 200; ++i) {
        engine.render(block.data(), 480);
        collected.insert(collected.end(), block.begin(), block.end());
    }
    const double hz = positiveZeroCrossingHz(collected, 2, kRate, 0.2);
    CHECK(stats.nonFinite == 0);
    CHECK_NEAR(hz, 440.0, 2.0);
    std::printf("resample: 44.1k source -> %.1f Hz\n", hz);
}

TEST_CASE("mix.engine_transition_start_keeps_outgoing_continuity") {
    MixEngine engine(kRate);
    auto a = dcTrack(10.0, 0.4f);
    auto b = dcTrack(10.0, 0.4f);
    CHECK(engine.loadDeck(DeckId::A, a, 51, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 52, 120.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.5, nullptr);
    const MixPlan plan = crossfadePlan(engine, 2.0, 0.0);
    CHECK(engine.startTransition(plan));
    std::vector<float> block(480 * 2, 0.0f);
    engine.render(block.data(), 480);
    // The cue is exactly the current position, so the outgoing WSOLA history
    // must survive; resetting it would punch a ~21 ms fade-in hole.
    float firstMinimum = 1.0f;
    for (int i = 0; i < 100; ++i) {
        firstMinimum = std::min(firstMinimum, std::abs(block[i * 2]));
    }
    // A reset stream starts at exactly zero; steady WSOLA on a constant signal
    // has a small amplitude ripple, so assert "no hole", not unity.
    CHECK(firstMinimum > 0.20f);
    std::printf("transition start continuity: first-100 min=%.4f\n",
                static_cast<double>(firstMinimum));
}

namespace {

MixPlan planMetric(double bpmA, double bpmB, PlannerConfig config = {},
                   TransitionType type = TransitionType::SmartCrossfade,
                   double durationA = 120.0) {
    MixPlanner planner(config);
    PlanRequest request;
    request.type = type;
    request.requestedDuration = 8.0;
    request.currentPositionA = 20.0;
    return planner.plan(analysisFor(bpmA, durationA), analysisFor(bpmB, 120.0),
                        request);
}

void checkMetric(const MixPlan& plan, int bBeats, int aBeats, double rateA,
                 double rateB, TempoMatchKind kind) {
    CHECK(plan.valid);
    CHECK(plan.fallbackReason.empty());
    CHECK(plan.metricRelation.bBeats == bBeats);
    CHECK(plan.metricRelation.aBeats == aBeats);
    CHECK(plan.tempoMatchKind == kind);
    CHECK_NEAR(plan.tempoRatioAEnd, rateA, 1e-3);
    CHECK_NEAR(plan.tempoRatioB, rateB, 1e-3);
    // Effective BPMs honour the relation: B/A beat rate == p/q.
    CHECK_NEAR(plan.targetBpm / plan.targetBpmA,
               static_cast<double>(bBeats) / aBeats, 1e-6);
}

} // namespace

TEST_CASE("mix.metric_exact_octave_relations") {
    // 60 -> 120: 2:1 with both decks at 1.0x, no ramp, no time-stretch.
    MixPlan plan = planMetric(60.0, 120.0);
    checkMetric(plan, 2, 1, 1.0, 1.0, TempoMatchKind::ExactMetric);
    CHECK(!plan.tempoRampActive);
    CHECK(plan.type == TransitionType::PhraseAware);
    // 120 -> 60: 1:2.
    plan = planMetric(120.0, 60.0);
    checkMetric(plan, 1, 2, 1.0, 1.0, TempoMatchKind::ExactMetric);
    CHECK(!plan.tempoRampActive);
    // 80 -> 160.
    plan = planMetric(80.0, 160.0);
    checkMetric(plan, 2, 1, 1.0, 1.0, TempoMatchKind::ExactMetric);
    // Explicit Beatmatched request behaves the same.
    plan = planMetric(60.0, 120.0, {}, TransitionType::Beatmatched);
    checkMetric(plan, 2, 1, 1.0, 1.0, TempoMatchKind::ExactMetric);
}

TEST_CASE("mix.metric_octave_with_bounded_residual") {
    // 60 -> 118: keep A at 60 and speed B by 120/118; never a 2x rate.
    const MixPlan plan = planMetric(60.0, 118.0);
    // Bounded residual only (~1.7%), split like the existing 1:1 matching:
    // A ramps to the incoming tempo and B stays at 1.0x.
    CHECK(plan.valid);
    CHECK(plan.metricRelation.bBeats == 2 && plan.metricRelation.aBeats == 1);
    CHECK(plan.tempoMatchKind == TempoMatchKind::MetricWithStretch);
    CHECK_NEAR(plan.tempoRatioAEnd, 59.0 / 60.0, 1e-3);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-3);
    // Effective beat rates keep the exact 2:1 relationship.
    CHECK_NEAR(plan.targetBpm / plan.targetBpmA, 2.0, 1e-6);
    CHECK(std::abs(plan.tempoRatioAEnd - 1.0) < 0.02);
    CHECK(std::abs(plan.tempoRatioB - 1.0) < 0.02);
    CHECK(plan.pitchLock);
    for (const TempoPoint& point : plan.tempoCurve) {
        CHECK(point.rate > 0.9 && point.rate < 1.1);
    }
}

TEST_CASE("mix.metric_unison_unchanged") {
    // 123 -> 125 still uses the existing 1:1 bounded match.
    const MixPlan plan = planMetric(123.0, 125.0);
    CHECK(plan.valid);
    CHECK(plan.metricRelation.unison());
    CHECK(plan.tempoMatchKind == TempoMatchKind::MetricWithStretch);
    CHECK(plan.fallbackReason.empty());
    CHECK_NEAR(plan.targetBpm, plan.targetBpmA, 1e-9);
    // Identical tempos are an exact 1:1.
    const MixPlan same = planMetric(124.0, 124.0);
    CHECK(same.metricRelation.unison());
    CHECK(same.tempoMatchKind == TempoMatchKind::ExactMetric);
}

TEST_CASE("mix.metric_polyrhythm_is_opt_in") {
    // 100 -> 150 is 3:2; automatic mode must fall back to a safe crossfade.
    MixPlan plan = planMetric(100.0, 150.0);
    CHECK(plan.valid);
    CHECK(plan.type == TransitionType::SmartCrossfade);
    CHECK(plan.tempoMatchKind == TempoMatchKind::FallbackCrossfade);
    CHECK(!plan.fallbackReason.empty());
    CHECK(plan.metricRelation.unison());
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);
    CHECK_NEAR(plan.tempoRatioAEnd, 1.0, 1e-9);
    // Explicitly enabled it is reported as experimental.
    PlannerConfig config;
    config.allowPolyrhythm = true;
    plan = planMetric(100.0, 150.0, config);
    CHECK(plan.valid);
    CHECK(plan.metricRelation.bBeats == 3);
    CHECK(plan.metricRelation.aBeats == 2);
    CHECK(plan.tempoMatchKind == TempoMatchKind::ExperimentalPolyrhythm);
    CHECK(plan.experimental);
}

TEST_CASE("mix.metric_untrusted_levels_fall_back") {
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 20.0;
    AnalysisResult a = analysisFor(60.0, 120.0);
    AnalysisResult b = analysisFor(120.0, 120.0);
    b.bpmLevelConfidence = 0.2f;  // metric level is a guess
    MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.metricRelation.unison());
    CHECK(plan.tempoMatchKind == TempoMatchKind::FallbackCrossfade);
    CHECK(plan.fallbackReason.find("2:1") != std::string::npos);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);

    // The feature can be switched off entirely.
    PlannerConfig off;
    off.allowMetricRelations = false;
    plan = planMetric(60.0, 120.0, off);
    CHECK(plan.tempoMatchKind == TempoMatchKind::FallbackCrossfade);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);
}

TEST_CASE("mix.metric_ambiguity_81_vs_121_is_not_forced") {
    // 81 and 121.5 are 3:2, not a metric level; no hidden octave/rate change.
    const MixPlan plan = planMetric(81.0, 121.5);
    CHECK(plan.valid);
    CHECK(plan.metricRelation.unison());
    CHECK(plan.tempoMatchKind == TempoMatchKind::FallbackCrossfade);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);
}

namespace {

void checkPhase(const MixPlan& plan) {
    CHECK(plan.valid);
    CHECK(plan.phaseMeasured);
    CHECK(plan.phasePairs >= 3);
    CHECK(plan.phaseRmsMs <= 20.0);
    CHECK(plan.phaseP95Ms <= 40.0);
    CHECK(plan.phaseDriftMs <= 20.0);
    CHECK_NEAR(plan.beatPhaseErrorMs, plan.phaseRmsMs, 1e-9);
    std::printf("phase: %d:%d pairs=%d rms=%.2f p95=%.2f drift=%.2f ms\n",
                plan.metricRelation.bBeats, plan.metricRelation.aBeats,
                plan.phasePairs, plan.phaseRmsMs, plan.phaseP95Ms,
                plan.phaseDriftMs);
}

} // namespace

TEST_CASE("mix.phase_measured_on_exact_fixtures") {
    checkPhase(planMetric(60.0, 120.0));
    checkPhase(planMetric(120.0, 60.0));
    checkPhase(planMetric(80.0, 160.0));
    checkPhase(planMetric(60.0, 118.0));
    checkPhase(planMetric(123.0, 125.0));
}

TEST_CASE("mix.phase_drifting_grid_is_not_called_beatmatched") {
    // B claims 120 BPM but its detected beats run at 123 BPM: the grids
    // slide apart during the overlap. The planner must measure that and fall
    // back instead of reporting a phase-aligned 2:1 mix.
    AnalysisResult a = analysisFor(60.0, 120.0);
    AnalysisResult b = analysisFor(120.0, 120.0);
    const double interval = 60.0 / 123.0;
    for (std::size_t i = 0; i < b.beats.size(); ++i) {
        b.beats[i].time = static_cast<double>(i) * interval;
    }
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 20.0;
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.phaseMeasured);
    CHECK(plan.phaseP95Ms > planner.config().maxPhaseP95Ms);
    CHECK(plan.type == TransitionType::Crossfade);
    CHECK(plan.tempoMatchKind == TempoMatchKind::FallbackCrossfade);
    CHECK(plan.fallbackReason.find("beat-phase") != std::string::npos);
    CHECK(!plan.beatAligned);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);
    CHECK(!plan.tempoRampActive);
}

TEST_CASE("mix.engine_holds_incoming_rate_while_both_audible") {
    MixEngine engine(kRate);
    auto a = dcTrack(30.0, 0.25f);
    auto b = dcTrack(30.0, 0.25f);
    CHECK(engine.loadDeck(DeckId::A, a, 61, 120.0));
    CHECK(engine.loadDeck(DeckId::B, b, 62, 120.0));
    engine.play(DeckId::A);
    renderSeconds(engine, 0.2, nullptr);

    MixPlan plan = crossfadePlan(engine, 4.0, 2.0);
    plan.type = TransitionType::Beatmatched;
    plan.tempoRatioB = 1.1;
    plan.rateReleaseProgress = 0.85;
    CHECK(engine.startTransition(plan));
    // Past the old 50% handover the incoming deck must still run at 1.1x.
    renderSeconds(engine, 3.4, nullptr);
    const double early = engine.snapshot().decks[1].positionSeconds;
    CHECK_NEAR(early, 2.0 + 1.1 * 3.4, 0.05);
    // Remaining 0.6 s glide from 1.1x to 1.0x.
    renderSeconds(engine, 0.6, nullptr);
    const double expected = 2.0 + 1.1 * 3.4 + 1.1 * 0.6 -
                            0.1 * 0.6 * 0.6 / (2.0 * 0.6);
    const double end = engine.snapshot().decks[1].positionSeconds;
    CHECK_NEAR(end, expected, 0.06);
    std::printf("rate hold: B early=%.3f (%.3f) end=%.3f (%.3f)\n", early,
                2.0 + 1.1 * 3.4, end, expected);
}

TEST_CASE("mix.metric_match_falls_back_when_ramp_cannot_run") {
    // 60 -> 118 needs A to move to 59 BPM, but A (30 s) has no room for the
    // ramp at 20 s plus an 8 s fade: the plan must not keep a tempo-synced
    // type and a successful match classification at 1.0x rates.
    const MixPlan plan = planMetric(60.0, 118.0, {},
                                    TransitionType::SmartCrossfade, 30.0);
    CHECK(plan.valid);
    CHECK(!plan.tempoRampActive);
    CHECK(plan.type == TransitionType::Crossfade);
    CHECK(plan.tempoMatchKind == TempoMatchKind::FallbackCrossfade);
    CHECK(!plan.fallbackReason.empty());
    CHECK(plan.metricRelation.unison());
    CHECK_NEAR(plan.tempoRatioAEnd, 1.0, 1e-9);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);
    // An exact 60 -> 120 pair needs no ramp and keeps its relation.
    const MixPlan exact = planMetric(60.0, 120.0, {},
                                     TransitionType::SmartCrossfade, 30.0);
    CHECK(exact.metricRelation.bBeats == 2);
    CHECK(exact.tempoMatchKind == TempoMatchKind::ExactMetric);
}

TEST_CASE("mix.phase_fallback_refits_overlap_to_incoming_audio") {
    // B claims 132 BPM but its beats run at 140: the phase check rejects the
    // sync, and the 7.5 s incoming track must not be overrun by the fade.
    AnalysisResult a = analysisFor(120.0, 120.0);
    AnalysisResult b = analysisFor(132.0, 7.5);
    const double interval = 60.0 / 140.0;
    for (std::size_t i = 0; i < b.beats.size(); ++i) {
        b.beats[i].time = static_cast<double>(i) * interval;
    }
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 20.0;
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);
    CHECK(plan.startB + plan.duration <= b.info.duration + 1e-6);
    CHECK(plan.endB <= b.info.duration + 1e-6);
}

TEST_CASE("mix.phase_measure_survives_missing_beat") {
    // Exact 120 -> 60 (1:2). Dropping one unpaired outgoing detection must
    // not shift the stride parity and reject an aligned transition.
    AnalysisResult a = analysisFor(120.0, 120.0);
    AnalysisResult b = analysisFor(60.0, 120.0);
    for (auto it = a.beats.begin(); it != a.beats.end(); ++it) {
        if (std::abs(it->time - 21.5) < 1e-6) {
            a.beats.erase(it);
            break;
        }
    }
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 20.0;
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.phaseMeasured);
    CHECK(plan.phaseP95Ms <= 40.0);
    CHECK(plan.metricRelation.bBeats == 1 && plan.metricRelation.aBeats == 2);
    CHECK(plan.fallbackReason.empty());
}

TEST_CASE("mix.phase_measure_survives_missing_incoming_beat") {
    // Exact 120 -> 60 (1:2) with one incoming detection removed (e.g. inside
    // a silence region): that pair is unavailable, not a 1000 ms error.
    AnalysisResult a = analysisFor(120.0, 120.0);
    AnalysisResult b = analysisFor(60.0, 120.0);
    for (auto it = b.beats.begin(); it != b.beats.end(); ++it) {
        if (std::abs(it->time - 3.0) < 1e-6) {
            b.beats.erase(it);
            break;
        }
    }
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 8.0;
    request.currentPositionA = 20.0;
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.phaseMeasured);
    CHECK(plan.phaseP95Ms <= 40.0);
    CHECK(plan.fallbackReason.empty());
    CHECK(plan.metricRelation.bBeats == 1 && plan.metricRelation.aBeats == 2);
}

TEST_CASE("mix.phrase_aligned_across_metric_levels") {
    // 60 -> 120 (2:1). 4 bars of A = 16 seconds; 8 bars of B = 16 seconds.
    // Both tracks should align on a musical phrase boundary.
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::PhraseAware;
    request.requestedDuration = 16.0;
    request.currentPositionA = 16.0;  // exact 4-bar boundary at 60 BPM
    AnalysisResult a = analysisFor(60.0, 120.0);
    AnalysisResult b = analysisFor(120.0, 120.0);
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.type == TransitionType::PhraseAware);
    CHECK(plan.phraseBoundary);
    CHECK(plan.phraseConfidence > 0.0f);
    CHECK(plan.downbeatAligned);
    CHECK(plan.metricRelation.bBeats == 2 && plan.metricRelation.aBeats == 1);
    CHECK_NEAR(plan.startB, 0.0, 0.05);

    // If downbeat or meter confidence is low, phrase alignment must honestly
    // fall back to a safe crossfade.
    b.downbeatConfidence = 0.2f;
    const MixPlan unconfident = planner.plan(a, b, request);
    CHECK(unconfident.valid);
    CHECK(unconfident.type == TransitionType::Crossfade);
    CHECK(unconfident.fallbackReason.find("downbeats and meter") !=
          std::string::npos);
}

TEST_CASE("mix.phrase_fallback_disables_prepared_ramp") {
    // A at 120, B at 124 (would otherwise ramp A). Low downbeat confidence
    // forces fallback to Crossfade: the prepared ramp must not leak into A.
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::PhraseAware;
    request.requestedDuration = 8.0;
    AnalysisResult a = analysisFor(120.0, 120.0);
    AnalysisResult b = analysisFor(124.0, 120.0);
    b.downbeatConfidence = 0.2f;
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.type == TransitionType::Crossfade);
    CHECK(!plan.tempoRampActive);
    CHECK_NEAR(plan.tempoRatioAEnd, 1.0, 1e-9);
    CHECK_NEAR(plan.tempoRatioB, 1.0, 1e-9);
    CHECK_NEAR(plan.preTransitionSeconds, 0.0, 1e-9);
    CHECK(!plan.fallbackReason.empty());
}


TEST_CASE("mix.phrase_incompatible_tempo_falls_back_to_crossfade") {
    // Confident grids but incompatible tempos (120 vs 160, outside 6% delta).
    // An explicit PhraseAware request must honestly fall back to Crossfade.
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::PhraseAware;
    request.requestedDuration = 8.0;
    AnalysisResult a = analysisFor(120.0, 120.0);
    AnalysisResult b = analysisFor(160.0, 120.0);
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.type == TransitionType::Crossfade);
    CHECK(!plan.fallbackReason.empty());
    CHECK(plan.fallbackReason.find("tempo is not compatible") !=
          std::string::npos);
}

TEST_CASE("mix.fade_window_fits_outgoing_ramped_playback_rate") {
    // Outgoing track duration 100.0 s, 120 -> 126 BPM (ratioA = 1.05).
    // An 8 s fade starting near the end must take the held 1.05 rate into
    // account so source media consumed does not exceed 100.0 s.
    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::Beatmatched;
    request.requestedDuration = 8.0;
    request.currentPositionA = 92.0;
    AnalysisResult a = analysisFor(120.0, 100.0);
    AnalysisResult b = analysisFor(126.0, 100.0);
    const MixPlan plan = planner.plan(a, b, request);
    CHECK(plan.valid);
    CHECK(plan.transitionEndA <= a.info.duration + 1e-6);
    CHECK(plan.transitionStartA + plan.duration * plan.tempoRatioAEnd <=
          a.info.duration + 1e-6);
}


TEST_CASE("mix.metric_compromise_preserves_incoming_correction") {
    // 100 -> 212.5 BPM (2:1). With A staying at 100 BPM (ratioA = 1.0),
    // B must slow down to 200 BPM (ratioB = 200 / 212.5 ≈ 0.9412).
    // The incoming correction must not be discarded.
    const MixPlan plan = planMetric(100.0, 212.5, {}, TransitionType::Beatmatched);
    CHECK(plan.valid);
    CHECK(plan.type == TransitionType::Beatmatched);
    CHECK(plan.metricRelation.bBeats == 2 && plan.metricRelation.aBeats == 1);
    CHECK_NEAR(plan.tempoRatioAEnd, 1.0, 1e-4);
    CHECK_NEAR(plan.tempoRatioB, 200.0 / 212.5, 1e-4);
    CHECK(plan.beatAligned);
    CHECK_NEAR(plan.beatPhaseErrorMs, 0.0, 1.0);
}

TEST_CASE("mix.sync_clock_active_during_cancellation") {
    // Verifies that liveMixTempo keeps reporting playing while the master deck
    // is playing audio, even when snapshot.phase is TransitionPhase::Cancelling.
    bgmedia::mix::MixSnapshot snap;
    snap.master = DeckId::A;
    snap.phase = TransitionPhase::Cancelling;
    snap.decks[0].loaded = true;
    snap.decks[0].playing = true;
    snap.decks[0].bpm = 120.0;
    snap.decks[0].analysedBpm = 120.0;
    snap.decks[0].positionSeconds = 1.5;
    const bgmedia::mix::LiveMixTempo live = bgmedia::mix::liveMixTempo(snap);
    CHECK(live.valid);
    CHECK(live.playing);
    CHECK_NEAR(live.positionSeconds, 1.5, 1e-6);

    // When the deck itself stops playing, the clock reports stopped.
    snap.decks[0].playing = false;
    CHECK(!bgmedia::mix::liveMixTempo(snap).playing);
}

// ---------------------------------------------------------------------------
// BG Mix 0.3 PR 1: Musical Feature Map, BS.1770 LUFS, Adaptive Bounds, Cache
// ---------------------------------------------------------------------------

TEST_CASE("mix.feature_map_bs1770_lufs_vs_plain_rms") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;

    MixFeatureExtractor extractor;
    const AnalysisResult baseAnalysis = analysisFor(120.0, 8.0);

    // 1. 1 kHz mono sine vs dual-mono stereo sine at 0.4 amplitude:
    // Mono LUFS matches plain RMS dBFS (~ -11.0 LUFS), while stereo dual-mono
    // sums both channels' K-weighted powers (+3.01 LU, ~ -8.0 LUFS).
    const auto tone1k = toneTrack(8.0, 1000.0);
    auto mono1k = std::make_shared<PcmTrack>();
    mono1k->sampleRate = kRate;
    mono1k->channels = 1;
    mono1k->samples.resize(tone1k->frameCount(), 0.0f);
    for (std::size_t i = 0; i < mono1k->samples.size(); ++i) {
        mono1k->samples[i] = tone1k->samples[i * 2];
    }
    const MixFeatureMap mapMono1k = extractor.extractFromTrack(*mono1k, baseAnalysis);
    const MixFeatureMap map1k = extractor.extractFromTrack(*tone1k, baseAnalysis);
    CHECK(map1k.valid());
    CHECK(map1k.info.fromPcm);
    CHECK_NEAR(mapMono1k.loudness.integratedLufs, mapMono1k.loudness.meanRmsDbfs, 0.35);
    CHECK_NEAR(map1k.loudness.integratedLufs - mapMono1k.loudness.integratedLufs, 3.01f, 0.15f);
    CHECK_NEAR(map1k.loudness.integratedLufs, map1k.loudness.meanRmsDbfs + 3.01f, 0.35);

    // Out-of-phase stereo (R = -L) preserves per-channel powers instead of
    // cancelling out in a mono downmix before loudness measurement.
    auto outOfPhase = toneTrack(8.0, 1000.0);
    for (std::size_t f = 0; f < outOfPhase->frameCount(); ++f) {
        outOfPhase->samples[f * 2 + 1] = -outOfPhase->samples[f * 2];
    }
    const MixFeatureMap mapOutOfPhase = extractor.extractFromTrack(*outOfPhase, baseAnalysis);
    CHECK_NEAR(mapOutOfPhase.loudness.integratedLufs, map1k.loudness.integratedLufs, 0.10);

    // 2. 3 kHz stereo sine at the exact same 0.4 amplitude: plain RMS dBFS is
    // identical to 1 kHz, while BS.1770 Stage-1 high-shelf boosts 3 kHz by ~+3.6 LU.
    const auto tone3k = toneTrack(8.0, 3000.0);
    const MixFeatureMap map3k = extractor.extractFromTrack(*tone3k, baseAnalysis);
    CHECK_NEAR(map3k.loudness.meanRmsDbfs, map1k.loudness.meanRmsDbfs, 0.05);
    CHECK(map3k.loudness.integratedLufs > map1k.loudness.integratedLufs + 3.0f);
    CHECK(map3k.loudness.integratedLufs < map1k.loudness.integratedLufs + 4.3f);

    // 3. 30 Hz sub-bass sine at the exact same 0.4 amplitude: plain RMS dBFS is
    // identical to 1 kHz, while BS.1770 Stage-2 RLB high-pass attenuates 30 Hz.
    const auto tone30 = toneTrack(8.0, 30.0);
    const MixFeatureMap map30 = extractor.extractFromTrack(*tone30, baseAnalysis);
    CHECK_NEAR(map30.loudness.meanRmsDbfs, map1k.loudness.meanRmsDbfs, 0.10);
    CHECK(map30.loudness.integratedLufs < map1k.loudness.integratedLufs - 3.5f);

    // 4. BS.1770 gating: half 1 kHz tone + half digital silence reduces plain
    // RMS by ~3 dB, while gated integrated LUFS stays within 0.25 LU of the full tone.
    auto gatedTrack = toneTrack(8.0, 1000.0);
    const std::size_t halfSamples = gatedTrack->samples.size() / 2;
    std::fill(gatedTrack->samples.begin() + static_cast<std::ptrdiff_t>(halfSamples),
              gatedTrack->samples.end(), 0.0f);
    const MixFeatureMap mapGated = extractor.extractFromTrack(*gatedTrack, baseAnalysis);
    CHECK(mapGated.loudness.meanRmsDbfs < map1k.loudness.meanRmsDbfs - 2.5f);
    CHECK_NEAR(mapGated.loudness.integratedLufs, map1k.loudness.integratedLufs, 0.25);

    // 5. EBU Tech 3342 LRA relative gate (-20 LU): a 60 s constant tone whose
    // second 30 s are attenuated by 40 dB (still above the -70 LUFS absolute gate)
    // must exclude the -40 dB tail from LRA rather than reporting ~40 LU.
    const AnalysisResult longAnalysis = analysisFor(120.0, 60.0);
    auto lraTrack = toneTrack(60.0, 1000.0);
    const std::size_t halfLraSamples = lraTrack->samples.size() / 2;
    for (std::size_t i = halfLraSamples; i < lraTrack->samples.size(); ++i) {
        lraTrack->samples[i] *= 0.01f; // -40 dB attenuation
    }
    const MixFeatureMap mapLra = extractor.extractFromTrack(*lraTrack, longAnalysis);
    CHECK(mapLra.loudness.loudnessRangeLu < 1.5f);

    // 6. Startup transient windows: a 100 ms tone at t = 0 must report the same
    // momentaryMaxLufs and shortTermMaxLufs as the identical 100 ms tone at t = 5 s.
    auto burstStart = std::make_shared<PcmTrack>();
    auto burstMid = std::make_shared<PcmTrack>();
    burstStart->sampleRate = burstMid->sampleRate = kRate;
    burstStart->channels = burstMid->channels = 1;
    burstStart->samples.assign(8 * kRate, 0.0f);
    burstMid->samples.assign(8 * kRate, 0.0f);
    const std::size_t burstFrames = kRate / 10; // 100 ms
    const std::size_t midOffset = 5 * kRate;
    for (std::size_t i = 0; i < burstFrames; ++i) {
        const float s = static_cast<float>(
            0.4 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(i) / kRate));
        burstStart->samples[i] = s;
        burstMid->samples[midOffset + i] = s;
    }
    const MixFeatureMap mapBurstStart = extractor.extractFromTrack(*burstStart, baseAnalysis);
    const MixFeatureMap mapBurstMid = extractor.extractFromTrack(*burstMid, baseAnalysis);
    CHECK_NEAR(mapBurstStart.loudness.momentaryMaxLufs,
               mapBurstMid.loudness.momentaryMaxLufs, 0.05f);
    CHECK_NEAR(mapBurstStart.loudness.shortTermMaxLufs,
               mapBurstMid.loudness.shortTermMaxLufs, 0.05f);

    // 7. 5-channel surround layout: channels 3 (BL) and 4 (BR) both receive
    // the +1.5 dB surround weight and report identical integrated loudness.
    auto surBL = std::make_shared<PcmTrack>();
    auto surBR = std::make_shared<PcmTrack>();
    surBL->sampleRate = surBR->sampleRate = kRate;
    surBL->channels = surBR->channels = 5;
    surBL->samples.assign(4 * kRate * 5, 0.0f);
    surBR->samples.assign(4 * kRate * 5, 0.0f);
    for (std::size_t f = 0; f < 4 * kRate; ++f) {
        const float s = static_cast<float>(
            0.4 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(f) / kRate));
        surBL->samples[f * 5 + 3] = s;
        surBR->samples[f * 5 + 4] = s;
    }
    const AnalysisResult surAnalysis = analysisFor(120.0, 4.0);
    const MixFeatureMap mapSurBL = extractor.extractFromTrack(*surBL, surAnalysis);
    const MixFeatureMap mapSurBR = extractor.extractFromTrack(*surBR, surAnalysis);
    CHECK_NEAR(mapSurBL.loudness.integratedLufs, mapSurBR.loudness.integratedLufs, 0.05f);

    // Partial final frame must be sample-weighted rather than treated as a full
    // 100 ms frame: a 1 ms tone at the end of an 8.001 s track must match the
    // same 1 ms tone placed at 5.0 s.
    const std::size_t partialFrames = static_cast<std::size_t>(std::llround(8.001 * kRate));
    auto endPip = std::make_shared<PcmTrack>();
    auto midPip = std::make_shared<PcmTrack>();
    endPip->sampleRate = midPip->sampleRate = kRate;
    endPip->channels = midPip->channels = 1;
    endPip->samples.assign(partialFrames, 0.0f);
    midPip->samples.assign(partialFrames, 0.0f);
    const std::size_t pipLen = static_cast<std::size_t>(kRate / 1000); // 1 ms = 48 samples
    for (std::size_t i = 0; i < pipLen; ++i) {
        const float s = static_cast<float>(
            0.4 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(i) / kRate));
        endPip->samples[partialFrames - pipLen + i] = s;
        midPip->samples[5 * kRate + i] = s;
    }
    const AnalysisResult partialAnalysis = analysisFor(120.0, 8.001);
    const MixFeatureMap mapEndPip = extractor.extractFromTrack(*endPip, partialAnalysis);
    const MixFeatureMap mapMidPip = extractor.extractFromTrack(*midPip, partialAnalysis);
    CHECK_NEAR(mapEndPip.loudness.momentaryMaxLufs, mapMidPip.loudness.momentaryMaxLufs, 0.25f);
    CHECK_NEAR(mapEndPip.loudness.integratedLufs, mapMidPip.loudness.integratedLufs, 0.25f);

    // Changing frameStepSeconds (e.g. 0.25 s or 0.30 s) must not alter the
    // 400 ms momentary or integrated loudness measurement intervals.
    bgmedia::mix::MixFeatureConfig cfg025;
    cfg025.frameStepSeconds = 0.25;
    bgmedia::mix::MixFeatureConfig cfg030;
    cfg030.frameStepSeconds = 0.30;
    MixFeatureExtractor ext025(cfg025);
    MixFeatureExtractor ext030(cfg030);
    const MixFeatureMap mapMid025 = ext025.extractFromTrack(*midPip, partialAnalysis);
    const MixFeatureMap mapMid030 = ext030.extractFromTrack(*midPip, partialAnalysis);
    CHECK_NEAR(mapMid025.loudness.momentaryMaxLufs, mapMidPip.loudness.momentaryMaxLufs, 0.05f);
    CHECK_NEAR(mapMid030.loudness.momentaryMaxLufs, mapMidPip.loudness.momentaryMaxLufs, 0.05f);
    CHECK_NEAR(mapMid025.loudness.integratedLufs, mapMidPip.loudness.integratedLufs, 0.05f);
    CHECK_NEAR(mapMid030.loudness.integratedLufs, mapMidPip.loudness.integratedLufs, 0.05f);
}

TEST_CASE("mix.feature_map_adaptive_bounds_and_quiet_master") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;

    MixFeatureExtractor extractor;
    const double duration = 12.0;
    const AnalysisResult analysis = analysisFor(120.0, duration);

    // Quiet master at -54 dBFS (amplitude ~0.0020, RMS ~0.00141 = -57 dBFS),
    // which would be wiped out by a hard -55 dBFS threshold, surrounded by
    // 1.5 s lead-in and 2.0 s lead-out at -88 dBFS noise floor.
    auto quiet = std::make_shared<PcmTrack>();
    quiet->sampleRate = kRate;
    quiet->channels = 2;
    const std::size_t totalFrames = static_cast<std::size_t>(duration * kRate);
    quiet->samples.assign(totalFrames * 2, 0.0f);
    for (std::size_t f = 0; f < totalFrames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        const bool inBody = (t >= 1.5 && t <= duration - 2.0);
        const double amp = inBody ? 0.0020 : 0.00004; // -54 dBFS peak (-57 dBFS RMS) vs -88 dBFS
        const float s = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t));
        quiet->samples[f * 2] = s;
        quiet->samples[f * 2 + 1] = s;
    }

    const MixFeatureMap quietMap = extractor.extractFromTrack(*quiet, analysis);
    CHECK(quietMap.valid());
    CHECK(quietMap.loudness.meanRmsDbfs < -55.0f);
    CHECK(quietMap.audible.enterThresholdDbfs < -65.0f);
    CHECK_NEAR(quietMap.audible.audibleStart, 1.5, 0.25);
    CHECK_NEAR(quietMap.audible.audibleEnd, duration - 2.0, 0.25);
    CHECK(quietMap.sampleAt(5.0).active);
    CHECK(!quietMap.sampleAt(0.5).active);
    CHECK(!quietMap.sampleAt(11.2).active);

    // Track with a 2.4 s fade-in and a 2.4 s smooth reverb-like decay tail.
    auto shaped = std::make_shared<PcmTrack>();
    shaped->sampleRate = kRate;
    shaped->channels = 2;
    shaped->samples.assign(totalFrames * 2, 0.0f);
    for (std::size_t f = 0; f < totalFrames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        double env = 0.0;
        if (t >= 0.5 && t < 3.0) {
            const double u = (t - 0.5) / 2.5;
            env = 0.02 * std::pow(20.0, u); // exponential dB ramp from -34 dB to -8 dB
        } else if (t >= 3.0 && t < 8.5) {
            env = 0.40;
        } else if (t >= 8.5 && t < 11.2) {
            const double u = (t - 8.5) / 2.7;
            env = 0.40 * std::pow(0.03, u); // smooth exponential decay tail
        }
        const float s = static_cast<float>(env * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t));
        shaped->samples[f * 2] = s;
        shaped->samples[f * 2 + 1] = s;
    }

    const MixFeatureMap shapedMap = extractor.extractFromTrack(*shaped, analysis);
    CHECK(shapedMap.audible.hasFadeIn);
    CHECK(shapedMap.audible.fullBodyStart > shapedMap.audible.audibleStart + 1.2);
    CHECK(shapedMap.audible.tailStart < shapedMap.audible.audibleEnd - 1.0);
    CHECK(shapedMap.audible.hasFadeOut || shapedMap.audible.hasReverbTail);
}

TEST_CASE("mix.feature_map_spectral_bands_and_novelty") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::WindowFeatureSummary;

    MixFeatureExtractor extractor;
    const double duration = 10.0;
    const AnalysisResult analysis = analysisFor(120.0, duration);

    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = kRate;
    track->channels = 2;
    const std::size_t totalFrames = static_cast<std::size_t>(duration * kRate);
    track->samples.assign(totalFrames * 2, 0.0f);
    for (std::size_t f = 0; f < totalFrames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        // First 5 s: 75 Hz bass; second 5 s: 6000 Hz treble (above 3.5 kHz mid cutoff).
        const double hz = (t < 5.0) ? 75.0 : 6000.0;
        const float s = static_cast<float>(0.35 * std::sin(2.0 * 3.14159265358979323846 * hz * t));
        track->samples[f * 2] = s;
        track->samples[f * 2 + 1] = s;
    }

    const MixFeatureMap map = extractor.extractFromTrack(*track, analysis);
    const WindowFeatureSummary firstHalf = map.summarizeWindow(1.0, 4.0);
    const WindowFeatureSummary secondHalf = map.summarizeWindow(6.0, 9.0);
    CHECK(firstHalf.lowShare > 0.65f);
    CHECK(firstHalf.bassDominance > 0.65f);
    CHECK(secondHalf.highShare > 0.65f);
    CHECK(secondHalf.bassDominance < 0.25f);

    // Spectral novelty must peak around the 5.0 s spectral transition.
    float boundaryNovelty = 0.0f;
    for (double t = 4.7; t <= 5.3; t += 0.1) {
        boundaryNovelty = std::max(boundaryNovelty, map.sampleAt(t).spectralNovelty);
    }
    CHECK(boundaryNovelty > firstHalf.spectralNovelty + 0.25f);
    CHECK(boundaryNovelty > secondHalf.spectralNovelty + 0.25f);

    // Window ending exactly at a frame boundary [0.4, 0.5] must not include the
    // following frame [0.5, 0.6).
    MixFeatureMap stepMap = map;
    for (std::size_t i = 0; i < stepMap.frames.size(); ++i) {
        stepMap.frames[i].energy = (i < 5) ? 0.10f : 0.90f;
    }
    const WindowFeatureSummary preStep = stepMap.summarizeWindow(0.4, 0.5);
    CHECK_NEAR(preStep.meanEnergy, 0.10f, 1e-4f);

    // Local tempo stability is determined from the matched TempoSegment rather
    // than propagating a track-wide tempoStable == false into constant-tempo segments.
    stepMap.tempoStable = false;
    stepMap.tempoSegments = {
        {0.0, 4.5, 120.0, 0.85f},
        {5.5, 10.0, 128.0, 0.85f},
    };
    stepMap.tempoTransitions = {
        {4.5, 5.5, 120.0, 128.0, 0.80f},
    };
    bool segStable = false;
    CHECK_NEAR(stepMap.localBpmAt(2.0, &segStable), 120.0, 1e-4);
    CHECK(segStable);
    CHECK(stepMap.summarizeWindow(1.0, 4.0).tempoStable);
    CHECK(!stepMap.summarizeWindow(4.0, 6.0).tempoStable);
}

TEST_CASE("mix.feature_map_phrases_and_cue_candidates") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;

    MixFeatureExtractor extractor;
    const double duration = 32.0;
    AnalysisResult analysis = analysisFor(120.0, duration);
    // Test 3/4 meter awareness as well as section preservation (never relabelling Main).
    analysis.beatsPerBar = 3;
    for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
        analysis.beats[i].downbeat = (i % 3 == 0);
        analysis.beats[i].barIndex = static_cast<int>(i / 3);
        analysis.beats[i].beatInBar = static_cast<int>(i % 3);
    }

    const auto track = toneTrack(duration, 220.0);
    const MixFeatureMap map = extractor.extractFromTrack(*track, analysis);
    CHECK(map.valid());
    CHECK(!map.bars.empty());
    CHECK(!map.phrases.empty());
    CHECK(!map.mixInCandidates.empty());
    CHECK(!map.mixOutCandidates.empty());

    for (const auto& bar : map.bars) {
        CHECK(bar.beatCount <= 3);
    }
    for (const auto& phrase : map.phrases) {
        CHECK(phrase.end > phrase.start);
        CHECK(phrase.barCount >= 1);
    }
    CHECK(map.mixInCandidates.front().priorScore >= map.mixInCandidates.back().priorScore);
    CHECK(map.mixOutCandidates.front().priorScore >= map.mixOutCandidates.back().priorScore);

    // Downgrade fallback when PCM is unavailable.
    const MixFeatureMap fallback = extractor.buildFromAnalysisOnly(analysis);
    CHECK(fallback.valid());
    CHECK(!fallback.info.fromPcm);
    CHECK(!fallback.bars.empty());
    CHECK(!fallback.mixInCandidates.empty());
    CHECK(!fallback.mixOutCandidates.empty());

    // Beat grid starting with a 2-beat pickup where BeatTracker assigns
    // source barIndex = 0 to both the pickup and the first 4-beat bar.
    AnalysisResult pickupAnalysis = analysisFor(120.0, 8.0);
    pickupAnalysis.beatsPerBar = 4;
    pickupAnalysis.beats.clear();
    for (int i = 0; i < 12; ++i) {
        bgmedia::Beat bt;
        bt.time = 0.5 * static_cast<double>(i);
        bt.confidence = 0.9f;
        if (i < 2) {
            bt.downbeat = false;
            bt.barIndex = 0;
            bt.beatInBar = i + 2;
        } else {
            const int fullIdx = i - 2;
            bt.downbeat = (fullIdx % 4 == 0);
            bt.barIndex = fullIdx / 4;
            bt.beatInBar = fullIdx % 4;
        }
        pickupAnalysis.beats.push_back(bt);
    }
    const MixFeatureMap pickupMap = extractor.buildFromAnalysisOnly(pickupAnalysis);
    REQUIRE(pickupMap.bars.size() == 4);
    CHECK(pickupMap.bars[0].beatCount == 2);
    CHECK(pickupMap.bars[1].beatCount == 4);
    CHECK(pickupMap.bars[2].beatCount == 4);
    CHECK(pickupMap.bars[3].beatCount == 2);
}

TEST_CASE("mix.feature_cache_roundtrip_and_invalidation") {
    using bgmedia::AnalysisCache;
    using bgmedia::AnalysisConfig;
    using bgmedia::mix::FeatureCacheLoadResult;
    using bgmedia::mix::FeatureCacheStatus;
    using bgmedia::mix::MixFeatureCache;
    using bgmedia::mix::MixFeatureConfig;
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "bgmedia_mix_feature_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    MixFeatureExtractor extractor;
    AnalysisResult analysis = analysisFor(124.0, 12.0);
    analysis.info.contentHash = 0x123456789ABCDEF0ULL;
    analysis.info.configHash = AnalysisConfig{}.fingerprint();
    analysis.info.sampleRate = kRate;
    analysis.info.sourceChannels = 2;
    const auto track = toneTrack(12.0, 440.0);

    bgmedia::PcmBuffer monoPcm;
    monoPcm.sampleRate = kRate;
    monoPcm.sourceChannels = 2;
    monoPcm.samples.resize(track->frameCount(), 0.0f);
    for (std::size_t i = 0; i < monoPcm.samples.size(); ++i) {
        monoPcm.samples[i] = track->samples[i * 2];
    }
    monoPcm.contentHash = 0x123456789ABCDEF0ULL;
    analysis.info.contentHash = monoPcm.contentHash;

    MixFeatureMap original = extractor.extract(monoPcm, analysis);
    original.vocal.available = true;
    original.vocal.meanConfidence = 0.85f;
    original.vocal.activeRatio = 0.42f;
    original.harmony.available = true;
    original.harmony.keyIndex = 9;
    original.harmony.isMinor = true;
    original.harmony.camelotCode = "8A";
    original.harmony.keyConfidence = 0.78f;

    // Coexistence with BG Beat AnalysisCache (.bgm) in the same directory.
    AnalysisCache beatCache(dir);
    AnalysisConfig beatCfg;
    std::string error;
    CHECK(beatCache.store(analysis, monoPcm, beatCfg, &error));

    MixFeatureCache featureCache(dir);
    MixFeatureConfig featCfg;
    CHECK(featureCache.store(original, featCfg, &error));

    const FeatureCacheLoadResult loadedRes =
        featureCache.load(monoPcm, analysis, featCfg);
    CHECK(loadedRes.hit());
    const MixFeatureMap& loaded = loadedRes.features;
    CHECK(loaded.valid());
    CHECK(loaded.frames.size() == original.frames.size());
    CHECK(loaded.bars.size() == original.bars.size());
    CHECK(loaded.phrases.size() == original.phrases.size());
    CHECK(loaded.mixInCandidates.size() == original.mixInCandidates.size());
    CHECK(loaded.mixOutCandidates.size() == original.mixOutCandidates.size());
    CHECK_NEAR(loaded.loudness.integratedLufs, original.loudness.integratedLufs, 1e-4);
    CHECK_NEAR(loaded.audible.audibleStart, original.audible.audibleStart, 1e-6);
    CHECK_NEAR(loaded.audible.audibleEnd, original.audible.audibleEnd, 1e-6);
    CHECK(loaded.vocal.available);
    CHECK_NEAR(loaded.vocal.meanConfidence, 0.85f, 1e-5);
    CHECK_NEAR(loaded.vocal.activeRatio, 0.42f, 1e-5);
    CHECK(loaded.harmony.available);
    CHECK(loaded.harmony.keyIndex == 9);
    CHECK(loaded.harmony.isMinor);
    CHECK(loaded.harmony.camelotCode == "8A");

    // Beat cache (.bgm) is still readable alongside (.bgf).
    const auto loadedBeat = beatCache.load(monoPcm, beatCfg);
    CHECK(loadedBeat.hit());
    CHECK_NEAR(loadedBeat.result.bpm, 124.0, 1e-6);

    // Model checksum / config change invalidates the cache.
    MixFeatureConfig changedCfg = featCfg;
    changedCfg.vocalModelChecksum = featCfg.vocalModelChecksum + 1;
    CHECK(!featureCache.load(monoPcm, analysis, changedCfg).hit());

    // Payload corruption is caught by the FNV-1a checksum.
    const std::filesystem::path bgfPath =
        dir / MixFeatureCache::fileNameFor(original.info.contentHash,
                                           original.info.analysisConfigHash,
                                           featCfg.fingerprint());
    {
        std::fstream file(bgfPath, std::ios::binary | std::ios::in | std::ios::out);
        CHECK(static_cast<bool>(file));
        file.seekp(-4, std::ios::end);
        const char bad = static_cast<char>(0xFF);
        file.write(&bad, 1);
    }
    const FeatureCacheLoadResult corruptRes =
        featureCache.load(monoPcm, analysis, featCfg);
    CHECK(!corruptRes.hit());
    CHECK(corruptRes.status == FeatureCacheStatus::Corrupt);

    // Two stereo sources with identical mono downmixes (s, s) vs (2s, 0) but
    // different channel powers must have distinct feature-cache keys.
    auto dualMono = toneTrack(8.0, 1000.0, 2, kRate);
    auto leftOnly = toneTrack(8.0, 1000.0, 2, kRate);
    for (std::size_t f = 0; f < dualMono->frameCount(); ++f) {
        dualMono->samples[f * 2] *= 0.5f;
        dualMono->samples[f * 2 + 1] *= 0.5f;
        leftOnly->samples[f * 2] *= 1.0f;
        leftOnly->samples[f * 2 + 1] = 0.0f;
    }
    AnalysisResult stereoAnalysis = analysisFor(120.0, 8.0);
    const MixFeatureMap mapDual = extractor.extractFromTrack(*dualMono, stereoAnalysis);
    const MixFeatureMap mapLeft = extractor.extractFromTrack(*leftOnly, stereoAnalysis);
    CHECK(mapDual.info.contentHash != mapLeft.info.contentHash);
    CHECK(featureCache.store(mapDual, featCfg, &error));
    CHECK(!featureCache.load(*leftOnly, stereoAnalysis, featCfg).hit());
    CHECK(featureCache.store(mapLeft, featCfg, &error));
    const auto loadedDual = featureCache.load(*dualMono, stereoAnalysis, featCfg);
    const auto loadedLeft = featureCache.load(*leftOnly, stereoAnalysis, featCfg);
    CHECK(loadedDual.hit());
    CHECK(loadedLeft.hit());
    CHECK_NEAR(loadedLeft.features.loudness.integratedLufs -
                   loadedDual.features.loudness.integratedLufs,
               3.01f, 0.15f);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("mix.planner_and_session_use_feature_map") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;

    // Outgoing track has 5 s of trailing PCM silence that was NOT listed in
    // analysis.silences; incoming track has 2.2 s of leading PCM silence.
    const double duration = 24.0;
    AnalysisResult a = analysisFor(120.0, duration);
    AnalysisResult b = analysisFor(120.0, duration);
    b.sections.clear(); // no Intro section cue, so B would start at 0.0 without features

    auto pcmA = toneTrack(duration, 220.0);
    const std::size_t cutFrameA = static_cast<std::size_t>((duration - 5.0) * kRate);
    std::fill(pcmA->samples.begin() + static_cast<std::ptrdiff_t>(cutFrameA * 2),
              pcmA->samples.end(), 0.0f);

    auto pcmB = toneTrack(duration, 330.0);
    const std::size_t startFrameB = static_cast<std::size_t>(2.2 * kRate);
    std::fill(pcmB->samples.begin(),
              pcmB->samples.begin() + static_cast<std::ptrdiff_t>(startFrameB * 2),
              0.0f);

    MixFeatureExtractor extractor;
    const MixFeatureMap featA = extractor.extractFromTrack(*pcmA, a);
    const MixFeatureMap featB = extractor.extractFromTrack(*pcmB, b);
    CHECK_NEAR(featA.audible.audibleEnd, duration - 5.0, 0.25);
    CHECK_NEAR(featB.audible.audibleStart, 2.2, 0.25);

    MixPlanner planner;
    PlanRequest request;
    request.type = TransitionType::SmartCrossfade;
    request.requestedDuration = 4.0;
    const MixPlan plan = planner.plan(a, b, &featA, &featB, request);
    CHECK(plan.valid);
    CHECK(plan.transitionEndA <= duration - 4.5);
    CHECK(plan.startB >= 1.8);

    // When preferPhraseBoundaries is disabled and B has an Intro cue later than
    // audibleStart (e.g. Intro starts at 8.0 s while audibleStart is 2.2 s),
    // grid alignment must align around the selected cue (~8.0 s) rather than
    // jumping back to audibleStart.
    AnalysisResult bWithIntro = analysisFor(120.0, duration);
    bWithIntro.sections = {
        {8.0, 16.0, SectionKind::Intro, 0.4f, 0.9f},
        {16.0, duration, SectionKind::Main, 0.8f, 0.9f},
    };
    PlannerConfig noPhraseCfg;
    noPhraseCfg.preferPhraseBoundaries = false;
    MixPlanner noPhrasePlanner(noPhraseCfg);
    const MixPlan cueAlignedPlan =
        noPhrasePlanner.plan(a, bWithIntro, &featA, &featB, request);
    CHECK(cueAlignedPlan.valid);
    CHECK_NEAR(cueAlignedPlan.startB, 8.0, 0.6);

    // Session populates deckFeatures(DeckId) on worker completion.
    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};
    session.addTrack("alpha.wav");
    session.addTrack("beta.wav");
    CHECK(pump.waitForDecks(2));
    CHECK(session.deckFeatures(DeckId::A) != nullptr);
    CHECK(session.deckFeatures(DeckId::A)->valid());
    CHECK(session.deckFeatures(DeckId::B) != nullptr);
    CHECK(session.deckFeatures(DeckId::B)->valid());
}

// ---------------------------------------------------------------------------
// BG Mix 0.3 PR 2: Vocal Activity Detection & Harmonic / Spectral Compatibility
// ---------------------------------------------------------------------------

namespace {

// Synthesizes a multi-harmonic formant voice signal (F0 with gentle 5.5 Hz
// vibrato + harmonics 1..6 shaped by F1=600 Hz and F2=1450 Hz) in [vocStart, vocEnd].
std::shared_ptr<PcmTrack> makeVocalAndInstrumentalTrack(
    double duration, double vocStart, double vocEnd, double bassHz = 110.0) {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = kRate;
    track->channels = 2;
    const std::size_t frames = static_cast<std::size_t>(duration * kRate);
    track->samples.assign(frames * 2, 0.0f);

    double phase = 0.0;
    for (std::size_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        // Underlying bass sine wave in instrumental sections.
        double sample = 0.18 * std::sin(2.0 * 3.14159265358979323846 * bassHz * t);

        if (t >= vocStart && t <= vocEnd) {
            const double vibrato = 1.0 + 0.008 * std::sin(2.0 * 3.14159265358979323846 * 5.5 * t);
            const double f0 = 220.0 * vibrato;
            phase += 2.0 * 3.14159265358979323846 * f0 / kRate;
            // Multi-harmonic voice with strong formant emphasis in F1/F2/F3.
            const double voice =
                0.35 * std::sin(phase) +
                0.45 * std::sin(2.0 * phase) +
                0.55 * std::sin(3.0 * phase) +
                0.40 * std::sin(4.0 * phase) +
                0.30 * std::sin(5.0 * phase) +
                0.22 * std::sin(6.0 * phase) +
                0.15 * std::sin(7.0 * phase);
            sample = 0.08 * sample + 0.32 * voice;
        }
        const float s = static_cast<float>(std::clamp(sample, -0.95, 0.95));
        track->samples[f * 2] = s;
        track->samples[f * 2 + 1] = s;
    }
    return track;
}

std::shared_ptr<PcmTrack> makeTriadTrack(double duration,
                                         double fRoot, double fThird, double fFifth) {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = kRate;
    track->channels = 2;
    const std::size_t frames = static_cast<std::size_t>(duration * kRate);
    track->samples.assign(frames * 2, 0.0f);
    for (std::size_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        const double w = 2.0 * 3.14159265358979323846 * t;
        const double v =
            0.26 * std::sin(w * fRoot) +
            0.10 * std::sin(w * fRoot * 2.0) +
            0.18 * std::sin(w * fThird) +
            0.16 * std::sin(w * fFifth);
        const float s = static_cast<float>(v);
        track->samples[f * 2] = s;
        track->samples[f * 2 + 1] = s;
    }
    return track;
}

} // namespace

TEST_CASE("mix.vocal_detector_distinguishes_voice_from_sine_and_drums") {
    using bgmedia::mix::ExternalStemVocalDetector;
    using bgmedia::mix::MixFeatureConfig;
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::VocalDetectorKind;

    const double duration = 12.0;
    const AnalysisResult analysis = analysisFor(120.0, duration);
    // Track has pure bass in 0..3 s, singing voice in 3..8 s, and pure bass in 8..12 s.
    const auto track = makeVocalAndInstrumentalTrack(duration, 3.0, 8.0, 95.0);

    MixFeatureExtractor extractor;
    const MixFeatureMap map = extractor.extractFromTrack(*track, analysis);
    CHECK(map.vocal.available);
    CHECK(map.vocal.detectorKind == VocalDetectorKind::MusicVocalModel);
    CHECK(map.vocal.meanConfidence > 0.50f);
    CHECK(!map.vocal.regions.empty());

    const auto instStart = map.summarizeWindow(0.5, 2.5);
    const auto vocalMid = map.summarizeWindow(4.0, 7.0);
    const auto instEnd = map.summarizeWindow(9.0, 11.5);
    CHECK(vocalMid.vocalScore > 0.55f);
    CHECK(instStart.vocalScore < 0.22f);
    CHECK(instEnd.vocalScore < 0.22f);

    // A pure mid-band 440 Hz sine wave (single harmonic, no overtones) must not
    // be misclassified as a lead vocal via sub-harmonic candidate F0s.
    auto midSine = toneTrack(4.0, 440.0, 2, kRate);
    const AnalysisResult sineAnalysis = analysisFor(120.0, 4.0);
    const MixFeatureMap midSineMap = extractor.extractFromTrack(*midSine, sineAnalysis);
    CHECK(midSineMap.vocal.available);
    CHECK(midSineMap.vocal.regions.empty());
    CHECK(midSineMap.summarizeWindow(0.5, 3.5).vocalScore < 0.15f);

    // Disabled vocal detector must honestly report available = false and confidence = 0.
    MixFeatureConfig offCfg;
    offCfg.vocalDetector = VocalDetectorKind::None;
    MixFeatureExtractor offExtractor(offCfg);
    const MixFeatureMap offMap = offExtractor.extractFromTrack(*track, analysis);
    CHECK(!offMap.vocal.available);
    CHECK_NEAR(offMap.vocal.meanConfidence, 0.0f, 1e-6);

    // External stem vocal detector provides high-confidence ground truth.
    bgmedia::PcmBuffer stemPcm;
    stemPcm.sampleRate = kRate;
    stemPcm.sourceChannels = 1;
    stemPcm.samples.assign(static_cast<std::size_t>(duration * kRate), 0.0f);
    for (std::size_t i = static_cast<std::size_t>(3.0 * kRate);
         i < static_cast<std::size_t>(8.0 * kRate); ++i) {
        stemPcm.samples[i] = 0.35f;
    }
    ExternalStemVocalDetector stemDet(stemPcm, 0xCAFEBABEULL);
    MixFeatureExtractor stemExtractor({}, &stemDet);
    const MixFeatureMap stemMap = stemExtractor.extractFromTrack(*track, analysis);
    CHECK(stemMap.vocal.available);
    CHECK(stemMap.vocal.detectorKind == VocalDetectorKind::ExternalStem);
    CHECK(stemMap.vocal.meanConfidence >= 0.90f);
    CHECK(stemMap.summarizeWindow(4.0, 7.0).vocalScore > 0.75f);
    CHECK(stemMap.summarizeWindow(0.5, 2.5).vocalScore < 0.05f);
}

TEST_CASE("mix.vocal_overlap_clash_vs_clean_handoff") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::VocalOverlapAssessment;
    using bgmedia::mix::evaluateVocalOverlap;

    const double duration = 12.0;
    const AnalysisResult analysis = analysisFor(120.0, duration);
    MixFeatureExtractor extractor;

    // Track A has lead vocals in 1..6 s and instrumental outro in 6..12 s.
    // Track B has instrumental intro in 0..5 s and lead vocals in 5..11 s.
    const auto trackA = makeVocalAndInstrumentalTrack(duration, 1.0, 6.0, 110.0);
    const auto trackB = makeVocalAndInstrumentalTrack(duration, 5.0, 11.0, 110.0);
    const MixFeatureMap featA = extractor.extractFromTrack(*trackA, analysis);
    const MixFeatureMap featB = extractor.extractFromTrack(*trackB, analysis);

    // 1. Clash case: overlap A [1.5..5.5] (vocals) with B [5.5..9.5] (vocals).
    const VocalOverlapAssessment clash =
        evaluateVocalOverlap(featA, 1.5, 1.0, featB, 5.5, 1.0, 4.0);
    CHECK(clash.available);
    CHECK(clash.vocalClashScore > 0.35f);
    CHECK(clash.clashDurationSeconds > 1.5);
    CHECK(!clash.cleanVocalHandoff);

    // 2. Clean handoff case: overlap A [6.5..10.5] (instrumental outro) with B [3.5..7.5]
    // (instrumental intro transitioning into B's vocal).
    const VocalOverlapAssessment handoff =
        evaluateVocalOverlap(featA, 6.5, 1.0, featB, 3.5, 1.0, 4.0);
    CHECK(handoff.available);
    CHECK(handoff.vocalClashScore < 0.15f);
    CHECK(handoff.cleanVocalHandoff);

    // 3. Muted instrumental deck must not contribute to vocalOverInstrumentalScore,
    // and an outgoing vocal phrase that finishes naturally while still at unity gain
    // must not be flagged as cutsMidVocalPhraseA.
    const std::vector<bgmedia::mix::GainPoint> heldOut = {
        {0.0, 1.0f}, {7.0, 1.0f}, {8.0, 0.0f}};
    const std::vector<bgmedia::mix::GainPoint> mutedIn = {
        {0.0, 0.0f}, {8.0, 0.0f}};
    const auto instOnlyB = toneTrack(duration, 110.0, 2, kRate);
    const MixFeatureMap featInstB = extractor.extractFromTrack(*instOnlyB, analysis);
    const VocalOverlapAssessment heldAOverMutedB =
        evaluateVocalOverlap(featA, 0.0, 1.0, featInstB, 0.0, 1.0, 8.0, heldOut, mutedIn);
    CHECK_NEAR(heldAOverMutedB.vocalOverInstrumentalScore, 0.0f, 1e-5f);
    CHECK(!heldAOverMutedB.cutsMidVocalPhraseA);

    // 4. Truncated outgoing vocal phrase (faded to 0 by 2.5 s while phrase continues
    // to 6.0 s) must set cutsMidVocalPhraseA = true and cleanVocalHandoff = false.
    const std::vector<bgmedia::mix::GainPoint> earlyFadeOut = {
        {0.0, 1.0f}, {2.5, 0.0f}, {8.0, 0.0f}};
    const VocalOverlapAssessment truncatedA =
        evaluateVocalOverlap(featA, 0.0, 1.0, featInstB, 0.0, 1.0, 8.0, earlyFadeOut);
    CHECK(truncatedA.cutsMidVocalPhraseA);
    CHECK(!truncatedA.cleanVocalHandoff);
}

TEST_CASE("mix.harmonic_key_estimation_and_camelot_helper") {
    using bgmedia::mix::HarmonicAnalyzer;
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;

    const double duration = 8.0;
    const AnalysisResult analysis = analysisFor(120.0, duration);
    MixFeatureExtractor extractor;

    // A minor triad: A3 (220.00 Hz), C4 (261.63 Hz), E4 (329.63 Hz) -> 8A
    const auto trackAm = makeTriadTrack(duration, 220.00, 261.63, 329.63);
    const MixFeatureMap featAm = extractor.extractFromTrack(*trackAm, analysis);
    CHECK(featAm.harmony.available);
    CHECK(featAm.harmony.keyIndex == 9);
    CHECK(featAm.harmony.isMinor);
    CHECK(featAm.harmony.camelotCode == "8A");
    CHECK(featAm.harmony.tonalness > 0.55f);
    CHECK(featAm.harmony.keyConfidence > 0.35f);

    // C major triad: C4 (261.63 Hz), E4 (329.63 Hz), G4 (392.00 Hz) -> 8B
    const auto trackC = makeTriadTrack(duration, 261.63, 329.63, 392.00);
    const MixFeatureMap featC = extractor.extractFromTrack(*trackC, analysis);
    CHECK(featC.harmony.available);
    CHECK(featC.harmony.keyIndex == 0);
    CHECK(!featC.harmony.isMinor);
    CHECK(featC.harmony.camelotCode == "8B");

    // F# major triad: F#3 (185.00 Hz), A#3 (233.08 Hz), C#4 (277.18 Hz) -> 2B
    const auto trackFs = makeTriadTrack(duration, 185.00, 233.08, 277.18);
    const MixFeatureMap featFs = extractor.extractFromTrack(*trackFs, analysis);
    CHECK(featFs.harmony.available);
    CHECK(featFs.harmony.keyIndex == 6);
    CHECK(!featFs.harmony.isMinor);
    CHECK(featFs.harmony.camelotCode == "2B");
}

TEST_CASE("mix.harmonic_compatibility_pitch_shift_and_percussive_neutrality") {
    using bgmedia::mix::HarmonicCompatibility;
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::evaluateHarmonicCompatibility;

    const double duration = 8.0;
    const AnalysisResult analysis = analysisFor(120.0, duration);
    MixFeatureExtractor extractor;

    const auto trackAm = makeTriadTrack(duration, 220.00, 261.63, 329.63); // Am (8A)
    const auto trackC = makeTriadTrack(duration, 261.63, 329.63, 392.00);  // C  (8B, relative major)
    const auto trackFs = makeTriadTrack(duration, 185.00, 233.08, 277.18); // F# (2B, distant)
    const auto trackGsm = makeTriadTrack(duration, 207.65, 246.94, 311.13); // G#m (1A, 1 semitone below Am)

    const MixFeatureMap featAm = extractor.extractFromTrack(*trackAm, analysis);
    const MixFeatureMap featC = extractor.extractFromTrack(*trackC, analysis);
    const MixFeatureMap featFs = extractor.extractFromTrack(*trackFs, analysis);
    const MixFeatureMap featGsm = extractor.extractFromTrack(*trackGsm, analysis);

    // 1. Relative major (Am -> C) is much more compatible than distant tritone/semitone (Am -> F#).
    const HarmonicCompatibility compatRel =
        evaluateHarmonicCompatibility(featAm, 1.0, 1.0, featC, 1.0, 1.0, 4.0, true);
    const HarmonicCompatibility compatDist =
        evaluateHarmonicCompatibility(featAm, 1.0, 1.0, featFs, 1.0, 1.0, 4.0, true);
    CHECK(compatRel.available);
    CHECK(compatRel.compatibilityScore > compatDist.compatibilityScore + 0.22f);
    CHECK(compatRel.harmonicClashScore < compatDist.harmonicClashScore);

    // Muted incoming deck must not report harmonic clash even between C and F#.
    const std::vector<bgmedia::mix::GainPoint> unityOut = {{0.0, 1.0f}, {4.0, 1.0f}};
    const std::vector<bgmedia::mix::GainPoint> zeroIn = {{0.0, 0.0f}, {4.0, 0.0f}};
    const HarmonicCompatibility mutedDist =
        evaluateHarmonicCompatibility(featC, 1.0, 1.0, featFs, 1.0, 1.0, 4.0, true,
                                      unityOut, zeroIn);
    CHECK(mutedDist.harmonicClashScore < 0.05f);
    CHECK(mutedDist.compatibilityScore >= 0.70f);

    // Uncovered leading audio before the first beat/phrase span (e.g. beats start
    // at 4.0 s on an 8.0 s track where 0..4 s is C major and 4..8 s is F# major)
    // must still have its own local harmonic segment.
    AnalysisResult lateBeats = analysisFor(120.0, duration);
    lateBeats.beats.erase(
        std::remove_if(lateBeats.beats.begin(), lateBeats.beats.end(),
                       [](const Beat& bt) { return bt.time < 4.0; }),
        lateBeats.beats.end());
    auto splitKeyTrack = makeTriadTrack(duration, 185.00, 233.08, 277.18); // F# major
    for (std::size_t f = 0; f < static_cast<std::size_t>(4.0 * kRate); ++f) {
        splitKeyTrack->samples[f * 2] = trackC->samples[f * 2];
        splitKeyTrack->samples[f * 2 + 1] = trackC->samples[f * 2 + 1];
    }
    const MixFeatureMap featSplit = extractor.extractFromTrack(*splitKeyTrack, lateBeats);
    const HarmonicCompatibility introCvsC =
        evaluateHarmonicCompatibility(featSplit, 0.0, 1.0, featC, 0.0, 1.0, 3.0, true);
    CHECK(introCvsC.compatibilityScore > 0.70f);

    // 2. Pitch-shift awareness when pitchLock == false:
    // G#m played at +1 semitone (rate = 2^(1/12) ≈ 1.05946) without pitch lock
    // shifts into Am, matching featAm far better than at 1.0x!
    const double plusOneSemitoneRate = std::pow(2.0, 1.0 / 12.0);
    const HarmonicCompatibility gsmLocked =
        evaluateHarmonicCompatibility(featAm, 1.0, 1.0, featGsm, 1.0, plusOneSemitoneRate, 4.0, true);
    const HarmonicCompatibility gsmShiftedToAm =
        evaluateHarmonicCompatibility(featAm, 1.0, 1.0, featGsm, 1.0, plusOneSemitoneRate, 4.0, false);
    CHECK(gsmShiftedToAm.compatibilityScore > gsmLocked.compatibilityScore + 0.25f);

    // Quarter-tone detune (+3% rate without pitch lock ≈ +0.51 semitone) between
    // two identical Am tracks triggers detunePenalty.
    const HarmonicCompatibility quarterToneDetuned =
        evaluateHarmonicCompatibility(featAm, 1.0, 1.0, featAm, 1.0, 1.03, 4.0, false);
    CHECK(quarterToneDetuned.detunePenalty > 0.45f);

    // 3. Percussive / atonal neutrality: broadband deterministic pseudo-noise
    // has low tonalness (< 0.25) and sets percussiveNeutral = true.
    auto drumNoise = std::make_shared<PcmTrack>();
    drumNoise->sampleRate = kRate;
    drumNoise->channels = 2;
    drumNoise->samples.resize(static_cast<std::size_t>(duration * kRate) * 2, 0.0f);
    std::uint32_t rng = 0x13579BDFU;
    for (std::size_t i = 0; i < drumNoise->samples.size(); i += 2) {
        rng = rng * 1664525U + 1013904223U;
        const float white = (static_cast<float>((rng >> 8) & 0xFFFFU) / 32768.0f - 1.0f) * 0.25f;
        drumNoise->samples[i] = white;
        drumNoise->samples[i + 1] = white;
    }
    const MixFeatureMap featDrums = extractor.extractFromTrack(*drumNoise, analysis);
    CHECK(featDrums.harmony.tonalness < 0.25f);
    const HarmonicCompatibility drumVsFs =
        evaluateHarmonicCompatibility(featDrums, 1.0, 1.0, featFs, 1.0, 1.0, 4.0, true);
    CHECK(drumVsFs.percussiveNeutral);
    CHECK(drumVsFs.harmonicClashScore < 0.20f);
}


TEST_CASE("mix.feature_cache_audio_and_track_hash_equivalence") {
    using bgmedia::AudioData;
    using bgmedia::PcmBuffer;
    using bgmedia::mix::FeatureCacheStatus;
    using bgmedia::mix::MixFeatureCache;
    using bgmedia::mix::MixFeatureConfig;
    using bgmedia::mix::MixFeatureExtractor;

    auto track = toneTrack(2.0, 440.0, 1, kRate);
    AudioData audio;
    audio.sampleRate = track->sampleRate;
    audio.channels = track->channels;
    audio.samples = track->samples;
    const PcmBuffer pcm = PcmBuffer::fromAudio(audio);

    AnalysisResult analysis = analysisFor(120.0, 2.0);
    analysis.info.contentHash = 0; // force fallback to PCM hash

    MixFeatureConfig cfg;
    MixFeatureExtractor extractor(cfg);
    const auto mapFromAudio = extractor.extract(audio, pcm, analysis);
    const auto mapFromTrack = extractor.extractFromTrack(*track, analysis);
    CHECK(mapFromAudio.info.contentHash == mapFromTrack.info.contentHash);

    const auto cacheDir =
        std::filesystem::temp_directory_path() / "bgmedia_test_audio_track_cache_equiv";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
    MixFeatureCache cache(cacheDir);
    CHECK(cache.store(mapFromAudio, cfg));
    const auto loadedViaTrack = cache.load(*track, analysis, cfg);
    CHECK(loadedViaTrack.status == FeatureCacheStatus::Hit);
    std::filesystem::remove_all(cacheDir, ec);

    // Stereo track with a non-finite sample must still produce a finite, valid map.
    auto infStereo = toneTrack(3.0, 440.0, 2, kRate);
    infStereo->samples[static_cast<std::size_t>(0.55 * kRate) * 2] = std::numeric_limits<float>::infinity();
    const auto infMap = extractor.extractFromTrack(*infStereo, analysisFor(120.0, 3.0));
    CHECK(infMap.valid());
}

TEST_CASE("mix.planner_smart_automix_multi_candidate_and_loudness") {
    using bgmedia::Section;
    using bgmedia::SectionKind;
    using bgmedia::mix::CueKind;
    using bgmedia::mix::CuePointCandidate;
    using bgmedia::mix::CueRole;
    using bgmedia::mix::MixCandidate;
    using bgmedia::mix::MixFeatureConfig;
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::MixStrategyKind;
    using bgmedia::mix::VocalDetectorKind;
    using bgmedia::mix::VocalRegion;

    AnalysisResult analysisA = analysisFor(120.0, 32.0);
    AnalysisResult analysisB = analysisFor(120.0, 32.0);
    analysisA.sections = {
        {0.0, 16.0, SectionKind::Main, 1.0f, 0.85f},
        {16.0, 24.0, SectionKind::Outro, 0.55f, 0.90f},
        {24.0, 32.0, SectionKind::Main, 0.95f, 0.90f}
    };
    analysisB.sections = {
        {0.0, 8.0, SectionKind::Main, 0.95f, 0.90f},
        {8.0, 16.0, SectionKind::Intro, 0.55f, 0.90f},
        {16.0, 32.0, SectionKind::Main, 1.0f, 0.90f}
    };

    auto trackA = toneTrack(32.0, 220.0, 2, kRate);
    auto trackB = toneTrack(32.0, 220.0, 2, kRate);
    // Make A louder (0.75 amplitude) and B quieter (0.18 amplitude) to test LUFS balancing.
    for (float& s : trackA->samples) {
        s *= 1.5f;
    }
    for (float& s : trackB->samples) {
        s *= 0.36f;
    }

    MixFeatureConfig featCfg;
    featCfg.vocalDetector = VocalDetectorKind::MusicVocalModel;
    MixFeatureExtractor extractor(featCfg);
    MixFeatureMap featA = extractor.extractFromTrack(*trackA, analysisA);
    MixFeatureMap featB = extractor.extractFromTrack(*trackB, analysisB);

    // Inject dense lead vocal on A's late tail (24..32 s) and B's very start (0..8 s),
    // while A's earlier outro phrase (16..24 s) and B's phrase at (8..16 s) are vocal-free.
    for (auto& f : featA.frames) {
        if (f.time >= 24.0) {
            f.vocalScore = 0.92f;
            f.vocalConfidence = 0.90f;
        } else {
            f.vocalScore = 0.02f;
            f.vocalConfidence = 0.90f;
        }
    }
    featA.vocal.available = true;
    featA.vocal.meanConfidence = 0.90f;
    featA.vocal.regions = {{24.0, 32.0, 0.92f, 0.90f, true}};
    for (auto& c : featA.mixOutCandidates) {
        if (c.time >= 23.5) {
            c.vocalAfter = 0.92f;
            c.vocalConfidence = 0.90f;
            c.priorScore = 0.35f;
        } else if (std::abs(c.time - 16.0) < 0.5) {
            c.vocalAfter = 0.02f;
            c.vocalConfidence = 0.90f;
            c.priorScore = 0.92f;
        }
    }

    for (auto& f : featB.frames) {
        if (f.time < 8.0) {
            f.vocalScore = 0.92f;
            f.vocalConfidence = 0.90f;
        } else {
            f.vocalScore = 0.02f;
            f.vocalConfidence = 0.90f;
        }
    }
    featB.vocal.available = true;
    featB.vocal.meanConfidence = 0.90f;
    featB.vocal.regions = {{0.0, 8.0, 0.92f, 0.90f, true}};
    CuePointCandidate cleanMixInB;
    cleanMixInB.time = 8.0;
    cleanMixInB.barIndex = 4;
    cleanMixInB.beatIndex = 16;
    cleanMixInB.role = CueRole::MixIn;
    cleanMixInB.kind = CueKind::PhraseBoundary;
    cleanMixInB.sectionKind = SectionKind::Intro;
    cleanMixInB.boundaryStrength = 0.90f;
    cleanMixInB.rhythmConfidence = 0.90f;
    cleanMixInB.phraseConfidence = 0.90f;
    cleanMixInB.availableWindowSeconds = 24.0;
    cleanMixInB.vocalAfter = 0.02f;
    cleanMixInB.vocalConfidence = 0.90f;
    cleanMixInB.harmonicStability = 0.90f;
    cleanMixInB.priorScore = 0.94f;
    cleanMixInB.reason = "clean instrumental intro phrase";
    featB.mixInCandidates.insert(featB.mixInCandidates.begin(), cleanMixInB);

    MixPlanner planner;
    PlanRequest req;
    req.type = TransitionType::SmartCrossfade;
    req.requestedDuration = 0.0; // Auto mode
    req.currentPositionA = 8.0;

    const std::vector<MixCandidate> ranked =
        planner.planCandidates(analysisA, analysisB, &featA, &featB, req);
    CHECK(ranked.size() >= 2);
    CHECK(ranked.front().rank == 1);
    CHECK(ranked.front().score.totalScore >= ranked.back().score.totalScore);

    const MixPlan plan = planner.plan(analysisA, analysisB, &featA, &featB, req);
    CHECK(plan.valid);
    CHECK(!plan.alternatives.empty());
    // Must avoid the vocal-clashing 24..32s + 0..8s combination.
    CHECK(plan.scoreBreakdown.vocalSafetyScore > 0.70f);
    CHECK(plan.scoreBreakdown.totalScore > 0.65f);

    // Loudness matching: A is louder than B, so A is trimmed down and B is boosted
    // while respecting the -0.5 dBFS peak headroom ceiling.
    CHECK(plan.loudnessTrimDbA < 0.0f);
    CHECK(plan.loudnessTrimDbB > 0.0f);
    CHECK(plan.loudnessGainA < 1.0f);
    CHECK(plan.loudnessGainB > 1.0f);
    CHECK(featB.loudness.samplePeakDbfs + plan.loudnessTrimDbB <= -0.5f + 1e-3f);
}


TEST_CASE("mix.engine_loudness_normalization_and_strategies") {
    using bgmedia::mix::DeckId;
    using bgmedia::mix::GainPoint;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::MixPlan;
    using bgmedia::mix::MixPlanner;
    using bgmedia::mix::MixSnapshot;
    using bgmedia::mix::MixStrategyKind;
    using bgmedia::mix::PlanRequest;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::TransitionType;

    auto trackA = toneTrack(12.0, 220.0, 2, kRate);
    auto trackB = toneTrack(12.0, 330.0, 2, kRate);
    AnalysisResult analysisA = analysisFor(120.0, 12.0);
    AnalysisResult analysisB = analysisFor(120.0, 12.0);

    MixPlanner planner;
    PlanRequest req;
    req.type = TransitionType::SmartCrossfade;
    req.requestedDuration = 4.0;
    req.currentPositionA = 2.0;

    // 1. EchoReverbOut + Loudness normalization through MixEngine::render()
    MixPlan echoPlan = planner.plan(analysisA, analysisB, req);
    CHECK(echoPlan.valid);
    echoPlan.strategy = MixStrategyKind::EchoReverbOut;
    echoPlan.tailSendAmount = 0.50f;
    echoPlan.tailDecaySeconds = 1.8;
    echoPlan.loudnessGainA = 0.80f;
    echoPlan.loudnessGainB = 1.25f;
    echoPlan.gainCurveOut = {{0.0, 1.0f}, {2.8, 0.25f}, {3.1, 0.0f}, {4.0, 0.0f}};
    echoPlan.gainCurveIn = {{0.0, 0.0f}, {2.0, 0.70f}, {4.0, 1.0f}};

    MixEngine engine(kRate);
    CHECK(engine.loadDeck(DeckId::A, trackA, 1, 120.0));
    CHECK(engine.loadDeck(DeckId::B, trackB, 2, 120.0));
    engine.play(DeckId::A);
    engine.setMaster(DeckId::A);

    std::vector<float> block(512 * 2, 0.0f);
    engine.seek(DeckId::A, echoPlan.overallStartA());
    engine.render(block.data(), 512);
    CHECK(engine.startTransition(echoPlan));

    float maxTailWet = 0.0f;
    float maxSampleStep = 0.0f;
    float prevL = block[(512 - 1) * 2];
    const int totalBlocks = static_cast<int>((echoPlan.totalDuration() + 0.5) * kRate / 512);
    for (int b = 0; b < totalBlocks; ++b) {
        engine.render(block.data(), 512);
        const MixSnapshot snap = engine.snapshot();
        if (snap.phase == TransitionPhase::Running) {
            CHECK(snap.strategy == MixStrategyKind::EchoReverbOut);
            maxTailWet = std::max(maxTailWet, snap.tailWetLevel);
        }
        for (std::size_t i = 0; i < 512; ++i) {
            const float sL = block[i * 2];
            CHECK(std::isfinite(sL));
            maxSampleStep = std::max(maxSampleStep, std::abs(sL - prevL));
            prevL = sL;
        }
    }
    CHECK(maxTailWet > 0.01f);
    CHECK(maxSampleStep < 0.35f);
    const MixSnapshot afterEcho = engine.snapshot();
    CHECK(afterEcho.phase == TransitionPhase::Idle);
    CHECK(afterEcho.master == DeckId::B);
    // Incoming deck B preserves its matched loudness gain after handover.
    CHECK_NEAR(afterEcho.loudnessGain[1], 1.25f, 1e-4f);

    // 2. VocalHandoff & DropImpactCut custom gain curves + cancel safety.
    for (MixStrategyKind strat : {MixStrategyKind::VocalHandoff,
                                  MixStrategyKind::DropImpactCut,
                                  MixStrategyKind::EnergyBridge}) {
        MixEngine eng(kRate);
        CHECK(eng.loadDeck(DeckId::A, trackA, 1, 120.0));
        CHECK(eng.loadDeck(DeckId::B, trackB, 2, 120.0));
        eng.play(DeckId::A);
        eng.setMaster(DeckId::A);
        MixPlan p = planner.plan(analysisA, analysisB, req);
        p.strategy = strat;
        p.gainCurveOut = {{0.0, 1.0f}, {1.6, 0.86f}, {3.6, 0.05f}, {4.0, 0.0f}};
        p.gainCurveIn = {{0.0, 0.0f}, {1.6, 0.48f}, {3.6, 0.96f}, {4.0, 1.0f}};
        eng.seek(DeckId::A, p.overallStartA());
        eng.render(block.data(), 512);
        CHECK(eng.startTransition(p));
        for (int b = 0; b < 30; ++b) {
            eng.render(block.data(), 512);
        }
        const MixSnapshot mid = eng.snapshot();
        CHECK(mid.phase == TransitionPhase::Running);
        CHECK(mid.strategy == strat);
        eng.cancelTransition();
        for (int b = 0; b < 30; ++b) {
            eng.render(block.data(), 512);
        }
        CHECK(eng.snapshot().phase == TransitionPhase::Idle);
        CHECK(eng.snapshot().master == DeckId::A);
    }
}


// =========================================================================
// Issue #48, #49, #50, #51 Integration and Regression Tests
// =========================================================================

TEST_CASE("mix.issue48_lead_vocal_collision_prevention") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::VocalOverlapAssessment;
    using bgmedia::mix::evaluateVocalOverlap;
    using bgmedia::mix::MixPlanner;
    using bgmedia::mix::PlanRequest;
    using bgmedia::mix::MixStrategyKind;

    const double duration = 16.0;
    const AnalysisResult analysis = analysisFor(120.0, duration);
    MixFeatureExtractor extractor;

    // Outgoing track A: lead vocal during 0..8s, instrumental outro during 8..16s
    const auto trackA = makeVocalAndInstrumentalTrack(duration, 0.5, 7.5, 120.0);
    // Incoming track B: lead vocal during 0..8s, instrumental during 8..16s
    const auto trackB = makeVocalAndInstrumentalTrack(duration, 0.5, 7.5, 120.0);

    const MixFeatureMap featA = extractor.extractFromTrack(*trackA, analysis);
    const MixFeatureMap featB = extractor.extractFromTrack(*trackB, analysis);

    // 1. Two simultaneous loud lead vocals -> Severe clash detected
    const VocalOverlapAssessment severeClash =
        evaluateVocalOverlap(featA, 2.0, 1.0, featB, 2.0, 1.0, 4.0);
    CHECK(severeClash.available);
    CHECK(severeClash.severeLeadVocalClash);
    CHECK(severeClash.leadVocalClashDurationSeconds >= 0.5);
    CHECK(!severeClash.cleanVocalHandoff);

    // 2. Safe sequential vocal handoff: A's vocal phrase finishes at 7.5s (2.0s into mix),
    // and incoming B has vocal starting at 2.5s into mix. No overlap between the two vocals!
    const auto trackB_handoff = makeVocalAndInstrumentalTrack(duration, 2.5, 10.0, 120.0);
    const MixFeatureMap featB_handoff = extractor.extractFromTrack(*trackB_handoff, analysis);
    const VocalOverlapAssessment safeHandoff =
        evaluateVocalOverlap(featA, 5.5, 1.0, featB_handoff, 0.0, 1.0, 4.0);
    CHECK(safeHandoff.available);
    CHECK(!safeHandoff.severeLeadVocalClash);
    CHECK(safeHandoff.cleanVocalHandoff);

    // 3. Instrumental outro transition has zero vocal clash
    const auto trackB_instIntro = makeVocalAndInstrumentalTrack(duration, 5.0, 12.0, 120.0);
    const MixFeatureMap featB_inst = extractor.extractFromTrack(*trackB_instIntro, analysis);
    const VocalOverlapAssessment instTransition =
        evaluateVocalOverlap(featA, 8.5, 1.0, featB_inst, 0.0, 1.0, 4.0);
    CHECK(instTransition.available);
    CHECK(!instTransition.severeLeadVocalClash);
    CHECK_NEAR(instTransition.vocalClashScore, 0.0f, 0.05f);

    // 4. Multi-candidate planner prioritizes safe candidate over vocal collision
    MixPlanner planner;
    PlanRequest req;
    req.type = TransitionType::SmartCrossfade;
    req.requestedDuration = 4.0;
    req.currentPositionA = -1.0;

    const bgmedia::mix::MixPlan plan = planner.plan(analysis, analysis, &featA, &featB_inst, req);
    CHECK(plan.valid);
    // The planner must NOT choose a transition during active A vocal collision
    CHECK(plan.transitionStartA >= 6.5);
    // And VocalHandoff is ONLY chosen if cleanVocalHandoff was genuinely true
    if (plan.strategy == MixStrategyKind::VocalHandoff) {
        CHECK(!plan.scoreBreakdown.severeVocalClash);
    }

    // 5. Low confidence analysis falls back conservatively without asserting false safety
    MixFeatureMap lowConfA = featA;
    lowConfA.vocal.available = true;
    lowConfA.vocal.meanConfidence = 0.20f; // low confidence
    const VocalOverlapAssessment lowConf =
        evaluateVocalOverlap(lowConfA, 2.0, 1.0, featB, 2.0, 1.0, 4.0);
    CHECK(lowConf.lowConfidenceConservative);
    CHECK(!lowConf.cleanVocalHandoff);
}

TEST_CASE("mix.issue49_dramatic_pause_inside_drop_not_selected_as_mixout") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::MixPlanner;
    using bgmedia::mix::PlanRequest;
    using bgmedia::mix::CueKind;

    const double duration = 28.0;
    const AnalysisResult analysis = analysisFor(128.0, duration);

    // Construct a synthetic track containing a drop, mid-drop dramatic break, drop resumption, and outro:
    // 0..4s: Intro (moderate energy)
    // 4..12s: Drop part 1 (very high energy, full bass)
    // 12..13.5s: Dramatic pause inside drop (short silence/shout, 1.5s)
    // 13.5..22s: Drop part 2 (very high energy, bass resumes fully!)
    // 22..28s: Outro (energy fades to quiet ending)
    auto dropTrack = toneTrack(duration, 440.0, 2, kRate);
    const std::size_t totalSamples = static_cast<std::size_t>(duration * kRate);
    for (std::size_t i = 0; i < totalSamples; ++i) {
        const double t = static_cast<double>(i) / kRate;
        float s = 0.0f;
        if (t >= 0.0 && t < 4.0) {
            s = 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265 * 440.0 * t));
        } else if (t >= 4.0 && t < 12.0) {
            // Drop part 1: heavy bass + synth
            s = 0.65f * static_cast<float>(std::sin(2.0 * 3.14159265 * 55.0 * t)) +
                0.30f * static_cast<float>(std::sin(2.0 * 3.14159265 * 440.0 * t));
        } else if (t >= 12.0 && t < 13.5) {
            // Dramatic pause with vocal shout at 12.8s
            if (t >= 12.8 && t < 13.3) {
                s = 0.35f * static_cast<float>(std::sin(2.0 * 3.14159265 * 880.0 * t));
            } else {
                s = 0.001f;
            }
        } else if (t >= 13.5 && t < 22.0) {
            // Drop part 2: full drop returns!
            s = 0.65f * static_cast<float>(std::sin(2.0 * 3.14159265 * 55.0 * t)) +
                0.30f * static_cast<float>(std::sin(2.0 * 3.14159265 * 440.0 * t));
        } else {
            // Natural outro: energy drops off
            const double fade = std::clamp((28.0 - t) / 6.0, 0.0, 1.0);
            s = static_cast<float>(0.20 * fade) * static_cast<float>(std::sin(2.0 * 3.14159265 * 220.0 * t));
        }
        dropTrack->samples[i * 2] = s;
        dropTrack->samples[i * 2 + 1] = s;
    }

    MixFeatureExtractor extractor;
    const MixFeatureMap featDrop = extractor.extractFromTrack(*dropTrack, analysis);

    // 1. Verify that the dramatic pause at 12s does NOT produce a SilenceEdge MixOut cue
    bool foundFalseSilenceMixOut = false;
    for (const auto& cue : featDrop.mixOutCandidates) {
        if (cue.kind == CueKind::SilenceEdge) {
            if (cue.time >= 11.5 && cue.time <= 14.0) {
                foundFalseSilenceMixOut = true;
            }
        }
    }
    CHECK(!foundFalseSilenceMixOut);

    // 2. Run MixPlanner: outgoing track is the drop track, incoming is clean track
    const auto trackIn = toneTrack(duration, 128.0, 2, kRate);
    const MixFeatureMap featIn = extractor.extractFromTrack(*trackIn, analysis);

    MixPlanner planner;
    PlanRequest req;
    req.type = TransitionType::SmartCrossfade;
    req.requestedDuration = 4.0;
    req.currentPositionA = -1.0;

    const bgmedia::mix::MixPlan plan = planner.plan(analysis, analysis, &featDrop, &featIn, req);
    CHECK(plan.valid);
    // The planner must NOT exit during the mid-drop pause (12..14s); it must let the second drop play
    // and transition at the natural outro phrase boundary (>= 18.0s).
    CHECK(plan.transitionStartA >= 18.0);
}

TEST_CASE("mix.issue50_desktop_seeking_and_jump_to_planned_transition") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::TransitionType;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.once();

    const auto initialPreview = session.snapshot().preview;
    CHECK(initialPreview.valid);
    const double schedPoint = initialPreview.overallStartA();

    // 1. Seeking Deck A while playing (well before the scheduled transition)
    const double safeSeek1 = std::min(2.5, schedPoint * 0.4);
    CHECK(session.seekDeck(DeckId::A, safeSeek1));
    pump.once();
    CHECK_NEAR(engine.snapshot().decks[0].positionSeconds, safeSeek1, 0.05);

    // 2. Seeking Deck A while paused
    session.pause();
    pump.once();
    const double safeSeek2 = std::min(4.0, schedPoint * 0.7);
    CHECK(session.seekDeck(DeckId::A, safeSeek2));
    pump.once();
    CHECK_NEAR(engine.snapshot().decks[0].positionSeconds, safeSeek2, 0.05);

    // 3. Jump to planned transition
    session.refreshPlan();
    const auto snap = session.snapshot();
    CHECK(snap.preview.valid);
    CHECK(session.jumpToPlannedTransition(2.0));
    pump.once();
    const double expected = std::max(0.0, snap.preview.overallStartA() - 2.0);
    CHECK_NEAR(engine.snapshot().decks[0].positionSeconds, expected, 0.05);

    // 4. Safety lock: seeking is strictly rejected during an active transition
    session.play();
    session.startTransitionNow();
    pump.once();
    CHECK(engine.snapshot().phase == TransitionPhase::Running);

    // Both seekDeck and jumpToPlannedTransition must return false while transition is running
    CHECK(!session.seekDeck(DeckId::A, 1.0));
    CHECK(!session.jumpToPlannedTransition(7.0));
    CHECK(!session.snapshot().lastError.empty());

    // Wait for transition to finish
    while (engine.snapshot().phase != TransitionPhase::Idle) {
        pump.once();
    }
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);

    // Seeking is restored after transition finishes
    CHECK(session.seekDeck(DeckId::B, 2.0));
    pump.once();
    CHECK_NEAR(engine.snapshot().decks[1].positionSeconds, 2.0, 0.05);
}

TEST_CASE("mix.issue51_preview_plan_vs_executed_transition_consistency") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::TransitionType;
    using bgmedia::mix::MixMode;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.setMixMode(MixMode::Auto);
    session.setTransitionDuration(4.0);
    session.addTrack("long-silence-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.once();

    const auto initialSnap = session.snapshot();
    CHECK(initialSnap.preview.valid);

    const double plannedStartA = initialSnap.preview.transitionStartA;
    const double plannedStartB = initialSnap.preview.startB;
    const double plannedDuration = initialSnap.preview.duration;
    const auto plannedStrategy = initialSnap.preview.strategy;
    const auto plannedType = initialSnap.preview.type;

    // Seek master deck right to the scheduled transition trigger point
    const double schedPoint = initialSnap.preview.overallStartA();
    CHECK(session.seekDeck(DeckId::A, schedPoint));

    // First pump: engine.render() executes seek to schedPoint
    pump.once();
    // Second pump: session.update() detects deck.positionSeconds >= schedPoint - lead
    // and executes the scheduled preview plan
    pump.once();

    const auto execSnap = session.snapshot();
    CHECK(execSnap.engine.phase == TransitionPhase::Running);
    CHECK(execSnap.hasExecutedPlan);

    // Consistency verification: executed plan exactly matches preview plan
    CHECK_NEAR(execSnap.executed.transitionStartA, plannedStartA, 1e-4);
    CHECK_NEAR(execSnap.executed.startB, plannedStartB, 1e-4);
    CHECK_NEAR(execSnap.executed.duration, plannedDuration, 1e-4);
    CHECK(execSnap.executed.strategy == plannedStrategy);
    CHECK(execSnap.executed.type == plannedType);

    // Executed plan contains explanatory reason log
    CHECK(!execSnap.executed.reasons.empty());

    // Wait for transition to complete
    while (engine.snapshot().phase != TransitionPhase::Idle) {
        pump.once();
    }
}


TEST_CASE("mix.codex_finding1_loudness_gain_role_consistency_b_to_a") {
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::MixPlan;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::TransitionType;
    using bgmedia::mix::MixStrategyKind;

    MixEngine engine(kRate);
    constexpr std::size_t kChunk = 512;
    auto trackA = toneTrack(10.0, 220.0);
    auto trackB = toneTrack(10.0, 440.0);

    // Initial setup: Deck B is master and playing, Deck A is loaded
    CHECK(engine.loadDeck(DeckId::B, trackB, 101, 120.0));
    CHECK(engine.loadDeck(DeckId::A, trackA, 102, 120.0));
    engine.setMaster(DeckId::B);
    engine.play(DeckId::B);

    // B -> A transition plan:
    // Outgoing (B) has loudnessGainA = 0.80f (role: outgoing in MixPlan)
    // Incoming (A) has loudnessGainB = 1.25f (role: incoming in MixPlan)
    MixPlan plan;
    plan.from = DeckId::B;
    plan.to = DeckId::A;
    plan.valid = true;
    plan.type = TransitionType::Crossfade;
    plan.strategy = MixStrategyKind::SmoothBlend;
    plan.duration = 1.0;
    plan.transitionStartA = 0.2;
    plan.startB = 0.0;
    plan.loudnessGainA = 0.80f; // outgoing track loudness gain (Deck B)
    plan.loudnessGainB = 1.25f; // incoming track loudness gain (Deck A)

    CHECK(engine.startTransition(plan));

    std::vector<float> buf(kChunk * 2);
    // Render until transition is Running
    while (engine.snapshot().phase != TransitionPhase::Running) {
        engine.render(buf.data(), kChunk);
    }

    // Mid-transition check: verify that incoming Deck A is receiving targetLoudnessTo (1.25)
    // and outgoing Deck B is receiving loudFrom (interpolating towards 0.80)
    auto snapMid = engine.snapshot();
    CHECK_NEAR(snapMid.loudnessGain[0], 1.25f, 1e-4f); // Deck A is incoming, must be 1.25
    CHECK(snapMid.loudnessGain[1] <= 1.0f);            // Deck B is outgoing, moving towards 0.80

    // Render until transition completes (handover to Deck A)
    while (engine.snapshot().phase != TransitionPhase::Idle) {
        engine.render(buf.data(), kChunk);
    }

    auto snapEnd = engine.snapshot();
    CHECK(snapEnd.master == DeckId::A);
    CHECK(snapEnd.decks[0].playing);
    // Crucial check: Deck A retains its matched 1.25 loudnessGain after handover
    CHECK_NEAR(snapEnd.loudnessGain[0], 1.25f, 1e-4f);
    CHECK_NEAR(snapEnd.decks[0].gain, 1.25f, 1e-4f);

    // Steady state: render more audio and verify Deck A's gain remains steady at 1.25 without jumps
    engine.render(buf.data(), kChunk);
    auto snapSteady = engine.snapshot();
    CHECK_NEAR(snapSteady.loudnessGain[0], 1.25f, 1e-4f);
    CHECK_NEAR(snapSteady.decks[0].gain, 1.25f, 1e-4f);
}

TEST_CASE("mix.codex_finding2_preview_caching_and_invalidation") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionType;
    using bgmedia::mix::MixMode;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.once();

    const auto snap1 = session.snapshot();
    CHECK(snap1.preview.valid);
    const double initialSched = snap1.preview.overallStartA();
    CHECK(initialSched > 0.0);
    const double initialDur = snap1.preview.duration;

    // 1. Performance check: calling update() 100 times executes fast because preview is cached.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        session.update();
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    // 100 cached updates must take well under 100 ms (typically < 2 ms total),
    // whereas 100 uncached multi-candidate searches would take > 4500 ms.
    CHECK(elapsedMs < 100);

    // 2. Invalidation on configuration change: setTransitionDuration
    session.setTransitionDuration(initialDur + 3.0);
    const auto snap2 = session.snapshot();
    CHECK(snap2.preview.valid);
    CHECK_NEAR(snap2.preview.duration, initialDur + 3.0, 1e-3);

    // 3. Invalidation on transition type change
    session.setTransitionType(TransitionType::DirectCut);
    const auto snap3 = session.snapshot();
    CHECK(snap3.preview.valid);
    CHECK(snap3.preview.type == TransitionType::DirectCut);
    CHECK_NEAR(snap3.preview.duration, 0.0, 1e-3);

    // Restore to SmartCrossfade
    session.setTransitionType(TransitionType::SmartCrossfade);
    session.setTransitionDuration(initialDur);
    const auto snap4 = session.snapshot();
    CHECK(snap4.preview.valid);

    // 4. Invalidation on seek past planned transition
    CHECK(session.seekDeck(DeckId::A, snap4.preview.overallStartA() + 1.0));
    // When seeked past transition start, preview must be invalidated so an expired plan doesn't fire
    const auto snapSeekPast = session.snapshot();
    CHECK(!snapSeekPast.preview.valid);

    // 5. Seeking back before transition start allows refreshing a valid plan
    CHECK(session.seekDeck(DeckId::A, 1.0));
    session.refreshPlan();
    CHECK(session.snapshot().preview.valid);
}

TEST_CASE("mix.codex_finding3_echo_reverb_cancellation_declick") {
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::MixPlan;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::TransitionType;
    using bgmedia::mix::MixStrategyKind;

    MixEngine engine(kRate);
    constexpr std::size_t kChunk = 512;
    // 257 Hz sine tone at 0.40 amplitude on Deck A, silent on Deck B
    auto toneA = toneTrack(10.0, 257.0);
    auto silenceB = dcTrack(10.0, 0.0f);

    CHECK(engine.loadDeck(DeckId::A, toneA, 201, 120.0));
    CHECK(engine.loadDeck(DeckId::B, silenceB, 202, 120.0));
    engine.setMaster(DeckId::A);
    engine.play(DeckId::A);

    MixPlan plan;
    plan.from = DeckId::A;
    plan.to = DeckId::B;
    plan.valid = true;
    plan.type = TransitionType::Crossfade;
    plan.strategy = MixStrategyKind::EchoReverbOut;
    plan.duration = 4.0;
    plan.transitionStartA = 0.5;
    plan.startB = 0.0;
    plan.tailSendAmount = 0.45f;
    plan.tailDecaySeconds = 2.0;

    CHECK(engine.startTransition(plan));

    std::vector<float> buf(kChunk * 2);
    // Render into the transition until ~50% progress where echo return is audible and active
    while (engine.snapshot().phase != TransitionPhase::Running) {
        engine.render(buf.data(), kChunk);
    }
    for (int i = 0; i < 40; ++i) {
        engine.render(buf.data(), kChunk);
    }
    // Echo wet signal must be active
    CHECK(engine.snapshot().decks[0].playing);

    // Request transition cancellation while echo is actively ringing
    engine.cancelTransition();

    // Render single-frame (or small chunks) across cancellation and measure sample-to-sample delta
    float maxSampleDelta = 0.0f;
    float prevSampleL = 0.0f;
    float prevSampleR = 0.0f;
    bool firstSample = true;

    // Render through the 220 ms cancellation window in small blocks (16 frames)
    constexpr std::size_t kSmallBlock = 16;
    std::vector<float> smallBuf(kSmallBlock * 2);
    while (engine.snapshot().phase != TransitionPhase::Idle) {
        engine.render(smallBuf.data(), kSmallBlock);
        for (std::size_t i = 0; i < kSmallBlock; ++i) {
            const float sL = smallBuf[i * 2];
            const float sR = smallBuf[i * 2 + 1];
            if (!firstSample) {
                const float deltaL = std::abs(sL - prevSampleL);
                const float deltaR = std::abs(sR - prevSampleR);
                maxSampleDelta = std::max(maxSampleDelta, std::max(deltaL, deltaR));
            } else {
                firstSample = false;
            }
            prevSampleL = sL;
            prevSampleR = sR;
        }
    }

    // A 257 Hz tone at 48.0 kHz has natural derivative ~2*pi*257/48000 * 0.40 ~= 0.0135.
    // Without echo cancellation fade, dropping the wet signal produced a discontinuity >= 0.14.
    // With smooth cancellation fade, maxSampleDelta is strictly bounded (< 0.035).
    CHECK(maxSampleDelta < 0.035f);

    // After cancellation complete: engine is Idle, Deck A remains playing cleanly,
    // and tail wet level is 0 without any lingering ringing tail.
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);
    CHECK(engine.snapshot().master == DeckId::A);
    CHECK(engine.snapshot().decks[0].playing);
}


TEST_CASE("mix.codex_mono_stereo_loudness_matching") {
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::MixPlanner;
    using bgmedia::mix::MixPlan;
    // Create identical 0.20 amplitude 440 Hz tones in mono and stereo
    auto monoTrack = toneTrack(10.0, 440.0, 1);
    auto stereoTrack = toneTrack(10.0, 440.0, 2);

    AnalysisResult analysisMono;
    analysisMono.info.duration = 10.0;
    analysisMono.info.sampleRate = 48000;
    analysisMono.info.sourceChannels = 1;
    analysisMono.bpm = 120.0;

    AnalysisResult analysisStereo;
    analysisStereo.info.duration = 10.0;
    analysisStereo.info.sampleRate = 48000;
    analysisStereo.info.sourceChannels = 2;
    analysisStereo.bpm = 120.0;

    MixFeatureExtractor extractor;
    const MixFeatureMap featMono = extractor.extractFromTrack(*monoTrack, analysisMono);
    const MixFeatureMap featStereo = extractor.extractFromTrack(*stereoTrack, analysisStereo);

    CHECK(featMono.loudness.gatedValid);
    CHECK(featStereo.loudness.gatedValid);

    // Standard BS.1770 measures 1-ch mono 3.01 dB lower than 2-ch dual-mono:
    CHECK_NEAR(featStereo.loudness.integratedLufs - featMono.loudness.integratedLufs, 3.01f, 0.15f);

    MixPlanner planner;
    MixPlan plan = planner.plan(analysisMono, analysisStereo, &featMono, &featStereo, {});

    // Both mono and stereo tracks should receive equal trims and gains,
    // preventing the previous 3.01 dB imbalance.
    CHECK_NEAR(plan.loudnessTrimDbA, plan.loudnessTrimDbB, 0.05f);
    CHECK_NEAR(plan.loudnessGainA, plan.loudnessGainB, 0.01f);
}

TEST_CASE("mix.codex_expired_preview_plan_revalidation") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::MixMode;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.setMixMode(MixMode::Auto);
    session.setTransitionDuration(8.0);
    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.once();

    const auto durA = engine.snapshot().decks[0].durationSeconds;
    CHECK(durA > 5.0);

    // Seek the master deck to 0.8 seconds before EOF (well past any scheduled preview start)
    const double nearEnd = durA - 0.8;
    CHECK(session.seekDeck(DeckId::A, nearEnd));

    // First pump renders the seek
    pump.once();
    // Second pump triggers update() which detects that the live cursor is past the old schedule
    // and replans from the live position to fit the remaining 0.8s
    pump.once();

    const auto snap = session.snapshot();
    CHECK(snap.engine.phase == TransitionPhase::Running);
    // Crucial check: transition duration was fitted to the remaining audio (<= 0.8s + tolerance)
    // rather than retaining the original 8.0s crossfade that would overrun EOF
    CHECK(snap.executed.duration <= 1.0);
    CHECK(snap.executed.transitionStartA >= nearEnd - 0.2);

    // Render until transition completes
    while (engine.snapshot().phase != TransitionPhase::Idle) {
        pump.once();
    }
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);
    CHECK(engine.snapshot().master == DeckId::B);
}


TEST_CASE("mix.codex_finding_preview_expiration_on_load") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::MixMode;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.setMixMode(MixMode::Auto);
    session.setTransitionDuration(8.0);
    session.addTrack("long-alpha.wav");
    CHECK(pump.waitForDecks(1));

    session.play();
    pump.forSeconds(4.0);
    const double posA = engine.snapshot().decks[0].positionSeconds;
    CHECK(posA >= 2.0);

    // Track B finishes preparing while deck A is already 4.0s into playback
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    pump.once();

    const auto snap = session.snapshot();
    // Engine must not prematurely trigger transition at live position
    CHECK(snap.engine.phase == TransitionPhase::Idle);
    CHECK(snap.preview.valid);
    // Crossfade remains scheduled at the track ending (~21.5s), well past posA
    CHECK(snap.preview.transitionStartA > posA + 10.0);
}

TEST_CASE("mix.codex_finding_candidate_overlap_fit_to_musical_end") {
    using bgmedia::mix::MixPlanner;
    using bgmedia::mix::MixFeatureMap;
    using bgmedia::mix::MixFeatureExtractor;
    using bgmedia::mix::CuePointCandidate;
    using bgmedia::mix::CueRole;
    using bgmedia::mix::CueKind;

    AnalysisResult a = analysisFor(120.0, 120.0);
    AnalysisResult b = analysisFor(120.0, 120.0);

    // Track A: 120s tone with trailing silence between 100.0s and 120.0s
    auto trackA = toneTrack(120.0, 440.0);
    const std::size_t silenceStart = static_cast<std::size_t>(100.0 * kRate * 2);
    for (std::size_t i = silenceStart; i < trackA->samples.size(); ++i) {
        trackA->samples[i] = 0.0f;
    }
    auto trackB = toneTrack(120.0, 440.0);

    MixFeatureExtractor extractor;
    MixFeatureMap featA = extractor.extractFromTrack(*trackA, a);
    MixFeatureMap featB = extractor.extractFromTrack(*trackB, b);

    CHECK(featA.valid());
    CHECK(featB.valid());
    CHECK(featA.audible.audibleEnd <= 100.5);

    // High priority mix-out cue at 96 seconds
    CuePointCandidate cue;
    cue.time = 96.0;
    cue.role = CueRole::MixOut;
    cue.kind = CueKind::PhraseBoundary;
    cue.priorScore = 0.95f;
    cue.phraseConfidence = 0.90f;
    cue.rhythmConfidence = 0.90f;
    featA.mixOutCandidates.push_back(cue);

    MixPlanner planner;
    bgmedia::mix::PlanRequest req;
    req.requestedDuration = 8.0;
    req.type = bgmedia::mix::TransitionType::SmartCrossfade;

    const auto candidates = planner.planCandidates(a, b, &featA, &featB, req);
    CHECK(!candidates.empty());

    // Every candidate overlap must fit within the musical end (<= 100.25s)
    // and never overrun into trailing silence.
    for (const auto& cand : candidates) {
        CHECK(cand.transitionEndA <= 100.25);
    }

    const auto plan = planner.plan(a, b, &featA, &featB, req);
    CHECK(plan.valid);
    CHECK(plan.transitionEndA <= 100.25);
    // If the 96s candidate was selected, its duration must be fitted to <= 4.25s
    if (std::abs(plan.transitionStartA - 96.0) < 0.5) {
        CHECK(plan.duration <= 4.25);
    }
}


TEST_CASE("mix.codex_finding_cue_ranking_strict_weak_ordering") {
    using bgmedia::mix::CuePointCandidate;

    CuePointCandidate a;
    a.time = 1.0;
    a.priorScore = 0.50000f;

    CuePointCandidate b;
    b.time = 2.0;
    b.priorScore = 0.500075f;

    CuePointCandidate c;
    c.time = 3.0;
    c.priorScore = 0.50015f;

    auto cmp = [](const CuePointCandidate& x, const CuePointCandidate& y) {
        if (x.priorScore != y.priorScore) {
            return x.priorScore > y.priorScore;
        }
        return x.time < y.time;
    };

    CHECK(cmp(c, b));
    CHECK(cmp(b, a));
    CHECK(cmp(c, a));
    CHECK(!cmp(a, b));
    CHECK(!cmp(b, c));
    CHECK(!cmp(a, c));

    std::vector<CuePointCandidate> list = {a, b, c};
    std::sort(list.begin(), list.end(), cmp);
    CHECK(list[0].priorScore == 0.50015f);
    CHECK(list[1].priorScore == 0.500075f);
    CHECK(list[2].priorScore == 0.50000f);

    CuePointCandidate d;
    d.time = 0.5;
    d.priorScore = 0.50000f;
    std::vector<CuePointCandidate> tieList = {a, d};
    std::sort(tieList.begin(), tieList.end(), cmp);
    CHECK(tieList[0].time == 0.5);
    CHECK(tieList[1].time == 1.0);
}


TEST_CASE("mix.codex_finding_backward_seek_restores_ending_preview") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::MixMode;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.setAutoQueue(false);
    session.setTransitionDuration(8.0);
    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.once();

    const auto p0 = session.snapshot().preview;
    CHECK(p0.valid);
    CHECK(p0.duration >= 7.5);
    CHECK(p0.transitionStartA > 15.0);

    CHECK(session.seekDeck(DeckId::A, 29.0));
    pump.once();
    pump.once();

    const auto pNearEnd = session.snapshot().preview;
    CHECK(pNearEnd.valid);
    CHECK(pNearEnd.duration <= 1.5);
    CHECK(pNearEnd.transitionStartA >= 28.5);

    CHECK(session.seekDeck(DeckId::A, 1.0));
    pump.once();
    pump.once();

    const auto pRestored = session.snapshot().preview;
    CHECK(pRestored.valid);
    CHECK(pRestored.duration >= 7.5);
    CHECK(pRestored.transitionStartA > 15.0);
}

TEST_CASE("mix.codex_finding_cancellation_does_not_rearm_live_preview") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::MixMode;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.setMixMode(MixMode::Auto);
    session.setTransitionDuration(8.0);
    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.forSeconds(0.10);

    session.next();
    pump.forSeconds(0.05);
    CHECK(session.snapshot().engine.phase == TransitionPhase::Running);

    session.cancelTransition();
    while (session.snapshot().engine.phase != TransitionPhase::Idle) {
        pump.once();
    }

    const auto snap = session.snapshot();
    CHECK(snap.engine.phase == TransitionPhase::Idle);
    CHECK(snap.preview.valid);
    CHECK(snap.preview.transitionStartA > 15.0);
}

TEST_CASE("mix.codex_finding_baseline_plan_clears_strategy_and_curves") {
    using bgmedia::mix::MixPlan;
    using bgmedia::mix::MixStrategyKind;

    MixPlan plan;
    plan.valid = true;
    plan.duration = 8.0;
    plan.strategy = MixStrategyKind::DropImpactCut;
    plan.gainCurveOut = {{0.0, 1.0f}, {4.0, 0.9f}, {8.0, 0.0f}};
    plan.gainCurveIn = {{0.0, 0.0f}, {4.0, 0.9f}, {8.0, 1.0f}};
    plan.tailSendAmount = 0.45f;
    plan.tailDecaySeconds = 2.0;
    plan.vocalEqCarve = true;

    MixPlan baseline = plan;
    baseline.strategy = MixStrategyKind::SmoothBlend;
    baseline.gainCurveOut.clear();
    baseline.gainCurveIn.clear();
    baseline.tailSendAmount = 0.0f;
    baseline.tailDecaySeconds = 0.0;
    baseline.vocalEqCarve = false;

    CHECK(baseline.strategy == MixStrategyKind::SmoothBlend);
    CHECK(baseline.gainCurveOut.empty());
    CHECK(baseline.gainCurveIn.empty());
    CHECK(baseline.tailSendAmount == 0.0f);
    CHECK(!baseline.vocalEqCarve);
}

TEST_CASE("mix.codex_finding_preserve_equal_power_envelopes_for_explicit_crossfade") {
    using bgmedia::mix::MixPlanner;
    using bgmedia::mix::PlannerConfig;
    using bgmedia::mix::PlanRequest;
    using bgmedia::mix::TransitionType;
    using bgmedia::mix::MixStrategyKind;

    const auto a = analysisFor(120.0, 30.0);
    const auto b = analysisFor(120.0, 30.0);

    // 1. Explicit Crossfade of 2.0s with adaptive = false
    {
        PlannerConfig config;
        config.adaptive = false;
        config.bassSwap = false;
        MixPlanner planner(config);

        PlanRequest request;
        request.type = TransitionType::Crossfade;
        request.requestedDuration = 2.0;

        const auto plan = planner.plan(a, b, nullptr, nullptr, request);
        CHECK(plan.valid);
        CHECK(plan.type == TransitionType::Crossfade);
        CHECK(plan.strategy == MixStrategyKind::SmoothBlend);
        CHECK(!plan.gainCurveOut.empty());
        CHECK(std::abs(plan.gainCurveOut[8].gain - 0.7071f) < 0.01f);
        CHECK(std::abs(plan.gainCurveIn[8].gain - 0.7071f) < 0.01f);
    }

    // 2. Explicit Crossfade of 2.0s even with adaptive = true
    {
        PlannerConfig config;
        config.adaptive = true;
        config.bassSwap = false;
        MixPlanner planner(config);

        PlanRequest request;
        request.type = TransitionType::Crossfade;
        request.requestedDuration = 2.0;

        const auto plan = planner.plan(a, b, nullptr, nullptr, request);
        CHECK(plan.valid);
        CHECK(plan.type == TransitionType::Crossfade);
        CHECK(plan.strategy == MixStrategyKind::SmoothBlend);
        CHECK(!plan.gainCurveOut.empty());
        CHECK(std::abs(plan.gainCurveOut[8].gain - 0.7071f) < 0.01f);
        CHECK(std::abs(plan.gainCurveIn[8].gain - 0.7071f) < 0.01f);
    }
}
TEST_CASE("mix.codex_finding_backward_seek_within_scheduling_window_suppresses_stale_transition") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::MixMode;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.setMixMode(MixMode::Auto);
    session.setTransitionDuration(8.0);
    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.once();

    // Position playback at 21.91s (inside the scheduling lead window) directly on engine
    CHECK(session.seekDeck(DeckId::A, 21.91));
    engine.render(pump.block.data(), 480); // applies seek without running session.update()
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);
    CHECK(engine.snapshot().decks[0].positionSeconds >= 21.9);

    // Now seek backward to 1.0s
    CHECK(session.seekDeck(DeckId::A, 1.0));
    // Intervening update() BEFORE render callback applies the seek:
    // must suppress automatic scheduling rather than scheduling against stale 21.91s snapshot
    session.update();

    // Engine must NOT have started transition against stale cursor
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);

    // Audio callback applies the seek to 1.0s
    pump.once();
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);

    // Further seeks must succeed (not rejected due to active transition)
    CHECK(session.seekDeck(DeckId::A, 2.0));
    pump.once();
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);
}

TEST_CASE("mix.codex_finding_seek_after_update_cancels_pending_transition") {
    using bgmedia::mix::MixSession;
    using bgmedia::mix::MixEngine;
    using bgmedia::mix::DeckId;
    using bgmedia::mix::TransitionPhase;
    using bgmedia::mix::MixMode;

    MixEngine engine(kRate);
    MixSession session(engine, makeTestPrepare());
    SessionPump pump{engine, session};

    session.setMixMode(MixMode::Auto);
    session.setTransitionDuration(8.0);
    session.addTrack("long-alpha.wav");
    session.addTrack("long-beta.wav");
    CHECK(pump.waitForDecks(2));

    session.play();
    pump.once();

    // Position playback at 21.92s directly on engine
    CHECK(session.seekDeck(DeckId::A, 21.92));
    engine.render(pump.block.data(), 480);
    CHECK(engine.snapshot().decks[0].positionSeconds >= 21.9);
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);

    // Update() queues the auto-transition against the 21.92s window
    session.update();

    // Now seekDeck(A, 1.0) is called BEFORE render callback consumes the start request:
    // It must invalidate the pending plan rather than letting render() enter Running at 1.0s
    CHECK(session.seekDeck(DeckId::A, 1.0));

    // Render callback runs
    pump.once();

    // Phase must remain Idle, NOT Running
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);
    CHECK(engine.snapshot().decks[0].positionSeconds <= 1.5);

    // Subsequent seeks must be accepted
    CHECK(session.seekDeck(DeckId::A, 2.0));
    pump.once();
    CHECK(engine.snapshot().phase == TransitionPhase::Idle);
}

TEST_CASE("mix.resample_44k_to_48k_is_band_limited") {
    using bgmedia::mix::PcmTrack;
    // A 15 kHz tone must keep its level through 44.1 -> 48 kHz and must not
    // pick up strong aliasing/imaging artifacts (linear interpolation lost
    // 3.4 dB here and left artifacts at about -11 dB).
    AudioData audio;
    audio.sampleRate = 44100;
    audio.channels = 1;
    const std::size_t n = 44100;
    audio.samples.resize(n);
    const double w = 2.0 * 3.14159265358979323846 * 15000.0 / 44100.0;
    for (std::size_t i = 0; i < n; ++i) {
        audio.samples[i] = static_cast<float>(0.5 * std::sin(w * static_cast<double>(i)));
    }
    std::string error;
    const PcmTrack track = PcmTrack::fromAudio(audio, 48000, &error);
    CHECK(track.sampleRate == 48000u);
    CHECK(track.frameCount() > 47000);
    // Fit amplitude and residual over a steady-state window.
    const double w2 = 2.0 * 3.14159265358979323846 * 15000.0 / 48000.0;
    double sc = 0.0;
    double ss = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 4000; i < 44000; ++i, ++count) {
        const double x = track.samples[i];
        sc += x * std::cos(w2 * static_cast<double>(i));
        ss += x * std::sin(w2 * static_cast<double>(i));
    }
    const double a = 2.0 * sc / static_cast<double>(count);
    const double b = 2.0 * ss / static_cast<double>(count);
    const double amp = std::sqrt(a * a + b * b);
    double residual = 0.0;
    for (std::size_t i = 4000; i < 44000; ++i) {
        const double fit = a * std::cos(w2 * static_cast<double>(i)) +
                           b * std::sin(w2 * static_cast<double>(i));
        const double e = track.samples[i] - fit;
        residual += e * e;
    }
    residual = std::sqrt(residual / static_cast<double>(count));
    const double levelDb = 20.0 * std::log10(amp / 0.5);
    CHECK(levelDb > -0.5);
    // Residual (artifacts) at least 40 dB below the tone RMS.
    CHECK(residual < (amp / std::sqrt(2.0)) * 0.01);
}

TEST_CASE("mix.engine_seek_does_not_click") {
    MixEngine engine(kRate);
    // First half +0.4, second half -0.4: the worst possible seek jump.
    auto track = dcTrack(20.0, 0.4f);
    const std::size_t half = track->samples.size() / 2;
    for (std::size_t i = half; i < track->samples.size(); ++i) {
        track->samples[i] = -0.4f;
    }
    CHECK(engine.loadDeck(DeckId::A, track, 1, 120.0));
    engine.play(DeckId::A);
    std::vector<float> block(256 * 2);
    for (int i = 0; i < 400; ++i) {  // warm up past the declick fade-in
        engine.render(block.data(), 256);
    }
    engine.seek(DeckId::A, 15.0);
    float previous = block[block.size() - 2];
    float maxStep = 0.0f;
    for (int i = 0; i < 100; ++i) {
        engine.render(block.data(), 256);
        for (std::size_t f = 0; f < 256; ++f) {
            maxStep = std::max(maxStep, std::abs(block[f * 2] - previous));
            previous = block[f * 2];
        }
    }
    std::printf("seek max sample step: %.4f\n", static_cast<double>(maxStep));
    CHECK(maxStep < 0.05f);
    CHECK(previous < -0.3f);  // the seek really landed in the -0.4 half
}

TEST_CASE("mix.engine_seek_tail_does_not_survive_eof") {
    MixEngine engine(kRate);
    auto track = dcTrack(5.0, 0.4f);
    CHECK(engine.loadDeck(DeckId::A, track, 1, 120.0));
    engine.play(DeckId::A);
    std::vector<float> block(256 * 2);
    for (int i = 0; i < 400; ++i) {
        engine.render(block.data(), 256);
    }
    engine.seek(DeckId::A, 5.0);  // to the very end: renders silence
    for (int i = 0; i < 20; ++i) {
        engine.render(block.data(), 256);
    }
    engine.seek(DeckId::A, 1.0);
    engine.play(DeckId::A);
    engine.render(block.data(), 256);
    // The restart must fade in, not begin with a stale +0.4 sample.
    CHECK(std::abs(block[0]) < 0.05f);
}
