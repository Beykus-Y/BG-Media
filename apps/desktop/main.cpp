#include "DesktopApp.hpp"
#include "bgmedia/version.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

#ifdef _WIN32
std::string wideToUtf8(const wchar_t* text) {
    if (!text) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}
#endif

std::filesystem::path pathFromUtf8(const char* value) {
    if (!value) {
        return {};
    }
    const std::u8string text(reinterpret_cast<const char8_t*>(value));
    return std::filesystem::path(text);
}

void printUsage() {
    std::cout
        << "BG Media desktop - musical analysis and choreography synchronisation\n\n"
        << "Usage: bgmedia_desktop [options]\n\n"
        << "Options:\n"
        << "  --file <path>          open an audio file on startup\n"
        << "  --mix                  start on the BG Mix screen\n"
        << "  --mix-track <path>     add a track to the BG Mix queue (repeatable)\n"
        << "  --cache-dir <dir>      analysis cache directory\n"
        << "  --no-cache             disable reading/writing analysis cache\n"
        << "  --no-audio             do not initialise an audio output device\n"
        << "  --autoplay             start audio playback when the initial track is ready\n"
        << "  --audio-smoke          with --capture: play, pause and seek for 2s\n"
        << "  --capture <file.bmp>   render one frame and exit (UI screenshot)\n"
        << "  --capture-time <sec>   media time for the captured frame (default 2)\n"
        << "  --motion-lab <bpm>     synthetic beat grid for animation demos\n"
        << "  --vrm <path>           load a VRM 0.x model and switch to 3D\n"
        << "  --3d                   start in the VRM 3D character view\n"
        << "  --vrm-demo             synthetic 123 BPM test dance (no music needed)\n"
        << "  --vrm-scale <value>    initial model scale (default 1.0)\n"
        << "  --vrm-offset-y <m>     initial model vertical offset\n"
        << "  --window-size <WxH>    initial window size in screen points (e.g. 1920x1080)\n"
        << "  --ui-scale <auto|100-250>  logical UI scale percentage\n"
        << "  --fullscreen           start fullscreen (SDL borderless fullscreen)\n"
        << "  --vrm-quality <50|75|100>  3D offscreen target as a share of the stage\n"

        << "  --motion-debug         show joints, trails and keyframe diagnostics\n"
        << "  --motion-stepped       use the legacy stepped sampler (A/B compare)\n"
        << "  --dance <name>         BG Motion 3.2 dance: groove|hiphop|sidestep|\n"
        << "                         energetic|idle|neonPulse|streetCurrent|fluidSignal|auto|test\n"
        << "  --dance-plan <a,b,..>  play styles in sequence (--dance-span beats each, default 16)\n"
        << "  --dance-bpm <bpm>      Motion Lab tempo when no music is loaded (default 100)\n"
        << "  --dance-expression <mode>  musical warp: off|soft|expressive (default soft)\n"
        << "  --dances <dir>         dance clip directory (default <assets>/dances)\n"
        << "  --presentation         clean presentation mode\n"
        << "  --debug-feet           foot contact debug views\n"
        << "  --debug-skeleton       skeleton debug views\n"
        << "  --debug-root           root motion debug view\n"
        << "  --legacy-motion        BG Motion 2.0 CharacterPose in 3D (A/B compare)\n"
        << "  --camera-yaw <deg>     3D camera yaw (180 = front, 90 / 270 = side)\n"
        << "  --camera-zoom <x>      3D camera distance scale\n"
        << "  --help                 show this help\n\n"
        << "Controls:\n"
        << "  Space play/pause   L load   Left/Right seek 5s   Shift+Left/Right 30s\n"
        << "  Home/End start/end   R playback rate   [ ] output-latency trim\n"
        << "  Mouse wheel zoom timeline   drag to scrub   middle-drag to pan\n"
        << "  D motion debug   V smooth/stepped compare   B motion lab BPM\n"
        << "  Double-click timeline to reset zoom   F fullscreen   Esc exit fullscreen\n";
}

bool parseArgs(int argc, char** argv, bgmedia::desktop::DesktopOptions& options) {
    using namespace bgmedia::desktop;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return false;
        }
        if (arg == "--file" && i + 1 < argc) {
            options.initialFile = pathFromUtf8(argv[++i]);
        } else if (arg == "--mix") {
            options.startInMix = true;
        } else if (arg == "--mix-track" && i + 1 < argc) {
            options.startInMix = true;
            options.mixTracks.push_back(pathFromUtf8(argv[++i]));
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            options.cacheDirectory = pathFromUtf8(argv[++i]);
        } else if (arg == "--no-cache") {
            options.noCache = true;
        } else if (arg == "--no-audio") {
            options.noAudio = true;
        } else if (arg == "--audio-smoke") {
            options.audioSmoke = true;
        } else if (arg == "--autoplay") {
            options.autoplay = true;
        } else if (arg == "--motion-lab" && i + 1 < argc) {
            try {
                options.motionLabBpm = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "invalid --motion-lab BPM\n";
                return false;
            }
        } else if (arg == "--vrm" && i + 1 < argc) {
            options.vrmFile = pathFromUtf8(argv[++i]);
            options.startIn3D = true;
        } else if (arg == "--3d") {
            options.startIn3D = true;
        } else if (arg == "--vrm-demo") {
            options.vrmDemo = true;
            options.startIn3D = true;
            if (options.motionLabBpm <= 0) {
                options.motionLabBpm = 123;
            }
        } else if (arg == "--vrm-scale" && i + 1 < argc) {
            try {
                options.vrmScale = std::stof(argv[++i]);
            } catch (...) {
                std::cerr << "invalid --vrm-scale value\n";
                return false;
            }
        } else if (arg == "--vrm-offset-y" && i + 1 < argc) {
            try {
                options.vrmOffsetY = std::stof(argv[++i]);
            } catch (...) {
                std::cerr << "invalid --vrm-offset-y value\n";
                return false;
            }
        } else if (arg == "--window-size" && i + 1 < argc) {
            const std::string value = argv[++i];
            const auto separator = value.find_first_of("xX");
            if (separator == std::string::npos) {
                std::cerr << "invalid --window-size (expected WxH)\n";
                return false;
            }
            try {
                options.windowWidth = std::stoi(value.substr(0, separator));
                options.windowHeight = std::stoi(value.substr(separator + 1));
            } catch (...) {
                std::cerr << "invalid --window-size (expected WxH)\n";
                return false;
            }
            if (options.windowWidth < 320 || options.windowHeight < 240) {
                std::cerr << "--window-size is too small\n";
                return false;
            }
        } else if (arg == "--ui-scale" && i + 1 < argc) {
            std::string value = argv[++i];
            if (value == "auto") {
                options.uiScale = 0.0f;
            } else {
                try {
                    double scale = std::stod(value);
                    if (scale > 10.0) {
                        scale /= 100.0;  // accept "150" as 150 percent
                    }
                    options.uiScale = static_cast<float>(
                        std::clamp(scale, 1.0, 2.5));
                } catch (...) {
                    std::cerr << "invalid --ui-scale value\n";
                    return false;
                }
            }
        } else if (arg == "--fullscreen") {
            options.fullscreen = true;
        } else if (arg == "--vrm-quality" && i + 1 < argc) {
            try {
                options.vrmQuality = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "invalid --vrm-quality value\n";
                return false;
            }
            if (options.vrmQuality < 50 || options.vrmQuality > 100) {
                std::cerr << "--vrm-quality must be 50..100\n";
                return false;
            }
        } else if (arg == "--dance" && i + 1 < argc) {
            options.dance = argv[++i];
            options.startIn3D = true;
        } else if (arg == "--dance-plan" && i + 1 < argc) {
            options.dancePlan = argv[++i];
            options.startIn3D = true;
        } else if (arg == "--dance-span" && i + 1 < argc) {
            try {
                options.danceSpan = std::stod(argv[++i]);
            } catch (...) {
                std::cerr << "invalid --dance-span value" << std::endl;
                return false;
            }
        } else if (arg == "--dance-bpm" && i + 1 < argc) {
            try {
                options.danceBpm = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "invalid --dance-bpm value\n";
                return false;
            }
        } else if (arg == "--dances" && i + 1 < argc) {
            options.dancesDirectory = pathFromUtf8(argv[++i]);
        } else if (arg == "--dance-expression" && i + 1 < argc) {
            options.danceExpression = argv[++i];
        } else if (arg == "--presentation") {
            options.presentation = true;
            options.startIn3D = true;
        } else if (arg == "--debug-feet") {
            options.debugFeet = true;
        } else if (arg == "--debug-skeleton") {
            options.debugSkeleton = true;
        } else if (arg == "--debug-root") {
            options.debugRoot = true;
        } else if (arg == "--legacy-motion") {
            options.legacyMotion = true;
        } else if (arg == "--camera-yaw" && i + 1 < argc) {
            try {
                options.cameraYawDegrees = std::stof(argv[++i]);
                options.hasCameraYaw = true;
            } catch (...) {
                std::cerr << "invalid --camera-yaw value\n";
                return false;
            }
        } else if (arg == "--camera-zoom" && i + 1 < argc) {
            try {
                options.cameraZoom = std::stof(argv[++i]);
            } catch (...) {
                std::cerr << "invalid --camera-zoom value\n";
                return false;
            }
        } else if (arg == "--motion-debug") {
            options.motionDebug = true;
        } else if (arg == "--motion-stepped") {
            options.motionStepped = true;
        } else if (arg == "--capture" && i + 1 < argc) {
            options.captureFile = pathFromUtf8(argv[++i]);
        } else if (arg == "--capture-time" && i + 1 < argc) {
            try {
                options.captureTime = std::stod(argv[++i]);
            } catch (...) {
                std::cerr << "invalid --capture-time value\n";
                return false;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << '\n';
            return false;
        } else if (options.initialFile.empty()) {
            options.initialFile = pathFromUtf8(arg.c_str());
        } else {
            std::cerr << "unexpected extra argument: " << arg << '\n';
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int effectiveArgc = argc;
    char** effectiveArgv = argv;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    // Windows passes the command line in the active code page through main().
    // Convert the wide command line to UTF-8 so file names with Cyrillic or
    // other non-ASCII characters survive into std::filesystem.
    int wideArgc = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Pointers;
    if (wideArgv && wideArgc > 0) {
        utf8Args.reserve(static_cast<std::size_t>(wideArgc));
        for (int i = 0; i < wideArgc; ++i) {
            utf8Args.push_back(wideToUtf8(wideArgv[i]));
        }
        utf8Pointers.reserve(utf8Args.size());
        for (std::string& value : utf8Args) {
            utf8Pointers.push_back(value.data());
        }
        effectiveArgc = wideArgc;
        effectiveArgv = utf8Pointers.data();
    }
    if (wideArgv) {
        LocalFree(wideArgv);
    }
#endif

    bgmedia::desktop::DesktopOptions options;
    if (!parseArgs(effectiveArgc, effectiveArgv, options)) {
        return 0;
    }

    std::cout << "BG Media " << BGMEDIA_VERSION_STRING << " desktop\n";
    bgmedia::desktop::DesktopApp app;
    return app.run(options);
}
