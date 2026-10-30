// BG Mix 0.2 listening-test renderer.
//
// This is deliberately not a simplified transition imitation: it prepares each
// track through the same BG Beat + decoder path as the desktop app, plans with
// the real MixPlanner, and renders with the real MixEngine (WSOLA, pre-roll,
// EQ/bass swap, limiter). The output is a WAV per ordered pair; the caller
// encodes the final MP3s with FFmpeg.

#include "bgmedia/audio/MiniaudioMixLoader.hpp"
#include "bgmedia/mix/MixEngine.hpp"
#include "bgmedia/mix/MixPlanner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bgmedia::AnalysisResult;
using bgmedia::MixTrackLoadOptions;
using bgmedia::mix::PreparedMixTrack;
using bgmedia::mix::DeckId;
using bgmedia::mix::MixEngine;
using bgmedia::mix::MixPlan;
using bgmedia::mix::MixPlanner;
using bgmedia::mix::PlanRequest;
using bgmedia::mix::PlannerConfig;
using bgmedia::mix::TransitionPhase;
using bgmedia::mix::TransitionType;

constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kBlock = 512;

struct TrackSpec {
    std::string name;
    std::filesystem::path path;
    PreparedMixTrack prepared;
};

struct Options {
    std::filesystem::path outDir = "artifacts/bg_mix_listening_test";
    std::filesystem::path cacheDir = "artifacts/bg_mix_listening_test/cache";
    std::vector<TrackSpec> tracks;
    std::vector<std::string> abPairs;
    int abTop = 0;
    double maxDelta = 10.0;
    double leadSeconds = 2.0;
    double tailSeconds = 2.0;
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

struct RenderResult {
    std::vector<float> pcm;
    Metrics metrics;
    bgmedia::mix::MixSnapshot lead;
    bgmedia::mix::MixSnapshot crossfade;
    bgmedia::mix::MixSnapshot fadeEnd;
    bgmedia::mix::MixSnapshot tail;
    bgmedia::mix::MixSnapshot after;
    std::string processor;
    bool pitchPreserving = false;
    double wallSeconds = 0.0;
    bool ok = false;
};

std::string utf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void printUsage() {
    std::printf(
        "usage: bgmedia_mix_listening --out <dir> --track <name>=<path> ...\n"
        "  --track <name>=<path>   add a source track (repeatable)\n"
        "  --cache <dir>           analysis cache directory\n"
        "  --no-cache              ignore the analysis cache\n"
        "  --max-delta <percent>   max tempo delta on each deck (default 10)\n"
        "  --lead <s>              source audio before the ramp (default 2)\n"
        "  --tail <s>              source audio after the fade (default 2)\n"
        "  --ab <nameA_to_nameB>   also write _adaptive/_baseline WAVs\n"
        "  --ab-top <n>            auto-pick the n most interesting pairs\n");
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
        if (arg == "--out") {
            const char* value = next("--out");
            if (!value) return false;
            options.outDir = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(value)));
        } else if (arg == "--cache") {
            const char* value = next("--cache");
            if (!value) return false;
            options.cacheDir = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(value)));
        } else if (arg == "--no-cache") {
            options.noCache = true;
        } else if (arg == "--max-delta") {
            const char* value = next("--max-delta");
            if (!value) return false;
            options.maxDelta = std::atof(value);
        } else if (arg == "--lead") {
            const char* value = next("--lead");
            if (!value) return false;
            options.leadSeconds = std::atof(value);
        } else if (arg == "--tail") {
            const char* value = next("--tail");
            if (!value) return false;
            options.tailSeconds = std::atof(value);
        } else if (arg == "--ab") {
            const char* value = next("--ab");
            if (!value) return false;
            options.abPairs.emplace_back(value);
        } else if (arg == "--ab-top") {
            const char* value = next("--ab-top");
            if (!value) return false;
            options.abTop = std::atoi(value);
        } else if (arg == "--track") {
            const char* value = next("--track");
            if (!value) return false;
            const std::string text(value);
            const std::size_t split = text.find('=');
            if (split == std::string::npos || split == 0 || split + 1 >= text.size()) {
                std::fprintf(stderr, "--track needs name=path\n");
                return false;
            }
            TrackSpec spec;
            spec.name = text.substr(0, split);
            spec.path = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(text.c_str() + split + 1)));
            options.tracks.push_back(std::move(spec));
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return false;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", arg.data());
            return false;
        }
    }
    if (options.tracks.size() < 2) {
        printUsage();
        return false;
    }
    return true;
}

bool prepareTrack(const Options& options, TrackSpec& track) {
    MixTrackLoadOptions load;
    load.cacheDirectory = options.cacheDir;
    load.useCache = !options.noCache;
    load.targetSampleRate = kRate;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = bgmedia::prepareMixTrack(
        track.path, load, track.prepared, error,
        [](float fraction, std::string_view stage) {
            if (!stage.empty()) {
                std::printf("  prepare %s: %.0f%%\n", stage.data(),
                            static_cast<double>(fraction) * 100.0);
            }
        });
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    if (!ok) {
        std::fprintf(stderr, "prepare failed for %s: %s\n", track.name.c_str(),
                     error.c_str());
        return false;
    }
    const AnalysisResult& analysis = *track.prepared.analysis;
    std::printf(
        "  %s: %.2f s, %.2f BPM (conf %.2f, level %.2f, phase %.2f, down %.2f), "
        "%d/4, sections %zu, beats %zu, prepared in %.0f ms\n",
        track.name.c_str(), analysis.info.duration, analysis.bpm,
        static_cast<double>(analysis.bpmConfidence),
        static_cast<double>(analysis.bpmLevelConfidence),
        static_cast<double>(analysis.beatPhaseConfidence),
        static_cast<double>(analysis.downbeatConfidence), analysis.beatsPerBar,
        analysis.sections.size(), analysis.beats.size(), ms);
    return true;
}

double sectionEnergyAt(const AnalysisResult& analysis, double time) {
    for (const auto& section : analysis.sections) {
        if (time >= section.start - 0.05 && time <= section.end + 0.05) {
            return static_cast<double>(section.energy);
        }
    }
    return 0.5;
}

double sectionBonusAt(const AnalysisResult& analysis, double time) {
    for (const auto& section : analysis.sections) {
        if (time >= section.start - 0.05 && time <= section.end + 0.05) {
            switch (section.kind) {
            case bgmedia::SectionKind::Main: return 0.20;
            case bgmedia::SectionKind::Build: return 0.15;
            case bgmedia::SectionKind::Outro: return 0.10;
            case bgmedia::SectionKind::Intro: return 0.00;
            case bgmedia::SectionKind::Breakdown: return -0.25;
            case bgmedia::SectionKind::Unknown: break;
            }
        }
    }
    return 0.0;
}

std::vector<double> transitionCandidates(const AnalysisResult& analysis) {
    std::vector<double> out;
    const double duration = analysis.info.duration;
    const double minTime = std::max(8.0, duration * 0.22);
    const double maxTime = duration * 0.90;
    for (const auto& beat : analysis.beats) {
        if (!beat.downbeat) {
            continue;
        }
        if (beat.time < minTime || beat.time > maxTime) {
            continue;
        }
        const int phrase = analysis.beatsPerBar >= 2 ? analysis.beatsPerBar : 4;
        const bool phraseBoundary = beat.barIndex % std::max(2, phrase) == 0;
        if (!phraseBoundary && analysis.beats.size() > 64) {
            continue;
        }
        if (!out.empty() && beat.time - out.back() < 0.20) {
            continue;
        }
        out.push_back(beat.time);
    }
    if (out.empty()) {
        for (const auto& section : analysis.sections) {
            if (section.start >= minTime && section.start <= maxTime) {
                out.push_back(section.start);
            }
        }
    }
    if (out.empty()) {
        out.push_back(duration * 0.55);
    }
    return out;
}

double meanIncomingRate(const MixPlan& plan) {
    const double handover = std::clamp(plan.handoverProgress, 0.0, 1.0);
    const double rate = std::clamp(plan.tempoRatioB, 0.25, 4.0);
    return rate * handover + 0.5 * (rate + 1.0) * (1.0 - handover);
}

struct ChosenPlan {
    MixPlan plan;
    double score = -std::numeric_limits<double>::max();
    double candidate = 0.0;
};

ChosenPlan choosePlan(const Options& options, const AnalysisResult& a,
                      const AnalysisResult& b) {
    PlannerConfig config;
    config.adaptive = true;
    config.allowTempoRamp = true;
    config.tempoRampMode = bgmedia::mix::TempoRampMode::Auto;
    config.tempoRampBars = 0.0;  // Auto
    config.tempoCurveKind = bgmedia::mix::TempoCurveKind::SCurve;
    config.maxTempoDeltaPercent = std::clamp(options.maxDelta, 1.0, 20.0);
    config.pitchLock = true;
    config.bassSwap = true;
    config.phraseAlignment = true;
    config.preferPhraseBoundaries = true;
    config.defaultCrossfadeSeconds = 8.0;
    config.minCrossfadeSeconds = 2.0;
    config.maxCrossfadeSeconds = 16.0;
    config.aggressiveness = bgmedia::mix::Aggressiveness::Balanced;
    config.phraseBars = 4;
    MixPlanner planner(config);

    ChosenPlan best;
    const std::vector<double> candidates = transitionCandidates(a);
    for (double candidate : candidates) {
        PlanRequest request;
        request.type = TransitionType::SmartCrossfade;
        request.requestedDuration = 0.0;
        request.currentPositionA = candidate;
        request.autoStart = true;
        MixPlan plan = planner.plan(a, b, request);
        if (!plan.valid) {
            continue;
        }
        const double overall = plan.overallStartA();
        if (overall < options.leadSeconds + 0.05) {
            continue;
        }
        if (plan.duration < 4.0) {
            continue;
        }
        const double tail = options.tailSeconds;
        if (plan.startB + meanIncomingRate(plan) * plan.duration + tail >
            b.info.duration + 1e-6) {
            continue;
        }
        if (plan.transitionEndA > a.info.duration + 1e-6) {
            continue;
        }
        double score = static_cast<double>(plan.confidence);
        if (plan.beatAligned) score += 0.20;
        if (plan.downbeatAligned) score += 0.20;
        if (plan.phraseBoundary) score += 0.15;
        if (plan.tempoRampActive) score += 0.15;
        if (plan.type == TransitionType::TempoRamp) score += 0.10;
        if (plan.type == TransitionType::PhraseAware) score += 0.08;
        if (plan.type == TransitionType::Crossfade ||
            plan.type == TransitionType::SmartCrossfade) {
            score -= 0.05;
        }
        score += 0.30 * sectionEnergyAt(a, plan.transitionStartA);
        score += sectionBonusAt(a, plan.transitionStartA);
        score -= 0.25 * std::abs(plan.tempoRatioB - 1.0);
        score -= 0.20 * std::abs(plan.transitionStartA / a.info.duration - 0.65);
        if (score > best.score) {
            best.plan = plan;
            best.score = score;
            best.candidate = candidate;
        }
    }
    return best;
}

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

RenderResult renderPlan(const PreparedMixTrack& a, const PreparedMixTrack& b,
                        const MixPlan& plan, double leadSeconds,
                        double tailSeconds) {
    RenderResult result;
    if (!plan.valid) {
        return result;
    }
    MixEngine engine(kRate);
    engine.setTimeStretch(std::shared_ptr<const bgmedia::mix::ITimeStretchProcessor>(
        std::make_shared<bgmedia::mix::WsolaStretcher>()));
    result.processor = engine.timeStretch().name();
    result.pitchPreserving = engine.timeStretch().pitchPreserving();
    if (!engine.loadDeck(DeckId::A, a.audio, 1, a.analysis->bpm) ||
        !engine.loadDeck(DeckId::B, b.audio, 2, b.analysis->bpm)) {
        std::fprintf(stderr, "render: failed to load decks\n");
        return result;
    }
    const double leadStart = plan.overallStartA() - leadSeconds;
    if (leadStart < -1e-6) {
        std::fprintf(stderr, "render: lead start is negative\n");
        return result;
    }
    engine.seek(DeckId::A, std::max(0.0, leadStart));
    engine.play(DeckId::A);
    engine.setMaster(DeckId::A);

    std::vector<float> block(kBlock * 2, 0.0f);
    const std::uint64_t leadFrames =
        static_cast<std::uint64_t>(std::llround(leadSeconds * kRate));
    const std::uint64_t transitionFrames = static_cast<std::uint64_t>(
        std::llround(plan.totalDuration() * kRate));
    const std::uint64_t tailFrames =
        static_cast<std::uint64_t>(std::llround(tailSeconds * kRate));
    result.pcm.reserve(static_cast<std::size_t>(
        (leadFrames + transitionFrames + tailFrames) * 2));

    auto renderFrames = [&](std::uint64_t frames) {
        std::uint64_t done = 0;
        while (done < frames) {
            const std::size_t chunk =
                static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, frames - done));
            engine.render(block.data(), chunk);
            measure(block.data(), chunk, result.metrics);
            result.pcm.insert(result.pcm.end(), block.begin(),
                              block.begin() + static_cast<std::ptrdiff_t>(chunk * 2));
            done += chunk;
        }
    };

    const auto wallStart = std::chrono::steady_clock::now();
    renderFrames(leadFrames);
    result.lead = engine.snapshot();
    if (!engine.startTransition(plan)) {
        std::fprintf(stderr, "render: engine rejected the plan\n");
        return result;
    }
    const std::uint64_t preFrames = std::min<std::uint64_t>(
        transitionFrames,
        static_cast<std::uint64_t>(
            std::llround(plan.preTransitionSeconds * kRate)));
    const std::uint64_t fadeFrames = transitionFrames - preFrames;
    renderFrames(preFrames);
    if (fadeFrames > 0) {
        // One frame past the boundary activates the incoming deck; the
        // snapshot then shows the real B rate/position at the crossfade start.
        renderFrames(1);
    }
    result.crossfade = engine.snapshot();
    if (fadeFrames > 0) {
        renderFrames(fadeFrames - 1);
    }
    result.fadeEnd = engine.snapshot();
    result.after = result.fadeEnd;
    if (result.after.phase != TransitionPhase::Idle) {
        // Give the block boundary a little room, but do not add it to the file.
        std::uint64_t guard = 0;
        while (result.after.phase != TransitionPhase::Idle && guard < 32) {
            engine.render(block.data(), static_cast<std::size_t>(kBlock));
            result.after = engine.snapshot();
            ++guard;
        }
        if (result.after.phase != TransitionPhase::Idle) {
            std::fprintf(stderr, "render: transition did not finish\n");
            return result;
        }
    }
    renderFrames(tailFrames);
    result.tail = engine.snapshot();
    result.wallSeconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - wallStart)
                             .count();
    result.ok = true;
    return result;
}

bool writeWav(const std::filesystem::path& path,
              const std::vector<float>& interleaved) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot write %s\n", utf8(path).c_str());
        return false;
    }
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(interleaved.size() * sizeof(std::int16_t));
    const std::uint32_t channels = 2;
    const std::uint32_t rate = kRate;
    const std::uint32_t byteRate = rate * channels * 2;
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * 2);
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

MixPlan baselinePlan(const MixPlan& plan, double bpmA, double bpmB) {
    MixPlan baseline = plan;
    baseline.type = TransitionType::Crossfade;
    baseline.requestedType = TransitionType::Crossfade;
    baseline.tempoRampActive = false;
    baseline.tempoCurve = {{0.0, 1.0, bpmA},
                           {std::max(0.001, plan.duration), 1.0, bpmA}};
    baseline.tempoRatioAStart = 1.0;
    baseline.tempoRatioAEnd = 1.0;
    baseline.tempoRatioB = 1.0;
    baseline.targetBpm = bpmB;
    // Match the crossfade source cues exactly: A at transitionStartA and B at
    // startB. The baseline deliberately has no pre-roll, so its file is shorter
    // by the adaptive ramp; align the two files at the crossfade for A/B.
    baseline.tempoRampStartA = plan.transitionStartA;
    baseline.preTransitionSeconds = 0.0;
    baseline.eqCurveOut.clear();
    baseline.eqCurveIn.clear();
    baseline.bassSwap = false;
    baseline.pitchLock = true;
    return baseline;
}

std::string joinReasons(const MixPlan& plan) {
    std::string text;
    for (const std::string& reason : plan.reasons) {
        if (!text.empty()) {
            text += " | ";
        }
        text += reason;
    }
    if (!plan.fallbackReason.empty()) {
        if (!text.empty()) {
            text += " | ";
        }
        text += "FALLBACK: " + plan.fallbackReason;
    }
    return text;
}

double interestScore(const MixPlan& plan) {
    double score = 0.0;
    if (plan.tempoRampActive) score += 2.0;
    if (plan.phraseBoundary) score += 1.0;
    score += 10.0 * std::abs(plan.tempoRatioAEnd - 1.0);
    score += 5.0 * std::abs(plan.tempoRatioB - 1.0);
    if (!plan.fallbackReason.empty()) score += 1.0;
    return score;
}

std::string pairName(const TrackSpec& from, const TrackSpec& to) {
    return from.name + "_to_" + to.name;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(options.outDir, ec);

    std::printf("BG Mix 0.2 listening test\n");
    for (TrackSpec& track : options.tracks) {
        if (!prepareTrack(options, track)) {
            return 1;
        }
    }

    struct Entry {
        const TrackSpec* from = nullptr;
        const TrackSpec* to = nullptr;
        MixPlan plan;
        RenderResult render;
        double interest = 0.0;
        std::string file;
    };
    std::vector<Entry> entries;
    entries.reserve(options.tracks.size() * (options.tracks.size() - 1));

    int index = 0;
    for (const TrackSpec& from : options.tracks) {
        for (const TrackSpec& to : options.tracks) {
            if (&from == &to) {
                continue;
            }
            ++index;
            std::printf("\n[%02d] %s -> %s\n", index, from.name.c_str(),
                        to.name.c_str());
            const ChosenPlan chosen =
                choosePlan(options, *from.prepared.analysis,
                           *to.prepared.analysis);
            if (!chosen.plan.valid) {
                std::fprintf(stderr, "  no valid plan for %s -> %s\n",
                             from.name.c_str(), to.name.c_str());
                continue;
            }
            const MixPlan& plan = chosen.plan;
            std::printf(
                "  A %.2f BPM -> B %.2f BPM, target %.2f, %s\n",
                plan.bpmA, plan.bpmB, plan.targetBpm,
                bgmedia::mix::transitionTypeName(plan.type));
            std::printf(
                "  pre %.2f s + fade %.2f s = %.2f s, A cue %.2f -> %.2f, "
                "B cue %.2f, ramp %.2f%%, ratioB %.3f\n",
                plan.preTransitionSeconds, plan.duration,
                plan.totalDuration(), plan.transitionStartA,
                plan.transitionEndA, plan.startB, plan.tempoRampPercent,
                plan.tempoRatioB);
            std::printf(
                "  beat=%d down=%d snap=%.1f ms phase=%.1f ms phrase=%d "
                "(%.2f), bass=%d lock=%d\n",
                plan.beatAligned ? 1 : 0, plan.downbeatAligned ? 1 : 0,
                plan.alignmentErrorMs, plan.beatPhaseErrorMs,
                plan.phraseBoundary ? 1 : 0,
                static_cast<double>(plan.phraseConfidence),
                plan.bassSwap ? 1 : 0, plan.pitchLock ? 1 : 0);
            if (!plan.fallbackReason.empty()) {
                std::printf("  fallback: %s\n", plan.fallbackReason.c_str());
            }
            for (const std::string& reason : plan.reasons) {
                std::printf("    - %s\n", reason.c_str());
            }
            Entry entry;
            entry.from = &from;
            entry.to = &to;
            entry.plan = plan;
            entry.interest = interestScore(plan);
            entry.render = renderPlan(from.prepared, to.prepared, plan,
                                      options.leadSeconds, options.tailSeconds);
            if (!entry.render.ok) {
                std::fprintf(stderr, "  render failed\n");
                continue;
            }
            char filename[256];
            std::snprintf(filename, sizeof(filename), "%02d_%s.wav", index,
                          pairName(from, to).c_str());
            entry.file = filename;
            const std::filesystem::path path = options.outDir / entry.file;
            if (!writeWav(path, entry.render.pcm)) {
                continue;
            }
            const Metrics& metrics = entry.render.metrics;
            const double seconds =
                static_cast<double>(metrics.frames) / kRate;
            std::printf(
                "  rendered %.3f s in %.3f s wall, peak %.3f, clipped %llu, "
                "nonfinite %llu, zero-run %.1f ms\n",
                seconds, entry.render.wallSeconds,
                static_cast<double>(metrics.peak),
                static_cast<unsigned long long>(metrics.clipped),
                static_cast<unsigned long long>(metrics.nonFinite),
                static_cast<double>(metrics.longestZeroRun) / kRate * 1000.0);
            const int fromIndex = bgmedia::mix::deckIndex(DeckId::A);
            const int toIndex = bgmedia::mix::deckIndex(DeckId::B);
            std::printf("  verify: processor=%s pitchPreserving=%d\n",
                        entry.render.processor.c_str(),
                        entry.render.pitchPreserving ? 1 : 0);
            if (plan.preTransitionSeconds > 0.0) {
                const auto& cf = entry.render.crossfade;
                const auto& a = cf.decks[fromIndex];
                const auto& b = cf.decks[toIndex];
                std::printf(
                    "  verify crossfade: A %.3f s (plan %.3f) rate %.4f "
                    "(plan %.4f) BPM %.2f | B %.3f s (plan %.3f) rate %.4f "
                    "(plan %.4f) BPM %.2f | stage %d\n",
                    a.positionSeconds, plan.transitionStartA, a.rate,
                    plan.tempoRatioAEnd, a.bpm, b.positionSeconds, plan.startB,
                    b.rate, plan.tempoRatioB, b.bpm,
                    static_cast<int>(cf.stage));
            }
            const auto& fade = entry.render.fadeEnd;
            const auto& bEnd = fade.decks[toIndex];
            const double expectedB =
                plan.startB + meanIncomingRate(plan) * plan.duration;
            std::printf(
                "  verify fade end: B %.3f s (expected %.3f) rate %.4f BPM "
                "%.2f, stage %d, phase %d\n",
                bEnd.positionSeconds, expectedB, bEnd.rate, bEnd.bpm,
                static_cast<int>(fade.stage), static_cast<int>(fade.phase));
            std::printf("  wrote %s\n", utf8(path).c_str());
            entries.push_back(std::move(entry));
        }
    }

    // A/B pairs: explicit names, or the top-N by objective interest.
    std::vector<std::string> ab = options.abPairs;
    if (options.abTop > 0) {
        std::vector<std::size_t> order(entries.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return entries[a].interest > entries[b].interest;
        });
        for (int i = 0; i < options.abTop && i < static_cast<int>(order.size());
             ++i) {
            const Entry& entry = entries[order[static_cast<std::size_t>(i)]];
            ab.push_back(pairName(*entry.from, *entry.to));
        }
    }

    for (const std::string& pair : ab) {
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [&](const Entry& entry) {
                                         return pairName(*entry.from, *entry.to) == pair;
                                     });
        if (it == entries.end()) {
            std::fprintf(stderr, "A/B pair not found: %s\n", pair.c_str());
            continue;
        }
        const Entry& entry = *it;
        const MixPlan baseline =
            baselinePlan(entry.plan, entry.from->prepared.analysis->bpm,
                         entry.to->prepared.analysis->bpm);
        const RenderResult baseRender =
            renderPlan(entry.from->prepared, entry.to->prepared, baseline,
                       options.leadSeconds, options.tailSeconds);
        if (!baseRender.ok) {
            std::fprintf(stderr, "baseline render failed for %s\n", pair.c_str());
            continue;
        }
        const std::filesystem::path adaptivePath =
            options.outDir / (pair + "_adaptive.wav");
        const std::filesystem::path baselinePath =
            options.outDir / (pair + "_baseline.wav");
        if (writeWav(adaptivePath, entry.render.pcm) &&
            writeWav(baselinePath, baseRender.pcm)) {
            std::printf(
                "  A/B %s: adaptive peak %.3f, baseline peak %.3f, "
                "baseline %.3f s\n",
                pair.c_str(), static_cast<double>(entry.render.metrics.peak),
                static_cast<double>(baseRender.metrics.peak),
                static_cast<double>(baseRender.metrics.frames) / kRate);
        }
    }

    // Manifest for the README / objective ranking.
    const std::filesystem::path manifest = options.outDir / "manifest.csv";
    std::ofstream csv(manifest);
    csv << "index,from,to,from_bpm,to_bpm,target_bpm,type,pre_s,fade_s,total_s,"
           "a_cue_s,b_cue_s,ratio_a,ratio_b,pitch_lock,bass_swap,phrase,"
           "beat_aligned,downbeat,confidence,peak,clipped,nonfinite,file,fallback\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        const MixPlan& plan = entry.plan;
        csv << (i + 1) << ',' << entry.from->name << ',' << entry.to->name << ','
            << plan.bpmA << ',' << plan.bpmB << ',' << plan.targetBpm << ','
            << bgmedia::mix::transitionTypeName(plan.type) << ','
            << plan.preTransitionSeconds << ',' << plan.duration << ','
            << plan.totalDuration() << ',' << plan.transitionStartA << ','
            << plan.startB << ',' << plan.tempoRatioAEnd << ','
            << plan.tempoRatioB << ',' << (plan.pitchLock ? 1 : 0) << ','
            << (plan.bassSwap ? 1 : 0) << ',' << (plan.phraseBoundary ? 1 : 0)
            << ',' << (plan.beatAligned ? 1 : 0) << ','
            << (plan.downbeatAligned ? 1 : 0) << ',' << plan.confidence << ','
            << entry.render.metrics.peak << ',' << entry.render.metrics.clipped
            << ',' << entry.render.metrics.nonFinite << ',' << entry.file << ','
            << plan.fallbackReason << '\n';
    }
    std::printf("\nmanifest: %s\n", utf8(manifest).c_str());
    std::printf("done: %zu transitions\n", entries.size());
    return entries.empty() ? 1 : 0;
}
