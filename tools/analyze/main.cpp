#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/audio/MiniaudioDecoder.hpp"
#include "bgmedia/cache/AnalysisCache.hpp"
#include "support/PathUtf8.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::filesystem::path input;
    std::filesystem::path cacheDirectory = ".bgmedia_cache";
    std::filesystem::path jsonPath;
    bool noCache = false;
    bool noTempoChanges = false;
    int repeat = 1;
    double minBpm = 55.0;
    double maxBpm = 200.0;
};

void printUsage() {
    std::cout
        << "bgmedia_analyze - headless BG Media analysis and benchmark\n\n"
        << "Usage: bgmedia_analyze <file.wav|file.mp3|file.flac> [options]\n\n"
        << "Options:\n"
        << "  --cache-dir <dir>     cache directory (default .bgmedia_cache)\n"
        << "  --no-cache            do not read or write the analysis cache\n"
        << "  --repeat <n>          run the analyzer n times, report timings\n"
        << "  --json <file|->       write machine-readable results\n"
        << "  --min-bpm <value>     tempo search lower bound (default 55)\n"
        << "  --max-bpm <value>     tempo search upper bound (default 200)\n"
        << "  --no-tempo-change     disable local tempo-change detection\n"
        << "  --help                show this text\n";
}

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::ostringstream escaped;
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                out += escaped.str();
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

const char* cacheStatusName(bgmedia::CacheStatus status) {
    switch (status) {
    case bgmedia::CacheStatus::Hit:
        return "hit";
    case bgmedia::CacheStatus::Miss:
        return "miss";
    case bgmedia::CacheStatus::Corrupt:
        return "corrupt";
    case bgmedia::CacheStatus::Error:
        return "error";
    }
    return "unknown";
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t mid = values.size() / 2;
    if (values.size() % 2 == 0) {
        return 0.5 * (values[mid - 1] + values[mid]);
    }
    return values[mid];
}

bool parseDouble(const std::string& text, double& value) {
    try {
        std::size_t consumed = 0;
        value = std::stod(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return false;
        }
        if (arg == "--no-cache") {
            options.noCache = true;
        } else if (arg == "--no-tempo-change") {
            options.noTempoChanges = true;
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            options.cacheDirectory = bgmedia::test::pathFromUtf8(argv[++i]);
        } else if (arg == "--json" && i + 1 < argc) {
            options.jsonPath = bgmedia::test::pathFromUtf8(argv[++i]);
        } else if (arg == "--repeat" && i + 1 < argc) {
            options.repeat = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--min-bpm" && i + 1 < argc) {
            if (!parseDouble(argv[++i], options.minBpm)) {
                std::cerr << "invalid --min-bpm value\n";
                return false;
            }
        } else if (arg == "--max-bpm" && i + 1 < argc) {
            if (!parseDouble(argv[++i], options.maxBpm)) {
                std::cerr << "invalid --max-bpm value\n";
                return false;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << '\n';
            return false;
        } else if (options.input.empty()) {
            options.input = bgmedia::test::pathFromUtf8(arg);
        } else {
            std::cerr << "unexpected extra argument: " << arg << '\n';
            return false;
        }
    }
    if (options.input.empty()) {
        std::cerr << "missing input file\n\n";
        printUsage();
        return false;
    }
    return true;
}

void writeJson(std::ostream& out, const Options& options,
               const bgmedia::AnalysisResult& result,
               const bgmedia::PcmBuffer& pcm, double decodeMs, double cacheMs,
               const std::string& cacheStatus, const std::vector<double>& runsMs) {
    out << std::fixed << std::setprecision(6);
    out << "{\n";
    out << "  \"file\": \"" << jsonEscape(options.input.string()) << "\",\n";
    out << "  \"durationSeconds\": " << pcm.duration() << ",\n";
    out << "  \"sampleRate\": " << pcm.sampleRate << ",\n";
    out << "  \"channels\": " << pcm.sourceChannels << ",\n";
    out << "  \"contentHash\": \"" << std::hex << pcm.contentHash << std::dec << "\",\n";
    out << "  \"decodeMs\": " << decodeMs << ",\n";
    out << "  \"cache\": {\"status\": \"" << cacheStatus << "\", \"loadMs\": "
        << cacheMs << "},\n";
    out << "  \"analyzeMs\": " << result.analysisSeconds * 1000.0 << ",\n";
    out << "  \"realtimeFactor\": "
        << (result.analysisSeconds > 1e-9
                ? pcm.duration() / result.analysisSeconds
                : 0.0)
        << ",\n";
    out << "  \"bpm\": " << result.bpm << ",\n";
    out << "  \"bpmConfidence\": " << result.bpmConfidence << ",\n";
    out << "  \"tempoStable\": " << (result.tempoStable ? "true" : "false") << ",\n";
    out << "  \"octaveAmbiguous\": " << (result.octaveAmbiguous ? "true" : "false")
        << ",\n";
    out << "  \"beatsPerBar\": " << result.beatsPerBar << ",\n";
    out << "  \"meterConfidence\": " << result.meterConfidence << ",\n";
    out << "  \"activeRatio\": " << result.activeRatio << ",\n";
    out << "  \"counts\": {\"beats\": " << result.beats.size()
        << ", \"onsets\": " << result.onsets.size()
        << ", \"bassAccents\": " << result.bassAccents.size()
        << ", \"silences\": " << result.silences.size()
        << ", \"tempoSegments\": " << result.tempoSegments.size()
        << ", \"tempoTransitions\": " << result.tempoTransitions.size() << "},\n";

    out << "  \"bpmCandidates\": [";
    for (std::size_t i = 0; i < result.bpmCandidates.size(); ++i) {
        if (i) {
            out << ", ";
        }
        out << "{\"bpm\": " << result.bpmCandidates[i] << ", \"score\": "
            << (i < result.bpmCandidateScores.size() ? result.bpmCandidateScores[i] : 0.0f)
            << "}";
    }
    out << "],\n";

    out << "  \"tempoSegments\": [";
    for (std::size_t i = 0; i < result.tempoSegments.size(); ++i) {
        if (i) {
            out << ", ";
        }
        const auto& s = result.tempoSegments[i];
        out << "{\"start\": " << s.start << ", \"end\": " << s.end
            << ", \"bpm\": " << s.bpm << ", \"confidence\": " << s.confidence << "}";
    }
    out << "],\n";

    out << "  \"tempoTransitions\": [";
    for (std::size_t i = 0; i < result.tempoTransitions.size(); ++i) {
        if (i) {
            out << ", ";
        }
        const auto& t = result.tempoTransitions[i];
        out << "{\"start\": " << t.start << ", \"end\": " << t.end
            << ", \"fromBpm\": " << t.fromBpm << ", \"toBpm\": " << t.toBpm
            << ", \"confidence\": " << t.confidence << "}";
    }
    out << "],\n";

    out << "  \"silences\": [";
    for (std::size_t i = 0; i < result.silences.size(); ++i) {
        if (i) {
            out << ", ";
        }
        out << "{\"start\": " << result.silences[i].start
            << ", \"end\": " << result.silences[i].end << "}";
    }
    out << "],\n";

    out << "  \"bassAccents\": [";
    const std::size_t accentCount = std::min<std::size_t>(32, result.bassAccents.size());
    for (std::size_t i = 0; i < accentCount; ++i) {
        if (i) {
            out << ", ";
        }
        const auto& accent = result.bassAccents[i];
        out << "{\"time\": " << accent.time
            << ", \"strength\": " << accent.strength
            << ", \"lowBandRatio\": " << accent.lowBandRatio
            << ", \"confidence\": " << accent.confidence
            << ", \"nearestBeat\": " << accent.nearestBeat
            << ", \"offsetFromBeat\": " << accent.offsetFromBeat << "}";
    }
    out << "],\n";

    out << "  \"firstBeats\": [";
    const std::size_t beatCount = std::min<std::size_t>(16, result.beats.size());
    for (std::size_t i = 0; i < beatCount; ++i) {
        if (i) {
            out << ", ";
        }
        out << "{\"time\": " << result.beats[i].time
            << ", \"downbeat\": " << (result.beats[i].downbeat ? "true" : "false")
            << ", \"strength\": " << result.beats[i].strength
            << ", \"confidence\": " << result.beats[i].confidence << "}";
    }
    out << "],\n";

    out << "  \"benchmark\": {\"runs\": " << runsMs.size() << ", \"minMs\": ";
    if (runsMs.empty()) {
        out << 0;
    } else {
        out << *std::min_element(runsMs.begin(), runsMs.end());
    }
    out << ", \"medianMs\": " << median(runsMs) << ", \"maxMs\": ";
    if (runsMs.empty()) {
        out << 0;
    } else {
        out << *std::max_element(runsMs.begin(), runsMs.end());
    }
    out << "}\n";
    out << "}\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    bgmedia::test::Utf8CommandLine arguments(argc, argv);
    Options options;
    if (!parseOptions(arguments.count, arguments.values, options)) {
        return 2;
    }

    bgmedia::MiniaudioDecoder decoder;
    if (!decoder.supports(options.input)) {
        std::cerr << "unsupported file type (expected .wav, .mp3 or .flac): "
                  << options.input.string() << '\n';
        return 2;
    }

    auto timeNow = [] { return Clock::now(); };
    auto elapsedMs = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    const auto decodeStart = timeNow();
    const bgmedia::DecodeResult decoded = decoder.decode(options.input);
    const double decodeMs = elapsedMs(decodeStart, timeNow());
    if (!decoded.ok) {
        std::cerr << "decode failed: " << decoded.error << '\n';
        return 2;
    }

    std::string pcmError;
    const bgmedia::PcmBuffer pcm = bgmedia::PcmBuffer::fromAudio(decoded.audio, &pcmError);
    if (!pcm.valid()) {
        std::cerr << "cannot prepare PCM: " << pcmError << '\n';
        return 2;
    }

    bgmedia::AnalysisConfig config;
    config.minBpm = options.minBpm;
    config.maxBpm = options.maxBpm;
    config.detectTempoChanges = !options.noTempoChanges;
    bgmedia::Analyzer analyzer(config);

    bgmedia::CacheLoadResult cached;
    double cacheMs = 0.0;
    if (!options.noCache) {
        const auto cacheStart = timeNow();
        cached = bgmedia::AnalysisCache(options.cacheDirectory).load(pcm, analyzer.config());
        cacheMs = elapsedMs(cacheStart, timeNow());
    }

    bgmedia::AnalysisResult result;
    bool haveResult = false;
    if (cached.hit()) {
        result = std::move(cached.result);
        haveResult = true;
    }

    std::vector<double> runsMs;
    if (!haveResult || options.repeat > 1) {
        const int runs = cached.hit() ? options.repeat : std::max(1, options.repeat);
        for (int run = 0; run < runs; ++run) {
            const auto start = timeNow();
            bgmedia::AnalysisReport report = analyzer.analyze(pcm);
            const double ms = elapsedMs(start, timeNow());
            if (!report.ok()) {
                std::cerr << "analysis failed: " << report.message << '\n';
                return 2;
            }
            result = std::move(report.result);
            haveResult = true;
            runsMs.push_back(ms);
        }
    } else {
        runsMs.push_back(result.analysisSeconds * 1000.0);
    }

    if (!options.noCache && !cached.hit()) {
        std::string cacheError;
        if (!bgmedia::AnalysisCache(options.cacheDirectory)
                 .store(result, pcm, analyzer.config(), &cacheError)) {
            std::cerr << "warning: cache store failed: " << cacheError << '\n';
        }
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "BG Media analysis\n";
    std::cout << "  file:       " << options.input.string() << '\n';
    std::cout << "  source:     " << pcm.sampleRate << " Hz, " << pcm.sourceChannels
              << " ch, " << pcm.duration() << " s\n";
    std::cout << "  decode:     " << decodeMs << " ms\n";
    if (options.noCache) {
        std::cout << "  cache:      disabled\n";
    } else {
        std::cout << "  cache:      " << cacheStatusName(cached.status) << " ("
                  << cacheMs << " ms)";
        if (!cached.message.empty()) {
            std::cout << " - " << cached.message;
        }
        std::cout << '\n';
    }
    std::cout << "  analysis:   " << result.analysisSeconds << " s";
    if (result.analysisSeconds > 1e-9) {
        std::cout << "  (" << pcm.duration() / result.analysisSeconds << "x realtime)";
    }
    std::cout << '\n';
    std::cout << "  tempo:      ";
    if (result.bpm > 0.0) {
        std::cout << result.bpm << " BPM, confidence " << result.bpmConfidence;
        std::cout << (result.tempoStable ? ", stable" : ", change suspected");
        if (result.octaveAmbiguous) {
            std::cout << ", octave ambiguous";
        }
        std::cout << '\n';
    } else {
        std::cout << "not detected\n";
    }
    std::cout << "  meter:      " << result.beatsPerBar << "/4 (confidence "
              << result.meterConfidence << ")\n";
    std::size_t downbeats = 0;
    for (const auto& beat : result.beats) {
        if (beat.downbeat) {
            ++downbeats;
        }
    }
    std::cout << "  events:     " << result.beats.size() << " beats (" << downbeats
              << " downbeats), " << result.onsets.size() << " onsets, "
              << result.bassAccents.size() << " bass accents\n";
    std::cout << "  silence:    " << result.silences.size() << " region(s), active "
              << result.activeRatio * 100.0 << "%\n";
    if (!result.tempoSegments.empty()) {
        std::cout << "  segments:   ";
        for (std::size_t i = 0; i < result.tempoSegments.size(); ++i) {
            if (i) {
                std::cout << " | ";
            }
            const auto& s = result.tempoSegments[i];
            std::cout << s.start << "-" << s.end << " s: " << s.bpm << " BPM";
        }
        std::cout << '\n';
    }
    if (!result.tempoTransitions.empty()) {
        std::cout << "  transitions: ";
        for (const auto& t : result.tempoTransitions) {
            std::cout << t.start << "-" << t.end << " s (" << t.fromBpm << "->"
                      << t.toBpm << "), confidence " << t.confidence << "  ";
        }
        std::cout << '\n';
    }
    if (options.repeat > 1) {
        std::cout << "  repeat:     min " << *std::min_element(runsMs.begin(), runsMs.end())
                  << " ms, median " << median(runsMs) << " ms, max "
                  << *std::max_element(runsMs.begin(), runsMs.end()) << " ms over "
                  << runsMs.size() << " run(s)\n";
    }

    if (!options.jsonPath.empty()) {
        const std::string cacheStatus =
            options.noCache ? "disabled" : cacheStatusName(cached.status);
        if (options.jsonPath == "-") {
            writeJson(std::cout, options, result, pcm, decodeMs, cacheMs, cacheStatus,
                      runsMs);
        } else {
            std::ofstream json(options.jsonPath, std::ios::trunc);
            if (!json) {
                std::cerr << "cannot write JSON file: " << options.jsonPath.string()
                          << '\n';
                return 2;
            }
            writeJson(json, options, result, pcm, decodeMs, cacheMs, cacheStatus, runsMs);
        }
    }

    return 0;
}
