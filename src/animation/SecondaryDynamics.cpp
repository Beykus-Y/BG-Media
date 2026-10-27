#include "bgmedia/animation/SecondaryDynamics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace bgmedia::animation {
namespace {

using Clock = std::chrono::steady_clock;
using math::Mat4;
using math::Quat;
using math::Vec3;

constexpr double kFixedStep = 1.0 / 120.0;
constexpr double kMaxAccumulated = 0.10;
constexpr float kTiny = 1.0e-6f;

bool finiteVec(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Vec3 safeNormalize(Vec3 value, Vec3 fallback = {0.0f, -1.0f, 0.0f}) {
    const float length = value.length();
    if (!std::isfinite(length) || length < kTiny) {
        return fallback;
    }
    return value / length;
}

} // namespace

struct SecondaryDynamics::Impl {
    const vrm::VrmModel* model = nullptr;
    struct Chain {
        int group = -1;
        std::vector<int> nodes;
        std::vector<float> rest;
        std::vector<int> colliderGroups;
        float stiffness = 0.5f;
        float gravityPower = 0.0f;
        float dragForce = 0.1f;
        float hitRadius = 0.01f;
        Vec3 gravityDir{0.0f, -1.0f, 0.0f};
        std::string comment;
        std::vector<Vec3> animated;
        std::vector<Vec3> current;
        std::vector<Vec3> previous;
        std::vector<Vec3> velocity;
        std::vector<Quat> animatedRotations;
        std::vector<Vec3> scales;
        float totalRest = 0.0f;
    };

    std::vector<Chain> chains;
    std::vector<Vec3> colliderCenters;   // per collider group index, per collider
    std::vector<float> colliderRadii;
    std::vector<std::vector<int>> colliderLookup;  // group index -> flat collider indices
    double accumulator = 0.0;
    SecondaryStats stats;
    bool initialized = false;
    Vec3 rootOffset{};
    Vec3 rootPivot{};
    Quat rootRotation = Quat::identity();

    void build();
    void readTargets(const RetargetedPose& pose);
    void ensureState(const RetargetedPose& pose);
    void step(Chain& chain, const SecondarySettings& settings, float stepSeconds);
    void solveConstraints(Chain& chain, float stepSeconds);
    void writePose(RetargetedPose& pose, const SecondarySettings& settings);
    [[nodiscard]] Vec3 toStage(Vec3 character) const {
        return rootOffset + rootPivot +
               math::rotate(rootRotation, character - rootPivot);
    }
    [[nodiscard]] Vec3 toCharacter(Vec3 stage) const {
        return rootPivot +
               math::rotate(rootRotation.conjugate(), stage - rootPivot - rootOffset);
    }
};

void SecondaryDynamics::Impl::build() {
    chains.clear();
    colliderCenters.clear();
    colliderRadii.clear();
    colliderLookup.clear();
    if (!model) {
        return;
    }
    for (const vrm::SpringColliderGroup& group : model->springColliderGroups) {
        colliderLookup.emplace_back();
        for (const vrm::SpringCollider& collider : group.colliders) {
            colliderLookup.back().push_back(static_cast<int>(colliderCenters.size()));
            colliderCenters.push_back({});
            colliderRadii.push_back(collider.radius);
        }
    }
    int groupIndex = 0;
    for (const vrm::SpringBoneChain& source : model->springChains) {
        if (source.nodes.size() < 2) {
            ++groupIndex;
            continue;
        }
        Chain chain;
        chain.group = source.group;
        chain.nodes = source.nodes;
        chain.colliderGroups = source.colliderGroups;
        chain.stiffness = std::clamp(source.stiffness, 0.0f, 1.0f);
        chain.gravityPower = std::max(0.0f, source.gravityPower);
        chain.dragForce = std::clamp(source.dragForce, 0.0f, 1.0f);
        chain.hitRadius = std::max(0.0f, source.hitRadius);
        chain.gravityDir = safeNormalize(source.gravityDir);
        chain.comment = source.group >= 0 &&
                                static_cast<std::size_t>(source.group) <
                                    model->springGroups.size()
                            ? model->springGroups[static_cast<std::size_t>(source.group)].comment
                            : std::string{};
        chain.rest.resize(source.nodes.size() - 1);
        bool valid = true;
        for (std::size_t i = 0; i + 1 < source.nodes.size(); ++i) {
            const int a = source.nodes[i];
            const int b = source.nodes[i + 1];
            if (a < 0 || b < 0 ||
                static_cast<std::size_t>(a) >= model->bindGlobals.size() ||
                static_cast<std::size_t>(b) >= model->bindGlobals.size()) {
                valid = false;
                break;
            }
            const Vec3 pa = model->bindGlobals[static_cast<std::size_t>(a)].translationPart();
            const Vec3 pb = model->bindGlobals[static_cast<std::size_t>(b)].translationPart();
            const float rest = (pb - pa).length();
            if (!std::isfinite(rest) || rest < 1.0e-5f) {
                valid = false;
                break;
            }
            chain.rest[i] = rest;
        }
        if (!valid) {
            ++groupIndex;
            continue;
        }
        chain.totalRest = 0.0f;
        for (float rest : chain.rest) {
            chain.totalRest += rest;
        }
        chains.push_back(std::move(chain));
        ++groupIndex;
    }
    stats.available = !chains.empty();
    stats.chains = static_cast<int>(chains.size());
    stats.bones = 0;
    for (const Chain& chain : chains) {
        stats.bones += static_cast<int>(chain.nodes.size());
    }
    stats.colliders = static_cast<int>(colliderCenters.size());
    stats.height = model->height;
    initialized = false;
}

void SecondaryDynamics::Impl::readTargets(const RetargetedPose& pose) {
    if (!model) {
        return;
    }
    rootOffset = pose.rootOffset;
    rootPivot = pose.rootPivot;
    rootRotation = pose.modelTransform.rotationQuat().normalized();
    // Collider spheres live on body bones and are read from the animated pose,
    // never from the simulated result, so hair cannot push itself around.
    std::size_t flat = 0;
    for (const vrm::SpringColliderGroup& group : model->springColliderGroups) {
        for (const vrm::SpringCollider& collider : group.colliders) {
            Vec3 center{};
            if (group.node >= 0 &&
                static_cast<std::size_t>(group.node) < pose.nodeGlobals.size()) {
                const Mat4& matrix = pose.nodeGlobals[static_cast<std::size_t>(group.node)];
                center = matrix.transformVector(collider.offset) + matrix.translationPart();
                center = toStage(center);
            }
            if (flat < colliderCenters.size()) {
                colliderCenters[flat] = center;
            }
            ++flat;
        }
    }
    for (Chain& chain : chains) {
        const std::size_t count = chain.nodes.size();
        chain.animated.resize(count);
        chain.animatedRotations.resize(count);
        chain.scales.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const int node = chain.nodes[i];
            if (node < 0 || static_cast<std::size_t>(node) >= pose.nodeGlobals.size()) {
                chain.animated[i] = {};
                chain.animatedRotations[i] = Quat::identity();
                chain.scales[i] = {1.0f, 1.0f, 1.0f};
                continue;
            }
            const Mat4& matrix = pose.nodeGlobals[static_cast<std::size_t>(node)];
            chain.animated[i] = toStage(matrix.translationPart());
            chain.animatedRotations[i] = matrix.rotationQuat();
            chain.scales[i] = {matrix.axisX().length(), matrix.axisY().length(),
                               matrix.axisZ().length()};
        }
    }
}

void SecondaryDynamics::Impl::ensureState(const RetargetedPose& pose) {
    if (!initialized) {
        for (Chain& chain : chains) {
            chain.current = chain.animated;
            chain.previous = chain.animated;
            chain.velocity.assign(chain.animated.size(), Vec3{});
        }
        initialized = true;
    }
    (void)pose;
}

void SecondaryDynamics::Impl::solveConstraints(Chain& chain, float stepSeconds) {
    (void)stepSeconds;
    if (chain.nodes.empty()) {
        return;
    }
    chain.current[0] = chain.animated[0];
    // Two alternating passes converge quickly for short VRM chains (4-6 nodes).
    for (int pass = 0; pass < 3; ++pass) {
        for (std::size_t i = 1; i < chain.current.size(); ++i) {
            Vec3 direction = chain.current[i] - chain.current[i - 1];
            const float length = direction.length();
            if (!std::isfinite(length) || length < kTiny) {
                direction = safeNormalize(chain.animated[i] - chain.animated[i - 1]);
                if (direction.lengthSquared() < 0.5f) {
                    direction = {0.0f, -1.0f, 0.0f};
                }
            } else {
                direction = direction / length;
            }
            chain.current[i] = chain.current[i - 1] + direction * chain.rest[i - 1];
        }
        for (std::size_t i = chain.current.size(); i-- > 1;) {
            Vec3 direction = chain.current[i - 1] - chain.current[i];
            const float length = direction.length();
            if (length < kTiny) {
                continue;
            }
            direction = direction / length;
            chain.current[i - 1] = chain.current[i] + direction * chain.rest[i - 1];
        }
        chain.current[0] = chain.animated[0];
        // Sphere collision after the length pass. Re-projecting may introduce a
        // small overlap, so the final pass below repeats it once more.
        for (int groupIndex : chain.colliderGroups) {
            if (groupIndex < 0 ||
                static_cast<std::size_t>(groupIndex) >= colliderLookup.size()) {
                continue;
            }
            for (int flat : colliderLookup[static_cast<std::size_t>(groupIndex)]) {
                if (flat < 0 || static_cast<std::size_t>(flat) >= colliderCenters.size()) {
                    continue;
                }
                const Vec3 center = colliderCenters[static_cast<std::size_t>(flat)];
                const float radius = colliderRadii[static_cast<std::size_t>(flat)] +
                                     chain.hitRadius;
                for (std::size_t i = 1; i < chain.current.size(); ++i) {
                    Vec3 delta = chain.current[i] - center;
                    const float distance = delta.length();
                    if (distance < radius && distance > kTiny) {
                        chain.current[i] = center + delta / distance * radius;
                    } else if (distance <= kTiny) {
                        chain.current[i] = center + Vec3{0.0f, radius, 0.0f};
                    }
                }
            }
        }
    }
}

void SecondaryDynamics::Impl::step(Chain& chain, const SecondarySettings& settings,
                                   float stepSeconds) {
    if (chain.nodes.empty()) {
        return;
    }
    chain.current[0] = chain.animated[0];
    chain.previous[0] = chain.animated[0];
    const float stiffness = 10.0f + chain.stiffness * 26.0f;
    const float drag = std::clamp(0.8f + chain.dragForce * 8.0f +
                                      std::clamp(settings.damping, 0.0f, 1.0f) * 10.0f,
                                  0.1f, 30.0f);
    const float decay = std::exp(-drag * stepSeconds);
    const Vec3 gravity = chain.gravityDir *
                         (9.81f * chain.gravityPower * 0.08f *
                          std::max(0.0f, settings.gravityScale));
    const float maxDisplacement =
        std::min(std::max(0.005f, settings.maxDisplacement),
                 std::max(0.006f, chain.totalRest * 0.45f));
    for (std::size_t i = 1; i < chain.current.size(); ++i) {
        Vec3 velocity = chain.current[i] - chain.previous[i];
        if (!finiteVec(velocity)) {
            velocity = {};
        }
        velocity += (chain.animated[i] - chain.current[i]) * (stiffness * stepSeconds);
        velocity = velocity * decay;
        velocity += gravity * stepSeconds;
        const float speed = velocity.length();
        const float maxSpeed = 12.0f;
        if (speed > maxSpeed) {
            velocity = velocity / speed * maxSpeed;
        }
        chain.previous[i] = chain.current[i];
        chain.current[i] += velocity * stepSeconds;
        // Keep values bounded even if a host feeds a pathological chain.
        Vec3 offset = chain.current[i] - chain.animated[i];
        const float distance = offset.length();
        if (!std::isfinite(distance)) {
            chain.current[i] = chain.animated[i];
            chain.previous[i] = chain.animated[i];
        } else if (distance > maxDisplacement) {
            chain.current[i] = chain.animated[i] + offset / distance * maxDisplacement;
        }
        chain.velocity[i] = velocity;
    }
    solveConstraints(chain, stepSeconds);
    for (std::size_t i = 1; i < chain.current.size(); ++i) {
        chain.velocity[i] = (chain.current[i] - chain.previous[i]) / stepSeconds;
    }
}

void SecondaryDynamics::Impl::writePose(RetargetedPose& pose,
                                        const SecondarySettings& settings) {
    const float intensity = std::clamp(settings.intensity, 0.0f, 2.0f);
    for (Chain& chain : chains) {
        const std::size_t count = chain.nodes.size();
        std::vector<Vec3> rendered(count);
        for (std::size_t i = 0; i < count; ++i) {
            Vec3 offset = chain.current[i] - chain.animated[i];
            if (!finiteVec(offset)) {
                offset = {};
            }
            rendered[i] = chain.animated[i] + offset * intensity;
        }
        const float maxDisplacement =
            std::min(std::max(0.005f, settings.maxDisplacement),
                     std::max(0.006f, chain.totalRest * 0.45f)) *
            std::max(1.0f, intensity);
        for (std::size_t i = 0; i < count; ++i) {
            Vec3 offset = rendered[i] - chain.animated[i];
            const float distance = offset.length();
            if (distance > maxDisplacement) {
                rendered[i] = chain.animated[i] + offset / distance * maxDisplacement;
            }
        }
        // Rotate each segment so the skinned vertices follow the simulated
        // direction. The tip inherits its parent's delta.
        Quat inherited = Quat::identity();
        for (std::size_t i = 0; i < count; ++i) {
            Quat rotation = chain.animatedRotations[i];
            if (i + 1 < count) {
                const Vec3 from = safeNormalize(chain.animated[i + 1] - chain.animated[i]);
                const Vec3 to = safeNormalize(rendered[i + 1] - rendered[i], from);
                if (from.lengthSquared() > 0.5f && to.lengthSquared() > 0.5f) {
                    const Quat delta = math::fromTo(from, to);
                    rotation = (delta * rotation).normalized();
                    inherited = delta;
                }
            } else {
                rotation = (inherited * rotation).normalized();
            }
            const int node = chain.nodes[i];
            if (node < 0 || static_cast<std::size_t>(node) >= pose.nodeGlobals.size()) {
                continue;
            }
            const Vec3 position = toCharacter(rendered[i]);
            if (!finiteVec(position) || !std::isfinite(rotation.w)) {
                continue;
            }
            pose.nodeGlobals[static_cast<std::size_t>(node)] =
                Mat4::fromTRS(position, rotation, chain.scales[i]);
        }
        // Stats.
        for (std::size_t i = 1; i < count; ++i) {
            const float displacement =
                (rendered[i] - chain.animated[i]).length();
            stats.maxDisplacement = std::max(stats.maxDisplacement, displacement);
            const float speed = chain.velocity[i].length();
            stats.maxSpeed = std::max(stats.maxSpeed, speed);
            const float length = (chain.current[i] - chain.current[i - 1]).length();
            const float rest = chain.rest[i - 1];
            if (rest > kTiny) {
                stats.maxStretch = std::max(stats.maxStretch,
                                            std::abs(length - rest) / rest);
            }
        }
        if (chain.current.size() > 1) {
            ++stats.activeChains;
        }
    }
}

SecondaryDynamics::SecondaryDynamics(const vrm::VrmModel& model)
    : impl_(std::make_unique<Impl>()) {
    impl_->model = &model;
    impl_->build();
}

SecondaryDynamics::~SecondaryDynamics() = default;
SecondaryDynamics::SecondaryDynamics(SecondaryDynamics&&) noexcept = default;
SecondaryDynamics& SecondaryDynamics::operator=(SecondaryDynamics&&) noexcept = default;

bool SecondaryDynamics::available() const { return impl_ && impl_->stats.available; }
int SecondaryDynamics::chainCount() const {
    return impl_ ? static_cast<int>(impl_->chains.size()) : 0;
}
int SecondaryDynamics::boneCount() const { return impl_ ? impl_->stats.bones : 0; }
int SecondaryDynamics::colliderCount() const { return impl_ ? impl_->stats.colliders : 0; }

void SecondaryDynamics::reset(const RetargetedPose& pose) {
    if (!impl_ || !impl_->stats.available || pose.nodeGlobals.empty()) {
        return;
    }
    impl_->readTargets(pose);
    impl_->ensureState(pose);
    for (auto& chain : impl_->chains) {
        chain.current = chain.animated;
        chain.previous = chain.animated;
        chain.velocity.assign(chain.animated.size(), Vec3{});
    }
    impl_->accumulator = 0.0;
    impl_->stats.valid = true;
}

void SecondaryDynamics::update(RetargetedPose& pose, double deltaSeconds,
                               const SecondarySettings& settings) {
    if (!impl_ || !impl_->stats.available) {
        return;
    }
    const auto started = Clock::now();
    impl_->stats = {};
    impl_->stats.available = true;
    impl_->stats.chains = static_cast<int>(impl_->chains.size());
    impl_->stats.bones = 0;
    for (const auto& chain : impl_->chains) {
        impl_->stats.bones += static_cast<int>(chain.nodes.size());
    }
    impl_->stats.colliders = static_cast<int>(impl_->colliderCenters.size());
    impl_->stats.height = impl_->model ? impl_->model->height : 0.0f;
    impl_->readTargets(pose);
    impl_->ensureState(pose);
    if (!settings.enabled) {
        impl_->accumulator = 0.0;
        impl_->stats.valid = true;
        impl_->stats.simulateMilliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return;
    }
    if (!(deltaSeconds > 0.0) || !std::isfinite(deltaSeconds)) {
        impl_->writePose(pose, settings);
        impl_->stats.valid = true;
        impl_->stats.simulateMilliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return;
    }
    impl_->accumulator = std::min(impl_->accumulator + deltaSeconds, kMaxAccumulated);
    int steps = 0;
    const int maxSteps = std::clamp(settings.maxSubsteps, 1, 32);
    while (impl_->accumulator >= kFixedStep && steps < maxSteps) {
        for (auto& chain : impl_->chains) {
            impl_->step(chain, settings, static_cast<float>(kFixedStep));
        }
        impl_->accumulator -= kFixedStep;
        ++steps;
    }
    if (steps >= maxSteps && impl_->accumulator > kFixedStep) {
        impl_->accumulator = std::fmod(impl_->accumulator, kFixedStep);
    }
    impl_->stats.substeps = steps;
    impl_->writePose(pose, settings);
    // Numerical health.
    for (const auto& chain : impl_->chains) {
        for (Vec3 point : chain.current) {
            if (!finiteVec(point)) {
                impl_->stats.nonFinite = true;
                break;
            }
        }
    }
    if (impl_->stats.nonFinite) {
        for (auto& chain : impl_->chains) {
            chain.current = chain.animated;
            chain.previous = chain.animated;
            chain.velocity.assign(chain.animated.size(), Vec3{});
        }
        impl_->accumulator = 0.0;
    }
    impl_->stats.valid = true;
    impl_->stats.simulateMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

const SecondaryStats& SecondaryDynamics::stats() const {
    static const SecondaryStats empty;
    return impl_ ? impl_->stats : empty;
}

std::vector<SecondaryDynamics::ChainInfo> SecondaryDynamics::chains() const {
    std::vector<ChainInfo> out;
    if (!impl_) {
        return out;
    }
    for (const auto& chain : impl_->chains) {
        ChainInfo info;
        info.group = chain.group;
        info.nodes = static_cast<int>(chain.nodes.size());
        info.comment = chain.comment;
        out.push_back(std::move(info));
    }
    return out;
}

math::Vec3 SecondaryDynamics::chainPoint(int chain, int index) const {
    if (!impl_ || chain < 0 ||
        static_cast<std::size_t>(chain) >= impl_->chains.size()) {
        return {};
    }
    const auto& nodes = impl_->chains[static_cast<std::size_t>(chain)].current;
    if (index < 0 || static_cast<std::size_t>(index) >= nodes.size()) {
        return {};
    }
    return nodes[static_cast<std::size_t>(index)];
}

} // namespace bgmedia::animation
