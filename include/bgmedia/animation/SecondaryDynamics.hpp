#pragma once

// BG Motion 3.2 - secondary dynamics for VRM spring bones.
//
// The simulator consumes the FINAL retargeted node globals, simulates every
// authored spring chain with a fixed timestep and writes the resulting node
// transforms back into the pose. It therefore reacts to the actual body motion
// (including root motion, foot IK and musical accents) and cannot be
// overwritten by a later retargeting stage.
//
// Scope: VRM 0.x `extensions.VRM.secondaryAnimation` (boneGroups +
// colliderGroups), which is what the desktop model uses. It is not the VRM 1.0
// `VRMC_springBone` schema, and the two are deliberately not mixed.
//
// The solver is a position-based chain with semi-implicit integration,
// exponential drag, length projection and sphere collision. It is not a cloth
// or soft-body FEM solver; it is the same class of approximation as UniVRM's
// spring bones and is documented as such.

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace bgmedia::animation {

struct SecondarySettings {
    bool enabled = true;
    float intensity = 1.0f;       // 0..2; 0 = authored spring, 2 = exaggerated
    float damping = 0.5f;         // 0..1 preset; higher settles faster
    float gravityScale = 1.0f;    // 0..2, multiplies authored gravityPower
    bool collisions = true;
    float maxDisplacement = 0.05f;  // metres per node from the animated target
    int maxSubsteps = 8;
};

struct SecondaryStats {
    bool valid = false;
    bool available = false;
    bool nonFinite = false;
    int chains = 0;
    int bones = 0;
    int colliders = 0;
    int activeChains = 0;
    int substeps = 0;
    float maxDisplacement = 0.0f;   // metres
    float maxSpeed = 0.0f;          // metres/second
    float maxStretch = 0.0f;        // relative error of the rest length
    float height = 0.0f;
    double simulateMilliseconds = 0.0;
};

class SecondaryDynamics {
public:
    explicit SecondaryDynamics(const vrm::VrmModel& model);
    ~SecondaryDynamics();
    SecondaryDynamics(SecondaryDynamics&&) noexcept;
    SecondaryDynamics& operator=(SecondaryDynamics&&) noexcept;
    SecondaryDynamics(const SecondaryDynamics&) = delete;
    SecondaryDynamics& operator=(const SecondaryDynamics&) = delete;

    [[nodiscard]] bool available() const;
    [[nodiscard]] int chainCount() const;
    [[nodiscard]] int boneCount() const;
    [[nodiscard]] int colliderCount() const;

    // Copies the current animated transforms into the simulation state. Call
    // after a seek, a model reload, a teleport or a large time jump.
    void reset(const RetargetedPose& pose);

    // Advances the simulation by `deltaSeconds` and writes the spring node globals
    // back into `pose`. The host passes WALL-clock time, not media time, so hair
    // keeps settling while playback is paused and does not speed up with playback
    // speed. Each fixed step samples the animated pose at its own time, and the written
    // offset from the animated pose is interpolated between the last two steps. The
    // output is smooth at any frame rate, and a steadily moving chain carries no lag
    // from the frame cadence.
    // When disabled, the pose is left untouched and the accumulator is kept bounded.
    void update(RetargetedPose& pose, double deltaSeconds,
                const SecondarySettings& settings);

    [[nodiscard]] const SecondaryStats& stats() const;

    // Diagnostic access: chain description and current simulated point.
    struct ChainInfo {
        int group = -1;
        int nodes = 0;
        std::string comment;
    };
    [[nodiscard]] std::vector<ChainInfo> chains() const;
    [[nodiscard]] math::Vec3 chainPoint(int chain, int index) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bgmedia::animation
