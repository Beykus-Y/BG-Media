#include "VrmScene.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace bgmedia::desktop {

namespace {

// Largest spring step the host feeds in one frame. A longer stall (window drag,
// debugger, hidden window) is not simulated as one big impulse.
constexpr double kMaxSpringFrameSeconds = 0.1;

} // namespace

struct VrmScene::Impl {
    sdlgpu::SdlGpuVrmRenderer renderer;
    std::unique_ptr<vrm::VrmModel> model;
    std::unique_ptr<animation::HumanoidRetargeter> retargeter;
    std::unique_ptr<animation::SecondaryDynamics> secondary;
    animation::SecondarySettings secondarySettings;
    animation::RetargetedPose lastPose;
    sdlgpu::RenderSettings settings;
    SDL_Texture* texture = nullptr;
    int textureWidth = 0;
    int textureHeight = 0;
    bool rendererInitialized = false;
    bool renderable = false;
    std::string warning;
    std::string renderError;
    double lastRetargetMs = 0.0;
    double lastMediaTime = 0.0;
    bool haveMediaTime = false;
    std::chrono::steady_clock::time_point lastFrameWall{};
    bool haveFrameWall = false;

    void destroyTexture() {
        if (texture) {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        textureWidth = 0;
        textureHeight = 0;
    }

    // Spring time is WALL-clock time: the hair keeps settling while playback is
    // paused and does not speed up or slow down with the playback rate. Only
    // seeks are detected from the media position.
    [[nodiscard]] double wallFrameSeconds() {
        const auto now = std::chrono::steady_clock::now();
        double dt = 0.0;
        if (haveFrameWall) {
            dt = std::chrono::duration<double>(now - lastFrameWall).count();
        }
        lastFrameWall = now;
        haveFrameWall = true;
        return std::clamp(dt, 0.0, kMaxSpringFrameSeconds);
    }

    void applySecondary(double mediaTime) {
        if (!secondary || !secondary->available()) {
            return;
        }
        const double wallDt = wallFrameSeconds();
        const bool seek = haveMediaTime &&
                          (mediaTime < lastMediaTime - 0.05 ||
                           mediaTime > lastMediaTime + 0.5);
        if (!haveMediaTime || seek) {
            secondary->reset(lastPose);
            lastMediaTime = mediaTime;
            haveMediaTime = true;
            secondary->update(lastPose, 0.0, secondarySettings);
            return;
        }
        lastMediaTime = mediaTime;
        secondary->update(lastPose, wallDt, secondarySettings);
    }
};

VrmScene::VrmScene() : impl_(std::make_unique<Impl>()) {}

VrmScene::~VrmScene() {
    shutdown();
}

void VrmScene::shutdown() {
    unload();
    impl_->renderer.shutdown();
    impl_->rendererInitialized = false;
    impl_->destroyTexture();
}

void VrmScene::setRenderSize(int width, int height) {
    impl_->renderer.setRenderSize(width, height);
}

bool VrmScene::initializeRenderer(std::string* error) {
    if (impl_->rendererInitialized) {
        return true;
    }
    if (!impl_->renderer.initialize(error)) {
        impl_->rendererInitialized = false;
        return false;
    }
    impl_->rendererInitialized = true;
    return true;
}

bool VrmScene::load(const std::filesystem::path& path, std::string* error) {
    unload();
    vrm::LoadResult loaded = vrm::VrmLoader::load(path);
    if (!loaded.ok()) {
        if (error) {
            *error = loaded.error.empty() ? "VRM import failed" : loaded.error;
        }
        return false;
    }
    auto model = std::move(loaded.model);
    auto retargeter =
        std::make_unique<animation::HumanoidRetargeter>(*model);
    if (!retargeter->valid()) {
        if (error) {
            *error = retargeter->error();
        }
        return false;
    }
    std::string warning;
    for (const std::string& text : loaded.warnings) {
        if (!warning.empty()) {
            warning += " | ";
        }
        warning += text;
    }
    if (impl_->rendererInitialized) {
        std::string renderError;
        if (!impl_->renderer.setModel(*model, &renderError)) {
            impl_->renderError = renderError;
            if (!warning.empty()) {
                warning += " | ";
            }
            warning += "3D upload failed: " + renderError;
        } else {
            impl_->renderable = true;
            // GPU resources now own the pixels. Releasing the CPU copies keeps
            // the process footprint within the measured ~150 MB range.
            model->releaseTexturePixels();
        }
    } else {
        impl_->renderError = "SDL_GPU renderer is not initialized";
    }
    impl_->model = std::move(model);
    impl_->retargeter = std::move(retargeter);
    impl_->secondary = std::make_unique<animation::SecondaryDynamics>(*impl_->model);
    impl_->lastPose = {};
    impl_->haveMediaTime = false;
    impl_->lastMediaTime = 0.0;
    impl_->warning = std::move(warning);
    impl_->lastRetargetMs = 0.0;
    return true;
}

void VrmScene::unload() {
    impl_->renderer.releaseModel();
    impl_->destroyTexture();
    impl_->secondary.reset();
    impl_->model.reset();
    impl_->retargeter.reset();
    impl_->lastPose = {};
    impl_->renderable = false;
    impl_->warning.clear();
    impl_->renderError.clear();
    impl_->lastRetargetMs = 0.0;
    impl_->haveMediaTime = false;
    impl_->lastMediaTime = 0.0;
}

bool VrmScene::render(const CharacterPose& pose, double mediaTime,
                      const sdlgpu::Camera& camera,
                      const sdlgpu::RenderSettings& settings, std::string* error) {
    if (!impl_->model || !impl_->retargeter || !impl_->renderable ||
        !impl_->renderer.hasModel()) {
        if (error) {
            *error = impl_->renderError.empty()
                         ? "VRM scene is not renderable"
                         : impl_->renderError;
        }
        return false;
    }
    impl_->lastPose = impl_->retargeter->apply(pose, mediaTime);
    impl_->lastRetargetMs = impl_->lastPose.stats.retargetMilliseconds;
    if (!impl_->lastPose.stats.valid) {
        if (error) {
            *error = "retargeting produced an invalid pose";
        }
        return false;
    }
    impl_->applySecondary(mediaTime);
    if (!impl_->renderer.render(impl_->lastPose, camera, settings, error)) {
        return false;
    }
    impl_->settings = settings;
    return true;
}

bool VrmScene::renderClip(const motion::ClipPose& pose, double mediaTime,
                          const sdlgpu::Camera& camera,
                          const sdlgpu::RenderSettings& settings, std::string* error) {
    if (!impl_->model || !impl_->retargeter || !impl_->renderable ||
        !impl_->renderer.hasModel()) {
        if (error) {
            *error = impl_->renderError.empty() ? "VRM scene is not renderable"
                                                : impl_->renderError;
        }
        return false;
    }
    impl_->lastPose = impl_->retargeter->applyClip(pose, mediaTime);
    impl_->lastRetargetMs = impl_->lastPose.stats.retargetMilliseconds;
    if (!impl_->lastPose.stats.valid) {
        if (error) {
            *error = "retargeting produced an invalid pose";
        }
        return false;
    }
    impl_->applySecondary(mediaTime);
    if (!impl_->renderer.render(impl_->lastPose, camera, settings, error)) {
        return false;
    }
    impl_->settings = settings;
    return true;
}

bool VrmScene::draw(SDL_Renderer* renderer, const SDL_FRect& destination,
                    std::string* error, bool cover) {
    if (!renderer) {
        return false;
    }
    const auto& rendererPixels = impl_->renderer.pixels();
    if (!impl_->renderer.hasModel() || rendererPixels.empty()) {
        if (error) {
            *error = "no rendered 3D frame";
        }
        return false;
    }
    const int width = impl_->renderer.width();
    const int height = impl_->renderer.height();
    if (!impl_->texture || impl_->textureWidth != width ||
        impl_->textureHeight != height) {
        impl_->destroyTexture();
        impl_->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STREAMING, width,
                                           height);
        if (!impl_->texture) {
            if (error) {
                *error = std::string("SDL_Texture creation failed: ") +
                         SDL_GetError();
            }
            return false;
        }
        SDL_SetTextureScaleMode(impl_->texture, SDL_SCALEMODE_LINEAR);
        impl_->textureWidth = width;
        impl_->textureHeight = height;
    }
    if (!SDL_UpdateTexture(impl_->texture, nullptr, rendererPixels.data(),
                           width * 4)) {
        if (error) {
            *error = std::string("SDL_UpdateTexture failed: ") + SDL_GetError();
        }
        return false;
    }
    SDL_FRect target = destination;
    const float sourceAspect = static_cast<float>(width) / height;
    const float targetAspect = destination.w / std::max(0.001f, destination.h);
    if (cover) {
        // Fill the whole destination and crop the overflowing edges of the frame.
        SDL_FRect source{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
        if (targetAspect > sourceAspect) {
            source.h = static_cast<float>(width) / targetAspect;
            source.y = (static_cast<float>(height) - source.h) * 0.5f;
        } else {
            source.w = static_cast<float>(height) * targetAspect;
            source.x = (static_cast<float>(width) - source.w) * 0.5f;
        }
        SDL_RenderTexture(renderer, impl_->texture, &source, &destination);
        return true;
    }
    if (targetAspect > sourceAspect) {
        target.w = destination.h * sourceAspect;
        target.x = destination.x + (destination.w - target.w) * 0.5f;
    } else {
        target.h = destination.w / sourceAspect;
        target.y = destination.y + (destination.h - target.h) * 0.5f;
    }
    SDL_RenderTexture(renderer, impl_->texture, nullptr, &target);
    return true;
}

const vrm::VrmModel* VrmScene::model() const { return impl_->model.get(); }

const animation::RetargetedPose& VrmScene::lastPose() const {
    return impl_->lastPose;
}

const sdlgpu::FrameStats& VrmScene::frameStats() const {
    return impl_->renderer.stats();
}

const sdlgpu::RenderSettings& VrmScene::renderSettings() const {
    return impl_->settings;
}

sdlgpu::RenderSettings& VrmScene::renderSettings() { return impl_->settings; }

bool VrmScene::ready() const { return impl_->model != nullptr; }
bool VrmScene::renderable() const { return impl_->renderable; }

const std::string& VrmScene::warning() const { return impl_->warning; }
const std::string& VrmScene::renderError() const { return impl_->renderError; }

double VrmScene::lastRetargetMs() const { return impl_->lastRetargetMs; }

motion::LegGeometry VrmScene::legGeometry() const {
    return impl_->retargeter ? impl_->retargeter->legGeometry() : motion::LegGeometry{};
}

bool VrmScene::secondaryAvailable() const {
    return impl_->secondary && impl_->secondary->available();
}

animation::SecondarySettings& VrmScene::secondarySettings() {
    return impl_->secondarySettings;
}

const animation::SecondarySettings& VrmScene::secondarySettings() const {
    return impl_->secondarySettings;
}

const animation::SecondaryStats& VrmScene::secondaryStats() const {
    static const animation::SecondaryStats empty;
    return impl_->secondary ? impl_->secondary->stats() : empty;
}

void VrmScene::resetSecondary() {
    if (impl_->secondary) {
        impl_->secondary->reset(impl_->lastPose);
        impl_->haveMediaTime = true;
    }
}

} // namespace bgmedia::desktop
