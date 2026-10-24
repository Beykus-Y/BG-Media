#pragma once

#include <filesystem>
#include <memory>

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
