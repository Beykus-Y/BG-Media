// bgmedia_skin_check: does the geometry measured on the CPU (bgmedia_mesh_clip)
// match what the GPU path really draws?
//
// For a neutral pose and a dance pose it
//   1. renders the model with the SDL_GPU renderer (only the selected materials
//      visible) and a second time with everything hidden; the pixel difference
//      is the GPU silhouette;
//   2. skins the same vertices on the CPU with the retargeted pose
//      (model * nodeGlobals[joint] * inverseBind, the formula of the shader's
//      joint palette), projects them with a copy of the renderer's camera and
//      rasterises the triangles into a silhouette;
//   3. reports the silhouette IoU and the one-sided pixel counts.
// If the CPU skinning (matrix order, joint indexing, weights) differed from the
// GPU path, the dance pose would agree much worse than the neutral pose.
// The y-direction of the projection is calibrated on the neutral pose only
// (harness convention); the dance pose uses the same choice.

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/sdlgpu/SdlGpuVrmRenderer.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace bgmedia;
using math::Mat4;
using math::Vec3;

namespace {

struct Silhouette {
    int w = 0, h = 0;
    std::vector<unsigned char> m;
};

bool nameMatches(const std::string& name, const std::vector<std::string>& needles) {
    for (const auto& n : needles)
        if (name.find(n) != std::string::npos) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::string modelPath = "models/Model1.vrm";
    std::string clip = "neonPulse";
    double time = 7.3;
    std::vector<std::string> visible = {"_SKIN", "_CLOTH"};
    float camYaw = 3.14159265f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--model" && i + 1 < argc) modelPath = argv[++i];
        else if (a == "--clip" && i + 1 < argc) clip = argv[++i];
        else if (a == "--time" && i + 1 < argc) time = std::atof(argv[++i]);
        else if (a == "--yaw" && i + 1 < argc) camYaw = 3.14159265f + static_cast<float>(std::atof(argv[++i])) * 3.14159265f / 180.0f;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    vrm::LoadResult loaded = vrm::VrmLoader::load(modelPath);
    if (!loaded.ok()) {
        std::fprintf(stderr, "load failed\n");
        return 1;
    }
    vrm::VrmModel& model = *loaded.model;
    animation::HumanoidRetargeter rig(model);
    sdlgpu::SdlGpuVrmRenderer renderer;
    std::string error;
    if (!renderer.initialize(&error) || !renderer.setModel(model, &error)) {
        std::fprintf(stderr, "renderer: %s\n", error.c_str());
        return 1;
    }
    std::size_t maxJoints = 0;
    for (const auto& s : model.skins) maxJoints = std::max(maxJoints, s.joints.size());
    std::printf("skins %zu, max joints per skin %zu (GPU palette limit 192)\n", model.skins.size(), maxJoints);

    motion::DancePlayer player;
    player.loadLibrary("assets/dances");
    player.setOverlayMode(motion::OverlayMode::Full);
    (void)player.selectClip(clip, 0.0, true);
    const motion::DanceFrame frame = player.update(time);

    struct Case {
        const char* name;
        animation::RetargetedPose pose;
    };
    std::vector<Case> cases;
    cases.push_back({"neutral (arms out)", rig.applyClip(motion::neutralClipPose(), 0.0)});
    cases.push_back({"dance", rig.applyClip(frame.pose, time)});

    const auto savedMaterials = model.materials;
    const auto setVisibility = [&](bool onlySelected, bool all) {
        model.materials = savedMaterials;
        for (auto& m : model.materials) {
            const bool show = all ? true : (onlySelected && nameMatches(m.name, visible));
            if (!show) {
                m.alphaMode = vrm::AlphaMode::Mask;
                m.alphaCutoff = 0.5f;
                m.baseColorFactor.w = 0.0f;
                m.emissiveFactor.w = 0.0f;
            }
        }
    };

    int flipChoice = -1;
    for (const Case& c : cases) {
        sdlgpu::Camera camera;
        camera.yaw = camYaw;
        const sdlgpu::RenderSettings settings;

        setVisibility(true, false);
        if (!renderer.render(c.pose, camera, settings, &error)) return 1;
        const std::vector<std::uint8_t> withModel = renderer.pixels();
        const int W = renderer.width(), H = renderer.height();
        setVisibility(false, false);
        if (!renderer.render(c.pose, camera, settings, &error)) return 1;
        const std::vector<std::uint8_t>& background = renderer.pixels();
        Silhouette gpu;
        gpu.w = W;
        gpu.h = H;
        gpu.m.assign(static_cast<std::size_t>(W) * H, 0);
        for (std::size_t p = 0; p < gpu.m.size(); ++p) {
            int d = 0;
            for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(withModel[p * 4 + k]) - int(background[p * 4 + k])));
            gpu.m[p] = d > 10;
        }

        // --- camera: copy of SdlGpuVrmRenderer::render ---------------------
        const Vec3 boundsCenter = model.bounds.center();
        const Mat4 modelMatrix = Mat4::translation(c.pose.rootOffset + c.pose.rootPivot) * c.pose.modelTransform *
                                 Mat4::translation(-c.pose.rootPivot);
        // The camera ignores root translation (stage fixed, dancer travels).
        const Mat4 cameraMatrix = Mat4::translation(c.pose.rootPivot) * c.pose.modelTransform *
                                  Mat4::translation(-c.pose.rootPivot);
        Vec3 target = cameraMatrix.transformPoint(boundsCenter);
        const float distance = std::max(0.5f, model.height * 2.15f);
        const float pitch = std::clamp(camera.pitch, -1.15f, 1.15f);
        const Vec3 eye = target + Vec3{std::sin(camera.yaw) * std::cos(pitch), std::sin(pitch),
                                       std::cos(camera.yaw) * std::cos(pitch)} * distance;
        const Mat4 view = math::lookAtLH(eye, target, {0.0f, 1.0f, 0.0f});
        Mat4 projection = math::perspectiveFovLH(std::clamp(camera.fov, 0.2f, 1.4f), static_cast<float>(W) / H,
                                                 std::max(0.02f, distance * 0.01f), 200.0f);
        projection.m[0] = -projection.m[0];
        const Mat4 vp = projection * view;

        // --- CPU skinning + rasterisation ------------------------------------
        const auto rasterise = [&](bool flipY) {
            Silhouette cpu;
            cpu.w = W;
            cpu.h = H;
            cpu.m.assign(static_cast<std::size_t>(W) * H, 0);
            for (const vrm::MeshInstance& inst : model.meshInstances) {
                const vrm::Skin* skin = model.skinForNode(inst.node);
                if (!skin) continue;
                std::vector<Mat4> palette(skin->joints.size());
                for (std::size_t j = 0; j < palette.size(); ++j)
                    palette[j] = modelMatrix * c.pose.nodeGlobals[static_cast<std::size_t>(skin->joints[j])] *
                                 skin->inverseBindMatrices[j];
                for (const vrm::Primitive& prim : model.meshes[static_cast<std::size_t>(inst.mesh)].primitives) {
                    if (!prim.skinned || prim.material < 0) continue;
                    if (!nameMatches(savedMaterials[static_cast<std::size_t>(prim.material)].name, visible)) continue;
                    std::vector<float> sx(prim.vertices.size()), sy(prim.vertices.size()), sw(prim.vertices.size());
                    for (std::size_t v = 0; v < prim.vertices.size(); ++v) {
                        const vrm::Vertex& vx = prim.vertices[v];
                        Vec3 p{};
                        for (int k = 0; k < 4; ++k)
                            if (vx.weights[static_cast<std::size_t>(k)] > 0.0f)
                                p = p + palette[vx.joints[static_cast<std::size_t>(k)]].transformPoint(vx.position) *
                                            vx.weights[static_cast<std::size_t>(k)];
                        const float* m = vp.m.data();
                        const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
                        const float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
                        const float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
                        sw[v] = cw;
                        sx[v] = (cx / cw * 0.5f + 0.5f) * W;
                        const float ny = cy / cw * 0.5f + 0.5f;
                        sy[v] = (flipY ? ny : 1.0f - ny) * H;
                    }
                    for (std::size_t t = 0; t + 2 < prim.indices.size(); t += 3) {
                        const auto i0 = prim.indices[t], i1 = prim.indices[t + 1], i2 = prim.indices[t + 2];
                        if (sw[i0] <= 0 || sw[i1] <= 0 || sw[i2] <= 0) continue;
                        const float x0 = sx[i0], y0 = sy[i0], x1 = sx[i1], y1 = sy[i1], x2 = sx[i2], y2 = sy[i2];
                        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                        if (std::abs(area) < 1e-6f) continue;
                        const int minX = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
                        const int maxX = std::min(W - 1, (int)std::ceil(std::max({x0, x1, x2})));
                        const int minY = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
                        const int maxY = std::min(H - 1, (int)std::ceil(std::max({y0, y1, y2})));
                        for (int y = minY; y <= maxY; ++y)
                            for (int x = minX; x <= maxX; ++x) {
                                const float px = x + 0.5f, py = y + 0.5f;
                                const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
                                const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
                                const float w2 = 1.0f - w0 - w1;
                                if (w0 >= 0 && w1 >= 0 && w2 >= 0) cpu.m[static_cast<std::size_t>(y) * W + x] = 1;
                            }
                    }
                }
            }
            return cpu;
        };
        const auto iou = [&](const Silhouette& cpu, long& gpuOnly, long& cpuOnly) {
            long both = 0, either = 0;
            gpuOnly = cpuOnly = 0;
            for (std::size_t p = 0; p < gpu.m.size(); ++p) {
                both += gpu.m[p] && cpu.m[p];
                either += gpu.m[p] || cpu.m[p];
                gpuOnly += gpu.m[p] && !cpu.m[p];
                cpuOnly += !gpu.m[p] && cpu.m[p];
            }
            return either ? double(both) / double(either) : 0.0;
        };
        long go = 0, co = 0;
        double best = -1.0;
        int choice = flipChoice;
        Silhouette bestCpu;
        for (int flip = 0; flip < 2; ++flip) {
            if (flipChoice >= 0 && flip != flipChoice) continue;
            Silhouette cpu = rasterise(flip != 0);
            long a = 0, b = 0;
            const double v = iou(cpu, a, b);
            if (v > best) {
                best = v;
                choice = flip;
                bestCpu = std::move(cpu);
                go = a;
                co = b;
            }
        }
        if (flipChoice < 0) flipChoice = choice;
        long gpuPixels = 0;
        for (auto v : gpu.m) gpuPixels += v;
        std::printf("%-20s GPU silhouette %ld px | IoU %.4f | GPU-only %ld px (%.2f%%) | CPU-only %ld px (%.2f%%) | flipY=%d\n",
                    c.name, gpuPixels, best, go, 100.0 * go / std::max(1L, gpuPixels), co,
                    100.0 * co / std::max(1L, gpuPixels), choice);
    }
    renderer.shutdown();
    SDL_Quit();
    return 0;
}
