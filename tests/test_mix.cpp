#include "framework.hpp"

#include "bgmedia/mix/Crossfade.hpp"
#include "bgmedia/mix/MixEngine.hpp"
#include "bgmedia/mix/MixPlanner.hpp"
#include "bgmedia/mix/MixSession.hpp"
#include "bgmedia/mix/PcmTrack.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
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
using bgmedia::mix::TransitionPhase;
using bgmedia::mix::TransitionType;

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
                                    std::uint32_t channels = 2) {
    auto track = std::make_shared<PcmTrack>();
    track->sampleRate = kRate;
    track->channels = channels;
    const std::size_t frames = static_cast<std::size_t>(seconds * kRate);
    track->samples.resize(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * frequency *
                             static_cast<double>(i) / kRate;
        const float value = static_cast<float>(0.4 * std::sin(phase));
        for (std::uint32_t c = 0; c < channels; ++c) {
            track->samples[i * channels + c] = value;
        }
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
        CHECK(plan.experimental);
        CHECK(plan.pitchShiftWarning);
        CHECK(plan.tempoCurve.size() > 20);
        CHECK_NEAR(plan.tempoCurve.front().rate, 1.0, 1e-4);
        const double ratio = toBpm / fromBpm;
        CHECK_NEAR(plan.tempoCurve.back().rate, ratio, 1e-3);

        double integral = 0.0;
        for (std::size_t i = 1; i < plan.tempoCurve.size(); ++i) {
            const auto& p0 = plan.tempoCurve[i - 1];
            const auto& p1 = plan.tempoCurve[i];
            integral += (p0.rate + p1.rate) * 0.5 * (p1.time - p0.time);
        }
        const double mean = integral / std::max(1e-9, plan.duration);
        CHECK_NEAR(mean, ratio, 0.005);
        CHECK_NEAR(plan.tempoCurve.back().bpm, toBpm, 0.5);
        std::printf("mix ramp %.0f->%.0f: mean=%.5f target=%.5f pitchWarn=%d\n",
                    fromBpm, toBpm, mean, ratio,
                    plan.pitchShiftWarning ? 1 : 0);
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
    const MixPlan tooFar =
        strictPlanner.plan(analysisFor(123.0), analysisFor(128.0), request);
    CHECK(tooFar.type == TransitionType::Crossfade);
    CHECK(tooFar.fallbackReason.find("exceeds") != std::string::npos);

    MixPlanner disabled;
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
        out.analysis =
            std::make_shared<AnalysisResult>(analysisFor(bpm, duration));
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
                engine.stop();
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
