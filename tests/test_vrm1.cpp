// VRM 1.0 adapter tests. A VRM 1.0 twin of tests/fixtures/minimal.vrm is
// written here as JSON around the fixture's own binary chunk: the same rig,
// turned to face +Z as the 1.0 spec requires, optionally with the non-identity
// rest rotations 1.0 allows. The loader must hand animation exactly the model
// the 0.x fixture produces, so every clip lands the bones on the same spots.
#include "framework.hpp"

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/animation/NodeConstraints.hpp"
#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace animation = bgmedia::animation;
namespace motion = bgmedia::motion;
namespace vrm = bgmedia::vrm;
namespace math = bgmedia::math;
using math::Quat;
using math::Vec3;
using vrm::HumanoidBone;

std::filesystem::path fixturePath() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

std::vector<std::uint8_t> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put32(std::vector<std::uint8_t>& v, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        v.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

// GLB = header + JSON chunk + the fixture's BIN chunk (one triangle).
std::vector<std::uint8_t> makeGlb(std::string json) {
    const std::vector<std::uint8_t> fixture = readAll(fixturePath());
    const std::uint32_t jsonLength = le32(fixture.data() + 12);
    const std::size_t binAt = 20 + jsonLength;
    while (json.size() % 4 != 0) {
        json.push_back(' ');
    }
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    put32(out, 2);
    put32(out, 0);  // patched below
    put32(out, static_cast<std::uint32_t>(json.size()));
    out.insert(out.end(), {'J', 'S', 'O', 'N'});
    out.insert(out.end(), json.begin(), json.end());
    out.insert(out.end(), fixture.begin() + static_cast<std::ptrdiff_t>(binAt), fixture.end());
    const auto size = static_cast<std::uint32_t>(out.size());
    for (int i = 0; i < 4; ++i) {
        out[8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
    }
    return out;
}

std::string num(float value) {
    std::ostringstream out;
    out << std::setprecision(9) << value;
    return out.str();
}

std::string vec(Vec3 v) {
    return "[" + num(v.x) + "," + num(v.y) + "," + num(v.z) + "]";
}

std::string quat(Quat q) {
    return "[" + num(q.x) + "," + num(q.y) + "," + num(q.z) + "," + num(q.w) + "]";
}

struct V1Options {
    // Give every node its own rest rotation, as a Blender export does (VRM 1.0
    // does not require identity rests). Positions stay those of the 0.x rig.
    bool restRotations = false;
    std::string extraExtensions;    // more top-level extensions, ",..." form
    std::string vrmcExtra;          // more VRMC_vrm members, ",..." form
    std::string materialExtensions; // materials[0].extensions body
    std::string constraints[3];     // VRMC_node_constraint bodies for nodes 25..27
    bool morphTarget = false;
};

// Node layout = minimal.vrm's 22 nodes (same indices) facing +Z, then
// 22..24 left thumb, 25 twist (under the left upper arm), 26 aimer (under the
// head), 27 copy (under the chest).
std::string vrm1Json(const V1Options& o) {
    struct N {
        const char* name;
        int parent;
        Vec3 t;  // offset from the parent with identity rests
        int constraint = -1;
    };
    const N nodes[] = {
        {"Hips", -1, {0, 0.9f, 0}},
        {"Spine", 0, {0, 0.1f, 0}},
        {"Chest", 1, {0, 0.12f, 0}},
        {"Neck", 2, {0, 0.15f, 0}},
        {"Head", 3, {0, 0.12f, 0}},
        {"LeftUpperArm", 2, {0.08f, 0.12f, 0}},
        {"LeftLowerArm", 5, {0.22f, 0, 0}},
        {"LeftHand", 6, {0.22f, 0, 0}},
        {"RightUpperArm", 2, {-0.08f, 0.12f, 0}},
        {"RightLowerArm", 8, {-0.22f, 0, 0}},
        {"RightHand", 9, {-0.22f, 0, 0}},
        {"LeftUpperLeg", 0, {0.08f, -0.05f, 0}},
        {"LeftLowerLeg", 11, {0, -0.4f, 0}},
        {"LeftFoot", 12, {0, -0.4f, 0}},
        {"LeftToes", 13, {0, -0.02f, 0.1f}},
        {"RightUpperLeg", 0, {-0.08f, -0.05f, 0}},
        {"RightLowerLeg", 15, {0, -0.4f, 0}},
        {"RightFoot", 16, {0, -0.4f, 0}},
        {"RightToes", 17, {0, -0.02f, 0.1f}},
        {"LeftEye", 4, {0.03f, 0.06f, 0.05f}},
        {"RightEye", 4, {-0.03f, 0.06f, 0.05f}},
        {"Body", -1, {0, 0, 0}},
        {"LeftThumb1", 7, {0.03f, 0, 0.02f}},
        {"LeftThumb2", 22, {0.03f, 0, 0}},
        {"LeftThumb3", 23, {0.02f, 0, 0}},
        {"Twist", 5, {0.11f, 0, 0}, 0},
        {"Aimer", 4, {0, 0.05f, 0.1f}, 1},
        {"Copy", 2, {0, 0.05f, 0}, 2},
    };
    const std::size_t kNodes = std::size(nodes);
    // Global rest rotation per node, then locals that keep the 0.x positions.
    std::vector<Quat> rest(kNodes);
    for (std::size_t i = 0; i < kNodes; ++i) {
        const float k = static_cast<float>(i);
        rest[i] = o.restRotations && i != 21
                      ? math::fromAxisAngle({std::sin(k), std::cos(1.3f * k), 0.5f}, 0.4f + 0.13f * k)
                      : Quat::identity();
    }
    std::vector<std::string> children(kNodes);
    for (std::size_t i = 0; i < kNodes; ++i) {
        if (nodes[i].parent >= 0) {
            std::string& list = children[static_cast<std::size_t>(nodes[i].parent)];
            list += (list.empty() ? "" : ",") + std::to_string(i);
        }
    }
    std::string json = R"({"asset":{"version":"2.0","generator":"BG Media VRM 1.0 test"},)"
                       R"("scene":0,"scenes":[{"nodes":[0,21]}],"nodes":[)";
    for (std::size_t i = 0; i < kNodes; ++i) {
        const N& n = nodes[i];
        json += i ? "," : "";
        json += std::string(R"({"name":")") + n.name + "\"";
        if (std::string(n.name) == "Body") {
            json += R"(,"mesh":0})";
            continue;
        }
        const Quat parentRest =
            n.parent >= 0 ? rest[static_cast<std::size_t>(n.parent)] : Quat::identity();
        json += ",\"translation\":" + vec(math::rotate(parentRest.conjugate(), n.t)) +
                ",\"rotation\":" + quat(parentRest.conjugate() * rest[i]);
        if (!children[i].empty()) {
            json += ",\"children\":[" + children[i] + "]";
        }
        if (n.constraint >= 0 && !o.constraints[n.constraint].empty()) {
            json += R"(,"extensions":{"VRMC_node_constraint":{"specVersion":"1.0","constraint":{)" +
                    o.constraints[n.constraint] + "}}}";
        }
        json += "}";
    }
    json += R"(],"meshes":[{"name":"Body","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0)";
    if (o.morphTarget) {
        json += R"(,"targets":[{"POSITION":1}])";
    }
    json += R"(}]}],"materials":[{"name":"Body","doubleSided":true,"alphaMode":"MASK","alphaCutoff":0.5,)"
            R"("pbrMetallicRoughness":{"baseColorFactor":[0.72,0.78,0.86,1.0],"metallicFactor":0.0,"roughnessFactor":0.9})";
    if (!o.materialExtensions.empty()) {
        json += ",\"extensions\":{" + o.materialExtensions + "}";
    }
    json += R"(}],"buffers":[{"byteLength":104}],"bufferViews":[)"
            R"({"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},)"
            R"({"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},)"
            R"({"buffer":0,"byteOffset":72,"byteLength":24,"target":34962},)"
            R"({"buffer":0,"byteOffset":96,"byteLength":6,"target":34963}],"accessors":[)"
            R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.2,0.0,0.0],"max":[0.2,1.7,0.0]},)"
            R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,-1],"max":[1,1,1]},)"
            R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},)"
            R"({"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],)"
            R"("extensionsUsed":["VRMC_vrm"],"extensions":{"VRMC_vrm":{"specVersion":"1.0",)"
            R"("meta":{"name":"BG Media Minimal Rig 1.0","version":"2","authors":["BG Media tests","Second"],)"
            R"("licenseUrl":"https://vrm.dev/licenses/1.0/","avatarPermission":"onlyAuthor",)"
            R"("commercialUsage":"personalProfit","allowExcessivelyViolentUsage":true,)"
            R"("allowRedistribution":false,"thumbnailImage":-1},)"
            R"("humanoid":{"humanBones":{)";
    const char* bones[] = {"hips", "spine", "chest", "neck", "head", "leftUpperArm",
                           "leftLowerArm", "leftHand", "rightUpperArm", "rightLowerArm",
                           "rightHand", "leftUpperLeg", "leftLowerLeg", "leftFoot", "leftToes",
                           "rightUpperLeg", "rightLowerLeg", "rightFoot", "rightToes",
                           "leftEye", "rightEye"};
    for (std::size_t i = 0; i < std::size(bones); ++i) {
        json += std::string(i ? "," : "") + "\"" + bones[i] + "\":{\"node\":" + std::to_string(i) + "}";
    }
    json += R"(,"leftThumbMetacarpal":{"node":22},"leftThumbProximal":{"node":23},"leftThumbDistal":{"node":24}}})";
    json += o.vrmcExtra + "}" + o.extraExtensions + "}}";
    return json;
}

std::unique_ptr<vrm::VrmModel> loadV1(const V1Options& options = {},
                                      std::vector<std::string>* warnings = nullptr) {
    const auto bytes = makeGlb(vrm1Json(options));
    auto result = vrm::VrmLoader::loadFromMemory(
        std::span<const std::uint8_t>(bytes.data(), bytes.size()), "synthetic-1.0.vrm");
    if (!result.ok()) {
        std::cout << "\n            " << result.error;
    }
    if (warnings && result.model) {
        *warnings = result.model->warnings;
    }
    return std::move(result.model);
}

std::unique_ptr<vrm::VrmModel> loadV0() {
    auto result = vrm::VrmLoader::load(fixturePath());
    return std::move(result.model);
}

// Inserts `members` (",..." form not needed: "a":..,"b":..) at the start of
// minimal.vrm's extensions.VRM object.
std::unique_ptr<vrm::VrmModel> loadV0With(const std::string& members) {
    const std::vector<std::uint8_t> bytes = readAll(fixturePath());
    const std::uint32_t jsonLength = le32(bytes.data() + 12);
    std::string json(reinterpret_cast<const char*>(bytes.data() + 20), jsonLength);
    const std::string marker = "\"VRM\":{";
    json.insert(json.find(marker) + marker.size(), members + ",");
    const auto glb = makeGlb(json);
    auto result = vrm::VrmLoader::loadFromMemory(
        std::span<const std::uint8_t>(glb.data(), glb.size()), "synthetic-0.x.vrm");
    return std::move(result.model);
}

float maxBoneGap(const vrm::VrmModel& a, const animation::RetargetedPose& pa,
                 const vrm::VrmModel& b, const animation::RetargetedPose& pb) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        const int na = a.humanoidNodes[i];
        const int nb = b.humanoidNodes[i];
        if (na < 0 || nb < 0) {
            continue;
        }
        const Vec3 qa = animation::HumanoidRetargeter::stagePosition(pa, na);
        const Vec3 qb = animation::HumanoidRetargeter::stagePosition(pb, nb);
        worst = std::max(worst, (qa - qb).length());
    }
    return worst;
}

}  // namespace

TEST_CASE(vrm1.detects_version_and_faces_minus_z) {
    auto v0 = loadV0();
    auto v1 = loadV1();
    REQUIRE(v0 != nullptr);
    REQUIRE(v1 != nullptr);
    CHECK(v0->version == vrm::VrmVersion::V0);
    CHECK(v1->version == vrm::VrmVersion::V1);
    CHECK(v1->specVersion == "1.0");
    CHECK(v1->exporterVersion == "BG Media VRM 1.0 test");
    CHECK(std::abs(v0->sourceToModel.w - 1.0f) < 1e-6f);
    CHECK(std::abs(v1->sourceToModel.y - 1.0f) < 1e-6f);
    // The 1.0 file faces +Z; after the adapter it faces -Z like 0.x.
    CHECK(v1->forward.z < -0.99f);
    CHECK(v1->right.x > 0.99f);
    CHECK(v1->completeHumanoid());
    CHECK_NEAR(v1->height, v0->height, 1e-4);
    // Same rig, same bind positions, node for node.
    for (std::size_t i = 0; i < v0->nodes.size(); ++i) {
        const Vec3 p0 = v0->bindGlobals[i].translationPart();
        const Vec3 p1 = v1->bindGlobals[i].translationPart();
        CHECK((p0 - p1).length() < 1e-5f);
    }
    // The character's left hand is on -X in both.
    CHECK(v1->boneBindPosition(HumanoidBone::LeftHand).x < -0.4f);
    CHECK(v1->bounds.valid);
}

TEST_CASE(vrm1.thumb_names_map_to_vrm0_chain) {
    auto v1 = loadV1();
    REQUIRE(v1 != nullptr);
    CHECK(v1->fingerNode(0, static_cast<int>(vrm::Finger::Thumb), 0) == 22);
    CHECK(v1->fingerNode(0, static_cast<int>(vrm::Finger::Thumb), 1) == 23);
    CHECK(v1->fingerNode(0, static_cast<int>(vrm::Finger::Thumb), 2) == 24);
    bool sawMetacarpal = false;
    for (const vrm::HumanoidEntry& entry : v1->humanoidEntries) {
        if (entry.sourceName == "leftThumbMetacarpal") {
            sawMetacarpal = true;
            CHECK(entry.name == "leftThumbProximal");
            CHECK(entry.node == 22);
        }
    }
    CHECK(sawMetacarpal);
}

TEST_CASE(vrm1.clips_land_bones_where_vrm0_does) {
    auto v0 = loadV0();
    REQUIRE(v0 != nullptr);
    V1Options rotated;
    rotated.restRotations = true;
    auto plain = loadV1();
    auto withRests = loadV1(rotated);
    REQUIRE(plain != nullptr);
    REQUIRE(withRests != nullptr);
    animation::HumanoidRetargeter r0(*v0);
    animation::HumanoidRetargeter r1(*plain);
    animation::HumanoidRetargeter r2(*withRests);
    REQUIRE(r0.valid());
    REQUIRE(r1.valid());
    REQUIRE(r2.valid());

    motion::DanceLibrary library;
    library.loadDirectory(std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances");
    REQUIRE(!library.clips().empty());
    float worstPlain = 0.0f;
    float worstRests = 0.0f;
    float worstThumb = 0.0f;  // the 0.x fixture has no fingers: 1.0 vs 1.0
    int samples = 0;
    for (const motion::HumanoidClip& clip : library.clips()) {
        for (int k = 0; k < 6; ++k) {
            const double beat = clip.beats * (k + 0.37) / 6.0;
            const motion::ClipPose pose = motion::sampleClip(clip, beat);
            const auto p0 = r0.applyClip(pose, beat);
            const auto p1 = r1.applyClip(pose, beat);
            const auto p2 = r2.applyClip(pose, beat);
            worstPlain = std::max(worstPlain, maxBoneGap(*v0, p0, *plain, p1));
            worstRests = std::max(worstRests, maxBoneGap(*v0, p0, *withRests, p2));
            for (int node = 22; node <= 24; ++node) {
                worstThumb = std::max(
                    worstThumb, (animation::HumanoidRetargeter::stagePosition(p1, node) -
                                 animation::HumanoidRetargeter::stagePosition(p2, node))
                                    .length());
            }
            ++samples;
        }
    }
    // Legacy CharacterPose path too.
    for (int k = 0; k < 8; ++k) {
        bgmedia::CharacterPose pose;
        pose.armLeft = 0.6f * std::sin(k * 0.9f);
        pose.armRight = -0.4f * std::cos(k * 1.3f);
        pose.lean = 0.3f * std::sin(k * 0.5f);
        pose.headNod = 0.4f;
        pose.rotation = 0.1f * k;
        const double t = k * 0.25;
        worstPlain = std::max(worstPlain, maxBoneGap(*v0, r0.apply(pose, t), *plain, r1.apply(pose, t)));
        worstRests =
            std::max(worstRests, maxBoneGap(*v0, r0.apply(pose, t), *withRests, r2.apply(pose, t)));
    }
    std::cout << "\n            " << samples << " clip samples: max bone gap 1.0="
              << worstPlain * 1000.0f << " mm, 1.0 with rest rotations="
              << worstRests * 1000.0f << " mm, thumb=" << worstThumb * 1000.0f << " mm";
    CHECK(worstPlain < 1.0e-3f);
    CHECK(worstRests < 1.0e-3f);
    CHECK(worstThumb < 1.0e-3f);
}

TEST_CASE(vrm1.meta_maps_onto_vrm0_fields) {
    auto v1 = loadV1();
    REQUIRE(v1 != nullptr);
    const vrm::Metadata& meta = v1->metadata;
    CHECK(meta.title == "BG Media Minimal Rig 1.0");
    CHECK(v1->name == "BG Media Minimal Rig 1.0");
    CHECK(meta.version == "2");
    CHECK(meta.authors.size() == 2);
    CHECK(meta.author == "BG Media tests, Second");
    CHECK(meta.licenseUrl == "https://vrm.dev/licenses/1.0/");
    CHECK(meta.allowedUserName == "OnlyAuthor");
    CHECK(meta.commercialUsage == "Allow");
    CHECK(meta.commercialUsageV1 == "personalProfit");
    CHECK(meta.violentUsage == "Allow");
    CHECK(meta.sexualUsage == "Disallow");
    REQUIRE(meta.allowRedistribution.has_value());
    CHECK(!*meta.allowRedistribution);
    CHECK(meta.restrictsRedistribution());
    // The 0.x fixture is CC0 / Everyone: unrestricted, as before.
    auto v0 = loadV0();
    REQUIRE(v0 != nullptr);
    CHECK(!v0->metadata.restrictsRedistribution());
    CHECK(v0->metadata.authors.size() == 1);
}

TEST_CASE(vrm1.mtoon_material) {
    V1Options options;
    options.materialExtensions =
        R"("VRMC_materials_mtoon":{"specVersion":"1.0","shadeColorFactor":[0.5,0.4,0.3]})";
    auto v1 = loadV1(options);
    REQUIRE(v1 != nullptr);
    REQUIRE(v1->materials.size() == 1);
    CHECK(v1->materials[0].toon);
    CHECK(v1->materials[0].shader == "VRMC_materials_mtoon");
    CHECK_NEAR(v1->materials[0].shadeColor.x, 0.5, 1e-6);
    CHECK_NEAR(v1->materials[0].shadeColor.z, 0.3, 1e-6);
    CHECK(v1->materials[0].doubleSided);
    CHECK(v1->materials[0].alphaMode == vrm::AlphaMode::Mask);
    // Without the extension a 1.0 material is plain glTF.
    auto plain = loadV1();
    REQUIRE(plain != nullptr);
    CHECK(!plain->materials[0].toon);
}

TEST_CASE(vrm1.expressions_lookat_first_person) {
    V1Options options;
    options.morphTarget = true;
    options.vrmcExtra =
        R"(,"expressions":{"preset":{)"
        R"("happy":{"morphTargetBinds":[{"node":21,"index":0,"weight":0.75},{"node":21,"index":5,"weight":1}],)"
        R"("materialColorBinds":[{"material":0,"type":"shadeColor","targetValue":[1,0,0,1]}],)"
        R"("isBinary":true,"overrideBlink":"block","overrideMouth":"blend"},)"
        R"("blinkLeft":{"morphTargetBinds":[{"node":21,"index":0,"weight":1}]},)"
        R"("bogus":{}},)"
        R"("custom":{"smirk":{"textureTransformBinds":[{"material":0,"scale":[2,1],"offset":[0.5,0]}]}}},)"
        R"("lookAt":{"offsetFromHeadBone":[0.01,0.06,0.02],"type":"expression",)"
        R"("rangeMapHorizontalOuter":{"inputMaxValue":45,"outputScale":0.8}},)"
        R"("firstPerson":{"meshAnnotations":[{"node":21,"type":"thirdPersonOnly"}]})";
    std::vector<std::string> warnings;
    auto v1 = loadV1(options, &warnings);
    REQUIRE(v1 != nullptr);
    REQUIRE(v1->expressions.size() == 3);
    const vrm::Expression* happy = v1->findExpression(vrm::ExpressionPreset::Happy);
    REQUIRE(happy != nullptr);
    CHECK(happy->name == "happy");
    CHECK(happy->isBinary);
    CHECK(happy->overrideBlink == vrm::ExpressionOverride::Block);
    CHECK(happy->overrideMouth == vrm::ExpressionOverride::Blend);
    CHECK(happy->overrideLookAt == vrm::ExpressionOverride::None);
    // Index 5 is out of range for the mesh's single morph target.
    REQUIRE(happy->morphTargetBinds.size() == 1);
    CHECK(happy->morphTargetBinds[0].mesh == 0);
    CHECK(happy->morphTargetBinds[0].node == 21);
    CHECK_NEAR(happy->morphTargetBinds[0].weight, 0.75, 1e-6);
    REQUIRE(happy->materialColorBinds.size() == 1);
    CHECK(happy->materialColorBinds[0].type == vrm::MaterialColorType::ShadeColor);
    CHECK(v1->findExpression(vrm::ExpressionPreset::BlinkLeft) != nullptr);
    const vrm::Expression* smirk = v1->findExpression("smirk");
    REQUIRE(smirk != nullptr);
    CHECK(smirk->preset == vrm::ExpressionPreset::Custom);
    REQUIRE(smirk->textureTransformBinds.size() == 1);
    CHECK_NEAR(smirk->textureTransformBinds[0].scale.x, 2.0, 1e-6);
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
        return w.find("bogus") != std::string::npos;
    }));

    CHECK(v1->lookAt.authored);
    CHECK(v1->lookAt.type == vrm::LookAtType::Expression);
    // Offset turned into the engine's -Z-forward frame with the scene.
    CHECK_NEAR(v1->lookAt.offsetFromHeadBone.x, -0.01, 1e-6);
    CHECK_NEAR(v1->lookAt.offsetFromHeadBone.y, 0.06, 1e-6);
    CHECK_NEAR(v1->lookAt.offsetFromHeadBone.z, -0.02, 1e-6);
    CHECK_NEAR(v1->lookAt.horizontalOuter.inputMaxValue, 45.0, 1e-6);
    CHECK_NEAR(v1->lookAt.horizontalOuter.outputScale, 0.8, 1e-6);
    CHECK_NEAR(v1->lookAt.verticalUp.outputScale, 1.0, 1e-6);  // expression default
    CHECK(v1->firstPersonBone == v1->nodeFor(HumanoidBone::Head));
    REQUIRE(v1->firstPersonAnnotations.size() == 1);
    CHECK(v1->firstPersonAnnotations[0].type == vrm::FirstPersonType::ThirdPersonOnly);
}

TEST_CASE(vrm1.vrm0_blend_shapes_and_first_person_use_the_same_model) {
    auto v0 = loadV0With(
        R"("blendShapeMaster":{"blendShapeGroups":[)"
        R"({"name":"Joy","presetName":"joy","binds":[],"materialValues":[)"
        R"({"materialName":"Body","propertyName":"_MainTex_ST","targetValue":[0.5,0.5,0.25,0.125]},)"
        R"({"materialName":"Body","propertyName":"_Color","targetValue":[1,0,0,1]}]},)"
        R"({"name":"Wink","presetName":"Blink_L","binds":[{"mesh":0,"index":0,"weight":100}]},)"
        R"({"name":"Mine","presetName":"unknown"}]},)"
        R"("firstPerson":{"firstPersonBone":4,"firstPersonBoneOffset":{"x":0.01,"y":0.07,"z":0.02},)"
        R"("meshAnnotations":[{"mesh":0,"firstPersonFlag":"FirstPersonOnly"}],"lookAtTypeName":"Bone",)"
        R"("lookAtHorizontalInner":{"curve":[0,0,0,1,1,1,1,0],"xRange":60,"yRange":8}})");
    REQUIRE(v0 != nullptr);
    REQUIRE(v0->expressions.size() == 3);
    const vrm::Expression* joy = v0->findExpression(vrm::ExpressionPreset::Happy);
    REQUIRE(joy != nullptr);
    CHECK(joy->name == "Joy");
    REQUIRE(joy->textureTransformBinds.size() == 1);
    // Unity bottom-left ST -> glTF top-left offset.
    CHECK_NEAR(joy->textureTransformBinds[0].offset.x, 0.25, 1e-6);
    CHECK_NEAR(joy->textureTransformBinds[0].offset.y, 1.0 - 0.5 - 0.125, 1e-6);
    REQUIRE(joy->materialColorBinds.size() == 1);
    CHECK(joy->materialColorBinds[0].type == vrm::MaterialColorType::Color);
    // The fixture mesh has no morph targets, so the wink's bind is dropped.
    const vrm::Expression* wink = v0->findExpression(vrm::ExpressionPreset::BlinkLeft);
    REQUIRE(wink != nullptr);
    CHECK(wink->morphTargetBinds.empty());
    CHECK(v0->findExpression("Mine")->preset == vrm::ExpressionPreset::Custom);
    CHECK(v0->diagnostics.blendShapeCount == 3);

    CHECK(v0->lookAt.authored);
    CHECK(v0->lookAt.type == vrm::LookAtType::Bone);
    CHECK_NEAR(v0->lookAt.offsetFromHeadBone.z, -0.02, 1e-6);
    CHECK_NEAR(v0->lookAt.horizontalInner.inputMaxValue, 60.0, 1e-6);
    CHECK_NEAR(v0->lookAt.horizontalInner.outputScale, 8.0, 1e-6);
    CHECK(v0->firstPersonBone == 4);
    REQUIRE(v0->firstPersonAnnotations.size() == 1);
    CHECK(v0->firstPersonAnnotations[0].node == 21);
    CHECK(v0->firstPersonAnnotations[0].type == vrm::FirstPersonType::FirstPersonOnly);
}

TEST_CASE(vrm1.spring_bone_extension) {
    V1Options options;
    options.extraExtensions =
        R"(,"VRMC_springBone":{"specVersion":"1.0",)"
        R"("colliders":[{"node":2,"shape":{"sphere":{"offset":[0,0.1,0],"radius":0.12}}},)"
        R"({"node":999,"shape":{"sphere":{"offset":[0,0,0],"radius":0.1}}},)"
        R"({"node":3,"shape":{"capsule":{"offset":[0,0,0],"radius":0.02,"tail":[0,0.1,0]}}}],)"
        R"("colliderGroups":[{"name":"body","colliders":[0,1]},{"name":"neck","colliders":[2,0]}],)"
        R"("springs":[)"
        R"({"name":"hair","center":0,"colliderGroups":[0,1,5],"joints":[)"
        R"({"node":3,"stiffness":0.8,"dragForce":0.4,"gravityPower":0.5,"gravityDir":[1,0,0],"hitRadius":0.02},)"
        R"({"node":4,"stiffness":0.4,"dragForce":0.2,"gravityPower":0.1,"gravityDir":[1,0,0],"hitRadius":0.04},)"
        R"({"node":19,"stiffness":9,"dragForce":9}]},)"
        R"({"name":"branch","joints":[{"node":4},{"node":20}]},)"
        R"({"name":"single","joints":[{"node":22}]}]})";
    std::vector<std::string> warnings;
    auto v1 = loadV1(options, &warnings);
    REQUIRE(v1 != nullptr);
    CHECK(v1->springSource == "VRMC_springBone");
    // Invalid collider dropped; the capsule became a row of spheres.
    REQUIRE(v1->springColliderGroups.size() == 2);
    CHECK(v1->springColliderGroups[0].node == 2);
    CHECK(v1->springColliderGroups[1].node == 3);
    CHECK(v1->springColliderGroups[1].colliders.size() >= 3);
    CHECK_NEAR(v1->springColliderGroups[1].colliders.back().offset.y, 0.1, 1e-6);
    // Two springs: the one-joint spring has no segment to simulate.
    REQUIRE(v1->springChains.size() == 2);
    const vrm::SpringBoneChain& hair = v1->springChains[0];
    CHECK((hair.nodes == std::vector<int>{3, 4, 19}));
    CHECK(hair.parentChain == -1);
    CHECK((hair.colliderGroups == std::vector<int>{0, 1}));
    // Segment settings (the tail joint's are unused) are averaged.
    CHECK_NEAR(hair.stiffness, 0.6, 1e-5);
    CHECK_NEAR(hair.dragForce, 0.3, 1e-5);
    CHECK_NEAR(hair.gravityPower, 0.3, 1e-5);
    CHECK_NEAR(hair.hitRadius, 0.03, 1e-5);
    // Gravity is in the file's model frame and turns with the scene.
    CHECK_NEAR(hair.gravityDir.x, -1.0, 1e-5);
    const vrm::SpringBoneChain& branch = v1->springChains[1];
    CHECK((branch.nodes == std::vector<int>{4, 20}));
    CHECK(branch.parentChain == 0);
    CHECK(branch.parentIndex == 1);
    CHECK(v1->springGroups[0].center == 0);
    CHECK(v1->diagnostics.springChainCount == 2);

    // The runtime takes the chains as they are.
    animation::HumanoidRetargeter rig(*v1);
    REQUIRE(rig.valid());
    animation::SecondaryDynamics springs(*v1);
    CHECK(springs.available());
    motion::ClipPose pose = motion::neutralClipPose();
    animation::RetargetedPose out = rig.applyClip(pose, 0.0);
    springs.reset(out);
    for (int frame = 0; frame < 120; ++frame) {
        pose.bone[static_cast<std::size_t>(HumanoidBone::Chest)] =
            math::fromAxisAngle({0.0f, 1.0f, 0.0f}, 0.5f * std::sin(frame * 0.2f));
        out = rig.applyClip(pose, frame / 60.0);
        springs.update(out, 1.0 / 60.0, animation::SecondarySettings{});
    }
    for (const auto& global : out.nodeGlobals) {
        CHECK(global.finite());
    }
}

TEST_CASE(vrm1.node_constraints) {
    V1Options options;
    options.constraints[0] = R"("roll":{"source":6,"rollAxis":"X","weight":0.5})";
    options.constraints[1] = R"("aim":{"source":7,"aimAxis":"PositiveZ"})";
    options.constraints[2] = R"("rotation":{"source":3,"weight":1})";
    auto v1 = loadV1(options);
    REQUIRE(v1 != nullptr);
    REQUIRE(v1->nodeConstraints.size() == 3);
    CHECK(v1->diagnostics.nodeConstraintCount == 3);
    CHECK(v1->nodeConstraints[0].type == vrm::NodeConstraintType::Roll);
    CHECK(v1->nodeConstraints[0].destination == 25);
    CHECK(v1->nodeConstraints[1].type == vrm::NodeConstraintType::Aim);
    CHECK(v1->nodeConstraints[2].type == vrm::NodeConstraintType::Rotation);

    animation::NodeConstraintSolver solver(*v1);
    CHECK(!solver.empty());
    CHECK(solver.skippedCycles() == 0);

    // Pose the sources by hand: local rotation deltas, globals re-derived.
    std::vector<math::Mat4> locals(v1->nodes.size());
    for (std::size_t i = 0; i < locals.size(); ++i) {
        locals[i] = v1->nodes[i].local;
    }
    const auto setRot = [&](int node, Quat q) {
        const vrm::Node& n = v1->nodes[static_cast<std::size_t>(node)];
        locals[static_cast<std::size_t>(node)] = math::Mat4::fromTRS(n.translation, q, n.scale);
    };
    const Quat twist = math::fromAxisAngle({1.0f, 0.0f, 0.0f}, 1.0f);
    const Quat swing = math::fromAxisAngle({0.0f, 0.0f, 1.0f}, 0.5f);
    setRot(6, swing * twist);
    const Quat neck = math::fromAxisAngle({0.2f, 1.0f, 0.1f}, 0.7f);
    setRot(3, neck);
    std::vector<math::Mat4> globals(locals.size());
    const auto evaluate = [&]() {
        std::vector<int> stack(v1->sceneRoots.rbegin(), v1->sceneRoots.rend());
        while (!stack.empty()) {
            const int n = stack.back();
            stack.pop_back();
            const int parent = v1->nodes[static_cast<std::size_t>(n)].parent;
            globals[static_cast<std::size_t>(n)] =
                parent >= 0 ? globals[static_cast<std::size_t>(parent)] * locals[static_cast<std::size_t>(n)]
                            : locals[static_cast<std::size_t>(n)];
            for (int c : v1->nodes[static_cast<std::size_t>(n)].children) {
                stack.push_back(c);
            }
        }
    };
    evaluate();
    const std::vector<math::Mat4> before = globals;
    CHECK(solver.apply(globals) == 3);
    const auto localOf = [&](int node) {
        const int parent = v1->nodes[static_cast<std::size_t>(node)].parent;
        return (globals[static_cast<std::size_t>(parent)].inverseAffine() *
                globals[static_cast<std::size_t>(node)])
            .rotationQuat();
    };
    // Roll: half of the source's twist about X, none of its swing.
    const Quat roll = localOf(25);
    const Quat expectRoll = math::fromAxisAngle({1.0f, 0.0f, 0.0f}, 0.5f);
    CHECK(std::abs(math::dot(roll, expectRoll)) > 0.99999f);
    // Rotation: the neck's delta, copied.
    CHECK(std::abs(math::dot(localOf(27), neck)) > 0.99999f);
    // Aim: the aimer's +Z points at the left hand.
    const Vec3 aimAxis = math::normalize(globals[26].axisZ());
    const Vec3 toHand = math::normalize(globals[7].translationPart() - globals[26].translationPart());
    CHECK(math::dot(aimAxis, toHand) > 0.9999f);
    // Nodes without constraints are untouched.
    CHECK((globals[7].translationPart() - before[7].translationPart()).length() < 1e-6f);

    // The retargeter evaluates them on every pose.
    animation::HumanoidRetargeter rig(*v1);
    REQUIRE(rig.valid());
    const auto pose = rig.applyClip(motion::neutralClipPose(), 0.0);
    const Vec3 aim = math::normalize(pose.nodeGlobals[26].axisZ());
    const Vec3 hand = math::normalize(pose.nodeGlobals[7].translationPart() -
                                      pose.nodeGlobals[26].translationPart());
    CHECK(math::dot(aim, hand) > 0.9999f);
}

TEST_CASE(vrm1.constraint_cycles_are_skipped) {
    V1Options options;
    // 25 copies 27 and 27 copies 25: no order satisfies both.
    options.constraints[0] = R"("rotation":{"source":27})";
    options.constraints[2] = R"("rotation":{"source":25})";
    options.constraints[1] = R"("aim":{"source":26,"aimAxis":"PositiveZ"})";  // self: invalid
    std::vector<std::string> warnings;
    auto v1 = loadV1(options, &warnings);
    REQUIRE(v1 != nullptr);
    CHECK(v1->nodeConstraints.size() == 2);
    animation::NodeConstraintSolver solver(*v1);
    CHECK(solver.skippedCycles() == 2);
    CHECK(solver.empty());
}

TEST_CASE(vrm1.rejects_and_survives_bad_input) {
    // Neither extension: a clear error.
    std::string json = vrm1Json({});
    const std::string key = "\"VRMC_vrm\":{";
    json.replace(json.find(key), key.size(), "\"VRMC_xxx\":{");
    auto bytes = makeGlb(json);
    auto result = vrm::VrmLoader::loadFromMemory(
        std::span<const std::uint8_t>(bytes.data(), bytes.size()), "none.vrm");
    CHECK(!result.ok());
    CHECK(result.error.find("VRMC_vrm") != std::string::npos);

    // Mutations of a 1.0 file never crash.
    V1Options rich;
    rich.morphTarget = true;
    rich.constraints[0] = R"("roll":{"source":6,"rollAxis":"X","weight":0.5})";
    rich.constraints[1] = R"("aim":{"source":7,"aimAxis":"NegativeY"})";
    rich.extraExtensions =
        R"(,"VRMC_springBone":{"colliders":[{"node":2,"shape":{"capsule":{"offset":[0,0,0],"radius":0.05,"tail":[0,0.2,0]}}}],)"
        R"("colliderGroups":[{"colliders":[0]}],"springs":[{"joints":[{"node":3},{"node":4},{"node":19}],"colliderGroups":[0]}]})";
    const auto base = makeGlb(vrm1Json(rich));
    std::uint32_t state = 0x2545F491u;
    int accepted = 0;
    for (int iteration = 0; iteration < 200; ++iteration) {
        std::vector<std::uint8_t> mutated = base;
        for (int m = 0; m < 4; ++m) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            mutated[state % mutated.size()] ^= static_cast<std::uint8_t>((state >> 8) & 0xffu);
        }
        const auto loaded = vrm::VrmLoader::loadFromMemory(
            std::span<const std::uint8_t>(mutated.data(), mutated.size()), "mutated-1.0.vrm");
        if (!loaded.ok()) {
            CHECK(!loaded.error.empty());
            continue;
        }
        ++accepted;
        CHECK(loaded.model->bounds.valid);
        animation::HumanoidRetargeter rig(*loaded.model);
        if (rig.valid()) {
            const auto pose = rig.applyClip(motion::neutralClipPose(), 0.0);
            for (const auto& global : pose.nodeGlobals) {
                CHECK(global.finite());
            }
        }
    }
    std::cout << "\n            1.0 mutation corpus: " << accepted << " accepted";
}


TEST_CASE(vrm1.spring_joints_may_skip_nodes) {
    // Neck -> LeftEye skips the head; the spec allows it. The head joins the
    // chain so it is carried along instead of staying at the animated pose.
    V1Options options;
    options.extraExtensions =
        R"(,"VRMC_springBone":{"specVersion":"1.0","springs":[{"joints":[{"node":3},{"node":19}]}]})";
    auto v1 = loadV1(options);
    REQUIRE(v1 != nullptr);
    REQUIRE(v1->springChains.size() == 1);
    CHECK((v1->springChains[0].nodes == std::vector<int>{3, 4, 19}));
    // Not a descendant: truncated to nothing.
    options.extraExtensions =
        R"(,"VRMC_springBone":{"specVersion":"1.0","springs":[{"joints":[{"node":19},{"node":3}]}]})";
    auto bad = loadV1(options);
    REQUIRE(bad != nullptr);
    CHECK(bad->springChains.empty());
}

TEST_CASE(vrm1.long_capsule_keeps_coverage) {
    V1Options options;
    options.extraExtensions =
        R"(,"VRMC_springBone":{"specVersion":"1.0","colliders":[)"
        R"({"node":2,"shape":{"capsule":{"offset":[0,0,0],"radius":0.01,"tail":[0,0.5,0]}}}]})";
    auto v1 = loadV1(options);
    REQUIRE(v1 != nullptr);
    REQUIRE(v1->springColliderGroups.size() == 1);
    const auto& spheres = v1->springColliderGroups[0].colliders;
    REQUIRE(spheres.size() >= 2);
    for (std::size_t i = 1; i < spheres.size(); ++i) {
        CHECK((spheres[i].offset - spheres[i - 1].offset).length() <= 0.01f + 1e-6f);
    }
    CHECK_NEAR(spheres.back().offset.y, 0.5, 1e-6);
}

TEST_CASE(vrm1.lookat_offset_follows_head_rest) {
    V1Options options;
    options.restRotations = true;
    options.vrmcExtra = R"(,"lookAt":{"offsetFromHeadBone":[0,0.06,0.02]})";
    auto v1 = loadV1(options);
    REQUIRE(v1 != nullptr);
    // Same rest formula as vrm1Json for node 4 (the head).
    const Quat headRest =
        math::fromAxisAngle({std::sin(4.0f), std::cos(1.3f * 4.0f), 0.5f}, 0.4f + 0.13f * 4.0f);
    const Vec3 expected = math::rotate(v1->sourceToModel * headRest, {0.0f, 0.06f, 0.02f});
    CHECK((v1->lookAt.offsetFromHeadBone - expected).length() < 1e-5f);
}
