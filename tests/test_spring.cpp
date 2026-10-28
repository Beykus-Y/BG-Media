// BG Motion 3.2 quality pass: Secondary Dynamics regression tests (C02, C03,
// C08, C09, C10). Synthetic VRMs are built from the minimal fixture by injecting
// a `secondaryAnimation` block into its JSON chunk; the optional private local
// model provides the realistic hair/skirt rig.
#include "framework.hpp"

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/vrm/ThighColliders.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace animation = bgmedia::animation;
namespace motion = bgmedia::motion;
namespace vrm = bgmedia::vrm;
namespace math = bgmedia::math;
using vrm::HumanoidBone;

std::filesystem::path fixturePath() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

std::filesystem::path realModelPath() {
    if (const char* env = std::getenv("BGMEDIA_VRM_TEST_MODEL")) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "models" / "Model1.vrm";
}

std::vector<std::uint8_t> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        v[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

// Rewrites the GLB so extensions.VRM carries `secondary` (a JSON object body).
std::vector<std::uint8_t> withSecondaryAnimation(const std::string& secondary) {
    const std::vector<std::uint8_t> bytes = readAll(fixturePath());
    const std::uint32_t jsonLength = le32(bytes.data() + 12);
    std::string json(reinterpret_cast<const char*>(bytes.data() + 20), jsonLength);
    const std::string marker = "\"VRM\":{";
    const std::size_t at = json.find(marker);
    if (at == std::string::npos) {
        return {};
    }
    json.insert(at + marker.size(), "\"secondaryAnimation\":{" + secondary + "},");
    while (json.size() % 4 != 0) {
        json.push_back(' ');
    }
    std::vector<std::uint8_t> out(bytes.begin(), bytes.begin() + 12);
    std::vector<std::uint8_t> chunkHeader(8);
    put32(chunkHeader, 0, static_cast<std::uint32_t>(json.size()));
    std::memcpy(chunkHeader.data() + 4, "JSON", 4);
    out.insert(out.end(), chunkHeader.begin(), chunkHeader.end());
    out.insert(out.end(), json.begin(), json.end());
    out.insert(out.end(), bytes.begin() + 20 + jsonLength, bytes.end());
    put32(out, 8, static_cast<std::uint32_t>(out.size()));
    return out;
}

std::unique_ptr<vrm::VrmModel> loadSynthetic(const std::string& secondary,
                                             std::vector<std::string>* warnings = nullptr) {
    const auto bytes = withSecondaryAnimation(secondary);
    auto result = vrm::VrmLoader::loadFromMemory(
        std::span<const std::uint8_t>(bytes.data(), bytes.size()), "synthetic-spring.vrm");
    if (warnings && result.model) {
        *warnings = result.model->warnings;
    }
    return std::move(result.model);
}

motion::ClipPose headBobPose(double t, float yawTurns = 0.0f, float rootX = 0.0f) {
    auto pose = motion::neutralClipPose();
    // A quick head/neck/chest motion drives the hair and the spring chains.
    pose.bone[static_cast<std::size_t>(HumanoidBone::Head)] =
        math::fromAxisAngle({0.0f, 1.0f, 0.0f}, 0.6f * static_cast<float>(std::sin(t * 7.0)));
    pose.bone[static_cast<std::size_t>(HumanoidBone::Neck)] =
        math::fromAxisAngle({1.0f, 0.0f, 0.0f}, 0.25f * static_cast<float>(std::sin(t * 5.0)));
    pose.bone[static_cast<std::size_t>(HumanoidBone::Chest)] =
        math::fromAxisAngle({0.0f, 0.0f, 1.0f}, 0.15f * static_cast<float>(std::sin(t * 3.0)));
    pose.rootYaw = yawTurns;
    pose.rootX = rootX;
    return pose;
}

}  // namespace

// --------------------------------------------------------------------------
// C10: collider group indices survive an invalid group
// --------------------------------------------------------------------------
TEST_CASE(spring.collider_group_indices_survive_an_invalid_group) {
    std::vector<std::string> warnings;
    auto model = loadSynthetic(
        R"("colliderGroups":[{"node":9999,"colliders":[{"offset":{"x":0,"y":0,"z":0},"radius":0.5}]},)"
        R"({"node":2,"colliders":[{"offset":{"x":0,"y":0.1,"z":0},"radius":0.12}]},)"
        R"({"node":3,"colliders":[{"offset":{"x":0,"y":0,"z":0},"radius":0.05}]}],)"
        R"("boneGroups":[{"comment":"refs","stiffiness":0.5,"gravityPower":0,"dragForce":0.4,"center":-1,)"
        R"("hitRadius":0.02,"bones":[3],"colliderGroups":[1,2,7]}])",
        &warnings);
    REQUIRE(model != nullptr);
    // Three groups in, three groups out: the bad one is an empty placeholder.
    REQUIRE(model->springColliderGroups.size() == 3);
    CHECK(model->springColliderGroups[0].colliders.empty());
    REQUIRE(model->springColliderGroups[1].colliders.size() == 1);
    CHECK(model->springColliderGroups[1].node == 2);
    CHECK_NEAR(model->springColliderGroups[1].colliders[0].radius, 0.12, 1e-6);
    REQUIRE(model->springColliderGroups[2].colliders.size() == 1);
    CHECK(model->springColliderGroups[2].node == 3);
    // The references still mean "the second and third authored group"; index 7 is
    // out of range and dropped with a warning, never wrapped to another group.
    REQUIRE(model->springGroups.size() == 1);
    const auto& refs = model->springGroups[0].colliderGroups;
    CHECK((refs == std::vector<int>{1, 2}));
    bool warned = false;
    for (const auto& w : warnings) {
        warned = warned || w.find("invalid node") != std::string::npos;
    }
    CHECK(warned);
    // The simulator tolerates the placeholder.
    animation::SecondaryDynamics dynamics(*model);
    CHECK(dynamics.available());
    CHECK(dynamics.colliderCount() == 2);
}

// --------------------------------------------------------------------------
// C09: branching chains and single-node (virtual tail) chains
// --------------------------------------------------------------------------
TEST_CASE(spring.branching_subtree_is_fully_covered_without_double_writes) {
    // Neck(3) -> Head(4) -> two eyes (19, 20): the old loader followed only the
    // first child and left eye 20 unsimulated.
    auto model = loadSynthetic(
        R"("boneGroups":[{"comment":"branch","stiffiness":0.5,"gravityPower":1,"dragForce":0.4,)"
        R"("center":-1,"hitRadius":0.02,"bones":[3],"colliderGroups":[]}])");
    REQUIRE(model != nullptr);
    REQUIRE(model->springChains.size() == 2);
    const auto& main = model->springChains[0];
    const auto& branch = model->springChains[1];
    CHECK((main.nodes == std::vector<int>{3, 4, 19}));
    CHECK(main.parentChain == -1);
    // The branch is anchored at the branch point (Head) of the main chain.
    CHECK((branch.nodes == std::vector<int>{4, 20}));
    CHECK(branch.parentChain == 0);
    CHECK(branch.parentIndex == 1);
    // Every subtree node is counted exactly once: 3, 4, 19, 20.
    CHECK(model->springBoneCount() == 4);

    animation::HumanoidRetargeter rig(*model);
    REQUIRE(rig.valid());
    animation::SecondaryDynamics dynamics(*model);
    REQUIRE(dynamics.available());
    CHECK(dynamics.boneCount() == 4);
    animation::SecondarySettings settings;
    settings.intensity = 1.0f;
    const double dt = 1.0 / 60.0;
    float eye20 = 0.0f;
    for (int i = 0; i < 120; ++i) {
        auto pose = rig.applyClip(headBobPose(i * dt), i * dt);
        const auto animated = pose.nodeGlobals[20].translationPart();
        dynamics.update(pose, i == 0 ? 0.0 : dt, settings);
        CHECK(!dynamics.stats().nonFinite);
        for (const auto& m : pose.nodeGlobals) CHECK(m.finite());
        eye20 = std::max(eye20, (pose.nodeGlobals[20].translationPart() - animated).length());
    }
    // The branch eye is simulated: it lags the head instead of being rigid.
    CHECK(eye20 > 0.0005f);
    CHECK(dynamics.stats().maxStretch < 0.02f);
}

TEST_CASE(spring.single_node_chain_uses_a_virtual_tail) {
    // Eye 19 has no children: a spring bone on a leaf needs the VRM 0.x virtual tail.
    auto model = loadSynthetic(
        R"("boneGroups":[{"comment":"leaf","stiffiness":0.3,"gravityPower":1,"dragForce":0.4,)"
        R"("center":-1,"hitRadius":0.02,"bones":[19],"colliderGroups":[]}])");
    REQUIRE(model != nullptr);
    REQUIRE(model->springChains.size() == 1);
    CHECK(model->springChains[0].nodes.size() == 1);
    animation::HumanoidRetargeter rig(*model);
    animation::SecondaryDynamics dynamics(*model);
    CHECK(dynamics.available());
    CHECK(dynamics.chainCount() == 1);
    animation::SecondarySettings settings;
    float rotated = 0.0f;
    const double dt = 1.0 / 60.0;
    for (int i = 0; i < 120; ++i) {
        auto pose = rig.applyClip(headBobPose(i * dt), i * dt);
        const auto before = pose.nodeGlobals[19].rotationQuat();
        dynamics.update(pose, i == 0 ? 0.0 : dt, settings);
        for (const auto& m : pose.nodeGlobals) CHECK(m.finite());
        const float d = std::abs(math::dot(before, pose.nodeGlobals[19].rotationQuat()));
        rotated = std::max(rotated, 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)));
    }
    CHECK(rotated > 0.003f);  // the leaf bone swings
}

// --------------------------------------------------------------------------
// C08: units. Step response of a chain under a root shift.
// --------------------------------------------------------------------------
TEST_CASE(spring.step_response_reacts_and_settles_with_consistent_units) {
    auto model = loadSynthetic(
        R"("boneGroups":[{"comment":"step","stiffiness":0.5,"gravityPower":1,"dragForce":0.4,)"
        R"("center":-1,"hitRadius":0.02,"bones":[3],"colliderGroups":[]}])");
    REQUIRE(model != nullptr);
    animation::HumanoidRetargeter rig(*model);
    animation::SecondaryDynamics dynamics(*model);
    animation::SecondarySettings settings;
    settings.maxDisplacement = 0.05f;
    const double dt = 1.0 / 60.0;
    // Settle at rest, then move the whole character sideways by 0.1 m in one
    // frame (below the teleport threshold) and watch the hair chain follow.
    float reaction = 0.0f;
    float final = 0.0f;
    for (int i = 0; i < 360; ++i) {
        const float rootX = i < 120 ? 0.0f : 0.06f;
        auto pose = rig.applyClip(headBobPose(0.0, 0.0f, rootX), i * dt);
        const auto animated = pose.nodeGlobals[19].translationPart();
        dynamics.update(pose, i == 0 ? 0.0 : dt, settings);
        const float d = (pose.nodeGlobals[19].translationPart() - animated).length();
        if (i >= 120 && i < 180) reaction = std::max(reaction, d);
        if (i == 359) final = d;
    }
    std::cout << std::fixed << std::setprecision(4) << "\n            step response: peak=" << reaction
              << " m, settled=" << final << " m";
    CHECK(reaction > 0.004f);   // it does react to the shift (not scaled away by dt)
    CHECK(reaction <= 0.05f);   // and stays inside the authored limit
    CHECK(final < 0.003f);      // and settles back onto the pose (gravity sag only)
}

// --------------------------------------------------------------------------
// C02: root yaw must not change the character-space response
// --------------------------------------------------------------------------
TEST_CASE(spring.response_is_independent_of_root_yaw) {
    const auto path = realModelPath();
    if (!std::filesystem::exists(path)) {
        return;
    }
    auto loaded = vrm::VrmLoader::load(path);
    REQUIRE(loaded.ok());
    const vrm::VrmModel& model = *loaded.model;
    animation::HumanoidRetargeter rig(model);
    std::vector<int> springNodes;
    for (const auto& chain : model.springChains) {
        for (int n : chain.nodes) springNodes.push_back(n);
    }
    REQUIRE(!springNodes.empty());

    const float yaws[] = {0.0f, 0.25f, 0.5f, -0.25f};
    // Per frame and node: (simulated - animated) position and rotation delta, both
    // in character space. The animated pose itself legitimately differs per yaw
    // (planted feet are fixed in stage space), the spring *response* must not.
    struct Response {
        math::Vec3 offset;
        math::Quat delta;
    };
    std::vector<std::vector<Response>> reference;
    float worstPosition = 0.0f;
    float worstRotation = 0.0f;
    for (float yaw : yaws) {
        animation::SecondaryDynamics dynamics(model);
        animation::SecondarySettings settings;
        const double dt = 1.0 / 60.0;
        std::vector<std::vector<Response>> frames;
        for (int i = 0; i < 150; ++i) {
            const double t = i * dt;
            auto pose = rig.applyClip(headBobPose(t, yaw), t);
            const auto animated = pose.nodeGlobals;
            dynamics.update(pose, i == 0 ? 0.0 : dt, settings);
            std::vector<Response> snapshot;
            for (int n : springNodes) {
                const auto& a = animated[static_cast<std::size_t>(n)];
                const auto& b = pose.nodeGlobals[static_cast<std::size_t>(n)];
                snapshot.push_back({b.translationPart() - a.translationPart(),
                                    (b.rotationQuat() * a.rotationQuat().conjugate()).normalized()});
            }
            frames.push_back(std::move(snapshot));
        }
        if (reference.empty()) {
            reference = frames;
            continue;
        }
        for (std::size_t f = 0; f < frames.size(); ++f) {
            for (std::size_t k = 0; k < frames[f].size(); ++k) {
                worstPosition = std::max(worstPosition,
                                         (frames[f][k].offset - reference[f][k].offset).length());
                const float d = std::abs(math::dot(frames[f][k].delta, reference[f][k].delta));
                worstRotation = std::max(worstRotation, 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)));
            }
        }
    }
    std::cout << std::fixed << std::setprecision(6) << "\n            yaw 90/180/-90 vs 0: worst position diff="
              << worstPosition << " m, worst rotation diff=" << worstRotation << " rad";
    CHECK(worstPosition < 1.0e-4f);
    CHECK(worstRotation < 2.0e-3f);
}

// --------------------------------------------------------------------------
// C03: OFF -> ON, seek and teleport
// --------------------------------------------------------------------------
TEST_CASE(spring.enabling_after_a_pose_change_does_not_pop) {
    const auto path = realModelPath();
    if (!std::filesystem::exists(path)) {
        return;
    }
    auto loaded = vrm::VrmLoader::load(path);
    REQUIRE(loaded.ok());
    animation::HumanoidRetargeter rig(*loaded.model);
    animation::SecondaryDynamics dynamics(*loaded.model);
    animation::SecondarySettings on;
    animation::SecondarySettings off;
    off.enabled = false;
    const double dt = 1.0 / 60.0;
    int frame = 0;
    std::vector<math::Mat4> lastAnimated;
    for (; frame < 120; ++frame) {  // simulate with spring ON, vigorous motion
        auto pose = rig.applyClip(headBobPose(frame * dt), frame * dt);
        dynamics.update(pose, frame == 0 ? 0.0 : dt, on);
    }
    for (int i = 0; i < 90; ++i, ++frame) {  // spring OFF while the pose changes a lot
        auto pose = rig.applyClip(headBobPose(10.0 + frame * dt * 3.0), frame * dt);
        const auto before = pose.nodeGlobals;
        lastAnimated = before;
        dynamics.update(pose, dt, off);
        for (std::size_t n = 0; n < before.size(); ++n) {  // OFF means NO deformation
            CHECK((pose.nodeGlobals[n].translationPart() - before[n].translationPart()).length() < 1.0e-7f);
        }
    }
    // Switch ON: the very first frame must not jump from the stale state.
    auto pose = rig.applyClip(headBobPose(10.0 + frame * dt * 3.0), frame * dt);
    const auto animated = pose.nodeGlobals;
    dynamics.update(pose, dt, on);
    float worst = 0.0f;
    float animatedMotion = 0.0f;
    for (const auto& chain : loaded.model->springChains) {
        for (int n : chain.nodes) {
            const auto i = static_cast<std::size_t>(n);
            worst = std::max(worst, (pose.nodeGlobals[i].translationPart() -
                                     animated[i].translationPart()).length());
            animatedMotion = std::max(animatedMotion, (animated[i].translationPart() -
                                                       lastAnimated[i].translationPart()).length());
        }
    }
    std::cout << std::fixed << std::setprecision(5)
              << "\n            first frame after OFF->ON: displacement=" << worst
              << " m, animation moved " << animatedMotion << " m that frame";
    // The simulation restarted from the pose of the last OFF frame, so the only
    // lag is the animation's own frame-to-frame motion; the stale ON state
    // (90 frames old, clamped at 50 mm) is gone.
    CHECK(worst <= animatedMotion + 0.002f);
}

TEST_CASE(spring.teleport_and_hitch_snap_to_the_pose) {
    const auto path = realModelPath();
    if (!std::filesystem::exists(path)) {
        return;
    }
    auto loaded = vrm::VrmLoader::load(path);
    REQUIRE(loaded.ok());
    animation::HumanoidRetargeter rig(*loaded.model);
    animation::SecondaryDynamics dynamics(*loaded.model);
    animation::SecondarySettings on;
    const double dt = 1.0 / 60.0;
    for (int i = 0; i < 120; ++i) {
        auto pose = rig.applyClip(headBobPose(i * dt), i * dt);
        dynamics.update(pose, i == 0 ? 0.0 : dt, on);
    }
    const auto displacementAfter = [&](float rootX, double delta, double t) {
        auto pose = rig.applyClip(headBobPose(t, 0.0f, rootX), t);
        const auto animated = pose.nodeGlobals;
        dynamics.update(pose, delta, on);
        float worst = 0.0f;
        for (const auto& chain : loaded.model->springChains) {
            for (int n : chain.nodes) {
                worst = std::max(worst, (pose.nodeGlobals[static_cast<std::size_t>(n)].translationPart() -
                                         animated[static_cast<std::size_t>(n)].translationPart()).length());
            }
        }
        return worst;
    };
    CHECK(displacementAfter(3.0f, dt, 2.1) < 1.0e-4f);      // teleport (3 heights away)
    CHECK(displacementAfter(0.0f, dt, 2.2) < 0.05f);        // and the sim keeps working
    CHECK(displacementAfter(0.0f, 5.0, 40.0) < 1.0e-4f);    // pause/resume: 5 s gap
    CHECK(displacementAfter(0.0f, -1.0, 1.0) < 1.0e-4f);    // seek backwards
}

// --------------------------------------------------------------------------
// C08: frame-rate independence of the fixed-step integrator
// --------------------------------------------------------------------------
TEST_CASE(spring.result_is_frame_rate_independent_and_finite) {
    const auto path = realModelPath();
    if (!std::filesystem::exists(path)) {
        return;
    }
    auto loaded = vrm::VrmLoader::load(path);
    REQUIRE(loaded.ok());
    animation::HumanoidRetargeter rig(*loaded.model);
    const int tipNode = loaded.model->springChains[0].nodes.back();
    std::vector<math::Vec3> tips;
    for (double fps : {30.0, 60.0, 120.0, 180.0}) {
        animation::SecondaryDynamics dynamics(*loaded.model);
        animation::SecondarySettings settings;
        math::Vec3 tip{};
        const int frames = static_cast<int>(2.0 * fps);
        for (int i = 0; i <= frames; ++i) {
            const double t = i / fps;
            auto pose = rig.applyClip(headBobPose(t), t);
            dynamics.update(pose, i == 0 ? 0.0 : 1.0 / fps, settings);
            CHECK(!dynamics.stats().nonFinite);
            CHECK(dynamics.stats().maxStretch < 0.05f);
            tip = animation::HumanoidRetargeter::stagePosition(pose, tipNode);
        }
        tips.push_back(tip);
    }
    float spread = 0.0f;
    for (const auto& a : tips) {
        for (const auto& b : tips) spread = std::max(spread, (a - b).length());
    }
    std::cout << std::fixed << std::setprecision(5)
              << "\n            tip position spread across 30/60/120/180 FPS: " << spread << " m";
    CHECK(spread < 0.02f);

    // A 0.2 s hitch (below the discontinuity threshold) stays finite and bounded.
    animation::SecondaryDynamics dynamics(*loaded.model);
    animation::SecondarySettings settings;
    for (int i = 0; i < 60; ++i) {
        auto pose = rig.applyClip(headBobPose(i / 60.0), i / 60.0);
        dynamics.update(pose, i == 0 ? 0.0 : 1.0 / 60.0, settings);
    }
    auto pose = rig.applyClip(headBobPose(1.2), 1.2);
    dynamics.update(pose, 0.2, settings);
    CHECK(!dynamics.stats().nonFinite);
    CHECK(dynamics.stats().maxDisplacement <= settings.maxDisplacement * 2.0f + 1.0e-4f);
}

// A dance (not a single head-bob instant): the same choreography simulated at 60
// and at 120 FPS must trace the same spring trajectories.
TEST_CASE(spring.dance_trajectories_agree_between_60_and_120_fps) {
    const auto path = realModelPath();
    if (!std::filesystem::exists(path)) {
        return;
    }
    auto loaded = vrm::VrmLoader::load(path);
    REQUIRE(loaded.ok());
    animation::HumanoidRetargeter rig(*loaded.model);
    std::vector<int> nodes;
    for (const auto& chain : loaded.model->springChains) {
        nodes.push_back(chain.nodes.back());
    }
    const auto run = [&](double fps) {
        motion::DancePlayer player;
        player.loadLibrary(std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances");
        player.setOverlayMode(motion::OverlayMode::Full);
        (void)player.selectClip("neonPulse", 0.0, true);
        animation::SecondaryDynamics dynamics(*loaded.model);
        animation::SecondarySettings settings;
        std::vector<std::vector<math::Vec3>> samples;  // per 1/60 s tick
        const int frames = static_cast<int>(8.0 * fps);
        for (int i = 0; i <= frames; ++i) {
            const double t = 1.0 + i / fps;
            const auto frame = player.update(t);
            auto pose = rig.applyClip(frame.pose, t);
            dynamics.update(pose, i == 0 ? 0.0 : 1.0 / fps, settings);
            if (i % static_cast<int>(fps / 60.0) == 0) {
                std::vector<math::Vec3> row;
                for (int n : nodes) row.push_back(animation::HumanoidRetargeter::stagePosition(pose, n));
                samples.push_back(std::move(row));
            }
        }
        return samples;
    };
    const auto a = run(60.0);
    const auto b = run(120.0);
    REQUIRE(a.size() == b.size());
    float worst = 0.0f;
    std::size_t worstI = 0, worstK = 0, over = 0;
    double mean = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 60; i < a.size(); ++i) {  // skip the first second (start-up)
        for (std::size_t k = 0; k < a[i].size(); ++k) {
            const float d = (a[i][k] - b[i][k]).length();
            if (d > worst) { worstI = i; worstK = k; }
            if (d > 0.02f) ++over;
            worst = std::max(worst, d);
            mean += d;
            ++count;
        }
    }
    std::cout << std::fixed << std::setprecision(5)
              << "\n            neonPulse tip trajectories, 60 vs 120 FPS: max diff=" << worst
              << " m, mean diff=" << mean / static_cast<double>(count) << " m at tick " << worstI << " chain " << worstK
              << "; samples over 2 cm: " << over << " of " << count;
    // Contact with collider spheres can amplify a tiny phase difference for a
    // moment; it must stay an isolated event, not a systematic frame-rate dependence.
    CHECK(mean / static_cast<double>(count) < 0.005);
    CHECK(static_cast<double>(over) < 0.005 * static_cast<double>(count));
    CHECK(worst <= 0.0501f);  // never beyond the authored displacement cap
}

// --------------------------------------------------------------------------
// BG Motion 3.3 (issue #11): opt-in thigh/hip colliders for lower-body chains
// --------------------------------------------------------------------------
namespace {

// Appends a node below `parent`, composing the bind pose the way the loader does.
int appendNode(vrm::VrmModel& model, int parent, math::Vec3 translation) {
    vrm::Node node;
    node.name = "Synthetic" + std::to_string(model.nodes.size());
    node.parent = parent;
    node.translation = translation;
    node.rotation = math::Quat::identity();
    node.local = math::Mat4::translation(translation);
    const int index = static_cast<int>(model.nodes.size());
    const auto p = static_cast<std::size_t>(parent);
    model.nodes[p].children.push_back(index);
    model.bindGlobals.push_back(model.bindGlobals[p] * node.local);
    model.bindGlobalRotations.push_back((model.bindGlobalRotations[p] * node.rotation).normalized());
    model.nodes.push_back(std::move(node));
    return index;
}

// A skirt-like chain: three new nodes hanging from the hips, the first one
// offset to (x, -0.05, z). Returns the index of the chain in springChains.
int appendHangingChain(vrm::VrmModel& model, float x, float z) {
    vrm::SpringBoneChain chain;
    chain.stiffness = 0.5f;
    chain.gravityPower = 1.0f;
    chain.dragForce = 0.4f;
    chain.hitRadius = 0.02f;
    int parent = model.nodeFor(HumanoidBone::Hips);
    for (int i = 0; i < 3; ++i) {
        const math::Vec3 offset = i == 0 ? math::Vec3{x, -0.05f, z} : math::Vec3{0.0f, -0.2f, 0.0f};
        parent = appendNode(model, parent, offset);
        chain.nodes.push_back(parent);
    }
    model.springChains.push_back(chain);
    return static_cast<int>(model.springChains.size()) - 1;
}

// Hair under the neck (Neck -> Head -> eyes) plus a skirt hanging from the hips.
std::unique_ptr<vrm::VrmModel> hairAndSkirtModel(float skirtZ = 0.07f) {
    auto model = loadSynthetic(
        R"("boneGroups":[{"comment":"hair","stiffiness":0.5,"gravityPower":1,"dragForce":0.4,)"
        R"("center":-1,"hitRadius":0.02,"bones":[3],"colliderGroups":[]}])");
    if (model) {
        appendHangingChain(*model, 0.08f, skirtZ);
    }
    return model;
}

motion::ClipPose hipSwayPose(double t) {
    auto pose = headBobPose(t);
    pose.bone[static_cast<std::size_t>(HumanoidBone::Hips)] =
        math::fromAxisAngle({0.0f, 0.0f, 1.0f}, 0.25f * static_cast<float>(std::sin(t * 4.0)));
    return pose;
}

vrm::ThighColliderOptions thighOn() {
    vrm::ThighColliderOptions options;
    options.enabled = true;
    return options;
}

}  // namespace

TEST_CASE(spring.thigh_colliders_are_off_by_default) {
    auto model = hairAndSkirtModel();
    REQUIRE(model != nullptr);
    const auto report = vrm::synthesizeThighColliders(*model);
    CHECK(!report.applied);
    CHECK(report.skipReason == "disabled");
    CHECK(model->springColliderGroups.empty());
    CHECK(model->springChains.back().colliderGroups.empty());
}

TEST_CASE(spring.thigh_colliders_leave_models_with_leg_or_hips_colliders_unchanged) {
    // Upper-leg colliders are authored: nothing is added and nothing re-pointed.
    auto legs = loadSynthetic(
        R"("colliderGroups":[{"node":11,"colliders":[{"offset":{"x":0,"y":0,"z":0},"radius":0.08}]},)"
        R"({"node":15,"colliders":[{"offset":{"x":0,"y":0,"z":0},"radius":0.08}]}])");
    REQUIRE(legs != nullptr);
    appendHangingChain(*legs, 0.08f, 0.07f);
    auto report = vrm::synthesizeThighColliders(*legs, thighOn());
    CHECK(!report.applied);
    CHECK(report.skipReason.find("already has") != std::string::npos);
    CHECK(legs->springColliderGroups.size() == 2);
    CHECK(legs->springChains.back().colliderGroups.empty());

    // A collider on the hips counts as an existing collider too.
    auto hips = loadSynthetic(
        R"("colliderGroups":[{"node":0,"colliders":[{"offset":{"x":0,"y":0,"z":0},"radius":0.2}]}])");
    REQUIRE(hips != nullptr);
    appendHangingChain(*hips, 0.08f, 0.07f);
    report = vrm::synthesizeThighColliders(*hips, thighOn());
    CHECK(!report.applied);
    CHECK(hips->springColliderGroups.size() == 1);
    CHECK(hips->springChains.back().colliderGroups.empty());
}

TEST_CASE(spring.thigh_colliders_attach_to_a_skirt_and_never_to_hair) {
    auto model = hairAndSkirtModel();
    REQUIRE(model != nullptr);
    const auto report = vrm::synthesizeThighColliders(*model, thighOn());
    REQUIRE(report.applied);
    CHECK(report.groupsAdded == 2);
    CHECK(report.spheresAdded == 6);
    CHECK(report.chainsTouched == 1);
    CHECK(report.chainsExcluded == 2);  // the hair chain and its eye branch
    REQUIRE(model->springColliderGroups.size() == 2);
    CHECK(model->springColliderGroups[0].node == model->nodeFor(HumanoidBone::LeftUpperLeg));
    CHECK(model->springColliderGroups[1].node == model->nodeFor(HumanoidBone::RightUpperLeg));
    CHECK(model->springColliderGroups[0].colliders.size() == 3);
    // Fallback radius (no skinned mesh): 4.5 % of model height.
    CHECK(!report.leftRadiusFromMesh);
    CHECK_NEAR(report.leftRadius, 0.045f * model->height, 1e-6);
    CHECK_NEAR(model->springColliderGroups[0].colliders[0].radius, report.leftRadius, 1e-6);

    // The sphere centres sit on the thigh axis (hip joint -> knee joint).
    const math::Mat4 hip = model->bindGlobals[static_cast<std::size_t>(model->nodeFor(HumanoidBone::LeftUpperLeg))];
    const math::Vec3 a = hip.translationPart();
    const math::Vec3 b = model->bindGlobals[static_cast<std::size_t>(model->nodeFor(HumanoidBone::LeftLowerLeg))].translationPart();
    const float fractions[3] = {0.2f, 0.5f, 0.8f};
    for (int k = 0; k < 3; ++k) {
        const math::Vec3 world = hip.transformPoint(
            model->springColliderGroups[0].colliders[static_cast<std::size_t>(k)].offset);
        CHECK((world - (a + (b - a) * fractions[k])).length() < 1.0e-4f);
    }

    // Hair (chains 0 and 1, rooted under the neck) never references the new groups.
    CHECK(model->springChains[0].colliderGroups.empty());
    CHECK(model->springChains[1].colliderGroups.empty());
    CHECK((model->springChains[2].colliderGroups == std::vector<int>{0, 1}));
}

TEST_CASE(spring.thigh_colliders_never_move_hair_at_runtime) {
    // Two identical models, one with the synthesis applied. Hair is not
    // referenced by the new groups, so its simulated pose must be identical.
    auto plain = hairAndSkirtModel();
    auto synthesized = hairAndSkirtModel();
    REQUIRE(plain != nullptr && synthesized != nullptr);
    REQUIRE(vrm::synthesizeThighColliders(*synthesized, thighOn()).applied);

    animation::HumanoidRetargeter rigPlain(*plain);
    animation::HumanoidRetargeter rigSynth(*synthesized);
    animation::SecondaryDynamics dynPlain(*plain);
    animation::SecondaryDynamics dynSynth(*synthesized);
    animation::SecondarySettings settings;
    const double dt = 1.0 / 60.0;
    const std::vector<int> hair = {3, 4, 19, 20};
    float worstHair = 0.0f;
    float worstSkirt = 0.0f;
    for (int i = 0; i < 300; ++i) {
        const double t = i * dt;
        auto a = rigPlain.applyClip(hipSwayPose(t), t);
        auto b = rigSynth.applyClip(hipSwayPose(t), t);
        dynPlain.update(a, i == 0 ? 0.0 : dt, settings);
        dynSynth.update(b, i == 0 ? 0.0 : dt, settings);
        for (int n : hair) {
            const auto index = static_cast<std::size_t>(n);
            worstHair = std::max(worstHair, (a.nodeGlobals[index].translationPart() -
                                             b.nodeGlobals[index].translationPart()).length());
        }
        for (int n = 22; n <= 24; ++n) {
            const auto index = static_cast<std::size_t>(n);
            worstSkirt = std::max(worstSkirt, (a.nodeGlobals[index].translationPart() -
                                               b.nodeGlobals[index].translationPart()).length());
        }
    }
    std::cout << std::fixed << std::setprecision(6)
              << "\n            hair difference with/without thigh colliders: " << worstHair
              << " m, skirt difference: " << worstSkirt << " m";
    CHECK(worstHair < 1.0e-6f);
    CHECK(!dynSynth.stats().nonFinite);
}

TEST_CASE(spring.thigh_colliders_are_idempotent) {
    auto model = hairAndSkirtModel();
    REQUIRE(model != nullptr);
    REQUIRE(vrm::synthesizeThighColliders(*model, thighOn()).applied);
    const auto groupsAfterFirst = model->springColliderGroups.size();
    const auto refsAfterFirst = model->springChains.back().colliderGroups;
    const auto second = vrm::synthesizeThighColliders(*model, thighOn());
    CHECK(!second.applied);
    CHECK(model->springColliderGroups.size() == groupsAfterFirst);
    CHECK(model->springChains.back().colliderGroups == refsAfterFirst);
}

TEST_CASE(spring.thigh_colliders_keep_existing_groups_and_references) {
    // Group 0 is an invalid placeholder; group 1 is an authored chest collider
    // that the hair references. The generated thigh groups must come after them.
    auto model = loadSynthetic(
        R"("colliderGroups":[{"node":9999,"colliders":[]},)"
        R"({"node":2,"colliders":[{"offset":{"x":0,"y":0,"z":0},"radius":0.1}]}],)"
        R"("boneGroups":[{"comment":"hair","stiffiness":0.5,"gravityPower":1,"dragForce":0.4,)"
        R"("center":-1,"hitRadius":0.02,"bones":[3],"colliderGroups":[1]}])");
    REQUIRE(model != nullptr);
    const auto originalGroup = model->springColliderGroups[1];
    appendHangingChain(*model, 0.08f, 0.07f);
    model->springChains.back().colliderGroups = {1};  // an authored reference that must survive
    REQUIRE(vrm::synthesizeThighColliders(*model, thighOn()).applied);
    REQUIRE(model->springColliderGroups.size() == 4);
    CHECK(model->springColliderGroups[0].colliders.empty());
    CHECK(model->springColliderGroups[0].node == -1);  // placeholder, not re-pointed
    CHECK(model->springColliderGroups[1].node == originalGroup.node);
    CHECK(model->springColliderGroups[1].colliders.size() == originalGroup.colliders.size());
    CHECK(model->springColliderGroups[2].node == model->nodeFor(HumanoidBone::LeftUpperLeg));
    CHECK(model->springColliderGroups[3].node == model->nodeFor(HumanoidBone::RightUpperLeg));
    CHECK((model->springChains.back().colliderGroups == std::vector<int>{1, 2, 3}));
    // The hair chains keep exactly their authored reference.
    CHECK((model->springChains[0].colliderGroups == std::vector<int>{1}));
    CHECK((model->springChains[1].colliderGroups == std::vector<int>{1}));
    // The simulator still builds on the result: 0 + 1 + 3 + 3 spheres.
    animation::SecondaryDynamics dynamics(*model);
    CHECK(dynamics.available());
    CHECK(dynamics.colliderCount() == 0 + 1 + 3 + 3);
}

TEST_CASE(spring.thigh_colliders_are_a_safe_no_op_on_broken_input) {
    // Missing upper-leg bone.
    auto noLegs = hairAndSkirtModel();
    REQUIRE(noLegs != nullptr);
    noLegs->humanoidNodes[static_cast<std::size_t>(HumanoidBone::LeftUpperLeg)] = -1;
    auto report = vrm::synthesizeThighColliders(*noLegs, thighOn());
    CHECK(!report.applied);
    CHECK(report.skipReason == "missing hips or leg bones");
    CHECK(noLegs->springColliderGroups.empty());

    // A model whose only chain points at nodes that do not exist: no lower-body chain.
    auto badChain = hairAndSkirtModel();
    REQUIRE(badChain != nullptr);
    vrm::SpringBoneChain broken;
    broken.nodes = {9999, 10000};
    badChain->springChains.clear();
    badChain->springChains.push_back(broken);
    report = vrm::synthesizeThighColliders(*badChain, thighOn());
    CHECK(!report.applied);
    CHECK(badChain->springColliderGroups.empty());

    // Out-of-range or negative references in an authored chain are left alone,
    // and the simulator tolerates them alongside the new groups.
    auto refs = hairAndSkirtModel();
    REQUIRE(refs != nullptr);
    refs->springChains.back().colliderGroups = {99, -4};
    report = vrm::synthesizeThighColliders(*refs, thighOn());
    REQUIRE(report.applied);
    CHECK((refs->springChains.back().colliderGroups == std::vector<int>{99, -4, 0, 1}));
    animation::SecondaryDynamics dynamics(*refs);
    CHECK(dynamics.available());
}

TEST_CASE(spring.thigh_colliders_fit_the_radius_from_the_skin) {
    // A skinned primitive whose vertices all sit 5 cm from the left thigh axis,
    // weighted to the left upper leg, drives the mesh-fitted radius.
    auto model = hairAndSkirtModel();
    REQUIRE(model != nullptr);
    const int upper = model->nodeFor(HumanoidBone::LeftUpperLeg);
    const int lower = model->nodeFor(HumanoidBone::LeftLowerLeg);
    const math::Vec3 mid =
        (model->bindGlobals[static_cast<std::size_t>(upper)].translationPart() +
         model->bindGlobals[static_cast<std::size_t>(lower)].translationPart()) * 0.5f;
    const math::Mat4 inverse = model->bindGlobals[static_cast<std::size_t>(upper)].inverseAffine();
    vrm::Primitive primitive;
    primitive.skinned = true;
    for (int i = 0; i < 64; ++i) {
        const float angle = 6.2831853f * static_cast<float>(i) / 64.0f;
        const math::Vec3 world = mid + math::Vec3{0.05f * std::cos(angle), 0.0f, 0.05f * std::sin(angle)};
        vrm::Vertex vertex;
        vertex.position = inverse.transformPoint(world);
        vertex.joints = {0, 0, 0, 0};
        vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
        primitive.vertices.push_back(vertex);
    }
    vrm::Skin skin;
    skin.joints = {upper};
    skin.inverseBindMatrices = {math::Mat4::identity()};
    model->skins.push_back(skin);
    vrm::Mesh mesh;
    mesh.name = "thigh";
    mesh.primitives.push_back(primitive);
    model->meshes.push_back(mesh);
    const int host = appendNode(*model, 0, {0.0f, 0.0f, 0.0f});
    model->nodes[static_cast<std::size_t>(host)].skin = 0;
    model->meshInstances.push_back({host, static_cast<int>(model->meshes.size()) - 1});

    const auto report = vrm::synthesizeThighColliders(*model, thighOn());
    REQUIRE(report.applied);
    CHECK(report.leftRadiusFromMesh);
    CHECK(!report.rightRadiusFromMesh);
    CHECK_NEAR(report.leftRadius, 0.05f, 1.0e-3);
    // The fitted radius stays within the 2 %..9 % of height bounds.
    CHECK(report.leftRadius >= 0.02f * model->height - 1.0e-6f);
    CHECK(report.leftRadius <= 0.09f * model->height + 1.0e-6f);
}

TEST_CASE(spring.thigh_colliders_stay_stable_at_30_60_and_120_fps) {
    // The skirt runs along the thigh (z = 0) so the spheres are actually hit while the hips sway.
    auto model = hairAndSkirtModel(0.0f);
    REQUIRE(model != nullptr);
    REQUIRE(vrm::synthesizeThighColliders(*model, thighOn()).applied);
    animation::HumanoidRetargeter rig(*model);
    const int tip = 24;
    std::vector<math::Vec3> tips;
    for (double fps : {30.0, 60.0, 120.0}) {
        animation::SecondaryDynamics dynamics(*model);
        animation::SecondarySettings settings;
        math::Vec3 last{};
        const int frames = static_cast<int>(4.0 * fps);
        for (int i = 0; i <= frames; ++i) {
            const double t = i / fps;
            auto pose = rig.applyClip(hipSwayPose(t), t);
            dynamics.update(pose, i == 0 ? 0.0 : 1.0 / fps, settings);
            CHECK(!dynamics.stats().nonFinite);
            CHECK(dynamics.stats().maxStretch < 0.05f);
            CHECK(dynamics.stats().maxDisplacement <= settings.maxDisplacement + 1.0e-4f);
            last = animation::HumanoidRetargeter::stagePosition(pose, tip);
        }
        tips.push_back(last);
    }
    float spread = 0.0f;
    for (const auto& a : tips) {
        for (const auto& b : tips) spread = std::max(spread, (a - b).length());
    }
    std::cout << std::fixed << std::setprecision(5)
              << "\n            skirt tip spread across 30/60/120 FPS with thigh colliders: " << spread << " m";
    CHECK(spread < 0.02f);
}

TEST_CASE(spring.thigh_colliders_push_a_skirt_out_of_the_thigh) {
    // The skirt starts on the right thigh axis, i.e. inside the leg. Without the
    // colliders it stays there; with them the hanging points are pushed out.
    const auto clearanceFromThigh = [](bool synthesize) {
        auto model = loadSynthetic(
            R"("boneGroups":[{"comment":"hair","stiffiness":0.5,"gravityPower":1,"dragForce":0.4,)"
            R"("center":-1,"hitRadius":0.02,"bones":[3],"colliderGroups":[]}])");
        REQUIRE(model != nullptr);
        // A skirt chain on the right thigh axis (nodes 22..24).
        appendHangingChain(*model, 0.08f, 0.0f);
        if (synthesize) {
            REQUIRE(vrm::synthesizeThighColliders(*model, thighOn()).applied);
        }
        animation::HumanoidRetargeter rig(*model);
        animation::SecondaryDynamics dynamics(*model);
        animation::SecondarySettings settings;
        const double dt = 1.0 / 60.0;
        const int thigh = model->nodeFor(HumanoidBone::RightUpperLeg);
        const int knee = model->nodeFor(HumanoidBone::RightLowerLeg);
        float clearance = 1.0e9f;
        for (int i = 0; i < 120; ++i) {
            auto pose = rig.applyClip(motion::neutralClipPose(), i * dt);
            dynamics.update(pose, i == 0 ? 0.0 : dt, settings);
            if (i < 100) {
                continue;
            }
            const math::Vec3 a = pose.nodeGlobals[static_cast<std::size_t>(thigh)].translationPart();
            const math::Vec3 b = pose.nodeGlobals[static_cast<std::size_t>(knee)].translationPart();
            for (int n = 23; n <= 24; ++n) {
                const math::Vec3 p = pose.nodeGlobals[static_cast<std::size_t>(n)].translationPart();
                const math::Vec3 ab = b - a;
                const float t = std::clamp(math::dot(p - a, ab) / ab.lengthSquared(), 0.0f, 1.0f);
                clearance = std::min(clearance, (p - (a + ab * t)).length());
            }
        }
        return clearance;
    };
    const float without = clearanceFromThigh(false);
    const float with = clearanceFromThigh(true);
    std::cout << std::fixed << std::setprecision(4) << "\n            skirt clearance from the right thigh axis: without="
              << without << " m, with thigh colliders=" << with << " m";
    CHECK(with > without + 0.03f);  // the colliders push it out to about radius + hitRadius
}
