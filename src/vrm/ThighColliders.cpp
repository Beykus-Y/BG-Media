#include "bgmedia/vrm/ThighColliders.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace bgmedia::vrm {
namespace {

using math::Mat4;
using math::Vec3;

// Radius fit. The fractions are of model height and use the same bounds as the
// retargeter's thigh proxy: the 90th percentile of the distance from the
// thigh-dominant skin vertices to the thigh axis, clamped to 2 %..9 % of
// height. Without enough skinned samples (no skinned mesh, unusual rig) the
// radius falls back to 4.5 % of height.
constexpr float kRadiusMinFraction = 0.020f;
constexpr float kRadiusMaxFraction = 0.090f;
constexpr float kRadiusFallbackFraction = 0.045f;
constexpr float kRadiusPercentile = 0.90f;
constexpr std::size_t kMinRadiusSamples = 50;
constexpr float kDominantWeight = 0.5f;
// Thighs shorter than this (fraction of height) are a broken rig, not a leg.
constexpr float kMinThighFraction = 0.01f;
// A rest joint may sit this far (fraction of height) above the hips and still
// count as hips-level. This absorbs a skirt root authored slightly high.
constexpr float kHipsTolerance = 0.02f;
constexpr float kTiny = 1.0e-6f;

// Sphere placement along the thigh axis (hip joint -> knee joint).
constexpr float kPlacementTwo[] = {0.25f, 0.75f};
constexpr float kPlacementThree[] = {0.20f, 0.50f, 0.80f};

// Bones whose subtrees are never given thigh colliders: hair, eyelashes, chest
// and shoulder accessories all hang from these. Spine is listed so that the
// whole upper body is excluded, not only the bones that are named here.
constexpr HumanoidBone kUpperBodyBones[] = {
    HumanoidBone::Spine,        HumanoidBone::Chest,         HumanoidBone::UpperChest,
    HumanoidBone::Neck,         HumanoidBone::Head,          HumanoidBone::LeftShoulder,
    HumanoidBone::RightShoulder, HumanoidBone::LeftUpperArm, HumanoidBone::RightUpperArm,
    HumanoidBone::LeftLowerArm, HumanoidBone::RightLowerArm, HumanoidBone::LeftHand,
    HumanoidBone::RightHand,    HumanoidBone::LeftEye,       HumanoidBone::RightEye,
};

bool validNode(const VrmModel& model, int node) {
    return node >= 0 && static_cast<std::size_t>(node) < model.nodes.size() &&
           static_cast<std::size_t>(node) < model.bindGlobals.size();
}

float distanceToSegment(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const float lengthSquared = ab.lengthSquared();
    const float t = lengthSquared > kTiny
                        ? std::clamp(math::dot(p - a, ab) / lengthSquared, 0.0f, 1.0f)
                        : 0.0f;
    return (p - (a + ab * t)).length();
}

// Nodes in the subtree of any upper-body humanoid bone (the bones included).
std::vector<char> upperBodySubtrees(const VrmModel& model) {
    std::vector<char> flagged(model.nodes.size(), 0);
    std::vector<int> stack;
    for (HumanoidBone bone : kUpperBodyBones) {
        const int node = model.nodeFor(bone);
        if (validNode(model, node)) {
            stack.push_back(node);
        }
    }
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        if (flagged[static_cast<std::size_t>(node)]) {
            continue;
        }
        flagged[static_cast<std::size_t>(node)] = 1;
        for (int child : model.nodes[static_cast<std::size_t>(node)].children) {
            if (child >= 0 && static_cast<std::size_t>(child) < model.nodes.size()) {
                stack.push_back(child);
            }
        }
    }
    return flagged;
}

// The placement rule. A chain is lower-body when its root is not under an
// upper-body bone and every rest joint is at or below the hips height.
bool isLowerBodyChain(const VrmModel& model, const SpringBoneChain& chain,
                      const std::vector<char>& upperBody, float hipsHeight, Vec3 up,
                      float height) {
    if (chain.nodes.empty()) {
        return false;
    }
    for (int node : chain.nodes) {
        if (!validNode(model, node)) {
            return false;
        }
    }
    if (upperBody[static_cast<std::size_t>(chain.nodes.front())]) {
        return false;
    }
    const float limit = hipsHeight + kHipsTolerance * height;
    for (int node : chain.nodes) {
        const float h = math::dot(model.bindGlobals[static_cast<std::size_t>(node)].translationPart(), up);
        if (!(h <= limit)) {  // also rejects NaN
            return false;
        }
    }
    return true;
}

// Radius of one thigh from its own skin. Only vertices whose dominant joint is
// the upper leg are used; they are placed in bind pose exactly as the retargeter
// does for its proxy fit, then measured against the thigh axis.
float fitThighRadius(const VrmModel& model, int upper, Vec3 hip, Vec3 knee, float height,
                     bool& fromMesh) {
    fromMesh = false;
    std::vector<float> distances;
    for (const MeshInstance& instance : model.meshInstances) {
        if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= model.meshes.size()) {
            continue;
        }
        const Skin* skin = model.skinForNode(instance.node);
        if (!skin) {
            continue;
        }
        for (const Primitive& primitive :
             model.meshes[static_cast<std::size_t>(instance.mesh)].primitives) {
            if (!primitive.skinned) {
                continue;
            }
            for (const Vertex& vertex : primitive.vertices) {
                std::size_t best = 0;
                for (std::size_t k = 1; k < 4; ++k) {
                    if (vertex.weights[k] > vertex.weights[best]) {
                        best = k;
                    }
                }
                const std::size_t joint = vertex.joints[best];
                if (vertex.weights[best] < kDominantWeight || joint >= skin->joints.size() ||
                    joint >= skin->inverseBindMatrices.size()) {
                    continue;
                }
                if (skin->joints[joint] != upper) {
                    continue;
                }
                const Vec3 p = model.bindGlobals[static_cast<std::size_t>(upper)].transformPoint(
                    skin->inverseBindMatrices[joint].transformPoint(vertex.position));
                if (math::finite(p)) {
                    distances.push_back(distanceToSegment(p, hip, knee));
                }
            }
        }
    }
    if (distances.size() < kMinRadiusSamples) {
        return kRadiusFallbackFraction * height;
    }
    const std::size_t rank = std::min(
        distances.size() - 1,
        static_cast<std::size_t>(kRadiusPercentile * static_cast<float>(distances.size() - 1)));
    std::nth_element(distances.begin(),
                     distances.begin() + static_cast<std::ptrdiff_t>(rank), distances.end());
    fromMesh = true;
    return std::clamp(distances[rank], kRadiusMinFraction * height, kRadiusMaxFraction * height);
}

}  // namespace

ThighColliderReport synthesizeThighColliders(VrmModel& model, const ThighColliderOptions& options) {
    ThighColliderReport report;
    const auto fail = [&report](const char* reason) {
        report.skipReason = reason;
        return report;
    };
    if (!options.enabled) {
        return fail("disabled");
    }

    const int hips = model.nodeFor(HumanoidBone::Hips);
    const int upper[2] = {model.nodeFor(HumanoidBone::LeftUpperLeg),
                          model.nodeFor(HumanoidBone::RightUpperLeg)};
    const int lower[2] = {model.nodeFor(HumanoidBone::LeftLowerLeg),
                          model.nodeFor(HumanoidBone::RightLowerLeg)};
    if (!validNode(model, hips) || !validNode(model, upper[0]) || !validNode(model, upper[1]) ||
        !validNode(model, lower[0]) || !validNode(model, lower[1])) {
        return fail("missing hips or leg bones");
    }
    // Any authored collider on the hips or upper legs means the author already
    // handled the legs: the model is left exactly as it is.
    for (const SpringColliderGroup& group : model.springColliderGroups) {
        if (!group.colliders.empty() &&
            (group.node == hips || group.node == upper[0] || group.node == upper[1])) {
            return fail("already has a collider on hips or upper legs");
        }
    }
    if (model.springChains.empty()) {
        return fail("no spring chains");
    }

    const float height = std::max(model.height, 0.05f);
    const Vec3 up = (math::finite(model.up) && model.up.lengthSquared() > kTiny)
                        ? model.up / model.up.length()
                        : Vec3{0.0f, 1.0f, 0.0f};
    const float hipsHeight = math::dot(model.bindGlobals[static_cast<std::size_t>(hips)].translationPart(), up);

    Vec3 hip[2];
    Vec3 knee[2];
    Mat4 inverse[2];
    for (int s = 0; s < 2; ++s) {
        const auto index = static_cast<std::size_t>(upper[s]);
        hip[s] = model.bindGlobals[index].translationPart();
        knee[s] = model.bindGlobals[static_cast<std::size_t>(lower[s])].translationPart();
        if (!math::finite(hip[s]) || !math::finite(knee[s]) ||
            (knee[s] - hip[s]).length() < kMinThighFraction * height) {
            return fail("degenerate thigh bind pose");
        }
        inverse[s] = model.bindGlobals[index].inverseAffine();
    }

    const std::vector<char> upperBody = upperBodySubtrees(model);
    std::vector<std::size_t> suitable;
    for (std::size_t c = 0; c < model.springChains.size(); ++c) {
        if (isLowerBodyChain(model, model.springChains[c], upperBody, hipsHeight, up, height)) {
            suitable.push_back(c);
        }
    }
    report.chainsExcluded = static_cast<int>(model.springChains.size() - suitable.size());
    if (suitable.empty()) {
        return fail("no lower-body spring chain");
    }

    // Build both groups completely before touching the model, so a failure
    // leaves no half-written state behind.
    const int spheres = std::clamp(options.spheresPerThigh, 2, 3);
    SpringColliderGroup groups[2];
    float radius[2] = {0.0f, 0.0f};
    bool fromMesh[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        radius[s] = fitThighRadius(model, upper[s], hip[s], knee[s], height, fromMesh[s]);
        groups[s].node = upper[s];
        for (int k = 0; k < spheres; ++k) {
            const float t = spheres == 3 ? kPlacementThree[k] : kPlacementTwo[k];
            const Vec3 world = hip[s] + (knee[s] - hip[s]) * t;
            const SpringCollider collider{inverse[s].transformPoint(world), radius[s]};
            if (!math::finite(collider.offset)) {
                return fail("degenerate thigh bind pose");
            }
            groups[s].colliders.push_back(collider);
        }
    }

    // Append only. Existing groups keep their indices, and chains keep every
    // reference they already have.
    for (SpringColliderGroup& group : groups) {
        model.springColliderGroups.push_back(std::move(group));
    }
    const int leftGroup = static_cast<int>(model.springColliderGroups.size()) - 2;
    for (std::size_t c : suitable) {
        std::vector<int>& refs = model.springChains[c].colliderGroups;
        refs.push_back(leftGroup);
        refs.push_back(leftGroup + 1);
    }
    model.diagnostics.springColliderGroupCount = static_cast<int>(model.springColliderGroups.size());
    model.diagnostics.springColliderCount += 2 * spheres;

    report.applied = true;
    report.groupsAdded = 2;
    report.spheresAdded = 2 * spheres;
    report.chainsTouched = static_cast<int>(suitable.size());
    report.leftRadius = radius[0];
    report.rightRadius = radius[1];
    report.leftRadiusFromMesh = fromMesh[0];
    report.rightRadiusFromMesh = fromMesh[1];
    return report;
}

}  // namespace bgmedia::vrm
