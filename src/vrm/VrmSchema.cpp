#include "VrmSchema.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bgmedia::vrm::schema {
namespace {

using json::JsonValue;
using json::numberField;
using json::objectField;
using json::stringField;
using json::vec3Field;

// A JSON number that is a whole index in [0, count).
std::optional<int> indexOf(const JsonValue* value, std::size_t count) {
    if (!value || !value->isNumber() || count == 0) {
        return std::nullopt;
    }
    const double raw = value->number;
    if (!(raw >= 0.0) || raw > static_cast<double>(count - 1) || std::floor(raw) != raw) {
        return std::nullopt;
    }
    return static_cast<int>(raw);
}

std::optional<int> indexField(const JsonValue* object, std::string_view key,
                              std::size_t count) {
    return indexOf(objectField(object, key), count);
}

std::optional<bool> boolField(const JsonValue* object, std::string_view key) {
    const JsonValue* value = objectField(object, key);
    if (!value || value->type != JsonValue::Type::Bool) {
        return std::nullopt;
    }
    return value->boolean;
}

float floatField(const JsonValue* object, std::string_view key, float fallback) {
    const double value = numberField(object, key, fallback);
    return std::isfinite(value) ? static_cast<float>(value) : fallback;
}

std::string joinStrings(const JsonValue* array) {
    std::string out;
    if (!array || !array->isArray()) {
        return out;
    }
    for (const JsonValue& item : array->items) {
        if (!item.isString() || item.string.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += item.string;
    }
    return out;
}

Vec4 colorField(const JsonValue* object, std::string_view key, Vec4 fallback) {
    const JsonValue* value = objectField(object, key);
    if (!value || !value->isArray() || value->items.size() < 3) {
        return fallback;
    }
    float c[4] = {fallback.x, fallback.y, fallback.z, fallback.w};
    for (std::size_t i = 0; i < value->items.size() && i < 4; ++i) {
        if (!value->items[i].isNumber() || !std::isfinite(value->items[i].number)) {
            return fallback;
        }
        c[i] = static_cast<float>(value->items[i].number);
    }
    return {c[0], c[1], c[2], c[3]};
}

Vec2 vec2Field(const JsonValue* object, std::string_view key, Vec2 fallback) {
    const JsonValue* value = objectField(object, key);
    if (!value || !value->isArray() || value->items.size() < 2 ||
        !value->items[0].isNumber() || !value->items[1].isNumber() ||
        !std::isfinite(value->items[0].number) || !std::isfinite(value->items[1].number)) {
        return fallback;
    }
    return {static_cast<float>(value->items[0].number),
            static_cast<float>(value->items[1].number)};
}

ExpressionOverride overrideField(const JsonValue* object, std::string_view key) {
    const std::string value = stringField(object, key);
    if (value == "block") {
        return ExpressionOverride::Block;
    }
    if (value == "blend") {
        return ExpressionOverride::Blend;
    }
    return ExpressionOverride::None;
}

std::optional<MaterialColorType> materialColorTypeV1(std::string_view name) {
    if (name == "color") return MaterialColorType::Color;
    if (name == "emissionColor") return MaterialColorType::EmissionColor;
    if (name == "shadeColor") return MaterialColorType::ShadeColor;
    if (name == "matcapColor") return MaterialColorType::MatcapColor;
    if (name == "rimColor") return MaterialColorType::RimColor;
    if (name == "outlineColor") return MaterialColorType::OutlineColor;
    return std::nullopt;
}

std::optional<MaterialColorType> materialColorTypeV0(std::string_view property) {
    if (property == "_Color") return MaterialColorType::Color;
    if (property == "_EmissionColor") return MaterialColorType::EmissionColor;
    if (property == "_ShadeColor") return MaterialColorType::ShadeColor;
    if (property == "_RimColor") return MaterialColorType::RimColor;
    if (property == "_OutlineColor") return MaterialColorType::OutlineColor;
    return std::nullopt;
}

LookAtRangeMap rangeMapV1(const JsonValue* object, float defaultScale) {
    LookAtRangeMap map;
    map.outputScale = defaultScale;
    if (!object || !object->isObject()) {
        return map;
    }
    map.inputMaxValue = std::clamp(floatField(object, "inputMaxValue", 90.0f), 0.0f, 180.0f);
    map.outputScale = floatField(object, "outputScale", defaultScale);
    return map;
}

LookAtRangeMap rangeMapV0(const JsonValue* object, float defaultScale) {
    LookAtRangeMap map;
    map.outputScale = defaultScale;
    if (!object || !object->isObject()) {
        return map;
    }
    map.inputMaxValue = std::clamp(floatField(object, "xRange", 90.0f), 0.0f, 180.0f);
    map.outputScale = floatField(object, "yRange", defaultScale);
    return map;
}

bool isAncestorOf(const VrmModel& model, int ancestor, int node) {
    for (int current = model.nodes[static_cast<std::size_t>(node)].parent, guard = 0;
         current >= 0 && guard < static_cast<int>(model.nodes.size());
         current = model.nodes[static_cast<std::size_t>(current)].parent, ++guard) {
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

// Appends spheres covering the segment head..tail (node-local), spaced at most
// one radius apart: the union then has no gap and its surface sags by at most
// 13 % of the radius between centres. Returns false when the sphere budget ran
// out first (a capsule more than ~127 radii long), which leaves gaps.
bool appendCapsule(SpringColliderGroup& group, Vec3 head, Vec3 tail, float radius) {
    constexpr int kMaxSpheres = 128;
    const float length = (tail - head).length();
    const float needed = std::ceil(length / std::max(radius, 1.0e-4f)) + 1.0f;
    const int count = static_cast<int>(std::clamp(needed, 2.0f, static_cast<float>(kMaxSpheres)));
    for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        group.colliders.push_back({math::lerp(head, tail, t), radius});
    }
    return needed <= static_cast<float>(kMaxSpheres);
}

} // namespace

std::string vrm0BoneName(std::string_view vrm1Name) {
    static constexpr const char* kSides[2] = {"left", "right"};
    for (const char* side : kSides) {
        const std::string prefix = std::string(side) + "Thumb";
        if (vrm1Name == prefix + "Metacarpal") {
            return prefix + "Proximal";
        }
        if (vrm1Name == prefix + "Proximal") {
            return prefix + "Intermediate";
        }
    }
    return std::string(vrm1Name);
}

void readMetaV1(const JsonValue* meta, VrmModel& model) {
    Metadata& out = model.metadata;
    out.title = stringField(meta, "name");
    out.version = stringField(meta, "version");
    if (const JsonValue* authors = objectField(meta, "authors"); authors && authors->isArray()) {
        for (const JsonValue& author : authors->items) {
            if (author.isString() && !author.string.empty()) {
                out.authors.push_back(author.string);
            }
        }
        out.author = joinStrings(authors);
    }
    out.copyrightInformation = stringField(meta, "copyrightInformation");
    out.contactInformation = stringField(meta, "contactInformation");
    out.reference = joinStrings(objectField(meta, "references"));
    out.thirdPartyLicenses = stringField(meta, "thirdPartyLicenses");
    out.thumbnailImage = static_cast<int>(numberField(meta, "thumbnailImage", -1.0));
    out.licenseUrl = stringField(meta, "licenseUrl");
    out.licenseName = out.licenseUrl;
    out.avatarPermission = stringField(meta, "avatarPermission");
    if (out.avatarPermission.empty()) {
        out.avatarPermission = "onlyAuthor";  // VRM 1.0 default
    }
    if (out.avatarPermission == "onlyAuthor") {
        out.allowedUserName = "OnlyAuthor";
    } else if (out.avatarPermission == "onlySeparatelyLicensedPerson") {
        out.allowedUserName = "ExplicitlyLicensedPerson";
    } else if (out.avatarPermission == "everyone") {
        out.allowedUserName = "Everyone";
    }
    const auto allow = [](std::optional<bool> value) {
        return value.value_or(false) ? std::string("Allow") : std::string("Disallow");
    };
    out.violentUsage = allow(boolField(meta, "allowExcessivelyViolentUsage"));
    out.sexualUsage = allow(boolField(meta, "allowExcessivelySexualUsage"));
    out.commercialUsageV1 = stringField(meta, "commercialUsage");
    if (out.commercialUsageV1.empty()) {
        out.commercialUsageV1 = "personalNonProfit";  // VRM 1.0 default
    }
    out.commercialUsage = out.commercialUsageV1 == "personalNonProfit" ? "Disallow" : "Allow";
    out.allowPoliticalOrReligiousUsage =
        boolField(meta, "allowPoliticalOrReligiousUsage").value_or(false);
    out.allowAntisocialOrHateUsage =
        boolField(meta, "allowAntisocialOrHateUsage").value_or(false);
    out.creditNotation = stringField(meta, "creditNotation");
    out.allowRedistribution = boolField(meta, "allowRedistribution").value_or(false);
    out.modification = stringField(meta, "modification");
    out.otherLicenseUrl = stringField(meta, "otherLicenseUrl");
    out.otherPermission = out.otherLicenseUrl;
}

void readSpringBoneV1(const JsonValue* springBone, VrmModel& model) {
    if (!springBone || !springBone->isObject()) {
        return;
    }
    model.springSource = "VRMC_springBone";
    const std::size_t nodeCount = model.nodes.size();

    // One SpringColliderGroup per 1.0 collider: the runtime keys a group to a
    // single node, while a 1.0 collider group may span several nodes.
    std::vector<int> colliderToGroup;
    if (const JsonValue* colliders = objectField(springBone, "colliders");
        colliders && colliders->isArray()) {
        for (const JsonValue& collider : colliders->items) {
            const auto node = indexField(&collider, "node", nodeCount);
            const JsonValue* shape = objectField(&collider, "shape");
            const JsonValue* sphere = objectField(shape, "sphere");
            const JsonValue* capsule = objectField(shape, "capsule");
            SpringColliderGroup group;
            if (node) {
                group.node = *node;
                if (sphere) {
                    const Vec3 offset = vec3Field(sphere, "offset", {});
                    const float radius = floatField(sphere, "radius", 0.0f);
                    if (math::finite(offset) && radius > 0.0f) {
                        group.colliders.push_back({offset, radius});
                    }
                } else if (capsule) {
                    const Vec3 offset = vec3Field(capsule, "offset", {});
                    const Vec3 tail = vec3Field(capsule, "tail", {});
                    const float radius = floatField(capsule, "radius", 0.0f);
                    if (math::finite(offset) && math::finite(tail) && radius > 0.0f &&
                        !appendCapsule(group, offset, tail, radius)) {
                        model.warnings.push_back(
                            "VRMC_springBone: capsule collider too long for its radius; "
                            "approximated with gaps");
                    }
                }
            }
            if (group.colliders.empty()) {
                model.warnings.push_back("VRMC_springBone: ignored invalid collider");
                colliderToGroup.push_back(-1);
                continue;
            }
            colliderToGroup.push_back(static_cast<int>(model.springColliderGroups.size()));
            model.springColliderGroups.push_back(std::move(group));
        }
    }

    std::vector<std::vector<int>> colliderGroups;
    if (const JsonValue* groups = objectField(springBone, "colliderGroups");
        groups && groups->isArray()) {
        for (const JsonValue& group : groups->items) {
            std::vector<int> members;
            if (const JsonValue* refs = objectField(&group, "colliders"); refs && refs->isArray()) {
                for (const JsonValue& ref : refs->items) {
                    const auto index = indexOf(&ref, colliderToGroup.size());
                    if (index && colliderToGroup[static_cast<std::size_t>(*index)] >= 0) {
                        members.push_back(colliderToGroup[static_cast<std::size_t>(*index)]);
                    }
                }
            }
            colliderGroups.push_back(std::move(members));
        }
    }

    // node -> (chain, index) of the chain that writes it.
    std::unordered_map<int, std::pair<int, int>> owner;
    const JsonValue* springs = objectField(springBone, "springs");
    if (!springs || !springs->isArray()) {
        finishSpringDiagnostics(model);
        return;
    }
    // Shallowest root first, so a spring below another is linked to it whatever
    // the file order.
    const auto depthOf = [&](const JsonValue& spring) {
        const JsonValue* list = objectField(&spring, "joints");
        const auto node = list && list->isArray() && !list->items.empty()
                              ? indexField(&list->items.front(), "node", nodeCount)
                              : std::nullopt;
        int depth = 0;
        for (int n = node ? *node : -1; n >= 0 && depth <= static_cast<int>(nodeCount);
             n = model.nodes[static_cast<std::size_t>(n)].parent) {
            ++depth;
        }
        return depth;
    };
    std::vector<const JsonValue*> ordered;
    for (const JsonValue& spring : springs->items) {
        ordered.push_back(&spring);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const JsonValue* a, const JsonValue* b) { return depthOf(*a) < depthOf(*b); });
    for (const JsonValue* springPtr : ordered) {
        const JsonValue& spring = *springPtr;
        std::vector<int> joints;
        std::vector<const JsonValue*> jointJson;
        if (const JsonValue* list = objectField(&spring, "joints"); list && list->isArray()) {
            for (const JsonValue& joint : list->items) {
                const auto node = indexField(&joint, "node", nodeCount);
                if (!node) {
                    model.warnings.push_back("VRMC_springBone: ignored invalid joint node");
                    break;
                }
                // Each joint must descend from the previous one; the spec allows
                // intermediate nodes to be skipped. Stop where that breaks.
                if (!joints.empty() && !isAncestorOf(model, joints.back(), *node)) {
                    model.warnings.push_back(
                        "VRMC_springBone: spring joints are not an ancestor/descendant "
                        "chain; truncated");
                    break;
                }
                joints.push_back(*node);
                jointJson.push_back(&joint);
            }
        }
        // The last joint is the tail point; a spring needs at least one segment.
        if (joints.size() < 2) {
            continue;
        }

        SpringBoneGroup group;
        group.comment = stringField(&spring, "name");
        if (const auto center = indexField(&spring, "center", nodeCount)) {
            group.center = *center;
        }
        if (const JsonValue* refs = objectField(&spring, "colliderGroups"); refs && refs->isArray()) {
            for (const JsonValue& ref : refs->items) {
                const auto index = indexOf(&ref, colliderGroups.size());
                if (!index) {
                    model.warnings.push_back(
                        "VRMC_springBone: ignored invalid collider group reference");
                    continue;
                }
                for (int member : colliderGroups[static_cast<std::size_t>(*index)]) {
                    if (std::find(group.colliderGroups.begin(), group.colliderGroups.end(),
                                  member) == group.colliderGroups.end()) {
                        group.colliderGroups.push_back(member);
                    }
                }
            }
        }
        // Joint i's settings drive the segment joint i -> joint i+1. The runtime
        // simulates one setting per chain, so the segment settings are averaged
        // (exporters, including UniVRM's 0.x migration, write them uniformly).
        float stiffness = 0.0f;
        float gravityPower = 0.0f;
        float dragForce = 0.0f;
        float hitRadius = 0.0f;
        Vec3 gravityDir{};
        const std::size_t segments = joints.size() - 1;
        for (std::size_t i = 0; i < segments; ++i) {
            const JsonValue* joint = jointJson[i];
            stiffness += floatField(joint, "stiffness", 1.0f);
            gravityPower += floatField(joint, "gravityPower", 0.0f);
            dragForce += floatField(joint, "dragForce", 0.5f);
            hitRadius += floatField(joint, "hitRadius", 0.0f);
            Vec3 dir = vec3Field(joint, "gravityDir", {0.0f, -1.0f, 0.0f});
            if (!math::finite(dir) || dir.lengthSquared() < 1.0e-6f) {
                dir = {0.0f, -1.0f, 0.0f};
            }
            gravityDir += math::normalize(dir);
        }
        const float inv = 1.0f / static_cast<float>(segments);
        group.stiffness = std::clamp(stiffness * inv, 0.0f, 1.0f);
        group.gravityPower = std::clamp(gravityPower * inv, 0.0f, 10.0f);
        group.dragForce = std::clamp(dragForce * inv, 0.0f, 1.0f);
        group.hitRadius = std::max(0.0f, hitRadius * inv);
        // gravityDir is in the file's model frame.
        gravityDir = math::rotate(model.sourceToModel, gravityDir);
        group.gravityDir = gravityDir.lengthSquared() > 1.0e-6f ? math::normalize(gravityDir)
                                                                : Vec3{0.0f, -1.0f, 0.0f};
        group.roots.push_back(joints.front());

        SpringBoneChain chain;
        chain.group = static_cast<int>(model.springGroups.size());
        chain.colliderGroups = group.colliderGroups;
        chain.stiffness = group.stiffness;
        chain.gravityPower = group.gravityPower;
        chain.dragForce = group.dragForce;
        chain.hitRadius = group.hitRadius;
        chain.gravityDir = group.gravityDir;
        // Skipped intermediate nodes are not simulated (the spec ignores them);
        // the runtime carries them rigidly with their simulated ancestor.
        std::size_t first = 0;
        // A spring that starts on a joint another spring already simulates is a
        // branch: that joint becomes its anchor, written only by the parent.
        if (const auto found = owner.find(joints.front()); found != owner.end()) {
            chain.parentChain = found->second.first;
            chain.parentIndex = found->second.second;
            chain.nodes.push_back(joints.front());
            first = 1;
        } else {
            // A spring rooted below another spring's joint rides on that joint.
            for (int n = model.nodes[static_cast<std::size_t>(joints.front())].parent; n >= 0;
                 n = model.nodes[static_cast<std::size_t>(n)].parent) {
                if (const auto above = owner.find(n); above != owner.end()) {
                    chain.parentChain = above->second.first;
                    chain.parentIndex = above->second.second;
                    chain.attached = true;
                    break;
                }
            }
        }
        for (std::size_t i = first; i < joints.size(); ++i) {
            if (owner.count(joints[i]) != 0) {
                model.warnings.push_back(
                    "VRMC_springBone: joint shared by two springs; later spring truncated");
                break;
            }
            chain.nodes.push_back(joints[i]);
        }
        if (chain.nodes.size() < 2) {
            continue;
        }
        const int chainIndex = static_cast<int>(model.springChains.size());
        for (std::size_t i = first; i < chain.nodes.size(); ++i) {
            owner.emplace(chain.nodes[i], std::make_pair(chainIndex, static_cast<int>(i)));
        }
        model.springGroups.push_back(std::move(group));
        model.springChains.push_back(std::move(chain));
    }
    finishSpringDiagnostics(model);
}

void readNodeConstraintsV1(const JsonValue* root, VrmModel& model) {
    const JsonValue* nodes = objectField(root, "nodes");
    if (!nodes || !nodes->isArray()) {
        return;
    }
    const std::size_t nodeCount = model.nodes.size();
    for (std::size_t i = 0; i < nodes->items.size() && i < nodeCount; ++i) {
        const JsonValue* extension = objectField(
            objectField(&nodes->items[i], "extensions"), "VRMC_node_constraint");
        if (!extension) {
            continue;
        }
        const JsonValue* constraint = objectField(extension, "constraint");
        NodeConstraint parsed;
        parsed.destination = static_cast<int>(i);
        const JsonValue* body = nullptr;
        if ((body = objectField(constraint, "roll"))) {
            parsed.type = NodeConstraintType::Roll;
            const std::string axis = stringField(body, "rollAxis");
            if (axis == "X") {
                parsed.axis = {1.0f, 0.0f, 0.0f};
            } else if (axis == "Y") {
                parsed.axis = {0.0f, 1.0f, 0.0f};
            } else if (axis == "Z") {
                parsed.axis = {0.0f, 0.0f, 1.0f};
            } else {
                body = nullptr;
            }
        } else if ((body = objectField(constraint, "aim"))) {
            parsed.type = NodeConstraintType::Aim;
            const std::string axis = stringField(body, "aimAxis");
            const float sign = axis.rfind("Negative", 0) == 0 ? -1.0f : 1.0f;
            if (axis == "PositiveX" || axis == "NegativeX") {
                parsed.axis = Vec3{1.0f, 0.0f, 0.0f} * sign;
            } else if (axis == "PositiveY" || axis == "NegativeY") {
                parsed.axis = Vec3{0.0f, 1.0f, 0.0f} * sign;
            } else if (axis == "PositiveZ" || axis == "NegativeZ") {
                parsed.axis = Vec3{0.0f, 0.0f, 1.0f} * sign;
            } else {
                body = nullptr;
            }
        } else if ((body = objectField(constraint, "rotation"))) {
            parsed.type = NodeConstraintType::Rotation;
        }
        const auto source = indexField(body, "source", nodeCount);
        if (!body || !source || *source == parsed.destination) {
            model.warnings.push_back("VRMC_node_constraint: ignored invalid constraint on node " +
                                     std::to_string(i));
            continue;
        }
        if (parsed.type == NodeConstraintType::Aim &&
            isAncestorOf(model, parsed.destination, *source)) {
            // Aiming at a descendant moves the target with every iteration.
            model.warnings.push_back(
                "VRMC_node_constraint: ignored aim constraint whose source is a descendant");
            continue;
        }
        parsed.source = *source;
        parsed.weight = std::clamp(floatField(body, "weight", 1.0f), 0.0f, 1.0f);
        model.nodeConstraints.push_back(parsed);
    }
    model.diagnostics.nodeConstraintCount = static_cast<int>(model.nodeConstraints.size());
}

void readMToonV1(const JsonValue* root, VrmModel& model) {
    const JsonValue* materials = objectField(root, "materials");
    if (!materials || !materials->isArray()) {
        return;
    }
    for (std::size_t i = 0; i < materials->items.size() && i < model.materials.size(); ++i) {
        const JsonValue* mtoon = objectField(
            objectField(&materials->items[i], "extensions"), "VRMC_materials_mtoon");
        if (!mtoon) {
            continue;
        }
        Material& material = model.materials[i];
        material.shader = "VRMC_materials_mtoon";
        material.toon = true;
        const Vec4 shade = colorField(mtoon, "shadeColorFactor", {1.0f, 1.0f, 1.0f, 1.0f});
        material.shadeColor = {shade.x, shade.y, shade.z, 1.0f};
    }
}

void readExpressionsV1(const JsonValue* vrmc, const std::vector<int>& morphTargetCounts,
                       VrmModel& model) {
    const JsonValue* expressions = objectField(vrmc, "expressions");
    if (!expressions || !expressions->isObject()) {
        return;
    }
    const auto readOne = [&](const std::string& name, const JsonValue& body,
                             ExpressionPreset preset) {
        Expression out;
        out.name = name;
        out.preset = preset;
        out.isBinary = boolField(&body, "isBinary").value_or(false);
        out.overrideBlink = overrideField(&body, "overrideBlink");
        out.overrideLookAt = overrideField(&body, "overrideLookAt");
        out.overrideMouth = overrideField(&body, "overrideMouth");
        if (const JsonValue* binds = objectField(&body, "morphTargetBinds");
            binds && binds->isArray()) {
            for (const JsonValue& bind : binds->items) {
                const auto node = indexField(&bind, "node", model.nodes.size());
                const int mesh = node ? model.nodes[static_cast<std::size_t>(*node)].mesh : -1;
                const double index = numberField(&bind, "index", -1.0);
                if (mesh < 0 || static_cast<std::size_t>(mesh) >= morphTargetCounts.size() ||
                    !(index >= 0.0) || std::floor(index) != index ||
                    index >= morphTargetCounts[static_cast<std::size_t>(mesh)]) {
                    model.warnings.push_back("expression '" + name +
                                             "': ignored invalid morph target bind");
                    continue;
                }
                out.morphTargetBinds.push_back(
                    {mesh, *node, static_cast<int>(index),
                     std::clamp(floatField(&bind, "weight", 0.0f), 0.0f, 1.0f)});
            }
        }
        if (const JsonValue* binds = objectField(&body, "materialColorBinds");
            binds && binds->isArray()) {
            for (const JsonValue& bind : binds->items) {
                const auto material = indexField(&bind, "material", model.materials.size());
                const auto type = materialColorTypeV1(stringField(&bind, "type"));
                if (!material || !type) {
                    model.warnings.push_back("expression '" + name +
                                             "': ignored invalid material color bind");
                    continue;
                }
                out.materialColorBinds.push_back(
                    {*material, *type, colorField(&bind, "targetValue", {1, 1, 1, 1})});
            }
        }
        if (const JsonValue* binds = objectField(&body, "textureTransformBinds");
            binds && binds->isArray()) {
            for (const JsonValue& bind : binds->items) {
                const auto material = indexField(&bind, "material", model.materials.size());
                if (!material) {
                    continue;
                }
                out.textureTransformBinds.push_back(
                    {*material, vec2Field(&bind, "scale", {1.0f, 1.0f}),
                     vec2Field(&bind, "offset", {0.0f, 0.0f})});
            }
        }
        model.expressions.push_back(std::move(out));
    };
    if (const JsonValue* preset = objectField(expressions, "preset"); preset && preset->isObject()) {
        for (const auto& [name, body] : preset->fields) {
            const auto kind = expressionPresetFromVrm1Name(name);
            if (!kind || !body.isObject()) {
                model.warnings.push_back("expressions: ignored unknown preset '" + name + "'");
                continue;
            }
            readOne(name, body, *kind);
        }
    }
    if (const JsonValue* custom = objectField(expressions, "custom"); custom && custom->isObject()) {
        for (const auto& [name, body] : custom->fields) {
            if (!body.isObject() || expressionPresetFromVrm1Name(name)) {
                // The spec forbids a custom expression named like a preset.
                model.warnings.push_back("expressions: ignored custom expression '" + name + "'");
                continue;
            }
            readOne(name, body, ExpressionPreset::Custom);
        }
    }
    model.diagnostics.expressionCount = static_cast<int>(model.expressions.size());
    model.diagnostics.blendShapeCount = model.diagnostics.expressionCount;
}

void readExpressionsV0(const JsonValue* vrm, const std::vector<int>& morphTargetCounts,
                       VrmModel& model) {
    const JsonValue* groups =
        objectField(objectField(vrm, "blendShapeMaster"), "blendShapeGroups");
    if (!groups || !groups->isArray()) {
        return;
    }
    std::unordered_map<std::string, int> materialByName;
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        materialByName.emplace(model.materials[i].name, static_cast<int>(i));
    }
    for (const JsonValue& group : groups->items) {
        Expression out;
        out.name = stringField(&group, "name");
        out.preset = expressionPresetFromVrm0Name(stringField(&group, "presetName"))
                         .value_or(ExpressionPreset::Custom);
        if (out.name.empty()) {
            out.name = std::string(expressionPresetName(out.preset));
        }
        out.isBinary = boolField(&group, "isBinary").value_or(false);
        if (const JsonValue* binds = objectField(&group, "binds"); binds && binds->isArray()) {
            for (const JsonValue& bind : binds->items) {
                const auto mesh = indexField(&bind, "mesh", morphTargetCounts.size());
                const double index = numberField(&bind, "index", -1.0);
                if (!mesh || !(index >= 0.0) || std::floor(index) != index ||
                    index >= morphTargetCounts[static_cast<std::size_t>(*mesh)]) {
                    model.warnings.push_back("blend shape '" + out.name +
                                             "': ignored invalid bind");
                    continue;
                }
                // 0.x weights are percentages.
                out.morphTargetBinds.push_back(
                    {*mesh, -1, static_cast<int>(index),
                     std::clamp(floatField(&bind, "weight", 0.0f) / 100.0f, 0.0f, 1.0f)});
            }
        }
        if (const JsonValue* values = objectField(&group, "materialValues");
            values && values->isArray()) {
            for (const JsonValue& value : values->items) {
                const auto found = materialByName.find(stringField(&value, "materialName"));
                if (found == materialByName.end()) {
                    continue;
                }
                const std::string property = stringField(&value, "propertyName");
                const Vec4 target = colorField(&value, "targetValue", {1, 1, 1, 1});
                if (const auto type = materialColorTypeV0(property)) {
                    out.materialColorBinds.push_back({found->second, *type, target});
                } else if (property == "_MainTex_ST") {
                    // Unity ST: xy = scale, zw = offset, UV origin bottom-left.
                    // glTF's origin is top-left, which moves the V offset.
                    out.textureTransformBinds.push_back(
                        {found->second, {target.x, target.y},
                         {target.z, 1.0f - target.y - target.w}});
                }
            }
        }
        model.expressions.push_back(std::move(out));
    }
    model.diagnostics.expressionCount = static_cast<int>(model.expressions.size());
}

void readFirstPersonV1(const JsonValue* vrmc, VrmModel& model) {
    model.firstPersonBone = model.nodeFor(HumanoidBone::Head);
    if (const JsonValue* lookAt = objectField(vrmc, "lookAt"); lookAt && lookAt->isObject()) {
        LookAt& out = model.lookAt;
        out.authored = true;
        out.type = stringField(lookAt, "type") == "expression" ? LookAtType::Expression
                                                               : LookAtType::Bone;
        const float scale = out.type == LookAtType::Bone ? 10.0f : 1.0f;
        // The offset is in the head bone's local space. The head's bind global
        // rotation carries both its authored rest orientation and the scene
        // turn, so it lands the offset in the engine's model axes.
        const int head = model.nodeFor(HumanoidBone::Head);
        const Quat headRest =
            head >= 0 && static_cast<std::size_t>(head) < model.bindGlobalRotations.size()
                ? model.bindGlobalRotations[static_cast<std::size_t>(head)]
                : model.sourceToModel;
        const Vec3 offset = vec3Field(lookAt, "offsetFromHeadBone", {0.0f, 0.06f, 0.0f});
        out.offsetFromHeadBone = math::rotate(
            headRest, math::finite(offset) ? offset : Vec3{0.0f, 0.06f, 0.0f});
        out.horizontalInner = rangeMapV1(objectField(lookAt, "rangeMapHorizontalInner"), scale);
        out.horizontalOuter = rangeMapV1(objectField(lookAt, "rangeMapHorizontalOuter"), scale);
        out.verticalDown = rangeMapV1(objectField(lookAt, "rangeMapVerticalDown"), scale);
        out.verticalUp = rangeMapV1(objectField(lookAt, "rangeMapVerticalUp"), scale);
    }
    const JsonValue* annotations =
        objectField(objectField(vrmc, "firstPerson"), "meshAnnotations");
    if (!annotations || !annotations->isArray()) {
        return;
    }
    for (const JsonValue& annotation : annotations->items) {
        const auto node = indexField(&annotation, "node", model.nodes.size());
        if (!node) {
            continue;
        }
        const std::string type = stringField(&annotation, "type");
        FirstPersonType parsed = FirstPersonType::Auto;
        if (type == "both") {
            parsed = FirstPersonType::Both;
        } else if (type == "thirdPersonOnly") {
            parsed = FirstPersonType::ThirdPersonOnly;
        } else if (type == "firstPersonOnly") {
            parsed = FirstPersonType::FirstPersonOnly;
        }
        model.firstPersonAnnotations.push_back({*node, parsed});
    }
}

void readFirstPersonV0(const JsonValue* vrm, VrmModel& model) {
    const JsonValue* firstPerson = objectField(vrm, "firstPerson");
    model.firstPersonBone = model.nodeFor(HumanoidBone::Head);
    if (!firstPerson || !firstPerson->isObject()) {
        return;
    }
    if (const auto bone = indexField(firstPerson, "firstPersonBone", model.nodes.size())) {
        model.firstPersonBone = *bone;
    }
    LookAt& out = model.lookAt;
    out.authored = true;
    out.type = stringField(firstPerson, "lookAtTypeName") == "BlendShape" ? LookAtType::Expression
                                                                          : LookAtType::Bone;
    const float scale = out.type == LookAtType::Bone ? 10.0f : 1.0f;
    // UniVRM 0.x writes this offset in Unity's left-handed axes; glTF's Z is
    // the opposite (three-vrm reads it the same way).
    const Vec3 offset = vec3Field(firstPerson, "firstPersonBoneOffset", {0.0f, 0.06f, 0.0f});
    out.offsetFromHeadBone =
        math::finite(offset) ? Vec3{offset.x, offset.y, -offset.z} : Vec3{0.0f, 0.06f, 0.0f};
    out.horizontalInner = rangeMapV0(objectField(firstPerson, "lookAtHorizontalInner"), scale);
    out.horizontalOuter = rangeMapV0(objectField(firstPerson, "lookAtHorizontalOuter"), scale);
    out.verticalDown = rangeMapV0(objectField(firstPerson, "lookAtVerticalDown"), scale);
    out.verticalUp = rangeMapV0(objectField(firstPerson, "lookAtVerticalUp"), scale);

    const JsonValue* annotations = objectField(firstPerson, "meshAnnotations");
    if (!annotations || !annotations->isArray()) {
        return;
    }
    for (const JsonValue& annotation : annotations->items) {
        const auto mesh = indexField(&annotation, "mesh", model.meshes.size());
        if (!mesh) {
            continue;
        }
        const std::string flag = stringField(&annotation, "firstPersonFlag");
        FirstPersonType parsed = FirstPersonType::Auto;
        if (flag == "Both") {
            parsed = FirstPersonType::Both;
        } else if (flag == "ThirdPersonOnly") {
            parsed = FirstPersonType::ThirdPersonOnly;
        } else if (flag == "FirstPersonOnly") {
            parsed = FirstPersonType::FirstPersonOnly;
        }
        for (const MeshInstance& instance : model.meshInstances) {
            if (instance.mesh == *mesh) {
                model.firstPersonAnnotations.push_back({instance.node, parsed});
            }
        }
    }
}

void finishSpringDiagnostics(VrmModel& model) {
    model.diagnostics.springGroupCount = static_cast<int>(model.springGroups.size());
    model.diagnostics.springChainCount = static_cast<int>(model.springChains.size());
    model.diagnostics.springBoneCount = model.springBoneCount();
    model.diagnostics.springColliderGroupCount =
        static_cast<int>(model.springColliderGroups.size());
    int colliderCount = 0;
    for (const SpringColliderGroup& group : model.springColliderGroups) {
        colliderCount += static_cast<int>(group.colliders.size());
    }
    model.diagnostics.springColliderCount = colliderCount;
}

} // namespace bgmedia::vrm::schema
