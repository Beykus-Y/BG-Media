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

struct PresetName {
    ExpressionPreset preset;
    std::string_view name;
};

// VRM 1.0 expressions.preset keys.
constexpr PresetName kPresetNames[] = {
    {ExpressionPreset::Neutral, "neutral"},
    {ExpressionPreset::Happy, "happy"},
    {ExpressionPreset::Angry, "angry"},
    {ExpressionPreset::Sad, "sad"},
    {ExpressionPreset::Relaxed, "relaxed"},
    {ExpressionPreset::Surprised, "surprised"},
    {ExpressionPreset::Aa, "aa"},
    {ExpressionPreset::Ih, "ih"},
    {ExpressionPreset::Ou, "ou"},
    {ExpressionPreset::Ee, "ee"},
    {ExpressionPreset::Oh, "oh"},
    {ExpressionPreset::Blink, "blink"},
    {ExpressionPreset::BlinkLeft, "blinkLeft"},
    {ExpressionPreset::BlinkRight, "blinkRight"},
    {ExpressionPreset::LookUp, "lookUp"},
    {ExpressionPreset::LookDown, "lookDown"},
    {ExpressionPreset::LookLeft, "lookLeft"},
    {ExpressionPreset::LookRight, "lookRight"},
};

// VRM 0.x blendShapeGroups[].presetName values (lower case).
constexpr PresetName kVrm0PresetNames[] = {
    {ExpressionPreset::Neutral, "neutral"},
    {ExpressionPreset::Happy, "joy"},
    {ExpressionPreset::Angry, "angry"},
    {ExpressionPreset::Sad, "sorrow"},
    {ExpressionPreset::Relaxed, "fun"},
    {ExpressionPreset::Aa, "a"},
    {ExpressionPreset::Ih, "i"},
    {ExpressionPreset::Ou, "u"},
    {ExpressionPreset::Ee, "e"},
    {ExpressionPreset::Oh, "o"},
    {ExpressionPreset::Blink, "blink"},
    {ExpressionPreset::BlinkLeft, "blink_l"},
    {ExpressionPreset::BlinkRight, "blink_r"},
    {ExpressionPreset::LookUp, "lookup"},
    {ExpressionPreset::LookDown, "lookdown"},
    {ExpressionPreset::LookLeft, "lookleft"},
    {ExpressionPreset::LookRight, "lookright"},
};

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view vrmVersionName(VrmVersion version) {
    return version == VrmVersion::V1 ? "1.0" : "0.x";
}

std::string_view expressionPresetName(ExpressionPreset preset) {
    for (const PresetName& entry : kPresetNames) {
        if (entry.preset == preset) {
            return entry.name;
        }
    }
    return "custom";
}

std::optional<ExpressionPreset> expressionPresetFromVrm1Name(std::string_view name) {
    for (const PresetName& entry : kPresetNames) {
        if (entry.name == name) {
            return entry.preset;
        }
    }
    return std::nullopt;
}

std::optional<ExpressionPreset> expressionPresetFromVrm0Name(std::string_view name) {
    for (const PresetName& entry : kVrm0PresetNames) {
        if (equalsIgnoreCase(entry.name, name)) {
            return entry.preset;
        }
    }
    return std::nullopt;
}

bool Metadata::restrictsRedistribution() const {
    if (!licenseName.empty() &&
        (allowedUserName == "OnlyAuthor" ||
         licenseName.find("Redistribution") != std::string::npos)) {
        return true;
    }
    return avatarPermission == "onlyAuthor" ||
           (allowRedistribution.has_value() && !*allowRedistribution);
}

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
        count += static_cast<int>(chain.nodes.size()) -
                 (chain.parentChain >= 0 && !chain.attached ? 1 : 0);
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

const Expression* VrmModel::findExpression(ExpressionPreset preset) const {
    if (preset == ExpressionPreset::Custom) {
        return nullptr;
    }
    for (const Expression& expression : expressions) {
        if (expression.preset == preset) {
            return &expression;
        }
    }
    return nullptr;
}

const Expression* VrmModel::findExpression(std::string_view expressionName) const {
    for (const Expression& expression : expressions) {
        if (expression.name == expressionName) {
            return &expression;
        }
    }
    return nullptr;
}

void VrmModel::releaseTexturePixels() {
    for (Texture& texture : textures) {
        std::vector<std::uint8_t>().swap(texture.rgba);
        texture.decoded = false;
    }
}

} // namespace bgmedia::vrm
