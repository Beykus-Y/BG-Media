// Development tool: renders contact sheets of dance clips (or a whole dance
// schedule) on a VRM model with the real retargeter and the real SDL_GPU
// renderer. Frames are placed on musical beats, so the same sheet shows the
// choreography exactly as the desktop app plays it.
//
//   bgmedia_motion_sheet --model models/Model1.vrm --dances assets/dances
//       --plan groove,hiphop --span 16 --bpm 100 --beats 0:16:10
//       --view front --cols 5 --out artifacts/sheet.bmp

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/motion3/DanceDirector.hpp"
#include "bgmedia/sdlgpu/SdlGpuVrmRenderer.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace bgmedia;

std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream stream(text);
    while (std::getline(stream, item, sep)) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::vector<double> parseBeats(const std::string& text) {
    std::vector<double> beats;
    if (text.find(':') != std::string::npos) {
        const auto parts = split(text, ':');
        if (parts.size() == 3) {
            const double a = std::stod(parts[0]);
            const double b = std::stod(parts[1]);
            const int n = std::max(1, std::stoi(parts[2]));
            for (int i = 0; i < n; ++i) {
                beats.push_back(n == 1 ? a : a + (b - a) * i / n);
            }
        }
        return beats;
    }
    for (const auto& item : split(text, ',')) {
        beats.push_back(std::stod(item));
    }
    return beats;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path modelPath = "models/Model1.vrm";
    std::filesystem::path dancesPath = "assets/dances";
    std::filesystem::path outPath = "artifacts/sheet.bmp";
    std::string plan = "groove";
    std::string beatSpec = "0:16:10";
    std::string view = "front";
    double bpm = 100.0;
    double span = 16.0;
    int cols = 5;
    int cropWidth = 560;
    double scale = 0.55;
    double zoom = 1.0;
    double modelScale = 1.0;
    bool oneShotFirst = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--model") modelPath = next();
        else if (arg == "--dances") dancesPath = next();
        else if (arg == "--out") outPath = next();
        else if (arg == "--plan") plan = next();
        else if (arg == "--beats") beatSpec = next();
        else if (arg == "--view") view = next();
        else if (arg == "--bpm") bpm = std::stod(next());
        else if (arg == "--span") span = std::stod(next());
        else if (arg == "--cols") cols = std::stoi(next());
        else if (arg == "--crop") cropWidth = std::stoi(next());
        else if (arg == "--scale") scale = std::stod(next());
        else if (arg == "--zoom") zoom = std::stod(next());
        else if (arg == "--model-scale") modelScale = std::stod(next());
        else if (arg == "--one-shot") oneShotFirst = true;
    }

    motion::DanceLibrary library;
    library.loadDirectory(dancesPath);
    for (const auto& d : library.diagnostics()) {
        std::fprintf(stderr, "%s: %s: %s\n",
                     d.severity == motion::ClipDiagnostic::Severity::Error ? "error" : "warning",
                     d.file.c_str(), d.message.c_str());
    }
    if (library.empty()) {
        std::fprintf(stderr, "no dances loaded from %s\n", dancesPath.string().c_str());
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    vrm::LoadResult loaded = vrm::VrmLoader::load(modelPath);
    if (!loaded.ok()) {
        std::fprintf(stderr, "VRM import failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const vrm::VrmModel& model = *loaded.model;
    animation::HumanoidRetargeter rig(model);
    if (!rig.valid()) {
        std::fprintf(stderr, "retargeter: %s\n", rig.error().c_str());
        return 1;
    }
    sdlgpu::SdlGpuVrmRenderer renderer;
    std::string error;
    if (!renderer.initialize(&error) || !renderer.setModel(model, &error)) {
        std::fprintf(stderr, "renderer: %s\n", error.c_str());
        return 1;
    }

    // Schedule: clip styles every `span` music beats starting at beat 0. Time 0
    // is beat 0 (constant grid).
    motion::DanceDirector director(&library);
    director.setBeatClock(motion::BeatClock::constant(bpm, 0.0));
    director.reset(0.0);
    double beat = 0.0;
    bool first = true;
    for (const std::string& name : split(plan, ',')) {
        const motion::HumanoidClip* clip = library.find(name);
        if (!clip) clip = library.forStyle(motion::styleFromName(name));
        if (!clip) {
            std::fprintf(stderr, "unknown clip '%s'\n", name.c_str());
            continue;
        }
        const double align = director.beatClock().timeAtBeat(beat);
        const double fade = std::clamp(2.0 * 60.0 / bpm, 0.45, 1.1);
        motion::ScheduleSegment seg = director.makeSegment(
            *clip, first ? align - 100.0 : align - fade * 0.5, align, fade);
        if (first) {
            seg.fadeSeconds = 0.0;
            seg.loop = !oneShotFirst;
        }
        first = false;
        director.schedule().append(seg);
        beat += span;
    }

    const std::vector<double> beats = parseBeats(beatSpec);
    const int rows = static_cast<int>((beats.size() + cols - 1) / cols);
    const int tileW = static_cast<int>(cropWidth * scale);
    const int tileH = static_cast<int>(640 * scale);
    std::vector<std::uint8_t> sheet(static_cast<std::size_t>(tileW * cols) * tileH * rows * 4, 0);

    sdlgpu::Camera camera;
    camera.pitch = 0.08f;
    camera.distanceScale = static_cast<float>(zoom);
    if (view == "front") camera.yaw = 3.14159265f;
    else if (view == "side") camera.yaw = 3.14159265f * 0.5f;
    else if (view == "back") camera.yaw = 0.0f;
    else camera.yaw = 3.14159265f - 0.6f;
    sdlgpu::RenderSettings settings;
    settings.modelScale = static_cast<float>(modelScale);

    for (std::size_t n = 0; n < beats.size(); ++n) {
        const double t = director.beatClock().timeAtBeat(beats[n]);
        const motion::DanceFrame frame = director.sample(t);
        const animation::RetargetedPose pose = rig.applyClip(frame.pose, t);
        std::printf("beat %6.2f  %-10s next=%-10s clipBeat=%6.2f phase=%.2f trans=%5.2f "
                    "footErr=%.4f hipDrop=%.3f reqDrop=%.3f hipsY=%.3f lowFoot=%.3f\n",
                    beats[n], frame.clipId.c_str(), frame.nextClipId.c_str(), frame.clipBeat,
                    frame.phase, frame.transitionProgress, pose.stats.maxFootError,
                    pose.stats.hipDrop, pose.stats.requestedHipDrop,
                    pose.bonePosition(vrm::HumanoidBone::Hips, model).y, pose.stats.lowestFootY);
        if (!renderer.render(pose, camera, settings, &error)) {
            std::fprintf(stderr, "render failed: %s\n", error.c_str());
            return 1;
        }
        const auto& pixels = renderer.pixels();
        const int srcW = renderer.width();
        const int srcH = renderer.height();
        const int x0 = (srcW - cropWidth) / 2;
        const int col = static_cast<int>(n) % cols;
        const int row = static_cast<int>(n) / cols;
        for (int y = 0; y < tileH; ++y) {
            for (int x = 0; x < tileW; ++x) {
                // Box filter over the source pixels that map to this tile pixel.
                const int sx0 = x0 + static_cast<int>(x / scale);
                const int sx1 = std::max(sx0 + 1, x0 + static_cast<int>((x + 1) / scale));
                const int sy0 = static_cast<int>(y / scale);
                const int sy1 = std::max(sy0 + 1, static_cast<int>((y + 1) / scale));
                int sum[4] = {0, 0, 0, 0};
                int count = 0;
                for (int sy = sy0; sy < sy1 && sy < srcH; ++sy) {
                    for (int sx = sx0; sx < sx1 && sx < srcW; ++sx) {
                        const std::uint8_t* p = &pixels[(static_cast<std::size_t>(sy) * srcW + sx) * 4];
                        for (int c = 0; c < 4; ++c) sum[c] += p[c];
                        ++count;
                    }
                }
                std::uint8_t* dst =
                    &sheet[((static_cast<std::size_t>(row * tileH + y)) * (tileW * cols) +
                            (col * tileW + x)) * 4];
                for (int c = 0; c < 4; ++c) {
                    dst[c] = static_cast<std::uint8_t>(count ? sum[c] / count : 0);
                }
                dst[3] = 255;
            }
        }
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(tileW * cols, tileH * rows, SDL_PIXELFORMAT_RGBA32,
                                                 sheet.data(), tileW * cols * 4);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (!surface || !SDL_SaveBMP(surface, outPath.string().c_str())) {
        std::fprintf(stderr, "save failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DestroySurface(surface);
    std::printf("wrote %s (%dx%d, %zu frames)\n", outPath.string().c_str(), tileW * cols,
                tileH * rows, beats.size());
    renderer.shutdown();
    SDL_Quit();
    return 0;
}
