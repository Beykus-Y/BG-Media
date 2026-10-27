#include "bgmedia/vrm/VrmModel.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace bgmedia::vrm {
namespace {

struct BoneName {
    HumanoidBone bone;
    std::string_view vrmName;
};

// VRM 0.x humanoid bone identifiers, exactly as used in
// extensions.VRM.humanoid.humanBones[].bone.
constexpr BoneName kBoneNames[] = {
    {HumanoidBone::Hips, "hips"},
    {HumanoidBone::Spine, "spine"},
    {HumanoidBone::Chest, "chest"},
    {HumanoidBone::UpperChest, "upperChest"},
    {HumanoidBone::Neck, "neck"},
    {HumanoidBone::Head, "head"},
    {HumanoidBone::LeftShoulder, "leftShoulder"},
    {HumanoidBone::LeftUpperArm, "leftUpperArm"},
    {HumanoidBone::LeftLowerArm, "leftLowerArm"},
    {HumanoidBone::LeftHand, "leftHand"},
    {HumanoidBone::RightShoulder, "rightShoulder"},
    {HumanoidBone::RightUpperArm, "rightUpperArm"},
    {HumanoidBone::RightLowerArm, "rightLowerArm"},
    {HumanoidBone::RightHand, "rightHand"},
    {HumanoidBone::LeftUpperLeg, "leftUpperLeg"},
    {HumanoidBone::LeftLowerLeg, "leftLowerLeg"},
    {HumanoidBone::LeftFoot, "leftFoot"},
    {HumanoidBone::LeftToes, "leftToes"},
    {HumanoidBone::RightUpperLeg, "rightUpperLeg"},
    {HumanoidBone::RightLowerLeg, "rightLowerLeg"},
    {HumanoidBone::RightFoot, "rightFoot"},
    {HumanoidBone::RightToes, "rightToes"},
    {HumanoidBone::LeftEye, "leftEye"},
    {HumanoidBone::RightEye, "rightEye"},
};

} // namespace

std::string_view humanoidBoneName(HumanoidBone bone) {
    for (const BoneName& entry : kBoneNames) {
        if (entry.bone == bone) {
            return entry.vrmName;
        }
    }
    return "unknown";
}

std::optional<HumanoidBone> humanoidBoneFromVrmName(std::string_view name) {
    for (const BoneName& entry : kBoneNames) {
        if (entry.vrmName == name) {
            return entry.bone;
        }
    }
    return std::nullopt;
}

VrmModel::VrmModel() {
    humanoidNodes.fill(-1);
    for (auto& side : fingerNodes) {
        for (auto& finger : side) {
            finger.fill(-1);
        }
    }
}

int VrmModel::nodeFor(HumanoidBone bone) const {
    const std::size_t index = static_cast<std::size_t>(bone);
    return index < humanoidNodes.size() ? humanoidNodes[index] : -1;
}

bool VrmModel::hasBone(HumanoidBone bone) const {
    return nodeFor(bone) >= 0;
}

int VrmModel::fingerNode(int side, int finger, int joint) const {
    if (side < 0 || side > 1 || finger < 0 || finger >= kFingerCount ||
        joint < 0 || joint >= kFingerJointCount) {
        return -1;
    }
    return fingerNodes[static_cast<std::size_t>(side)]
                      [static_cast<std::size_t>(finger)]
                      [static_cast<std::size_t>(joint)];
}

int VrmModel::fingerBoneCount() const {
    int count = 0;
    for (const auto& side : fingerNodes) {
        for (const auto& finger : side) {
            for (int node : finger) {
                count += node >= 0 ? 1 : 0;
            }
        }
    }
    return count;
}

bool VrmModel::hasFingers() const {
    return fingerBoneCount() > 0;
}

int VrmModel::springBoneCount() const {
    int count = 0;
    for (const SpringBoneChain& chain : springChains) {
        // A branch chain's first node is its parent chain's node.
        count += static_cast<int>(chain.nodes.size()) - (chain.parentChain >= 0 ? 1 : 0);
    }
    return count;
}

const Skin* VrmModel::skinForNode(int nodeIndex) const {
    if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= nodes.size()) {
        return nullptr;
    }
    const int skin = nodes[static_cast<std::size_t>(nodeIndex)].skin;
    if (skin < 0 || static_cast<std::size_t>(skin) >= skins.size()) {
        return nullptr;
    }
    return &skins[static_cast<std::size_t>(skin)];
}

const Material* VrmModel::materialForPrimitive(int meshIndex,
                                                int primitiveIndex) const {
    if (meshIndex < 0 || static_cast<std::size_t>(meshIndex) >= meshes.size()) {
        return nullptr;
    }
    const Mesh& mesh = meshes[static_cast<std::size_t>(meshIndex)];
    if (primitiveIndex < 0 ||
        static_cast<std::size_t>(primitiveIndex) >= mesh.primitives.size()) {
        return nullptr;
    }
    const int material =
        mesh.primitives[static_cast<std::size_t>(primitiveIndex)].material;
    if (material < 0 || static_cast<std::size_t>(material) >= materials.size()) {
        return nullptr;
    }
    return &materials[static_cast<std::size_t>(material)];
}

Vec3 VrmModel::boneBindPosition(HumanoidBone bone) const {
    const int node = nodeFor(bone);
    if (node < 0 || static_cast<std::size_t>(node) >= bindGlobals.size()) {
        return {};
    }
    return bindGlobals[static_cast<std::size_t>(node)].translationPart();
}

bool VrmModel::completeHumanoid() const {
    // The retargeter can produce a useful pose with this core. Spine/chest and
    // shoulders are optional because some stylised rigs collapse them, but the
    // hips, head, arms and legs are required to apply BG Motion without guessing.
    constexpr HumanoidBone required[] = {
        HumanoidBone::Hips,
        HumanoidBone::Head,
        HumanoidBone::LeftUpperArm,
        HumanoidBone::LeftLowerArm,
        HumanoidBone::LeftHand,
        HumanoidBone::RightUpperArm,
        HumanoidBone::RightLowerArm,
        HumanoidBone::RightHand,
        HumanoidBone::LeftUpperLeg,
        HumanoidBone::LeftLowerLeg,
        HumanoidBone::LeftFoot,
        HumanoidBone::RightUpperLeg,
        HumanoidBone::RightLowerLeg,
        HumanoidBone::RightFoot,
    };
    for (HumanoidBone bone : required) {
        if (!hasBone(bone)) {
            return false;
        }
    }
    return true;
}

void VrmModel::releaseTexturePixels() {
    for (Texture& texture : textures) {
        std::vector<std::uint8_t>().swap(texture.rgba);
        texture.decoded = false;
    }
}

} // namespace bgmedia::vrm
