#pragma once

// SDL_GPU offscreen renderer for an imported VRM character.
//
// Why offscreen: the established BG Media desktop UI is an SDL_Renderer
// application. SDL_GPU and SDL_Renderer must not fight over the same window or
// GPU context. This adapter owns an independent SDL_GPU device, renders the 3D
// scene to an RGBA8 offscreen target with a depth buffer, reads the result back
// with SDL's documented download path, and hands the pixels to the UI's normal
// SDL_Texture compositor. BG Beat/Sync/Motion and the VRM importer stay free of
// SDL and graphics API types.

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bgmedia::sdlgpu {

struct Camera {
    float yaw = 3.14159265f; // radians; pi places the camera in front of a -Z VRM character
    float pitch = 0.10f;
    float distanceScale = 1.0f;
    float fov = 0.56f;
    float targetYBias = 0.0f; // ~32 degrees
};

struct RenderSettings {
    float modelScale = 1.0f;
    math::Vec3 modelOffset{};
    math::Vec4 lightDirection{0.32f, 0.72f, -0.61f, 1.0f};
    math::Vec4 rimColor{0.090f, 0.145f, 0.130f, 1.0f};
};

struct FrameStats {
    bool valid = false;
    int width = 0;
    int height = 0;
    int triangles = 0;
    int drawCalls = 0;
    int skinCount = 0;
    int textureCount = 0;
    std::uint64_t textureBytes = 0;
    double gpuMilliseconds = 0.0;
    double readbackMilliseconds = 0.0;
    std::string driver;
    std::string deviceName;
};

class SdlGpuVrmRenderer {
public:
    SdlGpuVrmRenderer();
    ~SdlGpuVrmRenderer();
    SdlGpuVrmRenderer(const SdlGpuVrmRenderer&) = delete;
    SdlGpuVrmRenderer& operator=(const SdlGpuVrmRenderer&) = delete;

    [[nodiscard]] bool initialize(std::string* error);
    void shutdown();

    // Dynamic offscreen size. Recreates the color/depth targets and the
    // readback buffer only when the size actually changes, so a window resize
    // does not allocate GPU resources per frame. Clamped to 3840x2160.
    void setRenderSize(int width, int height);

    [[nodiscard]] bool setModel(const vrm::VrmModel& model, std::string* error);
    void releaseModel();

    [[nodiscard]] bool render(const animation::RetargetedPose& pose,
                              const Camera& camera,
                              const RenderSettings& settings,
                              std::string* error);

    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool hasModel() const;
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] const FrameStats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bgmedia::sdlgpu
