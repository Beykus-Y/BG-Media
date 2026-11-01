#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bgmedia::desktop {

struct DesktopOptions {
    std::filesystem::path initialFile;
    std::filesystem::path cacheDirectory;
    std::filesystem::path captureFile;
    std::filesystem::path vrmFile;
    double captureTime = 2.0;
    bool noCache = false;
    bool noAudio = false;
    bool audioSmoke = false;
    bool autoplay = false;

    // BG Mix screen options (also used by headless captures).
    bool startInMix = false;
    std::vector<std::filesystem::path> mixTracks;

    // BG Motion lab/demo options. They affect only choreography rendering,
    // not the analyzer or the cache.
    int motionLabBpm = 0;
    bool motionDebug = false;
    bool motionStepped = false;

    // 3D character options.
    bool startIn3D = false;
    bool vrmDemo = false;
    float vrmScale = 1.0f;
    float vrmOffsetY = 0.0f;

    // Display / high-DPI options. Zero means "use the saved preference or the
    // automatic choice" (window size) and "automatic" (UI scale).
    int windowWidth = 0;
    int windowHeight = 0;
    float uiScale = 0.0f;       // 0 = automatic; 1.0 - 2.0 = manual
    int vrmQuality = 0;         // 0 = default; 50 / 75 / 100 percent of the stage
    bool fullscreen = false;

    // BG Motion 3.0.
    std::filesystem::path dancesDirectory;  // default: <assets>/dances
    std::string dance;     // initial style: groove|hiphop|sidestep|energetic|idle|auto|test
    int danceBpm = 0;      // Motion Lab BPM when no music is loaded
    std::string dancePlan; // comma separated styles played in sequence (demo / captures)
    double danceSpan = 16.0;  // beats per style in --dance-plan
    std::string danceExpression;  // off|soft|expressive; empty = profile/Soft
    bool presentation = false;
    bool debugFeet = false;
    bool debugSkeleton = false;
    bool debugRoot = false;
    bool legacyMotion = false;  // use the BG Motion 2.0 CharacterPose in 3D
    bool hasCameraYaw = false;
    float cameraYawDegrees = 180.0f;
    float cameraZoom = 1.0f;
};

class DesktopApp {
public:
    DesktopApp();
    ~DesktopApp();

    DesktopApp(const DesktopApp&) = delete;
    DesktopApp& operator=(const DesktopApp&) = delete;

    int run(const DesktopOptions& options);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bgmedia::desktop
