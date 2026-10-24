#include "DesktopApp.hpp"

#include "MiniaudioPlayer.hpp"
#include "TextRenderer.hpp"
#include "Theme.hpp"
#include "Ui.hpp"
#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/audio/MiniaudioDecoder.hpp"
#include "bgmedia/cache/AnalysisCache.hpp"
#include "bgmedia/choreography/Choreography.hpp"
#include "bgmedia/version.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bgmedia::desktop {
namespace {

using Clock = std::chrono::steady_clock;
using ui::Rect;

constexpr float kPi = 3.14159265358979323846f;

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::filesystem::path fromUtf8(const char* value) {
    if (!value) {
        return {};
    }
    const std::u8string text(reinterpret_cast<const char8_t*>(value));
    return std::filesystem::path(text);
}

std::string formatTime(double seconds, bool hundredths = false) {
    if (!std::isfinite(seconds)) {
        seconds = 0.0;
    }
    seconds = std::max(0.0, seconds);
    const int total = static_cast<int>(seconds);
    const int hours = total / 3600;
    const int minutes = (total / 60) % 60;
    const int secs = total % 60;
    std::ostringstream out;
    if (hours > 0) {
        out << hours << ':' << std::setw(2) << std::setfill('0') << minutes << ':'
            << std::setw(2) << std::setfill('0') << secs;
    } else {
        out << std::setw(2) << std::setfill('0') << minutes << ':' << std::setw(2)
            << std::setfill('0') << secs;
    }
    if (hundredths) {
        const int cs = static_cast<int>(std::floor((seconds - std::floor(seconds)) * 100.0));
        out << '.' << std::setw(2) << std::setfill('0') << cs;
    }
    return out.str();
}

std::string formatNumber(double value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string formatMs(double milliseconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(milliseconds < 10.0 ? 1 : 0) << milliseconds
        << " ms";
    return out.str();
}

std::string moveName(MoveKind move) {
    switch (move) {
    case MoveKind::Idle:
        return "IDLE";
    case MoveKind::Pulse:
        return "PULSE";
    case MoveKind::Bounce:
        return "BOUNCE";
    case MoveKind::Sway:
        return "SWAY";
    case MoveKind::ArmWave:
        return "ARM WAVE";
    case MoveKind::Spin:
        return "SPIN";
    case MoveKind::Pose:
        return "POSE";
    case MoveKind::Freeze:
        return "FREEZE";
    case MoveKind::HeadNod:
        return "HEAD NOD";
    case MoveKind::BassDip:
        return "BASS DIP";
    case MoveKind::AccentPose:
        return "ACCENT POSE";
    }
    return "IDLE";
}

std::string cacheName(CacheStatus status, bool fromCache) {
    if (fromCache) {
        return "HIT";
    }
    switch (status) {
    case CacheStatus::Hit:
        return "HIT";
    case CacheStatus::Miss:
        return "MISS";
    case CacheStatus::Corrupt:
        return "CORRUPT";
    case CacheStatus::Error:
        return "ERROR";
    }
    return "MISS";
}

const Beat* previousBeat(const std::vector<Beat>& beats, double time, double* offset) {
    if (beats.empty()) {
        return nullptr;
    }
    const auto it = std::upper_bound(beats.begin(), beats.end(), time,
                                     [](double value, const Beat& beat) {
                                         return value < beat.time;
                                     });
    if (it == beats.begin()) {
        return nullptr;
    }
    const Beat* beat = &*(it - 1);
    if (offset) {
        *offset = time - beat->time;
    }
    return beat;
}

SDL_Color spectrogramColor(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    const struct Stop {
        float at;
        SDL_Color color;
    } stops[] = {
        {0.00f, {7, 10, 17, 255}},
        {0.16f, {13, 35, 47, 255}},
        {0.36f, {18, 87, 94, 255}},
        {0.58f, {54, 182, 166, 255}},
        {0.78f, {139, 238, 204, 255}},
        {0.92f, {244, 249, 202, 255}},
        {1.00f, {255, 231, 180, 255}},
    };
    for (std::size_t i = 1; i < std::size(stops); ++i) {
        if (value <= stops[i].at) {
            const float t = (value - stops[i - 1].at) /
                            std::max(1e-6f, stops[i].at - stops[i - 1].at);
            return theme::mix(stops[i - 1].color, stops[i].color, t);
        }
    }
    return stops[std::size(stops) - 1].color;
}

struct LoadedTrack {
    std::filesystem::path path;
    std::string displayName;
    std::string displayPath;
    PcmBuffer pcm;
    AnalysisResult analysis;
    Choreography choreography;
    std::vector<WaveformPeak> hiresWaveform;
    double hiresPeaksPerSecond = 1000.0;
    double decodeMs = 0.0;
    double cacheMs = 0.0;
    double totalMs = 0.0;
    CacheStatus cacheStatus = CacheStatus::Miss;
    bool fromCache = false;
    std::string cacheMessage;
    std::string warning;
};

struct CompletedLoad {
    std::uint64_t generation = 0;
    bool ok = false;
    std::string error;
    std::unique_ptr<LoadedTrack> track;
};

struct Layout {
    Rect header{0, 0, 1440, 64};
    Rect toolbar{24, 76, 1392, 44};
    Rect timelineCard{24, 124, 1004, 408};
    Rect choreoCard{24, 544, 1004, 296};
    Rect right1{1044, 124, 372, 190};
    Rect right2{1044, 326, 372, 188};
    Rect right3{1044, 526, 372, 142};
    Rect right4{1044, 680, 372, 160};
    Rect footer{0, 856, 1440, 44};
    Rect empty{24, 124, 1392, 716};

    Rect loadButton{24, 80, 116, 40};
    Rect playButton{148, 80, 44, 40};
    Rect stopButton{200, 80, 40, 40};
    Rect rateButton{248, 80, 72, 40};
    Rect syncMinus{1244, 88, 28, 24};
    Rect syncPlus{1352, 88, 28, 24};
    Rect emptyLoad{};

    Rect motionSmooth{};
    Rect motionDebug{};
    Rect motionLab{};

    Rect ruler{};
    Rect wave{};
    Rect spectrogram{};
    Rect tempo{};
    Rect events{};
    Rect plot{};
};

enum class AppState {
    Empty,
    Loading,
    Ready,
    Error
};

std::vector<WaveformPeak> buildHiresWaveform(const PcmBuffer& pcm, double& peaksPerSecond) {
    std::vector<WaveformPeak> peaks;
    if (!pcm.valid()) {
        return peaks;
    }
    const double duration = pcm.duration();
    const double targetRate = 1000.0;
    const std::size_t bins = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::ceil(duration * targetRate)), 1, 2'000'000);
    peaks.resize(bins);
    const std::size_t sampleCount = pcm.samples.size();
    for (std::size_t i = 0; i < sampleCount; ++i) {
        float value = pcm.samples[i];
        if (!std::isfinite(value)) {
            value = 0.0f;
        }
        value = std::clamp(value, -1.0f, 1.0f);
        const std::size_t bin = std::min<std::size_t>(
            bins - 1, static_cast<std::size_t>(
                          static_cast<double>(i) * static_cast<double>(bins) /
                          static_cast<double>(std::max<std::size_t>(1, sampleCount))));
        WaveformPeak& peak = peaks[bin];
        peak.minValue = std::min(peak.minValue, value);
        peak.maxValue = std::max(peak.maxValue, value);
    }
    peaksPerSecond = static_cast<double>(bins) / std::max(0.001, duration);
    return peaks;
}

bool isAudioExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

std::filesystem::path findAssetsDirectory(const std::filesystem::path& base) {
    const std::vector<std::filesystem::path> candidates = {
        base / "assets",
        base.parent_path() / "assets",
        std::filesystem::current_path() / "assets",
        std::filesystem::current_path() / ".." / "assets",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "fonts" / "Inter-Regular.ttf")) {
            return candidate;
        }
    }
    return base / "assets";
}

std::string ellipsize(const TextRenderer& text, std::string value, float maxWidth,
                      int size, FontWeight weight) {
    if (maxWidth <= 0.0f) {
        return {};
    }
    if (text.measure(value, size, weight) <= maxWidth) {
        return value;
    }
    const std::string suffix = "...";
    while (!value.empty() && text.measure(value + suffix, size, weight) > maxWidth) {
        value.pop_back();
        while (!value.empty() &&
               (static_cast<unsigned char>(value.back()) & 0xC0) == 0x80) {
            value.pop_back();
        }
    }
    return value + suffix;
}

} // namespace

struct DesktopApp::Impl {
    // ---------------------------------------------------------------------
    // Lifecycle and state
    // ---------------------------------------------------------------------
    DesktopOptions options;
    Layout layout;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<TextRenderer> text;
    MiniaudioPlayer player;
    PlaybackClock clock;
    AnalysisConfig config;

    std::filesystem::path cacheDirectory;
    std::filesystem::path assetsDirectory;
    std::vector<SDL_DialogFileFilter> filters;
    std::string dialogStartLocation;

    AppState state = AppState::Empty;
    std::unique_ptr<LoadedTrack> track;
    SDL_Texture* spectrogramTexture = nullptr;
    int spectrogramWidth = 0;
    int spectrogramHeight = 0;

    double viewStart = 0.0;
    double viewEnd = 0.0;
    double hoverTime = -1.0;
    double mouseX = -1.0f;
    double mouseY = -1.0f;
    bool mouseDown = false;
    bool scrubbing = false;
    bool panning = false;
    bool resumeAfterScrub = false;
    float panStartX = 0.0f;
    double panStartView = 0.0;
    double scrubPosition = 0.0;
    double fallbackPosition = 0.0;
    double capturePosition = 0.0;
    double latencyTrim = 0.0;
    double toastUntil = 0.0;
    std::string toast;
    std::string audioError;
    std::string errorMessage;
    std::string loadingName;
    std::atomic<float> loadingProgress{0.0f};
    double animationTime = 0.0;
    bool captureMode = false;
    bool running = true;
    bool fullscreen = false;

    bool motionDebug = false;
    bool motionStepped = false;
    int motionLabBpm = 0;
    std::unique_ptr<AnalysisResult> motionLabAnalysis;
    std::unique_ptr<Choreography> motionLabChoreography;

    std::thread workerThread;
    std::atomic<bool> workerCancel{false};
    std::atomic<std::uint64_t> generation{0};
    std::mutex workerMutex;
    std::unique_ptr<CompletedLoad> completedLoad;
    std::string workerStage = "preparing";
    std::atomic<float> workerProgress{0.0f};

    std::mutex dialogMutex;
    std::string dialogPath;
    bool dialogPending = false;
    std::string dialogError;

    ~Impl() {
        stopWorker();
        if (spectrogramTexture) {
            SDL_DestroyTexture(spectrogramTexture);
        }
        player.close();
        text.reset();
        if (renderer) {
            SDL_DestroyRenderer(renderer);
        }
        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
    }

    // ---------------------------------------------------------------------
    // Setup
    // ---------------------------------------------------------------------
    bool initialize() {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        window = SDL_CreateWindow("BG Media", 1440, 900,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetWindowMinimumSize(window, 1100, 700);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            renderer = SDL_CreateRenderer(window, "software");
        }
        if (!renderer) {
            std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderLogicalPresentation(renderer, 1440, 900,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
        SDL_SetRenderVSync(renderer, 1);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        const char* base = SDL_GetBasePath();
        // SDL_GetBasePath() returns a cached internal string and must not be
        // freed; only SDL_GetPrefPath() transfers ownership.
        const std::filesystem::path basePath =
            base ? fromUtf8(base) : std::filesystem::current_path();
        assetsDirectory = findAssetsDirectory(basePath);

        text = std::make_unique<TextRenderer>();
        std::string fontError;
        if (!text->init(renderer, assetsDirectory, &fontError)) {
            std::fprintf(stderr, "font setup failed: %s\n", fontError.c_str());
            return false;
        }

        if (!options.cacheDirectory.empty()) {
            cacheDirectory = options.cacheDirectory;
        } else {
            char* pref = SDL_GetPrefPath("BGMedia", "BGMedia");
            if (pref) {
                cacheDirectory = fromUtf8(pref) / "analysis-cache";
                SDL_free(pref);
            } else {
                cacheDirectory = std::filesystem::current_path() / ".bgmedia_cache";
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(cacheDirectory, ec);

        filters = {
            SDL_DialogFileFilter{"Audio files", "*.wav;*.mp3;*.flac"},
            SDL_DialogFileFilter{"All files", "*.*"},
        };

        motionDebug = options.motionDebug;
        motionStepped = options.motionStepped;
        motionLabBpm = std::max(0, options.motionLabBpm);

        SDL_ShowWindow(window);
        return true;
    }

    void shutdown() {
        stopWorker();
        player.close();
        if (spectrogramTexture) {
            SDL_DestroyTexture(spectrogramTexture);
            spectrogramTexture = nullptr;
        }
        text.reset();
        if (renderer) {
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }
        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
    }

    // ---------------------------------------------------------------------
    // Worker
    // ---------------------------------------------------------------------
    void setStage(const std::string& stage, float progress) {
        const float clamped = std::clamp(progress, 0.0f, 1.0f);
        {
            std::lock_guard lock(workerMutex);
            workerStage = stage;
        }
        workerProgress.store(clamped, std::memory_order_relaxed);
        loadingProgress.store(clamped, std::memory_order_relaxed);
    }

    std::unique_ptr<LoadedTrack> performLoad(const std::filesystem::path& path,
                                             std::uint64_t gen, std::string& error) {
        const auto started = Clock::now();
        auto result = std::make_unique<LoadedTrack>();
        result->path = path;
        result->displayName = toUtf8(path.filename());
        result->displayPath = toUtf8(path);

        if (!isAudioExtension(path)) {
            error = "Unsupported file type. Choose WAV, MP3 or FLAC.";
            return nullptr;
        }
        if (!std::filesystem::exists(path)) {
            error = "File does not exist: " + toUtf8(path);
            return nullptr;
        }

        setStage("decoding audio", 0.02f);
        MiniaudioDecoder decoder;
        const DecodeResult decoded = decoder.decode(
            path, [&](float fraction, std::string_view stage) {
                setStage(std::string(stage), 0.02f + 0.18f * fraction);
            });
        if (!decoded.ok) {
            error = decoded.error;
            return nullptr;
        }
        if (workerCancel.load() || generation.load() != gen) {
            error = "cancelled";
            return nullptr;
        }

        std::string pcmError;
        result->pcm = PcmBuffer::fromAudio(decoded.audio, &pcmError);
        if (!result->pcm.valid()) {
            error = "Could not prepare PCM: " + pcmError;
            return nullptr;
        }

        result->decodeMs =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();

        setStage("checking cache", 0.22f);
        std::optional<AnalysisResult> cached;
        if (!options.noCache) {
            const auto cacheStart = Clock::now();
            AnalysisCache cache(cacheDirectory);
            CacheLoadResult loaded = cache.load(result->pcm, config);
            result->cacheMs = std::chrono::duration<double, std::milli>(
                                  Clock::now() - cacheStart)
                                  .count();
            result->cacheStatus = loaded.status;
            result->cacheMessage = loaded.message;
            if (loaded.hit()) {
                cached = std::move(loaded.result);
                result->fromCache = true;
            }
        }

        if (cached) {
            result->analysis = std::move(*cached);
            setStage("loaded from cache", 0.84f);
        } else {
            Analyzer analyzer(config);
            setStage("analysing", 0.25f);
            AnalysisReport report = analyzer.analyze(
                result->pcm,
                [&](float fraction, const std::string& stage) {
                    setStage(stage, 0.25f + 0.58f * fraction);
                },
                [&]() {
                    return workerCancel.load() || generation.load() != gen;
                });
            if (!report.ok()) {
                error = report.status == AnalysisStatus::Cancelled
                            ? "cancelled"
                            : report.message;
                return nullptr;
            }
            result->analysis = std::move(report.result);
            result->cacheStatus = CacheStatus::Miss;
            if (!options.noCache) {
                setStage("saving cache", 0.86f);
                AnalysisCache cache(cacheDirectory);
                std::string cacheError;
                if (!cache.store(result->analysis, result->pcm, config, &cacheError)) {
                    result->warning = "Cache write failed: " + cacheError;
                }
            }
        }

        if (workerCancel.load() || generation.load() != gen) {
            error = "cancelled";
            return nullptr;
        }

        setStage("building choreography", 0.90f);
        result->choreography = ChoreographyBuilder{}.build(result->analysis);
        setStage("preparing waveform", 0.94f);
        result->hiresWaveform =
            buildHiresWaveform(result->pcm, result->hiresPeaksPerSecond);
        result->totalMs =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        setStage("ready", 1.0f);
        return result;
    }

    void startLoad(const std::filesystem::path& path) {
        stopWorker();
        if (player.isOpen()) {
            player.pause();
            player.close();
        }
        audioError.clear();
        errorMessage.clear();
        loadingName = toUtf8(path.filename());
        loadingProgress.store(0.0f);
        state = AppState::Loading;
        workerCancel.store(false);
        const std::uint64_t gen = generation.fetch_add(1) + 1;
        {
            std::lock_guard lock(workerMutex);
            completedLoad.reset();
            workerStage = "preparing";
            workerProgress.store(0.0f);
        }
        workerThread = std::thread([this, path, gen]() {
            std::string error;
            std::unique_ptr<LoadedTrack> loaded = performLoad(path, gen, error);
            auto completed = std::make_unique<CompletedLoad>();
            completed->generation = gen;
            completed->ok = loaded != nullptr;
            completed->error = std::move(error);
            completed->track = std::move(loaded);
            {
                std::lock_guard lock(workerMutex);
                if (completed->generation == generation.load()) {
                    completedLoad = std::move(completed);
                }
            }
        });
    }

    void stopWorker() {
        workerCancel.store(true);
        if (workerThread.joinable()) {
            workerThread.join();
        }
        workerCancel.store(false);
    }

    void pollWorker() {
        std::unique_ptr<CompletedLoad> completed;
        {
            std::lock_guard lock(workerMutex);
            completed = std::move(completedLoad);
        }
        if (!completed || completed->generation != generation.load()) {
            return;
        }
        if (!completed->ok) {
            if (completed->error != "cancelled") {
                state = AppState::Error;
                errorMessage = completed->error;
                showToast("Analysis failed");
            }
            return;
        }
        applyTrack(std::move(completed->track));
    }

    void applyTrack(std::unique_ptr<LoadedTrack> loaded) {
        if (!loaded) {
            return;
        }
        track = std::move(loaded);
        state = AppState::Ready;
        errorMessage.clear();
        audioError.clear();
        rebuildSpectrogram();
        if (motionLabBpm > 0) {
            rebuildMotionLab();
        } else {
            motionLabAnalysis.reset();
            motionLabChoreography.reset();
        }
        viewStart = 0.0;
        viewEnd = std::max(0.001, track->analysis.info.duration);
        fallbackPosition = 0.0;
        capturePosition = std::clamp(options.captureTime, 0.0, viewEnd);
        scrubbing = false;
        panning = false;
        clock.reset();
        player.close();
        if (!options.noAudio && (!captureMode || options.audioSmoke)) {
            std::string error;
            if (!player.open(track->path, track->analysis.info.duration, &error)) {
                audioError = error;
            } else {
                (void)clock.update(player.snapshot());
            }
        }
        std::string title = "BG Media";
        if (!track->displayName.empty()) {
            title += " - " + track->displayName;
        }
        SDL_SetWindowTitle(window, title.c_str());
        if (!captureMode) {
            showToast(track->fromCache ? "Analysis loaded from cache"
                                       : "Analysis complete");
        }
    }

    void showToast(std::string message) {
        toast = std::move(message);
        toastUntil = animationTime + 3.2;
    }

    // ---------------------------------------------------------------------
    // BG Motion lab / comparison
    // ---------------------------------------------------------------------
    [[nodiscard]] const AnalysisResult* activeAnalysis() const {
        if (motionLabBpm > 0 && motionLabAnalysis) {
            return motionLabAnalysis.get();
        }
        return track ? &track->analysis : nullptr;
    }

    [[nodiscard]] const Choreography* activeChoreography() const {
        if (motionLabBpm > 0 && motionLabChoreography) {
            return motionLabChoreography.get();
        }
        return track ? &track->choreography : nullptr;
    }

    [[nodiscard]] CharacterPose sampleActiveAt(double time) const {
        const Choreography* choreography = activeChoreography();
        if (!choreography) {
            return {};
        }
        ChoreographySampler sampler;
        return motionStepped ? sampler.sampleStepped(*choreography, time)
                             : sampler.sample(*choreography, time);
    }

    [[nodiscard]] MotionSample sampleActiveWithDebug(double time) const {
        MotionSample sample;
        const Choreography* choreography = activeChoreography();
        if (!choreography) {
            return sample;
        }
        ChoreographySampler sampler;
        if (motionStepped) {
            sample.pose = sampler.sampleStepped(*choreography, time);
            sample.move = sample.pose.move;
            sample.accent = sample.pose.accented;
            return sample;
        }
        return sampler.sampleWithDebug(*choreography, time);
    }

    void rebuildMotionLab() {
        if (!track || motionLabBpm <= 0) {
            motionLabAnalysis.reset();
            motionLabChoreography.reset();
            return;
        }
        const double duration = std::max(2.0, track->analysis.info.duration);
        auto analysis = std::make_unique<AnalysisResult>();
        analysis->info = track->analysis.info;
        analysis->info.duration = duration;
        analysis->bpm = static_cast<double>(motionLabBpm);
        analysis->bpmConfidence = 1.0f;
        analysis->tempoStable = true;
        analysis->beatsPerBar = 4;
        const double period = 60.0 / static_cast<double>(motionLabBpm);
        const double offset = 0.45;
        int index = 0;
        for (double time = offset; time < duration - 0.1; time += period, ++index) {
            Beat beat;
            beat.time = time;
            beat.beatInBar = index % 4;
            beat.barIndex = index / 4;
            beat.downbeat = beat.beatInBar == 0;
            beat.strength = beat.downbeat ? 1.0f : 0.64f;
            beat.confidence = 1.0f;
            analysis->beats.push_back(beat);
        }
        // The lab uses the same musical event vocabulary as real analysis:
        // bass on every other downbeat, so both BassDip and AccentPose appear.
        for (std::size_t i = 0; i < analysis->beats.size(); ++i) {
            const Beat& beat = analysis->beats[i];
            if (beat.downbeat && (beat.barIndex % 2 == 0)) {
                BassAccent accent;
                accent.time = beat.time;
                accent.strength = 0.95f;
                accent.lowBandRatio = 0.75f;
                accent.confidence = 1.0f;
                accent.nearestBeat = static_cast<int>(i);
                analysis->bassAccents.push_back(accent);
            }
        }
        Choreography style = ChoreographyBuilder{}.build(*analysis);
        motionLabChoreography = std::make_unique<Choreography>(std::move(style));
        motionLabAnalysis = std::move(analysis);
    }

    void setMotionLab(int bpm) {
        motionLabBpm = std::max(0, bpm);
        if (motionLabBpm == 0) {
            motionLabAnalysis.reset();
            motionLabChoreography.reset();
            showToast("Motion lab off - using analysed music");
            return;
        }
        if (!track) {
            motionLabBpm = 0;
            showToast("Load a track before enabling the motion lab");
            return;
        }
        rebuildMotionLab();
        showToast("Motion lab: synthetic " + std::to_string(motionLabBpm) +
                  " BPM beat grid");
    }

    void cycleMotionLab() {
        static constexpr int choices[] = {0, 92, 123, 163, 180};
        int next = choices[1];
        for (std::size_t i = 0; i < std::size(choices); ++i) {
            if (choices[i] == motionLabBpm) {
                next = choices[(i + 1) % std::size(choices)];
                break;
            }
        }
        setMotionLab(next);
    }

    // ---------------------------------------------------------------------
    // Dialog and file handling
    // ---------------------------------------------------------------------
    void openFileDialog() {
        if (!window) {
            return;
        }
        if (track) {
            dialogStartLocation = toUtf8(track->path.parent_path());
        } else {
            dialogStartLocation = toUtf8(std::filesystem::current_path());
        }
        SDL_ShowOpenFileDialog(&Impl::dialogCallback, this, window, filters.data(),
                               static_cast<int>(filters.size()),
                               dialogStartLocation.empty() ? nullptr
                                                           : dialogStartLocation.c_str(),
                               false);
    }

    static void SDLCALL dialogCallback(void* userdata, const char* const* filelist,
                                       int) {
        auto* self = static_cast<Impl*>(userdata);
        std::lock_guard lock(self->dialogMutex);
        if (filelist && filelist[0]) {
            self->dialogPath = filelist[0];
            self->dialogPending = true;
        } else if (!filelist) {
            self->dialogError = SDL_GetError();
        }
    }

    void pollDialog() {
        std::string path;
        std::string error;
        {
            std::lock_guard lock(dialogMutex);
            if (dialogPending) {
                path = std::move(dialogPath);
                dialogPending = false;
                dialogPath.clear();
            }
            if (!dialogError.empty()) {
                error = std::move(dialogError);
                dialogError.clear();
            }
        }
        if (!error.empty()) {
            showToast("File dialog: " + error);
        }
        if (!path.empty()) {
            startLoad(fromUtf8(path.c_str()));
        }
    }

    // ---------------------------------------------------------------------
    // Text helpers
    // ---------------------------------------------------------------------
    void label(std::string_view value, float x, float y, int size = 10,
               SDL_Color color = theme::muted, float spacing = 1.15f) {
        text->draw(value, x, y, size, FontWeight::SemiBold, color, 1.0f, spacing);
    }

    void labelRight(std::string_view value, float right, float y, int size = 10,
                    SDL_Color color = theme::muted, float spacing = 1.15f) {
        text->drawRight(value, right, y, size, FontWeight::SemiBold, color, 1.0f,
                        spacing);
    }

    void body(std::string_view value, float x, float y, int size = 14,
              SDL_Color color = theme::text, FontWeight weight = FontWeight::Regular) {
        text->draw(value, x, y, size, weight, color);
    }

    // ---------------------------------------------------------------------
    // Spectrogram texture
    // ---------------------------------------------------------------------
    void rebuildSpectrogram() {
        if (spectrogramTexture) {
            SDL_DestroyTexture(spectrogramTexture);
            spectrogramTexture = nullptr;
        }
        spectrogramWidth = 0;
        spectrogramHeight = 0;
        if (!track || !track->analysis.spectrogram.valid()) {
            return;
        }
        const Spectrogram& spec = track->analysis.spectrogram;
        const int width = std::clamp(static_cast<int>(spec.frameCount), 256, 4096);
        const int height = spec.bands;
        spectrogramWidth = width;
        spectrogramHeight = height;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
        for (int x = 0; x < width; ++x) {
            const std::uint32_t begin = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(x) * spec.frameCount / width);
            const std::uint32_t end = std::max(
                begin + 1, static_cast<std::uint32_t>(
                               static_cast<std::uint64_t>(x + 1) * spec.frameCount / width));
            for (int band = 0; band < height; ++band) {
                std::uint8_t value = 0;
                for (std::uint32_t frame = begin;
                     frame < end && frame < spec.frameCount; ++frame) {
                    value = std::max(
                        value, spec.magnitudes[static_cast<std::size_t>(frame) * height + band]);
                }
                const SDL_Color color = spectrogramColor(static_cast<float>(value) / 255.0f);
                const std::size_t at =
                    (static_cast<std::size_t>(band) * width + x) * 4;
                pixels[at] = color.r;
                pixels[at + 1] = color.g;
                pixels[at + 2] = color.b;
                pixels[at + 3] = 255;
            }
        }
        spectrogramTexture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
        if (!spectrogramTexture) {
            return;
        }
        SDL_SetTextureScaleMode(spectrogramTexture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(spectrogramTexture, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(spectrogramTexture, nullptr, pixels.data(), width * 4);
    }

    // ---------------------------------------------------------------------
    // Transport
    // ---------------------------------------------------------------------
    [[nodiscard]] double duration() const {
        return track ? track->analysis.info.duration : 0.0;
    }

    [[nodiscard]] double currentPosition() const {
        if (!track) {
            return 0.0;
        }
        if (captureMode) {
            return std::clamp(capturePosition, 0.0, duration());
        }
        if (scrubbing) {
            return std::clamp(scrubPosition, 0.0, duration());
        }
        if (player.audioAvailable() && player.isOpen()) {
            return std::clamp(clock.position(), 0.0, duration());
        }
        return std::clamp(fallbackPosition, 0.0, duration());
    }

    [[nodiscard]] bool playing() const {
        if (!track || captureMode) {
            return false;
        }
        if (scrubbing) {
            return false;
        }
        return clock.playing();
    }

    void updatePlayback() {
        if (!track || captureMode) {
            return;
        }
        if (!player.audioAvailable() || !player.isOpen()) {
            return;
        }
        if (player.ended()) {
            player.pause();
        }
        const TransportSnapshot snapshot = player.snapshot();
        const double position = clock.update(snapshot);
        if (snapshot.playing) {
            const double span = viewEnd - viewStart;
            if (span > 0.0 && (position > viewEnd || position < viewStart)) {
                viewStart = std::clamp(position - span * 0.2, 0.0,
                                       std::max(0.0, duration() - span));
                viewEnd = viewStart + span;
            }
        }
    }

    void togglePlay() {
        if (!track) {
            openFileDialog();
            return;
        }
        if (captureMode) {
            return;
        }
        if (!player.audioAvailable()) {
            showToast(audioError.empty() ? "Audio output is unavailable"
                                         : audioError);
            return;
        }
        if (clock.playing()) {
            player.pause();
        } else {
            player.play();
        }
        (void)clock.update(player.snapshot());
    }

    void stopPlayback() {
        if (!track) {
            return;
        }
        if (player.audioAvailable()) {
            player.pause();
            player.seek(0.0);
            (void)clock.update(player.snapshot());
        }
        fallbackPosition = 0.0;
        viewStart = 0.0;
        viewEnd = std::max(0.001, duration());
    }

    void seekTo(double time, bool resume) {
        if (!track) {
            return;
        }
        time = std::clamp(time, 0.0, duration());
        fallbackPosition = time;
        if (player.audioAvailable()) {
            player.pause();
            player.seek(time);
            (void)clock.update(player.snapshot());
            if (resume) {
                player.play();
            }
        }
        ensureTimeVisible(time);
    }

    void seekRelative(double delta) {
        if (!track) {
            return;
        }
        seekTo(currentPosition() + delta, playing());
    }

    void ensureTimeVisible(double time) {
        if (duration() <= 0.0) {
            return;
        }
        const double span = std::max(0.5, viewEnd - viewStart);
        if (time < viewStart || time > viewEnd) {
            viewStart = std::clamp(time - span * 0.5, 0.0,
                                   std::max(0.0, duration() - span));
            viewEnd = viewStart + span;
        }
    }

    void cycleRate() {
        if (!track) {
            return;
        }
        static constexpr double rates[] = {0.75, 1.0, 1.25};
        const double current = player.snapshot().rate;
        int index = 1;
        for (int i = 0; i < 3; ++i) {
            if (std::abs(current - rates[i]) < 0.01) {
                index = i;
                break;
            }
        }
        index = (index + 1) % 3;
        player.setRate(rates[index]);
        (void)clock.update(player.snapshot());
        showToast("Playback rate " + formatNumber(rates[index], 2) + "x (pitch follows speed)");
    }

    void adjustLatency(double delta) {
        if (!track) {
            return;
        }
        latencyTrim = std::clamp(latencyTrim + delta, -0.15, 0.15);
        player.setLatencyTrim(latencyTrim);
        (void)clock.update(player.snapshot());
        showToast("Sync trim " + std::string(latencyTrim >= 0.0 ? "+" : "") +
                  formatNumber(latencyTrim * 1000.0, 0) + " ms");
    }

    // ---------------------------------------------------------------------
    // Timeline mapping
    // ---------------------------------------------------------------------
    [[nodiscard]] double timelineSpan() const {
        return std::max(0.001, viewEnd - viewStart);
    }

    [[nodiscard]] float timeToX(double time, const Rect& plot) const {
        const double t = (time - viewStart) / timelineSpan();
        return plot.x + static_cast<float>(t) * plot.w;
    }

    [[nodiscard]] double xToTime(float x, const Rect& plot) const {
        const double t = (x - plot.x) / std::max(1.0f, plot.w);
        return std::clamp(viewStart + t * timelineSpan(), 0.0, duration());
    }

    void zoomAt(float mousePositionX, float wheelY) {
        if (!track) {
            return;
        }
        const double span = timelineSpan();
        const double anchor = xToTime(mousePositionX, layout.plot);
        double newSpan = span * std::pow(1.16, -wheelY);
        newSpan = std::clamp(newSpan, std::min(2.0, std::max(0.25, duration())),
                             std::max(0.25, duration()));
        const double fraction =
            (mousePositionX - layout.plot.x) / std::max(1.0f, layout.plot.w);
        double newStart = anchor - fraction * newSpan;
        newStart = std::clamp(newStart, 0.0, std::max(0.0, duration() - newSpan));
        viewStart = newStart;
        viewEnd = newStart + newSpan;
    }

    // ---------------------------------------------------------------------
    // Event handling
    // ---------------------------------------------------------------------
    void handleEvent(const SDL_Event& original, bool converted) {
        SDL_Event event = original;
        if (!converted &&
            (event.type == SDL_EVENT_MOUSE_MOTION ||
             event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
             event.type == SDL_EVENT_MOUSE_WHEEL)) {
            (void)SDL_ConvertEventToRenderCoordinates(renderer, &event);
        }

        switch (event.type) {
        case SDL_EVENT_QUIT:
            running = false;
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data) {
                startLoad(fromUtf8(event.drop.data));
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat) {
                break;
            }
            handleKey(event.key);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            mouseX = event.motion.x;
            mouseY = event.motion.y;
            if (scrubbing) {
                scrubPosition = xToTime(static_cast<float>(mouseX), layout.plot);
            }
            if (panning) {
                const double span = timelineSpan();
                const double delta =
                    (static_cast<double>(mouseX) - panStartX) / layout.plot.w * span;
                viewStart = std::clamp(panStartView - delta, 0.0,
                                       std::max(0.0, duration() - span));
                viewEnd = viewStart + span;
                updateTimelineLayout();
            }
            if (layout.plot.contains(static_cast<float>(mouseX),
                                     static_cast<float>(mouseY))) {
                hoverTime = xToTime(static_cast<float>(mouseX), layout.plot);
            } else {
                hoverTime = -1.0;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            mouseX = event.button.x;
            mouseY = event.button.y;
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouseDown = true;
                handleLeftDown(static_cast<float>(event.button.x),
                               static_cast<float>(event.button.y),
                               event.button.clicks);
            } else if (event.button.button == SDL_BUTTON_MIDDLE &&
                       layout.plot.contains(static_cast<float>(event.button.x),
                                            static_cast<float>(event.button.y))) {
                panning = true;
                panStartX = event.button.x;
                panStartView = viewStart;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            mouseX = event.button.x;
            mouseY = event.button.y;
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouseDown = false;
                handleLeftUp(static_cast<float>(event.button.x),
                             static_cast<float>(event.button.y));
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                panning = false;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            mouseX = event.wheel.mouse_x;
            mouseY = event.wheel.mouse_y;
            if (layout.plot.contains(static_cast<float>(mouseX),
                                     static_cast<float>(mouseY)) ||
                layout.timelineCard.contains(static_cast<float>(mouseX),
                                             static_cast<float>(mouseY))) {
                zoomAt(static_cast<float>(mouseX), event.wheel.y);
                updateTimelineLayout();
            }
            break;
        default:
            break;
        }
    }

    void handleKey(const SDL_KeyboardEvent& key) {
        const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
        const bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;
        switch (key.key) {
        case SDLK_SPACE:
            togglePlay();
            break;
        case SDLK_L:
            if (ctrl || key.key == SDLK_L) {
                openFileDialog();
            }
            break;
        case SDLK_O:
            if (ctrl) {
                openFileDialog();
            }
            break;
        case SDLK_LEFT:
            seekRelative(shift ? -30.0 : -5.0);
            break;
        case SDLK_RIGHT:
            seekRelative(shift ? 30.0 : 5.0);
            break;
        case SDLK_HOME:
            seekTo(0.0, playing());
            break;
        case SDLK_END:
            seekTo(duration(), playing());
            break;
        case SDLK_R:
            cycleRate();
            break;
        case SDLK_D:
            motionDebug = !motionDebug;
            showToast(motionDebug ? "Motion debug on" : "Motion debug off");
            break;
        case SDLK_V:
            motionStepped = !motionStepped;
            showToast(motionStepped ? "Legacy stepped animation"
                                    : "Smooth BG Motion 2.0");
            break;
        case SDLK_B:
            cycleMotionLab();
            break;
        case SDLK_LEFTBRACKET:
            adjustLatency(-0.005);
            break;
        case SDLK_RIGHTBRACKET:
            adjustLatency(0.005);
            break;
        case SDLK_F:
            fullscreen = !fullscreen;
            (void)SDL_SetWindowFullscreen(window, fullscreen);
            break;
        case SDLK_ESCAPE:
            if (fullscreen) {
                fullscreen = false;
                (void)SDL_SetWindowFullscreen(window, false);
            }
            break;
        default:
            break;
        }
    }

    void handleLeftDown(float x, float y, int clicks) {
        if (state == AppState::Loading) {
            return;
        }
        if (layout.loadButton.contains(x, y) ||
            ((state == AppState::Empty || state == AppState::Error) &&
             layout.emptyLoad.contains(x, y))) {
            openFileDialog();
            return;
        }
        if (layout.playButton.contains(x, y)) {
            togglePlay();
            return;
        }
        if (layout.stopButton.contains(x, y)) {
            stopPlayback();
            return;
        }
        if (layout.rateButton.contains(x, y)) {
            cycleRate();
            return;
        }
        if (layout.syncMinus.contains(x, y)) {
            adjustLatency(-0.005);
            return;
        }
        if (layout.syncPlus.contains(x, y)) {
            adjustLatency(0.005);
            return;
        }
        if (layout.motionSmooth.contains(x, y)) {
            motionStepped = !motionStepped;
            showToast(motionStepped ? "Legacy stepped animation"
                                    : "Smooth BG Motion 2.0");
            return;
        }
        if (layout.motionDebug.contains(x, y)) {
            motionDebug = !motionDebug;
            showToast(motionDebug ? "Motion debug on" : "Motion debug off");
            return;
        }
        if (layout.motionLab.contains(x, y)) {
            cycleMotionLab();
            return;
        }
        if (!track) {
            return;
        }
        if (layout.plot.contains(x, y)) {
            if (clicks >= 2) {
                viewStart = 0.0;
                viewEnd = std::max(0.001, duration());
                updateTimelineLayout();
                return;
            }
            resumeAfterScrub = playing();
            if (player.audioAvailable()) {
                player.pause();
                (void)clock.update(player.snapshot());
            }
            scrubbing = true;
            scrubPosition = xToTime(x, layout.plot);
            hoverTime = scrubPosition;
        }
    }

    void handleLeftUp(float x, float y) {
        (void)y;
        if (scrubbing) {
            scrubbing = false;
            const bool resume = resumeAfterScrub;
            seekTo(xToTime(x, layout.plot), resume);
            resumeAfterScrub = false;
        }
    }

    // ---------------------------------------------------------------------
    // Layout
    // ---------------------------------------------------------------------
    void updateTimelineLayout() {
        const Rect card = layout.timelineCard;
        const float x = card.x + 18.0f;
        const float w = card.w - 36.0f;
        layout.ruler = {x, card.y + 44.0f, w, 22.0f};
        layout.wave = {x, card.y + 70.0f, w, 90.0f};
        layout.spectrogram = {x, card.y + 168.0f, w, 88.0f};
        layout.tempo = {x, card.y + 264.0f, w, 16.0f};
        layout.events = {x, card.y + 288.0f, w, 78.0f};
        layout.plot = {x, layout.wave.y, w, layout.events.bottom() - layout.wave.y};
    }

    void computeLayout() {
        layout.emptyLoad = {layout.empty.centerX() - 86.0f,
                            layout.empty.y + 380.0f, 172.0f, 44.0f};
        const Rect card = layout.choreoCard;
        layout.motionLab = {card.right() - 104.0f, card.y + 10.0f, 90.0f, 24.0f};
        layout.motionDebug = {layout.motionLab.x - 76.0f, card.y + 10.0f, 70.0f, 24.0f};
        layout.motionSmooth = {layout.motionDebug.x - 104.0f, card.y + 10.0f, 98.0f, 24.0f};
        updateTimelineLayout();
    }

    // ---------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------
    void renderFrame();

    void drawBackground();
    void drawHeader();
    void drawToolbar();
    void drawEmptyState();
    void drawTimeline();
    void drawRightPanels();
    void drawChoreography();
    void drawFooter();
    void drawOverlays();
    void drawDancer(const Rect& stage, const CharacterPose& pose);
    void drawBpmPanel(const Rect& rect);
    void drawTempoPanel(const Rect& rect);
    void drawSignalPanel(const Rect& rect);
    void drawAnalysisPanel(const Rect& rect);
    void drawButton(const Rect& rect, std::string_view labelText, bool primary,
                    bool active, int icon);
    void drawSpectrogram();
    void drawWaveform();
    void drawEvents();
    void drawTempoStrip();
    void drawRuler();
    void drawCursor();
    void drawPills();

    void drawIcon(int icon, float cx, float cy, float size, SDL_Color color);
    [[nodiscard]] bool hover(const Rect& rect) const {
        return rect.contains(static_cast<float>(mouseX), static_cast<float>(mouseY));
    }

    [[nodiscard]] std::string currentMoveName() const {
        if (!activeChoreography()) {
            return "IDLE";
        }
        return moveName(sampleActiveAt(currentPosition()).move);
    }

    [[nodiscard]] std::pair<int, int> currentBarBeat() const {
        const AnalysisResult* analysis = activeAnalysis();
        if (!analysis || analysis->beats.empty()) {
            return {0, 0};
        }
        double offset = 0.0;
        const Beat* beat = previousBeat(analysis->beats, currentPosition(), &offset);
        if (!beat || offset > 2.5) {
            return {0, 0};
        }
        return {beat->barIndex + 1, beat->beatInBar + 1};
    }

    // capture and entry point
    int run();
    int runCapture();
    void saveCapture(const std::filesystem::path& path);
};

// -------------------------------------------------------------------------
// Rendering: background, header, toolbar
// -------------------------------------------------------------------------
void DesktopApp::Impl::drawBackground() {
    ui::fillRect(renderer, {0, 0, 1440, 900}, theme::background);
    ui::verticalGradient(renderer, {0, 0, 1440, 900}, SDL_Color{11, 15, 23, 255},
                         SDL_Color{5, 7, 11, 255});
    // A restrained technical grid.
    for (float x = 0; x < 1440; x += 72) {
        SDL_SetRenderDrawColor(renderer, 44, 58, 78, 18);
        SDL_RenderLine(renderer, x, 0, x, 900);
    }
    for (float y = 0; y < 900; y += 72) {
        SDL_SetRenderDrawColor(renderer, 44, 58, 78, 14);
        SDL_RenderLine(renderer, 0, y, 1440, y);
    }
    ui::fillCircle(renderer, 1300, 40, 420, theme::alpha(theme::accent, 0.025f));
    ui::fillCircle(renderer, 90, 850, 330, theme::alpha(theme::violet, 0.035f));
}

void DesktopApp::Impl::drawIcon(int icon, float cx, float cy, float size, SDL_Color color) {
    switch (icon) {
    case 0: { // folder
        const float w = size;
        const float h = size * 0.72f;
        ui::fillRoundedRect(renderer,
                            {cx - w * 0.5f, cy - h * 0.4f + 2.0f, w, h}, 2.0f, color);
        ui::fillRoundedRect(renderer,
                            {cx - w * 0.5f, cy - h * 0.6f, w * 0.36f, h * 0.35f}, 1.5f,
                            color);
        break;
    }
    case 1: // play
        ui::triangle(renderer, cx - size * 0.28f, cy - size * 0.38f,
                     cx - size * 0.28f, cy + size * 0.38f, cx + size * 0.42f, cy,
                     color);
        break;
    case 2: // pause
        ui::fillRoundedRect(renderer, {cx - size * 0.32f, cy - size * 0.34f,
                                       size * 0.22f, size * 0.68f},
                            1.5f, color);
        ui::fillRoundedRect(renderer, {cx + size * 0.1f, cy - size * 0.34f,
                                       size * 0.22f, size * 0.68f},
                            1.5f, color);
        break;
    case 3: // stop
        ui::fillRoundedRect(renderer, {cx - size * 0.28f, cy - size * 0.28f,
                                       size * 0.56f, size * 0.56f},
                            2.0f, color);
        break;
    case 4: // waveform
        for (int i = 0; i < 5; ++i) {
            const float height = size * (0.2f + 0.16f * (i % 3));
            ui::thickLine(renderer, cx - size * 0.4f + i * size * 0.2f, cy - height,
                          cx - size * 0.4f + i * size * 0.2f, cy + height, 2.0f,
                          color);
        }
        break;
    default:
        break;
    }
}

void DesktopApp::Impl::drawHeader() {
    // Logo.
    const Rect logo{24, 13, 38, 38};
    ui::fillRoundedRect(renderer, logo, 10.0f, theme::panelRaised);
    ui::strokeRoundedRect(renderer, logo, 10.0f, theme::borderBright, 1.0f);
    const float bars[] = {9.0f, 18.0f, 25.0f, 15.0f, 7.0f};
    for (int i = 0; i < 5; ++i) {
        const float bx = logo.x + 8.0f + i * 5.0f;
        ui::fillRoundedRect(renderer, {bx, logo.centerY() - bars[i] * 0.5f, 2.2f,
                                        bars[i]},
                            1.1f, i == 2 ? theme::accent : theme::alpha(theme::accent, 0.55f));
    }
    text->draw("BG MEDIA", 74, 13, 22, FontWeight::SemiBold, theme::text);
    label("MUSIC INTELLIGENCE ENGINE", 75, 41, 9, theme::dim, 1.6f);

    drawPills();
}

void DesktopApp::Impl::drawPills() {
    const float right = 1416.0f;
    // Module pills, intentionally compact.
    struct Pill {
        std::string name;
        SDL_Color color;
        float width;
    };
    const Pill pills[] = {
        {"BG BEAT", theme::violet, 72.0f},
        {"BG MOTION", theme::accent, 82.0f},
    };
    float x = right;
    for (int i = 1; i >= 0; --i) {
        const Pill& pill = pills[i];
        const Rect rect{x - pill.width, 18, pill.width, 26};
        ui::fillRoundedRect(renderer, rect, 13.0f, theme::alpha(pill.color, 0.09f));
        ui::strokeRoundedRect(renderer, rect, 13.0f, theme::alpha(pill.color, 0.30f), 1.0f);
        text->drawCentered(pill.name, rect.centerX(), rect.y + 7, 9, FontWeight::SemiBold,
                           theme::alpha(pill.color, 0.9f));
        x -= pill.width + 8.0f;
    }

    const std::string status = state == AppState::Loading
                                   ? "ANALYSING"
                                   : (state == AppState::Error
                                          ? "ERROR"
                                          : (state == AppState::Ready ? "READY" : "NO TRACK"));
    const SDL_Color statusColor = state == AppState::Error
                                      ? theme::red
                                      : (state == AppState::Loading ? theme::amber
                                                                    : theme::accent);
    const float statusWidth = 92.0f;
    const Rect statusRect{x - statusWidth - 10.0f, 18, statusWidth, 26};
    ui::fillRoundedRect(renderer, statusRect, 13.0f, theme::alpha(statusColor, 0.08f));
    ui::strokeRoundedRect(renderer, statusRect, 13.0f, theme::alpha(statusColor, 0.28f),
                          1.0f);
    ui::fillCircle(renderer, statusRect.x + 12.0f, statusRect.centerY(), 3.0f,
                  statusColor);
    text->draw(status, statusRect.x + 21.0f, statusRect.y + 7, 9, FontWeight::SemiBold,
               statusColor);
}

void DesktopApp::Impl::drawButton(const Rect& rect, std::string_view labelText,
                                  bool primary, bool active, int icon) {
    const bool isHover = hover(rect);
    SDL_Color fill = primary ? theme::accent : theme::panelRaised;
    if (primary) {
        fill = isHover ? theme::mix(theme::accent, theme::white, 0.12f)
                       : theme::accent;
    } else if (active) {
        fill = theme::alpha(theme::accent, 0.16f);
    } else if (isHover) {
        fill = theme::panelHover;
    }
    const SDL_Color content = primary ? SDL_Color{8, 18, 16, 255} : theme::text;
    ui::fillRoundedRect(renderer, rect, 8.0f, fill);
    if (!primary) {
        ui::strokeRoundedRect(renderer, rect, 8.0f,
                              active ? theme::alpha(theme::accent, 0.4f)
                                     : theme::border,
                              1.0f);
    }
    float iconX = rect.x + 18.0f;
    if (icon >= 0) {
        drawIcon(icon, iconX, rect.centerY(), 15.0f, content);
        iconX += 18.0f;
    }
    if (!labelText.empty()) {
        body(labelText, iconX + 6.0f, rect.y + (rect.h - 15.0f) * 0.5f - 1.0f, 11,
             content, FontWeight::SemiBold);
    }
}

void DesktopApp::Impl::drawToolbar() {
    const bool isPlaying = playing();
    drawButton(layout.loadButton, "LOAD", false, false, 0);

    drawButton(layout.playButton, "", false, isPlaying, isPlaying ? 2 : 1);
    drawButton(layout.stopButton, "", false, false, 3);
    drawButton(layout.rateButton,
               formatNumber(player.snapshot().rate, 2) + "x", false,
               std::abs(player.snapshot().rate - 1.0) > 0.01, 4);

    const double pos = currentPosition();
    const double dur = duration();
    text->draw(formatTime(pos, true), 342, 82, 26, FontWeight::SemiBold, theme::text);
    text->draw("/ " + formatTime(dur), 470, 92, 11, FontWeight::Regular, theme::muted);
    label("MEDIA TIME", 343, 111, 8, theme::dim, 1.4f);

    if (track) {
        const std::string name = ellipsize(*text, track->displayName, 380.0f, 15,
                                           FontWeight::SemiBold);
        body(name, 590, 82, 15, theme::text, FontWeight::SemiBold);
        const std::string path = ellipsize(*text, track->displayPath, 370.0f, 10,
                                           FontWeight::Regular);
        body(path, 590, 102, 10, theme::muted);
        label("CACHE " + cacheName(track->cacheStatus, track->fromCache), 975, 90,
              9, track->fromCache ? theme::accent : theme::dim, 1.1f);
    } else {
        body("No track loaded", 590, 87, 14, theme::muted);
        label("DROP WAV / MP3 / FLAC ANYWHERE", 590, 104, 9, theme::dim, 1.2f);
    }

    // Sync trim.
    label("SYNC TRIM", 1244, 72, 8, theme::dim, 1.4f);
    ui::fillRoundedRect(renderer, layout.syncMinus, 6.0f,
                        hover(layout.syncMinus) ? theme::panelHover : theme::panelRaised);
    ui::fillRoundedRect(renderer, layout.syncPlus, 6.0f,
                        hover(layout.syncPlus) ? theme::panelHover : theme::panelRaised);
    text->drawCentered("-", layout.syncMinus.centerX(), layout.syncMinus.y + 4, 16,
                       FontWeight::SemiBold, theme::muted);
    text->drawCentered("+", layout.syncPlus.centerX(), layout.syncPlus.y + 4, 16,
                       FontWeight::SemiBold, theme::muted);
    const std::string trim = std::string(latencyTrim >= 0.0 ? "+" : "") +
                             formatNumber(latencyTrim * 1000.0, 0) + " ms";
    text->drawCentered(trim, (layout.syncMinus.right() + layout.syncPlus.x) * 0.5f,
                       92, 11, FontWeight::SemiBold, theme::text);
}

// -------------------------------------------------------------------------
// Empty state
// -------------------------------------------------------------------------
void DesktopApp::Impl::drawEmptyState() {
    const Rect card = layout.empty;
    ui::fillRoundedRect(renderer, card, 16.0f, theme::panel);
    ui::strokeRoundedRect(renderer, card, 16.0f, theme::border, 1.0f);
    // Dashed inner drop area.
    const Rect drop{card.x + 22.0f, card.y + 22.0f, card.w - 44.0f, card.h - 44.0f};
    const SDL_Color dash = theme::alpha(theme::accent, 0.18f);
    ui::dashedLine(renderer, drop.x, drop.y, drop.right(), drop.y, 7.0f, 8.0f, dash);
    ui::dashedLine(renderer, drop.right(), drop.y, drop.right(), drop.bottom(), 7.0f,
                   8.0f, dash);
    ui::dashedLine(renderer, drop.right(), drop.bottom(), drop.x, drop.bottom(), 7.0f,
                   8.0f, dash);
    ui::dashedLine(renderer, drop.x, drop.bottom(), drop.x, drop.y, 7.0f, 8.0f, dash);

    const float centerX = card.centerX();
    const float top = card.y + 180.0f;
    // Animated equalizer mark.
    for (int i = 0; i < 9; ++i) {
        const float phase = static_cast<float>(animationTime) * 2.1f + i * 0.55f;
        const float height = 20.0f + 34.0f * (0.5f + 0.5f * std::sin(phase));
        const float x = centerX - 42.0f + i * 10.0f;
        ui::fillRoundedRect(renderer, {x, top - height + 24.0f, 4.0f, height}, 2.0f,
                            theme::alpha(i % 2 == 0 ? theme::accent : theme::violet, 0.8f));
    }

    text->drawCentered("DROP YOUR TRACK", centerX, top + 92, 36, FontWeight::SemiBold,
                       theme::text);
    text->drawCentered("BG Media reads the music before it moves anything.", centerX,
                       top + 140, 14, FontWeight::Regular, theme::muted);
    label("WAV   /   MP3   /   FLAC", centerX, top + 166, 10, theme::dim, 2.0f);

    // Empty load button.
    const Rect button = layout.emptyLoad;
    const bool isHover = hover(button);
    ui::fillRoundedRect(renderer, button, 9.0f,
                        isHover ? theme::mix(theme::accent, theme::white, 0.12f)
                                : theme::accent);
    drawIcon(0, button.x + 24.0f, button.centerY(), 16.0f, SDL_Color{8, 18, 16, 255});
    body("CHOOSE AUDIO", button.x + 44.0f, button.y + 14.0f, 11,
         SDL_Color{8, 18, 16, 255}, FontWeight::SemiBold);

    const float featureY = button.bottom() + 40.0f;
    const char* features[] = {"BEAT MAP", "BASS ACCENTS", "TEMPO CHANGE", "DANCE CUES",
                              "OFFLINE CACHE"};
    const float widths[] = {104.0f, 128.0f, 122.0f, 108.0f, 118.0f};
    float x = centerX - 305.0f;
    for (int i = 0; i < 5; ++i) {
        const Rect chip{x, featureY, widths[i], 28.0f};
        ui::fillRoundedRect(renderer, chip, 14.0f, theme::alpha(theme::violet, 0.08f));
        ui::strokeRoundedRect(renderer, chip, 14.0f, theme::alpha(theme::violet, 0.18f),
                              1.0f);
        text->drawCentered(features[i], chip.centerX(), chip.y + 8, 8,
                           FontWeight::SemiBold, theme::muted);
        x += widths[i] + 8.0f;
    }

    label("ALL ANALYSIS RUNS LOCALLY - NO NETWORK REQUESTS", centerX,
          featureY + 52.0f, 9, theme::dim, 1.25f);
}

// -------------------------------------------------------------------------
// Timeline
// -------------------------------------------------------------------------
void DesktopApp::Impl::drawRuler() {
    if (!track) {
        return;
    }
    const Rect ruler = layout.ruler;
    const double span = timelineSpan();
    double interval = 1.0;
    if (span > 600.0) {
        interval = 120.0;
    } else if (span > 300.0) {
        interval = 60.0;
    } else if (span > 120.0) {
        interval = 30.0;
    } else if (span > 60.0) {
        interval = 10.0;
    } else if (span > 20.0) {
        interval = 5.0;
    } else if (span > 8.0) {
        interval = 2.0;
    } else {
        interval = 1.0;
    }
    const double first = std::ceil(viewStart / interval) * interval;
    for (double t = first; t <= viewEnd + 1e-6; t += interval) {
        const float x = timeToX(t, layout.plot);
        SDL_SetRenderDrawColor(renderer, 66, 82, 105, 90);
        SDL_RenderLine(renderer, x, ruler.y + 11, x, layout.events.bottom());
        label(formatTime(t), x + 4.0f, ruler.y + 1.0f, 9, theme::dim, 0.2f);
    }
    labelRight(formatTime(viewStart) + " - " + formatTime(viewEnd), ruler.right(),
               ruler.y + 1.0f, 9, theme::dim);

    // Bar numbers replace time labels when zoomed close enough.
    const AnalysisResult* visibleAnalysis = activeAnalysis();
    if (visibleAnalysis && visibleAnalysis->meterConfidence >= 0.55f &&
        span <= 24.0 && !visibleAnalysis->beats.empty()) {
        for (const Beat& beat : visibleAnalysis->beats) {
            if (!beat.downbeat || beat.time < viewStart || beat.time > viewEnd) {
                continue;
            }
            const float x = timeToX(beat.time, layout.plot);
            label("BAR " + std::to_string(beat.barIndex + 1), x + 5.0f, ruler.y + 11.0f,
                  8, theme::alpha(theme::pink, 0.8f), 0.5f);
        }
    }
}

void DesktopApp::Impl::drawWaveform() {
    const Rect rect = layout.wave;
    ui::fillRoundedRect(renderer, rect, 5.0f, SDL_Color{10, 14, 22, 255});
    const float centerY = rect.centerY();
    SDL_SetRenderDrawColor(renderer, 50, 66, 88, 80);
    SDL_RenderLine(renderer, rect.x, centerY, rect.right(), centerY);
    if (!track || track->hiresWaveform.empty()) {
        return;
    }
    const auto& peaks = track->hiresWaveform;
    const double rate = track->hiresPeaksPerSecond;
    const int columns = static_cast<int>(rect.w);
    for (int px = 0; px < columns; ++px) {
        const double t0 = viewStart + (static_cast<double>(px) / columns) * timelineSpan();
        const double t1 = viewStart + (static_cast<double>(px + 1) / columns) * timelineSpan();
        std::size_t begin = static_cast<std::size_t>(std::max(0.0, t0 * rate));
        std::size_t end = static_cast<std::size_t>(std::max(0.0, t1 * rate));
        if (begin >= peaks.size()) {
            break;
        }
        end = std::min(peaks.size(), std::max(begin + 1, end));
        const std::size_t step = std::max<std::size_t>(1, (end - begin) / 10);
        float minValue = 0.0f;
        float maxValue = 0.0f;
        for (std::size_t i = begin; i < end; i += step) {
            minValue = std::min(minValue, peaks[i].minValue);
            maxValue = std::max(maxValue, peaks[i].maxValue);
        }
        const float amplitude = std::max(std::abs(minValue), std::abs(maxValue));
        const float y0 = centerY - maxValue * rect.h * 0.47f;
        const float y1 = centerY - minValue * rect.h * 0.47f;
        const float alpha = 0.35f + 0.55f * std::min(1.0f, amplitude * 2.0f);
        SDL_SetRenderDrawColor(renderer, 89, 231, 199,
                               static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255));
        SDL_RenderLine(renderer, rect.x + px, y0, rect.x + px, y1);
    }
    // A quiet top highlight.
    ui::fillRect(renderer, {rect.x, rect.y, rect.w, 1.0f},
                 theme::alpha(theme::accent, 0.08f));
}

void DesktopApp::Impl::drawSpectrogram() {
    const Rect rect = layout.spectrogram;
    ui::fillRoundedRect(renderer, rect, 4.0f, SDL_Color{8, 11, 17, 255});
    if (!spectrogramTexture || spectrogramWidth <= 0 || spectrogramHeight <= 0 ||
        !track || duration() <= 0.0) {
        return;
    }
    const float srcX = static_cast<float>(viewStart / duration()) * spectrogramWidth;
    float srcW = static_cast<float>(timelineSpan() / duration()) * spectrogramWidth;
    const float clampedX =
        std::clamp(srcX, 0.0f, static_cast<float>(spectrogramWidth) - 1.0f);
    srcW = std::clamp(srcW, 1.0f, static_cast<float>(spectrogramWidth) - clampedX);
    const SDL_FRect src{clampedX, 0.0f, srcW,
                        static_cast<float>(spectrogramHeight)};
    const SDL_FRect dst{rect.x, rect.y, rect.w, rect.h};
    SDL_RenderTexture(renderer, spectrogramTexture, &src, &dst);
    ui::fillRect(renderer, {rect.x, rect.y, rect.w, 1.0f},
                 theme::alpha(theme::white, 0.08f));
}

void DesktopApp::Impl::drawTempoStrip() {
    const Rect rect = layout.tempo;
    ui::fillRoundedRect(renderer, rect, 3.0f, SDL_Color{11, 15, 22, 255});
    if (!track) {
        return;
    }
    const auto& analysis = track->analysis;
    for (const TempoSegment& segment : analysis.tempoSegments) {
        const double start = std::max(segment.start, viewStart);
        const double end = std::min(segment.end, viewEnd);
        if (end <= start) {
            continue;
        }
        const float x0 = timeToX(start, layout.plot);
        const float x1 = timeToX(end, layout.plot);
        const Rect part{x0, rect.y + 2.0f, std::max(1.0f, x1 - x0), rect.h - 4.0f};
        ui::fillRoundedRect(renderer, part, 2.0f, theme::alpha(theme::violet, 0.22f));
        if (part.w > 58.0f) {
            label(formatNumber(segment.bpm, 1) + " BPM", part.x + 5.0f, part.y + 1.0f,
                  8, theme::alpha(theme::violet, 0.95f), 0.5f);
        }
    }
    for (const TempoTransition& transition : analysis.tempoTransitions) {
        const double start = std::max(transition.start, viewStart);
        const double end = std::min(transition.end, viewEnd);
        if (end <= start) {
            continue;
        }
        const float x0 = timeToX(start, layout.plot);
        const float x1 = timeToX(end, layout.plot);
        const Rect part{x0, rect.y + 2.0f, std::max(1.0f, x1 - x0), rect.h - 4.0f};
        ui::fillRoundedRect(renderer, part, 2.0f, theme::alpha(theme::amber, 0.16f));
        ui::dashedLine(renderer, x0, rect.y + 2.0f, x0, rect.bottom() - 2.0f, 3.0f, 3.0f,
                       theme::alpha(theme::amber, 0.7f));
        if (part.w > 44.0f) {
            label("TEMPO ?", part.x + 4.0f, part.y + 1.0f, 8,
                  theme::alpha(theme::amber, 0.9f), 0.5f);
        }
    }
    label("TEMPO", rect.x + 5.0f, rect.y - 12.0f, 8, theme::dim, 1.3f);
}

void DesktopApp::Impl::drawEvents() {
    const Rect rect = layout.events;
    ui::fillRoundedRect(renderer, rect, 4.0f, SDL_Color{9, 13, 20, 255});
    const float beatY = rect.y + 23.0f;
    const float accentY = rect.y + 59.0f;
    SDL_SetRenderDrawColor(renderer, 42, 56, 74, 100);
    SDL_RenderLine(renderer, rect.x, beatY, rect.right(), beatY);
    SDL_RenderLine(renderer, rect.x, accentY, rect.right(), accentY);

    if (!track) {
        return;
    }
    const auto& analysis = track->analysis;

    // Onsets as a faint texture.
    SDL_SetRenderDrawColor(renderer, 137, 151, 176, 55);
    int lastX = -100;
    for (const Onset& onset : analysis.onsets) {
        if (onset.time < viewStart || onset.time > viewEnd) {
            continue;
        }
        const int x = static_cast<int>(timeToX(onset.time, layout.plot));
        if (x == lastX) {
            continue;
        }
        lastX = x;
        SDL_RenderLine(renderer, static_cast<float>(x), rect.y + 7.0f,
                       static_cast<float>(x), rect.y + 13.0f);
    }

    // Beats. When meter phase is weak, do not paint downbeats as facts.
    const bool meterReliable = analysis.meterConfidence >= 0.55f;
    for (const Beat& beat : analysis.beats) {
        if (beat.time < viewStart || beat.time > viewEnd) {
            continue;
        }
        const float x = timeToX(beat.time, layout.plot);
        if (meterReliable && beat.downbeat) {
            SDL_SetRenderDrawColor(renderer, 255, 91, 157,
                                   beat.extrapolated ? 120 : 235);
            SDL_RenderLine(renderer, x, beatY - 14.0f, x, beatY + 14.0f);
            ui::fillCircle(renderer, x, beatY - 16.0f, 2.0f,
                           theme::alpha(theme::pink, 0.9f));
        } else {
            SDL_SetRenderDrawColor(renderer, 222, 232, 246,
                                   beat.extrapolated ? 70 : 170);
            const float height = beat.extrapolated ? 6.0f : 10.0f;
            SDL_RenderLine(renderer, x, beatY - height, x, beatY + height);
        }
    }

    // Bass accents.
    int lastAccentX = -100;
    for (const BassAccent& accent : analysis.bassAccents) {
        if (accent.time < viewStart || accent.time > viewEnd) {
            continue;
        }
        const int x = static_cast<int>(timeToX(accent.time, layout.plot));
        if (x == lastAccentX) {
            continue;
        }
        lastAccentX = x;
        ui::diamond(renderer, static_cast<float>(x), accentY, 3.2f,
                    theme::alpha(theme::amber, 0.85f));
    }

    label("BEAT", rect.x + 6.0f, rect.y + 5.0f, 7, theme::dim, 0.8f);
    label("BASS", rect.x + 6.0f, accentY - 4.0f, 7, theme::alpha(theme::amber, 0.8f),
          0.8f);
}

void DesktopApp::Impl::drawCursor() {
    if (!track) {
        return;
    }
    const double pos = currentPosition();
    if (pos < viewStart || pos > viewEnd) {
        return;
    }
    const float x = timeToX(pos, layout.plot);
    // Glow, then crisp line.
    ui::thickLine(renderer, x, layout.wave.y, x, layout.events.bottom(), 4.0f,
                  theme::alpha(theme::accent, 0.12f), false);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 230);
    SDL_RenderLine(renderer, x, layout.wave.y - 2.0f, x, layout.events.bottom());
    ui::triangle(renderer, x - 5.0f, layout.wave.y - 8.0f, x + 5.0f,
                 layout.wave.y - 8.0f, x, layout.wave.y + 1.0f, theme::white);

    // Time flag.
    const std::string time = formatTime(pos, true);
    const float width = std::max(54.0f, text->measure(time, 9, FontWeight::SemiBold) + 12.0f);
    Rect flag{x - width * 0.5f, layout.ruler.y - 1.0f, width, 16.0f};
    flag.x = std::clamp(flag.x, layout.ruler.x, layout.ruler.right() - width);
    ui::fillRoundedRect(renderer, flag, 3.0f, theme::white);
    text->drawCentered(time, flag.centerX(), flag.y + 3.0f, 9, FontWeight::SemiBold,
                       SDL_Color{12, 15, 20, 255});
}

void DesktopApp::Impl::drawTimeline() {
    const Rect card = layout.timelineCard;
    ui::fillRoundedRect(renderer, card, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, card, 12.0f, theme::border, 1.0f);

    text->draw("ANALYSIS TIMELINE", card.x + 18.0f, card.y + 14.0f, 12,
               FontWeight::SemiBold, theme::text);
    label("BG BEAT", card.x + 168.0f, card.y + 17.0f, 8, theme::violet, 1.1f);
    labelRight("WHEEL ZOOM  ·  DRAG TO SEEK  ·  MIDDLE-DRAG PAN  ·  DOUBLE-CLICK RESET",
               card.right() - 18.0f, card.y + 17.0f, 8, theme::dim, 0.6f);

    SDL_Rect clip{static_cast<int>(layout.ruler.x),
                  static_cast<int>(layout.ruler.y - 22.0f),
                  static_cast<int>(layout.ruler.w),
                  static_cast<int>(layout.events.bottom() - layout.ruler.y + 24.0f)};
    SDL_SetRenderClipRect(renderer, &clip);

    drawRuler();
    drawWaveform();
    drawSpectrogram();
    drawTempoStrip();
    drawEvents();

    // Silence overlays.
    if (track) {
        for (const SilenceRegion& silence : track->analysis.silences) {
            const double start = std::max(silence.start, viewStart);
            const double end = std::min(silence.end, viewEnd);
            if (end <= start) {
                continue;
            }
            const float x0 = timeToX(start, layout.plot);
            const float x1 = timeToX(end, layout.plot);
            ui::fillRect(renderer, {x0, layout.wave.y, x1 - x0,
                                    layout.events.bottom() - layout.wave.y},
                         SDL_Color{5, 8, 13, 170});
            ui::dashedLine(renderer, x0, layout.wave.y, x0, layout.events.bottom(), 4.0f,
                           4.0f, theme::alpha(theme::white, 0.22f));
            ui::dashedLine(renderer, x1, layout.wave.y, x1, layout.events.bottom(), 4.0f,
                           4.0f, theme::alpha(theme::white, 0.22f));
            if (x1 - x0 > 74.0f) {
                label("SILENCE", (x0 + x1) * 0.5f - 24.0f, layout.wave.y + 8.0f, 8,
                      theme::alpha(theme::white, 0.55f), 1.2f);
            }
        }
    }

    // Hover inspection.
    if (hoverTime >= viewStart && hoverTime <= viewEnd && !scrubbing) {
        const float x = timeToX(hoverTime, layout.plot);
        ui::dashedLine(renderer, x, layout.wave.y, x, layout.events.bottom(), 4.0f, 4.0f,
                       theme::alpha(theme::white, 0.22f));
    }
    if (scrubbing) {
        hoverTime = scrubPosition;
    }

    drawCursor();

    SDL_SetRenderClipRect(renderer, nullptr);

    // Legend.
    float legendX = card.x + 18.0f;
    const float legendY = card.y + 378.0f;
    auto legendItem = [&](SDL_Color color, const char* label, float width) {
        ui::fillCircle(renderer, legendX + 3.0f, legendY + 5.0f, 2.5f, color);
        body(label, legendX + 10.0f, legendY - 1.0f, 9, theme::dim);
        legendX += width;
    };
    legendItem(theme::pink, "DOWNBEAT", 82.0f);
    legendItem(theme::text, "BEAT", 56.0f);
    legendItem(theme::amber, "BASS", 54.0f);
    legendItem(theme::violet, "TEMPO", 62.0f);
    legendItem(theme::alpha(theme::white, 0.4f), "SILENCE", 72.0f);
    if (track) {
        labelRight("analysis " + formatMs(track->analysis.analysisSeconds * 1000.0) +
                       "  ·  " +
                       formatNumber(track->analysis.info.duration /
                                        std::max(0.001, track->analysis.analysisSeconds),
                                    0) +
                       "x realtime",
                   card.right() - 18.0f, legendY - 1.0f, 9, theme::dim, 0.3f);
    }
}

// -------------------------------------------------------------------------
// Right panels
// -------------------------------------------------------------------------
void DesktopApp::Impl::drawBpmPanel(const Rect& rect) {
    ui::fillRoundedRect(renderer, rect, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, rect, 12.0f, theme::border, 1.0f);
    label("DOMINANT TEMPO", rect.x + 18.0f, rect.y + 16.0f, 9, theme::dim, 1.4f);

    if (!track) {
        text->draw("--", rect.x + 18.0f, rect.y + 38.0f, 54, FontWeight::SemiBold,
                   theme::dim);
        body("Load a track to analyse its pulse.", rect.x + 18.0f, rect.y + 118.0f, 12,
             theme::muted);
        return;
    }
    const auto& analysis = track->analysis;
    const bool meterReliable = analysis.meterConfidence >= 0.55f;
    if (motionLabBpm > 0) {
        labelRight("MOTION LAB " + std::to_string(motionLabBpm), rect.right() - 18.0f,
                   rect.y + 16.0f, 8, theme::pink, 0.8f);
    } else if (!analysis.tempoStable) {
        labelRight("PERIOD VARIES", rect.right() - 18.0f, rect.y + 16.0f, 8,
                   theme::amber, 0.8f);
    } else if (analysis.octaveAmbiguous) {
        labelRight("METRIC AMBIGUOUS", rect.right() - 18.0f, rect.y + 16.0f, 8,
                   theme::amber, 0.8f);
    } else if (analysis.bpm > 0.0) {
        labelRight("PERIOD STABLE", rect.right() - 18.0f, rect.y + 16.0f, 8,
                   theme::muted, 0.8f);
    }

    if (!analysis.tempoStable) {
        text->draw("MIXED", rect.x + 18.0f, rect.y + 38.0f, 36, FontWeight::SemiBold,
                   theme::amber);
        body(formatNumber(analysis.bpm, 1) + " BPM weighted  ·  see tempo map",
             rect.x + 18.0f, rect.y + 86.0f, 11, theme::muted);
        body("Period confidence is heuristic, not a probability",
             rect.x + 18.0f, rect.y + 101.0f, 9, theme::dim);
        body("level " + formatNumber(analysis.bpmLevelConfidence * 100.0, 0) +
                 "%  ·  phase " +
                 formatNumber(analysis.beatPhaseConfidence * 100.0, 0) +
                 "%  ·  downbeat " +
                 formatNumber(analysis.downbeatConfidence * 100.0, 0) + "%",
             rect.x + 18.0f, rect.y + 117.0f, 9, theme::dim);
    } else if (analysis.bpm > 0.0) {
        text->draw(formatNumber(analysis.bpm, 1), rect.x + 18.0f, rect.y + 30.0f, 56,
                   FontWeight::SemiBold, theme::text);
        const float numberWidth =
            text->measure(formatNumber(analysis.bpm, 1), 56, FontWeight::SemiBold);
        label("BPM", rect.x + 28.0f + numberWidth, rect.y + 62.0f, 11, theme::muted,
              1.0f);
        body("beat " + formatNumber(60.0 / analysis.bpm, 3) + " s  ·  " +
                 (meterReliable
                      ? std::to_string(analysis.beatsPerBar) + "/4 meter"
                      : "meter uncertain"),
             rect.x + 18.0f, rect.y + 82.0f, 11, theme::muted);
        body("level " + formatNumber(analysis.bpmLevelConfidence * 100.0, 0) +
                 "%  ·  phase " +
                 formatNumber(analysis.beatPhaseConfidence * 100.0, 0) + "%",
             rect.x + 18.0f, rect.y + 99.0f, 9,
             analysis.octaveAmbiguous ? theme::amber : theme::dim);
        if (analysis.octaveAmbiguous) {
            std::string alternatives = "alt";
            int shown = 0;
            for (std::size_t i = 0;
                 i < analysis.bpmCandidates.size() &&
                 i < analysis.bpmCandidateScores.size() && shown < 2;
                 ++i) {
                if (std::abs(analysis.bpmCandidates[i] - analysis.bpm) < 0.5) {
                    continue;
                }
                alternatives += "  " + formatNumber(analysis.bpmCandidates[i], 1) +
                                " (" +
                                formatNumber(analysis.bpmCandidateScores[i] * 100.0, 0) +
                                "%)";
                ++shown;
            }
            if (shown > 0) {
                body(alternatives, rect.x + 18.0f, rect.y + 116.0f, 9,
                     theme::alpha(theme::amber, 0.9f));
            }
        }
    } else {
        text->draw("--", rect.x + 18.0f, rect.y + 34.0f, 54, FontWeight::SemiBold,
                   theme::dim);
        body("No reliable pulse detected", rect.x + 18.0f, rect.y + 92.0f, 11,
             theme::muted);
    }

    const float barX = rect.x + 18.0f;
    const float barY = rect.bottom() - 24.0f;
    const float barW = rect.w - 36.0f;
    label("HEURISTIC BPM SCORE", barX, barY - 14.0f, 8, theme::dim, 1.0f);
    labelRight(formatNumber(analysis.bpmConfidence * 100.0, 0) + "%", rect.right() - 18.0f,
               barY - 14.0f, 8, theme::muted, 0.6f);
    ui::fillRoundedRect(renderer, {barX, barY, barW, 4.0f}, 2.0f,
                        theme::alpha(theme::white, 0.08f));
    ui::fillRoundedRect(renderer, {barX, barY, barW * analysis.bpmConfidence, 4.0f}, 2.0f,
                        analysis.bpmConfidence > 0.55f ? theme::accent : theme::amber);
}

void DesktopApp::Impl::drawTempoPanel(const Rect& rect) {
    ui::fillRoundedRect(renderer, rect, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, rect, 12.0f, theme::border, 1.0f);
    label("TEMPO MAP", rect.x + 18.0f, rect.y + 16.0f, 9, theme::dim, 1.4f);
    if (!track) {
        body("--", rect.x + 18.0f, rect.y + 48.0f, 14, theme::dim);
        return;
    }
    const auto& analysis = track->analysis;
    const std::string count = std::to_string(analysis.tempoSegments.size()) +
                              (analysis.tempoSegments.size() == 1 ? " SEGMENT"
                                                                  : " SEGMENTS");
    labelRight(count, rect.right() - 18.0f, rect.y + 16.0f, 8,
               analysis.tempoStable ? theme::accent : theme::amber, 0.8f);

    if (analysis.octaveAmbiguous) {
        label("METRIC AMBIGUITY - ALTERNATIVES EXPOSED", rect.x + 18.0f,
              rect.y + 34.0f, 8, theme::alpha(theme::amber, 0.85f), 0.5f);
    }

    float y = rect.y + 56.0f;
    if (analysis.tempoSegments.empty()) {
        body("No stable tempo section", rect.x + 18.0f, y, 12, theme::muted);
    } else {
        const std::size_t maxRows =
            analysis.tempoTransitions.empty() ? 3u : 2u;
        for (std::size_t i = 0; i < analysis.tempoSegments.size() && i < maxRows; ++i) {
            const auto& segment = analysis.tempoSegments[i];
            ui::fillCircle(renderer, rect.x + 22.0f, y + 7.0f, 3.0f,
                           i == 0 ? theme::accent : theme::violet);
            body(formatTime(segment.start, true) + "  -  " +
                     formatTime(segment.end, true),
                 rect.x + 34.0f, y, 10, theme::muted);
            text->drawRight(formatNumber(segment.bpm, 1) + " BPM", rect.right() - 18.0f,
                            y - 2.0f, 13, FontWeight::SemiBold, theme::text);
            ui::fillRoundedRect(renderer, {rect.x + 34.0f, y + 17.0f, 150.0f, 3.0f},
                                1.5f, theme::alpha(theme::white, 0.06f));
            ui::fillRoundedRect(renderer, {rect.x + 34.0f, y + 17.0f,
                                           150.0f * segment.confidence, 3.0f},
                                1.5f, i == 0 ? theme::accent : theme::violet);
            y += 34.0f;
        }
    }

    if (!analysis.tempoTransitions.empty()) {
        const auto& transition = analysis.tempoTransitions.front();
        const Rect box{rect.x + 18.0f, rect.bottom() - 58.0f, rect.w - 36.0f, 32.0f};
        ui::fillRoundedRect(renderer, box, 5.0f, theme::alpha(theme::amber, 0.08f));
        ui::strokeRoundedRect(renderer, box, 5.0f, theme::alpha(theme::amber, 0.22f), 1.0f);
        label("TRANSITION  " + formatTime(transition.start, true) + "-" +
                  formatTime(transition.end, true),
              box.x + 8.0f, box.y + 6.0f, 8, theme::amber, 0.7f);
        labelRight(formatNumber(transition.fromBpm, 0) + " -> " +
                       formatNumber(transition.toBpm, 0) + "  uncertain",
                   box.right() - 8.0f, box.y + 6.0f, 8, theme::alpha(theme::amber, 0.8f),
                   0.4f);
    } else if (analysis.tempoStable) {
        label("No tempo change evidence", rect.x + 18.0f, rect.bottom() - 31.0f, 9,
              theme::dim, 0.5f);
    }
}

void DesktopApp::Impl::drawSignalPanel(const Rect& rect) {
    ui::fillRoundedRect(renderer, rect, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, rect, 12.0f, theme::border, 1.0f);
    label("SIGNAL EVENTS", rect.x + 18.0f, rect.y + 14.0f, 9, theme::dim, 1.4f);
    if (!track) {
        return;
    }
    const auto& analysis = track->analysis;
    int bars = 0;
    for (const auto& beat : analysis.beats) {
        bars = std::max(bars, beat.barIndex + 1);
    }
    const struct Stat {
        const char* label;
        std::string value;
        SDL_Color color;
    } stats[] = {
        {"BEATS", std::to_string(analysis.beats.size()), theme::text},
        {"BARS", std::to_string(bars) +
                     (analysis.meterConfidence >= 0.55f ? "" : "?"),
         analysis.meterConfidence >= 0.55f ? theme::text : theme::amber},
        {"ONSETS", std::to_string(analysis.onsets.size()), theme::muted},
        {"BASS", std::to_string(analysis.bassAccents.size()), theme::amber},
    };
    const float cellW = (rect.w - 36.0f) * 0.5f;
    for (int i = 0; i < 4; ++i) {
        const float x = rect.x + 18.0f + (i % 2) * cellW;
        const float y = rect.y + 40.0f + (i / 2) * 44.0f;
        body(stats[i].value, x, y, 22, stats[i].color, FontWeight::SemiBold);
        label(stats[i].label, x + 44.0f, y + 10.0f, 8, theme::dim, 1.1f);
    }
    label(std::to_string(analysis.silences.size()) + " silence region(s)  ·  " +
              formatNumber(analysis.activeRatio * 100.0, 0) + "% active",
          rect.x + 18.0f, rect.bottom() - 24.0f, 8, theme::dim, 0.5f);
}

void DesktopApp::Impl::drawAnalysisPanel(const Rect& rect) {
    ui::fillRoundedRect(renderer, rect, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, rect, 12.0f, theme::border, 1.0f);
    label("SYSTEM / CACHE", rect.x + 18.0f, rect.y + 14.0f, 9, theme::dim, 1.4f);
    if (!track) {
        body("BG Sync ready", rect.x + 18.0f, rect.y + 42.0f, 12, theme::muted);
        return;
    }
    const auto& info = track->analysis.info;
    const double realtime = info.duration /
                            std::max(0.001, track->analysis.analysisSeconds);
    struct Row {
        const char* name;
        std::string value;
        SDL_Color color;
    };
    const Row rows[] = {
        {"ANALYSIS",
         formatMs(track->analysis.analysisSeconds * 1000.0) + "  ·  " +
             formatNumber(realtime, 0) + "x RT",
         theme::text},
        {"CACHE", cacheName(track->cacheStatus, track->fromCache) + "  ·  " +
                      formatMs(track->cacheMs),
         track->fromCache ? theme::accent : theme::muted},
        {"SOURCE", std::to_string(info.sampleRate) + " Hz  ·  " +
                       std::to_string(info.sourceChannels) + " ch",
         theme::muted},
        {"OUTPUT", player.audioAvailable()
                       ? formatNumber((player.baseLatency() + latencyTrim) * 1000.0, 0) +
                             " ms latency  ·  48 kHz"
                       : (audioError.empty() ? "not initialised" : "unavailable"),
         player.audioAvailable() ? theme::accent : theme::amber},
    };
    float y = rect.y + 38.0f;
    for (const Row& row : rows) {
        label(row.name, rect.x + 18.0f, y + 2.0f, 8, theme::dim, 1.0f);
        text->drawRight(row.value, rect.right() - 18.0f, y - 1.0f, 10,
                        FontWeight::SemiBold, row.color);
        y += 26.0f;
    }
    label("MEDIA-TIME CLOCK  ·  FPS INDEPENDENT", rect.x + 18.0f, rect.bottom() - 18.0f,
          8, theme::alpha(theme::accent, 0.7f), 0.6f);
}

void DesktopApp::Impl::drawRightPanels() {
    drawBpmPanel(layout.right1);
    drawTempoPanel(layout.right2);
    drawSignalPanel(layout.right3);
    drawAnalysisPanel(layout.right4);
}

// -------------------------------------------------------------------------
// Choreography card and dancer
// -------------------------------------------------------------------------
namespace {

struct JointPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct DancerSkeleton {
    JointPoint hip;
    JointPoint shoulder;
    JointPoint head;
    JointPoint leftShoulder;
    JointPoint rightShoulder;
    JointPoint leftElbow;
    JointPoint rightElbow;
    JointPoint leftHand;
    JointPoint rightHand;
    JointPoint leftKnee;
    JointPoint rightKnee;
    JointPoint leftFoot;
    JointPoint rightFoot;
    float floorY = 0.0f;
};

JointPoint solveKnee(JointPoint hip, JointPoint foot, float thigh, float shin,
                     float sideSign) {
    const float dx = foot.x - hip.x;
    const float dy = foot.y - hip.y;
    const float distance = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
    const float clamped = std::clamp(distance, std::abs(thigh - shin) + 0.01f,
                                     thigh + shin - 0.01f);
    const float ux = dx / distance;
    const float uy = dy / distance;
    const float along = (thigh * thigh - shin * shin + clamped * clamped) /
                        (2.0f * clamped);
    const float bend = std::sqrt(std::max(0.0f, thigh * thigh - along * along));
    const float baseX = hip.x + ux * along;
    const float baseY = hip.y + uy * along;
    const float perpX = -uy;
    const float perpY = ux;
    return {baseX + perpX * bend * sideSign, baseY + perpY * bend * sideSign};
}

DancerSkeleton makeDancerSkeleton(const Rect& stage, const CharacterPose& pose) {
    DancerSkeleton skeleton;
    const float cx = stage.centerX();
    const float floorY = stage.y + stage.h - 28.0f;
    skeleton.floorY = floorY;
    const float bob = -pose.bob * 13.0f;
    const float hipX = cx + pose.lean * 9.0f + pose.hipShift * 12.0f;
    const float hipY = floorY - 66.0f + bob;
    const float shoulderX = cx + pose.lean * 23.0f + pose.hipShift * 8.0f;
    const float shoulderY = hipY - 62.0f;
    const float headX = shoulderX + pose.lean * 9.0f + pose.headTurn * 8.0f;
    const float headY = shoulderY - 34.0f + pose.headNod * 7.0f;
    const float twist = pose.chestTwist * 10.0f;

    skeleton.hip = {hipX, hipY};
    skeleton.shoulder = {shoulderX, shoulderY};
    skeleton.head = {headX, headY};
    skeleton.leftShoulder = {shoulderX - 18.0f + twist, shoulderY + 6.0f};
    skeleton.rightShoulder = {shoulderX + 18.0f - twist, shoulderY + 6.0f};

    const float footLiftLeft = std::max(0.0f, pose.footLeft) * 9.0f;
    const float footLiftRight = std::max(0.0f, pose.footRight) * 9.0f;
    skeleton.leftFoot = {cx - 24.0f, floorY + 2.0f - footLiftLeft};
    skeleton.rightFoot = {cx + 24.0f, floorY + 2.0f - footLiftRight};
    // Bend each knee away from the body's centre line. The foot anchors are
    // fixed; only the knee solve changes with bob/dip.
    skeleton.leftKnee =
        solveKnee(skeleton.hip, skeleton.leftFoot, 35.0f, 39.0f, 1.0f);
    skeleton.rightKnee =
        solveKnee(skeleton.hip, skeleton.rightFoot, 35.0f, 39.0f, -1.0f);

    const float leftReach = pose.armLeft * 1.05f;
    const float rightReach = pose.armRight * 1.05f;
    skeleton.leftElbow = {
        skeleton.leftShoulder.x + std::sin(leftReach - 0.35f) * 31.0f,
        skeleton.leftShoulder.y + std::cos(leftReach - 0.35f) * 31.0f};
    skeleton.rightElbow = {
        skeleton.rightShoulder.x + std::sin(rightReach + 0.35f) * 31.0f,
        skeleton.rightShoulder.y + std::cos(rightReach + 0.35f) * 31.0f};
    skeleton.leftHand = {
        skeleton.leftElbow.x + std::sin(leftReach - 0.35f + pose.elbowLeft * 0.6f) * 27.0f,
        skeleton.leftElbow.y + std::cos(leftReach - 0.35f + pose.elbowLeft * 0.6f) * 27.0f};
    skeleton.rightHand = {
        skeleton.rightElbow.x + std::sin(rightReach + 0.35f + pose.elbowRight * 0.6f) * 27.0f,
        skeleton.rightElbow.y + std::cos(rightReach + 0.35f + pose.elbowRight * 0.6f) * 27.0f};
    return skeleton;
}

void drawJoint(SDL_Renderer* renderer, JointPoint point, SDL_Color color,
               float radius) {
    ui::fillCircle(renderer, point.x, point.y, radius, color);
    ui::ring(renderer, point.x, point.y, radius + 1.5f, 1.0f,
             theme::alpha(theme::white, 0.22f));
}

} // namespace

void DesktopApp::Impl::drawDancer(const Rect& stage, const CharacterPose& pose) {
    const float cx = stage.centerX();
    const float floorY = stage.y + stage.h - 28.0f;

    // Stage floor and depth lines.
    ui::thickLine(renderer, cx - 150.0f, floorY + 10.0f, cx + 150.0f, floorY + 10.0f,
                  1.0f, theme::alpha(theme::white, 0.08f), false);
    ui::thickLine(renderer, cx - 80.0f, floorY + 18.0f, cx + 80.0f, floorY + 18.0f,
                  1.0f, theme::alpha(theme::white, 0.04f), false);
    ui::fillCircle(renderer, cx, floorY + 13.0f, 44.0f,
                   theme::alpha(theme::accent, 0.035f));

    // A deterministic trail pass is drawn before the character, so debug
    // trajectories never obscure the pose itself. It samples media time
    // directly, not rendered frames.
    if (motionDebug) {
        const Choreography* motion = activeChoreography();
        if (motion) {
            const double pos = currentPosition();
            constexpr int kTrailSamples = 52;
            constexpr double kTrailSpan = 1.15;
            for (int sampleIndex = 0; sampleIndex < kTrailSamples; ++sampleIndex) {
                const double u =
                    static_cast<double>(sampleIndex) / (kTrailSamples - 1);
                const double sampleTime = pos - kTrailSpan + kTrailSpan * u;
                if (sampleTime < 0.0) {
                    continue;
                }
                const CharacterPose trailPose = sampleActiveAt(sampleTime);
                const DancerSkeleton trail = makeDancerSkeleton(stage, trailPose);
                if (sampleIndex > 0) {
                    const double previousU =
                        static_cast<double>(sampleIndex - 1) / (kTrailSamples - 1);
                    const CharacterPose previousPose =
                        sampleActiveAt(pos - kTrailSpan + kTrailSpan * previousU);
                    const DancerSkeleton previous =
                        makeDancerSkeleton(stage, previousPose);
                    const float alpha = static_cast<float>(0.20 + 0.65 * u);
                    ui::thickLine(renderer, previous.leftHand.x, previous.leftHand.y,
                                  trail.leftHand.x, trail.leftHand.y, 2.0f,
                                  theme::alpha(theme::violet, alpha), false);
                    ui::thickLine(renderer, previous.rightHand.x, previous.rightHand.y,
                                  trail.rightHand.x, trail.rightHand.y, 2.0f,
                                  theme::alpha(theme::pink, alpha), false);
                    ui::thickLine(renderer, previous.head.x, previous.head.y,
                                  trail.head.x, trail.head.y, 1.6f,
                                  theme::alpha(theme::accent, alpha * 0.8f), false);
                }
                if (sampleIndex % 8 == 0) {
                    const float markerAlpha =
                        static_cast<float>(0.25 + 0.5 * u);
                    ui::fillCircle(renderer, trail.leftHand.x, trail.leftHand.y, 1.8f,
                                   theme::alpha(theme::violet, markerAlpha));
                    ui::fillCircle(renderer, trail.rightHand.x, trail.rightHand.y, 1.8f,
                                   theme::alpha(theme::pink, markerAlpha));
                }
                if (sampleIndex == kTrailSamples - 1) {
                    ui::fillCircle(renderer, trail.leftHand.x, trail.leftHand.y, 3.0f,
                                   theme::violet);
                    ui::fillCircle(renderer, trail.rightHand.x, trail.rightHand.y, 3.0f,
                                   theme::pink);
                }
            }
        }
    }

    // Beat pulse ring, driven by media time.
    const double pos = currentPosition();
    const AnalysisResult* active = activeAnalysis();
    if (active && !active->beats.empty()) {
        double nearest = 1e9;
        for (const Beat& beat : active->beats) {
            nearest = std::min(nearest, std::abs(beat.time - pos));
        }
        if (nearest < 0.16) {
            const float phase = static_cast<float>(nearest / 0.16);
            ui::ring(renderer, cx, floorY + 8.0f, 34.0f + phase * 52.0f, 1.0f,
                     theme::alpha(theme::accent, 0.24f * (1.0f - phase)));
        }
    }

    if (pose.energy > 0.05f) {
        ui::fillCircle(renderer, cx, floorY - 92.0f, 86.0f,
                       theme::alpha(pose.accented ? theme::amber : theme::accent,
                                    0.035f + 0.03f * pose.energy));
    }

    const DancerSkeleton skeleton = makeDancerSkeleton(stage, pose);
    const SDL_Color limbDark{17, 24, 34, 255};
    const SDL_Color limbColor = pose.move == MoveKind::Freeze
                                    ? theme::muted
                                    : (pose.accented ? theme::amber : theme::accent);
    const SDL_Color jointColor = theme::violet;
    const float thickness = 11.0f;

    // Shadow anchored to the planted feet.
    ui::thickLine(renderer, skeleton.leftFoot.x - 7.0f, floorY + 4.0f,
                  skeleton.rightFoot.x + 7.0f, floorY + 4.0f, 7.0f,
                  SDL_Color{0, 0, 0, 90}, false);

    // Legs use a two-bone IK solve with planted feet, so bob/dip/weight shifts
    // bend the knees instead of sliding the feet across the floor.
    for (int side = 0; side < 2; ++side) {
        const JointPoint knee = side == 0 ? skeleton.leftKnee : skeleton.rightKnee;
        const JointPoint foot = side == 0 ? skeleton.leftFoot : skeleton.rightFoot;
        ui::thickLine(renderer, skeleton.hip.x, skeleton.hip.y, knee.x, knee.y,
                      thickness + 4.0f, limbDark);
        ui::thickLine(renderer, knee.x, knee.y, foot.x, foot.y, thickness + 4.0f,
                      limbDark);
        ui::thickLine(renderer, skeleton.hip.x, skeleton.hip.y, knee.x, knee.y,
                      thickness, limbColor);
        ui::thickLine(renderer, knee.x, knee.y, foot.x, foot.y, thickness, limbColor);
        ui::fillCircle(renderer, knee.x, knee.y, thickness * 0.58f, jointColor);
        ui::fillCircle(renderer, foot.x, foot.y, 6.0f, jointColor);
    }

    // Arms with articulated elbows.
    for (int side = 0; side < 2; ++side) {
        const JointPoint shoulder =
            side == 0 ? skeleton.leftShoulder : skeleton.rightShoulder;
        const JointPoint elbow = side == 0 ? skeleton.leftElbow : skeleton.rightElbow;
        const JointPoint hand = side == 0 ? skeleton.leftHand : skeleton.rightHand;
        ui::thickLine(renderer, shoulder.x, shoulder.y, elbow.x, elbow.y,
                      thickness - 1.0f, limbDark);
        ui::thickLine(renderer, elbow.x, elbow.y, hand.x, hand.y, thickness - 1.0f,
                      limbDark);
        ui::thickLine(renderer, shoulder.x, shoulder.y, elbow.x, elbow.y,
                      thickness - 4.0f, limbColor);
        ui::thickLine(renderer, elbow.x, elbow.y, hand.x, hand.y, thickness - 4.0f,
                      limbColor);
        ui::fillCircle(renderer, elbow.x, elbow.y, thickness * 0.5f, jointColor);
        ui::fillCircle(renderer, hand.x, hand.y, 4.5f, limbColor);
    }

    // Torso, chest light and head.
    ui::thickLine(renderer, skeleton.hip.x, skeleton.hip.y + 2.0f,
                  skeleton.shoulder.x, skeleton.shoulder.y, 41.0f, limbDark, true);
    ui::thickLine(renderer, skeleton.hip.x, skeleton.hip.y + 2.0f,
                  skeleton.shoulder.x, skeleton.shoulder.y, 34.0f,
                  SDL_Color{22, 30, 43, 255}, true);
    ui::fillCircle(renderer, skeleton.shoulder.x, skeleton.shoulder.y, 4.0f,
                   limbColor);
    ui::fillCircle(renderer, skeleton.hip.x, skeleton.hip.y, 4.0f, limbColor);
    const float chestX = (skeleton.hip.x + skeleton.shoulder.x) * 0.5f;
    const float chestY = (skeleton.hip.y + skeleton.shoulder.y) * 0.5f - 2.0f;
    ui::fillCircle(renderer, chestX, chestY, 4.0f,
                   theme::alpha(pose.accented ? theme::amber : theme::accent, 0.9f));

    ui::fillCircle(renderer, skeleton.head.x, skeleton.head.y, 25.0f, limbDark);
    ui::ring(renderer, skeleton.head.x, skeleton.head.y, 25.0f, 1.5f,
             theme::alpha(theme::white, 0.18f));
    ui::fillRoundedRect(renderer,
                        {skeleton.head.x - 15.0f + pose.headTurn * 5.0f,
                         skeleton.head.y - 4.0f + pose.headNod * 2.0f, 30.0f, 10.0f},
                        5.0f, theme::alpha(theme::accent, 0.85f));
    ui::fillCircle(renderer, skeleton.head.x + 8.0f + pose.headTurn * 4.0f,
                   skeleton.head.y + 1.0f + pose.headNod * 2.0f, 1.8f,
                   SDL_Color{8, 20, 18, 255});
    ui::thickLine(renderer, skeleton.head.x, skeleton.head.y - 25.0f,
                  skeleton.head.x + pose.lean * 4.0f, skeleton.head.y - 37.0f,
                  2.0f, theme::alpha(theme::muted, 0.8f));
    ui::fillCircle(renderer, skeleton.head.x + pose.lean * 4.0f,
                   skeleton.head.y - 38.0f, 2.4f,
                   pose.accented ? theme::amber : theme::accent);

    if (motionDebug) {
        const SDL_Color debugJoint = theme::alpha(theme::accent, 0.95f);
        drawJoint(renderer, skeleton.hip, debugJoint, 2.6f);
        drawJoint(renderer, skeleton.shoulder, debugJoint, 2.6f);
        drawJoint(renderer, skeleton.head, theme::pink, 2.8f);
        drawJoint(renderer, skeleton.leftElbow, theme::violet, 2.2f);
        drawJoint(renderer, skeleton.rightElbow, theme::violet, 2.2f);
        drawJoint(renderer, skeleton.leftKnee, theme::amber, 2.2f);
        drawJoint(renderer, skeleton.rightKnee, theme::amber, 2.2f);
        drawJoint(renderer, skeleton.leftFoot, theme::accent, 2.4f);
        drawJoint(renderer, skeleton.rightFoot, theme::accent, 2.4f);

        // Keyframe/accent ruler at the bottom edge of the stage.
        const float stripY = stage.bottom() - 13.0f;
        ui::thickLine(renderer, stage.x + 18.0f, stripY, stage.right() - 18.0f, stripY,
                      1.0f, theme::alpha(theme::white, 0.14f), false);
        const auto* activeData = activeAnalysis();
        const Choreography* activeMotion = activeChoreography();
        if (activeMotion) {
            for (const MotionKeyframe& key : activeMotion->keyframes) {
                if (std::abs(key.time - pos) > 1.2) {
                    continue;
                }
                const float x = stage.centerX() +
                                static_cast<float>((key.time - pos) / 1.2) *
                                    (stage.w * 0.45f);
                SDL_SetRenderDrawColor(renderer, 156, 130, 255, 125);
                SDL_RenderLine(renderer, x, stripY + 1.0f, x, stripY + 4.0f);
            }
        }
        if (activeData) {
            for (const Beat& beat : activeData->beats) {
                if (std::abs(beat.time - pos) > 1.2) {
                    continue;
                }
                const float x = stage.centerX() +
                                static_cast<float>((beat.time - pos) / 1.2) *
                                    (stage.w * 0.45f);
                const bool nearAccent =
                    beat.downbeat ||
                    std::any_of(activeData->bassAccents.begin(),
                                activeData->bassAccents.end(),
                                [&](const BassAccent& accent) {
                                    return std::abs(accent.time - beat.time) < 0.08;
                                });
                SDL_SetRenderDrawColor(renderer, nearAccent ? 255 : 150,
                                       nearAccent ? 181 : 180,
                                       nearAccent ? 77 : 205,
                                       nearAccent ? 220 : 100);
                SDL_RenderLine(renderer, x, stripY - 5.0f, x, stripY + 4.0f);
            }
        }
    }
}

void DesktopApp::Impl::drawChoreography() {
    const Rect card = layout.choreoCard;
    ui::fillRoundedRect(renderer, card, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, card, 12.0f, theme::border, 1.0f);
    text->draw("CHOREOGRAPHY", card.x + 18.0f, card.y + 14.0f, 12,
               FontWeight::SemiBold, theme::text);
    label("BG MOTION", card.x + 150.0f, card.y + 17.0f, 8, theme::accent, 1.1f);
    label("MEDIA-TIME  ·  FPS INDEPENDENT", card.x + 248.0f, card.y + 17.0f, 8,
          theme::dim, 0.5f);

    auto motionChip = [&](const Rect& rect, const std::string& caption, bool selected,
                          SDL_Color color) {
        ui::fillRoundedRect(renderer, rect, 12.0f,
                            theme::alpha(color, selected ? 0.14f : 0.055f));
        ui::strokeRoundedRect(renderer, rect, 12.0f,
                              theme::alpha(color, selected ? 0.38f : 0.16f), 1.0f);
        text->drawCentered(caption, rect.centerX(), rect.y + 7.0f, 8,
                           FontWeight::SemiBold,
                           selected ? color : theme::muted);
    };
    motionChip(layout.motionSmooth, motionStepped ? "STEPPED" : "SMOOTH",
               true, motionStepped ? theme::amber : theme::accent);
    motionChip(layout.motionDebug, "DEBUG", motionDebug, theme::violet);
    motionChip(layout.motionLab,
               motionLabBpm > 0 ? "LAB " + std::to_string(motionLabBpm) : "LAB OFF",
               motionLabBpm > 0, theme::pink);

    const Rect stage{card.x + 18.0f, card.y + 46.0f, 420.0f, 224.0f};
    ui::fillRoundedRect(renderer, stage, 8.0f, theme::panelRaised);
    ui::strokeRoundedRect(renderer, stage, 8.0f, theme::border, 1.0f);
    // Stage grid.
    for (int i = 0; i < 5; ++i) {
        const float y = stage.y + 44.0f + i * 40.0f;
        ui::thickLine(renderer, stage.x + 20.0f, y, stage.right() - 20.0f, y, 1.0f,
                      theme::alpha(theme::white, 0.025f), false);
    }

    CharacterPose pose;
    MotionSample motionSample;
    if (activeChoreography()) {
        motionSample = sampleActiveWithDebug(currentPosition());
        pose = motionSample.pose;
    }
    drawDancer(stage, pose);

    // Details to the right of the stage.
    const float x = stage.right() + 28.0f;
    float y = card.y + 52.0f;
    label("ACTIVE MOVE", x, y, 8, theme::dim, 1.3f);
    y += 15.0f;
    const std::string move = moveName(pose.move);
    text->draw(move, x, y, 27, FontWeight::SemiBold,
               pose.accented ? theme::amber : theme::text);
    y += 39.0f;
    const auto [bar, beat] = currentBarBeat();
    const AnalysisResult* currentAnalysis = activeAnalysis();
    const bool meterReliable =
        currentAnalysis && currentAnalysis->meterConfidence >= 0.55f;
    body("Bar " + std::to_string(bar) + (meterReliable ? "" : "?") +
             "   ·   Beat " + std::to_string(beat) +
             (currentAnalysis ? "/" + std::to_string(currentAnalysis->beatsPerBar) : "") +
             (meterReliable ? "" : " ?"),
         x, y, 13, meterReliable ? theme::muted : theme::amber);
    y += 24.0f;

    label("INTENSITY", x, y, 8, theme::dim, 1.2f);
    y += 14.0f;
    ui::fillRoundedRect(renderer, {x, y, 230.0f, 4.0f}, 2.0f,
                        theme::alpha(theme::white, 0.07f));
    ui::fillRoundedRect(renderer, {x, y, 230.0f * pose.energy, 4.0f}, 2.0f,
                        pose.accented ? theme::amber : theme::accent);
    y += 24.0f;

    if (pose.accented) {
        const Rect accent{x, y, 126.0f, 22.0f};
        ui::fillRoundedRect(renderer, accent, 11.0f, theme::alpha(theme::amber, 0.1f));
        ui::strokeRoundedRect(renderer, accent, 11.0f, theme::alpha(theme::amber, 0.3f),
                              1.0f);
        ui::diamond(renderer, accent.x + 12.0f, accent.centerY(), 3.0f, theme::amber);
        body("BASS ACCENT", accent.x + 22.0f, accent.y + 5.0f, 8, theme::amber,
             FontWeight::SemiBold);
    }
    y += 34.0f;

    label("NEXT CUES", x, y, 8, theme::dim, 1.3f);
    y += 16.0f;
    const Choreography* motion = activeChoreography();
    if (motion) {
        int shown = 0;
        const double pos = currentPosition();
        for (const DanceCue& cue : motion->cues) {
            if (cue.time <= pos + 0.02) {
                continue;
            }
            if (shown >= 3) {
                break;
            }
            ui::fillCircle(renderer, x + 3.0f, y + 7.0f, 2.2f,
                           cue.bassAccent ? theme::amber : theme::violet);
            body(formatTime(cue.time, true), x + 12.0f, y, 10, theme::muted);
            body(moveName(cue.move), x + 82.0f, y, 10, theme::text);
            y += 19.0f;
            ++shown;
        }
        if (shown == 0) {
            body("End of choreography", x, y, 10, theme::dim);
        }
    } else {
        body("Waiting for a track", x, y, 10, theme::dim);
    }

    const float footerY = card.bottom() - 24.0f;
    ui::thickLine(renderer, x, footerY - 12.0f, card.right() - 18.0f, footerY - 12.0f,
                  1.0f, theme::alpha(theme::white, 0.05f), false);
    label("CURSOR", x, footerY, 8, theme::dim, 1.1f);
    body(formatTime(currentPosition(), true), x + 54.0f, footerY - 2.0f, 10,
         theme::text);
    label("LATENCY", x + 132.0f, footerY, 8, theme::dim, 1.1f);
    body(player.audioAvailable()
             ? formatNumber((player.baseLatency() + latencyTrim) * 1000.0, 0) + " ms"
             : "--",
         x + 188.0f, footerY - 2.0f, 10, theme::muted);
    label("MOTION", x + 250.0f, footerY, 8, theme::dim, 1.1f);
    body(motionStepped ? "STEPPED" : "SMOOTH", x + 302.0f, footerY - 2.0f, 10,
         motionStepped ? theme::amber : theme::accent,
         FontWeight::SemiBold);

    if (motionDebug) {
        // A compact diagnostics strip over the stage. It deliberately uses
        // media time and sampler output, never a frame counter.
        const float dx = stage.x + 8.0f;
        float dy = stage.y + 7.0f;
        auto debugLine = [&](const std::string& lineText) {
            const Rect line{dx - 4.0f, dy - 2.0f,
                            text->measure(lineText, 8, FontWeight::SemiBold) + 10.0f,
                            13.0f};
            ui::fillRoundedRect(renderer, line, 3.0f, theme::alpha(theme::background, 0.9f));
            body(lineText, dx, dy, 8, theme::alpha(theme::accent, 0.9f));
            dy += 13.0f;
        };
        debugLine("t " + formatTime(currentPosition(), true) + "   phase " +
                  formatNumber(pose.phase, 2) + "   impact " +
                  formatNumber(pose.impact, 2));
        debugLine("key " + std::to_string(motionSample.keyframeIndex) + " -> " +
                  std::to_string(motionSample.nextKeyframeIndex) + "   blend " +
                  formatNumber(motionSample.segmentT, 2) + "   movement " +
                  formatNumber(pose.blend, 2));
        debugLine("easing " + std::string(motionSample.easing == Easing::Cubic
                                             ? "cubic"
                                             : "eased") +
                  "   " + moveName(pose.move));
    }
}

// -------------------------------------------------------------------------
// Footer and overlays
// -------------------------------------------------------------------------
void DesktopApp::Impl::drawFooter() {
    const Rect bar = layout.footer;
    ui::fillRect(renderer, bar, SDL_Color{6, 8, 13, 255});
    SDL_SetRenderDrawColor(renderer, 31, 40, 55, 255);
    SDL_RenderLine(renderer, 0, bar.y, 1440, bar.y);

    SDL_Color dot = theme::dim;
    std::string status;
    if (state == AppState::Loading) {
        dot = theme::amber;
        status = "ANALYSING IN BACKGROUND";
    } else if (state == AppState::Error) {
        dot = theme::red;
        status = "ANALYSIS ERROR";
    } else if (!track) {
        dot = theme::dim;
        status = "NO TRACK";
    } else {
        dot = audioError.empty() ? theme::accent : theme::amber;
        status = audioError.empty() ? "READY" : "ANALYSIS READY  ·  AUDIO UNAVAILABLE";
    }
    ui::fillCircle(renderer, 24.0f, bar.centerY(), 3.2f, dot);
    label(status, 34.0f, bar.y + 15.0f, 8, theme::muted, 1.1f);
    if (track) {
        body(std::to_string(track->analysis.info.sampleRate) + " Hz  ·  " +
                 std::to_string(track->analysis.info.sourceChannels) + " ch  ·  " +
                 formatTime(track->analysis.info.duration),
             190.0f, bar.y + 13.0f, 9, theme::dim);
    }

    text->drawCentered("SPACE PLAY/PAUSE   ←/→ 5s   WHEEL ZOOM   D DEBUG   "
                       "V A/B   B LAB   R RATE",
                       720.0f, bar.y + 13.0f, 8, FontWeight::SemiBold,
                       theme::alpha(theme::muted, 0.75f), 1.0f, 0.2f);
    labelRight("BG MEDIA v" BGMEDIA_VERSION_STRING "   ·   OFFLINE / DETERMINISTIC",
               1416.0f, bar.y + 15.0f, 8, theme::dim, 0.6f);
}

void DesktopApp::Impl::drawOverlays() {
    if (state == AppState::Loading) {
        ui::fillRect(renderer, {0, 64, 1440, 792}, SDL_Color{5, 7, 11, 170});
        const Rect card{440, 330, 560, 220};
        ui::fillRoundedRect(renderer, card, 14.0f, theme::panel);
        ui::strokeRoundedRect(renderer, card, 14.0f, theme::borderBright, 1.0f);
        // Spinner.
        const float cx = card.x + 48.0f;
        const float cy = card.y + 54.0f;
        for (int i = 0; i < 18; ++i) {
            const float a = static_cast<float>(animationTime) * 3.2f + i * 0.35f;
            const float alpha = 0.1f + 0.9f * (static_cast<float>(i) / 18.0f);
            const float r0 = 13.0f;
            const float r1 = 18.0f;
            SDL_SetRenderDrawColor(renderer, 83, 231, 196,
                                   static_cast<Uint8>(alpha * 255));
            SDL_RenderLine(renderer, cx + std::cos(a) * r0, cy + std::sin(a) * r0,
                           cx + std::cos(a) * r1, cy + std::sin(a) * r1);
        }
        label("ANALYSING", card.x + 82.0f, card.y + 32.0f, 10, theme::accent, 1.7f);
        body(ellipsize(*text, loadingName, 430.0f, 18, FontWeight::SemiBold),
             card.x + 82.0f, card.y + 48.0f, 18, theme::text, FontWeight::SemiBold);
        std::string stage;
        {
            std::lock_guard lock(workerMutex);
            stage = workerStage;
        }
        body(stage, card.x + 82.0f, card.y + 76.0f, 11, theme::muted);
        const float progress = loadingProgress.load();
        const Rect bar{card.x + 82.0f, card.y + 108.0f, card.w - 130.0f, 5.0f};
        ui::fillRoundedRect(renderer, bar, 2.5f, theme::alpha(theme::white, 0.08f));
        ui::fillRoundedRect(renderer, {bar.x, bar.y, bar.w * progress, bar.h}, 2.5f,
                            theme::accent);
        labelRight(formatNumber(progress * 100.0, 0) + "%", card.right() - 48.0f,
                   card.y + 126.0f, 10, theme::text, 0.5f);
        label("THE UI STAYS RESPONSIVE  ·  CACHE IS CHECKED FIRST",
              card.x + 48.0f, card.bottom() - 34.0f, 8, theme::dim, 0.7f);
    }

    if (state == AppState::Error) {
        ui::fillRect(renderer, {0, 64, 1440, 792}, SDL_Color{5, 7, 11, 180});
        const Rect card{390, 310, 660, 240};
        ui::fillRoundedRect(renderer, card, 14.0f, theme::panel);
        ui::strokeRoundedRect(renderer, card, 14.0f, theme::alpha(theme::red, 0.4f), 1.0f);
        ui::fillCircle(renderer, card.x + 46.0f, card.y + 46.0f, 15.0f,
                       theme::alpha(theme::red, 0.1f));
        drawIcon(4, card.x + 46.0f, card.y + 46.0f, 18.0f, theme::red);
        label("COULD NOT ANALYSE", card.x + 74.0f, card.y + 28.0f, 9, theme::red, 1.5f);
        body(ellipsize(*text, errorMessage, 560.0f, 16, FontWeight::SemiBold),
             card.x + 74.0f, card.y + 48.0f, 16, theme::text);
        body("Choose another WAV, MP3 or FLAC file.", card.x + 74.0f, card.y + 76.0f,
             12, theme::muted);
        const Rect button{card.x + 74.0f, card.y + 112.0f, 176.0f, 42.0f};
        ui::fillRoundedRect(renderer, button, 8.0f,
                            hover(button) ? theme::mix(theme::accent, theme::white, 0.12f)
                                          : theme::accent);
        drawIcon(0, button.x + 22.0f, button.centerY(), 15.0f,
                 SDL_Color{8, 18, 16, 255});
        body("CHOOSE ANOTHER", button.x + 40.0f, button.y + 14.0f, 10,
             SDL_Color{8, 18, 16, 255}, FontWeight::SemiBold);
        // The error overlay's button reuses the empty-load hitbox.
        layout.emptyLoad = button;
    }

    if (!toast.empty() && animationTime < toastUntil) {
        const float alpha = static_cast<float>(
            std::clamp((toastUntil - animationTime) / 0.4, 0.0, 1.0));
        const float width = text->measure(toast, 11, FontWeight::SemiBold) + 34.0f;
        const Rect pill{720.0f - width * 0.5f, 802.0f, width, 30.0f};
        ui::fillRoundedRect(renderer, pill, 15.0f,
                            theme::alpha(theme::panelRaised, alpha));
        ui::strokeRoundedRect(renderer, pill, 15.0f,
                              theme::alpha(theme::borderBright, alpha), 1.0f);
        text->drawCentered(toast, pill.centerX(), pill.y + 8.0f, 11,
                           FontWeight::SemiBold, theme::alpha(theme::text, alpha));
    }
}

// -------------------------------------------------------------------------
// Frame
// -------------------------------------------------------------------------
void DesktopApp::Impl::renderFrame() {
    computeLayout();
    drawBackground();
    drawHeader();
    drawToolbar();

    if (state == AppState::Empty) {
        drawEmptyState();
    } else {
        drawTimeline();
        drawRightPanels();
        drawChoreography();
    }

    drawFooter();
    drawOverlays();
}

// -------------------------------------------------------------------------
// Capture and run
// -------------------------------------------------------------------------
void DesktopApp::Impl::saveCapture(const std::filesystem::path& path) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) {
        std::fprintf(stderr, "SDL_RenderReadPixels failed: %s\n", SDL_GetError());
        return;
    }
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const std::string pathUtf8 = toUtf8(path);
    if (!SDL_SaveBMP(surface, pathUtf8.c_str())) {
        std::fprintf(stderr, "SDL_SaveBMP failed: %s\n", SDL_GetError());
    } else {
        std::printf("captured %s (%dx%d)\n", toUtf8(path).c_str(), surface->w,
                surface->h);
    }
    SDL_DestroySurface(surface);
}

int DesktopApp::Impl::runCapture() {
    captureMode = true;
    options.noAudio = !options.audioSmoke;
    if (!options.captureFile.empty() && !options.initialFile.empty()) {
        std::string error;
        std::unique_ptr<LoadedTrack> loaded =
            performLoad(options.initialFile, generation.load(), error);
        if (loaded) {
            applyTrack(std::move(loaded));
        } else {
            state = AppState::Error;
            errorMessage = error.empty() ? "load failed" : error;
        }
    }

    if (options.audioSmoke && track) {
        if (!player.audioAvailable()) {
            std::printf("audio smoke: unavailable (%s)\n",
                        audioError.empty() ? "no device" : audioError.c_str());
        } else {
            player.play();
            for (int i = 0; i < 30; ++i) {
                SDL_Delay(50);
                (void)clock.update(player.snapshot());
            }
            const double afterPlay = clock.position();
            player.pause();
            player.seek(5.0);
            (void)clock.update(player.snapshot());
            const double afterSeek = clock.position();
            player.play();
            for (int i = 0; i < 16; ++i) {
                SDL_Delay(50);
                (void)clock.update(player.snapshot());
            }
            const double afterResume = clock.position();
            player.pause();
            player.setRate(1.25);
            const double rateStart = clock.position();
            player.play();
            for (int i = 0; i < 16; ++i) {
                SDL_Delay(50);
                (void)clock.update(player.snapshot());
            }
            const double rateEnd = clock.position();
            player.pause();
            player.setRate(1.0);
            std::printf("audio smoke: device='%s' latency=%.1f ms play+1.5s=%.3f "
                        "seek=%.3f resume+0.8s=%.3f rate1.25+0.8s delta=%.3f\n",
                        player.deviceName().c_str(),
                        (player.baseLatency() + player.latencyTrim()) * 1000.0,
                        afterPlay, afterSeek, afterResume, rateEnd - rateStart);
            capturePosition = std::clamp(rateEnd, 0.0, duration());
        }
    }

    SDL_PumpEvents();
    SDL_Delay(80);
    renderFrame();
    SDL_RenderPresent(renderer);
    SDL_Delay(20);
    renderFrame();
    saveCapture(options.captureFile);
    return 0;
}

int DesktopApp::Impl::run() {
    if (!initialize()) {
        return 1;
    }
    computeLayout();
    if (options.captureFile.empty()) {
        if (!options.initialFile.empty()) {
            startLoad(options.initialFile);
        }
        Uint64 last = SDL_GetTicksNS();
        while (running) {
            pollWorker();
            pollDialog();
            updatePlayback();
            animationTime = static_cast<double>(SDL_GetTicksNS()) / 1e9;

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handleEvent(event, false);
            }
            renderFrame();
            SDL_RenderPresent(renderer);

            // SDL's vsync normally paces us; this protects against a renderer
            // that reports success but has vsync disabled.
            const Uint64 now = SDL_GetTicksNS();
            const double frameMs = static_cast<double>(now - last) / 1e6;
            if (frameMs < 8.0) {
                SDL_Delay(static_cast<Uint32>(8.0 - frameMs));
            }
            last = SDL_GetTicksNS();
        }
    } else {
        runCapture();
    }
    shutdown();
    return 0;
}

} // namespace bgmedia::desktop

namespace bgmedia::desktop {

DesktopApp::DesktopApp() : impl_(std::make_unique<Impl>()) {}

DesktopApp::~DesktopApp() = default;

int DesktopApp::run(const DesktopOptions& options) {
    impl_->options = options;
    return impl_->run();
}

} // namespace bgmedia::desktop
