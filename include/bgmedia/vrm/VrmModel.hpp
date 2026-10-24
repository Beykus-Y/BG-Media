#pragma once

// VRM 0.x (glTF 2.0 + extensions.VRM) import data model.
//
// This header is deliberately graphics-API agnostic. It contains no SDL types
// and no renderer objects, so the importer can be tested headlessly and reused
// by future Metal/Vulkan/Android renderers. The desktop SDL_GPU adapter is the
// only place that turns these structures into GPU resources.

#include "bgmedia/math/Math.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgmedia::vrm {

using math::Mat4;
using math::Quat;
using math::Vec2;
using math::Vec3;
using math::Vec4;

// Humanoid bones used by the generic retargeter. VRM 0.x defines more bones
// (fingers, eyes, toes); the loader validates and reports all of them, while
// this list is the stable core contract consumed by animation.
enum class HumanoidBone : std::uint8_t {
    Hips,
    Spine,
    Chest,
    UpperChest,
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToes,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToes,
    LeftEye,
    RightEye,
    Count
};

inline constexpr std::size_t kHumanoidBoneCount =
    static_cast<std::size_t>(HumanoidBone::Count);

[[nodiscard]] std::string_view humanoidBoneName(HumanoidBone bone);
[[nodiscard]] std::optional<HumanoidBone> humanoidBoneFromVrmName(
    std::string_view name);

struct Bounds {
    Vec3 min{};
    Vec3 max{};
    bool valid = false;

    void include(Vec3 point) {
        if (!math::finite(point)) {
            return;
        }
        if (!valid) {
            min = point;
            max = point;
            valid = true;
            return;
        }
        min.x = std::min(min.x, point.x);
        min.y = std::min(min.y, point.y);
        min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x);
        max.y = std::max(max.y, point.y);
        max.z = std::max(max.z, point.z);
    }
    [[nodiscard]] Vec3 center() const {
        return valid ? (min + max) * 0.5f : Vec3{};
    }
    [[nodiscard]] Vec3 size() const { return valid ? max - min : Vec3{}; }
};

struct Vertex {
    Vec3 position{};
    Vec3 normal{};
    Vec2 uv{};
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{0.0f, 0.0f, 0.0f, 0.0f};
};

struct Primitive {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    int material = -1;
    // True when the source accessors carried skin data. The skin instance is
    // resolved through the node that references this mesh.
    bool skinned = false;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
};

// A glTF mesh referenced by a node. Keeping the instance separate from the
// mesh allows one mesh to be reused with a different skin/node transform.
struct MeshInstance {
    int node = -1;
    int mesh = -1;
};

enum class AlphaMode : std::uint8_t {
    Opaque,
    Mask,
    Blend
};

struct Material {
    std::string name;
    std::string shader;
    Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    Vec4 shadeColor{1.0f, 1.0f, 1.0f, 1.0f};
    Vec4 emissiveFactor{0.0f, 0.0f, 0.0f, 1.0f};
    int baseColorTexture = -1;
    int emissiveTexture = -1;
    Vec2 baseColorUvOffset{0.0f, 0.0f};
    Vec2 baseColorUvScale{1.0f, 1.0f};
    float baseColorUvRotation = 0.0f;
    float alphaCutoff = 0.5f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false;
    bool unlit = false;
    bool toon = false;
};

struct Texture {
    std::string name;
    int imageIndex = -1;
    int sourceTexture = -1;
    int width = 0;
    int height = 0;
    // RGBA8, top-left origin. Empty when decoding failed; the renderer uses
    // its fallback checker instead of aborting the whole import.
    std::vector<std::uint8_t> rgba;
    bool decoded = false;
};

struct Node {
    std::string name;
    Vec3 translation{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    Mat4 local = Mat4::identity();
    int parent = -1;
    std::vector<int> children;
    int mesh = -1;
    int skin = -1;
};

struct Skin {
    std::string name;
    int skeleton = -1;
    std::vector<int> joints;
    std::vector<Mat4> inverseBindMatrices;
};

struct HumanoidEntry {
    std::string name;
    int node = -1;
    std::optional<HumanoidBone> core;
};

struct Metadata {
    std::string title;
    std::string author;
    std::string licenseName;
    std::string allowedUserName;
    std::string commercialUsage;
    std::string sexualUsage;
    std::string violentUsage;
    std::string otherPermission;
    int thumbnailTexture = -1;
};

struct Diagnostics {
    int nodeCount = 0;
    int sceneRootCount = 0;
    int meshCount = 0;
    int primitiveCount = 0;
    int triangleCount = 0;
    int vertexCount = 0;
    int materialCount = 0;
    int textureCount = 0;
    int decodedTextureCount = 0;
    int skinCount = 0;
    int jointCount = 0;
    int humanoidCount = 0;
    int humanoidCoreCount = 0;
    int estimatedCpuTextureBytes = 0;
    double loadMilliseconds = 0.0;
};

class VrmModel {
public:
    std::string sourcePath;
    std::string name;
    std::string exporterVersion;
    std::string specVersion;
    std::uint64_t fileSize = 0;

    std::vector<Node> nodes;
    std::vector<int> sceneRoots;
    std::vector<Mesh> meshes;
    std::vector<MeshInstance> meshInstances;
    std::vector<Skin> skins;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<HumanoidEntry> humanoidEntries;
    std::array<int, kHumanoidBoneCount> humanoidNodes{};
    Metadata metadata;
    Diagnostics diagnostics;

    // Bind-pose global transforms and rotations, computed once by the loader.
    std::vector<Mat4> bindGlobals;
    std::vector<Quat> bindGlobalRotations;
    Bounds bounds;
    Vec3 forward{0.0f, 0.0f, 1.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    float height = 1.0f;

    std::vector<std::string> warnings;

    VrmModel();
    ~VrmModel() = default;
    VrmModel(VrmModel&&) noexcept = default;
    VrmModel& operator=(VrmModel&&) noexcept = default;
    VrmModel(const VrmModel&) = delete;
    VrmModel& operator=(const VrmModel&) = delete;

    [[nodiscard]] int nodeFor(HumanoidBone bone) const;
    [[nodiscard]] bool hasBone(HumanoidBone bone) const;
    [[nodiscard]] const Skin* skinForNode(int nodeIndex) const;
    [[nodiscard]] const Material* materialForPrimitive(int mesh, int primitive) const;
    [[nodiscard]] Vec3 boneBindPosition(HumanoidBone bone) const;
    [[nodiscard]] bool completeHumanoid() const;
    void releaseTexturePixels();
};

struct LoadResult {
    std::unique_ptr<VrmModel> model;
    std::string error;
    std::vector<std::string> warnings;

    [[nodiscard]] bool ok() const { return model != nullptr; }
};

class VrmLoader {
public:
    // Loads a binary glTF/VRM container. Every buffer, accessor, index and
    // extension reference is range checked; corrupt input returns an error and
    // never throws or dereferences out-of-range data.
    [[nodiscard]] static LoadResult load(const std::filesystem::path& path);
    [[nodiscard]] static LoadResult loadFromMemory(
        std::span<const std::uint8_t> bytes, std::string sourceName = {});
};

} // namespace bgmedia::vrm
