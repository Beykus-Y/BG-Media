#include "bgmedia/sdlgpu/SdlGpuVrmRenderer.hpp"

#include "shaders/generated/background_frag.h"
#include "shaders/generated/background_vert.h"
#include "shaders/generated/floor_frag.h"
#include "shaders/generated/floor_vert.h"
#include "shaders/generated/mesh_frag.h"
#include "shaders/generated/mesh_vert.h"

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace bgmedia::sdlgpu {
namespace {

using Clock = std::chrono::steady_clock;
using math::Mat4;
using math::Vec3;

constexpr int kWidth = 960;
constexpr int kHeight = 640;
constexpr std::size_t kMaxJointMatrices = 192;
constexpr SDL_GPUTextureFormat kColorFormat =
    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

struct GpuVertex {
    float position[3];
    float normal[3];
    float uv[2];
    std::uint32_t joints[4];
    float weights[4];
};
static_assert(sizeof(GpuVertex) == 64, "vertex layout must match the shader");

struct alignas(16) MeshVsUniforms {
    float viewProj[16];
    float joints[16 * kMaxJointMatrices];
};
static_assert(sizeof(MeshVsUniforms) == 16 * 4 + kMaxJointMatrices * 16 * 4,
              "mesh vertex uniform layout");

struct alignas(16) MeshFsUniforms {
    float baseColor[4];
    float shadeColor[4];
    float params[4];
    float lightDirection[4];
    float cameraPosition[4];
    float rimColor[4];
};
static_assert(sizeof(MeshFsUniforms) == 96, "mesh fragment uniform layout");

struct alignas(16) FloorVsUniforms {
    float viewProj[16];
    float floorParams[4];
};
static_assert(sizeof(FloorVsUniforms) == 80, "floor vertex uniform layout");

struct alignas(16) FloorFsUniforms {
    float cameraPosition[4];
    float shadow[4];
};
static_assert(sizeof(FloorFsUniforms) == 32, "floor fragment uniform layout");

struct DrawCommand {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::int32_t vertexOffset = 0;
    int material = -1;
    int skin = -1;
    int node = -1;
    int primitive = -1;
    Vec3 centroid{};
    bool blend = false;
    bool doubleSided = false;
};

void copyMatrix(float* destination, const Mat4& matrix) {
    std::memcpy(destination, matrix.m.data(), sizeof(matrix.m));
}

void setFragmentUniforms(MeshFsUniforms& out, const vrm::Material& material,
                         const Vec3& camera, const RenderSettings& settings,
                         int textureIndex) {
    (void)textureIndex;
    out.baseColor[0] = material.baseColorFactor.x;
    out.baseColor[1] = material.baseColorFactor.y;
    out.baseColor[2] = material.baseColorFactor.z;
    out.baseColor[3] = material.baseColorFactor.w;
    out.shadeColor[0] = material.shadeColor.x;
    out.shadeColor[1] = material.shadeColor.y;
    out.shadeColor[2] = material.shadeColor.z;
    out.shadeColor[3] = 1.0f;
    out.params[0] = material.alphaCutoff;
    out.params[1] = material.alphaMode == vrm::AlphaMode::Mask
                        ? 1.0f
                        : (material.alphaMode == vrm::AlphaMode::Blend ? 2.0f
                                                                       : 0.0f);
    out.params[2] = material.toon ? 1.0f : 0.0f;
    out.params[3] = 0.0f;
    const Vec3 light = math::normalize(
        Vec3{settings.lightDirection.x, settings.lightDirection.y,
             settings.lightDirection.z});
    out.lightDirection[0] = light.x;
    out.lightDirection[1] = light.y;
    out.lightDirection[2] = light.z;
    out.lightDirection[3] = settings.lightDirection.w;
    out.cameraPosition[0] = camera.x;
    out.cameraPosition[1] = camera.y;
    out.cameraPosition[2] = camera.z;
    out.cameraPosition[3] = 1.0f;
    out.rimColor[0] = settings.rimColor.x;
    out.rimColor[1] = settings.rimColor.y;
    out.rimColor[2] = settings.rimColor.z;
    out.rimColor[3] = 0.0f;
}

std::uint32_t mipLevelsFor(std::uint32_t width, std::uint32_t height) {
    std::uint32_t levels = 1;
    while (width > 1 || height > 1) {
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        ++levels;
    }
    return levels;
}

} // namespace

struct SdlGpuVrmRenderer::Impl {
    SDL_GPUDevice* device_ = nullptr;
    std::string driver_;
    std::string deviceName;

    SDL_GPUShader* meshVertexShader = nullptr;
    SDL_GPUShader* meshFragmentShader = nullptr;
    SDL_GPUShader* backgroundVertexShader = nullptr;
    SDL_GPUShader* backgroundFragmentShader = nullptr;
    SDL_GPUShader* floorVertexShader = nullptr;
    SDL_GPUShader* floorFragmentShader = nullptr;

    SDL_GPUGraphicsPipeline* meshOpaqueBack = nullptr;
    SDL_GPUGraphicsPipeline* meshOpaqueDouble = nullptr;
    SDL_GPUGraphicsPipeline* meshBlendBack = nullptr;
    SDL_GPUGraphicsPipeline* meshBlendDouble = nullptr;
    SDL_GPUGraphicsPipeline* backgroundPipeline = nullptr;
    SDL_GPUGraphicsPipeline* floorPipeline = nullptr;

    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUTexture* colorTarget = nullptr;
    SDL_GPUTexture* depthTarget = nullptr;
    SDL_GPUTexture* fallbackTexture = nullptr;

    SDL_GPUBuffer* vertexBuffer = nullptr;
    SDL_GPUBuffer* indexBuffer = nullptr;
    std::uint32_t vertexBufferBytes = 0;
    std::uint32_t indexBufferBytes = 0;

    std::vector<SDL_GPUTexture*> textures;
    std::vector<DrawCommand> draws;
    std::vector<int> opaqueOrder;
    std::vector<int> blendOrder;

    const vrm::VrmModel* model = nullptr;
    bool ready = false;
    bool modelReady = false;
    int width = kWidth;
    int height = kHeight;
    std::vector<std::uint8_t> pixels =
        std::vector<std::uint8_t>(static_cast<std::size_t>(kWidth) * kHeight * 4);
    FrameStats stats;

    SDL_GPUTransferBuffer* downloadBuffer = nullptr;
    std::uint32_t downloadBytes = 0;

    [[nodiscard]] bool initialize(std::string* error);
    [[nodiscard]] bool resizeTargets(int newWidth, int newHeight,
                                     std::string* error);
    void releaseTargets();
    void shutdown();
    void releaseModelResources();
    [[nodiscard]] bool createPipelines(std::string* error);
    [[nodiscard]] bool uploadMesh(std::string* error);
    [[nodiscard]] bool uploadTextures(std::string* error);
    [[nodiscard]] bool uploadFallbackTexture(std::string* error);
    [[nodiscard]] SDL_GPUTexture* uploadTexture(const vrm::Texture& texture,
                                                std::string* error);
    [[nodiscard]] bool render(const animation::RetargetedPose& pose,
                              const Camera& camera,
                              const RenderSettings& settings,
                              std::string* error);
};

bool SdlGpuVrmRenderer::Impl::initialize(std::string* error) {
    if (device_) {
        return true;
    }
    const SDL_GPUShaderFormat requested = SDL_GPU_SHADERFORMAT_DXIL;
    device_ = SDL_CreateGPUDevice(requested, false, nullptr);
    if (!device_) {
        if (error) {
            *error = std::string("SDL_GPU device creation failed: ") +
                     SDL_GetError() +
                     " (packaged shaders are DXIL; D3D12 is required in this "
                     "build)";
        }
        return false;
    }
    const char* driverName = SDL_GetGPUDeviceDriver(device_);
    driver_ = driverName ? driverName : "unknown";
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
    if ((formats & SDL_GPU_SHADERFORMAT_DXIL) == 0) {
        if (error) {
            *error = "SDL_GPU backend does not accept the packaged DXIL shaders";
        }
        shutdown();
        return false;
    }
    deviceName = driver_;

    const auto createShader = [&](const unsigned char* code, std::size_t size,
                                  SDL_GPUShaderStage stage, Uint32 samplers,
                                  Uint32 uniforms) -> SDL_GPUShader* {
        SDL_GPUShaderCreateInfo info{};
        info.code_size = size;
        info.code = code;
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        return SDL_CreateGPUShader(device_, &info);
    };
    meshVertexShader =
        createShader(mesh_vert_dxil, sizeof(mesh_vert_dxil),
                     SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    meshFragmentShader =
        createShader(mesh_frag_dxil, sizeof(mesh_frag_dxil),
                     SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    backgroundVertexShader =
        createShader(background_vert_dxil, sizeof(background_vert_dxil),
                     SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    backgroundFragmentShader =
        createShader(background_frag_dxil, sizeof(background_frag_dxil),
                     SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    floorVertexShader =
        createShader(floor_vert_dxil, sizeof(floor_vert_dxil),
                     SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    floorFragmentShader =
        createShader(floor_frag_dxil, sizeof(floor_frag_dxil),
                     SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!meshVertexShader || !meshFragmentShader || !backgroundVertexShader ||
        !backgroundFragmentShader || !floorVertexShader || !floorFragmentShader) {
        if (error) {
            *error = "SDL_GPU shader creation failed: " + std::string(SDL_GetError());
        }
        shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.max_anisotropy = 1.0f;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 32.0f;
    sampler = SDL_CreateGPUSampler(device_, &samplerInfo);
    if (!sampler) {
        if (error) {
            *error = "SDL_GPU sampler creation failed: " + std::string(SDL_GetError());
        }
        shutdown();
        return false;
    }

    SDL_GPUTextureCreateInfo colorInfo{};
    colorInfo.type = SDL_GPU_TEXTURETYPE_2D;
    colorInfo.format = kColorFormat;
    colorInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    colorInfo.width = static_cast<Uint32>(width);
    colorInfo.height = static_cast<Uint32>(height);
    colorInfo.layer_count_or_depth = 1;
    colorInfo.num_levels = 1;
    colorInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    colorTarget = SDL_CreateGPUTexture(device_, &colorInfo);
    if (!colorTarget) {
        if (error) {
            *error = "SDL_GPU color target creation failed: " +
                     std::string(SDL_GetError());
        }
        shutdown();
        return false;
    }

    SDL_GPUTextureCreateInfo depthInfo{};
    depthInfo.type = SDL_GPU_TEXTURETYPE_2D;
    depthInfo.format = kDepthFormat;
    depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depthInfo.width = static_cast<Uint32>(width);
    depthInfo.height = static_cast<Uint32>(height);
    depthInfo.layer_count_or_depth = 1;
    depthInfo.num_levels = 1;
    depthInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    depthTarget = SDL_CreateGPUTexture(device_, &depthInfo);
    if (!depthTarget) {
        if (error) {
            *error = "SDL_GPU depth target creation failed: " +
                     std::string(SDL_GetError());
        }
        shutdown();
        return false;
    }

    downloadBytes = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height) * 4;
    SDL_GPUTransferBufferCreateInfo downloadInfo{};
    downloadInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    downloadInfo.size = downloadBytes;
    downloadBuffer = SDL_CreateGPUTransferBuffer(device_, &downloadInfo);
    if (!downloadBuffer) {
        if (error) {
            *error = "SDL_GPU readback buffer creation failed: " +
                     std::string(SDL_GetError());
        }
        shutdown();
        return false;
    }

    if (!createPipelines(error)) {
        shutdown();
        return false;
    }
    pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);
    ready = true;
    return true;
}

bool SdlGpuVrmRenderer::Impl::createPipelines(std::string* error) {
    const auto makeMeshPipeline = [&](bool blend, bool doubleSided,
                                      bool depthWrite) -> SDL_GPUGraphicsPipeline* {
        SDL_GPUVertexBufferDescription bufferDescription{};
        bufferDescription.slot = 0;
        bufferDescription.pitch = sizeof(GpuVertex);
        bufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        bufferDescription.instance_step_rate = 0;

        SDL_GPUVertexAttribute attributes[5]{};
        attributes[0].location = 0;
        attributes[0].buffer_slot = 0;
        attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[0].offset = offsetof(GpuVertex, position);
        attributes[1].location = 1;
        attributes[1].buffer_slot = 0;
        attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[1].offset = offsetof(GpuVertex, normal);
        attributes[2].location = 2;
        attributes[2].buffer_slot = 0;
        attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attributes[2].offset = offsetof(GpuVertex, uv);
        attributes[3].location = 3;
        attributes[3].buffer_slot = 0;
        attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_UINT4;
        attributes[3].offset = offsetof(GpuVertex, joints);
        attributes[4].location = 4;
        attributes[4].buffer_slot = 0;
        attributes[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[4].offset = offsetof(GpuVertex, weights);

        SDL_GPUVertexInputState vertexInput{};
        vertexInput.vertex_buffer_descriptions = &bufferDescription;
        vertexInput.num_vertex_buffers = 1;
        vertexInput.vertex_attributes = attributes;
        vertexInput.num_vertex_attributes = 5;

        SDL_GPUColorTargetDescription colorTargetDescription{};
        colorTargetDescription.format = kColorFormat;
        colorTargetDescription.blend_state.src_color_blendfactor =
            SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorTargetDescription.blend_state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTargetDescription.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colorTargetDescription.blend_state.src_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE;
        colorTargetDescription.blend_state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTargetDescription.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        colorTargetDescription.blend_state.color_write_mask =
            SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
            SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
        colorTargetDescription.blend_state.enable_color_write_mask = false;
        colorTargetDescription.blend_state.enable_blend = blend;

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = meshVertexShader;
        info.fragment_shader = meshFragmentShader;
        info.vertex_input_state = vertexInput;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.rasterizer_state.enable_depth_clip = true;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        // Blended layers (eyes, brows, lashes, highlights) must still be depth *tested*
        // against the opaque pass, otherwise they are drawn over everything, including
        // through the back of the head and the hair. They do not write depth (the
        // back-to-front sort handles their mutual order). A compare op of ALWAYS here
        // made the "depth test" a no-op.
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = depthWrite;
        info.target_info.color_target_descriptions = &colorTargetDescription;
        info.target_info.num_color_targets = 1;
        info.target_info.depth_stencil_format = kDepthFormat;
        info.target_info.has_depth_stencil_target = true;
        return SDL_CreateGPUGraphicsPipeline(device_, &info);
    };

    meshOpaqueBack = makeMeshPipeline(false, false, true);
    meshOpaqueDouble = makeMeshPipeline(false, true, true);
    meshBlendBack = makeMeshPipeline(true, false, false);
    meshBlendDouble = makeMeshPipeline(true, true, false);
    if (!meshOpaqueBack || !meshOpaqueDouble || !meshBlendBack ||
        !meshBlendDouble) {
        if (error) {
            *error = "SDL_GPU mesh pipeline creation failed: " +
                     std::string(SDL_GetError());
        }
        return false;
    }

    const auto makeSimplePipeline = [&](SDL_GPUShader* vertexShader,
                                        SDL_GPUShader* fragmentShader,
                                        bool depthTest,
                                        bool depthWrite)
        -> SDL_GPUGraphicsPipeline* {
        SDL_GPUColorTargetDescription color{};
        color.format = kColorFormat;
        color.blend_state.enable_blend = false;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertexShader;
        info.fragment_shader = fragmentShader;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.rasterizer_state.enable_depth_clip = true;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.depth_stencil_state.enable_depth_test = depthTest;
        info.depth_stencil_state.enable_depth_write = depthWrite;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.depth_stencil_format = kDepthFormat;
        info.target_info.has_depth_stencil_target = true;
        return SDL_CreateGPUGraphicsPipeline(device_, &info);
    };

    backgroundPipeline = makeSimplePipeline(backgroundVertexShader,
                                            backgroundFragmentShader, false, false);
    floorPipeline =
        makeSimplePipeline(floorVertexShader, floorFragmentShader, true, true);
    if (!backgroundPipeline || !floorPipeline) {
        if (error) {
            *error = "SDL_GPU scene pipeline creation failed: " +
                     std::string(SDL_GetError());
        }
        return false;
    }
    return true;
}

bool SdlGpuVrmRenderer::Impl::uploadMesh(std::string* error) {
    if (!model) {
        return false;
    }
    std::vector<GpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    draws.clear();
    opaqueOrder.clear();
    blendOrder.clear();

    for (const vrm::MeshInstance& instance : model->meshInstances) {
        if (instance.mesh < 0 ||
            static_cast<std::size_t>(instance.mesh) >= model->meshes.size() ||
            instance.node < 0 ||
            static_cast<std::size_t>(instance.node) >= model->nodes.size()) {
            continue;
        }
        const vrm::Mesh& mesh = model->meshes[static_cast<std::size_t>(instance.mesh)];
        const int skinIndex = model->nodes[static_cast<std::size_t>(instance.node)].skin;
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const vrm::Primitive& primitive = mesh.primitives[p];
            if (primitive.indices.empty() || primitive.vertices.empty()) {
                continue;
            }
            if (vertices.size() + primitive.vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                if (error) {
                    *error = "vertex buffer exceeds the 32-bit draw limit";
                }
                return false;
            }
            const std::uint32_t firstIndex =
                static_cast<std::uint32_t>(indices.size());
            const std::int32_t vertexOffset =
                static_cast<std::int32_t>(vertices.size());
            Vec3 centroid{};
            for (const vrm::Vertex& source : primitive.vertices) {
                GpuVertex vertex{};
                vertex.position[0] = source.position.x;
                vertex.position[1] = source.position.y;
                vertex.position[2] = source.position.z;
                vertex.normal[0] = source.normal.x;
                vertex.normal[1] = source.normal.y;
                vertex.normal[2] = source.normal.z;
                vertex.uv[0] = source.uv.x;
                vertex.uv[1] = source.uv.y;
                for (int k = 0; k < 4; ++k) {
                    vertex.joints[k] = source.joints[static_cast<std::size_t>(k)];
                    vertex.weights[k] = source.weights[static_cast<std::size_t>(k)];
                }
                vertices.push_back(vertex);
                centroid += source.position;
            }
            centroid = centroid / static_cast<float>(primitive.vertices.size());
            for (std::uint32_t index : primitive.indices) {
                if (index >= primitive.vertices.size()) {
                    if (error) {
                        *error = "primitive index exceeds its vertex range";
                    }
                    return false;
                }
                indices.push_back(static_cast<std::uint32_t>(vertexOffset) + index);
            }
            DrawCommand draw;
            draw.firstIndex = firstIndex;
            draw.indexCount = static_cast<std::uint32_t>(primitive.indices.size());
            draw.vertexOffset = 0; // indices already include the base vertex
            draw.material = primitive.material;
            draw.skin = primitive.skinned ? skinIndex : -1;
            draw.node = instance.node;
            draw.primitive = static_cast<int>(p);
            draw.centroid = centroid;
            if (draw.material >= 0 &&
                static_cast<std::size_t>(draw.material) < model->materials.size()) {
                const vrm::Material& material =
                    model->materials[static_cast<std::size_t>(draw.material)];
                draw.blend = material.alphaMode == vrm::AlphaMode::Blend;
                draw.doubleSided = material.doubleSided;
            }
            draws.push_back(draw);
            if (draw.blend) {
                blendOrder.push_back(static_cast<int>(draws.size() - 1));
            } else {
                opaqueOrder.push_back(static_cast<int>(draws.size() - 1));
            }
        }
    }
    if (draws.empty() || vertices.empty() || indices.empty()) {
        if (error) {
            *error = "VRM contains no uploadable triangles";
        }
        return false;
    }

    vertexBufferBytes = static_cast<std::uint32_t>(
        vertices.size() * sizeof(GpuVertex));
    indexBufferBytes =
        static_cast<std::uint32_t>(indices.size() * sizeof(std::uint32_t));

    SDL_GPUBufferCreateInfo vertexInfo{};
    vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexInfo.size = vertexBufferBytes;
    vertexBuffer = SDL_CreateGPUBuffer(device_, &vertexInfo);
    SDL_GPUBufferCreateInfo indexInfo{};
    indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexInfo.size = indexBufferBytes;
    indexBuffer = SDL_CreateGPUBuffer(device_, &indexInfo);
    if (!vertexBuffer || !indexBuffer) {
        if (error) {
            *error = "SDL_GPU mesh buffer creation failed: " +
                     std::string(SDL_GetError());
        }
        return false;
    }

    const std::uint32_t vertexBytes = vertexBufferBytes;
    const std::uint32_t indexBytes = indexBufferBytes;
    const std::uint32_t transferSize =
        ((vertexBytes + 3u) & ~3u) + ((indexBytes + 3u) & ~3u);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = transferSize;
    SDL_GPUTransferBuffer* transfer =
        SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!transfer) {
        if (error) {
            *error = "SDL_GPU mesh transfer buffer creation failed: " +
                     std::string(SDL_GetError());
        }
        return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) {
        if (error) {
            *error = "SDL_GPU mesh transfer buffer map failed: " +
                     std::string(SDL_GetError());
        }
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return false;
    }
    std::memcpy(mapped, vertices.data(), vertexBytes);
    std::memcpy(static_cast<std::uint8_t*>(mapped) + vertexBytes, indices.data(),
                indexBytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (!command) {
        if (error) {
            *error = "SDL_GPU command buffer acquisition failed: " +
                     std::string(SDL_GetError());
        }
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation vertexSource{transfer, 0};
    SDL_GPUBufferRegion vertexDestination{vertexBuffer, 0, vertexBytes};
    SDL_UploadToGPUBuffer(copy, &vertexSource, &vertexDestination, false);
    SDL_GPUTransferBufferLocation indexSource{transfer, vertexBytes};
    SDL_GPUBufferRegion indexDestination{indexBuffer, 0, indexBytes};
    SDL_UploadToGPUBuffer(copy, &indexSource, &indexDestination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (!fence) {
        if (error) {
            *error = "SDL_GPU mesh upload submission failed: " +
                     std::string(SDL_GetError());
        }
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    stats.triangles = 0;
    for (const DrawCommand& draw : draws) {
        stats.triangles += static_cast<int>(draw.indexCount / 3);
    }
    stats.drawCalls = static_cast<int>(draws.size());
    return true;
}

SDL_GPUTexture* SdlGpuVrmRenderer::Impl::uploadTexture(
    const vrm::Texture& texture, std::string* error) {
    if (!texture.decoded || texture.rgba.empty() || texture.width <= 0 ||
        texture.height <= 0) {
        return nullptr;
    }
    const std::uint32_t texWidth = static_cast<std::uint32_t>(texture.width);
    const std::uint32_t texHeight = static_cast<std::uint32_t>(texture.height);
    const std::uint32_t byteCount = texWidth * texHeight * 4;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kColorFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                 SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = texWidth;
    info.height = texHeight;
    info.layer_count_or_depth = 1;
    info.num_levels = mipLevelsFor(texWidth, texHeight);
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* gpuTexture = SDL_CreateGPUTexture(device_, &info);
    if (!gpuTexture) {
        if (error) {
            *error = "SDL_GPU texture creation failed: " +
                     std::string(SDL_GetError());
        }
        return nullptr;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = byteCount;
    SDL_GPUTransferBuffer* transfer =
        SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!transfer) {
        SDL_ReleaseGPUTexture(device_, gpuTexture);
        return nullptr;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, gpuTexture);
        return nullptr;
    }
    std::memcpy(mapped, texture.rgba.data(), byteCount);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (!command) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, gpuTexture);
        return nullptr;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source{transfer, 0, texWidth, texHeight};
    SDL_GPUTextureRegion destination{gpuTexture, 0, 0, 0, 0, 0, texWidth, texHeight, 1};
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    if (info.num_levels > 1) {
        SDL_GenerateMipmapsForGPUTexture(command, gpuTexture);
    }
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    stats.textureBytes += static_cast<std::uint64_t>(byteCount) * 4ull / 3ull;
    return gpuTexture;
}

bool SdlGpuVrmRenderer::Impl::uploadFallbackTexture(std::string* error) {
    (void)error;
    constexpr int size = 8;
    std::vector<std::uint8_t> fallbackPixels(static_cast<std::size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool light = ((x / 2) + (y / 2)) % 2 == 0;
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            fallbackPixels[at + 0] = light ? 220 : 72;
            fallbackPixels[at + 1] = light ? 92 : 56;
            fallbackPixels[at + 2] = light ? 168 : 140;
            fallbackPixels[at + 3] = 255;
        }
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kColorFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = size;
    info.height = size;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    fallbackTexture = SDL_CreateGPUTexture(device_, &info);
    if (!fallbackTexture) {
        return false;
    }
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(fallbackPixels.size());
    SDL_GPUTransferBuffer* transfer =
        SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!transfer) {
        return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return false;
    }
    std::memcpy(mapped, fallbackPixels.data(), fallbackPixels.size());
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (!command) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source{transfer, 0, size, size};
    SDL_GPUTextureRegion destination{fallbackTexture, 0, 0, 0, 0, 0, size, size, 1};
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return true;
}

bool SdlGpuVrmRenderer::Impl::uploadTextures(std::string* error) {
    textures.assign(model ? model->textures.size() : 0, nullptr);
    if (!model) {
        return false;
    }
    if (!uploadFallbackTexture(error)) {
        if (error) {
            *error = "fallback texture upload failed: " + std::string(SDL_GetError());
        }
        return false;
    }
    stats.textureCount = 0;
    stats.textureBytes = 0;
    for (std::size_t i = 0; i < model->textures.size(); ++i) {
        const vrm::Texture& texture = model->textures[i];
        if (!texture.decoded) {
            continue;
        }
        textures[i] = uploadTexture(texture, error);
        if (!textures[i]) {
            if (error) {
                *error = "failed to upload texture '" + texture.name + "': " +
                         SDL_GetError();
            }
            return false;
        }
        ++stats.textureCount;
    }
    return true;
}

void SdlGpuVrmRenderer::Impl::releaseModelResources() {
    if (device_) {
        SDL_WaitForGPUIdle(device_);
    }
    if (device_) {
        for (SDL_GPUTexture*& texture : textures) {
            if (texture) {
                SDL_ReleaseGPUTexture(device_, texture);
                texture = nullptr;
            }
        }
        if (fallbackTexture) {
            SDL_ReleaseGPUTexture(device_, fallbackTexture);
            fallbackTexture = nullptr;
        }
        if (vertexBuffer) {
            SDL_ReleaseGPUBuffer(device_, vertexBuffer);
            vertexBuffer = nullptr;
        }
        if (indexBuffer) {
            SDL_ReleaseGPUBuffer(device_, indexBuffer);
            indexBuffer = nullptr;
        }
    }
    textures.clear();
    draws.clear();
    opaqueOrder.clear();
    blendOrder.clear();
    vertexBufferBytes = 0;
    indexBufferBytes = 0;
    model = nullptr;
    modelReady = false;
    stats = {};
}

bool SdlGpuVrmRenderer::Impl::resizeTargets(int newWidth, int newHeight,
                                            std::string* error) {
    newWidth = std::clamp(newWidth, 64, 3840);
    newHeight = std::clamp(newHeight, 64, 2160);
    if (!device_ || !ready) {
        // Size is stored and applied during initialize().
        width = newWidth;
        height = newHeight;
        return true;
    }
    if (newWidth == width && newHeight == height) {
        return true;
    }
    SDL_WaitForGPUIdle(device_);
    releaseTargets();

    SDL_GPUTextureCreateInfo colorInfo{};
    colorInfo.type = SDL_GPU_TEXTURETYPE_2D;
    colorInfo.format = kColorFormat;
    colorInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    colorInfo.width = static_cast<Uint32>(newWidth);
    colorInfo.height = static_cast<Uint32>(newHeight);
    colorInfo.layer_count_or_depth = 1;
    colorInfo.num_levels = 1;
    colorInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    colorTarget = SDL_CreateGPUTexture(device_, &colorInfo);

    SDL_GPUTextureCreateInfo depthInfo{};
    depthInfo.type = SDL_GPU_TEXTURETYPE_2D;
    depthInfo.format = kDepthFormat;
    depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depthInfo.width = static_cast<Uint32>(newWidth);
    depthInfo.height = static_cast<Uint32>(newHeight);
    depthInfo.layer_count_or_depth = 1;
    depthInfo.num_levels = 1;
    depthInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    depthTarget = SDL_CreateGPUTexture(device_, &depthInfo);

    downloadBytes = static_cast<std::uint32_t>(newWidth) *
                    static_cast<std::uint32_t>(newHeight) * 4;
    SDL_GPUTransferBufferCreateInfo downloadInfo{};
    downloadInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    downloadInfo.size = downloadBytes;
    downloadBuffer = SDL_CreateGPUTransferBuffer(device_, &downloadInfo);

    if (!colorTarget || !depthTarget || !downloadBuffer) {
        if (error) {
            *error = "3D render target resize failed: " +
                     std::string(SDL_GetError());
        }
        releaseTargets();
        ready = false;
        return false;
    }
    width = newWidth;
    height = newHeight;
    pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);
    return true;
}

void SdlGpuVrmRenderer::Impl::releaseTargets() {
    if (!device_) {
        return;
    }
    if (colorTarget) {
        SDL_ReleaseGPUTexture(device_, colorTarget);
        colorTarget = nullptr;
    }
    if (depthTarget) {
        SDL_ReleaseGPUTexture(device_, depthTarget);
        depthTarget = nullptr;
    }
    if (downloadBuffer) {
        SDL_ReleaseGPUTransferBuffer(device_, downloadBuffer);
        downloadBuffer = nullptr;
    }
    downloadBytes = 0;
}

void SdlGpuVrmRenderer::Impl::shutdown() {
    if (device_) {
        SDL_WaitForGPUIdle(device_);
    }
    releaseModelResources();
    if (device_) {
        if (downloadBuffer) {
            SDL_ReleaseGPUTransferBuffer(device_, downloadBuffer);
            downloadBuffer = nullptr;
        }
        if (meshOpaqueBack) {
            SDL_ReleaseGPUGraphicsPipeline(device_, meshOpaqueBack);
            meshOpaqueBack = nullptr;
        }
        if (meshOpaqueDouble) {
            SDL_ReleaseGPUGraphicsPipeline(device_, meshOpaqueDouble);
            meshOpaqueDouble = nullptr;
        }
        if (meshBlendBack) {
            SDL_ReleaseGPUGraphicsPipeline(device_, meshBlendBack);
            meshBlendBack = nullptr;
        }
        if (meshBlendDouble) {
            SDL_ReleaseGPUGraphicsPipeline(device_, meshBlendDouble);
            meshBlendDouble = nullptr;
        }
        if (backgroundPipeline) {
            SDL_ReleaseGPUGraphicsPipeline(device_, backgroundPipeline);
            backgroundPipeline = nullptr;
        }
        if (floorPipeline) {
            SDL_ReleaseGPUGraphicsPipeline(device_, floorPipeline);
            floorPipeline = nullptr;
        }
        if (sampler) {
            SDL_ReleaseGPUSampler(device_, sampler);
            sampler = nullptr;
        }
        if (meshVertexShader) {
            SDL_ReleaseGPUShader(device_, meshVertexShader);
            meshVertexShader = nullptr;
        }
        if (meshFragmentShader) {
            SDL_ReleaseGPUShader(device_, meshFragmentShader);
            meshFragmentShader = nullptr;
        }
        if (backgroundVertexShader) {
            SDL_ReleaseGPUShader(device_, backgroundVertexShader);
            backgroundVertexShader = nullptr;
        }
        if (backgroundFragmentShader) {
            SDL_ReleaseGPUShader(device_, backgroundFragmentShader);
            backgroundFragmentShader = nullptr;
        }
        if (floorVertexShader) {
            SDL_ReleaseGPUShader(device_, floorVertexShader);
            floorVertexShader = nullptr;
        }
        if (floorFragmentShader) {
            SDL_ReleaseGPUShader(device_, floorFragmentShader);
            floorFragmentShader = nullptr;
        }
        if (colorTarget) {
            SDL_ReleaseGPUTexture(device_, colorTarget);
            colorTarget = nullptr;
        }
        if (depthTarget) {
            SDL_ReleaseGPUTexture(device_, depthTarget);
            depthTarget = nullptr;
        }
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }
    ready = false;
    downloadBuffer = nullptr;
    downloadBytes = 0;
}

bool SdlGpuVrmRenderer::Impl::render(
    const animation::RetargetedPose& pose, const Camera& camera,
    const RenderSettings& settings, std::string* error) {
    if (!device_ || !ready || !model || !modelReady) {
        if (error) {
            *error = "3D renderer is not ready";
        }
        return false;
    }
    if (pose.nodeGlobals.size() != model->nodes.size()) {
        if (error) {
            *error = "retargeted pose is missing node transforms";
        }
        return false;
    }

    const auto started = Clock::now();
    const Vec3 boundsCenter = model->bounds.center();
    const float scale = std::clamp(settings.modelScale, 0.1f, 10.0f);
    const Mat4 scaleMatrix = Mat4::scale({scale, scale, scale});
    // BG Motion 3.0 root translation scales with the user's model scale and
    // never moves the camera: the stage stays fixed while the dancer travels.
    const Vec3 pivotScaled = pose.rootPivot * scale;
    const Mat4 modelMatrix =
        Mat4::translation(settings.modelOffset + pose.rootOffset * scale + pivotScaled) *
        pose.modelTransform * Mat4::translation(-pivotScaled) * scaleMatrix;
    const Mat4 cameraMatrix = Mat4::translation(settings.modelOffset) *
                              Mat4::translation(pivotScaled) * pose.modelTransform *
                              Mat4::translation(-pivotScaled) * scaleMatrix;
    Vec3 target = cameraMatrix.transformPoint(boundsCenter);
    target.y += camera.targetYBias;
    const float modelHeight = std::max(0.1f, model->height * scale);
    const float distance =
        std::max(0.5f, model->height * 2.15f * std::sqrt(scale)) *
        std::clamp(camera.distanceScale, 0.35f, 3.0f);
    const float pitch = std::clamp(camera.pitch, -1.15f, 1.15f);
    const Vec3 orbit =
        Vec3{std::sin(camera.yaw) * std::cos(pitch), std::sin(pitch),
             std::cos(camera.yaw) * std::cos(pitch)} *
        distance;
    const Vec3 eye = target + orbit;
    const Mat4 view = math::lookAtLH(eye, target, {0.0f, 1.0f, 0.0f});
    Mat4 projection =
        math::perspectiveFovLH(std::clamp(camera.fov, 0.2f, 1.4f),
                               static_cast<float>(width) / static_cast<float>(std::max(1, height)),
                               std::max(0.02f, distance * 0.01f), 200.0f);
    // glTF/VRM data is right handed while lookAtLH/perspectiveFovLH build a
    // left handed view. Without this correction the image is a mirror image:
    // a character facing the camera would show its right arm on the viewer's
    // right. Back-face culling is disabled, so flipping X needs nothing else.
    projection.m[0] = -projection.m[0];
    const Mat4 viewProjection = projection * view;

    // Sort blended primitives back-to-front using their bind-space centroid.
    // A depth test remains enabled; the sort prevents hair/eye layers from
    // popping when triangles are close to coplanar.
    std::stable_sort(blendOrder.begin(), blendOrder.end(),
                     [&](int a, int b) {
                         const Vec3 ca = modelMatrix.transformPoint(
                             draws[static_cast<std::size_t>(a)].centroid);
                         const Vec3 cb = modelMatrix.transformPoint(
                             draws[static_cast<std::size_t>(b)].centroid);
                         return (ca - eye).lengthSquared() >
                                (cb - eye).lengthSquared();
                     });

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (!command) {
        if (error) {
            *error = "SDL_GPU command buffer acquisition failed: " +
                     std::string(SDL_GetError());
        }
        return false;
    }
    SDL_GPUColorTargetInfo colorInfo{};
    colorInfo.texture = colorTarget;
    colorInfo.clear_color = SDL_FColor{0.008f, 0.012f, 0.020f, 1.0f};
    colorInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    colorInfo.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    depthInfo.texture = depthTarget;
    depthInfo.clear_depth = 1.0f;
    depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.store_op = SDL_GPU_STOREOP_STORE;
    depthInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depthInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass =
        SDL_BeginGPURenderPass(command, &colorInfo, 1, &depthInfo);
    if (!pass) {
        if (error) {
            *error = "SDL_GPU render pass failed: " + std::string(SDL_GetError());
        }
        SDL_CancelGPUCommandBuffer(command);
        return false;
    }
    const SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<float>(width),
                                   static_cast<float>(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect scissor{0, 0, width, height};
    SDL_SetGPUScissor(pass, &scissor);

    // Background gradient (depth disabled).
    SDL_BindGPUGraphicsPipeline(pass, backgroundPipeline);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

    // Ground and contact shadow.
    FloorVsUniforms floorVs{};
    copyMatrix(floorVs.viewProj, viewProjection);
    floorVs.floorParams[0] = std::max(3.0f, modelHeight * 4.0f);
    floorVs.floorParams[1] = modelHeight;
    floorVs.floorParams[2] = 0.0f;
    floorVs.floorParams[3] = 0.0f;
    FloorFsUniforms floorFs{};
    floorFs.cameraPosition[0] = eye.x;
    floorFs.cameraPosition[1] = eye.y;
    floorFs.cameraPosition[2] = eye.z;
    floorFs.cameraPosition[3] = 1.0f;
    const Vec3 footCenter = modelMatrix.transformPoint({0.0f, model->bounds.min.y, 0.0f});
    floorFs.shadow[0] = footCenter.x;
    floorFs.shadow[1] = footCenter.z;
    floorFs.shadow[2] = modelHeight * 0.55f;
    floorFs.shadow[3] = 0.95f;
    SDL_BindGPUGraphicsPipeline(pass, floorPipeline);
    SDL_PushGPUVertexUniformData(command, 0, &floorVs, sizeof(floorVs));
    SDL_PushGPUFragmentUniformData(command, 0, &floorFs, sizeof(floorFs));
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);

    const SDL_GPUBufferBinding vertexBinding{vertexBuffer, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    const SDL_GPUBufferBinding indexBinding{indexBuffer, 0};
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    const auto drawOne = [&](int drawIndex, bool blend) {
        const DrawCommand& draw = draws[static_cast<std::size_t>(drawIndex)];
        MeshVsUniforms vs{};
        copyMatrix(vs.viewProj, viewProjection);
        const std::size_t matrixCount = kMaxJointMatrices;
        for (std::size_t i = 0; i < matrixCount; ++i) {
            for (int k = 0; k < 16; ++k) {
                vs.joints[i * 16 + static_cast<std::size_t>(k)] =
                    (k % 5 == 0) ? 1.0f : 0.0f;
            }
        }
        const vrm::Skin* skin =
            draw.skin >= 0 && draw.skin < static_cast<int>(model->skins.size())
                ? &model->skins[static_cast<std::size_t>(draw.skin)]
                : nullptr;
        if (skin && !skin->joints.empty()) {
            if (skin->joints.size() > kMaxJointMatrices) {
                return;
            }
            for (std::size_t i = 0; i < skin->joints.size(); ++i) {
                const int joint = skin->joints[i];
                Mat4 jointMatrix = Mat4::identity();
                if (joint >= 0 && static_cast<std::size_t>(joint) <
                                     pose.nodeGlobals.size()) {
                    jointMatrix = pose.nodeGlobals[static_cast<std::size_t>(joint)];
                }
                const Mat4 palette =
                    modelMatrix * jointMatrix * skin->inverseBindMatrices[i];
                copyMatrix(&vs.joints[i * 16], palette);
            }
        } else {
            Mat4 nodeMatrix = Mat4::identity();
            if (draw.node >= 0 &&
                static_cast<std::size_t>(draw.node) < pose.nodeGlobals.size()) {
                nodeMatrix = pose.nodeGlobals[static_cast<std::size_t>(draw.node)];
            }
            const Mat4 palette = modelMatrix * nodeMatrix;
            copyMatrix(vs.joints, palette);
        }

        const vrm::Material* material = nullptr;
        if (draw.material >= 0 &&
            static_cast<std::size_t>(draw.material) < model->materials.size()) {
            material = &model->materials[static_cast<std::size_t>(draw.material)];
        }
        MeshFsUniforms fs{};
        if (material) {
            setFragmentUniforms(fs, *material, eye, settings,
                                material->baseColorTexture);
        } else {
            vrm::Material fallback;
            fallback.baseColorFactor = {0.8f, 0.8f, 0.8f, 1.0f};
            fallback.shadeColor = {0.7f, 0.7f, 0.75f, 1.0f};
            setFragmentUniforms(fs, fallback, eye, settings, -1);
        }

        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        if (blend) {
            pipeline = draw.doubleSided ? meshBlendDouble : meshBlendBack;
        } else {
            pipeline = draw.doubleSided ? meshOpaqueDouble : meshOpaqueBack;
        }
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_PushGPUVertexUniformData(command, 0, &vs, sizeof(vs));
        SDL_PushGPUFragmentUniformData(command, 0, &fs, sizeof(fs));
        SDL_GPUTexture* texture = fallbackTexture;
        if (material && material->baseColorTexture >= 0 &&
            static_cast<std::size_t>(material->baseColorTexture) < textures.size() &&
            textures[static_cast<std::size_t>(material->baseColorTexture)]) {
            texture = textures[static_cast<std::size_t>(material->baseColorTexture)];
        }
        const SDL_GPUTextureSamplerBinding samplerBinding{texture, sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &samplerBinding, 1);
        SDL_DrawGPUIndexedPrimitives(pass, draw.indexCount, 1, draw.firstIndex,
                                     0, 0);
    };

    for (int index : opaqueOrder) {
        drawOne(index, false);
    }
    for (int index : blendOrder) {
        drawOne(index, true);
    }

    SDL_EndGPURenderPass(pass);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureRegion source{colorTarget, 0, 0, 0, 0, 0,
                                static_cast<Uint32>(width),
                                static_cast<Uint32>(height), 1};
    SDL_GPUTextureTransferInfo destination{downloadBuffer, 0,
                                           static_cast<Uint32>(width),
                                           static_cast<Uint32>(height)};
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (!fence) {
        if (error) {
            *error = "SDL_GPU submit failed: " + std::string(SDL_GetError());
        }
        return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
    const auto submitted = Clock::now();
    void* mapped = SDL_MapGPUTransferBuffer(device_, downloadBuffer, false);
    if (!mapped) {
        if (error) {
            *error = "SDL_GPU readback map failed: " + std::string(SDL_GetError());
        }
        return false;
    }
    std::memcpy(pixels.data(), mapped, downloadBytes);
    SDL_UnmapGPUTransferBuffer(device_, downloadBuffer);

    stats.valid = true;
    stats.width = width;
    stats.height = height;
    stats.driver = driver_;
    stats.deviceName = deviceName;
    stats.gpuMilliseconds =
        std::chrono::duration<double, std::milli>(submitted - started).count();
    stats.readbackMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - submitted).count();
    return true;
}

SdlGpuVrmRenderer::SdlGpuVrmRenderer() : impl_(std::make_unique<Impl>()) {}
SdlGpuVrmRenderer::~SdlGpuVrmRenderer() {
    if (impl_) {
        impl_->shutdown();
    }
}

bool SdlGpuVrmRenderer::initialize(std::string* error) {
    return impl_->initialize(error);
}

void SdlGpuVrmRenderer::shutdown() { impl_->shutdown(); }

bool SdlGpuVrmRenderer::setModel(const vrm::VrmModel& model, std::string* error) {
    if (!impl_->ready) {
        if (error) {
            *error = "renderer is not initialized";
        }
        return false;
    }
    impl_->releaseModelResources();
    impl_->model = &model;
    if (!impl_->uploadMesh(error)) {
        impl_->releaseModelResources();
        return false;
    }
    if (!impl_->uploadTextures(error)) {
        impl_->releaseModelResources();
        return false;
    }
    impl_->stats.skinCount = static_cast<int>(model.skins.size());
    impl_->modelReady = true;
    return true;
}

void SdlGpuVrmRenderer::releaseModel() { impl_->releaseModelResources(); }

bool SdlGpuVrmRenderer::render(const animation::RetargetedPose& pose,
                               const Camera& camera,
                               const RenderSettings& settings,
                               std::string* error) {
    return impl_->render(pose, camera, settings, error);
}

bool SdlGpuVrmRenderer::ready() const { return impl_->ready; }
bool SdlGpuVrmRenderer::hasModel() const { return impl_->modelReady; }

const std::vector<std::uint8_t>& SdlGpuVrmRenderer::pixels() const {
    return impl_->pixels;
}

int SdlGpuVrmRenderer::width() const { return impl_->width; }
int SdlGpuVrmRenderer::height() const { return impl_->height; }

void SdlGpuVrmRenderer::setRenderSize(int width, int height) {
    (void)impl_->resizeTargets(width, height, nullptr);
}const FrameStats& SdlGpuVrmRenderer::stats() const { return impl_->stats; }

} // namespace bgmedia::sdlgpu



