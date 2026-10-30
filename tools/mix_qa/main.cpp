// BG Mix QA harness: prepares real tracks through the same BG Beat + decoder
// path as the desktop app, renders a transition offline, and (optionally)
// repeats it on the real output device to report latency and callback load.
//
// This is an instrument, not a benchmark of synthetic signals: the goal is to
// make the numbers in the pull request reproducible.

#include "bgmedia/audio/MiniaudioMixDevice.hpp"
#include "bgmedia/audio/MiniaudioMixLoader.hpp"
#include "bgmedia/mix/MixEngine.hpp"
#include "bgmedia/mix/MixPlanner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bgmedia::MiniaudioMixDevice;
using bgmedia::MixTrackLoadOptions;
using bgmedia::mix::DeckId;
using bgmedia::mix::MixEngine;
using bgmedia::mix::MixPlan;
using bgmedia::mix::MixPlanner;
using bgmedia::mix::PlanRequest;
using bgmedia::mix::PlannerConfig;
using bgmedia::mix::PreparedMixTrack;
using bgmedia::mix::TransitionPhase;
using bgmedia::mix::TransitionType;

constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kBlock = 512;

struct Options {
    std::filesystem::path a;
    std::filesystem::path b;
    std::filesystem::path cache = ".bgmedia_cache";
    TransitionType type = TransitionType::SmartCrossfade;
    double duration = 0.0;
    double rampPercent = 6.0;
    double rampBars = 0.0;
    std::string rampMode = "auto";  // off | auto | manual
    std::string curve = "scurve";   // linear | smooth | scurve
    bool pitchLock = true;
    bool bassSwap = true;
    double warmupSeconds = 8.0;
    double tailSeconds = 1.5;
    double stressSeconds = 0.0;
    std::filesystem::path exportDir;
    std::string name = "mix";
    bool device = false;
    bool noCache = false;
};

struct Metrics {
    std::uint64_t frames = 0;
    std::uint64_t nonFinite = 0;
    std::uint64_t clipped = 0;
    std::uint64_t longestZeroRun = 0;
    std::uint64_t zeroRun = 0;
    float peak = 0.0f;
    double sumSquares = 0.0;
};

void measure(const float* samples, std::size_t frames, Metrics& metrics) {
    for (std::size_t i = 0; i < frames * 2; ++i) {
        const float value = samples[i];
        if (!std::isfinite(value)) {
            ++metrics.nonFinite;
            continue;
        }
        const float magnitude = std::abs(value);
        metrics.peak = std::max(metrics.peak, magnitude);
        if (magnitude >= 0.999f) {
            ++metrics.clipped;
        }
        metrics.sumSquares += static_cast<double>(value) * value;
        if (value == 0.0f) {
            ++metrics.zeroRun;
            metrics.longestZeroRun =
                std::max(metrics.longestZeroRun, metrics.zeroRun);
        } else {
            metrics.zeroRun = 0;
        }
    }
    metrics.frames += frames;
}

double rms(const Metrics& metrics) {
    if (metrics.frames == 0) {
        return 0.0;
    }
    return std::sqrt(metrics.sumSquares /
                     (2.0 * static_cast<double>(metrics.frames)));
}

void printUsage() {
    std::printf(
        "usage: bgmedia_mix_qa --a <track> --b <track> [options]\n"
        "  --type smart|beat|tempo|phrase|cut  transition type (default smart)\n"
        "  --duration <s>                manual crossfade length (0 = auto)\n"
        "  --ramp <percent>              tempo ramp max delta (default 6)\n"
        "  --ramp-mode off|auto|manual   tempo ramp mode (default auto)\n"
        "  --ramp-bars <n>               ramp length in bars (0 = auto)\n"
        "  --curve linear|smooth|scurve  tempo curve shape (default scurve)\n"
        "  --no-pitch-lock               use linear resampling (pitch follows)\n"
        "  --no-bass-swap                disable the three-band bass swap\n"
        "  --export <dir>                write <name>_mix.wav and <name>_baseline.wav\n"
        "  --name <label>                artifact name (default mix)\n"
        "  --warmup <s>                  playback before the transition (8)\n"
        "  --stress <s>                  concurrent play/pause/seek/load/clear/\n"
        "                                next/stop stress for this many seconds\n"
        "  --device                      also run on the real output device\n"
        "  --cache <dir>                 analysis cache directory\n"
        "  --no-cache                    ignore the analysis cache\n");
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--a") {
            const char* value = next("--a");
            if (!value) return false;
            options.a = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(value)));
        } else if (arg == "--b") {
            const char* value = next("--b");
            if (!value) return false;
            options.b = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(value)));
        } else if (arg == "--type") {
            const char* value = next("--type");
            if (!value) return false;
            const std::string_view type = value;
            if (type == "smart") {
                options.type = TransitionType::SmartCrossfade;
            } else if (type == "beat") {
                options.type = TransitionType::Beatmatched;
            } else if (type == "tempo") {
                options.type = TransitionType::TempoRamp;
            } else if (type == "phrase") {
                options.type = TransitionType::PhraseAware;
            } else if (type == "cut") {
                options.type = TransitionType::DirectCut;
            } else {
                std::fprintf(stderr, "unknown --type %s\n", value);
                return false;
            }
        } else if (arg == "--duration") {
            const char* value = next("--duration");
            if (!value) return false;
            options.duration = std::atof(value);
        } else if (arg == "--ramp") {
            const char* value = next("--ramp");
            if (!value) return false;
            options.rampPercent = std::atof(value);
        } else if (arg == "--ramp-mode") {
            const char* value = next("--ramp-mode");
            if (!value) return false;
            options.rampMode = value;
            if (options.rampMode != "off" && options.rampMode != "auto" &&
                options.rampMode != "manual") {
                std::fprintf(stderr, "unknown --ramp-mode %s\n", value);
                return false;
            }
        } else if (arg == "--ramp-bars") {
            const char* value = next("--ramp-bars");
            if (!value) return false;
            options.rampBars = std::atof(value);
        } else if (arg == "--curve") {
            const char* value = next("--curve");
            if (!value) return false;
            options.curve = value;
            if (options.curve != "linear" && options.curve != "smooth" &&
                options.curve != "scurve") {
                std::fprintf(stderr, "unknown --curve %s\n", value);
                return false;
            }
        } else if (arg == "--no-pitch-lock") {
            options.pitchLock = false;
        } else if (arg == "--no-bass-swap") {
            options.bassSwap = false;
        } else if (arg == "--export") {
            const char* value = next("--export");
            if (!value) return false;
            options.exportDir = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(value)));
        } else if (arg == "--name") {
            const char* value = next("--name");
            if (!value) return false;
            options.name = value;
        } else if (arg == "--warmup") {
            const char* value = next("--warmup");
            if (!value) return false;
            options.warmupSeconds = std::atof(value);
        } else if (arg == "--stress") {
            const char* value = next("--stress");
            if (!value) return false;
            options.stressSeconds = std::atof(value);
        } else if (arg == "--cache") {
            const char* value = next("--cache");
            if (!value) return false;
            options.cache = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(value)));
        } else if (arg == "--no-cache") {
            options.noCache = true;
        } else if (arg == "--device") {
            options.device = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return false;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", arg.data());
            return false;
        }
    }
    if (options.a.empty() || options.b.empty()) {
        printUsage();
        return false;
    }
    return true;
}

bool prepare(const Options& options, const std::filesystem::path& path,
             PreparedMixTrack& out, const char* label) {
    MixTrackLoadOptions load;
    load.cacheDirectory = options.cache;
    load.useCache = !options.noCache;
    load.targetSampleRate = kRate;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = bgmedia::prepareMixTrack(
        path, load, out, error,
        [](float fraction, std::string_view stage) {
            if (!stage.empty()) {
                std::printf("  prepare %s: %.0f%%\n", stage.data(),
                            static_cast<double>(fraction) * 100.0);
            }
        });
    const double ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start)
            .count();
    if (!ok) {
        std::fprintf(stderr, "prepare failed: %s\n", error.c_str());
        return false;
    }
    std::printf("  prepared %s in %.0f ms (%.1f s, %.1f BPM, conf %.2f)\n",
                label, ms, out.audio->duration(), out.analysis->bpm,
                static_cast<double>(out.analysis->bpmConfidence));
    return true;
}

MixPlan planTransition(const Options& options, const PreparedMixTrack& a,
                       const PreparedMixTrack& b, const MixEngine& engine) {
    PlannerConfig config = PlannerConfig::forTransition(options.type);
    config.tempoRampMode =
        options.rampMode == "off"
            ? bgmedia::mix::TempoRampMode::Off
            : (options.rampMode == "manual"
                   ? bgmedia::mix::TempoRampMode::Manual
                   : bgmedia::mix::TempoRampMode::Auto);
    config.allowTempoRamp =
        config.tempoRampMode != bgmedia::mix::TempoRampMode::Off;
    config.tempoRampBars = options.rampBars;
    config.tempoCurveKind =
        options.curve == "linear"
            ? bgmedia::mix::TempoCurveKind::Linear
            : (options.curve == "smooth"
                   ? bgmedia::mix::TempoCurveKind::Smooth
                   : bgmedia::mix::TempoCurveKind::SCurve);
    config.pitchLock = options.pitchLock;
    config.bassSwap = options.bassSwap;
    config.maxTempoDeltaPercent = std::clamp(options.rampPercent, 1.0, 16.0);
    MixPlanner planner(config);

    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    PlanRequest request;
    request.type = options.type;
    request.requestedDuration = options.duration;
    request.currentPositionA = snap.decks[0].positionSeconds;
    request.autoStart = true;
    request.trackNameB = b.audio ? "B" : "";
    return planner.plan(*a.analysis, *b.analysis, request);
}

void printPlan(const MixPlan& plan) {
    std::printf(
        "  plan: %s (%s) %.2f s, confidence %.2f, beatAligned=%d "
        "error=%.1f ms\n",
        bgmedia::mix::transitionTypeName(plan.type),
        bgmedia::mix::transitionStatusLabel(plan.type), plan.duration,
        static_cast<double>(plan.confidence), plan.beatAligned ? 1 : 0,
        plan.alignmentErrorMs);
    for (const std::string& reason : plan.reasons) {
        std::printf("    - %s\n", reason.c_str());
    }
    if (!plan.fallbackReason.empty()) {
        std::printf("    ! fallback: %s\n", plan.fallbackReason.c_str());
    }
    std::printf(
        "    window: pre %.2f s + fade %.2f s, A cue %.3f -> %.3f, "
        "B cue %.3f, ramp %.1f%%\n",
        plan.preTransitionSeconds, plan.duration, plan.transitionStartA,
        plan.transitionEndA, plan.startB, plan.tempoRampPercent);
    std::printf(
        "    align: beat=%d downbeat=%d snap=%.1f ms phase=%.1f ms, "
        "phrase=%d (%.2f), bassSwap=%d pitchLock=%d\n",
        plan.beatAligned ? 1 : 0, plan.downbeatAligned ? 1 : 0,
        plan.alignmentErrorMs, plan.beatPhaseErrorMs,
        plan.phraseBoundary ? 1 : 0,
        static_cast<double>(plan.phraseConfidence), plan.bassSwap ? 1 : 0,
        plan.pitchLock ? 1 : 0);
    if (!plan.tempoCurve.empty()) {
        std::printf("    tempo curve: %zu points, %.4f -> %.4f\n",
                    plan.tempoCurve.size(), plan.tempoCurve.front().rate,
                    plan.tempoCurve.back().rate);
    }
}

bool writeWav(const std::filesystem::path& path,
              const std::vector<float>& interleaved) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
        return false;
    }
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(interleaved.size() * sizeof(std::int16_t));
    const std::uint32_t channels = 2;
    const std::uint32_t rate = kRate;
    const std::uint32_t byteRate = rate * channels * 2;
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * 2);
    auto u32 = [&](std::uint32_t v) {
        file.write(reinterpret_cast<const char*>(&v), 4);
    };
    auto u16 = [&](std::uint16_t v) {
        file.write(reinterpret_cast<const char*>(&v), 2);
    };
    file.write("RIFF", 4);
    u32(36 + dataBytes);
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(static_cast<std::uint16_t>(channels));
    u32(rate);
    u32(byteRate);
    u16(blockAlign);
    u16(16);
    file.write("data", 4);
    u32(dataBytes);
    for (float value : interleaved) {
        const float clamped = std::clamp(value, -1.0f, 1.0f);
        const auto sample = static_cast<std::int16_t>(
            std::lround(clamped * 32767.0f));
        file.write(reinterpret_cast<const char*>(&sample), 2);
    }
    return true;
}

struct CaseResult {
    Metrics metrics;
    std::vector<float> pcm;
    bgmedia::mix::MixSnapshot after;
    double wallSeconds = 0.0;
    bool ok = false;
};

CaseResult renderCase(const PreparedMixTrack& a, const PreparedMixTrack& b,
                      const MixPlan& plan, bool captureAudio) {
    CaseResult result;
    if (!plan.valid) {
        return result;
    }
    MixEngine engine(kRate);
    // The planner flag must select the real renderer, otherwise --no-pitch-lock
    // would only change metadata and both A/B files would be identical.
    engine.setTimeStretch(
        plan.pitchLock
            ? std::shared_ptr<const bgmedia::mix::ITimeStretchProcessor>(
                  std::make_shared<bgmedia::mix::WsolaStretcher>())
            : std::shared_ptr<const bgmedia::mix::ITimeStretchProcessor>(
                  std::make_shared<bgmedia::mix::LinearResampleStretcher>()));
    if (!engine.loadDeck(DeckId::A, a.audio, 1, a.analysis->bpm) ||
        !engine.loadDeck(DeckId::B, b.audio, 2, b.analysis->bpm)) {
        return result;
    }
    engine.play(DeckId::A);
    engine.setMaster(DeckId::A);
    std::vector<float> block(kBlock * 2, 0.0f);
    engine.render(block.data(), kBlock);  // settle the load reset
    if (!engine.startTransition(plan)) {
        return result;
    }
    const std::uint64_t totalFrames = static_cast<std::uint64_t>(
        std::llround(plan.totalDuration() * kRate));
    constexpr double kTailSeconds = 0.25;
    const std::uint64_t blocks = static_cast<std::uint64_t>(
        std::ceil((plan.totalDuration() + kTailSeconds) * kRate / kBlock));
    if (captureAudio) {
        result.pcm.reserve(static_cast<std::size_t>(totalFrames) * 2);
    }
    std::uint64_t captured = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < blocks; ++i) {
        engine.render(block.data(), kBlock);
        const std::uint64_t take =
            captureAudio ? std::min<std::uint64_t>(kBlock, totalFrames - captured)
                         : 0;
        if (take > 0) {
            measure(block.data(), static_cast<std::size_t>(take),
                    result.metrics);
            result.pcm.insert(result.pcm.end(), block.begin(),
                              block.begin() + static_cast<std::ptrdiff_t>(take * 2));
            captured += take;
        }
    }
    result.wallSeconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    result.after = engine.snapshot();
    result.ok = result.after.phase == TransitionPhase::Idle;
    return result;
}

bool offlineRun(const Options& options, const PreparedMixTrack& a,
                const PreparedMixTrack& b) {
    // Plan from the same warmup position the host would use, but render the
    // planned window from the plan's own cue so pre-roll and crossfade are
    // measured exactly.
    MixEngine probe(kRate);
    if (!probe.loadDeck(DeckId::A, a.audio, 1, a.analysis->bpm) ||
        !probe.loadDeck(DeckId::B, b.audio, 2, b.analysis->bpm)) {
        std::fprintf(stderr, "failed to load prepared decks\n");
        return false;
    }
    probe.play(DeckId::A);
    probe.setMaster(DeckId::A);
    std::vector<float> block(kBlock * 2, 0.0f);
    const std::uint64_t warmupBlocks = static_cast<std::uint64_t>(
        std::ceil(options.warmupSeconds * kRate / kBlock));
    for (std::uint64_t i = 0; i < warmupBlocks; ++i) {
        probe.render(block.data(), kBlock);
    }

    const MixPlan plan = planTransition(options, a, b, probe);
    printPlan(plan);
    if (!plan.valid) {
        std::fprintf(stderr, "plan is not valid\n");
        return false;
    }

    const CaseResult planned = renderCase(a, b, plan, true);
    if (!planned.ok) {
        std::fprintf(stderr, "transition did not finish\n");
        return false;
    }
    const double audioSeconds = plan.totalDuration();
    std::printf(
        "  offline: %.2f s audio in %.3f s wall (%.1f%% of one core), "
        "peak %.3f, clipped %llu, nonfinite %llu\n",
        audioSeconds, planned.wallSeconds,
        planned.wallSeconds / std::max(1e-9, audioSeconds) * 100.0,
        static_cast<double>(planned.metrics.peak),
        static_cast<unsigned long long>(planned.metrics.clipped),
        static_cast<unsigned long long>(planned.metrics.nonFinite));
    std::printf(
        "  output: rms %.4f, longest zero run %.1f ms, limiter %.3f\n",
        rms(planned.metrics),
        static_cast<double>(planned.metrics.longestZeroRun) / kRate * 1000.0,
        static_cast<double>(planned.after.limiterGain));
    std::printf(
        "  transition: completed=%llu phase=%d, A -> %.3f, "
        "B -> %.3f (expected %.3f)\n",
        static_cast<unsigned long long>(
            planned.after.completedTransitions),
        static_cast<int>(planned.after.phase),
        planned.after.decks[0].positionSeconds,
        planned.after.decks[1].positionSeconds,
        plan.startB + plan.duration);

    // A/B artifact: the same window with no tempo ramp, no bass swap and a
    // plain equal-power fade. This is what a listener can compare in a DAW.
    MixPlan baseline = plan;
    baseline.type = TransitionType::Crossfade;
    baseline.requestedType = TransitionType::Crossfade;
    baseline.tempoCurve = {{0.0, 1.0, plan.localBpmA},
                           {std::max(0.001, plan.duration), 1.0,
                            plan.localBpmA}};
    baseline.tempoRampActive = false;
    baseline.tempoRatioAStart = 1.0;
    baseline.tempoRatioAEnd = 1.0;
    baseline.tempoRatioB = 1.0;
    baseline.tempoRampStartA = plan.transitionStartA -
                               plan.preTransitionSeconds;
    if (baseline.tempoRampStartA < 0.0) {
        baseline.tempoRampStartA = 0.0;
    }
    baseline.preTransitionSeconds = plan.preTransitionSeconds;
    baseline.eqCurveOut.clear();
    baseline.eqCurveIn.clear();
    baseline.bassSwap = false;
    const CaseResult base = renderCase(a, b, baseline, true);
    if (base.ok) {
        std::printf("  baseline: rms %.4f, peak %.3f, wall %.3f s\n",
                    rms(base.metrics),
                    static_cast<double>(base.metrics.peak),
                    base.wallSeconds);
    }

    if (!options.exportDir.empty()) {
        const std::filesystem::path dir = options.exportDir;
        const std::string name = options.name.empty() ? "mix" : options.name;
        if (writeWav(dir / (name + "_mix.wav"), planned.pcm) &&
            (!base.ok || writeWav(dir / (name + "_baseline.wav"), base.pcm))) {
            std::printf("  exported %s_mix.wav (+ baseline) to %s\n",
                        name.c_str(), dir.string().c_str());
        }
    }
    return true;
}

bool deviceRun(const Options& options, const PreparedMixTrack& a,
               const PreparedMixTrack& b) {
    MixEngine engine(kRate);
    engine.setTimeStretch(
        options.pitchLock
            ? std::shared_ptr<const bgmedia::mix::ITimeStretchProcessor>(
                  std::make_shared<bgmedia::mix::WsolaStretcher>())
            : std::shared_ptr<const bgmedia::mix::ITimeStretchProcessor>(
                  std::make_shared<bgmedia::mix::LinearResampleStretcher>()));
    const bool loadedA =
        engine.loadDeck(DeckId::A, a.audio, 1, a.analysis->bpm);
    const bool loadedB =
        engine.loadDeck(DeckId::B, b.audio, 2, b.analysis->bpm);
    if (!loadedA || !loadedB) {
        std::fprintf(stderr, "device: failed to load prepared decks\n");
        return false;
    }
    engine.play(DeckId::A);
    engine.setMaster(DeckId::A);

    MiniaudioMixDevice device(engine);
    std::string error;
    if (!device.open(kRate, &error)) {
        std::fprintf(stderr, "device: %s\n", error.c_str());
        return false;
    }
    std::printf("  device: %s\n", device.deviceName().c_str());

    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<int>(options.warmupSeconds *
                                                   1000.0)));
    const MixPlan plan = planTransition(options, a, b, engine);
    if (!plan.valid || !engine.startTransition(plan)) {
        std::fprintf(stderr, "device: engine rejected the plan\n");
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              static_cast<int>((plan.duration + 3.0) * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.completedTransitions() >= 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    std::printf(
        "  realtime: base latency %.1f ms, callback load %.2f%%, "
        "overruns %llu, peak %.3f, limiter %.3f, completed %llu\n",
        device.baseLatency() * 1000.0, device.callbackLoad() * 100.0,
        static_cast<unsigned long long>(device.overruns()),
        static_cast<double>(snap.outputPeak),
        static_cast<double>(snap.limiterGain),
        static_cast<unsigned long long>(snap.completedTransitions));
    device.close();
    return snap.completedTransitions >= 1;
}

std::shared_ptr<const bgmedia::mix::PcmTrack> stressPcm(double seconds,
                                                       float value) {
    auto track = std::make_shared<bgmedia::mix::PcmTrack>();
    track->sampleRate = kRate;
    track->channels = 2;
    const std::size_t frames =
        static_cast<std::size_t>(std::llround(seconds * kRate));
    track->samples.assign(frames * 2, value);
    return track;
}

MixPlan stressPlan(const MixEngine& engine) {
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    if (snap.phase != TransitionPhase::Idle || !snap.decks[0].loaded ||
        !snap.decks[1].loaded) {
        return {};
    }
    const DeckId from = snap.master;
    const DeckId to = bgmedia::mix::otherDeck(from);
    MixPlan plan;
    plan.valid = true;
    plan.type = TransitionType::Crossfade;
    plan.requestedType = TransitionType::Crossfade;
    plan.from = from;
    plan.to = to;
    plan.duration = 0.6;
    plan.transitionStartA =
        snap.decks[static_cast<int>(from)].positionSeconds;
    plan.transitionEndA = plan.transitionStartA + plan.duration;
    plan.startB = 0.0;
    plan.endB = plan.duration;
    plan.handoverProgress = 0.5;
    plan.gainCurveOut = {{0.0, 1.0f}, {plan.duration, 0.0f}};
    plan.gainCurveIn = {{0.0, 0.0f}, {plan.duration, 1.0f}};
    return plan;
}

// Concurrent stress: play/pause/seek/load/clear/next/cancel/stop while either a
// real output callback (--device) or an emulated render thread consumes the
// engine. Run the binary under ASan to turn the ownership protocol into a
// checkable memory-safety test; this mode is not a performance benchmark.
bool stressRun(const Options& options, const PreparedMixTrack& a,
               const PreparedMixTrack& b) {
    MixEngine engine(kRate);
    if (!engine.loadDeck(DeckId::A, a.audio, 1, a.analysis->bpm) ||
        !engine.loadDeck(DeckId::B, b.audio, 2, b.analysis->bpm)) {
        std::fprintf(stderr, "stress: failed to load prepared decks\n");
        return false;
    }
    engine.play(DeckId::A);
    engine.setMaster(DeckId::A);

    std::unique_ptr<MiniaudioMixDevice> device;
    bool realCallback = false;
    if (options.device) {
        device = std::make_unique<MiniaudioMixDevice>(engine);
        std::string error;
        if (device->open(kRate, &error)) {
            realCallback = true;
            std::printf("  stress device: %s\n", device->deviceName().c_str());
        } else {
            std::printf("  stress device unavailable (%s); using an emulated "
                        "render thread\n",
                        error.c_str());
            device.reset();
        }
    }

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> renders{0};
    std::atomic<std::uint64_t> nonFinite{0};
    std::atomic<float> peak{0.0f};
    std::thread audio;
    if (!realCallback) {
        audio = std::thread([&] {
            std::vector<float> buffer(kBlock * 2, 0.0f);
            while (!stop.load(std::memory_order_acquire)) {
                engine.render(buffer.data(), kBlock);
                float localPeak = 0.0f;
                for (float value : buffer) {
                    if (!std::isfinite(value)) {
                        nonFinite.fetch_add(1, std::memory_order_relaxed);
                    }
                    localPeak = std::max(localPeak, std::abs(value));
                }
                peak.store(std::max(peak.load(std::memory_order_relaxed),
                                    localPeak),
                           std::memory_order_relaxed);
                renders.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(
            static_cast<int>(std::max(0.1, options.stressSeconds) * 1000.0));
    std::uint64_t ops = 0;
    std::uint64_t loads = 0;
    std::uint64_t transitions = 0;
    std::uint64_t cancels = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        switch (ops % 10) {
        case 0:
            engine.setPaused(false);
            engine.play(DeckId::A);
            engine.setMaster(DeckId::A);
            break;
        case 1:
            engine.play(DeckId::B);
            engine.setMaster(DeckId::B);
            break;
        case 2:
            engine.pause(DeckId::A);
            engine.pause(DeckId::B);
            break;
        case 3:
            engine.seek(DeckId::A, static_cast<double>(ops % 160) * 0.05);
            break;
        case 4:
            engine.seek(DeckId::B, static_cast<double>(ops % 160) * 0.05);
            break;
        case 5:
        case 6: {
            const DeckId deck = (ops & 1) ? DeckId::A : DeckId::B;
            const bgmedia::mix::MixSnapshot snap = engine.snapshot();
            const int index = static_cast<int>(deck);
            if (!snap.decks[index].playing && !snap.paused &&
                snap.phase == TransitionPhase::Idle) {
                (void)engine.loadDeck(deck, stressPcm(0.25, 0.05f), 1000 + ops,
                                      120.0);
                ++loads;
            }
            break;
        }
        case 7: {
            const MixPlan plan = stressPlan(engine);
            if (plan.valid && engine.startTransition(plan)) {
                ++transitions;
            }
            break;
        }
        case 8:
            engine.cancelTransition();
            ++cancels;
            break;
        default:
            engine.stop();
            break;
        }
        ++ops;
        // Keep the control rate in the same order of magnitude as a human UI
        // (plus automation) instead of millions of no-op tickets; the audio
        // callback keeps running continuously in the meantime.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    stop.store(true, std::memory_order_release);
    if (audio.joinable()) {
        audio.join();
    }
    if (device) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    const bgmedia::mix::MixSnapshot snap = engine.snapshot();
    const std::uint64_t measuredNonFinite =
        realCallback ? device->nonFiniteSamples() : nonFinite.load();
    const double measuredPeak = realCallback ? device->outputPeak() : peak.load();

    std::printf(
        "  stress: callback=%s ops=%llu loads=%llu transitions=%llu "
        "cancels=%llu renders=%llu peak=%.3f nonfinite=%llu phase=%d\n",
        realCallback ? "real device" : "emulated thread",
        static_cast<unsigned long long>(ops),
        static_cast<unsigned long long>(loads),
        static_cast<unsigned long long>(transitions),
        static_cast<unsigned long long>(cancels),
        static_cast<unsigned long long>(renders.load()),
        measuredPeak,
        static_cast<unsigned long long>(measuredNonFinite),
        static_cast<int>(snap.phase));
    if (device) {
        std::printf(
            "  stress device: latency %.1f ms, callback load %.2f%%, "
            "overruns %llu, samples measured %llu\n",
            device->baseLatency() * 1000.0, device->callbackLoad() * 100.0,
            static_cast<unsigned long long>(device->overruns()),
            static_cast<unsigned long long>(device->renderedSamples()));
        device->close();
    }
    return measuredNonFinite == 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 1;
    }

    PreparedMixTrack a;
    PreparedMixTrack b;
    std::printf("BG Mix QA\n");
    if (!prepare(options, options.a, a, "A") ||
        !prepare(options, options.b, b, "B")) {
        return 1;
    }
    const std::u8string pathA = options.a.u8string();
    const std::u8string pathB = options.b.u8string();
    std::printf("  A: %s\n  B: %s\n",
                std::string(reinterpret_cast<const char*>(pathA.data()),
                            pathA.size())
                    .c_str(),
                std::string(reinterpret_cast<const char*>(pathB.data()),
                            pathB.size())
                    .c_str());

    if (options.stressSeconds > 0.0) {
        const bool ok = stressRun(options, a, b);
        return ok ? 0 : 1;
    }

    const bool offline = offlineRun(options, a, b);
    if (!offline) {
        return 1;
    }
    if (options.device) {
        return deviceRun(options, a, b) ? 0 : 1;
    }
    return 0;
}
