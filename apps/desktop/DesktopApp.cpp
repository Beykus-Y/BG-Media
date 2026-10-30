#include "DesktopApp.hpp"

#include "MiniaudioPlayer.hpp"
#include "TextRenderer.hpp"
#include "Theme.hpp"
#include "Ui.hpp"
#include "UiMetrics.hpp"
#include "VrmScene.hpp"
#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/audio/MiniaudioDecoder.hpp"
#include "bgmedia/audio/MiniaudioMixDevice.hpp"
#include "bgmedia/audio/MiniaudioMixLoader.hpp"
#include "bgmedia/cache/AnalysisCache.hpp"
#include "bgmedia/choreography/Choreography.hpp"
#include "bgmedia/mix/MixEngine.hpp"
#include "bgmedia/mix/MixSession.hpp"
#include "bgmedia/mix/MixSync.hpp"
#include "bgmedia/motion3/AliveLayer.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/version.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

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
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <deque>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace bgmedia::desktop {
namespace {

using Clock = std::chrono::steady_clock;
using ui::Rect;

// Personality of the idle life layer (breathing phase, look-around rhythm).
constexpr std::uint32_t kAliveSeed = 0x6d61736cU;

constexpr float kPi = 3.14159265358979323846f;

// Render-to-display latency compensation for the dance pose (about 15 to 30 ms
// between sampling a frame and it reaching the screen). The dancer is sampled
// this far ahead of the media clock so it lands on the beat the viewer hears.
// Applies to the 3D character only: audio, the transport and UI cursors keep
// the media time.
constexpr double kDanceVisualLeadSeconds = 0.020;

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

const char* modeLabel(motion::DanceMode mode) {
    return mode == motion::DanceMode::Auto ? "AUTO" : "MANUAL";
}

std::string clipTitle(const motion::DanceFrame& frame) {
    return frame.clipName.empty() ? std::string("IDLE") : frame.clipName;
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
    Rect header{};
    Rect toolbar{};
    Rect timelineCard{};
    Rect choreoCard{};
    Rect right1{};
    Rect right2{};
    Rect right3{};
    Rect right4{};
    Rect footer{};
    Rect empty{};

    Rect loadButton{};
    Rect playButton{};
    Rect stopButton{};
    Rect rateButton{};
    Rect syncMinus{};
    Rect syncPlus{};
    Rect emptyLoad{};

    Rect motionSmooth{};
    Rect motionDebug{};
    Rect motionLab{};

    // Character-view switch and VRM controls.
    Rect mode2D{};
    Rect mode3D{};
    Rect vrmLoadButton{};
    Rect stage3D{};
    Rect vrmResetCamera{};
    Rect vrmDemoButton{};
    Rect vrmScaleMinus{};
    Rect vrmScalePlus{};
    Rect vrmOffsetMinus{};
    Rect vrmOffsetPlus{};
    Rect vrmLoadStage{};
    Rect vrmEmptyLoad{};
    Rect vrmRightLoad{};

    // BG MOTION 3.1 controls (3D view).
    Rect danceManual{};
    Rect danceAuto{};
    Rect danceLoop{};
    Rect danceTest{};
    Rect danceStyle[5]{};
    Rect danceBpmMinus{};
    Rect danceBpmPlus{};
    Rect danceExpr{};
    Rect danceLayer{};
    Rect danceGen{};
    Rect danceSpring{};
    Rect dbgFeetBtn{};
    Rect dbgSkelBtn{};
    Rect dbgRootBtn{};
    Rect springMinusBtn{};
    Rect springPlusBtn{};
    Rect springDampBtn{};
    Rect springResetBtn{};

    Rect ruler{};
    Rect wave{};
    Rect spectrogram{};
    Rect tempo{};
    Rect events{};
    Rect plot{};
};

// BG Mix screen geometry. It is rebuilt every frame from the logical canvas,
// so it follows window size, aspect ratio and UI scale.
struct MixLayout {
    Rect queueCard{};
    Rect deckA{};
    Rect deckB{};
    Rect planCard{};
    Rect controlCard{};
    Rect addButton{};
    Rect clearButton{};
    Rect playButton{};
    Rect stopButton{};
    Rect nextButton{};
    Rect cancelButton{};
    Rect previewButton{};
    Rect openDeckA{};
    Rect openDeckB{};
    Rect adaptiveChip{};
    Rect tempoRampChip{};
    Rect autoQueueChip{};
    Rect typePrev{};
    Rect typeNext{};
    Rect durationMinus{};
    Rect durationPlus{};
    Rect modePrev{};
    Rect modeNext{};
    Rect rampModePrev{};
    Rect rampModeNext{};
    Rect rampBarsPrev{};
    Rect rampBarsNext{};
    Rect curvePrev{};
    Rect curveNext{};
    Rect maxDeltaPrev{};
    Rect maxDeltaNext{};
    Rect rampMinus{};
    Rect rampPlus{};
    Rect pitchChip{};
    Rect bassChip{};
    Rect qualityStretchChip{};
    Rect timeline{};
    Rect monitor{};
    Rect uiScaleChip{};
    Rect qualityChip{};
    Rect fullscreenChip{};
    Rect deckGainMinus[2]{};
    Rect deckGainPlus[2]{};
    Rect deckWave[2]{};
    Rect queueRow[8]{};
    Rect queueRemove[8]{};
};

enum class Module {
    Beat,
    Motion,
    Vrm,
    Mix
};

enum class AppState {
    Empty,
    Loading,
    Ready,
    Error
};

enum class CharacterView {
    Robot2D,
    Vrm3D
};

enum class DialogKind {
    Audio,
    Vrm,
    Mix
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

bool isVrmExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".vrm" || ext == ".glb";
}

std::filesystem::path findDefaultVrm(const std::filesystem::path& base) {
    const std::vector<std::filesystem::path> directories = {
        base / "models",
        base.parent_path() / "models",
        base.parent_path().parent_path() / "models",
        base.parent_path().parent_path().parent_path() / "models",
        std::filesystem::current_path() / "models",
        std::filesystem::current_path().parent_path() / "models",
    };
    for (const auto& directory : directories) {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            continue;
        }
        std::vector<std::filesystem::path> candidates;
        for (std::filesystem::directory_iterator it(directory, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && isVrmExtension(it->path())) {
                candidates.push_back(it->path());
            }
        }
        std::sort(candidates.begin(), candidates.end());
        if (!candidates.empty()) {
            return candidates.front();
        }
    }
    return {};
}

double residentMemoryMb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return 0.0;
}

AnalysisResult makeSyntheticAnalysis(int bpm, double duration) {
    AnalysisResult analysis;
    analysis.info.duration = duration;
    analysis.info.sampleRate = 44100;
    analysis.info.sourceChannels = 2;
    analysis.bpm = static_cast<double>(bpm);
    analysis.bpmConfidence = 1.0f;
    analysis.beatsPerBar = 4;
    analysis.tempoStable = true;
    const double period = 60.0 / static_cast<double>(std::max(1, bpm));
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
        analysis.beats.push_back(beat);
    }
    for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
        const Beat& beat = analysis.beats[i];
        if (beat.downbeat && (beat.barIndex % 2 == 0)) {
            BassAccent accent;
            accent.time = beat.time;
            accent.strength = 0.95f;
            accent.lowBandRatio = 0.75f;
            accent.confidence = 1.0f;
            accent.nearestBeat = static_cast<int>(i);
            analysis.bassAccents.push_back(accent);
        }
    }
    return analysis;
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
    MixLayout mixLayout;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<TextRenderer> text;
    MiniaudioPlayer player;
    PlaybackClock clock;
    AnalysisConfig config;

    // ---------------------------------------------------------------
    // BG Mix 0.1
    // ---------------------------------------------------------------
    std::unique_ptr<mix::MixEngine> mixEngine;
    std::unique_ptr<mix::MixSession> mixSession;
    std::unique_ptr<MiniaudioMixDevice> mixDevice;
    Module module = Module::Beat;
    Rect modulePills[4]{};
    std::string mixError;
    bool mixPlanDetail = false;
    float mixDeckGain[2] = {1.0f, 1.0f};
    std::vector<float> mixSilentPump;

    // ---------------------------------------------------------------
    // Display / high-DPI state
    // ---------------------------------------------------------------
    int outputWidth = 1440;
    int outputHeight = 900;
    float uiScale = 1.0f;
    float autoUiScale = 1.0f;
    float uiScaleSetting = 0.0f;   // 0 = automatic
    float requestedUiScale = 1.0f;
    bool uiScaleLimited = false;
    float canvasW = 1440.0f;
    float canvasH = 900.0f;
    int vrmQuality = 100;          // percent of the stage size
    bool maximized = false;
    int windowedW = 0;
    int windowedH = 0;
    bool restoreWindowedOnLeave = false;
    int savedWindowW = 0;
    int savedWindowH = 0;
    bool savedMaximized = false;
    std::filesystem::path settingsPath;
    bool settingsDirty = false;
    double settingsSavedAt = 0.0;

    VrmScene vrm;
    CharacterView characterView = CharacterView::Robot2D;
    std::filesystem::path defaultVrmPath;
    std::filesystem::path lastVrmDirectory;
    std::string vrmError;
    std::string vrmFrameError;
    sdlgpu::Camera vrmCamera;
    bool cameraDragging = false;
    float cameraDragX = 0.0f;
    float cameraDragY = 0.0f;

    // Synthetic BG Motion test grid for the 3D view. It is used only when no
    // audio track is loaded, so the character can be verified independently of
    // BG Beat. Music playback always uses the real track choreography.
    bool syntheticDemo = false;
    std::unique_ptr<AnalysisResult> syntheticAnalysis;
    std::unique_ptr<Choreography> syntheticChoreography;
    double syntheticPosition = 0.0;
    bool syntheticPlaying = true;

    double fpsSmoothed = 0.0;
    double frameMsSmoothed = 0.0;

    std::filesystem::path cacheDirectory;
    std::filesystem::path assetsDirectory;
    std::vector<SDL_DialogFileFilter> filters;
    std::vector<SDL_DialogFileFilter> vrmFilters;
    std::string dialogStartLocation;
    DialogKind dialogKind = DialogKind::Audio;

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
    std::vector<std::string> dialogPaths;
    bool dialogPending = false;
    std::string dialogError;

    ~Impl() {
        stopWorker();
        mixDevice.reset();
        mixSession.reset();
        mixEngine.reset();
        if (spectrogramTexture) {
            SDL_DestroyTexture(spectrogramTexture);
        }
        player.close();
        vrm.shutdown();
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
    // Preferences, high-DPI metrics and module switching
    // ---------------------------------------------------------------------
    void loadSettings() {
        if (settingsPath.empty()) {
            return;
        }
        std::ifstream file(settingsPath);
        if (!file) {
            return;
        }
        std::string line;
        while (std::getline(file, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, eq);
            const std::string value = line.substr(eq + 1);
            try {
                if (key == "window_w") {
                    savedWindowW = std::max(320, std::stoi(value));
                } else if (key == "window_h") {
                    savedWindowH = std::max(240, std::stoi(value));
                } else if (key == "maximized") {
                    savedMaximized = std::stoi(value) != 0;
                } else if (key == "fullscreen") {
                    options.fullscreen = std::stoi(value) != 0;
                } else if (key == "ui_scale") {
                    const double v = std::stod(value);
                    uiScaleSetting = v >= 1.0 ? static_cast<float>(v) : 0.0f;
                } else if (key == "vrm_quality") {
                    vrmQuality = std::clamp(std::stoi(value), 50, 100);
                }
            } catch (...) {
                // A malformed preference must never stop the application.
            }
        }
    }

    void saveSettings() {
        if (settingsPath.empty() || !window) {
            return;
        }
        int w = 0;
        int h = 0;
        SDL_GetWindowSize(window, &w, &h);
        if (fullscreen && windowedW > 0 && windowedH > 0) {
            // Persist the windowed geometry, not the fullscreen display size;
            // otherwise leaving fullscreen on the next run restores a
            // screen-sized window.
            w = windowedW;
            h = windowedH;
        }
        std::error_code ec;
        std::filesystem::create_directories(settingsPath.parent_path(), ec);
        std::ofstream file(settingsPath, std::ios::trunc);
        if (!file) {
            return;
        }
        file << "window_w=" << w << "\n";
        file << "window_h=" << h << "\n";
        file << "maximized=" << (maximized ? 1 : 0) << "\n";
        file << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
        file << "ui_scale=" << (uiScaleSetting > 0.0f ? uiScaleSetting : 0.0f)
             << "\n";
        file << "vrm_quality=" << vrmQuality << "\n";
        settingsDirty = false;
        settingsSavedAt = animationTime;
    }

    // Reads the real drawable size, chooses a logical scale and applies it to
    // both SDL_Renderer and the glyph atlas. Layout then works in canvas units.
    void refreshRenderMetrics() {
        if (!renderer || !window) {
            return;
        }
        int w = 0;
        int h = 0;
        if (!SDL_GetRenderOutputSize(renderer, &w, &h) || w <= 0 || h <= 0) {
            SDL_GetWindowSize(window, &w, &h);
        }
        w = std::max(320, w);
        h = std::max(240, h);
        float dpi = SDL_GetWindowDisplayScale(window);
        if (!(dpi > 0.0f)) {
            dpi = 1.0f;
        }
        const float resolutionScale =
            std::min(static_cast<float>(w) / 1600.0f,
                     static_cast<float>(h) / 1000.0f);
        autoUiScale = std::clamp(std::max(dpi, resolutionScale), 1.0f, 2.5f);
        const float requested =
            uiScaleSetting > 0.0f ? uiScaleSetting : autoUiScale;
        // Keep enough logical pixels for the widest layout (queue + decks +
        // plan/transport). Without this clamp a 1280-point window at 200% would
        // leave only 640 logical pixels and push the transport off-screen.
        requestedUiScale = requested;
        uiScale = effectiveUiScale(static_cast<float>(w), static_cast<float>(h),
                                   requested);
        uiScaleLimited = requested > uiScale + 0.005f;
        outputWidth = w;
        outputHeight = h;
        canvasW = static_cast<float>(w) / uiScale;
        canvasH = static_cast<float>(h) / uiScale;
        SDL_SetRenderScale(renderer, uiScale, uiScale);
        if (text) {
            text->setScale(uiScale);
        }
        SDL_SetWindowMinimumSize(window, 1280, 860);
        settingsDirty = true;
    }

    void toggleFullscreen(bool enabled) {
        if (!window) {
            return;
        }
        if (enabled && !fullscreen) {
            int w = 0;
            int h = 0;
            SDL_GetWindowSize(window, &w, &h);
            if (w > 0 && h > 0) {
                windowedW = w;
                windowedH = h;
            }
        }
        const bool wasFullscreen = fullscreen;
        fullscreen = enabled;
        if (!SDL_SetWindowFullscreen(window, enabled)) {
            fullscreen = wasFullscreen;
            showToast(std::string("Fullscreen failed: ") + SDL_GetError());
            return;
        }
        if (!enabled && wasFullscreen && windowedW > 0 && windowedH > 0) {
            // SDL normally restores the previous geometry; enforce it so a
            // display change while fullscreen cannot leave a tiny window. Some
            // platforms apply the mode change asynchronously, so the leave
            // event repeats this once the window is windowed again.
            restoreWindowedOnLeave = true;
            SDL_SetWindowSize(window, windowedW, windowedH);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED);
        }
        refreshRenderMetrics();
        computeLayout();
        settingsDirty = true;
        showToast(enabled ? "Fullscreen  ·  F11 or Esc for windowed"
                          : "Windowed  ·  F11 for fullscreen");
    }

    void cycleUiScale() {
        static constexpr float kChoices[] = {0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
        int index = 0;
        for (int i = 0; i < static_cast<int>(std::size(kChoices)); ++i) {
            if (std::abs(kChoices[i] - uiScaleSetting) < 0.01f) {
                index = i + 1;
                break;
            }
        }
        uiScaleSetting = kChoices[index % static_cast<int>(std::size(kChoices))];
        refreshRenderMetrics();
        computeLayout();
        settingsDirty = true;
        std::string message =
            uiScaleSetting <= 0.0f
                ? "UI scale: automatic"
                : "UI scale: " + formatNumber(uiScaleSetting * 100.0, 0) + "%";
        if (uiScaleLimited) {
            message += " (limited to " + formatNumber(uiScale * 100.0, 0) +
                       "% so the controls stay on screen)";
        }
        showToast(message);
    }

    [[nodiscard]] std::string uiScaleCaption() const {
        const int effective =
            static_cast<int>(std::lround(uiScale * 100.0f));
        if (uiScaleSetting <= 0.0f) {
            return uiScaleLimited ? "UI AUTO / " + std::to_string(effective) + "%"
                                  : "UI AUTO";
        }
        std::string caption =
            "UI " +
            formatNumber(uiScaleSetting * 100.0, 0) + "%";
        if (uiScaleLimited) {
            caption += " / " + std::to_string(effective) + "%";
        }
        return caption;
    }

    void cycleVrmQuality() {
        static constexpr int kChoices[] = {100, 75, 50};
        int index = 0;
        for (int i = 0; i < 3; ++i) {
            if (kChoices[i] == vrmQuality) {
                index = (i + 1) % 3;
                break;
            }
        }
        vrmQuality = kChoices[index];
        settingsDirty = true;
        showToast("3D quality: " + std::to_string(vrmQuality) + "%");
    }

    void setModule(Module next) {
        if (module == next) {
            return;
        }
        if (module == Module::Mix && mixSession) {
            mixSession->pause();
        }
        module = next;
        switch (next) {
        case Module::Beat:
        case Module::Motion:
            if (characterView == CharacterView::Vrm3D) {
                setCharacterView(CharacterView::Robot2D, false);
            }
            break;
        case Module::Vrm:
            setCharacterView(CharacterView::Vrm3D, false);
            break;
        case Module::Mix:
            if (playing()) {
                player.pause();
            }
            break;
        }
        computeLayout();
    }

    // ---------------------------------------------------------------------
    // Setup
    // ---------------------------------------------------------------------
    bool initialize() {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }

        // Preferences live in the per-user SDL pref directory. They are read
        // before the window so the previous size and fullscreen state restore
        // without a visible jump.
        {
            char* pref = SDL_GetPrefPath("BGMedia", "BGMedia");
            if (pref) {
                settingsPath = fromUtf8(pref) / "desktop.ini";
                SDL_free(pref);
            }
            loadSettings();
        }

        int windowW = options.windowWidth > 0 ? options.windowWidth
                                              : (savedWindowW > 0 ? savedWindowW : 1440);
        int windowH = options.windowHeight > 0 ? options.windowHeight
                                               : (savedWindowH > 0 ? savedWindowH : 900);
        windowW = std::max(1280, windowW);
        windowH = std::max(860, windowH);
        window = SDL_CreateWindow("BG Media", windowW, windowH,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetWindowMinimumSize(window, 1280, 860);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            renderer = SDL_CreateRenderer(window, "software");
        }
        if (!renderer) {
            std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return false;
        }
        // No letterboxed logical presentation: the UI is laid out directly in
        // logical canvas units and SDL_RenderScale maps them 1:1 onto the
        // physical drawable. This is what keeps 2560x1440 and 3840x2160 sharp.
        SDL_SetRenderVSync(renderer, 1);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        if (options.uiScale > 0.0f) {
            uiScaleSetting = std::clamp(options.uiScale, 1.0f, kUiScaleMax);
        }
        if (options.vrmQuality > 0) {
            vrmQuality = std::clamp(options.vrmQuality, 50, 100);
        }
        refreshRenderMetrics();

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
        refreshRenderMetrics();

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

        // BG Mix 0.1: one prepared-audio engine, one session worker and one
        // output device. The mixer stays silent until the BG MIX screen starts
        // a track; the analysis player is a separate, untouched path.
        mixEngine = std::make_unique<mix::MixEngine>(48000);
        mix::MixPlanner planner;
        mixSession = std::make_unique<mix::MixSession>(
            *mixEngine,
            [this](const std::filesystem::path& path, mix::PreparedMixTrack& out,
                   std::string& error,
                   std::function<void(float, std::string_view)> progress) {
                MixTrackLoadOptions loadOptions;
                loadOptions.cacheDirectory = cacheDirectory;
                loadOptions.analysis = config;
                loadOptions.useCache = !options.noCache;
                loadOptions.targetSampleRate = 48000;
                return prepareMixTrack(path, loadOptions, out, error, progress);
            },
            planner);
        if (!options.noAudio) {
            mixDevice = std::make_unique<MiniaudioMixDevice>(*mixEngine);
            std::string mixDeviceError;
            if (!mixDevice->open(48000, &mixDeviceError)) {
                mixError = mixDeviceError;
                mixDevice.reset();
            }
        }

        filters = {
            SDL_DialogFileFilter{"Audio files", "*.wav;*.mp3;*.flac"},
            SDL_DialogFileFilter{"All files", "*.*"},
        };
        vrmFilters = {
            SDL_DialogFileFilter{"VRM 0.x models", "*.vrm;*.glb"},
            SDL_DialogFileFilter{"All files", "*.*"},
        };

        motionDebug = options.motionDebug;
        motionStepped = options.motionStepped;
        motionLabBpm = std::max(0, options.motionLabBpm);
        defaultVrmPath = findDefaultVrm(basePath);

        vrmCamera.yaw = kPi;
        vrmCamera.pitch = 0.10f;
        vrmCamera.distanceScale = 1.0f;
        vrmCamera.fov = 0.56f;
        vrm.renderSettings().modelScale = std::clamp(options.vrmScale, 0.2f, 5.0f);
        vrm.renderSettings().modelOffset.y = options.vrmOffsetY;
        initDance();

        // Preserve the old 2D startup path: the SDL_GPU device and private VRM
        // are loaded lazily on the first switch to 3D. Explicit --vrm/--3d or
        // --vrm-demo start in 3D and initialize immediately.
        if (options.startIn3D) {
            setCharacterView(CharacterView::Vrm3D, false);
            if (options.vrmDemo || options.motionLabBpm > 0) {
                setSyntheticDemo(options.motionLabBpm > 0 ? options.motionLabBpm
                                                          : 123,
                                 false);
            }
        }
        if (options.startInMix) {
            module = Module::Mix;
        } else if (options.startIn3D && options.dance.empty() &&
                   options.dancePlan.empty() && !options.vrmDemo &&
                   options.motionLabBpm <= 0 && !options.presentation) {
            // --vrm / --3d: the header selection must match the visible screen.
            module = Module::Vrm;
        }

        SDL_ShowWindow(window);
        if (options.fullscreen) {
            toggleFullscreen(true);
        } else if (savedMaximized && options.windowWidth == 0) {
            SDL_MaximizeWindow(window);
            maximized = true;
            refreshRenderMetrics();
        } else if (options.captureFile.empty() && options.windowWidth == 0 &&
                   savedWindowW == 0 && savedWindowH == 0) {
            // First run without explicit geometry: start maximized so the
            // layout fills the display instead of a small 1440x900 window.
            SDL_MaximizeWindow(window);
            maximized = true;
            refreshRenderMetrics();
        }
        refreshRenderMetrics();
        computeLayout();
        return true;
    }

    void shutdown() {
        saveSettings();
        stopWorker();
        mixDevice.reset();
        mixSession.reset();
        mixEngine.reset();
        player.close();
        vrm.shutdown();
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
        syntheticDemo = false;
        syntheticAnalysis.reset();
        syntheticChoreography.reset();
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
                if (options.autoplay && !captureMode) {
                    player.play();
                    (void)clock.update(player.snapshot());
                }
            }
        }
        std::string title = "BG Media";
        if (!track->displayName.empty()) {
            title += " - " + track->displayName;
        }
        SDL_SetWindowTitle(window, title.c_str());
        onTrackChangedForDance();
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
    // 3D character mode
    // ---------------------------------------------------------------------
    void draw3DStage();
    void draw3DRightPanels();
    void drawDancePanels();
    void drawDanceDebug(const Rect& stage);
    void drawPresentation();
    void drawModeChip(const Rect& rect, std::string_view labelText, bool active,
                      SDL_Color color);

    // ---------------------------------------------------------------------
    // BG Motion lab / comparison
    // ---------------------------------------------------------------------
    [[nodiscard]] const AnalysisResult* activeAnalysis() const {
        if (syntheticDemo && !track && syntheticAnalysis) {
            return syntheticAnalysis.get();
        }
        if (motionLabBpm > 0 && motionLabAnalysis) {
            return motionLabAnalysis.get();
        }
        return track ? &track->analysis : nullptr;
    }

    [[nodiscard]] const Choreography* activeChoreography() const {
        if (syntheticDemo && !track && syntheticChoreography) {
            return syntheticChoreography.get();
        }
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
            syntheticDemo = false;
            syntheticAnalysis.reset();
            syntheticChoreography.reset();
            syntheticPosition = 0.0;
            showToast(track ? "Motion lab off - using analysed music"
                            : "Synthetic dance off");
            return;
        }
        if (!track) {
            setSyntheticDemo(motionLabBpm, true);
            return;
        }
        syntheticDemo = false;
        syntheticAnalysis.reset();
        syntheticChoreography.reset();
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
    // 3D character mode
    // ---------------------------------------------------------------------
    bool loadVrmModel(const std::filesystem::path& path, bool announce) {
        if (!vrm.initializeRenderer(&vrmError)) {
            vrmFrameError = vrmError;
            if (announce) {
                showToast("3D renderer unavailable: " + vrmError);
            }
            return false;
        }
        const auto started = Clock::now();
        std::string error;
        if (!vrm.load(path, &error)) {
            vrmError = error.empty() ? "VRM import failed" : error;
            vrmFrameError = vrmError;
            if (announce) {
                showToast("VRM import failed");
            }
            return false;
        }
        if (dance.ready()) {
            dance.setLegGeometry(vrm.legGeometry());  // re-plan the authored footwork for this body
        }
        vrmError = vrm.renderError();
        vrmFrameError = vrmError;
        lastVrmDirectory = path.parent_path();
        resetVrmCamera(false);
        const double ms =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (announce) {
            const vrm::VrmModel* loaded = vrm.model();
            std::string message = "VRM loaded";
            if (loaded) {
                message += ": " + std::to_string(loaded->diagnostics.triangleCount) +
                           " tris, " +
                           std::to_string(loaded->diagnostics.humanoidCount) +
                           " humanoid bones";
            }
            message += " · " + formatNumber(ms, 0) + " ms";
            showToast(message);
        }
        return true;
    }

    void setCharacterView(CharacterView view, bool announce) {
        if (view == CharacterView::Vrm3D && !vrm.ready()) {
            std::filesystem::path candidate = options.vrmFile;
            if (candidate.empty()) {
                candidate = defaultVrmPath;
            }
            if (!candidate.empty()) {
                (void)loadVrmModel(candidate, false);
            }
        }
        characterView = view;
        cameraDragging = false;
        if (announce) {
            if (view == CharacterView::Vrm3D) {
                showToast(vrm.ready() ? "3D VRM character view"
                                      : "3D view · import a VRM model");
            } else {
                showToast("2D robot debug view");
            }
        }
    }

    void toggleCharacterView() {
        setCharacterView(characterView == CharacterView::Robot2D
                             ? CharacterView::Vrm3D
                             : CharacterView::Robot2D,
                         true);
    }

    void resetVrmCamera(bool announce = true) {
        vrmCamera.yaw = kPi;
        vrmCamera.pitch = 0.10f;
        vrmCamera.distanceScale = 1.0f;
        vrmCamera.fov = 0.56f;
        if (options.hasCameraYaw) {
            vrmCamera.yaw = options.cameraYawDegrees * kPi / 180.0f;
        }
        vrmCamera.distanceScale = options.cameraZoom;
        if (presentation) {
            // Whole body incl. side steps and turns, centred, a little farther away.
            vrmCamera.pitch = 0.07f;
            vrmCamera.distanceScale = options.cameraZoom * 1.26f;
        }
        if (announce) {
            showToast("Camera reset");
        }
    }

    void adjustVrmScale(float delta) {
        auto& settings = vrm.renderSettings();
        settings.modelScale =
            std::clamp(settings.modelScale + delta, 0.25f, 3.0f);
        showToast("Model scale " + formatNumber(settings.modelScale, 2) + "x");
    }

    void adjustVrmOffset(float delta) {
        auto& settings = vrm.renderSettings();
        settings.modelOffset.y =
            std::clamp(settings.modelOffset.y + delta, -0.5f, 0.5f);
        showToast("Model height " +
                  std::string(settings.modelOffset.y >= 0.0f ? "+" : "") +
                  formatNumber(settings.modelOffset.y, 2) + " m");
    }

    void rebuildSyntheticDemo() {
        if (motionLabBpm <= 0) {
            syntheticAnalysis.reset();
            syntheticChoreography.reset();
            return;
        }
        syntheticAnalysis =
            std::make_unique<AnalysisResult>(makeSyntheticAnalysis(motionLabBpm, 60.0));
        syntheticChoreography = std::make_unique<Choreography>(
            ChoreographyBuilder{}.build(*syntheticAnalysis));
    }

    void setSyntheticDemo(int bpm, bool announce) {
        motionLabBpm = std::max(0, bpm);
        if (motionLabBpm == 0) {
            syntheticDemo = false;
            syntheticAnalysis.reset();
            syntheticChoreography.reset();
            syntheticPosition = 0.0;
            if (announce) {
                showToast("Synthetic dance off");
            }
            return;
        }
        syntheticDemo = true;
        syntheticPlaying = true;
        syntheticPosition = 0.0;
        rebuildSyntheticDemo();
        if (announce) {
            showToast("Synthetic dance · " + std::to_string(motionLabBpm) + " BPM");
        }
    }

    void updateDemo(double deltaSeconds) {
        if (!track && danceLabRunning && !captureMode) {
            danceLabTime += deltaSeconds;
        }
        if (!syntheticDemo || track) {
            return;
        }
        if (!syntheticPlaying || !syntheticAnalysis) {
            return;
        }
        const double duration = syntheticAnalysis->info.duration;
        syntheticPosition += deltaSeconds;
        if (syntheticPosition >= duration) {
            syntheticPosition = std::fmod(syntheticPosition, duration);
        }
    }

    [[nodiscard]] double vrmMediaTime() const {
        if (!track) {
            return syntheticDemo ? syntheticPosition : animationTime;
        }
        return currentPosition();
    }

    void updateVrmFrame() {
        if (characterView != CharacterView::Vrm3D) {
            return;
        }
        if (!vrm.ready()) {
            return;
        }
        // Dynamic offscreen target: the stage's physical size times the 3D
        // quality setting, capped at 3840x2160 so a 4K readback never exceeds
        // one 4K frame. The renderer only reallocates when the size changes.
        {
            const float quality = static_cast<float>(vrmQuality) / 100.0f;
            int targetW = std::clamp(
                static_cast<int>(std::lround(layout.stage3D.w * uiScale * quality)),
                256, 3840);
            int targetH = std::clamp(
                static_cast<int>(std::lround(layout.stage3D.h * uiScale * quality)),
                256, 2160);
            const double cap = 3840.0 * 2160.0;
            if (static_cast<double>(targetW) * targetH > cap) {
                const double factor = std::sqrt(cap / (static_cast<double>(targetW) * targetH));
                targetW = std::max(256, static_cast<int>(targetW * factor));
                targetH = std::max(256, static_cast<int>(targetH * factor));
            }
            vrm.setRenderSize(targetW, targetH);
        }
        vrm.renderSettings().modelScale =
            std::clamp(vrm.renderSettings().modelScale, 0.25f, 3.0f);
        vrm.renderSettings().modelOffset.y =
            std::clamp(vrm.renderSettings().modelOffset.y, -0.5f, 0.5f);
        if (danceUi()) {
            const double danceNow = danceVisualTime();
            updateDance();
            // The foot order of the pause hand-over is chosen once, from the pose the
            // dancer stopped in, and kept until the blend is fully back to the dance.
            const float idleMix = danceIdle.mix(animationTime);
            // The Idle stance is anchored at the same pose's root, so on resume the
            // advancing dance root does not drag the planted Idle feet along.
            if (idleMix <= 0.0f) {
                danceIdleFirstFoot = -1;
            } else if (danceIdleFirstFoot < 0) {
                danceIdleFirstFoot = motion::airborneFootFirst(danceFrame.pose);
                danceIdleAnchor = danceFrame.pose;
            }
            const motion::ClipPose pose =
                composeDancePose(danceFrame.pose, animationTime, idleMix,
                                 danceFrame.idleWeight, danceIdleFirstFoot);
            std::string danceError;
            if (!vrm.renderClip(pose, danceNow, vrmCamera, vrm.renderSettings(),
                                &danceError)) {
                vrmFrameError = danceError;
            } else {
                vrmFrameError = vrm.renderError();
                recordDanceTrails();
            }
            return;
        }
        const double time = vrmMediaTime();
        const CharacterPose pose = sampleActiveAt(time);
        std::string error;
        if (!vrm.render(pose, time, vrmCamera, vrm.renderSettings(), &error)) {
            vrmFrameError = error;
        } else {
            vrmFrameError = vrm.renderError();
        }
    }

    // ---------------------------------------------------------------------
    // BG MOTION 3.1: authored dance clips
    // ---------------------------------------------------------------------
    motion::DancePlayer dance;
    motion::DanceFrame danceFrame;
    // Dance <-> Idle crossfade for a stopped transport (wall-clock driven).
    motion::IdleCrossfade danceIdle;
    int danceIdleFirstFoot = -1;  // latched foot order of the pause hand-over
    motion::ClipPose danceIdleAnchor;  // latched pose whose root places the Idle stance
    bool useClips = true;
    double danceLabTime = 0.0;
    // Set once the user picks a dance mode or a style this session. Until then a
    // loaded track starts in Auto (the out-of-box default), and the choice is not
    // persisted, so every launch begins that way.
    bool danceModeChosen = false;
    bool danceLabRunning = true;
    int danceLabBpm = 100;
    bool musicDebug = false;
    std::filesystem::path motionProfilePath;
    std::filesystem::path motion32ProfilePath;
    int generatedDanceIndex = -1;
    bool dbgFeet = false;
    bool dbgSkeleton = false;
    bool dbgRoot = false;
    bool presentation = false;
    double presentationSince = 0.0;
    struct TrailPoint {
        float x = 0.0f;
        float z = 0.0f;
        motion::FootPhase phase = motion::FootPhase::Stance;
    };
    std::deque<TrailPoint> footTrail[2];
    std::deque<std::pair<float, float>> rootTrail;
    // Live-mix follow: the master deck id and tempo last installed into the
    // dance grid, so a handover or a tempo ramp re-anchors exactly once.
    std::uint64_t mixDanceTrackId = 0;
    // DancePlayer keeps a raw analysis pointer, so the mix follow adapter must
    // own the analysis for as long as it is installed.
    std::shared_ptr<const AnalysisResult> mixDanceAnalysis;

    [[nodiscard]] bool danceUi() const { return useClips && dance.ready(); }
    [[nodiscard]] mix::LiveMixTempo liveMixTempoNow() const {
        return mixSession ? mix::liveMixTempo(mixSession->engine().snapshot())
                          : mix::LiveMixTempo{};
    }
    [[nodiscard]] bool mixDrivesDance() const {
        if (track) {
            return false;
        }
        const mix::LiveMixTempo live = liveMixTempoNow();
        return live.valid && live.playing;
    }
    [[nodiscard]] double danceTime() const {
        if (track) {
            return currentPosition();
        }
        if (mixSession) {
            const mix::LiveMixTempo live = liveMixTempoNow();
            if (live.valid && live.playing) {
                // Follow the audible master position, not the raw deck cursor.
                return live.positionSeconds;
            }
        }
        return danceLabTime;
    }
    // Dance time for the character's pose on this frame: the media clock plus the
    // render latency lead. Scheduling and the transport keep using danceTime(). A capture
    // (still or offline render) shows the exact media time, so it takes no lead.
    [[nodiscard]] double danceVisualTime() const {
        return track && !captureMode ? danceTime() + kDanceVisualLeadSeconds : danceTime();
    }

    // Whether the dance transport is moving. Scrubbing and capture drive the dance
    // from the position, so they never count as stopped.
    [[nodiscard]] bool danceTransportRunning() const {
        if (!track) {
            // A live BG Mix drives the dance clock; otherwise the Motion Lab
            // clock only advances while Motion Lab runs.
            if (mixDrivesDance()) {
                return true;
            }
            return captureMode || danceLabRunning;
        }
        return captureMode || scrubbing || playing();
    }

    // The pose the 3D character shows this frame. The dance is frozen while the
    // transport is stopped; once it has been stopped for a moment the pose eases
    // to the Idle clip, sampled on wall time. The alive layer (breathing, weight
    // shift, head look-around) comes last, with its energy taken from the blended
    // pose: full strength at idle, reduced while dancing.
    // `scheduledIdle` is the Idle clip's share in the dance pose itself (Idle selected,
    // a one-shot's return, an Auto silence segment): that pose already breathes too.
    [[nodiscard]] motion::ClipPose composeDancePose(const motion::ClipPose& dancePose,
                                                    double wallNow, float idleMix,
                                                    float scheduledIdle,
                                                    int firstFoot) const {
        motion::ClipPose pose = dancePose;
        float bodyWeight = 1.0f - std::clamp(scheduledIdle, 0.0f, 1.0f);
        if (idleMix > 0.0f) {
            const motion::HumanoidClip* idle =
                dance.library().forStyle(motion::DanceStyle::Idle);
            motion::ClipPose idlePose = motion::neutralClipPose();
            if (idle && idle->bpm > 0.0) {
                // Idle has a wall-clock time axis (tempoSync false): one beat per 60 / bpm s.
                idlePose = motion::sampleClip(*idle, wallNow * idle->bpm / 60.0);
            }
            // The dancer keeps the dance root and the feet are handed over one at a
            // time, so a travelled dancer does not glide back to the Idle origin.
            pose = motion::blendToIdleKeepingFeet(dancePose, idlePose, idleMix, firstFoot,
                                                  firstFoot >= 0 ? &danceIdleAnchor : nullptr);
            // Idle already breathes and shifts its weight, so the alive layer's
            // breathing and weight shift fade out as it takes over. The head look stays.
            bodyWeight *= 1.0f - std::clamp(idleMix, 0.0f, 1.0f);
        }
        motion::applyAlive(pose, wallNow, pose.energy, kAliveSeed, 1.0f, bodyWeight, 1.0f);
        return pose;
    }

    [[nodiscard]] motion::BeatClock danceGrid() const {
        if (track) {
            if (motionLabBpm > 0) {
                return motion::BeatClock::constant(motionLabBpm, 0.45, 4);
            }
            return motion::BeatClock::fromAnalysis(track->analysis);
        }
        // With no video track, a live BG Mix is the musical source. Build a
        // constant grid from the master deck's audible tempo and phase; BG
        // Motion 3.x itself is unchanged, it only receives a different clock.
        if (mixSession) {
            const mix::LiveMixTempo live = liveMixTempoNow();
            if (live.valid && live.playing && live.bpm > 0.0) {
                // The dance is sampled at the source-media position, so the
                // analysed source-time grid stays correct at any deck rate
                // and keeps the real beat offset/downbeats. Only fall back to
                // a constant clock when no analysis is available yet.
                if (mixDanceAnalysis && live.trackId == mixDanceTrackId) {
                    return motion::BeatClock::fromAnalysis(*mixDanceAnalysis);
                }
                // The dance is sampled at the source-media position, so the
                // clock must use the source-time beat period. The live BPM is
                // sourceBpm * rate and would apply the rate twice.
                const double period = live.beatPeriodSeconds;
                const double firstBeat =
                    live.positionSeconds - live.beatPhase * period;
                return motion::BeatClock::constant(live.sourceBpm, firstBeat, 4);
            }
        }
        return motion::BeatClock::constant(danceLabBpm, 0.0, 4);
    }

    void refreshDanceGrid() { dance.setGrid(danceGrid(), danceTime()); }

    void onTrackChangedForDance() {
        dance.setAnalysis(track ? activeAnalysis() : nullptr, danceTime());
        // Only override the analysis timeline when the user explicitly asks for
        // a Motion Lab tempo. Re-installing a grid-only clock here would throw
        // away the downbeat confidence and the real accent map.
        if (!track || motionLabBpm > 0) {
            dance.setGrid(danceGrid(), danceTime());
        }
        // Default to Auto for a track the user has not steered in this session.
        // setMode(Auto) builds the plan from the analysis set above.
        if (track && !danceModeChosen && danceUi() && dance.mode() != motion::DanceMode::Auto) {
            dance.setMode(motion::DanceMode::Auto, danceTime());
        }
        if (dance.mode() == motion::DanceMode::Auto && track) {
            std::fprintf(stderr, "BG Motion Auto plan:\n%s",
                         dance.director().describeSchedule().c_str());
        }
        footTrail[0].clear();
        footTrail[1].clear();
        rootTrail.clear();
    }

    [[nodiscard]] static const char* expressionLabel(motion::MotionExpression expression) {
        switch (expression) {
        case motion::MotionExpression::Off: return "EXPR OFF";
        case motion::MotionExpression::Soft: return "EXPR SOFT";
        case motion::MotionExpression::Expressive: return "EXPR EXP";
        }
        return "EXPR SOFT";
    }

    void saveMotion32Settings() {
        if (motion32ProfilePath.empty()) {
            return;
        }
        motion::Motion32Profile profile;
        profile.overlayMode = dance.overlayMode();
        profile.springEnabled = vrm.secondarySettings().enabled;
        profile.springIntensity = vrm.secondarySettings().intensity;
        profile.springDamping = vrm.secondarySettings().damping;
        std::string error;
        if (!motion::saveMotion32Profile(motion32ProfilePath, profile, &error)) {
            std::fprintf(stderr, "motion32 profile save failed: %s\n", error.c_str());
        }
    }

    void cycleExpression() {
        if (!danceUi()) {
            return;
        }
        motion::MotionExpression next = motion::MotionExpression::Soft;
        switch (dance.expression()) {
        case motion::MotionExpression::Off: next = motion::MotionExpression::Soft; break;
        case motion::MotionExpression::Soft: next = motion::MotionExpression::Expressive; break;
        case motion::MotionExpression::Expressive: next = motion::MotionExpression::Off; break;
        }
        dance.setExpression(next);
        if (!motionProfilePath.empty()) {
            std::string error;
            if (!motion::saveWarpProfile(motionProfilePath, dance.warpProfile(), &error)) {
                std::fprintf(stderr, "motion profile save failed: %s\n", error.c_str());
            }
        }
        showToast(std::string("Expression: ") + expressionLabel(next));
    }

    void initDance() {
        std::filesystem::path directory = options.dancesDirectory;
        if (directory.empty()) {
            directory = assetsDirectory / "dances";
        }
        const std::size_t count = dance.loadLibrary(directory);
        if (vrm.model()) {
            dance.setLegGeometry(vrm.legGeometry());
        }
        for (const motion::ClipDiagnostic& d : dance.library().diagnostics()) {
            std::fprintf(stderr, "dance %s: %s: %s\n",
                         d.severity == motion::ClipDiagnostic::Severity::Error ? "error"
                                                                               : "warning",
                         d.file.c_str(), d.message.c_str());
        }
        std::fprintf(stderr, "BG MOTION 3.2: %zu dance clip(s) loaded from %s\n", count,
                     toUtf8(directory).c_str());
        useClips = count > 0 && !options.legacyMotion;
        if (options.danceBpm > 0) {
            danceLabBpm = std::clamp(options.danceBpm, 40, 220);
        } else if (options.motionLabBpm > 0) {
            danceLabBpm = options.motionLabBpm;
        }
        dbgFeet = options.debugFeet;
        dbgSkeleton = options.debugSkeleton;
        dbgRoot = options.debugRoot;
        presentation = options.presentation;
        musicDebug = options.motionDebug;
        if (!cacheDirectory.empty()) {
            motionProfilePath = cacheDirectory / "motion-profile.cfg";
            motion::WarpProfile profile;
            if (motion::loadWarpProfile(motionProfilePath, profile, nullptr)) {
                dance.setWarpProfile(profile);
            }
            motion32ProfilePath = cacheDirectory / "motion32-profile.cfg";
            motion::Motion32Profile runtimeProfile;
            if (motion::loadMotion32Profile(motion32ProfilePath, runtimeProfile, nullptr)) {
                dance.setOverlayMode(runtimeProfile.overlayMode);
                vrm.secondarySettings().enabled = runtimeProfile.springEnabled;
                vrm.secondarySettings().intensity = runtimeProfile.springIntensity;
                vrm.secondarySettings().damping = runtimeProfile.springDamping;
            }
        }
        if (!options.danceExpression.empty()) {
            motion::WarpProfile profile = dance.warpProfile();
            profile.expression = motion::expressionFromName(options.danceExpression);
            dance.setWarpProfile(profile);
            if (!motionProfilePath.empty()) {
                std::string error;
                if (!motion::saveWarpProfile(motionProfilePath, profile, &error)) {
                    std::fprintf(stderr, "motion profile save failed: %s\n", error.c_str());
                }
            }
        }
        dance.setGrid(danceGrid(), 0.0);
        vrm.secondarySettings().enabled = true;
        vrm.secondarySettings().intensity = 1.1f;
        vrm.secondarySettings().damping = 0.5f;
        if (!useClips) {
            return;
        }
        if (!options.dancePlan.empty() || !options.dance.empty()) {
            // A dance named on the command line is an explicit choice.
            danceModeChosen = true;
        }
        if (!options.dancePlan.empty()) {
            std::vector<motion::DanceStyle> styles;
            std::stringstream list(options.dancePlan);
            std::string item;
            while (std::getline(list, item, ',')) {
                styles.push_back(motion::styleFromName(item));
            }
            dance.startSequence(styles, 0.0, options.danceSpan, /*immediate=*/true);
            return;
        }
        const std::string& name = options.dance;
        if (name == "test") {
            dance.startTestDance(0.0);
        } else if (name == "auto") {
            dance.setMode(motion::DanceMode::Auto, 0.0);
        } else if (!name.empty()) {
            if (dance.library().find(name)) {
                (void)dance.selectClip(name, 0.0, true);
            } else {
                const motion::DanceStyle style = motion::styleFromName(name);
                dance.select(style, 0.0, /*immediate=*/true);
            }
        }
    }

    void selectDance(motion::DanceStyle style) {
        if (!danceUi()) {
            showToast("No dance clips loaded");
            return;
        }
        generatedDanceIndex = -1;
        danceModeChosen = true;
        dance.select(style, danceTime());
        const motion::HumanoidClip* clip = dance.library().forStyle(style);
        showToast(std::string("Dance: ") + (clip ? clip->name : motion::styleName(style)) +
                  (track ? "  ·  on the next beat" : ""));
    }

    void cycleOverlay() {
        if (!danceUi()) {
            return;
        }
        motion::OverlayMode next = motion::OverlayMode::Off;
        switch (dance.overlayMode()) {
        case motion::OverlayMode::Off: next = motion::OverlayMode::Light; break;
        case motion::OverlayMode::Light: next = motion::OverlayMode::Full; break;
        case motion::OverlayMode::Full: next = motion::OverlayMode::HandsOnly; break;
        case motion::OverlayMode::HandsOnly: next = motion::OverlayMode::Off; break;
        }
        dance.setOverlayMode(next);
        saveMotion32Settings();
        showToast(std::string("Motion layer: ") + motion::overlayModeName(next));
    }

    void cycleGenerated() {
        if (!danceUi()) {
            return;
        }
        const auto& generated = dance.generatedDances();
        if (generated.empty()) {
            showToast("No procedural dances generated");
            return;
        }
        danceModeChosen = true;
        generatedDanceIndex =
            (generatedDanceIndex + 1) % static_cast<int>(generated.size() + 1);
        if (generatedDanceIndex == static_cast<int>(generated.size())) {
            generatedDanceIndex = -1;
            dance.select(motion::DanceStyle::Groove, danceTime());
            showToast("Procedural: off");
            return;
        }
        const auto& entry = generated[static_cast<std::size_t>(generatedDanceIndex)];
        if (dance.selectClip(entry.clip.id, danceTime() + (track ? 0.10 : 0.0),
                             !track)) {
            showToast("Procedural: " + entry.clip.name);
        }
    }

    void toggleSpring() {
        if (!vrm.secondaryAvailable()) {
            showToast("This model has no VRM spring bones");
            return;
        }
        auto& settings = vrm.secondarySettings();
        settings.enabled = !settings.enabled;
        if (settings.enabled) {
            vrm.resetSecondary();
        }
        showToast(std::string("Secondary dynamics: ") +
                  (settings.enabled ? "ON" : "OFF"));
        saveMotion32Settings();
    }

    void adjustSpringIntensity(float delta) {
        if (!vrm.secondaryAvailable()) {
            return;
        }
        auto& settings = vrm.secondarySettings();
        settings.intensity = std::clamp(settings.intensity + delta, 0.0f, 2.0f);
        vrm.resetSecondary();
        showToast("Spring intensity " + formatNumber(settings.intensity, 1) + "x");
        saveMotion32Settings();
    }

    void cycleSpringDamping() {
        if (!vrm.secondaryAvailable()) {
            return;
        }
        auto& settings = vrm.secondarySettings();
        if (settings.damping < 0.35f) {
            settings.damping = 0.5f;
        } else if (settings.damping < 0.7f) {
            settings.damping = 0.85f;
        } else {
            settings.damping = 0.2f;
        }
        vrm.resetSecondary();
        showToast("Spring damping " + formatNumber(settings.damping, 2));
        saveMotion32Settings();
    }

    void setDanceMode(motion::DanceMode mode) {
        if (!danceUi()) {
            return;
        }
        danceModeChosen = true;
        dance.setMode(mode, danceTime());
        if (mode == motion::DanceMode::Auto) {
            showToast(dance.autoNote().empty() ? "Auto dance" : dance.autoNote());
        } else {
            showToast("Manual dance");
        }
    }

    void changeDanceBpm(int delta) {
        if (track) {
            if (motionLabBpm > 0) {
                setMotionLab(std::clamp(motionLabBpm + delta, 40, 220));
                onTrackChangedForDance();
            } else {
                showToast("BPM comes from BG Beat; use LAB to override");
            }
            return;
        }
        danceLabBpm = std::clamp(danceLabBpm + delta, 40, 220);
        refreshDanceGrid();
        showToast("Motion Lab " + std::to_string(danceLabBpm) + " BPM");
    }

    void startTestDance() {
        if (!danceUi()) {
            return;
        }
        danceModeChosen = true;
        dance.startTestDance(danceTime());
        showToast("TEST DANCE: Groove > Hip-Hop > Side Step > Energetic");
    }

    void togglePresentation() {
        presentation = !presentation;
        presentationSince = animationTime;
        if (presentation) {
            vrmCamera.yaw = kPi;
            vrmCamera.pitch = 0.07f;
            vrmCamera.distanceScale = 1.26f;
            vrmCamera.fov = 0.56f;
        }
    }

    void refreshMixDanceIfNeeded() {
        if (track || !mixSession) {
            return;
        }
        const mix::LiveMixTempo live = liveMixTempoNow();
        if (!live.valid || !live.playing) {
            return;
        }
        // Re-install only when the musical master changes, not when its live
        // rate changes: the analysed source-time grid stays valid at any rate,
        // and setAnalysis() rebuilds the Auto plan and restarts its blend.
        if (live.trackId == mixDanceTrackId && mixDanceAnalysis) {
            return;
        }
        mixDanceTrackId = live.trackId;
        mixDanceAnalysis = mixSession->deckAnalysis(live.master);
        if (mixDanceAnalysis) {
            dance.setAnalysis(mixDanceAnalysis.get(), danceTime());
        }
        dance.setGrid(danceGrid(), danceTime());
    }

    void updateDance() {
        refreshMixDanceIfNeeded();
        danceFrame = dance.update(danceVisualTime());
    }

    void recordDanceTrails() {
        const vrm::VrmModel* loaded = vrm.model();
        const animation::RetargetedPose& pose = vrm.lastPose();
        if (!loaded || pose.nodeGlobals.empty()) {
            return;
        }
        const vrm::HumanoidBone ankles[2] = {vrm::HumanoidBone::LeftFoot,
                                             vrm::HumanoidBone::RightFoot};
        for (int f = 0; f < 2; ++f) {
            const int node = loaded->nodeFor(ankles[f]);
            const math::Vec3 p = animation::HumanoidRetargeter::stagePosition(pose, node);
            auto& trail = footTrail[f];
            trail.push_back({p.x, p.z, danceFrame.pose.foot[static_cast<std::size_t>(f)].phase});
            while (trail.size() > 180) {
                trail.pop_front();
            }
        }
        rootTrail.emplace_back(pose.rootOffset.x + pose.rootPivot.x,
                               pose.rootOffset.z + pose.rootPivot.z);
        while (rootTrail.size() > 240) {
            rootTrail.pop_front();
        }
    }

    [[nodiscard]] static SDL_Color phaseColor(motion::FootPhase phase) {
        switch (phase) {
        case motion::FootPhase::Stance: return theme::accent;
        case motion::FootPhase::Plant: return SDL_Color{150, 255, 120, 255};
        case motion::FootPhase::Release: return SDL_Color{255, 232, 120, 255};
        case motion::FootPhase::Pivot: return theme::violet;
        case motion::FootPhase::Swing: return theme::amber;
        }
        return theme::muted;
    }

    // ---------------------------------------------------------------------
    // Dialog and file handling
    // ---------------------------------------------------------------------
    void openFileDialog() {
        if (!window) {
            return;
        }
        dialogKind = DialogKind::Audio;
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

    void loadStartupMixTracks() {
        if (!mixSession) {
            return;
        }
        for (const auto& path : options.mixTracks) {
            mixSession->addTrack(path);
        }
    }

    // Headless captures and machines where the output device failed to open
    // still need the engine clock to advance and snapshots to publish.
    void pumpMixSilent(double seconds) {
        if (!mixEngine) {
            return;
        }
        const double clamped = std::clamp(seconds, 0.0, 0.05);
        const int frames =
            std::clamp(static_cast<int>(clamped * 48000.0), 64, 2048);
        mixSilentPump.resize(static_cast<std::size_t>(frames) * 2, 0.0f);
        mixEngine->render(mixSilentPump.data(), static_cast<std::size_t>(frames));
    }

    void openMixFileDialog() {
        if (!window) {
            return;
        }
        dialogKind = DialogKind::Mix;
        dialogStartLocation = toUtf8(std::filesystem::current_path());
        SDL_ShowOpenFileDialog(&Impl::dialogCallback, this, window, filters.data(),
                               static_cast<int>(filters.size()),
                               dialogStartLocation.empty() ? nullptr
                                                           : dialogStartLocation.c_str(),
                               true);
    }

    void openVrmFileDialog() {
        if (!window) {
            return;
        }
        dialogKind = DialogKind::Vrm;
        if (!lastVrmDirectory.empty()) {
            dialogStartLocation = toUtf8(lastVrmDirectory);
        } else {
            dialogStartLocation = toUtf8(std::filesystem::current_path());
        }
        SDL_ShowOpenFileDialog(&Impl::dialogCallback, this, window, vrmFilters.data(),
                               static_cast<int>(vrmFilters.size()),
                               dialogStartLocation.empty() ? nullptr
                                                           : dialogStartLocation.c_str(),
                               false);
    }

    static void SDLCALL dialogCallback(void* userdata, const char* const* filelist,
                                       int) {
        auto* self = static_cast<Impl*>(userdata);
        std::lock_guard lock(self->dialogMutex);
        self->dialogPaths.clear();
        if (filelist && filelist[0]) {
            for (int i = 0; filelist[i]; ++i) {
                self->dialogPaths.emplace_back(filelist[i]);
            }
            self->dialogPath = filelist[0];
            self->dialogPending = true;
        } else if (!filelist) {
            self->dialogError = SDL_GetError();
        }
    }

    void pollDialog() {
        std::string path;
        std::vector<std::string> paths;
        std::string error;
        {
            std::lock_guard lock(dialogMutex);
            if (dialogPending) {
                path = std::move(dialogPath);
                paths = std::move(dialogPaths);
                dialogPending = false;
                dialogPath.clear();
                dialogPaths.clear();
            }
            if (!dialogError.empty()) {
                error = std::move(dialogError);
                dialogError.clear();
            }
        }
        if (!error.empty()) {
            showToast("File dialog: " + error);
        }
        if (!paths.empty()) {
            if (dialogKind == DialogKind::Vrm) {
                if (loadVrmModel(fromUtf8(paths.front().c_str()), true)) {
                    setCharacterView(CharacterView::Vrm3D, false);
                }
            } else if (dialogKind == DialogKind::Mix && mixSession) {
                std::vector<std::filesystem::path> queue;
                queue.reserve(paths.size());
                for (const std::string& value : paths) {
                    const std::filesystem::path file = fromUtf8(value.c_str());
                    if (isAudioExtension(file)) {
                        queue.push_back(file);
                    }
                }
                if (!queue.empty()) {
                    mixSession->addTracks(queue);
                    showToast(std::to_string(queue.size()) + " track(s) queued");
                }
            } else if (!path.empty()) {
                startLoad(fromUtf8(path.c_str()));
            }
        } else if (!path.empty()) {
            if (dialogKind == DialogKind::Vrm) {
                if (loadVrmModel(fromUtf8(path.c_str()), true)) {
                    setCharacterView(CharacterView::Vrm3D, false);
                }
            } else if (dialogKind == DialogKind::Mix && mixSession) {
                if (isAudioExtension(fromUtf8(path.c_str()))) {
                    mixSession->addTrack(fromUtf8(path.c_str()));
                }
            } else {
                startLoad(fromUtf8(path.c_str()));
            }
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
        if (!track) {
            return syntheticDemo && syntheticAnalysis
                       ? syntheticAnalysis->info.duration
                       : 0.0;
        }
        return track->analysis.info.duration;
    }

    [[nodiscard]] double currentPosition() const {
        if (!track && syntheticDemo) {
            return std::clamp(syntheticPosition, 0.0, duration());
        }
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
        if (!track) {
            return syntheticDemo && syntheticPlaying;
        }
        if (captureMode) {
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
        if (!track && characterView == CharacterView::Vrm3D && danceUi()) {
            danceLabRunning = !danceLabRunning;
            showToast(danceLabRunning ? "Motion Lab running" : "Motion Lab paused");
            return;
        }
        if (!track) {
            if (syntheticDemo) {
                syntheticPlaying = !syntheticPlaying;
                showToast(syntheticPlaying ? "Synthetic dance playing"
                                           : "Synthetic dance paused");
                return;
            }
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

    void toggleMixPlay() {
        if (!mixSession) {
            return;
        }
        const mix::MixSessionSnapshot snap = mixSession->snapshot();
        const bool anyPlaying = snap.engine.decks[0].playing ||
                                snap.engine.decks[1].playing;
        if (!snap.engine.paused && anyPlaying) {
            mixSession->pause();
        } else {
            if (!mixDevice && !options.noAudio) {
                showToast(mixError.empty() ? "Mix audio device unavailable"
                                           : mixError);
            }
            mixSession->play();
        }
    }

    void stopPlayback() {
        if (!track) {
            if (syntheticDemo) {
                syntheticPosition = 0.0;
                syntheticPlaying = false;
                showToast("Synthetic dance stopped");
            }
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
            if (syntheticDemo) {
                syntheticPosition = std::clamp(time, 0.0, duration());
                syntheticPlaying = resume;
            }
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
        if (!track && !syntheticDemo) {
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
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            refreshRenderMetrics();
            computeLayout();
            break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            // The OS owns the transition; mirror its state instead of calling
            // SDL_SetWindowFullscreen() again from the event handler.
            fullscreen = true;
            refreshRenderMetrics();
            computeLayout();
            settingsDirty = true;
            break;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            fullscreen = false;
            if (restoreWindowedOnLeave && windowedW > 0 && windowedH > 0) {
                SDL_SetWindowSize(window, windowedW, windowedH);
                SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                                      SDL_WINDOWPOS_CENTERED);
                restoreWindowedOnLeave = false;
            }
            refreshRenderMetrics();
            computeLayout();
            settingsDirty = true;
            break;
        case SDL_EVENT_WINDOW_MAXIMIZED:
            maximized = true;
            refreshRenderMetrics();
            computeLayout();
            settingsDirty = true;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            maximized = false;
            refreshRenderMetrics();
            computeLayout();
            settingsDirty = true;
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data) {
                const std::filesystem::path dropped = fromUtf8(event.drop.data);
                if (isVrmExtension(dropped)) {
                    if (loadVrmModel(dropped, true)) {
                        setCharacterView(CharacterView::Vrm3D, false);
                    }
                } else if (module == Module::Mix && isAudioExtension(dropped) &&
                           mixSession) {
                    mixSession->addTrack(dropped);
                    showToast("Queued " + toUtf8(dropped.filename()));
                } else {
                    startLoad(dropped);
                }
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
            if (cameraDragging && characterView == CharacterView::Vrm3D) {
                const float dx = static_cast<float>(mouseX) - cameraDragX;
                const float dy = static_cast<float>(mouseY) - cameraDragY;
                cameraDragX = static_cast<float>(mouseX);
                cameraDragY = static_cast<float>(mouseY);
                vrmCamera.yaw += dx * 0.0075f;
                vrmCamera.pitch =
                    std::clamp(vrmCamera.pitch + dy * 0.0055f, -1.10f, 1.10f);
                break;
            }
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
                cameraDragging = false;
                handleLeftUp(static_cast<float>(event.button.x),
                             static_cast<float>(event.button.y));
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                panning = false;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            mouseX = event.wheel.mouse_x;
            mouseY = event.wheel.mouse_y;
            if (characterView == CharacterView::Vrm3D &&
                layout.stage3D.contains(static_cast<float>(mouseX),
                                        static_cast<float>(mouseY))) {
                vrmCamera.distanceScale = std::clamp(
                    vrmCamera.distanceScale * std::pow(1.12f, -event.wheel.y),
                    0.42f, 2.2f);
                break;
            }
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
        const bool alt = (key.mod & SDL_KMOD_ALT) != 0;
        // Window management is global: F11 and Alt+Enter work on every screen.
        if (key.key == SDLK_F11 ||
            ((key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) && alt)) {
            toggleFullscreen(!fullscreen);
            return;
        }
        if (key.key == SDLK_ESCAPE && fullscreen) {
            toggleFullscreen(false);
            return;
        }
        if (module == Module::Mix && mixSession) {
            switch (key.key) {
            case SDLK_SPACE:
                toggleMixPlay();
                return;
            case SDLK_N:
                mixSession->next();
                return;
            case SDLK_X:
                mixSession->cancelTransition();
                return;
            case SDLK_A:
                mixSession->setAdaptive(!mixSession->adaptive());
                showToast(mixSession->adaptive() ? "Adaptive planning on"
                                                : "Adaptive planning off");
                return;
            case SDLK_T:
                mixSession->setTempoRampEnabled(!mixSession->tempoRampEnabled());
                showToast(mixSession->tempoRampEnabled()
                              ? "Tempo Ramp armed (experimental)"
                              : "Tempo Ramp disarmed");
                return;
            case SDLK_U:
                cycleUiScale();
                return;
            case SDLK_L:
                openMixFileDialog();
                return;
            case SDLK_M:
                mixSession->setAutoQueue(!mixSession->autoQueue());
                return;
            default:
                break;
            }
        }
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
        case SDLK_G:
            toggleCharacterView();
            break;
        case SDLK_C:
            if (characterView == CharacterView::Vrm3D) {
                resetVrmCamera();
            }
            break;
        case SDLK_1:
        case SDLK_4:
        case SDLK_0:
            if (characterView == CharacterView::Vrm3D && danceUi()) {
                selectDance(key.key == SDLK_1   ? motion::DanceStyle::Groove
                            : key.key == SDLK_4 ? motion::DanceStyle::Energetic
                                                : motion::DanceStyle::Idle);
            }
            break;
        case SDLK_5:
            if (characterView == CharacterView::Vrm3D && danceUi()) {
                selectDance(motion::DanceStyle::Idle);
            }
            break;
        case SDLK_A:
            if (characterView == CharacterView::Vrm3D && danceUi()) {
                setDanceMode(dance.mode() == motion::DanceMode::Auto ? motion::DanceMode::Manual
                                                                    : motion::DanceMode::Auto);
            }
            break;
        case SDLK_N:
            if (danceUi()) {
                dance.setOneShot(!dance.oneShot());
                showToast(dance.oneShot() ? "ONE SHOT: dance plays once, then Idle" : "LOOP");
            }
            break;
        case SDLK_E:
            if (danceUi()) {
                cycleExpression();
            }
            break;
        case SDLK_Y:
            if (danceUi()) {
                cycleOverlay();
            }
            break;
        case SDLK_J:
            if (danceUi()) {
                cycleGenerated();
            }
            break;
        case SDLK_K:
            if (characterView == CharacterView::Vrm3D) {
                toggleSpring();
            }
            break;
        case SDLK_M:
            musicDebug = !musicDebug;
            showToast(musicDebug ? "Music debug on" : "Music debug off");
            break;
        case SDLK_P:
            if (characterView == CharacterView::Vrm3D) {
                togglePresentation();
            }
            break;
        case SDLK_UP:
            if (characterView == CharacterView::Vrm3D && danceUi()) {
                changeDanceBpm(shift ? 10 : 5);
            }
            break;
        case SDLK_DOWN:
            if (characterView == CharacterView::Vrm3D && danceUi()) {
                changeDanceBpm(shift ? -10 : -5);
            }
            break;
        case SDLK_F5:
            dbgFeet = !dbgFeet;
            break;
        case SDLK_F6:
            dbgSkeleton = !dbgSkeleton;
            break;
        case SDLK_F7:
            dbgRoot = !dbgRoot;
            break;
        case SDLK_T:
            if (characterView == CharacterView::Vrm3D && danceUi()) {
                startTestDance();
                break;
            }
            if (!track) {
                setSyntheticDemo(syntheticDemo ? 0 : 123, true);
            } else {
                cycleMotionLab();
            }
            break;
        case SDLK_2:
            if (characterView == CharacterView::Vrm3D && danceUi()) {
                selectDance(motion::DanceStyle::HipHop);
                break;
            }
            setCharacterView(CharacterView::Robot2D, true);
            break;
        case SDLK_3:
            if (characterView == CharacterView::Vrm3D && danceUi()) {
                selectDance(motion::DanceStyle::SideStep);
                break;
            }
            setCharacterView(CharacterView::Vrm3D, true);
            break;
        case SDLK_F2:
            openVrmFileDialog();
            break;
        case SDLK_MINUS:
            if (characterView == CharacterView::Vrm3D) {
                adjustVrmScale(-0.05f);
            }
            break;
        case SDLK_EQUALS:
            if (characterView == CharacterView::Vrm3D) {
                adjustVrmScale(0.05f);
            }
            break;
        case SDLK_COMMA:
            if (characterView == CharacterView::Vrm3D) {
                adjustVrmOffset(-0.01f);
            }
            break;
        case SDLK_PERIOD:
            if (characterView == CharacterView::Vrm3D) {
                adjustVrmOffset(0.01f);
            }
            break;
        case SDLK_LEFTBRACKET:
            adjustLatency(-0.005);
            break;
        case SDLK_RIGHTBRACKET:
            adjustLatency(0.005);
            break;
        case SDLK_F:
            toggleFullscreen(!fullscreen);
            break;
        case SDLK_U:
            cycleUiScale();
            break;
        case SDLK_ESCAPE:
            if (presentation) {
                presentation = false;
                break;
            }
            break;
        default:
            break;
        }
    }

    void handleLeftDown(float x, float y, int clicks) {
        for (int i = 0; i < 4; ++i) {
            if (modulePills[i].w > 0.0f && modulePills[i].contains(x, y)) {
                setModule(static_cast<Module>(i));
                return;
            }
        }
        if (module == Module::Mix) {
            handleMixClick(x, y, clicks);
            return;
        }
        if (layout.mode2D.contains(x, y)) {
            setCharacterView(CharacterView::Robot2D, true);
            return;
        }
        if (layout.mode3D.contains(x, y)) {
            setCharacterView(CharacterView::Vrm3D, true);
            return;
        }
        if (layout.vrmLoadButton.contains(x, y)) {
            openVrmFileDialog();
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

        if (characterView == CharacterView::Vrm3D && danceUi()) {
            if (layout.danceManual.contains(x, y)) {
                setDanceMode(motion::DanceMode::Manual);
                return;
            }
            if (layout.danceAuto.contains(x, y)) {
                setDanceMode(motion::DanceMode::Auto);
                return;
            }
            if (layout.danceLoop.contains(x, y)) {
                dance.setOneShot(!dance.oneShot());
                return;
            }
            if (layout.danceTest.contains(x, y)) {
                startTestDance();
                return;
            }
            static constexpr motion::DanceStyle kStyles[5] = {
                motion::DanceStyle::Groove, motion::DanceStyle::HipHop,
                motion::DanceStyle::SideStep, motion::DanceStyle::Energetic,
                motion::DanceStyle::Idle};
            for (int i = 0; i < 5; ++i) {
                if (layout.danceStyle[i].contains(x, y)) {
                    selectDance(kStyles[i]);
                    return;
                }
            }
            if (layout.danceBpmMinus.contains(x, y)) {
                changeDanceBpm(-5);
                return;
            }
            if (layout.danceBpmPlus.contains(x, y)) {
                changeDanceBpm(5);
                return;
            }
            if (layout.danceExpr.contains(x, y)) {
                cycleExpression();
                return;
            }
            if (layout.danceLayer.contains(x, y)) {
                cycleOverlay();
                return;
            }
            if (layout.danceGen.contains(x, y)) {
                cycleGenerated();
                return;
            }
            if (layout.danceSpring.contains(x, y)) {
                toggleSpring();
                return;
            }
            if (layout.dbgFeetBtn.contains(x, y)) {
                dbgFeet = !dbgFeet;
                return;
            }
            if (layout.dbgSkelBtn.contains(x, y)) {
                dbgSkeleton = !dbgSkeleton;
                return;
            }
            if (layout.dbgRootBtn.contains(x, y)) {
                dbgRoot = !dbgRoot;
                return;
            }
            if (layout.springMinusBtn.contains(x, y)) {
                adjustSpringIntensity(-0.2f);
                return;
            }
            if (layout.springPlusBtn.contains(x, y)) {
                adjustSpringIntensity(0.2f);
                return;
            }
            if (layout.springDampBtn.contains(x, y)) {
                cycleSpringDamping();
                return;
            }
            if (layout.springResetBtn.contains(x, y)) {
                vrm.resetSecondary();
                showToast("Secondary dynamics reset");
                return;
            }
            if (layout.vrmDemoButton.contains(x, y)) {
                togglePresentation();
                return;
            }
        }
        if (characterView == CharacterView::Vrm3D) {
            if (layout.vrmLoadStage.contains(x, y) ||
                layout.vrmEmptyLoad.contains(x, y) ||
                layout.vrmRightLoad.contains(x, y)) {
                openVrmFileDialog();
                return;
            }
            if (layout.vrmResetCamera.contains(x, y)) {
                resetVrmCamera();
                return;
            }
            if (layout.vrmDemoButton.contains(x, y)) {
                if (track) {
                    cycleMotionLab();
                } else {
                    setSyntheticDemo(syntheticDemo ? 0 : 123, true);
                }
                return;
            }
            if (layout.vrmScaleMinus.contains(x, y)) {
                adjustVrmScale(-0.05f);
                return;
            }
            if (layout.vrmScalePlus.contains(x, y)) {
                adjustVrmScale(0.05f);
                return;
            }
            if (layout.vrmOffsetMinus.contains(x, y)) {
                adjustVrmOffset(-0.01f);
                return;
            }
            if (layout.vrmOffsetPlus.contains(x, y)) {
                adjustVrmOffset(0.01f);
                return;
            }
            if (layout.stage3D.contains(x, y)) {
                cameraDragging = true;
                cameraDragX = x;
                cameraDragY = y;
            }
            return;
        }

        if (state == AppState::Loading) {
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
        const float usable = std::max(120.0f, card.h - 60.0f);
        const float waveH = std::clamp(usable * 0.26f, 58.0f, 112.0f);
        const float specH = std::clamp(usable * 0.24f, 50.0f, 100.0f);
        layout.ruler = {x, card.y + 44.0f, w, 22.0f};
        layout.wave = {x, layout.ruler.bottom() + 4.0f, w, waveH};
        layout.spectrogram = {x, layout.wave.bottom() + 8.0f, w, specH};
        layout.tempo = {x, layout.spectrogram.bottom() + 8.0f, w, 16.0f};
        const float eventsY = layout.tempo.bottom() + 8.0f;
        layout.events = {x, eventsY, w,
                         std::max(34.0f, card.bottom() - 16.0f - eventsY)};
        layout.plot = {x, layout.wave.y, w, layout.events.bottom() - layout.wave.y};
    }

    void computeLayout() {
        const float W = canvasW;
        const float H = canvasH;
        const float margin = 24.0f;
        const float gap = 16.0f;
        const float contentY = 124.0f;
        const float footerY = H - 44.0f;
        const float contentBottom = footerY - 12.0f;
        const float width = W - margin * 2.0f;
        const float contentH = std::max(180.0f, contentBottom - contentY);

        float rightW = std::clamp(W * 0.27f, 400.0f, 480.0f);
        rightW = std::min(rightW, std::max(360.0f, width - gap - 540.0f));
        const float leftW = width - gap - rightW;
        const float rightX = margin + leftW + gap;

        layout.header = {0.0f, 0.0f, W, 64.0f};
        layout.toolbar = {margin, 76.0f, width, 44.0f};
        layout.footer = {0.0f, footerY, W, 44.0f};
        layout.empty = {margin, contentY, width, contentH};

        const float timelineH = contentH * 0.575f - gap * 0.5f;
        layout.timelineCard = {margin, contentY, leftW, timelineH};
        layout.choreoCard = {margin, layout.timelineCard.bottom() + gap, leftW,
                             contentBottom - layout.timelineCard.bottom() - gap};
        layout.stage3D = {margin, contentY, leftW, contentH};

        const float avail = contentH - gap * 3.0f;
        const float h1 = avail * 0.279f;
        const float h2 = avail * 0.276f;
        const float h3 = avail * 0.207f;
        layout.right1 = {rightX, contentY, rightW, h1};
        layout.right2 = {rightX, layout.right1.bottom() + gap, rightW, h2};
        layout.right3 = {rightX, layout.right2.bottom() + gap, rightW, h3};
        layout.right4 = {rightX, layout.right3.bottom() + gap, rightW,
                         contentBottom - layout.right3.bottom() - gap};

        const float rightEdge = margin + width;
        layout.loadButton = {margin, 80.0f, 116.0f, 40.0f};
        layout.playButton = {margin + 124.0f, 80.0f, 44.0f, 40.0f};
        layout.stopButton = {margin + 176.0f, 80.0f, 40.0f, 40.0f};
        layout.rateButton = {margin + 224.0f, 80.0f, 72.0f, 40.0f};
        layout.syncPlus = {rightEdge - 28.0f, 88.0f, 28.0f, 24.0f};
        layout.syncMinus = {layout.syncPlus.x - 108.0f, 88.0f, 28.0f, 24.0f};
        layout.vrmLoadButton = {layout.syncMinus.x - 92.0f, 80.0f, 68.0f, 40.0f};
        layout.mode3D = {layout.vrmLoadButton.x - 60.0f, 80.0f, 54.0f, 40.0f};
        layout.mode2D = {layout.mode3D.x - 60.0f, 80.0f, 54.0f, 40.0f};

        layout.emptyLoad = {layout.empty.centerX() - 86.0f,
                            layout.empty.y + layout.empty.h * 0.53f, 172.0f, 44.0f};

        const Rect card = layout.choreoCard;
        layout.motionLab = {card.right() - 104.0f, card.y + 10.0f, 90.0f, 24.0f};
        layout.motionDebug = {layout.motionLab.x - 76.0f, card.y + 10.0f, 70.0f, 24.0f};
        layout.motionSmooth = {layout.motionDebug.x - 104.0f, card.y + 10.0f, 98.0f, 24.0f};
        updateTimelineLayout();

        layout.vrmLoadStage = {layout.stage3D.right() - 116.0f,
                               layout.stage3D.y + 16.0f, 96.0f, 28.0f};
        layout.vrmEmptyLoad = {layout.stage3D.centerX() - 78.0f,
                               layout.stage3D.centerY() + 54.0f, 156.0f, 40.0f};
        const Rect right4 = layout.right4;
        layout.vrmRightLoad = {right4.x + 18.0f, right4.y + 34.0f, 88.0f, 28.0f};
        layout.vrmResetCamera = {right4.x + 114.0f, right4.y + 34.0f, 96.0f, 28.0f};
        layout.vrmDemoButton = {right4.x + 218.0f, right4.y + 34.0f, 136.0f, 28.0f};
        layout.vrmScaleMinus = {right4.x + 74.0f, right4.y + 82.0f, 26.0f, 24.0f};
        layout.vrmScalePlus = {right4.x + 104.0f, right4.y + 82.0f, 26.0f, 24.0f};
        layout.vrmOffsetMinus = {right4.x + 268.0f, right4.y + 82.0f, 26.0f, 24.0f};
        layout.vrmOffsetPlus = {right4.x + 298.0f, right4.y + 82.0f, 26.0f, 24.0f};

        const Rect r1 = layout.right1;
        layout.danceManual = {r1.x + 18.0f, r1.y + 38.0f, 78.0f, 26.0f};
        layout.danceAuto = {r1.x + 100.0f, r1.y + 38.0f, 78.0f, 26.0f};
        layout.danceLoop = {r1.x + 182.0f, r1.y + 38.0f, 78.0f, 26.0f};
        layout.danceTest = {r1.x + 264.0f, r1.y + 38.0f, 90.0f, 26.0f};
        for (int i = 0; i < 5; ++i) {
            layout.danceStyle[i] = {r1.x + 18.0f + static_cast<float>(i) * 68.0f,
                                    r1.y + 72.0f, 64.0f, 28.0f};
        }
        layout.danceBpmMinus = {r1.x + 110.0f, r1.y + 110.0f, 26.0f, 24.0f};
        layout.danceBpmPlus = {r1.x + 140.0f, r1.y + 110.0f, 26.0f, 24.0f};
        layout.danceExpr = {r1.x + 244.0f, r1.y + 110.0f, 108.0f, 24.0f};
        layout.danceLayer = {r1.x + 18.0f, r1.y + 142.0f, 104.0f, 24.0f};
        layout.danceGen = {r1.x + 128.0f, r1.y + 142.0f, 96.0f, 24.0f};
        layout.danceSpring = {r1.x + 230.0f, r1.y + 142.0f, 124.0f, 24.0f};
        const Rect r3 = layout.right3;
        layout.dbgFeetBtn = {r3.x + 18.0f, r3.y + 34.0f, 106.0f, 24.0f};
        layout.dbgSkelBtn = {r3.x + 130.0f, r3.y + 34.0f, 104.0f, 24.0f};
        layout.dbgRootBtn = {r3.x + 240.0f, r3.y + 34.0f, 114.0f, 24.0f};
        layout.springMinusBtn = {r3.x + 18.0f, r3.y + 62.0f, 44.0f, 18.0f};
        layout.springPlusBtn = {r3.x + 66.0f, r3.y + 62.0f, 44.0f, 18.0f};
        layout.springDampBtn = {r3.x + 114.0f, r3.y + 62.0f, 92.0f, 18.0f};
        layout.springResetBtn = {r3.x + 210.0f, r3.y + 62.0f, 144.0f, 18.0f};

        computeMixLayout();
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

    // BG Mix 0.1 screen
    void computeMixLayout();
    void drawMixScreen();
    void drawMixDeck(const Rect& card, mix::DeckId deck, bool incoming);
    void drawMixQueue(const Rect& card, const mix::MixSessionSnapshot& snap);
    void drawMixPlan(const Rect& card, const mix::MixSessionSnapshot& snap);
    void drawMixControls(const Rect& card, const mix::MixSessionSnapshot& snap);
    void handleMixClick(float x, float y, int clicks);
    void drawMixButton(const Rect& rect, std::string_view caption, bool active,
                       SDL_Color color, int icon = -1);
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
    ui::fillRect(renderer, {0, 0, canvasW, canvasH}, theme::background);
    ui::verticalGradient(renderer, {0, 0, canvasW, canvasH},
                         SDL_Color{11, 15, 23, 255}, SDL_Color{5, 7, 11, 255});
    // A restrained technical grid.
    for (float x = 0; x < canvasW; x += 72) {
        SDL_SetRenderDrawColor(renderer, 44, 58, 78, 18);
        SDL_RenderLine(renderer, x, 0, x, canvasH);
    }
    for (float y = 0; y < canvasH; y += 72) {
        SDL_SetRenderDrawColor(renderer, 44, 58, 78, 14);
        SDL_RenderLine(renderer, 0, y, canvasW, y);
    }
    ui::fillCircle(renderer, canvasW - 140.0f, 40.0f, 420,
                   theme::alpha(theme::accent, 0.025f));
    ui::fillCircle(renderer, 90, canvasH - 50.0f, 330,
                   theme::alpha(theme::violet, 0.035f));
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
    const float right = canvasW - 24.0f;
    struct Pill {
        const char* name;
        SDL_Color color;
        float width;
        Module module;
    };
    const Pill pills[] = {
        {"BG BEAT", theme::violet, 72.0f, Module::Beat},
        {"BG MOTION", theme::accent, 82.0f, Module::Motion},
        {"BG VRM", theme::pink, 68.0f, Module::Vrm},
        {"BG MIX", theme::amber, 68.0f, Module::Mix},
    };
    float x = right;
    for (int i = static_cast<int>(std::size(pills)) - 1; i >= 0; --i) {
        const Pill& pill = pills[i];
        const Rect rect{x - pill.width, 18, pill.width, 26};
        modulePills[i] = rect;
        const bool active = module == pill.module;
        ui::fillRoundedRect(renderer, rect, 13.0f,
                            theme::alpha(pill.color, active ? 0.18f : 0.09f));
        ui::strokeRoundedRect(renderer, rect, 13.0f,
                              theme::alpha(pill.color, active ? 0.62f : 0.30f),
                              active ? 1.5f : 1.0f);
        text->drawCentered(pill.name, rect.centerX(), rect.y + 7, 9,
                           FontWeight::SemiBold,
                           theme::alpha(pill.color, active ? 1.0f : 0.9f));
        x -= pill.width + 8.0f;
    }

    std::string status;
    SDL_Color statusColor = theme::accent;
    if (module == Module::Mix) {
        const mix::MixSessionSnapshot mixSnap =
            mixSession ? mixSession->snapshot() : mix::MixSessionSnapshot{};
        const bool bothDecks = mixSnap.engine.decks[0].loaded &&
                               mixSnap.engine.decks[1].loaded;
        status = bothDecks ? "2 DECKS"
                           : (mixSnap.queue.empty() ? "NO TRACK" : "MIX QUEUE");
        statusColor = mixSnap.queue.empty() ? theme::accent : theme::amber;
    } else if (state == AppState::Loading) {
        status = "ANALYSING";
        statusColor = theme::amber;
    } else if (state == AppState::Error) {
        status = "ERROR";
        statusColor = theme::red;
    } else if (state == AppState::Ready) {
        status = "READY";
        statusColor = theme::accent;
    } else {
        status = "NO TRACK";
        statusColor = theme::accent;
    }
    const float statusWidth = 92.0f;
    const Rect statusRect{x - statusWidth - 10.0f, 18, statusWidth, 26};
    if (statusRect.x > 420.0f) {
        ui::fillRoundedRect(renderer, statusRect, 13.0f,
                            theme::alpha(statusColor, 0.08f));
        ui::strokeRoundedRect(renderer, statusRect, 13.0f,
                              theme::alpha(statusColor, 0.28f), 1.0f);
        ui::fillCircle(renderer, statusRect.x + 12.0f, statusRect.centerY(), 3.0f,
                       statusColor);
        text->draw(status, statusRect.x + 21.0f, statusRect.y + 7, 9,
                   FontWeight::SemiBold, statusColor);
    }
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

    const float margin = layout.loadButton.x;
    const double pos = currentPosition();
    const double dur = duration();
    text->draw(formatTime(pos, true), margin + 318, 82, 26, FontWeight::SemiBold,
               theme::text);
    text->draw("/ " + formatTime(dur), margin + 446, 92, 11, FontWeight::Regular,
               theme::muted);
    label("MEDIA TIME", margin + 319, 111, 8, theme::dim, 1.4f);

    const float infoX = margin + 566.0f;
    const float infoRight = layout.mode2D.x - 22.0f;
    const float infoWidth = std::max(120.0f, infoRight - infoX);
    if (track) {
        const std::string name = ellipsize(*text, track->displayName, infoWidth, 15,
                                           FontWeight::SemiBold);
        body(name, infoX, 82, 15, theme::text, FontWeight::SemiBold);
        const std::string path = ellipsize(*text, track->displayPath, infoWidth - 10.0f,
                                           10, FontWeight::Regular);
        body(path, infoX, 102, 10, theme::muted);
        label("CACHE " + cacheName(track->cacheStatus, track->fromCache),
              infoX + 385.0f, 90, 9,
              track->fromCache ? theme::accent : theme::dim, 1.1f);
    } else {
        body("No track loaded", infoX, 87, 14, theme::muted);
        label("DROP WAV / MP3 / FLAC ANYWHERE", infoX, 104, 9, theme::dim, 1.2f);
    }

    drawModeChip(layout.mode2D, "2D", characterView == CharacterView::Robot2D,
                 theme::violet);
    drawModeChip(layout.mode3D, "3D", characterView == CharacterView::Vrm3D,
                 theme::accent);
    drawButton(layout.vrmLoadButton, "VRM", false, vrm.ready(), 0);

    // Sync trim.
    label("SYNC TRIM", layout.syncMinus.x, 72, 8, theme::dim, 1.4f);
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

    // BG Motion 3.1 accent map: real onset timestamps that are not simply the
    // beat or the bass row (syncopations and subdivisions). The debug panel
    // exposes the same map numerically.
    {
        const auto& accents = dance.timeline().accents();
        int lastAccentRowX = -100;
        for (const motion::AccentEvent& accent : accents) {
            if (accent.time < viewStart || accent.time > viewEnd ||
                accent.strength < 0.10f || accent.downbeat || accent.bass ||
                accent.kind == motion::AccentKind::Beat) {
                continue;
            }
            const int x = static_cast<int>(timeToX(accent.time, layout.plot));
            if (x == lastAccentRowX) {
                continue;
            }
            lastAccentRowX = x;
            const Uint8 alpha = static_cast<Uint8>(
                std::clamp(90.0f + 150.0f * accent.strength, 0.0f, 245.0f));
            ui::diamond(renderer, static_cast<float>(x), beatY + 19.0f, 2.2f,
                        SDL_Color{166, 132, 255, alpha});
        }
        label("ACC", rect.x + 6.0f, beatY + 12.0f, 7,
              theme::alpha(theme::violet, 0.8f), 0.8f);
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
    const float legendY = card.bottom() - 26.0f;
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
// 3D character view
// -------------------------------------------------------------------------
void DesktopApp::Impl::drawModeChip(const Rect& rect, std::string_view labelText,
                                    bool active, SDL_Color color) {
    const bool isHover = hover(rect);
    ui::fillRoundedRect(renderer, rect, 8.0f,
                        theme::alpha(color, active ? 0.16f
                                                  : (isHover ? 0.09f : 0.035f)));
    ui::strokeRoundedRect(renderer, rect, 8.0f,
                          theme::alpha(color, active ? 0.48f : 0.18f), 1.0f);
    text->drawCentered(labelText, rect.centerX(), rect.y + 14.0f, 10,
                       FontWeight::SemiBold, active ? color : theme::muted);
}

void DesktopApp::Impl::draw3DStage() {
    const Rect stage = layout.stage3D;
    ui::fillRoundedRect(renderer, stage, 12.0f, theme::panel);
    const Rect sceneRect{stage.x + 2.0f, stage.y + 2.0f, stage.w - 4.0f,
                         stage.h - 4.0f};
    std::string drawError;
    const bool hasFrame = vrm.ready() && vrm.renderable() &&
                          vrm.draw(renderer, ui::fr(sceneRect), &drawError);
    if (!hasFrame) {
        ui::verticalGradient(renderer, sceneRect, SDL_Color{12, 20, 33, 255},
                             SDL_Color{4, 7, 12, 255});
        for (int i = 1; i < 12; ++i) {
            const float y = sceneRect.y + sceneRect.h * (static_cast<float>(i) / 12.0f);
            ui::thickLine(renderer, sceneRect.x, y, sceneRect.right(), y, 1.0f,
                          theme::alpha(theme::white, 0.025f), false);
        }
    }
    ui::strokeRoundedRect(renderer, stage, 12.0f, theme::border, 1.0f);

    // Readability gradients over the live 3D frame.
    ui::verticalGradient(renderer, {sceneRect.x, sceneRect.y, sceneRect.w, 104.0f},
                         theme::alpha(theme::background, 0.80f),
                         theme::alpha(theme::background, 0.0f));
    ui::verticalGradient(renderer,
                         {sceneRect.x, sceneRect.bottom() - 128.0f, sceneRect.w, 128.0f},
                         theme::alpha(theme::background, 0.0f),
                         theme::alpha(theme::background, 0.88f));

    const vrm::VrmModel* loaded = vrm.model();
    const float left = stage.x + 22.0f;
    text->draw("BG MEDIA 3D", left, stage.y + 18.0f, 15,
               FontWeight::SemiBold, theme::accent);
    label("VRM 0.x  ·  SDL_GPU", left + 138.0f, stage.y + 23.0f, 8, theme::dim,
          1.2f);
    std::string modelTitle = "NO VRM MODEL";
    if (loaded) {
        modelTitle = loaded->name.empty() ? "VRM CHARACTER" : loaded->name;
    }
    body(ellipsize(*text, modelTitle, stage.w - 360.0f, 14, FontWeight::SemiBold),
         left, stage.y + 40.0f, 14, theme::text, FontWeight::SemiBold);
    std::string modelLine;
    if (loaded) {
        modelLine = std::to_string(loaded->diagnostics.nodeCount) + " nodes  ·  " +
                    std::to_string(loaded->diagnostics.humanoidCount) +
                    " humanoid bones  ·  " +
                    std::to_string(loaded->diagnostics.triangleCount) + " tris";
        if (!loaded->metadata.licenseName.empty() &&
            (loaded->metadata.allowedUserName == "OnlyAuthor" ||
             loaded->metadata.licenseName.find("Redistribution") !=
                 std::string::npos)) {
            modelLine += "  ·  PRIVATE / LOCAL";
        }
    } else {
        modelLine = vrmError.empty() ? "Load a VRM file to begin" : vrmError;
    }
    body(ellipsize(*text, modelLine, stage.w - 260.0f, 10, FontWeight::Regular),
         left, stage.y + 60.0f, 10, theme::muted);

    // Stage-level load button.
    const Rect loadRect = layout.vrmLoadStage;
    ui::fillRoundedRect(renderer, loadRect, 7.0f,
                        hover(loadRect) ? theme::panelHover
                                        : theme::alpha(theme::panelRaised, 0.92f));
    ui::strokeRoundedRect(renderer, loadRect, 7.0f,
                          theme::alpha(theme::accent, 0.32f), 1.0f);
    text->drawCentered("LOAD VRM", loadRect.centerX(), loadRect.y + 8.0f, 9,
                       FontWeight::SemiBold, theme::accent);

    if (!hasFrame) {
        const Rect card{stage.centerX() - 250.0f, stage.centerY() - 92.0f, 500.0f,
                        190.0f};
        ui::fillRoundedRect(renderer, card, 10.0f, theme::alpha(theme::panel, 0.94f));
        ui::strokeRoundedRect(renderer, card, 10.0f,
                              theme::alpha(theme::pink, 0.28f), 1.0f);
        label("3D CHARACTER IMPORT", card.x + 20.0f, card.y + 18.0f, 9, theme::pink,
              1.4f);
        text->draw(loaded ? "GPU FRAME UNAVAILABLE" : "VRM MODEL REQUIRED",
                   card.x + 20.0f, card.y + 38.0f, 18, FontWeight::SemiBold,
                   theme::text);
        const std::string message =
            loaded ? (vrmFrameError.empty() ? drawError : vrmFrameError) : vrmError;
        body(ellipsize(*text, message, card.w - 40.0f, 11, FontWeight::Regular),
             card.x + 20.0f, card.y + 70.0f, 11, theme::muted);
        body("The character is imported into the engine; the 3D view needs the "
             "SDL_GPU device.",
             card.x + 20.0f, card.y + 96.0f, 10, theme::dim);
        const Rect button = layout.vrmEmptyLoad;
        ui::fillRoundedRect(renderer, button, 8.0f,
                            hover(button) ? theme::mix(theme::accent, theme::white, 0.12f)
                                          : theme::accent);
        text->drawCentered("CHOOSE VRM FILE", button.centerX(), button.y + 13.0f, 10,
                           FontWeight::SemiBold, SDL_Color{8, 18, 16, 255});
    }

    // Bottom transport / motion strip.
    const float bottom = stage.bottom();
    const double pos = vrmMediaTime();
    const double total = duration();
    CharacterPose pose = sampleActiveAt(pos);
    std::string move = moveName(pose.move);
    auto [bar, beat] = currentBarBeat();
    if (danceUi()) {
        move = clipTitle(danceFrame);
        pose.accented = danceFrame.pose.accent > 0.5f;
        const double beatPosition = std::floor(dance.director().beatClock().beatAt(pos));
        bar = static_cast<int>(std::floor(beatPosition / 4.0)) + 1;
        beat = dance.director().beatClock().beatInBar(beatPosition) + 1;
    }
    text->draw(move, left, bottom - 96.0f, 28, FontWeight::SemiBold,
               pose.accented ? theme::amber : theme::text);
    std::string syncState;
    SDL_Color stateColor = theme::accent;
    if (danceUi() && !track) {
        syncState = std::string("MOTION LAB  ·  NO MUSIC  ·  ") + std::to_string(danceLabBpm) +
                    " BPM  ·  " + (danceLabRunning ? "RUNNING" : "PAUSED");
        stateColor = theme::pink;
    } else if (!track && syntheticDemo) {
        syncState = "SYNTHETIC BEAT GRID  ·  " + std::to_string(motionLabBpm) +
                " BPM  ·  BG MOTION";
        stateColor = theme::pink;
    } else if (track) {
        syncState = std::string("BG SYNC  ·  ") +
                (playing() ? "PLAYING" : "PAUSED") + "  ·  " +
                formatNumber(player.snapshot().rate, 2) + "x";
        if (!audioError.empty()) {
            syncState = "BG SYNC  ·  AUDIO UNAVAILABLE";
            stateColor = theme::amber;
        }
    } else {
        syncState = "IDLE PREVIEW  ·  NO TRACK";
        stateColor = theme::muted;
    }
    label(syncState, left, bottom - 62.0f, 9, stateColor, 1.1f);
    const std::string barBeat =
        (bar > 0 ? "BAR " + std::to_string(bar) + "  ·  BEAT " +
                       std::to_string(beat)
                 : "NO BEAT") +
        (pose.accented ? "  ·  BASS ACCENT" : "");
    body(barBeat, left, bottom - 44.0f, 11, theme::muted);

    const auto& frame = vrm.frameStats();
    std::string rightInfo = formatNumber(fpsSmoothed, 0) + " FPS";
    if (frame.valid) {
        rightInfo += "  ·  GPU " + formatNumber(frame.gpuMilliseconds, 1) + " ms";
    }
    labelRight(rightInfo, stage.right() - 22.0f, bottom - 96.0f, 10,
               frame.valid ? theme::accent : theme::dim, 0.5f);
    labelRight(formatTime(pos, true) + " / " + formatTime(total),
               stage.right() - 22.0f, bottom - 78.0f, 10, theme::muted);
    if (loaded) {
        labelRight(std::to_string(loaded->diagnostics.triangleCount) + " tris  ·  " +
                       std::to_string(frame.drawCalls) + " draw calls  ·  " +
                       formatNumber(vrm.lastRetargetMs(), 2) + " ms retarget",
                   stage.right() - 22.0f, bottom - 59.0f, 9, theme::dim);
        const std::string asset = std::to_string(loaded->diagnostics.meshCount) +
                                  " meshes  ·  " +
                                  std::to_string(loaded->diagnostics.materialCount) +
                                  " materials  ·  " +
                                  std::to_string(loaded->diagnostics.decodedTextureCount) +
                                  "/" + std::to_string(loaded->diagnostics.textureCount) +
                                  " textures  ·  " +
                                  formatNumber(loaded->height, 2) + " m";
        labelRight(asset, stage.right() - 22.0f, bottom - 43.0f, 9, theme::dim);
    }
    if (!vrm.warning().empty()) {
        const std::string warning =
            "IMPORT WARNINGS  ·  " + std::to_string(std::count(
                vrm.warning().begin(), vrm.warning().end(), '|') + 1);
        const float width = text->measure(warning, 8, FontWeight::SemiBold) + 18.0f;
        const Rect badge{stage.right() - 22.0f - width, stage.y + 54.0f, width, 18.0f};
        ui::fillRoundedRect(renderer, badge, 9.0f, theme::alpha(theme::amber, 0.10f));
        ui::strokeRoundedRect(renderer, badge, 9.0f, theme::alpha(theme::amber, 0.32f),
                              1.0f);
        text->drawCentered(warning, badge.centerX(), badge.y + 5.0f, 8,
                           FontWeight::SemiBold, theme::amber);
    }
    if (!vrmFrameError.empty()) {
        label("RENDER ERROR  ·  " +
                  ellipsize(*text, vrmFrameError, stage.w - 90.0f, 9,
                            FontWeight::SemiBold),
              left, bottom - 24.0f, 9, theme::red, 0.5f);
    }
    label("DRAG ORBIT   ·   WHEEL ZOOM   ·   C RESET   ·   G 2D/3D   ·   P PRESENT",
          left, bottom - 14.0f, 8, theme::dim, 0.8f);
    if (danceUi() && (dbgFeet || dbgSkeleton || dbgRoot)) {
        drawDanceDebug(stage);
    }

    if (total > 0.001) {
        const Rect trackBar{stage.x + 22.0f, stage.bottom() - 7.0f,
                            stage.w - 44.0f, 2.0f};
        ui::fillRoundedRect(renderer, trackBar, 1.0f,
                            theme::alpha(theme::white, 0.08f));
        const float fraction =
            static_cast<float>(std::clamp(pos / total, 0.0, 1.0));
        ui::fillRoundedRect(renderer,
                            {trackBar.x, trackBar.y, trackBar.w * fraction,
                             trackBar.h},
                            1.0f, theme::accent);
    }
}

void DesktopApp::Impl::draw3DRightPanels() {
    const auto panelCard = [&](const Rect& rect) {
        ui::fillRoundedRect(renderer, rect, 12.0f, theme::panel);
        ui::strokeRoundedRect(renderer, rect, 12.0f, theme::border, 1.0f);
    };
    const auto controlButton = [&](const Rect& rect, const std::string& caption,
                                   bool active, SDL_Color color) {
        ui::fillRoundedRect(renderer, rect, 6.0f,
                            theme::alpha(color, hover(rect) ? 0.18f
                                                            : (active ? 0.12f
                                                                      : 0.045f)));
        ui::strokeRoundedRect(renderer, rect, 6.0f,
                              theme::alpha(color, active ? 0.44f : 0.16f), 1.0f);
        text->drawCentered(caption, rect.centerX(), rect.y + 8.0f, 8,
                           FontWeight::SemiBold, active ? color : theme::muted);
    };

    const vrm::VrmModel* loaded = vrm.model();

    // 1. Character / import.
    if (!danceUi()) {
        const Rect card = layout.right1;
        panelCard(card);
        text->draw("VRM CHARACTER", card.x + 18.0f, card.y + 14.0f, 11,
                   FontWeight::SemiBold, theme::text);
        label("IMPORT / RIG", card.x + 152.0f, card.y + 17.0f, 8, theme::accent,
              1.2f);
        float y = card.y + 40.0f;
        if (loaded) {
            body(ellipsize(*text, loaded->name, card.w - 36.0f, 15,
                           FontWeight::SemiBold),
                 card.x + 18.0f, y, 15, theme::text, FontWeight::SemiBold);
            y += 24.0f;
            label("SPEC", card.x + 18.0f, y, 8, theme::dim, 1.1f);
            body("VRM " + loaded->specVersion + "  ·  glTF 2.0  ·  " +
                     (loaded->materials.empty() ? "fallback materials" : "MToon"),
                 card.x + 66.0f, y - 1.0f, 10, theme::muted);
            y += 19.0f;
            label("SKELETON", card.x + 18.0f, y, 8, theme::dim, 1.1f);
            body(std::to_string(loaded->diagnostics.nodeCount) + " nodes  ·  " +
                     std::to_string(loaded->diagnostics.humanoidCount) +
                     " humanoid / " +
                     std::to_string(loaded->diagnostics.humanoidCoreCount) +
                     " core  ·  " + std::to_string(loaded->diagnostics.skinCount) +
                     " skins",
                 card.x + 90.0f, y - 1.0f, 10, theme::muted);
            y += 19.0f;
            label("GEOMETRY", card.x + 18.0f, y, 8, theme::dim, 1.1f);
            body(std::to_string(loaded->diagnostics.meshCount) + " meshes  ·  " +
                     std::to_string(loaded->diagnostics.triangleCount) +
                     " tris  ·  " + std::to_string(loaded->diagnostics.vertexCount) +
                     " verts",
                 card.x + 90.0f, y - 1.0f, 10, theme::muted);
            y += 19.0f;
            label("MATERIALS", card.x + 18.0f, y, 8, theme::dim, 1.1f);
            body(std::to_string(loaded->diagnostics.materialCount) + " mats  ·  " +
                     std::to_string(loaded->diagnostics.decodedTextureCount) + "/" +
                     std::to_string(loaded->diagnostics.textureCount) + " textures  ·  " +
                     formatNumber(loaded->height, 2) + " m",
                 card.x + 100.0f, y - 1.0f, 10, theme::muted);
            y += 19.0f;
            const std::string license =
                loaded->metadata.licenseName.empty()
                    ? "license not specified"
                    : loaded->metadata.licenseName + " / " +
                          loaded->metadata.allowedUserName;
            label("LICENSE", card.x + 18.0f, y, 8, theme::dim, 1.1f);
            body(ellipsize(*text, license, card.w - 112.0f, 9, FontWeight::Regular),
                 card.x + 90.0f, y - 1.0f, 9,
                 (loaded->metadata.allowedUserName == "OnlyAuthor" ||
                  loaded->metadata.licenseName.find("Redistribution") !=
                      std::string::npos)
                     ? theme::amber
                     : theme::muted);
            if (!vrm.warning().empty()) {
                const Rect warn{card.x + 18.0f, card.bottom() - 26.0f,
                                card.w - 36.0f, 18.0f};
                ui::fillRoundedRect(renderer, warn, 5.0f,
                                    theme::alpha(theme::amber, 0.08f));
                text->drawCentered(
                    ellipsize(*text, "IMPORT WARNINGS: " + vrm.warning(),
                              warn.w - 14.0f, 8, FontWeight::SemiBold),
                    warn.centerX(), warn.y + 5.0f, 8, FontWeight::SemiBold,
                    theme::amber);
            }
        } else {
            label("NO MODEL", card.x + 18.0f, y, 9, theme::pink, 1.3f);
            y += 20.0f;
            body(ellipsize(*text, vrmError.empty() ? "Use LOAD VRM." : vrmError,
                           card.w - 36.0f, 11, FontWeight::Regular),
                 card.x + 18.0f, y, 11, theme::muted);
        }
    }

    // 2. BG Sync and motion.
    if (!danceUi()) {
        const Rect card = layout.right2;
        panelCard(card);
        text->draw("BG SYNC / MOTION", card.x + 18.0f, card.y + 14.0f, 11,
                   FontWeight::SemiBold, theme::text);
        label(track ? "ANALYSED TRACK" : (syntheticDemo ? "SYNTHETIC" : "IDLE"),
              card.x + 190.0f, card.y + 17.0f, 8,
              track ? theme::violet : (syntheticDemo ? theme::pink : theme::dim), 1.0f);
        const double pos = vrmMediaTime();
        const double total = duration();
        const CharacterPose pose = sampleActiveAt(pos);
        float y = card.y + 40.0f;
        label("SOURCE", card.x + 18.0f, y, 8, theme::dim, 1.1f);
        const std::string source =
            track ? track->displayName
                  : (syntheticDemo
                         ? "Synthetic dance · " + std::to_string(motionLabBpm) + " BPM"
                         : "No music loaded");
        body(ellipsize(*text, source, card.w - 98.0f, 10, FontWeight::Regular),
             card.x + 84.0f, y - 1.0f, 10, theme::muted);
        y += 19.0f;
        label("MEDIA", card.x + 18.0f, y, 8, theme::dim, 1.1f);
        body(formatTime(pos, true) + " / " + formatTime(total), card.x + 84.0f,
             y - 1.0f, 10, theme::muted);
        y += 19.0f;
        label("STATE", card.x + 18.0f, y, 8, theme::dim, 1.1f);
        std::string sync = "IDLE PREVIEW";
        SDL_Color syncColor = theme::dim;
        if (track) {
            sync = std::string("BG SYNC · ") + (playing() ? "PLAYING" : "PAUSED") +
                   " · " + formatNumber(player.snapshot().rate, 2) + "x";
            syncColor = playing() ? theme::accent : theme::amber;
        } else if (syntheticDemo) {
            sync = std::string("BG MOTION · ") +
                   (syntheticPlaying ? "PLAYING" : "PAUSED");
            syncColor = syntheticPlaying ? theme::pink : theme::amber;
        }
        body(sync, card.x + 84.0f, y - 1.0f, 10, syncColor);
        y += 25.0f;
        ui::thickLine(renderer, card.x + 18.0f, y, card.right() - 18.0f, y, 1.0f,
                      theme::alpha(theme::white, 0.06f), false);
        y += 14.0f;
        text->draw(moveName(pose.move), card.x + 18.0f, y, 21,
                   FontWeight::SemiBold,
                   pose.accented ? theme::amber : theme::text);
        y += 30.0f;
        body("ACCENT " + std::string(pose.accented ? "ON" : "вЂ”") +
                 "   ·   ENERGY " + formatNumber(pose.energy, 2) +
                 "   ·   IMPACT " + formatNumber(pose.impact, 2),
             card.x + 18.0f, y, 9, theme::muted);
        y += 20.0f;
        const auto& retarget = vrm.lastPose().stats;
        body("FOOT IK " + std::string(retarget.footIK ? "ON" : "OFF") +
                 "   ·   ERR " + formatNumber(retarget.maxFootError * 1000.0f, 1) +
                 " mm   ·   RETARGET " + formatNumber(vrm.lastRetargetMs(), 2) + " ms",
             card.x + 18.0f, y, 9, theme::muted);
    }

    // 3. Renderer.
    if (!danceUi()) {
        const Rect card = layout.right3;
        panelCard(card);
        text->draw("SDL_GPU RENDERER", card.x + 18.0f, card.y + 14.0f, 11,
                   FontWeight::SemiBold, theme::text);
        const auto& frame = vrm.frameStats();
        label(frame.valid ? "FRAME READY" : "OFFLINE",
              card.x + 172.0f, card.y + 17.0f, 8,
              frame.valid ? theme::accent : theme::amber, 1.0f);
        float y = card.y + 40.0f;
        label("DEVICE", card.x + 18.0f, y, 8, theme::dim, 1.1f);
        body((frame.driver.empty() ? std::string("D3D12") : frame.driver) +
                 "  ·  " + std::to_string(frame.width) + "x" +
                 std::to_string(frame.height) + " offscreen  ·  D32 depth",
             card.x + 78.0f, y - 1.0f, 9, theme::muted);
        y += 19.0f;
        label("FRAME", card.x + 18.0f, y, 8, theme::dim, 1.1f);
        body(formatNumber(fpsSmoothed, 0) + " FPS  ·  GPU " +
                 formatNumber(frame.gpuMilliseconds, 1) + " ms  ·  readback " +
                 formatNumber(frame.readbackMilliseconds, 2) + " ms",
             card.x + 78.0f, y - 1.0f, 9, theme::muted);
        y += 19.0f;
        label("GEOMETRY", card.x + 18.0f, y, 8, theme::dim, 1.1f);
        body(std::to_string(frame.drawCalls) + " draws  ·  " +
                 std::to_string(frame.triangles) + " tris  ·  " +
                 std::to_string(frame.skinCount) + " skins",
             card.x + 90.0f, y - 1.0f, 9, theme::muted);
        y += 19.0f;
        label("MEMORY", card.x + 18.0f, y, 8, theme::dim, 1.1f);
        body(std::to_string(frame.textureCount) + " GPU textures  ·  " +
                 formatNumber(static_cast<double>(frame.textureBytes) /
                                  (1024.0 * 1024.0),
                              0) +
                 " MB VRAM  ·  RSS " + formatNumber(residentMemoryMb(), 0) + " MB",
             card.x + 78.0f, y - 1.0f, 9, theme::muted);
        if (!vrmFrameError.empty()) {
            body(ellipsize(*text, vrmFrameError, card.w - 36.0f, 8, FontWeight::Regular),
                 card.x + 18.0f, card.bottom() - 20.0f, 8, theme::red);
        }
    }

    // 4. Controls.
    {
        const Rect card = layout.right4;
        panelCard(card);
        text->draw("3D CONTROLS", card.x + 18.0f, card.y + 12.0f, 11,
                   FontWeight::SemiBold, theme::text);
        label("LOCAL VRM", card.right() - 86.0f, card.y + 15.0f, 8, theme::dim, 0.8f);
        controlButton(layout.vrmRightLoad, "LOAD VRM", false, theme::accent);
        controlButton(layout.vrmResetCamera, "RESET CAM", false, theme::violet);
        std::string demoLabel;
        bool demoActive = false;
        SDL_Color demoColor = theme::pink;
        if (danceUi()) {
            demoLabel = "PRESENT (P)";
            demoActive = presentation;
            demoColor = theme::accent;
        } else if (track) {
            demoLabel = motionLabBpm > 0 ? "LAB " + std::to_string(motionLabBpm)
                                         : "MOTION LAB";
            demoActive = motionLabBpm > 0;
        } else {
            demoLabel = syntheticDemo
                            ? "DEMO " + std::to_string(motionLabBpm)
                            : "TEST DANCE";
            demoActive = syntheticDemo;
        }
        controlButton(layout.vrmDemoButton, demoLabel, demoActive, demoColor);

        label("SCALE", card.x + 18.0f, card.y + 90.0f, 8, theme::dim, 1.1f);
        controlButton(layout.vrmScaleMinus, "-", false, theme::muted);
        controlButton(layout.vrmScalePlus, "+", false, theme::muted);
        body(formatNumber(vrm.renderSettings().modelScale, 2) + "x",
             card.x + 138.0f, card.y + 86.0f, 10, theme::text);
        label("HEIGHT", card.x + 186.0f, card.y + 90.0f, 8, theme::dim, 1.1f);
        controlButton(layout.vrmOffsetMinus, "-", false, theme::muted);
        controlButton(layout.vrmOffsetPlus, "+", false, theme::muted);
        body((vrm.renderSettings().modelOffset.y >= 0.0f ? "+" : "") +
                 formatNumber(vrm.renderSettings().modelOffset.y, 2) + " m",
             card.x + 330.0f, card.y + 86.0f, 9, theme::text);
        label("G 2D/3D  ·  C CAMERA  ·  -/+ SCALE  ·  ,/. HEIGHT  ·  T TEST",
              card.x + 18.0f, card.bottom() - 26.0f, 8, theme::dim, 0.5f);
        const std::string renderState =
            vrm.ready() ? (vrm.renderable() ? "3D READY" : "IMPORT ONLY")
                        : "NO MODEL";
        labelRight(renderState, card.right() - 18.0f, card.bottom() - 44.0f, 8,
                   vrm.renderable() ? theme::accent : theme::amber, 0.8f);
    }
}

// -------------------------------------------------------------------------
// BG MIX 0.1 screen
// -------------------------------------------------------------------------
void DesktopApp::Impl::computeMixLayout() {
    const float margin = 24.0f;
    const float gap = 16.0f;
    const float top = 76.0f;
    const float footerY = canvasH - 44.0f;
    const float bottom = footerY - 12.0f;
    const float contentH = std::max(220.0f, bottom - top);

    const MixColumns columns = computeMixColumns(canvasW);
    float queueW = columns.queueW;
    float rightW = columns.rightW;
    float centerW = columns.centerW;

    const Rect queue{ margin, top, queueW, contentH };
    const Rect deckA{ margin + queueW + gap, top, centerW,
                      (contentH - gap) * 0.5f };
    const Rect deckB{ deckA.x, deckA.bottom() + gap, centerW,
                      (contentH - gap) * 0.5f };
    const float rightX = deckA.right() + gap;
    const float planH = std::clamp(contentH * 0.40f, 190.0f, 330.0f);
    const Rect plan{ rightX, top, rightW, planH };
    const Rect control{ rightX, plan.bottom() + gap, rightW,
                        std::max(180.0f, bottom - plan.bottom() - gap) };

    mixLayout.queueCard = queue;
    mixLayout.deckA = deckA;
    mixLayout.deckB = deckB;
    mixLayout.planCard = plan;
    mixLayout.controlCard = control;

    mixLayout.addButton = { queue.right() - 128.0f, queue.y + 13.0f, 56.0f, 26.0f };
    mixLayout.clearButton = { queue.right() - 66.0f, queue.y + 13.0f, 56.0f, 26.0f };

    const float rowsTop = queue.y + 72.0f;
    const float rowsBottom = queue.bottom() - 12.0f;
    const float rowH = std::clamp((rowsBottom - rowsTop) / 8.0f, 30.0f, 58.0f);
    for (int i = 0; i < 8; ++i) {
        const Rect row{ queue.x + 10.0f, rowsTop + static_cast<float>(i) * rowH,
                        queue.w - 20.0f, rowH - 4.0f };
        mixLayout.queueRow[i] = row;
        mixLayout.queueRemove[i] = { row.right() - 24.0f, row.y + 8.0f, 20.0f, 20.0f };
    }

    const Rect decks[2] = { deckA, deckB };
    for (int i = 0; i < 2; ++i) {
        const Rect& card = decks[i];
        mixLayout.deckWave[i] = { card.x + 16.0f, card.y + 88.0f,
                                  card.w - 32.0f,
                                  std::max(40.0f, card.h - 88.0f - 48.0f) };
        mixLayout.deckGainMinus[i] = { card.x + 70.0f, card.bottom() - 36.0f,
                                       24.0f, 24.0f };
        mixLayout.deckGainPlus[i] = { card.x + 100.0f, card.bottom() - 36.0f,
                                      24.0f, 24.0f };
    }
    mixLayout.openDeckA = { deckA.right() - 98.0f, deckA.bottom() - 36.0f,
                            82.0f, 26.0f };
    mixLayout.openDeckB = { deckB.right() - 98.0f, deckB.bottom() - 36.0f,
                            82.0f, 26.0f };

    const float innerX = control.x + 16.0f;
    const float innerW = control.w - 32.0f;
    const float available = control.h - 56.0f - 46.0f;
    // Eleven rows: transport, seven value rows, two chip rows, preview.
    const float rowGap = std::clamp((available - 298.0f) / 10.0f, 2.0f, 9.0f);
    mixLayout.monitor = { innerX, control.y + 32.0f, innerW, 16.0f };
    float y = control.y + 54.0f;
    const float bw = (innerW - 18.0f) / 4.0f;
    mixLayout.playButton = { innerX, y, bw, 30.0f };
    mixLayout.nextButton = { innerX + bw + 6.0f, y, bw, 30.0f };
    mixLayout.stopButton = { innerX + (bw + 6.0f) * 2.0f, y, bw, 30.0f };
    mixLayout.cancelButton = { innerX + (bw + 6.0f) * 3.0f, y, bw, 30.0f };
    y += 30.0f + rowGap;

    auto valueRow = [&](Rect& prev, Rect& next) {
        prev = { innerX + innerW - 58.0f, y, 26.0f, 26.0f };
        next = { innerX + innerW - 26.0f, y, 26.0f, 26.0f };
        y += 26.0f + rowGap;
    };
    valueRow(mixLayout.typePrev, mixLayout.typeNext);
    valueRow(mixLayout.durationMinus, mixLayout.durationPlus);
    valueRow(mixLayout.modePrev, mixLayout.modeNext);
    valueRow(mixLayout.rampModePrev, mixLayout.rampModeNext);
    valueRow(mixLayout.rampBarsPrev, mixLayout.rampBarsNext);
    valueRow(mixLayout.curvePrev, mixLayout.curveNext);
    valueRow(mixLayout.maxDeltaPrev, mixLayout.maxDeltaNext);

    const float chipW = (innerW - 12.0f) / 3.0f;
    mixLayout.pitchChip = { innerX, y, chipW, 28.0f };
    mixLayout.bassChip = { innerX + chipW + 6.0f, y, chipW, 28.0f };
    mixLayout.adaptiveChip = { innerX + (chipW + 6.0f) * 2.0f, y, chipW, 28.0f };
    y += 28.0f + rowGap;
    mixLayout.tempoRampChip = { innerX, y, chipW, 28.0f };
    mixLayout.autoQueueChip = { innerX + chipW + 6.0f, y, chipW, 28.0f };
    mixLayout.qualityStretchChip = { innerX + (chipW + 6.0f) * 2.0f, y, chipW,
                                     28.0f };
    y += 28.0f + rowGap;

    mixLayout.previewButton = { innerX, y, innerW, 30.0f };

    mixLayout.timeline = { plan.x + 16.0f, plan.y + 90.0f, plan.w - 32.0f,
                           std::clamp(plan.h * 0.16f, 40.0f, 58.0f) };

    const float displayY = control.bottom() - 42.0f;
    const float displayW = (innerW - 12.0f) / 3.0f;
    mixLayout.uiScaleChip = { innerX, displayY, displayW, 28.0f };
    mixLayout.qualityChip = { innerX + displayW + 6.0f, displayY, displayW, 28.0f };
    mixLayout.fullscreenChip = { innerX + (displayW + 6.0f) * 2.0f, displayY,
                                 displayW, 28.0f };
}

void DesktopApp::Impl::drawMixButton(const Rect& rect, std::string_view caption,
                                     bool active, SDL_Color color, int icon) {
    const bool isHover = hover(rect);
    const SDL_Color fill = active
                               ? theme::alpha(color, isHover ? 0.30f : 0.19f)
                               : (isHover ? theme::panelHover : theme::panelRaised);
    ui::fillRoundedRect(renderer, rect, 8.0f, fill);
    ui::strokeRoundedRect(renderer, rect, 8.0f,
                          active ? theme::alpha(color, 0.68f) : theme::border, 1.0f);
    const SDL_Color content = active ? color : theme::text;
    float textX = rect.x;
    float textW = rect.w;
    if (icon >= 0) {
        drawIcon(icon, rect.x + 15.0f, rect.centerY(), 13.0f, content);
        textX += 22.0f;
        textW -= 22.0f;
    }
    if (!caption.empty()) {
        text->drawCentered(caption, textX + textW * 0.5f,
                           rect.y + (rect.h - 15.0f) * 0.5f - 1.0f, 9,
                           FontWeight::SemiBold, content);
    }
}

void DesktopApp::Impl::drawMixQueue(const Rect& card,
                                    const mix::MixSessionSnapshot& snap) {
    ui::fillRoundedRect(renderer, card, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, card, 12.0f, theme::border, 1.0f);
    text->draw("QUEUE", card.x + 16.0f, card.y + 14.0f, 13,
               FontWeight::SemiBold, theme::text);
    label(std::to_string(snap.queue.size()) + " TRACKS", card.x + 16.0f,
          card.y + 38.0f, 8, theme::dim, 1.2f);

    drawMixButton(mixLayout.addButton, "ADD", false, theme::accent);
    drawMixButton(mixLayout.clearButton, "CLEAR", false, theme::red);

    if (snap.preparingNext) {
        const Rect bar{ card.x + 16.0f, card.y + 56.0f, card.w - 32.0f, 3.0f };
        ui::fillRoundedRect(renderer, bar, 1.5f,
                            theme::alpha(theme::white, 0.06f));
        ui::fillRoundedRect(renderer, { bar.x, bar.y,
                                       bar.w * std::clamp(snap.preparingProgress, 0.0f, 1.0f),
                                       bar.h }, 1.5f, theme::amber);
    }

    if (snap.queue.empty()) {
        body("Drop audio files here", card.x + 16.0f, card.y + 92.0f, 12,
             theme::muted);
        label("WAV / MP3 / FLAC", card.x + 16.0f, card.y + 112.0f, 8,
              theme::dim, 1.2f);
        return;
    }

    auto stateColor = [](mix::QueueState state) {
        switch (state) {
        case mix::QueueState::Pending: return theme::dim;
        case mix::QueueState::Preparing: return theme::amber;
        case mix::QueueState::Ready: return theme::accent;
        case mix::QueueState::Failed: return theme::red;
        case mix::QueueState::Playing: return theme::accent;
        case mix::QueueState::Played: return theme::dim;
        }
        return theme::dim;
    };
    auto stateName = [](mix::QueueState state) {
        switch (state) {
        case mix::QueueState::Pending: return "PENDING";
        case mix::QueueState::Preparing: return "PREPARING";
        case mix::QueueState::Ready: return "READY";
        case mix::QueueState::Failed: return "FAILED";
        case mix::QueueState::Playing: return "LIVE";
        case mix::QueueState::Played: return "PLAYED";
        }
        return "";
    };

    const int count = std::min<int>(8, static_cast<int>(snap.queue.size()));
    for (int i = 0; i < count; ++i) {
        const mix::QueueItemStatus& item = snap.queue[static_cast<std::size_t>(i)];
        const Rect row = mixLayout.queueRow[i];
        const bool onDeck = item.id == snap.engine.decks[0].trackId ||
                            item.id == snap.engine.decks[1].trackId;
        if (onDeck) {
            ui::fillRoundedRect(renderer, row, 7.0f,
                                theme::alpha(theme::accent, 0.075f));
            ui::strokeRoundedRect(renderer, row, 7.0f,
                                  theme::alpha(theme::accent, 0.22f), 1.0f);
        } else if (hover(row)) {
            ui::fillRoundedRect(renderer, row, 7.0f, theme::panelRaised);
        }
        const SDL_Color dot = stateColor(item.state);
        ui::fillCircle(renderer, row.x + 10.0f, row.centerY(), 3.0f, dot);
        const std::string name = ellipsize(
            *text, item.displayName, row.w - 84.0f, 10, FontWeight::SemiBold);
        body(name, row.x + 20.0f, row.y + 6.0f, 10, theme::text,
             FontWeight::SemiBold);
        std::string sub = stateName(item.state);
        if (item.bpm > 0.0) {
            sub += "  ·  " + formatNumber(item.bpm, 1) + " BPM";
        }
        if (item.durationSeconds > 0.0) {
            sub += "  ·  " + formatTime(item.durationSeconds);
        }
        label(sub, row.x + 20.0f, row.y + 21.0f, 8, dot, 0.7f);
        if (item.state == mix::QueueState::Preparing) {
            const Rect bar{ row.x + 20.0f, row.bottom() - 5.0f, row.w - 58.0f, 2.0f };
            ui::fillRoundedRect(renderer, bar, 1.0f,
                                theme::alpha(theme::white, 0.06f));
            ui::fillRoundedRect(renderer, { bar.x, bar.y,
                                           bar.w * std::clamp(item.progress, 0.0f, 1.0f),
                                           bar.h }, 1.0f, theme::amber);
        }
        drawMixButton(mixLayout.queueRemove[i], "X", false, theme::red);
    }
    if (static_cast<int>(snap.queue.size()) > count) {
        labelRight("+" + std::to_string(snap.queue.size() - count) + " MORE",
                   card.right() - 16.0f, card.bottom() - 22.0f, 8, theme::dim,
                   0.8f);
    }
}

void DesktopApp::Impl::drawMixDeck(const Rect& card, mix::DeckId deck,
                                   bool incoming) {
    const int idx = mix::deckIndex(deck);
    const mix::MixSessionSnapshot snap = mixSession->snapshot();
    const mix::DeckStatus& status = snap.engine.decks[idx];
    const bool master = snap.engine.master == deck;
    const SDL_Color tint = incoming ? theme::violet
                                    : (master ? theme::accent : theme::muted);

    ui::fillRoundedRect(renderer, card, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, card, 12.0f,
                          master ? theme::alpha(theme::accent, 0.42f)
                                 : theme::border,
                          master ? 1.4f : 1.0f);

    label(std::string("DECK ") + (deck == mix::DeckId::A ? "A" : "B"),
          card.x + 16.0f, card.y + 14.0f, 9, theme::dim, 1.6f);
    const std::string role = master ? "MASTER" : (incoming ? "INCOMING" : "STANDBY");
    const Rect roleRect{ card.x + 88.0f, card.y + 10.0f, 78.0f, 20.0f };
    ui::fillRoundedRect(renderer, roleRect, 10.0f, theme::alpha(tint, 0.12f));
    ui::strokeRoundedRect(renderer, roleRect, 10.0f, theme::alpha(tint, 0.38f),
                          1.0f);
    text->drawCentered(role, roleRect.centerX(), roleRect.y + 5.0f, 8,
                       FontWeight::SemiBold, tint);

    const std::string deckName = mixSession->deckName(deck);
    const std::string name = deckName.empty()
                                 ? "NO TRACK"
                                 : ellipsize(*text, deckName,
                                             card.w - 32.0f, 15,
                                             FontWeight::SemiBold);
    body(name, card.x + 16.0f, card.y + 36.0f, 15,
         deckName.empty() ? theme::muted : theme::text, FontWeight::SemiBold);

    std::string info;
    if (status.loaded) {
        info = formatNumber(status.bpm, 1) + " BPM  ·  " +
               formatTime(status.positionSeconds, true) + " / " +
               formatTime(status.durationSeconds) + "  ·  " +
               formatNumber(status.rate * 100.0, 1) + "%";
        if (status.ended) {
            info += "  ·  END";
        }
    } else {
        info = "deck is empty";
    }
    label(info, card.x + 16.0f, card.y + 62.0f, 8,
          status.loaded ? theme::muted : theme::dim, 0.8f);

    const Rect wave = mixLayout.deckWave[idx];
    ui::fillRoundedRect(renderer, wave, 8.0f, theme::backgroundSoft);
    ui::strokeRoundedRect(renderer, wave, 8.0f, theme::border, 1.0f);

    const std::shared_ptr<const AnalysisResult> analysis =
        mixSession->deckAnalysis(deck);
    const bool hasWave = analysis && !analysis->waveform.empty() &&
                         analysis->waveformPeaksPerSecond > 0.0f;
    if (hasWave) {
        const double duration = std::max(0.01, analysis->info.duration);
        // Highlight the planned transition region first.
        if (snap.preview.valid) {
            double fromTime = -1.0;
            double toTime = -1.0;
            if (snap.preview.from == deck) {
                fromTime = snap.preview.transitionStartA;
                toTime = snap.preview.transitionEndA;
            } else if (snap.preview.to == deck) {
                fromTime = snap.preview.startB;
                toTime = snap.preview.endB;
            }
            if (toTime > fromTime && fromTime >= 0.0) {
                const float x0 = wave.x + static_cast<float>(
                    std::clamp(fromTime / duration, 0.0, 1.0)) * wave.w;
                const float x1 = wave.x + static_cast<float>(
                    std::clamp(toTime / duration, 0.0, 1.0)) * wave.w;
                ui::fillRect(renderer, {x0, wave.y, std::max(2.0f, x1 - x0),
                                        wave.h},
                             theme::alpha(tint, 0.13f));
            }
        }
        const float mid = wave.centerY();
        const float amp = wave.h * 0.45f;
        const float step = 2.0f;
        const float pps = analysis->waveformPeaksPerSecond;
        const int peakCount = static_cast<int>(analysis->waveform.size());
        for (float x = wave.x + 2.0f; x < wave.right() - 2.0f; x += step) {
            const double t = (x - wave.x) / std::max(1.0f, wave.w) * duration;
            const int index = std::clamp(static_cast<int>(t * pps), 0,
                                         std::max(0, peakCount - 1));
            float lo = analysis->waveform[static_cast<std::size_t>(index)].minValue;
            float hi = analysis->waveform[static_cast<std::size_t>(index)].maxValue;
            if (amp * (hi - lo) < 1.0f) {
                lo = -0.02f;
                hi = 0.02f;
            }
            const float y0 = mid - std::clamp(hi, -1.0f, 1.0f) * amp;
            const float y1 = mid - std::clamp(lo, -1.0f, 1.0f) * amp;
            ui::thickLine(renderer, x, y0, x, y1, 1.2f,
                          theme::alpha(tint, 0.78f), false);
        }
        if (status.loaded && status.durationSeconds > 0.0) {
            const float px = wave.x + static_cast<float>(
                std::clamp(status.positionSeconds / status.durationSeconds,
                           0.0, 1.0)) * wave.w;
            ui::thickLine(renderer, px, wave.y + 2.0f, px, wave.bottom() - 2.0f,
                          1.6f, theme::text, false);
        }
    } else {
        text->drawCentered("NO WAVEFORM", wave.centerX(),
                           wave.centerY() - 8.0f, 9, FontWeight::SemiBold,
                           theme::dim);
        labelRight("PREPARED PCM + BG BEAT", wave.right() - 8.0f,
                   wave.bottom() - 16.0f, 7, theme::dim, 0.6f);
    }

    // Gain, tempo and load controls.
    label("GAIN", card.x + 16.0f, card.bottom() - 30.0f, 8, theme::dim, 1.2f);
    drawMixButton(mixLayout.deckGainMinus[idx], "-", false, theme::accent);
    drawMixButton(mixLayout.deckGainPlus[idx], "+", false, theme::accent);
    body(formatNumber(mixDeckGain[idx] * 100.0, 0) + "%",
         card.x + 130.0f, card.bottom() - 30.0f, 9, theme::muted);
    if (status.loaded) {
        label(formatNumber(status.peak, 2) + " PK", card.x + 190.0f,
              card.bottom() - 30.0f, 8, theme::dim, 1.0f);
    }
    drawMixButton(idx == 0 ? mixLayout.openDeckA : mixLayout.openDeckB,
                  "OPEN", false, tint, 0);
}

void DesktopApp::Impl::drawMixPlan(const Rect& card,
                                   const mix::MixSessionSnapshot& snap) {
    ui::fillRoundedRect(renderer, card, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, card, 12.0f, theme::border, 1.0f);
    text->draw("TRANSITION PLAN", card.x + 16.0f, card.y + 13.0f, 12,
               FontWeight::SemiBold, theme::text);

    const mix::MixPlan& plan = snap.preview;
    if (!plan.valid) {
        label("NO PLAN", card.x + 16.0f, card.y + 40.0f, 9, theme::amber, 1.4f);
        body("Load two tracks and press play. The planner needs a prepared",
             card.x + 16.0f, card.y + 60.0f, 10, theme::muted);
        body("incoming deck before it can describe a transition.",
             card.x + 16.0f, card.y + 76.0f, 10, theme::muted);
        label("It never invents a plan: low confidence becomes a fallback.",
              card.x + 16.0f, card.y + 100.0f, 8, theme::dim, 0.7f);
        return;
    }

    const SDL_Color statusColor =
        mix::transitionStatus(plan.type) == mix::ImplementationStatus::Working
            ? theme::accent
            : (mix::transitionStatus(plan.type) ==
                       mix::ImplementationStatus::Experimental
                   ? theme::amber
                   : theme::red);
    const std::string typeName = mix::transitionTypeName(plan.type);
    body(ellipsize(*text, typeName, card.w - 130.0f, 15, FontWeight::SemiBold),
         card.x + 16.0f, card.y + 34.0f, 15, theme::text,
         FontWeight::SemiBold);
    const Rect statusRect{ card.right() - 108.0f, card.y + 32.0f, 92.0f, 20.0f };
    ui::fillRoundedRect(renderer, statusRect, 10.0f,
                        theme::alpha(statusColor, 0.12f));
    ui::strokeRoundedRect(renderer, statusRect, 10.0f,
                          theme::alpha(statusColor, 0.38f), 1.0f);
    text->drawCentered(mix::transitionStatusLabel(plan.type),
                       statusRect.centerX(), statusRect.y + 5.0f, 7,
                       FontWeight::SemiBold, statusColor);

    std::string route = std::string("A ") +
                        formatNumber(plan.from == mix::DeckId::A ? plan.bpmA
                                                                : plan.bpmB, 1) +
                        "  ->  B " +
                        formatNumber(plan.to == mix::DeckId::B ? plan.bpmB
                                                              : plan.bpmA, 1) +
                        " BPM  ·  " + formatNumber(plan.duration, 1) + " s";
    label(route, card.x + 16.0f, card.y + 56.0f, 8, theme::muted, 0.8f);

    std::string alignment;
    if (plan.beatAligned) {
        alignment = "BEAT ALIGNED  ·  grid snap " +
                    formatNumber(plan.alignmentErrorMs, 1) + " ms";
        if (plan.downbeatAligned) {
            alignment += "  ·  downbeat";
        }
    } else {
        alignment = "NOT BEAT ALIGNED  ·  confidence " +
                    formatNumber(plan.confidence * 100.0, 0) + "%";
    }
    label(alignment, card.x + 16.0f, card.y + 72.0f, 8,
          plan.beatAligned ? theme::accent : theme::amber, 0.8f);
    if (plan.experimental) {
        labelRight("EXPERIMENTAL", card.right() - 16.0f, card.y + 72.0f, 8,
                   theme::amber, 0.8f);
    }

    // Transition Timeline: pre-roll, crossfade, handover and live playhead.
    const Rect timeline = mixLayout.timeline;
    ui::fillRoundedRect(renderer, timeline, 7.0f, theme::backgroundSoft);
    ui::strokeRoundedRect(renderer, timeline, 7.0f, theme::border, 1.0f);
    const double total = std::max(0.001, plan.totalDuration());
    const float preW = static_cast<float>(
        std::clamp(plan.preTransitionSeconds / total, 0.0, 1.0)) * timeline.w;
    const float fadeW = std::max(0.0f, timeline.w - preW);
    if (preW > 0.5f) {
        ui::fillRect(renderer, { timeline.x, timeline.y + 3.0f, preW,
                                 timeline.h - 6.0f },
                     theme::alpha(theme::amber, 0.16f));
        label("PRE-ROLL", timeline.x + 5.0f, timeline.y + 4.0f, 7,
              theme::amber, 0.6f);
    }
    ui::fillRect(renderer, { timeline.x + preW, timeline.y + 3.0f, fadeW,
                             timeline.h - 6.0f },
                 theme::alpha(theme::accent, 0.14f));
    if (fadeW > 40.0f) {
        label("CROSSFADE", timeline.x + preW + 5.0f, timeline.y + 4.0f, 7,
              theme::accent, 0.6f);
    }
    const float handoverX = timeline.x + preW +
                            fadeW * static_cast<float>(
                                        std::clamp(plan.handoverProgress, 0.0, 1.0));
    ui::thickLine(renderer, handoverX, timeline.y + 3.0f, handoverX,
                  timeline.bottom() - 3.0f, 1.0f,
                  theme::alpha(theme::white, 0.35f), false);
    label("HANDOVER", std::min(handoverX + 3.0f, timeline.right() - 52.0f),
          timeline.y + 4.0f, 6, theme::dim, 0.4f);
    if (snap.engine.phase == mix::TransitionPhase::Running ||
        snap.engine.phase == mix::TransitionPhase::Cancelling) {
        const double elapsed = snap.engine.transitionElapsed;
        const float playX = timeline.x + static_cast<float>(
            std::clamp(elapsed / total, 0.0, 1.0)) * timeline.w;
        ui::thickLine(renderer, playX, timeline.y + 1.0f, playX,
                      timeline.bottom() - 1.0f, 2.0f, theme::text, false);
    }
    labelRight(formatNumber(plan.preTransitionSeconds, 1) + " + " +
                   formatNumber(plan.duration, 1) + " s",
               timeline.right() - 4.0f, timeline.bottom() - 13.0f, 7,
               theme::muted, 0.5f);

    // Gain and tempo curves from the actual plan.
    const Rect curve{ card.x + 16.0f, timeline.bottom() + 8.0f,
                      card.w - 32.0f,
                      std::clamp(card.h * 0.18f, 38.0f, 58.0f) };
    ui::fillRoundedRect(renderer, curve, 7.0f, theme::backgroundSoft);
    ui::strokeRoundedRect(renderer, curve, 7.0f, theme::border, 1.0f);
    auto plot = [&](const std::vector<mix::GainPoint>& points, SDL_Color color) {
        if (points.size() < 2) {
            return;
        }
        SDL_FPoint previous{};
        bool first = true;
        for (const mix::GainPoint& point : points) {
            const float x = curve.x + static_cast<float>(std::clamp(
                (plan.preTransitionSeconds + point.time) / total, 0.0, 1.0)) *
                                        curve.w;
            const float y = curve.bottom() - 4.0f -
                            std::clamp(point.gain, 0.0f, 1.2f) * (curve.h - 8.0f);
            if (!first) {
                ui::thickLine(renderer, previous.x, previous.y, x, y, 1.4f,
                              color, false);
            }
            previous = { x, y };
            first = false;
        }
    };
    plot(plan.gainCurveOut, theme::accent);
    plot(plan.gainCurveIn, theme::violet);
    if (!plan.tempoCurve.empty()) {
        SDL_FPoint previous{};
        bool first = true;
        for (const mix::TempoPoint& point : plan.tempoCurve) {
            const float x = curve.x + static_cast<float>(std::clamp(
                (plan.preTransitionSeconds + point.time) / total, 0.0, 1.0)) *
                                        curve.w;
            const double deviation = std::clamp((point.rate - 1.0) / 0.15, -1.0, 1.0);
            const float y = curve.centerY() -
                            static_cast<float>(deviation) * (curve.h * 0.5f - 4.0f);
            if (!first) {
                ui::thickLine(renderer, previous.x, previous.y, x, y, 1.2f,
                              theme::amber, false);
            }
            previous = { x, y };
            first = false;
        }
        if (plan.pitchShiftWarning) {
            label("TEMPO RAMP  ·  PITCH SHIFT BY DESIGN", curve.x + 6.0f,
                  curve.y + 4.0f, 7, theme::amber, 0.5f);
        } else if (plan.tempoRampActive) {
            label("TEMPO RAMP  ·  PITCH LOCKED", curve.x + 6.0f,
                  curve.y + 4.0f, 7, theme::accent, 0.5f);
        }
    }
    label("OUT", curve.x + 6.0f, curve.bottom() - 13.0f, 7, theme::accent, 0.6f);
    labelRight("IN", curve.right() - 6.0f, curve.bottom() - 13.0f, 7,
               theme::violet, 0.6f);

    float y = curve.bottom() + 8.0f;
    int shown = 0;
    for (const std::string& reason : plan.reasons) {
        if (shown >= 3 || y > card.bottom() - 46.0f) {
            break;
        }
        body(ellipsize(*text, reason, card.w - 32.0f, 9, FontWeight::Regular),
             card.x + 16.0f, y, 9, theme::muted);
        y += 13.0f;
        ++shown;
    }
    if (!plan.fallbackReason.empty()) {
        label("FALLBACK", card.x + 16.0f, card.bottom() - 36.0f, 8,
              theme::amber, 1.0f);
        body(ellipsize(*text, plan.fallbackReason, card.w - 96.0f, 8,
                       FontWeight::Regular),
             card.x + 82.0f, card.bottom() - 36.0f, 8, theme::amber);
    } else if (plan.pitchShiftWarning) {
        body("Linear time-stretch changes pitch. Choose Quality + Pitch Lock",
             card.x + 16.0f, card.bottom() - 36.0f, 8, theme::amber);
    } else {
        label("TIMELINE + GAIN/TEMPO CURVES FROM BG BEAT", card.x + 16.0f,
              card.bottom() - 36.0f, 7, theme::dim, 0.6f);
    }

    const float peakDb = snap.engine.outputPeak > 0.0001f
                             ? 20.0f * std::log10(snap.engine.outputPeak)
                             : -96.0f;
    const std::string diagnostics =
        "OUT " + formatNumber(peakDb, 1) + " dBFS  ·  LIM " +
        formatNumber(snap.engine.limiterGain, 2) + "  ·  LAT " +
        formatNumber(snap.engine.outputLatencySeconds * 1000.0, 1) + " ms  ·  " +
        std::to_string(snap.engine.completedTransitions) + " MIXES";
    labelRight(diagnostics, card.right() - 16.0f, card.bottom() - 18.0f, 7,
               theme::dim, 0.5f);
}

void DesktopApp::Impl::drawMixControls(const Rect& card,
                                       const mix::MixSessionSnapshot& snap) {
    ui::fillRoundedRect(renderer, card, 12.0f, theme::panel);
    ui::strokeRoundedRect(renderer, card, 12.0f, theme::border, 1.0f);
    text->draw("MIX CONTROLS", card.x + 16.0f, card.y + 13.0f, 12,
               FontWeight::SemiBold, theme::text);
    labelRight("LIVE", card.right() - 16.0f, card.y + 16.0f, 7,
               snap.engine.paused ? theme::amber : theme::accent, 1.0f);

    const bool anyPlaying = snap.engine.decks[0].playing ||
                            snap.engine.decks[1].playing;
    const bool transportRunning = !snap.engine.paused && anyPlaying;
    const bool mixing = snap.engine.phase == mix::TransitionPhase::Running ||
                        snap.engine.phase == mix::TransitionPhase::Cancelling;

    auto stageName = [](mix::TransitionStage stage) -> const char* {
        switch (stage) {
        case mix::TransitionStage::Preparing: return "PREPARING";
        case mix::TransitionStage::TempoRamp: return "TEMPO RAMP";
        case mix::TransitionStage::BeatAlign: return "BEAT ALIGN";
        case mix::TransitionStage::Mixing: return "MIXING";
        case mix::TransitionStage::Handover: return "HANDOVER";
        case mix::TransitionStage::Complete: return "COMPLETE";
        case mix::TransitionStage::Cancelled: return "CANCELLED";
        case mix::TransitionStage::Idle: break;
        }
        return "IDLE";
    };
    // Live Transition Monitor: stage, both live rates, EQ, phase and the two
    // audio-integrity flags. This is the same state BG Sync consumes.
    std::string monitor;
    if (mixing) {
        const mix::MixSnapshot& e = snap.engine;
        monitor = std::string(stageName(e.stage)) + "  ·  A " +
                  formatNumber(e.tempoRatioA, 3) + "x  ·  B " +
                  formatNumber(e.tempoRatioB, 3) + "x  ·  LOW " +
                  formatNumber(e.eqLow[0], 2) + "/" +
                  formatNumber(e.eqLow[1], 2) + "  ·  PHASE " +
                  formatNumber(e.beatPhaseErrorMs, 1) + " ms";
        if (e.pitchLock) {
            monitor += "  ·  LOCK";
        }
        if (e.bassSwap) {
            monitor += "  ·  BASS SWAP";
        }
    } else if (snap.preview.valid) {
        monitor = std::string("NEXT  ·  ") +
                  mix::transitionTypeName(snap.preview.type) + "  ·  " +
                  formatNumber(snap.preview.preTransitionSeconds, 1) + " + " +
                  formatNumber(snap.preview.duration, 1) + " s  ·  " +
                  formatNumber(snap.preview.confidence * 100.0, 0) + "%";
    } else {
        monitor = "IDLE  ·  load two tracks to arm a transition";
    }
    label(ellipsize(*text, monitor, card.w - 32.0f, 8, FontWeight::Regular),
          card.x + 16.0f, mixLayout.monitor.y, 8,
          mixing ? theme::accent : theme::dim, 0.4f);

    drawMixButton(mixLayout.playButton, transportRunning ? "PAUSE" : "PLAY",
                  transportRunning, theme::accent, transportRunning ? 2 : 1);
    drawMixButton(mixLayout.nextButton, "NEXT", mixing, theme::violet);
    drawMixButton(mixLayout.stopButton, "STOP", false, theme::muted, 3);
    drawMixButton(mixLayout.cancelButton, "CANCEL", mixing, theme::red);

    auto valueRow = [&](const Rect& prev, const Rect& next, const char* caption,
                        const std::string& value) {
        label(caption, card.x + 16.0f, prev.y + 8.0f, 8, theme::dim, 1.2f);
        text->drawRight(ellipsize(*text, value, 200.0f, 10, FontWeight::SemiBold),
                        prev.x - 8.0f, prev.y + 4.0f, 10, FontWeight::SemiBold,
                        theme::text);
        drawMixButton(prev, "<", false, theme::muted);
        drawMixButton(next, ">", false, theme::muted);
    };

    const std::string typeName =
        mix::transitionTypeName(mixSession->transitionType());
    valueRow(mixLayout.typePrev, mixLayout.typeNext, "TYPE", typeName);

    const double duration = mixSession->transitionDuration();
    valueRow(mixLayout.durationMinus, mixLayout.durationPlus, "DURATION",
             duration <= 0.0 ? "AUTO" : formatNumber(duration, 1) + " s");

    const mix::MixMode mode = mixSession->mixMode();
    const char* modeName = mode == mix::MixMode::Auto
                               ? "AUTO"
                               : (mode == mix::MixMode::Manual ? "MANUAL"
                                                               : "PREVIEW");
    valueRow(mixLayout.modePrev, mixLayout.modeNext, "MODE", modeName);

    const mix::TempoRampMode rampMode = mixSession->tempoRampMode();
    const char* rampModeName = rampMode == mix::TempoRampMode::Off
                                   ? "OFF"
                                   : (rampMode == mix::TempoRampMode::Auto
                                          ? "AUTO"
                                          : "MANUAL");
    valueRow(mixLayout.rampModePrev, mixLayout.rampModeNext, "RAMP",
             rampModeName);

    const double bars = mixSession->tempoRampBars();
    valueRow(mixLayout.rampBarsPrev, mixLayout.rampBarsNext, "RAMP BARS",
             bars < 1.0 ? "AUTO" : formatNumber(bars, 0) + " bars");

    const mix::TempoCurveKind curve = mixSession->tempoCurve();
    valueRow(mixLayout.curvePrev, mixLayout.curveNext, "CURVE",
             curve == mix::TempoCurveKind::Linear
                 ? "LINEAR"
                 : (curve == mix::TempoCurveKind::Smooth ? "SMOOTH"
                                                        : "S-CURVE"));

    valueRow(mixLayout.maxDeltaPrev, mixLayout.maxDeltaNext, "MAX DELTA",
             formatNumber(mixSession->tempoRampMaxPercent(), 0) + " %");

    drawMixButton(mixLayout.pitchChip, "PITCH LOCK",
                  mixSession->pitchLock(), theme::accent);
    drawMixButton(mixLayout.bassChip, "BASS SWAP", mixSession->bassSwap(),
                  theme::violet);
    drawMixButton(mixLayout.adaptiveChip, "ADAPTIVE",
                  mixSession->adaptive(), theme::accent);

    drawMixButton(mixLayout.tempoRampChip, "RAMP " +
                      std::string(mixSession->tempoRampEnabled() ? "ON" : "OFF"),
                  mixSession->tempoRampEnabled(), theme::amber);
    drawMixButton(mixLayout.autoQueueChip, "AUTO QUEUE",
                  mixSession->autoQueue(), theme::violet);
    drawMixButton(mixLayout.qualityStretchChip,
                  mixSession->quality() == mix::StretchQuality::Quality
                      ? "STRETCH Q"
                      : "STRETCH F",
                  mixSession->quality() == mix::StretchQuality::Quality,
                  theme::pink);

    drawMixButton(mixLayout.previewButton, "PREVIEW PLAN", mixPlanDetail,
                  theme::accent);

    const std::string uiCaption = uiScaleCaption();
    drawMixButton(mixLayout.uiScaleChip, uiCaption,
                  uiScaleSetting > 0.0f, theme::violet);
    drawMixButton(mixLayout.qualityChip,
                  "3D " + std::to_string(vrmQuality) + "%", false, theme::pink);
    drawMixButton(mixLayout.fullscreenChip,
                  fullscreen ? "WINDOWED" : "FULLSCREEN", fullscreen,
                  theme::accent);
}

void DesktopApp::Impl::drawMixScreen() {
    if (!mixSession) {
        return;
    }
    const mix::MixSessionSnapshot snap = mixSession->snapshot();

    drawMixQueue(mixLayout.queueCard, snap);
    const bool incomingA = snap.preview.valid &&
                           snap.preview.to == mix::DeckId::A;
    const bool incomingB = snap.preview.valid &&
                           snap.preview.to == mix::DeckId::B;
    drawMixDeck(mixLayout.deckA, mix::DeckId::A, incomingA);
    drawMixDeck(mixLayout.deckB, mix::DeckId::B, incomingB);
    drawMixPlan(mixLayout.planCard, snap);
    drawMixControls(mixLayout.controlCard, snap);

    if (mixPlanDetail) {
        ui::fillRect(renderer, {0.0f, 64.0f, canvasW,
                                std::max(0.0f, layout.footer.y - 64.0f)},
                     SDL_Color{5, 7, 11, 210});
        const float cardW = std::min(860.0f, canvasW - 80.0f);
        const float cardH = std::min(520.0f, canvasH - 180.0f);
        const Rect card{canvasW * 0.5f - cardW * 0.5f,
                        canvasH * 0.5f - cardH * 0.5f, cardW, cardH};
        ui::fillRoundedRect(renderer, card, 14.0f, theme::panel);
        ui::strokeRoundedRect(renderer, card, 14.0f, theme::borderBright, 1.0f);
        text->draw("PLAN PREVIEW", card.x + 22.0f, card.y + 18.0f, 16,
                   FontWeight::SemiBold, theme::text);
        label("DETERMINISTIC · NO AUDIO IS PLAYED BY THIS PANEL",
              card.x + 22.0f, card.y + 42.0f, 8, theme::dim, 0.8f);
        labelRight("CLICK ANYWHERE TO CLOSE", card.right() - 22.0f,
                   card.y + 20.0f, 8, theme::muted, 0.8f);

        const mix::MixPlan& plan = snap.preview;
        if (!plan.valid) {
            body("No valid plan yet. Add at least two tracks; both decks must",
                 card.x + 22.0f, card.y + 76.0f, 12, theme::muted);
            body("finish preparation before the planner can choose a point.",
                 card.x + 22.0f, card.y + 96.0f, 12, theme::muted);
            if (!snap.lastError.empty()) {
                body(ellipsize(*text, snap.lastError, card.w - 44.0f, 11,
                               FontWeight::Regular),
                     card.x + 22.0f, card.y + 126.0f, 11, theme::amber);
            }
        } else {
            body(std::string(mix::transitionTypeName(plan.type)) + "  ·  " +
                     formatNumber(plan.duration, 2) + " s  ·  " +
                     formatNumber(plan.confidence * 100.0, 0) + "% confidence",
                 card.x + 22.0f, card.y + 68.0f, 14, theme::text,
                 FontWeight::SemiBold);
            const Rect curve{ card.x + 22.0f, card.y + 96.0f, card.w - 44.0f,
                              140.0f };
            ui::fillRoundedRect(renderer, curve, 8.0f, theme::backgroundSoft);
            ui::strokeRoundedRect(renderer, curve, 8.0f, theme::border, 1.0f);
            ui::thickLine(renderer, curve.x + 6.0f, curve.centerY(),
                          curve.right() - 6.0f, curve.centerY(), 1.0f,
                          theme::alpha(theme::white, 0.10f), false);
            const double span = std::max(0.001, plan.duration);
            auto plot = [&](const std::vector<mix::GainPoint>& points,
                            SDL_Color color) {
                if (points.size() < 2) {
                    return;
                }
                SDL_FPoint previous{};
                bool first = true;
                for (const mix::GainPoint& point : points) {
                    const float x = curve.x + 6.0f + static_cast<float>(
                        std::clamp(point.time / span, 0.0, 1.0)) *
                        (curve.w - 12.0f);
                    const float y = curve.bottom() - 8.0f -
                                    std::clamp(point.gain, 0.0f, 1.2f) *
                                    (curve.h - 16.0f);
                    if (!first) {
                        ui::thickLine(renderer, previous.x, previous.y, x, y,
                                      2.0f, color, false);
                    }
                    previous = { x, y };
                    first = false;
                }
            };
            plot(plan.gainCurveOut, theme::accent);
            plot(plan.gainCurveIn, theme::violet);
            if (!plan.tempoCurve.empty()) {
                SDL_FPoint previous{};
                bool first = true;
                for (const mix::TempoPoint& point : plan.tempoCurve) {
                    const float x = curve.x + 6.0f + static_cast<float>(
                        std::clamp(point.time / span, 0.0, 1.0)) *
                        (curve.w - 12.0f);
                    const double deviation = std::clamp((point.rate - 1.0) /
                                                            0.15, -1.0, 1.0);
                    const float y = curve.centerY() - static_cast<float>(deviation) *
                                                        (curve.h * 0.5f - 8.0f);
                    if (!first) {
                        ui::thickLine(renderer, previous.x, previous.y, x, y,
                                      1.8f, theme::amber, false);
                    }
                    previous = { x, y };
                    first = false;
                }
                label("TEMPO (AMBER) · OUT GAIN (GREEN) · IN GAIN (VIOLET)",
                      curve.x + 8.0f, curve.y + 5.0f, 7, theme::dim, 0.5f);
            }
            float y = curve.bottom() + 12.0f;
            int shown = 0;
            for (const std::string& reason : plan.reasons) {
                if (shown >= 7) {
                    break;
                }
                body("- " + ellipsize(*text, reason, card.w - 68.0f, 10,
                                      FontWeight::Regular),
                     card.x + 22.0f, y, 10, theme::muted);
                y += 16.0f;
                ++shown;
            }
            if (!plan.fallbackReason.empty()) {
                body("Fallback: " + plan.fallbackReason, card.x + 22.0f, y, 10,
                     theme::amber);
                y += 16.0f;
            }
            if (plan.pitchShiftWarning) {
                body("Pitch warning: the planner asks the outgoing deck to change "
                     "speed linearly.",
                     card.x + 22.0f, y, 10, theme::amber);
                y += 16.0f;
                body("The 0.1 engine has no time-stretch; pitch follows tempo by "
                     "design.",
                     card.x + 22.0f, y, 10, theme::amber);
            }
            labelRight("FROM " + std::string(plan.from == mix::DeckId::A ? "A" : "B") +
                           "  ->  TO " +
                           std::string(plan.to == mix::DeckId::A ? "A" : "B"),
                       card.right() - 22.0f, card.bottom() - 26.0f, 8,
                       theme::dim, 0.8f);
        }
    }
}

void DesktopApp::Impl::handleMixClick(float x, float y, int clicks) {
    (void)clicks;
    if (mixPlanDetail) {
        mixPlanDetail = false;
        return;
    }
    if (!mixSession) {
        return;
    }
    auto hit = [&](const Rect& rect) { return rect.w > 0.0f && rect.contains(x, y); };

    if (hit(mixLayout.addButton) || hit(mixLayout.openDeckA) ||
        hit(mixLayout.openDeckB)) {
        openMixFileDialog();
        return;
    }
    if (hit(mixLayout.clearButton)) {
        mixSession->clearQueue();
        showToast("Mix queue cleared");
        return;
    }
    for (int i = 0; i < 8; ++i) {
        if (i < static_cast<int>(mixSession->snapshot().queue.size()) &&
            hit(mixLayout.queueRemove[i])) {
            const auto snap = mixSession->snapshot();
            if (mixSession->removeTrack(
                    snap.queue[static_cast<std::size_t>(i)].id)) {
                showToast("Removed from queue");
            }
            return;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (hit(mixLayout.deckGainMinus[i])) {
            mixDeckGain[i] = std::max(0.0f, mixDeckGain[i] - 0.05f);
            mixSession->engine().setDeckGain(i == 0 ? mix::DeckId::A
                                                    : mix::DeckId::B,
                                             mixDeckGain[i]);
            return;
        }
        if (hit(mixLayout.deckGainPlus[i])) {
            mixDeckGain[i] = std::min(1.5f, mixDeckGain[i] + 0.05f);
            mixSession->engine().setDeckGain(i == 0 ? mix::DeckId::A
                                                    : mix::DeckId::B,
                                             mixDeckGain[i]);
            return;
        }
    }
    if (hit(mixLayout.playButton)) {
        toggleMixPlay();
        return;
    }
    if (hit(mixLayout.stopButton)) {
        mixSession->stopAll();
        showToast("Mix stopped");
        return;
    }
    if (hit(mixLayout.nextButton)) {
        mixSession->next();
        return;
    }
    if (hit(mixLayout.cancelButton)) {
        mixSession->cancelTransition();
        return;
    }
    if (hit(mixLayout.previewButton)) {
        mixPlanDetail = true;
        return;
    }
    if (hit(mixLayout.adaptiveChip)) {
        mixSession->setAdaptive(!mixSession->adaptive());
        showToast(mixSession->adaptive() ? "Adaptive planning on"
                                        : "Adaptive planning off");
        return;
    }
    if (hit(mixLayout.tempoRampChip)) {
        mixSession->setTempoRampEnabled(!mixSession->tempoRampEnabled());
        showToast(mixSession->tempoRampEnabled()
                      ? "Tempo ramp armed (Auto)"
                      : "Tempo ramp off");
        return;
    }
    if (hit(mixLayout.autoQueueChip)) {
        mixSession->setAutoQueue(!mixSession->autoQueue());
        showToast(mixSession->autoQueue() ? "Auto queue on" : "Auto queue off");
        return;
    }
    if (hit(mixLayout.uiScaleChip)) {
        cycleUiScale();
        return;
    }
    if (hit(mixLayout.qualityChip)) {
        cycleVrmQuality();
        return;
    }
    if (hit(mixLayout.fullscreenChip)) {
        toggleFullscreen(!fullscreen);
        return;
    }

    const std::vector<mix::TransitionType> types =
        mix::selectableTransitionTypes();
    if (hit(mixLayout.typePrev) || hit(mixLayout.typeNext)) {
        const int direction = hit(mixLayout.typeNext) ? 1 : -1;
        int index = 0;
        for (int i = 0; i < static_cast<int>(types.size()); ++i) {
            if (types[static_cast<std::size_t>(i)] == mixSession->transitionType()) {
                index = i;
                break;
            }
        }
        const int count = static_cast<int>(types.size());
        index = (index + direction + count) % count;
        mixSession->setTransitionType(types[static_cast<std::size_t>(index)]);
        showToast(std::string("Transition: ") +
                  mix::transitionTypeName(types[static_cast<std::size_t>(index)]) +
                  " (" +
                  mix::transitionStatusLabel(
                      types[static_cast<std::size_t>(index)]) +
                  ")");
        return;
    }
    if (hit(mixLayout.durationPlus) || hit(mixLayout.durationMinus)) {
        double value = mixSession->transitionDuration();
        if (hit(mixLayout.durationPlus)) {
            value = value <= 0.0 ? 8.0 : std::min(16.0, value + 1.0);
        } else if (value > 0.0) {
            value = value - 1.0 < 3.0 ? 0.0 : value - 1.0;
        }
        mixSession->setTransitionDuration(value);
        showToast(value <= 0.0
                      ? "Transition duration: automatic"
                      : "Transition duration: " + formatNumber(value, 1) + " s");
        return;
    }
    if (hit(mixLayout.modePrev) || hit(mixLayout.modeNext)) {
        const int direction = hit(mixLayout.modeNext) ? 1 : -1;
        const int current = static_cast<int>(mixSession->mixMode());
        const int next = (current + direction + 3) % 3;
        const mix::MixMode mode = static_cast<mix::MixMode>(next);
        mixSession->setMixMode(mode);
        showToast(mode == mix::MixMode::Auto
                      ? "Mode: Auto (adaptive, automatic scheduling)"
                      : (mode == mix::MixMode::Manual
                             ? "Mode: Manual (explicit type, manual start)"
                             : "Mode: Preview (plans only, no auto-start)"));
        return;
    }
    if (hit(mixLayout.rampModePrev) || hit(mixLayout.rampModeNext)) {
        const int direction = hit(mixLayout.rampModeNext) ? 1 : -1;
        const int current = static_cast<int>(mixSession->tempoRampMode());
        const int next = (current + direction + 3) % 3;
        const mix::TempoRampMode mode = static_cast<mix::TempoRampMode>(next);
        mixSession->setTempoRampMode(mode);
        showToast(mode == mix::TempoRampMode::Off
                      ? "Tempo ramp: Off"
                      : (mode == mix::TempoRampMode::Auto
                             ? "Tempo ramp: Auto"
                             : "Tempo ramp: Manual"));
        return;
    }
    if (hit(mixLayout.rampBarsPrev) || hit(mixLayout.rampBarsNext)) {
        const double values[] = {0.0, 4.0, 8.0, 16.0};
        int index = 0;
        for (int i = 0; i < 4; ++i) {
            if (std::abs(values[i] - mixSession->tempoRampBars()) < 0.5) {
                index = i;
                break;
            }
        }
        const int direction = hit(mixLayout.rampBarsNext) ? 1 : -1;
        index = (index + direction + 4) % 4;
        mixSession->setTempoRampBars(values[index]);
        showToast(values[index] < 1.0 ? "Ramp length: Auto"
                                      : "Ramp length: " +
                                            formatNumber(values[index], 0) +
                                            " bars");
        return;
    }
    if (hit(mixLayout.curvePrev) || hit(mixLayout.curveNext)) {
        const int direction = hit(mixLayout.curveNext) ? 1 : -1;
        const int current = static_cast<int>(mixSession->tempoCurve());
        const int next = (current + direction + 3) % 3;
        mixSession->setTempoCurve(static_cast<mix::TempoCurveKind>(next));
        showToast(std::string("Tempo curve: ") +
                  (next == 0 ? "Linear" : (next == 1 ? "Smooth" : "S-Curve")));
        return;
    }
    if (hit(mixLayout.maxDeltaPrev) || hit(mixLayout.maxDeltaNext)) {
        const double delta = hit(mixLayout.maxDeltaNext) ? 1.0 : -1.0;
        mixSession->setTempoRampMaxPercent(
            std::clamp(mixSession->tempoRampMaxPercent() + delta, 1.0, 16.0));
        showToast("Max tempo delta: " +
                  formatNumber(mixSession->tempoRampMaxPercent(), 0) + "%");
        return;
    }
    if (hit(mixLayout.pitchChip)) {
        mixSession->setPitchLock(!mixSession->pitchLock());
        showToast(mixSession->pitchLock()
                      ? "Pitch lock on (WSOLA)"
                      : "Pitch lock off (resampling changes pitch)");
        return;
    }
    if (hit(mixLayout.bassChip)) {
        mixSession->setBassSwap(!mixSession->bassSwap());
        showToast(mixSession->bassSwap() ? "Bass swap on" : "Bass swap off");
        return;
    }
    if (hit(mixLayout.qualityStretchChip)) {
        const mix::StretchQuality next =
            mixSession->quality() == mix::StretchQuality::Quality
                ? mix::StretchQuality::Performance
                : mix::StretchQuality::Quality;
        mixSession->setQuality(next);
        showToast(next == mix::StretchQuality::Quality
                      ? "Time-stretch: Quality (WSOLA, pitch locked)"
                      : "Time-stretch: Fast (linear, pitch follows tempo)");
        return;
    }
}

void DesktopApp::Impl::drawFooter() {
    const Rect bar = layout.footer;
    ui::fillRect(renderer, bar, SDL_Color{6, 8, 13, 255});
    SDL_SetRenderDrawColor(renderer, 31, 40, 55, 255);
    SDL_RenderLine(renderer, 0, bar.y, canvasW, bar.y);

    SDL_Color dot = theme::dim;
    std::string status;
    if (module == Module::Mix && mixSession) {
        const mix::MixSessionSnapshot snap = mixSession->snapshot();
        const bool paused = snap.engine.paused;
        dot = paused ? theme::amber : theme::accent;
        status = snap.status.empty() ? "BG MIX" : snap.status;
        ui::fillCircle(renderer, 24.0f, bar.centerY(), 3.2f, dot);
        label(status, 34.0f, bar.y + 15.0f, 8, theme::muted, 1.1f);
        const std::string engineLine =
            std::string("2 DECKS  ·  ") +
            (snap.engine.decks[0].loaded ? "A" : "-") +
            (snap.engine.decks[1].loaded ? "B" : "-") + "  ·  " +
            formatNumber(snap.engine.outputLatencySeconds * 1000.0, 1) + " ms";
        body(engineLine, 190.0f, bar.y + 13.0f, 9, theme::dim);
        text->drawCentered(
            "SPACE PLAY/PAUSE   N NEXT   X CANCEL   A ADAPTIVE   T TEMPO RAMP   "
            "U UI SCALE",
            canvasW * 0.5f, bar.y + 13.0f, 8, FontWeight::SemiBold,
            theme::alpha(theme::muted, 0.75f), 1.0f, 0.2f);
        labelRight("BG MIX 0.2   ·   " BGMEDIA_VERSION_STRING, canvasW - 24.0f,
                   bar.y + 15.0f, 8, theme::dim, 0.6f);
        return;
    }

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

    text->drawCentered("SPACE PLAY/PAUSE   LEFT/RIGHT 5s   WHEEL ZOOM   G 2D/3D   C CAMERA   D DEBUG   "
                       "V A/B   B LAB   R RATE",
                       canvasW * 0.5f, bar.y + 13.0f, 8, FontWeight::SemiBold,
                       theme::alpha(theme::muted, 0.75f), 1.0f, 0.2f);
    labelRight("BG MEDIA v" BGMEDIA_VERSION_STRING "   ·   OFFLINE / DETERMINISTIC",
                canvasW - 24.0f, bar.y + 15.0f, 8, theme::dim, 0.6f);
}

void DesktopApp::Impl::drawOverlays() {
    if (state == AppState::Loading) {
        ui::fillRect(renderer, {0, 64, canvasW, std::max(0.0f, layout.footer.y - 64.0f)}, SDL_Color{5, 7, 11, 170});
        const Rect card{canvasW * 0.5f - 280.0f, canvasH * 0.5f - 110.0f, 560.0f, 220.0f};
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
        ui::fillRect(renderer, {0, 64, canvasW, std::max(0.0f, layout.footer.y - 64.0f)}, SDL_Color{5, 7, 11, 180});
        const Rect card{canvasW * 0.5f - 330.0f, canvasH * 0.5f - 120.0f, 660.0f, 240.0f};
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
        const Rect pill{canvasW * 0.5f - width * 0.5f, canvasH - 84.0f, width, 30.0f};
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
    if (module == Module::Mix) {
        drawBackground();
        drawHeader();
        drawMixScreen();
        drawFooter();
        drawOverlays();
        return;
    }
    if (characterView == CharacterView::Vrm3D) {
        updateVrmFrame();
    }
    if (presentation && characterView == CharacterView::Vrm3D && danceUi()) {
        drawPresentation();
        return;
    }
    drawBackground();
    drawHeader();
    drawToolbar();

    if (characterView == CharacterView::Vrm3D) {
        draw3DStage();
        if (danceUi()) {
            drawDancePanels();
        }
        draw3DRightPanels();
    } else if (state == AppState::Empty) {
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
// BG MOTION 3.1 panels, debug views and presentation mode
// -------------------------------------------------------------------------
void DesktopApp::Impl::drawDancePanels() {
    const auto panelCard = [&](const Rect& rect) {
        ui::fillRoundedRect(renderer, rect, 12.0f, theme::panel);
        ui::strokeRoundedRect(renderer, rect, 12.0f, theme::border, 1.0f);
    };
    const auto button = [&](const Rect& rect, const std::string& caption, bool active,
                            SDL_Color color) {
        ui::fillRoundedRect(renderer, rect, 6.0f,
                            theme::alpha(color, hover(rect) ? 0.20f : (active ? 0.16f : 0.045f)));
        ui::strokeRoundedRect(renderer, rect, 6.0f,
                              theme::alpha(color, active ? 0.55f : 0.16f), 1.0f);
        text->drawCentered(caption, rect.centerX(), rect.y + rect.h * 0.5f - 5.0f, 8,
                           FontWeight::SemiBold, active ? color : theme::muted);
    };

    // 1. Dance selector -----------------------------------------------------
    {
        const Rect card = layout.right1;
        panelCard(card);
        text->draw("DANCE", card.x + 18.0f, card.y + 12.0f, 11, FontWeight::SemiBold,
                   theme::text);
        label("BG MOTION 3.2", card.right() - 112.0f, card.y + 15.0f, 8, theme::accent, 1.2f);
        const bool autoMode = dance.mode() == motion::DanceMode::Auto;
        button(layout.danceManual, "MANUAL", !autoMode, theme::accent);
        button(layout.danceAuto, "AUTO", autoMode, theme::violet);
        button(layout.danceLoop, dance.oneShot() ? "ONE SHOT" : "LOOP", dance.oneShot(),
               theme::amber);
        button(layout.danceTest, "TEST DANCE", false, theme::pink);

        static constexpr const char* kStyleLabels[5] = {"GROOVE", "HIP-HOP", "SIDE STEP",
                                                        "ENERGETIC", "IDLE"};
        static constexpr motion::DanceStyle kStyles[5] = {
            motion::DanceStyle::Groove, motion::DanceStyle::HipHop, motion::DanceStyle::SideStep,
            motion::DanceStyle::Energetic, motion::DanceStyle::Idle};
        for (int i = 0; i < 5; ++i) {
            const bool active =
                !autoMode ? (dance.selectedStyle() == kStyles[i])
                          : (danceFrame.style == kStyles[i]);
            const bool missing = dance.library().forStyle(kStyles[i]) == nullptr;
            button(layout.danceStyle[i], std::string(kStyleLabels[i]), active && !missing,
                   missing ? theme::dim : theme::accent);
        }
        label(track ? "TEMPO  BG BEAT" : "MOTION LAB", card.x + 18.0f, card.y + 120.0f, 8,
              theme::dim, 1.1f);
        button(layout.danceBpmMinus, "-", false, theme::muted);
        button(layout.danceBpmPlus, "+", false, theme::muted);
        const int shownBpm =
            track ? static_cast<int>(std::lround(dance.director().beatClock().bpmAt(danceTime())))
                  : danceLabBpm;
        body(std::to_string(shownBpm) + " BPM", card.x + 176.0f, card.y + 116.0f, 12,
             theme::text, FontWeight::SemiBold);
        button(layout.danceExpr, expressionLabel(dance.expression()),
               dance.expression() != motion::MotionExpression::Off,
               dance.expression() == motion::MotionExpression::Expressive ? theme::pink
                                                                          : theme::violet);
        button(layout.danceLayer,
               std::string("LAYER ") + motion::overlayModeName(dance.overlayMode()),
               dance.overlayMode() != motion::OverlayMode::Off, theme::violet);
        std::string genLabel = "GEN";
        if (generatedDanceIndex >= 0 &&
            static_cast<std::size_t>(generatedDanceIndex) < dance.generatedDances().size()) {
            genLabel = dance.generatedDances()[static_cast<std::size_t>(generatedDanceIndex)]
                           .clip.name;
        }
        button(layout.danceGen, genLabel, generatedDanceIndex >= 0, theme::pink);
        button(layout.danceSpring,
               vrm.secondaryAvailable()
                   ? (vrm.secondarySettings().enabled ? "SPRING ON" : "SPRING OFF")
                   : "NO SPRING",
               vrm.secondaryAvailable() && vrm.secondarySettings().enabled,
               theme::accent);
        label("1-4 STYLE · A AUTO · Y LAYER · J GEN · K SPRING · T TEST",
              card.x + 18.0f, card.bottom() - 12.0f, 8, theme::dim, 0.6f);
    }

    // 2. Clip state ---------------------------------------------------------
    {
        const Rect card = layout.right2;
        panelCard(card);
        text->draw("CLIP STATE", card.x + 18.0f, card.y + 12.0f, 11, FontWeight::SemiBold,
                   theme::text);
        labelRight(modeLabel(dance.mode()), card.right() - 18.0f, card.y + 15.0f, 8,
                   dance.mode() == motion::DanceMode::Auto ? theme::violet : theme::accent, 1.0f);
        float y = card.y + 36.0f;
        label("CURRENT", card.x + 18.0f, y + 4.0f, 8, theme::dim, 1.1f);
        text->draw(ellipsize(*text, clipTitle(danceFrame), card.w - 120.0f, 20,
                             FontWeight::SemiBold),
                   card.x + 92.0f, y - 2.0f, 20, FontWeight::SemiBold,
                   danceFrame.pose.accent > 0.5f ? theme::amber : theme::text);
        y += 30.0f;
        label("NEXT", card.x + 18.0f, y + 1.0f, 8, theme::dim, 1.1f);
        std::string nextName = "none";
        if (!danceFrame.nextClipId.empty()) {
            if (const motion::HumanoidClip* next = dance.library().find(danceFrame.nextClipId)) {
                nextName = next->name;
            }
        }
        body(nextName, card.x + 92.0f, y - 2.0f, 11, theme::muted);
        y += 22.0f;
        label("PHASE", card.x + 18.0f, y + 1.0f, 8, theme::dim, 1.1f);
        const Rect phaseBar{card.x + 92.0f, y + 2.0f, card.w - 112.0f, 8.0f};
        ui::fillRoundedRect(renderer, phaseBar, 4.0f, theme::alpha(theme::white, 0.07f));
        ui::fillRoundedRect(renderer,
                            {phaseBar.x, phaseBar.y,
                             std::max(2.0f, phaseBar.w * std::clamp(danceFrame.phase, 0.0f, 1.0f)),
                             phaseBar.h},
                            4.0f, theme::accent);
        if (const motion::HumanoidClip* clip = dance.library().find(danceFrame.clipId)) {
            const int beats = std::max(1, static_cast<int>(std::lround(clip->beats)));
            for (int b = 0; b <= beats; ++b) {
                const float tx = phaseBar.x + phaseBar.w * static_cast<float>(b) / beats;
                ui::thickLine(renderer, tx, phaseBar.y - 2.0f, tx,
                              phaseBar.bottom() + (b % clip->beatsPerBar == 0 ? 4.0f : 1.0f),
                              1.0f, theme::alpha(theme::white, b % clip->beatsPerBar == 0 ? 0.35f : 0.15f),
                              false);
            }
            for (const motion::AccentMark& mark : clip->accents) {
                ui::diamond(renderer, phaseBar.x + phaseBar.w * mark.beat / clip->beats,
                            phaseBar.bottom() + 9.0f, 2.6f, theme::alpha(theme::amber, 0.9f));
            }
            body("beat " + formatNumber(std::fmod(std::max(0.0, danceFrame.clipBeat),
                                                  static_cast<double>(clip->beats)), 1) +
                     " / " + std::to_string(beats),
                 phaseBar.x, phaseBar.bottom() + 16.0f, 8, theme::dim);
        }
        y += 40.0f;
        label("SPEED", card.x + 18.0f, y + 1.0f, 8, theme::dim, 1.1f);
        body(formatNumber(danceFrame.playbackSpeed, 2) + "x  ·  " +
                 formatNumber(danceFrame.bpm, 0) + " BPM",
             card.x + 92.0f, y - 2.0f, 11, danceFrame.speedLimited ? theme::amber : theme::muted);
        if (danceFrame.speedLimited) {
            labelRight("RANGE LIMIT", card.right() - 18.0f, y, 8, theme::amber, 0.8f);
        }
        y += 22.0f;
        label("BLEND", card.x + 18.0f, y + 1.0f, 8, theme::dim, 1.1f);
        const Rect blendBar{card.x + 92.0f, y + 2.0f, card.w - 112.0f, 6.0f};
        ui::fillRoundedRect(renderer, blendBar, 3.0f, theme::alpha(theme::white, 0.07f));
        if (danceFrame.transitionProgress >= 0.0f) {
            ui::fillRoundedRect(renderer,
                                {blendBar.x, blendBar.y,
                                 std::max(2.0f, blendBar.w * danceFrame.transitionProgress),
                                 blendBar.h},
                                3.0f, theme::pink);
            labelRight(formatNumber(danceFrame.transitionProgress * 100.0f, 0) + "%",
                       card.right() - 18.0f, y - 2.0f, 8, theme::pink, 0.5f);
        } else {
            body("steady", blendBar.x, y + 10.0f, 8, theme::dim);
        }
        const std::string reason = !dance.autoNote().empty() ? dance.autoNote()
                                                             : danceFrame.reason;
        body(ellipsize(*text, reason.empty() ? std::string("вЂ”") : reason, card.w - 36.0f, 8,
                       FontWeight::Regular),
             card.x + 18.0f, card.bottom() - 18.0f, 8, theme::dim);
    }

    // 3. Debug --------------------------------------------------------------
    {
        const Rect card = layout.right3;
        panelCard(card);
        text->draw("DEBUG", card.x + 18.0f, card.y + 12.0f, 11, FontWeight::SemiBold,
                   theme::text);
        button(layout.dbgFeetBtn, "FOOT CONTACT", dbgFeet, theme::accent);
        button(layout.dbgSkelBtn, "SKELETON", dbgSkeleton, theme::violet);
        button(layout.dbgRootBtn, "ROOT MOTION", dbgRoot, theme::pink);
        button(layout.springMinusBtn, "S-", false, theme::muted);
        button(layout.springPlusBtn, "S+", false, theme::muted);
        button(layout.springDampBtn,
               "DAMP " + formatNumber(vrm.secondarySettings().damping, 2), false,
               theme::muted);
        button(layout.springResetBtn, "RESET SPRING", false, theme::accent);
        const auto& stats = vrm.lastPose().stats;
        float y = card.y + 84.0f;
        const char* names[2] = {"L", "R"};
        for (int f = 0; f < 2; ++f) {
            const auto& foot = danceFrame.pose.foot[static_cast<std::size_t>(f)];
            ui::fillCircle(renderer, card.x + 24.0f + f * 118.0f, y + 6.0f, 4.0f,
                           phaseColor(foot.phase));
            body(std::string(names[f]) + "  " + motion::footPhaseName(foot.phase),
                 card.x + 34.0f + f * 118.0f, y, 10, theme::muted);
        }
        body("ROOT  yaw " + formatNumber(danceFrame.pose.rootYaw * 360.0f, 0) + "В°  ·  x " +
                 formatNumber(danceFrame.pose.rootX * 160.0f, 0) + " z " +
                 formatNumber(danceFrame.pose.rootZ * 160.0f, 0) + " cm  ·  IK " +
                 formatNumber(stats.maxFootError * 1000.0f, 1) + " mm  ·  LYR " +
                 std::to_string(danceFrame.overlayActive) + "/" +
                 std::to_string(danceFrame.overlayActive + danceFrame.overlayRejected),
             card.x + 18.0f, y + 18.0f, 9, theme::muted);
        if (musicDebug) {
            body("MUSIC  bar " + std::to_string(danceFrame.barIndex + 1) + "  beat " +
                     std::to_string(danceFrame.beatInBar + 1) + "/" +
                     std::to_string(std::max(1, danceFrame.beatsPerBar)) + "  ·  down " +
                     formatNumber(danceFrame.downbeatConfidence * 100.0f, 0) + "%",
                 card.x + 18.0f, y + 34.0f, 9, theme::muted);
            const double deviationMs =
                danceFrame.warpDeviationBeats * 60000.0 /
                std::max(1.0, danceFrame.bpm);
            body("WARP  " + std::string(danceFrame.warpActive ? "on " : "off ") +
                     formatNumber(deviationMs, 1) + " ms  ·  " +
                     formatNumber(danceFrame.warpRate, 2) + "x  ·  acc " +
                     formatNumber(danceFrame.musicAccent, 2) + "  ·  bass " +
                     formatNumber(danceFrame.bassStrength, 2) + "  ·  align " +
                     formatNumber(danceFrame.alignmentErrorMs, 1) + " ms",
                 card.x + 18.0f, y + 50.0f, 9, theme::muted);
        } else {
            const auto& spring = vrm.secondaryStats();
            body("HIPS  +" + formatNumber(stats.reachHipDrop * 100.0f, 1) +
                     " cm reach  ·  IK err " +
                     formatNumber(stats.maxFootError * 1000.0f, 1) + " mm  ·  " +
                     formatNumber(vrm.lastRetargetMs(), 2) + " ms",
                 card.x + 18.0f, y + 34.0f, 9, theme::muted);
            body(std::string("SPRING ") +
                     (vrm.secondaryAvailable()
                          ? (vrm.secondarySettings().enabled ? "on " : "off ")
                          : "none ") +
                     std::to_string(spring.chains) + " chains  ·  " +
                     formatNumber(spring.maxDisplacement * 100.0f, 1) + " cm  ·  " +
                     formatNumber(vrm.secondarySettings().intensity, 1) + "x d" +
                     formatNumber(vrm.secondarySettings().damping, 2) + "  ·  " +
                     formatNumber(spring.simulateMilliseconds, 2) + " ms  ·  fingers " +
                     std::to_string(stats.fingerBones),
                 card.x + 18.0f, y + 50.0f, 9, theme::muted);
        }
    }
}

void DesktopApp::Impl::drawDanceDebug(const Rect& stage) {
    const vrm::VrmModel* loaded = vrm.model();
    const animation::RetargetedPose& pose = vrm.lastPose();
    if (!loaded || pose.nodeGlobals.empty()) {
        return;
    }
    using vrm::HumanoidBone;
    const float boxW = 164.0f;
    const float boxH = 172.0f;
    const float x0 = stage.right() - boxW - 14.0f;
    const float y0 = stage.y + 84.0f;
    static constexpr const char* kTitles[3] = {"FRONT", "SIDE", "TOP"};

    const auto position = [&](HumanoidBone bone, bool* ok) {
        const int node = loaded->nodeFor(bone);
        if (ok) {
            *ok = node >= 0;
        }
        return animation::HumanoidRetargeter::stagePosition(pose, node);
    };
    static const std::vector<std::vector<HumanoidBone>> kChains = {
        {HumanoidBone::Hips, HumanoidBone::Spine, HumanoidBone::Chest, HumanoidBone::UpperChest,
         HumanoidBone::Neck, HumanoidBone::Head},
        {HumanoidBone::UpperChest, HumanoidBone::LeftShoulder, HumanoidBone::LeftUpperArm,
         HumanoidBone::LeftLowerArm, HumanoidBone::LeftHand},
        {HumanoidBone::UpperChest, HumanoidBone::RightShoulder, HumanoidBone::RightUpperArm,
         HumanoidBone::RightLowerArm, HumanoidBone::RightHand},
        {HumanoidBone::Hips, HumanoidBone::LeftUpperLeg, HumanoidBone::LeftLowerLeg,
         HumanoidBone::LeftFoot, HumanoidBone::LeftToes},
        {HumanoidBone::Hips, HumanoidBone::RightUpperLeg, HumanoidBone::RightLowerLeg,
         HumanoidBone::RightFoot, HumanoidBone::RightToes},
    };

    for (int v = 0; v < 3; ++v) {
        const Rect box{x0, y0 + static_cast<float>(v) * (boxH + 6.0f), boxW, boxH};
        ui::fillRoundedRect(renderer, box, 8.0f, theme::alpha(theme::panel, 0.82f));
        ui::strokeRoundedRect(renderer, box, 8.0f, theme::alpha(theme::borderBright, 0.7f), 1.0f);
        label(kTitles[v], box.x + 8.0f, box.y + 6.0f, 8, theme::dim, 1.2f);
        const float scale = v == 2 ? 100.0f : (boxH - 40.0f) / std::max(0.5f, loaded->height);
        const float floorPx = box.bottom() - 12.0f;
        const float cx = box.centerX();
        const float cy = box.centerY() + 6.0f;
        const auto map = [&](math::Vec3 p) {
            if (v == 0) return SDL_FPoint{cx - p.x * scale, floorPx - p.y * scale};
            if (v == 1) return SDL_FPoint{cx - p.z * scale, floorPx - p.y * scale};
            return SDL_FPoint{cx - p.x * scale, cy + p.z * scale};
        };
        if (v < 2) {
            ui::thickLine(renderer, box.x + 8.0f, floorPx, box.right() - 8.0f, floorPx, 1.0f,
                          theme::alpha(theme::white, 0.18f), false);
        } else {
            ui::thickLine(renderer, box.x + 8.0f, cy, box.right() - 8.0f, cy, 1.0f,
                          theme::alpha(theme::white, 0.08f), false);
            ui::thickLine(renderer, cx, box.y + 22.0f, cx, box.bottom() - 6.0f, 1.0f,
                          theme::alpha(theme::white, 0.08f), false);
        }
        if (dbgSkeleton && v < 2) {
            for (const auto& chain : kChains) {
                bool havePrev = false;
                SDL_FPoint prev{};
                for (HumanoidBone bone : chain) {
                    bool ok = false;
                    const math::Vec3 p = position(bone, &ok);
                    if (!ok) {
                        continue;
                    }
                    const SDL_FPoint q = map(p);
                    if (havePrev) {
                        ui::thickLine(renderer, prev.x, prev.y, q.x, q.y, 2.0f,
                                      theme::alpha(theme::white, 0.85f), true);
                    }
                    ui::fillCircle(renderer, q.x, q.y, 2.0f, theme::alpha(theme::accent, 0.95f));
                    prev = q;
                    havePrev = true;
                }
            }
        }
        if (dbgSkeleton && v < 2 && loaded->hasSpringBones() &&
            vrm.secondarySettings().enabled) {
            for (const auto& chain : loaded->springChains) {
                bool havePrev = false;
                SDL_FPoint prev{};
                for (int node : chain.nodes) {
                    const math::Vec3 p =
                        animation::HumanoidRetargeter::stagePosition(pose, node);
                    const SDL_FPoint q = map(p);
                    if (havePrev) {
                        ui::thickLine(renderer, prev.x, prev.y, q.x, q.y, 1.2f,
                                      theme::alpha(theme::amber, 0.65f), false);
                    }
                    prev = q;
                    havePrev = true;
                }
            }
        }
        if (dbgFeet) {
            for (int f = 0; f < 2; ++f) {
                const auto& trail = footTrail[f];
                if (v == 2) {
                    std::size_t index = 0;
                    for (const TrailPoint& point : trail) {
                        const float age = static_cast<float>(index++) / std::max<std::size_t>(1, trail.size());
                        const SDL_FPoint q = map({point.x, 0.0f, point.z});
                        ui::fillCircle(renderer, q.x, q.y, 1.6f,
                                       theme::alpha(phaseColor(point.phase), 0.15f + 0.6f * age));
                    }
                }
                bool ok = false;
                const math::Vec3 ankle =
                    position(f == 0 ? HumanoidBone::LeftFoot : HumanoidBone::RightFoot, &ok);
                if (ok) {
                    const auto phase = danceFrame.pose.foot[static_cast<std::size_t>(f)].phase;
                    const SDL_FPoint q = map(ankle);
                    ui::fillCircle(renderer, q.x, q.y, 5.0f, theme::alpha(phaseColor(phase), 0.9f));
                    text->drawCentered(f == 0 ? "L" : "R", q.x, q.y - 4.0f, 7,
                                       FontWeight::SemiBold, SDL_Color{6, 10, 14, 255});
                }
            }
        }
        if (dbgRoot) {
            if (v == 2) {
                bool first = true;
                SDL_FPoint prev{};
                for (const auto& point : rootTrail) {
                    const SDL_FPoint q = map({point.first, 0.0f, point.second});
                    if (!first) {
                        ui::thickLine(renderer, prev.x, prev.y, q.x, q.y, 1.5f,
                                      theme::alpha(theme::pink, 0.7f), false);
                    }
                    prev = q;
                    first = false;
                }
                const math::Vec3 centre = pose.rootOffset + pose.rootPivot;
                const SDL_FPoint c = map(centre);
                const math::Vec3 forward = pose.modelTransform.transformVector(loaded->forward);
                const SDL_FPoint tip = map({centre.x + forward.x * 0.25f, 0.0f,
                                            centre.z + forward.z * 0.25f});
                ui::thickLine(renderer, c.x, c.y, tip.x, tip.y, 2.0f, theme::pink, true);
                ui::fillCircle(renderer, c.x, c.y, 3.0f, theme::pink);
            } else {
                bool ok = false;
                const math::Vec3 hips = position(HumanoidBone::Hips, &ok);
                if (ok) {
                    const SDL_FPoint q = map(hips);
                    ui::ring(renderer, q.x, q.y, 5.0f, 1.5f, theme::pink);
                }
            }
        }
    }
}

void DesktopApp::Impl::drawPresentation() {
    ui::fillRect(renderer, {0.0f, 0.0f, canvasW, canvasH}, SDL_Color{5, 7, 12, 255});
    const Rect full{0.0f, 0.0f, canvasW, canvasH};
    std::string error;
    const bool ok = vrm.ready() && vrm.renderable() && vrm.draw(renderer, ui::fr(full), &error, true);
    if (!ok) {
        ui::verticalGradient(renderer, full, SDL_Color{16, 24, 38, 255}, SDL_Color{4, 6, 10, 255});
        text->drawCentered("Load a VRM model to present", canvasW * 0.5f, canvasH * 0.5f - 20.0f, 16, FontWeight::SemiBold,
                           theme::muted);
    }
    // soft vignette so the picture sits on the screen without UI chrome
    ui::verticalGradient(renderer, {0.0f, 0.0f, canvasW, 120.0f}, SDL_Color{5, 7, 12, 150},
                         SDL_Color{5, 7, 12, 0});
    ui::verticalGradient(renderer, {0.0f, canvasH - 160.0f, canvasW, 160.0f}, SDL_Color{5, 7, 12, 0},
                         SDL_Color{5, 7, 12, 190});
    const std::string caption = clipTitle(danceFrame) + (track ? "" : "  ·  " + std::to_string(danceLabBpm) + " BPM");
    text->drawCentered(caption, canvasW * 0.5f, canvasH - 58.0f, 18, FontWeight::SemiBold,
                       theme::alpha(theme::text, 0.88f));
    const double since = animationTime - presentationSince;
    if (since < 4.0 && !captureMode) {
        const float a = static_cast<float>(std::clamp(1.0 - (since - 2.5) / 1.5, 0.0, 1.0));
        text->drawCentered("P / ESC to leave presentation  ·  1-4 change dance", canvasW * 0.5f, canvasH - 28.0f, 10,
                           FontWeight::Regular, theme::alpha(theme::muted, a));
    }
    if (!toast.empty() && animationTime < toastUntil) {
        const float alpha = static_cast<float>(std::clamp((toastUntil - animationTime) / 0.4, 0.0, 1.0));
        text->drawCentered(toast, canvasW * 0.5f, 40.0f, 11, FontWeight::SemiBold,
                           theme::alpha(theme::text, alpha * 0.8f));
    }
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
    loadStartupMixTracks();
    if (options.startInMix && mixSession) {
        // Wait for the queue worker to finish the first two decks so the
        // captured BG Mix screen shows real names, BPM, waveforms and plan.
        const Uint64 deadline = SDL_GetTicks() + 15000;
        while (SDL_GetTicks() < deadline) {
            mixSession->update();
            if (!mixDevice) {
                pumpMixSilent(0.02);
            }
            const mix::MixSnapshot snap = mixSession->snapshot().engine;
            if (snap.decks[0].loaded && snap.decks[1].loaded) {
                break;
            }
            SDL_Delay(30);
        }
        mixSession->update();
        if (!mixDevice) {
            pumpMixSilent(0.02);
        }
    }
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

    if (options.captureTime > 0.0) {
        if (!track && syntheticDemo) {
            syntheticPosition =
                std::clamp(options.captureTime, 0.0, duration());
            syntheticPlaying = false;
        } else if (track) {
            capturePosition = std::clamp(options.captureTime, 0.0, duration());
        }
        animationTime = options.captureTime;
        if (!track) {
            danceLabTime = options.captureTime;
        }
    }
    if (characterView == CharacterView::Vrm3D && vrm.ready()) {
        // Warm up the GPU upload/pipeline state so the captured frame is the
        // second, representative one, not the first-use frame.
        renderFrame();
        SDL_RenderPresent(renderer);
    }

    SDL_PumpEvents();
    SDL_Delay(40);
    if (characterView == CharacterView::Vrm3D) {
        // Warm up the GPU, measure a representative UI+3D frame, then render
        // the final frame so the measured FPS is already visible in the
        // captured still. Read the backbuffer before Present.
        renderFrame();
        SDL_RenderPresent(renderer);
        SDL_Delay(20);
        const auto captureStart = Clock::now();
        renderFrame();
        const auto captureEnd = Clock::now();
        const double captureMs = std::chrono::duration<double, std::milli>(
                                     captureEnd - captureStart).count();
        if (captureMs > 0.0001) {
            fpsSmoothed = 1000.0 / captureMs;
            frameMsSmoothed = captureMs;
        }
        renderFrame();
        saveCapture(options.captureFile);
        SDL_RenderPresent(renderer);
    } else {
        renderFrame();
        SDL_RenderPresent(renderer);
        SDL_Delay(20);
        renderFrame();
        saveCapture(options.captureFile);
    }
    return 0;
}

int DesktopApp::Impl::run() {
    if (!options.captureFile.empty()) {
        // Captures are deterministic: no device, the mix engine is pumped from
        // the capture loop instead of an audio callback.
        options.noAudio = true;
    }
    if (!initialize()) {
        return 1;
    }
    computeLayout();
    if (options.captureFile.empty()) {
        if (!options.initialFile.empty()) {
            startLoad(options.initialFile);
        }
        loadStartupMixTracks();
        Uint64 previousFrame = SDL_GetTicksNS();
        while (running) {
            const Uint64 frameStart = SDL_GetTicksNS();
            const double deltaSeconds =
                static_cast<double>(frameStart - previousFrame) / 1e9;
            previousFrame = frameStart;
            if (deltaSeconds > 0.0001 && deltaSeconds < 0.5) {
                const double frameMs = deltaSeconds * 1000.0;
                frameMsSmoothed = frameMsSmoothed <= 0.0
                                      ? frameMs
                                      : frameMsSmoothed * 0.90 + frameMs * 0.10;
                fpsSmoothed = 1000.0 / frameMsSmoothed;
            }
            pollWorker();
            pollDialog();
            if (mixSession) {
                mixSession->update();
            }
            if (mixSession && !mixDevice) {
                pumpMixSilent(deltaSeconds);
            }
            updateDemo(deltaSeconds);
            updatePlayback();
            animationTime = static_cast<double>(frameStart) / 1e9;
            danceIdle.update(danceTransportRunning(), animationTime);
            if (settingsDirty && animationTime - settingsSavedAt > 1.5) {
                saveSettings();
            }

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handleEvent(event, false);
            }
            renderFrame();
            SDL_RenderPresent(renderer);

            // SDL's vsync normally paces us; this protects against a renderer
            // that reports success but has vsync disabled.
            const Uint64 now = SDL_GetTicksNS();
            const double frameMs = static_cast<double>(now - frameStart) / 1e6;
            if (frameMs < 8.0) {
                SDL_Delay(static_cast<Uint32>(8.0 - frameMs));
            }
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



