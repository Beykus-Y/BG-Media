// Headless 3D preview used during BG Media 3D development. It exercises the
// same importer, retargeter and SDL_GPU renderer as the desktop application but
// writes one BMP frame, so renderer problems can be isolated from the UI.

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/choreography/Choreography.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/motion3/MotionPrimitive.hpp"
#include "bgmedia/motion3/ProceduralChoreography.hpp"
#include "bgmedia/sdlgpu/SdlGpuVrmRenderer.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

bgmedia::AnalysisResult makeSyntheticAnalysis(double bpm, double duration) {
    using namespace bgmedia;
    AnalysisResult analysis;
    analysis.info.duration = duration;
    analysis.info.sampleRate = 44100;
    analysis.info.sourceChannels = 2;
    analysis.bpm = bpm;
    analysis.bpmConfidence = 1.0f;
    analysis.beatsPerBar = 4;
    analysis.tempoStable = true;
    const double period = 60.0 / bpm;
    double time = 0.45;
    int index = 0;
    while (time < duration - 0.1) {
        Beat beat;
        beat.time = time;
        beat.beatInBar = index % 4;
        beat.barIndex = index / 4;
        beat.downbeat = beat.beatInBar == 0;
        beat.strength = beat.downbeat ? 1.0f : 0.64f;
        beat.confidence = 1.0f;
        analysis.beats.push_back(beat);
        if (beat.downbeat && (beat.barIndex % 2 == 0)) {
            BassAccent accent;
            accent.time = time;
            accent.strength = 0.95f;
            accent.lowBandRatio = 0.75f;
            accent.confidence = 1.0f;
            accent.nearestBeat = index;
            analysis.bassAccents.push_back(accent);
        }
        ++index;
        time += period;
    }
    return analysis;
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path modelPath = "models/Model1.vrm";
    std::filesystem::path outputPath = "artifacts/vrm-preview.bmp";
    double time = 2.0;
    double bpm = 123.0;
    double zoom = 1.0;
    double focusY = 0.0;
    double yawDegrees = 180.0;
    double pitch = 0.10;
    int generatedIndex = -1;
    std::filesystem::path clipPath;
    bool overlay = true;
    bool footworkPrepass = true;  // BG Motion 3.2 stance-level pre-pass for authored clips
    bool spring = false;
    float springIntensity = 1.0f;
    float springDamping = 0.5f;
    int frames = 1;
    double fps = 30.0;
    std::filesystem::path frameDir;
    std::string playerClip;
    std::string switchClip;
    double switchAt = 1.0e9;
    std::filesystem::path dancesDir = "assets/dances";
    bool opaqueOverride = false;  // QA: draw every material opaque (no blend, no alpha test)
    std::vector<std::string> hideMaterials;  // QA: hide materials whose name contains any of these
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) {
            modelPath = std::filesystem::path(argv[++i]);
        } else if (arg == "--out" && i + 1 < argc) {
            outputPath = std::filesystem::path(argv[++i]);
        } else if (arg == "--time" && i + 1 < argc) {
            time = std::stod(argv[++i]);
        } else if (arg == "--bpm" && i + 1 < argc) {
            bpm = std::stod(argv[++i]);
        } else if (arg == "--zoom" && i + 1 < argc) {
            zoom = std::stod(argv[++i]);
        } else if (arg == "--focus-y" && i + 1 < argc) {
            focusY = std::stod(argv[++i]);
        } else if (arg == "--yaw" && i + 1 < argc) {
            yawDegrees = std::stod(argv[++i]);
        } else if (arg == "--pitch" && i + 1 < argc) {
            pitch = std::stod(argv[++i]);
        } else if (arg == "--generated" && i + 1 < argc) {
            generatedIndex = std::atoi(argv[++i]);
        } else if (arg == "--clip" && i + 1 < argc) {
            clipPath = std::filesystem::path(argv[++i]);
        } else if (arg == "--overlay" && i + 1 < argc) {
            overlay = std::atoi(argv[++i]) != 0;
        } else if (arg == "--spring" && i + 1 < argc) {
            spring = std::atoi(argv[++i]) != 0;
        } else if (arg == "--spring-intensity" && i + 1 < argc) {
            springIntensity = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--spring-damping" && i + 1 < argc) {
            springDamping = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--fps" && i + 1 < argc) {
            fps = std::max(1.0, std::atof(argv[++i]));
        } else if (arg == "--framedir" && i + 1 < argc) {
            frameDir = std::filesystem::path(argv[++i]);
        } else if (arg == "--footwork" && i + 1 < argc) {
            footworkPrepass = std::string(argv[++i]) != "off";
        } else if (arg == "--player" && i + 1 < argc) {
            playerClip = argv[++i];
        } else if (arg == "--switch-to" && i + 1 < argc) {
            switchClip = argv[++i];
        } else if (arg == "--switch-at" && i + 1 < argc) {
            switchAt = std::atof(argv[++i]);
        } else if (arg == "--opaque") {
            opaqueOverride = true;
        } else if (arg == "--hide-material" && i + 1 < argc) {
            hideMaterials.push_back(argv[++i]);
        } else if (arg == "--dances" && i + 1 < argc) {
            dancesDir = std::filesystem::path(argv[++i]);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    bgmedia::vrm::LoadResult loaded = bgmedia::vrm::VrmLoader::load(modelPath);
    if (!loaded.ok()) {
        std::fprintf(stderr, "VRM import failed: %s\n", loaded.error.c_str());
        SDL_Quit();
        return 1;
    }
    // QA aid for telling apart "the pose is wrong" from "a rigid material overlaps
    // the body": a hidden material is fully transparent and alpha-tested away.
    for (bgmedia::vrm::Material& material : loaded.model->materials) {
        if (opaqueOverride) {
            material.alphaMode = bgmedia::vrm::AlphaMode::Opaque;
            material.baseColorFactor.w = 1.0f;
        }
        for (const std::string& needle : hideMaterials) {
            if (material.name.find(needle) != std::string::npos) {
                material.alphaMode = bgmedia::vrm::AlphaMode::Mask;
                material.alphaCutoff = 0.5f;
                material.baseColorFactor.w = 0.0f;
                material.emissiveFactor.w = 0.0f;
            }
        }
    }
    const bgmedia::vrm::VrmModel& model = *loaded.model;
    std::printf("VRM %s: nodes=%d meshes=%d tris=%d materials=%d textures=%d "
                "decoded=%d humanoid=%d height=%.3f forward=(%.2f,%.2f,%.2f)\n",
                toUtf8(modelPath).c_str(), model.diagnostics.nodeCount,
                model.diagnostics.meshCount, model.diagnostics.triangleCount,
                model.diagnostics.materialCount, model.diagnostics.textureCount,
                model.diagnostics.decodedTextureCount,
                model.diagnostics.humanoidCoreCount, model.height,
                model.forward.x, model.forward.y, model.forward.z);
    for (const std::string& warning : loaded.warnings) {
        std::printf("warning: %s\n", warning.c_str());
    }

    bgmedia::animation::HumanoidRetargeter rig(model);
    if (!rig.valid()) {
        std::fprintf(stderr, "retargeter unavailable: %s\n", rig.error().c_str());
        SDL_Quit();
        return 1;
    }
    bgmedia::sdlgpu::SdlGpuVrmRenderer renderer;
    std::string error;
    if (!renderer.initialize(&error)) {
        std::fprintf(stderr, "renderer init failed: %s\n", error.c_str());
        SDL_Quit();
        return 1;
    }
    if (!renderer.setModel(model, &error)) {
        std::fprintf(stderr, "renderer model upload failed: %s\n", error.c_str());
        renderer.shutdown();
        SDL_Quit();
        return 1;
    }

    bgmedia::sdlgpu::Camera camera;
    camera.distanceScale = static_cast<float>(zoom);
    camera.targetYBias = static_cast<float>(focusY);
    camera.yaw = static_cast<float>(yawDegrees * 3.14159265358979323846 / 180.0);
    camera.pitch = static_cast<float>(pitch);
    bgmedia::sdlgpu::RenderSettings settings;

    if (!playerClip.empty() && frames >= 1) {
        bgmedia::motion::DancePlayer player;
        const std::size_t loadedCount = player.loadLibrary(dancesDir);
        if (footworkPrepass) {
            player.setLegGeometry(rig.legGeometry());
        } else {
            player.setFootworkPlanning(false);
        }
        if (loadedCount == 0) {
            std::fprintf(stderr, "no dance clips in %s\n", dancesDir.string().c_str());
            renderer.shutdown();
            SDL_Quit();
            return 1;
        }
        player.setOverlayMode(overlay ? bgmedia::motion::OverlayMode::Full
                                      : bgmedia::motion::OverlayMode::Off);
        if (!player.selectClip(playerClip, 0.0, true)) {
            std::fprintf(stderr, "unknown clip %s\n", playerClip.c_str());
            renderer.shutdown();
            SDL_Quit();
            return 1;
        }
        bgmedia::animation::SecondaryDynamics secondary(model);
        bgmedia::animation::SecondarySettings springSettings;
        springSettings.enabled = spring;
        springSettings.intensity = springIntensity;
        springSettings.damping = springDamping;
        bool switched = false;
        std::error_code ec;
        if (frames > 1) {
            std::filesystem::create_directories(frameDir, ec);
        } else {
            std::filesystem::create_directories(outputPath.parent_path(), ec);
        }
        std::printf("player %s -> %s @ %.2f, overlay %d, spring %d\n",
                    playerClip.c_str(), switchClip.c_str(), switchAt, overlay ? 1 : 0,
                    spring ? 1 : 0);
        for (int frame = 0; frame < frames; ++frame) {
            const double frameTime = time + static_cast<double>(frame) / fps;
            if (!switched && !switchClip.empty() && frameTime >= switchAt) {
                if (player.selectClip(switchClip, frameTime, false)) {
                    std::printf("switched to %s at %.2f\n", switchClip.c_str(), frameTime);
                }
                switched = true;
            }
            const bgmedia::motion::DanceFrame danceFrame = player.update(frameTime);
            bgmedia::animation::RetargetedPose retargeted =
                rig.applyClip(danceFrame.pose, frameTime);
            if (spring) {
                if (frame == 0) {
                    secondary.reset(retargeted);
                }
                secondary.update(retargeted, frame == 0 ? 0.0 : 1.0 / fps,
                                 springSettings);
            }
            if (!renderer.render(retargeted, camera, settings, &error)) {
                std::fprintf(stderr, "render failed at frame %d: %s\n", frame,
                             error.c_str());
                renderer.shutdown();
                SDL_Quit();
                return 1;
            }
            std::filesystem::path path = outputPath;
            if (frames > 1) {
                char name[64];
                std::snprintf(name, sizeof name, "frame_%04d.bmp", frame);
                path = frameDir / name;
            }
            SDL_Surface* surface = SDL_CreateSurfaceFrom(
                renderer.width(), renderer.height(), SDL_PIXELFORMAT_RGBA32,
                const_cast<std::uint8_t*>(renderer.pixels().data()),
                renderer.width() * 4);
            if (!surface || !SDL_SaveBMP(surface, toUtf8(path).c_str())) {
                std::fprintf(stderr, "frame save failed: %s\n", SDL_GetError());
                if (surface) SDL_DestroySurface(surface);
                renderer.shutdown();
                SDL_Quit();
                return 1;
            }
            SDL_DestroySurface(surface);
            if (frame == 0 || frame == frames - 1) {
                const auto& ss = secondary.stats();
                std::printf("frame %d t=%.2f clip=%s beat=%.2f layers=%d/%d "
                            "springDisp=%.3fmm\n",
                            frame, frameTime, danceFrame.clipId.c_str(),
                            danceFrame.clipBeat, danceFrame.overlayActive,
                            danceFrame.overlayActive + danceFrame.overlayRejected,
                            ss.maxDisplacement * 1000.0f);
            }
        }
        std::printf("wrote %d frame(s)\n", frames);
        renderer.shutdown();
        SDL_Quit();
        return 0;
    }

    if ((generatedIndex >= 0 || !clipPath.empty()) && frames >= 1) {
        bgmedia::motion::HumanoidClip clip;
        std::vector<bgmedia::motion::PrimitiveCue> cues;
        if (!clipPath.empty()) {
            bgmedia::motion::ClipLoadResult clipLoaded =
                bgmedia::motion::loadClipFile(clipPath);
            if (!clipLoaded.ok) {
                std::fprintf(stderr, "clip load failed: %s\n",
                             clipLoaded.diagnostics.empty()
                                 ? "unknown error"
                                 : clipLoaded.diagnostics.front().message.c_str());
                renderer.shutdown();
                SDL_Quit();
                return 1;
            }
            clip = std::move(clipLoaded.clip);
            cues = bgmedia::motion::planPrimitiveOverlays(clip, 7u, 0.8f, true);
        } else {
            std::vector<bgmedia::motion::GeneratedDance> showcase =
                bgmedia::motion::ProceduralChoreography::showcase();
            if (generatedIndex < 0 ||
                static_cast<std::size_t>(generatedIndex) >= showcase.size()) {
                generatedIndex = 0;
            }
            clip = showcase[static_cast<std::size_t>(generatedIndex)].clip;
            cues = showcase[static_cast<std::size_t>(generatedIndex)].overlays;
        }
        bgmedia::animation::SecondaryDynamics secondary(model);
        bgmedia::animation::SecondarySettings springSettings;
        springSettings.enabled = spring;
        springSettings.intensity = springIntensity;
        springSettings.damping = springDamping;
        std::printf("clip %s: beats %.0f bpm %.0f cues %zu fingerBones %d "
                    "springChains %d\n",
                    clip.id.c_str(), clip.beats, clip.bpm, cues.size(),
                    model.fingerBoneCount(), secondary.chainCount());
        std::error_code ec;
        if (frames > 1) {
            std::filesystem::create_directories(frameDir, ec);
        } else {
            std::filesystem::create_directories(outputPath.parent_path(), ec);
        }
        for (int frame = 0; frame < frames; ++frame) {
            const double frameTime = time + static_cast<double>(frame) / fps;
            const double beat = frameTime * clip.bpm / 60.0;
            bgmedia::motion::ClipPose pose = bgmedia::motion::sampleClip(clip, beat);
            if (overlay && !cues.empty()) {
                bgmedia::motion::CompositionOptions options;
                bgmedia::motion::ComposedPose composed = bgmedia::motion::composeMotion(
                    pose, cues, beat, clip.beats, clip.loop, options);
                pose = composed.pose;
            }
            bgmedia::animation::RetargetedPose retargeted =
                rig.applyClip(pose, frameTime);
            if (spring) {
                if (frame == 0) {
                    secondary.reset(retargeted);
                }
                secondary.update(retargeted, frame == 0 ? 0.0 : 1.0 / fps,
                                 springSettings);
            }
            if (!renderer.render(retargeted, camera, settings, &error)) {
                std::fprintf(stderr, "render failed at frame %d: %s\n", frame,
                             error.c_str());
                renderer.shutdown();
                SDL_Quit();
                return 1;
            }
            std::filesystem::path path = outputPath;
            if (frames > 1) {
                char name[64];
                std::snprintf(name, sizeof name, "frame_%04d.bmp", frame);
                path = frameDir / name;
            }
            SDL_Surface* surface = SDL_CreateSurfaceFrom(
                renderer.width(), renderer.height(), SDL_PIXELFORMAT_RGBA32,
                const_cast<std::uint8_t*>(renderer.pixels().data()),
                renderer.width() * 4);
            if (!surface || !SDL_SaveBMP(surface, toUtf8(path).c_str())) {
                std::fprintf(stderr, "frame save failed: %s\n", SDL_GetError());
                if (surface) SDL_DestroySurface(surface);
                renderer.shutdown();
                SDL_Quit();
                return 1;
            }
            SDL_DestroySurface(surface);
            if (frame == 0 || frame == frames - 1) {
                const auto& ss = secondary.stats();
                std::printf("frame %d t=%.2f beat=%.2f wrist=%d footErr=%.4f "
                            "springDisp=%.4f stretch=%.4f maxA=%.2fms\n",
                            frame, frameTime, beat, pose.detailedHands ? 1 : 0,
                            retargeted.stats.maxFootError, ss.maxDisplacement,
                            ss.maxStretch, ss.simulateMilliseconds);
            }
        }
        std::printf("wrote %d frame(s)\n", frames);
        renderer.shutdown();
        SDL_Quit();
        return 0;
    }

    const bgmedia::AnalysisResult analysis = makeSyntheticAnalysis(bpm, 120.0);
    const bgmedia::Choreography choreography =
        bgmedia::ChoreographyBuilder{}.build(analysis);
    const bgmedia::CharacterPose pose =
        bgmedia::ChoreographySampler{}.sample(choreography, time);
    const bgmedia::animation::RetargetedPose retargeted = rig.apply(pose, time);
    std::printf("pose move=%d bob=%.2f lean=%.2f armL=%.2f armR=%.2f rot=%.3f hipDrop=%.3f footErr=%.4f\n",
                static_cast<int>(pose.move), pose.bob, pose.lean, pose.armLeft,
                pose.armRight, pose.rotation, retargeted.stats.hipDrop,
                retargeted.stats.maxFootError);
    const auto target = model.bounds.center();
    const auto eye = target + bgmedia::math::Vec3{0.0f, 0.42f, -model.height * 2.22f};
    const auto view = bgmedia::math::lookAtLH(eye, target, {0.0f, 1.0f, 0.0f});
    const auto proj = bgmedia::math::perspectiveFovLH(0.56f, 1.5f, 0.03f, 200.0f);
    const auto vp = proj * view;
    const auto clipPoint = [&](bgmedia::math::Vec3 p) {
        const auto m = vp.m;
        return bgmedia::math::Vec4{m[0]*p.x + m[4]*p.y + m[8]*p.z + m[12],
                                   m[1]*p.x + m[5]*p.y + m[9]*p.z + m[13],
                                   m[2]*p.x + m[6]*p.y + m[10]*p.z + m[14],
                                   m[3]*p.x + m[7]*p.y + m[11]*p.z + m[15]};
    };
    const auto c = clipPoint(target);
    std::printf("camera eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f) clip center=(%.3f,%.3f,%.3f,%.3f) ndc=(%.3f,%.3f)\n",
                eye.x, eye.y, eye.z, target.x, target.y, target.z, c.x, c.y, c.z, c.w,
                c.x / c.w, c.y / c.w);
    if (!renderer.render(retargeted, camera, settings, &error)) {
        std::fprintf(stderr, "render failed: %s\n", error.c_str());
        renderer.shutdown();
        SDL_Quit();
        return 1;
    }

    const auto& stats = renderer.stats();
    std::printf("frame %dx%d: tris=%d draws=%d gpu=%.2fms readback=%.2fms "
                "driver=%s tex=%d/%llu KiB\n",
                stats.width, stats.height, stats.triangles, stats.drawCalls,
                stats.gpuMilliseconds, stats.readbackMilliseconds,
                stats.driver.c_str(), stats.textureCount,
                static_cast<unsigned long long>(stats.textureBytes / 1024));

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        renderer.width(), renderer.height(), SDL_PIXELFORMAT_RGBA32,
        const_cast<std::uint8_t*>(renderer.pixels().data()),
        renderer.width() * 4);
    if (!surface) {
        std::fprintf(stderr, "surface creation failed: %s\n", SDL_GetError());
        renderer.shutdown();
        SDL_Quit();
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    if (!SDL_SaveBMP(surface, toUtf8(outputPath).c_str())) {
        std::fprintf(stderr, "BMP save failed: %s\n", SDL_GetError());
        SDL_DestroySurface(surface);
        renderer.shutdown();
        SDL_Quit();
        return 1;
    }
    SDL_DestroySurface(surface);
    std::printf("wrote %s\n", toUtf8(outputPath).c_str());

    renderer.shutdown();
    SDL_Quit();
    return 0;
}
