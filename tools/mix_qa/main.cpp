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
    double warmupSeconds = 8.0;
    double tailSeconds = 1.5;
    double stressSeconds = 0.0;
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
        "  --type smart|beat|tempo|cut   transition type (default smart)\n"
        "  --duration <s>                manual crossfade length (0 = auto)\n"
        "  --ramp <percent>              Tempo Ramp max delta (default 6)\n"
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
    config.allowTempoRamp = options.type == TransitionType::TempoRamp;
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
    if (!plan.tempoCurve.empty()) {
        std::printf("    tempo curve: %zu points, %.4f -> %.4f\n",
                    plan.tempoCurve.size(), plan.tempoCurve.front().rate,
                    plan.tempoCurve.back().rate);
    }
}

bool offlineRun(const Options& options, const PreparedMixTrack& a,
                const PreparedMixTrack& b) {
    MixEngine engine(kRate);
    const bool loadedA =
        engine.loadDeck(DeckId::A, a.audio, 1, a.analysis->bpm);
    const bool loadedB =
        engine.loadDeck(DeckId::B, b.audio, 2, b.analysis->bpm);
    if (!loadedA || !loadedB) {
        std::fprintf(stderr, "failed to load prepared decks\n");
        return false;
    }
    engine.play(DeckId::A);
    engine.setMaster(DeckId::A);

    std::vector<float> block(kBlock * 2, 0.0f);
    Metrics warmup;
    const auto wallStart = std::chrono::steady_clock::now();
    const std::uint64_t warmupBlocks = static_cast<std::uint64_t>(
        std::ceil(options.warmupSeconds * kRate / kBlock));
    for (std::uint64_t i = 0; i < warmupBlocks; ++i) {
        engine.render(block.data(), kBlock);
        measure(block.data(), kBlock, warmup);
    }

    const bgmedia::mix::MixSnapshot before = engine.snapshot();
    const MixPlan plan = planTransition(options, a, b, engine);
    printPlan(plan);
    if (!plan.valid) {
        std::fprintf(stderr, "plan is not valid\n");
        return false;
    }
    if (!engine.startTransition(plan)) {
        std::fprintf(stderr, "engine rejected the plan\n");
        return false;
    }

    Metrics mix;
    const std::uint64_t mixBlocks = static_cast<std::uint64_t>(
        std::ceil(plan.duration * kRate / kBlock));
    for (std::uint64_t i = 0; i < mixBlocks; ++i) {
        engine.render(block.data(), kBlock);
        measure(block.data(), kBlock, mix);
    }
    const bgmedia::mix::MixSnapshot after = engine.snapshot();
    // The tail is deliberately not measured: after a handover the incoming
    // track may legitimately end, and that is content silence, not a gap.
    const std::uint64_t tailBlocks = static_cast<std::uint64_t>(
        std::ceil(options.tailSeconds * kRate / kBlock));
    for (std::uint64_t i = 0; i < tailBlocks; ++i) {
        engine.render(block.data(), kBlock);
    }
    const double wallSeconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wallStart)
            .count();
    const double audioSeconds =
        static_cast<double>(warmup.frames + (mixBlocks + tailBlocks) * kBlock) /
        kRate;
    const bgmedia::mix::MixSnapshot tail = engine.snapshot();

    std::printf(
        "  offline: %.2f s audio in %.3f s wall (%.1f%% of one core), "
        "peak %.3f, clipped %llu, nonfinite %llu\n",
        audioSeconds, wallSeconds, wallSeconds / audioSeconds * 100.0,
        static_cast<double>(mix.peak),
        static_cast<unsigned long long>(mix.clipped),
        static_cast<unsigned long long>(mix.nonFinite));
    std::printf(
        "  output: rms %.4f, longest zero run %.1f ms, limiter %.3f\n",
        rms(mix),
        static_cast<double>(mix.longestZeroRun) / kRate * 1000.0,
        static_cast<double>(after.limiterGain));
    std::printf(
        "  transition: completed=%llu phase=%d, A %.3f -> %.3f, "
        "B start %.3f -> %.3f (expected %.3f)\n",
        static_cast<unsigned long long>(after.completedTransitions),
        static_cast<int>(after.phase), before.decks[0].positionSeconds,
        after.decks[0].positionSeconds, plan.startB,
        after.decks[1].positionSeconds, plan.startB + plan.duration);
    if (after.phase != TransitionPhase::Idle) {
        std::fprintf(stderr, "transition did not finish\n");
        return false;
    }
    std::printf("  tail: master=%c, B ended=%d, B position %.3f\n",
                tail.master == DeckId::A ? 'A' : 'B',
                tail.decks[1].ended ? 1 : 0, tail.decks[1].positionSeconds);
    return true;
}

bool deviceRun(const Options& options, const PreparedMixTrack& a,
               const PreparedMixTrack& b) {
    MixEngine engine(kRate);
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
