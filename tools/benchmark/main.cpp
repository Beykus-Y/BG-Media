#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/cache/AnalysisCache.hpp"
#include "support/PathUtf8.hpp"
#include "support/SyntheticSignals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using bgmedia::test::SyntheticTrack;

struct Metrics {
    std::string name;
    double expectedBpm = 0.0;
    double detectedBpm = 0.0;
    double confidence = 0.0;
    double precision = 0.0;
    double recall = 0.0;
    double meanErrorMs = 0.0;
    double analysisMs = 0.0;
    double realtimeFactor = 0.0;
    std::size_t beats = 0;
    bool tempoChange = false;
    bool ok = false;
};

double elapsedMs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

std::tuple<double, double, double> beatAccuracy(
    const std::vector<double>& truth, const std::vector<bgmedia::Beat>& detected,
    double tolerance, double fromTime, double toTime) {
    std::vector<double> expected;
    for (double time : truth) {
        if (time >= fromTime && time <= toTime) {
            expected.push_back(time);
        }
    }
    if (expected.empty() || detected.empty()) {
        return {0.0, 0.0, 0.0};
    }
    int truePositive = 0;
    double errorSum = 0.0;
    for (double time : expected) {
        double best = 1e9;
        for (const auto& beat : detected) {
            best = std::min(best, std::abs(beat.time - time));
        }
        if (best <= tolerance) {
            ++truePositive;
            errorSum += best;
        }
    }
    int detectedHits = 0;
    for (const auto& beat : detected) {
        if (beat.time < fromTime || beat.time > toTime) {
            continue;
        }
        bool hit = false;
        for (double time : expected) {
            if (std::abs(beat.time - time) <= tolerance) {
                hit = true;
                break;
            }
        }
        if (hit) {
            ++detectedHits;
        }
    }
    const double recall = static_cast<double>(truePositive) / expected.size();
    const double precision = static_cast<double>(detectedHits) / detected.size();
    const double meanError = truePositive > 0
                                 ? errorSum / static_cast<double>(truePositive) * 1000.0
                                 : 0.0;
    return {precision, recall, meanError};
}

Metrics runCase(const std::string& name, const SyntheticTrack& track,
                double expectedBpm, bgmedia::AnalysisConfig config = {}) {
    Metrics metrics;
    metrics.name = name;
    metrics.expectedBpm = expectedBpm;
    std::string error;
    const bgmedia::PcmBuffer pcm = bgmedia::PcmBuffer::fromAudio(track.audio, &error);
    if (!pcm.valid()) {
        std::cerr << "PCM failed for " << name << ": " << error << '\n';
        return metrics;
    }
    bgmedia::Analyzer analyzer(config);
    const auto start = Clock::now();
    bgmedia::AnalysisReport report = analyzer.analyze(pcm);
    metrics.analysisMs = elapsedMs(start, Clock::now());
    if (!report.ok()) {
        std::cerr << "analysis failed for " << name << ": " << report.message << '\n';
        return metrics;
    }
    const auto& result = report.result;
    metrics.ok = true;
    metrics.detectedBpm = result.bpm;
    metrics.confidence = result.bpmConfidence;
    metrics.beats = result.beats.size();
    metrics.realtimeFactor = pcm.duration() / std::max(0.001, result.analysisSeconds);
    metrics.tempoChange = result.hasTempoChange();
    const auto [precision, recall, meanError] =
        beatAccuracy(track.beatTimes, result.beats, 0.06, 1.0,
                     std::max(1.0, pcm.duration() - 1.0));
    metrics.precision = precision;
    metrics.recall = recall;
    metrics.meanErrorMs = meanError;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  " << std::left << std::setw(22) << name << std::right
              << std::setw(8) << result.bpm << " BPM  conf " << std::setw(4)
              << result.bpmConfidence << "  P/R " << std::setw(4) << precision << "/"
              << std::setw(4) << recall << "  err " << std::setw(5) << meanError
              << " ms  " << std::setw(7) << metrics.analysisMs << " ms  "
              << std::setw(6) << metrics.realtimeFactor << "x\n";
    return metrics;
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

void printUsage() {
    std::cout << "bgmedia_benchmark - repeatable quality and speed measurements\n\n"
              << "Usage: bgmedia_benchmark [--json <file>] [--long-seconds <seconds>]\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    bgmedia::test::Utf8CommandLine arguments(argc, argv);
    std::filesystem::path jsonPath;
    double longSeconds = 300.0;
    for (int i = 1; i < arguments.count; ++i) {
        const std::string arg = arguments.values[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--json" && i + 1 < argc) {
            jsonPath = bgmedia::test::pathFromUtf8(argv[++i]);
        } else if (arg == "--long-seconds" && i + 1 < argc) {
            longSeconds = std::clamp(std::stod(argv[++i]), 30.0, 1800.0);
        } else {
            std::cerr << "unknown option: " << arg << '\n';
            return 2;
        }
    }

    using namespace bgmedia;
    using namespace bgmedia::test;
    std::vector<Metrics> results;

    std::cout << "BG Media benchmark\n"
              << "Core library only (no GUI, no audio device)\n\n";
    std::cout << "Synthetic accuracy corpus:\n";

    TrackOptions opt92;
    opt92.bpm = 92.0;
    opt92.duration = 20.0;
    results.push_back(runCase("92 BPM", makeClickTrack(opt92), 92.0));

    TrackOptions opt123;
    opt123.bpm = 123.0;
    opt123.duration = 20.0;
    results.push_back(runCase("123 BPM", makeClickTrack(opt123), 123.0));

    TrackOptions opt163;
    opt163.bpm = 163.0;
    opt163.duration = 20.0;
    results.push_back(runCase("163 BPM", makeClickTrack(opt163), 163.0));

    results.push_back(runCase("tempo 92 -> 123", makeTempoChangeTrack(92.0, 123.0, 12.0, 24.0),
                              0.0));
    results.push_back(runCase("silence gap", makeSilenceTrack(), 92.0));
    results.push_back(runCase("bass accents", makeBassAccentTrack(), 92.0));
    results.push_back(runCase("ambiguous rhythm", makeAmbiguousTrack(), 0.0));

    // Long-track speed and cache benchmark.
    std::cout << "\nLong track (" << std::fixed << std::setprecision(0) << longSeconds
              << " s):\n";
    TrackOptions longOptions;
    longOptions.bpm = 118.0;
    longOptions.duration = longSeconds;
    const SyntheticTrack longTrack = makeClickTrack(longOptions);
    std::string pcmError;
    const PcmBuffer longPcm = PcmBuffer::fromAudio(longTrack.audio, &pcmError);
    if (!longPcm.valid()) {
        std::cerr << "long PCM failed: " << pcmError << '\n';
        return 2;
    }
    Analyzer analyzer;
    std::vector<double> runMs;
    AnalysisResult longResult;
    for (int run = 0; run < 3; ++run) {
        const auto start = Clock::now();
        AnalysisReport report = analyzer.analyze(longPcm);
        runMs.push_back(elapsedMs(start, Clock::now()));
        if (!report.ok()) {
            std::cerr << "long analysis failed: " << report.message << '\n';
            return 2;
        }
        longResult = std::move(report.result);
    }
    std::sort(runMs.begin(), runMs.end());
    const double medianMs = runMs[runMs.size() / 2];
    std::cout << "  " << longPcm.duration() << " s track: min " << runMs.front()
              << " ms, median " << medianMs << " ms, max " << runMs.back() << " ms, "
              << (longPcm.duration() / (medianMs / 1000.0)) << "x realtime\n";
    std::cout << "  detected " << longResult.bpm << " BPM, confidence "
              << longResult.bpmConfidence << ", " << longResult.beats.size()
              << " beats, " << longResult.spectrogram.frameCount
              << " spectrogram frames\n";

    const auto cacheDir =
        std::filesystem::temp_directory_path() / "bgmedia_benchmark_cache";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
    AnalysisCache cache(cacheDir);
    const auto storeStart = Clock::now();
    std::string cacheError;
    const bool stored = cache.store(longResult, longPcm, analyzer.config(), &cacheError);
    const double storeMs = elapsedMs(storeStart, Clock::now());
    const auto loadStart = Clock::now();
    CacheLoadResult loaded = cache.load(longPcm, analyzer.config());
    const double loadMs = elapsedMs(loadStart, Clock::now());
    std::cout << "  cache: write " << storeMs << " ms, read " << loadMs << " ms ("
              << (stored ? "stored" : cacheError) << ", " << (loaded.hit() ? "hit" : "miss")
              << ")\n";
    std::filesystem::remove_all(cacheDir, ec);

    if (!jsonPath.empty()) {
        std::ofstream out(jsonPath, std::ios::trunc);
        if (!out) {
            std::cerr << "cannot write " << jsonPath.string() << '\n';
            return 2;
        }
        out << std::fixed << std::setprecision(6) << "{\n  \"longTrackSeconds\": "
            << longSeconds << ",\n  \"longMedianMs\": " << medianMs
            << ",\n  \"longRealtimeFactor\": "
            << (longPcm.duration() / (medianMs / 1000.0))
            << ",\n  \"cacheWriteMs\": " << storeMs << ",\n  \"cacheReadMs\": " << loadMs
            << ",\n  \"cases\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const Metrics& m = results[i];
            out << "    {\"name\": \"" << jsonEscape(m.name) << "\", \"expectedBpm\": "
                << m.expectedBpm << ", \"detectedBpm\": " << m.detectedBpm
                << ", \"confidence\": " << m.confidence << ", \"precision\": "
                << m.precision << ", \"recall\": " << m.recall
                << ", \"meanBeatErrorMs\": " << m.meanErrorMs
                << ", \"analysisMs\": " << m.analysisMs
                << ", \"realtimeFactor\": " << m.realtimeFactor
                << ", \"beats\": " << m.beats << ", \"tempoChange\": "
                << (m.tempoChange ? "true" : "false") << "}";
            if (i + 1 < results.size()) {
                out << ',';
            }
            out << '\n';
        }
        out << "  ]\n}\n";
        std::cout << "\nJSON written to " << jsonPath.string() << '\n';
    }

    return 0;
}
