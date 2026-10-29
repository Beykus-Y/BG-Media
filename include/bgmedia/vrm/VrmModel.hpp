#pragma once

// VRM import data model for both schema generations:
//   VRM 0.x  glTF 2.0 + extensions.VRM            (UniVRM 0.x, faces -Z)
//   VRM 1.0  glTF 2.0 + extensions.VRMC_vrm and   (faces +Z)
//            VRMC_springBone / VRMC_node_constraint / VRMC_materials_mtoon
//
// The loader detects the generation and adapts a 1.0 file into the same
// conventions the rest of the engine was built on (see VrmModel::version and
// VrmModel::sourceToModel), so animation, springs and the renderer never branch
// on the version.
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

// VRM 0.x finger bones are optional. They are kept separate from the stable
// core enum above because every clip track and retarget path is sized by
// kHumanoidBoneCount; expanding that enum would silently invalidate existing
// clips. The retargeter discovers these nodes through humanoidEntries and this
// table is the shared capability report for the UI, QA and hand layer.
enum class Finger : std::uint8_t {
    Thumb,
    Index,
    Middle,
    Ring,
    Little,
    Count
};

inline constexpr int kFingerCount = static_cast<int>(Finger::Count);
inline constexpr int kFingerJointCount = 3;

// BG Motion 3.2 secondary animation. The importer stores the authored VRM 0.x
// parameters verbatim; SecondaryDynamics expands `springGroups` into linear
// chains (`springChains`) once, because the runtime simulates chains, not the
// original group graph.
struct SpringCollider {
    Vec3 offset{};
    float radius = 0.0f;
};

// Which schema generation the file was authored in.
enum class VrmVersion : std::uint8_t {
    V0,  // extensions.VRM
    V1   // extensions.VRMC_vrm
};

[[nodiscard]] std::string_view vrmVersionName(VrmVersion version);

struct SpringColliderGroup {
    int node = -1;
    std::vector<SpringCollider> colliders;
};

struct SpringBoneGroup {
    std::string comment;
    int center = -1;
    std::vector<int> roots;             // nodes listed in secondaryAnimation.boneGroups[].bones
    std::vector<int> colliderGroups;    // indices into springColliderGroups
    float stiffness = 0.5f;             // VRM 0.x spelling: stiffiness
    float gravityPower = 0.0f;
    float dragForce = 0.1f;
    float hitRadius = 0.01f;
    Vec3 gravityDir{0.0f, -1.0f, 0.0f};
};

struct SpringBoneChain {
    int group = -1;
    // root -> tip, parent/child order. A branch chain (parentChain >= 0) starts
    // with its *anchor*: the branch point, which belongs to - and is written by -
    // the parent chain; it must not be simulated or written a second time.
    std::vector<int> nodes;
    int parentChain = -1;               // index into springChains, -1 = root chain
    int parentIndex = 0;                // index of the anchor inside the parent chain
    // VRM 1.0: a spring rooted below another chain's node (not on it). Then
    // nodes[0] is this chain's own root, written by this chain, and it rides
    // rigidly on the simulated ancestor parentChain/parentIndex instead of
    // being an anchor shared with it.
    bool attached = false;
    std::vector<int> colliderGroups;
    float stiffness = 0.5f;
    float gravityPower = 0.0f;
    float dragForce = 0.1f;
    float hitRadius = 0.01f;
    Vec3 gravityDir{0.0f, -1.0f, 0.0f};
};

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
    // VRM 0.x bone name. A 1.0 file's names are normalised to it: the 1.0
    // thumb chain (Metacarpal, Proximal, Distal) becomes the 0.x chain
    // (Proximal, Intermediate, Distal), the same bones under older names.
    std::string name;
    std::string sourceName;  // as written in the file
    int node = -1;
    std::optional<HumanoidBone> core;
};

// The VRM 0.x fields keep their 0.x spelling and values. A VRM 1.0 file fills
// them with the nearest 0.x equivalent (title <- name, author <- authors,
// allowedUserName <- avatarPermission, ...) so existing UI keeps working, and
// also fills the 1.0-only fields below.
struct Metadata {
    std::string title;
    std::string author;
    std::string licenseName;
    std::string allowedUserName;   // "OnlyAuthor", "ExplicitlyLicensedPerson", "Everyone"
    std::string commercialUsage;   // "Allow" / "Disallow"
    std::string sexualUsage;
    std::string violentUsage;
    std::string otherPermission;
    int thumbnailTexture = -1;

    std::string version;                 // model version (both generations)
    std::vector<std::string> authors;    // 1.0 authors; 0.x: {author}
    std::string contactInformation;
    std::string reference;
    std::string copyrightInformation;    // 1.0
    std::string licenseUrl;              // 1.0
    std::string thirdPartyLicenses;      // 1.0
    std::string otherLicenseUrl;         // both
    std::string avatarPermission;        // 1.0 raw value
    std::string commercialUsageV1;       // 1.0 raw value (personalNonProfit, ...)
    std::string creditNotation;          // 1.0 ("required" / "unnecessary")
    std::string modification;            // 1.0 ("prohibited", ...)
    std::optional<bool> allowRedistribution;           // 1.0 only
    std::optional<bool> allowPoliticalOrReligiousUsage;  // 1.0 only
    std::optional<bool> allowAntisocialOrHateUsage;    // 1.0 only
    int thumbnailImage = -1;             // glTF image index (1.0 thumbnailImage)

    // True when the licence keeps the avatar to its author or forbids
    // redistribution. Same answer as the 0.x-only check the desktop used before.
    [[nodiscard]] bool restrictsRedistribution() const;
};

// ---------------------------------------------------------------------------
// Expressions (VRM 1.0 expressions, VRM 0.x blendShapeMaster)
// ---------------------------------------------------------------------------
// The preset set is VRM 1.0's; 0.x presets are mapped onto it (joy -> Happy,
// sorrow -> Sad, fun -> Relaxed, a -> Aa, blink_l -> BlinkLeft, ...).
enum class ExpressionPreset : std::uint8_t {
    Custom,
    Neutral,
    Happy,
    Angry,
    Sad,
    Relaxed,
    Surprised,
    Aa,
    Ih,
    Ou,
    Ee,
    Oh,
    Blink,
    BlinkLeft,
    BlinkRight,
    LookUp,
    LookDown,
    LookLeft,
    LookRight
};

[[nodiscard]] std::string_view expressionPresetName(ExpressionPreset preset);
[[nodiscard]] std::optional<ExpressionPreset> expressionPresetFromVrm1Name(
    std::string_view name);
// Case-insensitive, as UniVRM 0.x wrote both "Joy" and "joy".
[[nodiscard]] std::optional<ExpressionPreset> expressionPresetFromVrm0Name(
    std::string_view name);

enum class ExpressionOverride : std::uint8_t { None, Block, Blend };

struct MorphTargetBind {
    int mesh = -1;      // index into VrmModel::meshes
    int node = -1;      // 1.0: the node that instances the mesh; 0.x: -1
    int index = -1;     // morph target index inside the mesh
    float weight = 0.0f;  // 0..1 (0.x weights are 0..100 and are rescaled)
};

enum class MaterialColorType : std::uint8_t {
    Color,
    EmissionColor,
    ShadeColor,
    MatcapColor,
    RimColor,
    OutlineColor
};

struct MaterialColorBind {
    int material = -1;
    MaterialColorType type = MaterialColorType::Color;
    Vec4 targetValue{1.0f, 1.0f, 1.0f, 1.0f};
};

struct TextureTransformBind {
    int material = -1;
    Vec2 scale{1.0f, 1.0f};
    Vec2 offset{0.0f, 0.0f};
};

struct Expression {
    std::string name;
    ExpressionPreset preset = ExpressionPreset::Custom;
    bool isBinary = false;
    ExpressionOverride overrideBlink = ExpressionOverride::None;
    ExpressionOverride overrideLookAt = ExpressionOverride::None;
    ExpressionOverride overrideMouth = ExpressionOverride::None;
    std::vector<MorphTargetBind> morphTargetBinds;
    std::vector<MaterialColorBind> materialColorBinds;
    std::vector<TextureTransformBind> textureTransformBinds;
};

// ---------------------------------------------------------------------------
// Look-at (VRM 1.0 lookAt, VRM 0.x firstPerson.lookAt*)
// ---------------------------------------------------------------------------
enum class LookAtType : std::uint8_t { Bone, Expression };

// Yaw/pitch in degrees up to inputMaxValue map linearly to outputScale
// (degrees of eye bone rotation for Bone, expression weight for Expression).
// 0.x curves are reduced to their end points (xRange -> inputMaxValue,
// yRange -> outputScale), which is what UniVRM's default linear curve means.
struct LookAtRangeMap {
    float inputMaxValue = 90.0f;
    float outputScale = 10.0f;
};

struct LookAt {
    bool authored = false;
    LookAtType type = LookAtType::Bone;
    // Eye origin relative to the head bone, in model axes (already converted
    // to the engine's -Z-forward frame for both generations).
    Vec3 offsetFromHeadBone{0.0f, 0.06f, 0.0f};
    LookAtRangeMap horizontalInner;
    LookAtRangeMap horizontalOuter;
    LookAtRangeMap verticalDown;
    LookAtRangeMap verticalUp;
};

// First-person mesh visibility (1.0 meshAnnotations by node; 0.x by mesh,
// expanded here to every node that instances that mesh).
enum class FirstPersonType : std::uint8_t {
    Auto,
    Both,
    ThirdPersonOnly,
    FirstPersonOnly
};

struct FirstPersonAnnotation {
    int node = -1;
    FirstPersonType type = FirstPersonType::Auto;
};

// ---------------------------------------------------------------------------
// Node constraints (VRMC_node_constraint, VRM 1.0 only)
// ---------------------------------------------------------------------------
enum class NodeConstraintType : std::uint8_t { Roll, Aim, Rotation };

struct NodeConstraint {
    int destination = -1;   // the node carrying the extension
    int source = -1;
    NodeConstraintType type = NodeConstraintType::Rotation;
    // Roll: the roll axis; Aim: the aim axis (signed). Unit vector in the
    // destination's local space. Unused for Rotation.
    Vec3 axis{1.0f, 0.0f, 0.0f};
    float weight = 1.0f;
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
    int fingerBoneCount = 0;
    int springGroupCount = 0;
    int springBoneCount = 0;
    int springChainCount = 0;
    int springColliderGroupCount = 0;
    int springColliderCount = 0;
    int blendShapeCount = 0;   // 0.x blend shape groups / 1.0 expressions
    int expressionCount = 0;
    int nodeConstraintCount = 0;
    int estimatedCpuTextureBytes = 0;
    double loadMilliseconds = 0.0;
};

class VrmModel {
public:
    std::string sourcePath;
    std::string name;
    std::string exporterVersion;
    std::string specVersion;
    VrmVersion version = VrmVersion::V0;
    // Rotation the loader applied to the file's scene roots so every model
    // faces -Z like VRM 0.x (the frame the camera, clips and tools assume).
    // Identity for 0.x; a half turn about +Y for 1.0, which faces +Z. Any data
    // read later in the file's own model frame must be rotated by it.
    Quat sourceToModel = Quat::identity();
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

    // [side][finger][joint] node indices, -1 when the model has no such bone.
    // Side 0 = left, 1 = right; joint 0 = proximal, 1 = intermediate,
    // 2 = distal. VRM 0.x may omit any or all of them.
    std::array<std::array<std::array<int, kFingerJointCount>, kFingerCount>, 2>
        fingerNodes{};

    // Secondary animation, parsed from extensions.VRM.secondaryAnimation
    // (VRM 0.x) or VRMC_springBone (VRM 1.0). Empty vectors mean the model
    // has no authored soft-body rig.
    std::vector<SpringBoneGroup> springGroups;
    std::vector<SpringColliderGroup> springColliderGroups;
    std::vector<SpringBoneChain> springChains;
    std::string springSource;  // "VRM.secondaryAnimation", "VRMC_springBone" or empty

    std::vector<Expression> expressions;
    LookAt lookAt;
    int firstPersonBone = -1;  // 0.x firstPersonBone; 1.0: the head
    std::vector<FirstPersonAnnotation> firstPersonAnnotations;
    // Evaluated by animation::applyNodeConstraints after retargeting.
    std::vector<NodeConstraint> nodeConstraints;

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
    [[nodiscard]] int fingerNode(int side, int finger, int joint) const;
    [[nodiscard]] int fingerBoneCount() const;
    [[nodiscard]] bool hasFingers() const;
    [[nodiscard]] int springBoneCount() const;
    [[nodiscard]] bool hasSpringBones() const { return !springChains.empty(); }
    [[nodiscard]] const Skin* skinForNode(int nodeIndex) const;
    [[nodiscard]] const Material* materialForPrimitive(int mesh, int primitive) const;
    [[nodiscard]] Vec3 boneBindPosition(HumanoidBone bone) const;
    [[nodiscard]] bool completeHumanoid() const;
    [[nodiscard]] const Expression* findExpression(ExpressionPreset preset) const;
    [[nodiscard]] const Expression* findExpression(std::string_view name) const;
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
