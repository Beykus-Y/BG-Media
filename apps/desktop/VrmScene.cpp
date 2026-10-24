#include "VrmScene.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace bgmedia::desktop {


struct VrmScene::Impl {
    sdlgpu::SdlGpuVrmRenderer renderer;
    std::unique_ptr<vrm::VrmModel> model;
    std::unique_ptr<animation::HumanoidRetargeter> retargeter;
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

    void destroyTexture() {
        if (texture) {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        textureWidth = 0;
        textureHeight = 0;
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
    impl_->warning = std::move(warning);
    impl_->lastRetargetMs = 0.0;
    return true;
}

void VrmScene::unload() {
    impl_->renderer.releaseModel();
    impl_->destroyTexture();
    impl_->model.reset();
    impl_->retargeter.reset();
    impl_->lastPose = {};
    impl_->renderable = false;
    impl_->warning.clear();
    impl_->renderError.clear();
    impl_->lastRetargetMs = 0.0;
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
    if (!impl_->renderer.render(impl_->lastPose, camera, settings, error)) {
        return false;
    }
    impl_->settings = settings;
    return true;
}

bool VrmScene::draw(SDL_Renderer* renderer, const SDL_FRect& destination,
                    std::string* error) {
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

} // namespace bgmedia::desktop
