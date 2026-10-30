#pragma once

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/sdlgpu/SdlGpuVrmRenderer.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bgmedia::desktop {

// Desktop-facing wrapper: owns the imported VRM, the generic retargeter and
// the independent SDL_GPU offscreen renderer. It is the only place where 3D
// pixels are converted into an SDL_Texture for the existing UI compositor.
class VrmScene {
public:
    VrmScene();
    ~VrmScene();
    VrmScene(const VrmScene&) = delete;
    VrmScene& operator=(const VrmScene&) = delete;

    [[nodiscard]] bool initializeRenderer(std::string* error);
    [[nodiscard]] bool load(const std::filesystem::path& path, std::string* error);
    void unload();
    void shutdown();

    // Dynamic offscreen resolution. Safe to call every frame: the underlying
    // renderer only reallocates when the size actually changes.
    void setRenderSize(int width, int height);

    [[nodiscard]] bool render(const CharacterPose& pose, double mediaTime,
                              const sdlgpu::Camera& camera,
                              const sdlgpu::RenderSettings& settings,
                              std::string* error);
    // BG Motion 3.0: poses the model from a sampled dance clip instead of a
    // CharacterPose.
    [[nodiscard]] bool renderClip(const motion::ClipPose& pose, double mediaTime,
                                  const sdlgpu::Camera& camera,
                                  const sdlgpu::RenderSettings& settings,
                                  std::string* error);
    [[nodiscard]] bool draw(SDL_Renderer* renderer, const SDL_FRect& destination,
                            std::string* error, bool cover = false);

    [[nodiscard]] const vrm::VrmModel* model() const;
    [[nodiscard]] const animation::RetargetedPose& lastPose() const;
    [[nodiscard]] const sdlgpu::FrameStats& frameStats() const;
    [[nodiscard]] const sdlgpu::RenderSettings& renderSettings() const;
    [[nodiscard]] sdlgpu::RenderSettings& renderSettings();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool renderable() const;
    [[nodiscard]] const std::string& warning() const;
    [[nodiscard]] const std::string& renderError() const;
    [[nodiscard]] double lastRetargetMs() const;
    // The loaded model's leg geometry (reference body when no model is loaded).
    [[nodiscard]] motion::LegGeometry legGeometry() const;

    // BG Motion 3.2 secondary dynamics (VRM 0.x spring bones).
    [[nodiscard]] bool secondaryAvailable() const;
    [[nodiscard]] animation::SecondarySettings& secondarySettings();
    [[nodiscard]] const animation::SecondarySettings& secondarySettings() const;
    [[nodiscard]] const animation::SecondaryStats& secondaryStats() const;
    void resetSecondary();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bgmedia::desktop
