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
// VRM 0.x: a spring bone without a child simulates a virtual tail 7 cm along the
// parent->bone direction.
constexpr float kVirtualTailLength = 0.07f;
// An animated chain root that moves further than this (in character heights)
// between two updates is a teleport/seek, not motion: the state is snapped.
constexpr float kTeleportHeights = 0.25f;
// A frame gap longer than this is a pause/resume or a seek.
constexpr double kHitchSeconds = 0.25;

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
        // Simulated points: one per node, plus one virtual tail point for a
        // single-node chain. `animated` etc. are sized to the point count;
        // `animatedRotations` / `scales` to the node count.
        bool virtualTail = false;
        int parentChain = -1;   // index into `chains` (not springChains), -1 = root chain
        int parentIndex = 0;
        bool attached = false;  // see vrm::SpringBoneChain::attached
        std::vector<Vec3> animated;
        // Animated target of the previous frame. Each fixed step samples the target at
        // its own time between the two frames (stepTarget), so the spring never sees a
        // frame-start staircase.
        std::vector<Vec3> animatedPrevFrame;
        std::vector<Vec3> stepTarget;
        std::vector<Vec3> current;
        std::vector<Vec3> previous;
        // Target each stored state was produced against: animPrev pairs with previous,
        // animCur with current. Render interpolation works on the offsets from these.
        std::vector<Vec3> animPrev;
        std::vector<Vec3> animCur;
        std::vector<Vec3> velocity;
        std::vector<Quat> animatedRotations;
        std::vector<Vec3> scales;
        // Per node: descendants no chain simulates (a VRM 1.0 spring may end
        // before the hierarchy's leaf), parent before child. They keep their
        // local transform and follow the node. Empty for 0.x chains, which
        // run to the leaves.
        std::vector<std::vector<int>> carried;
        Vec3 lastAnimatedRoot{};
        bool haveLastRoot = false;
        float totalRest = 0.0f;
        [[nodiscard]] std::size_t pointCount() const {
            return nodes.size() + (virtualTail ? 1u : 0u);
        }
    };

    std::vector<Chain> chains;
    std::vector<Vec3> colliderCenters;   // per collider group index, per collider
    std::vector<float> colliderRadii;
    std::vector<std::vector<int>> colliderLookup;  // group index -> flat collider indices
    double accumulator = 0.0;
    float renderAlpha = 0.0f;  // accumulator / kFixedStep, set by writePose
    SecondaryStats stats;
    bool initialized = false;
    Vec3 rootOffset{};
    Vec3 rootPivot{};
    Quat rootRotation = Quat::identity();

    void build();
    void readTargets(const RetargetedPose& pose);
    void ensureState(const RetargetedPose& pose);
    // Fills chain.stepTarget with the animated target at `fraction` of the frame.
    void interpolateStepTarget(Chain& chain, double fraction);
    void step(Chain& chain, const SecondarySettings& settings, float stepSeconds,
              const std::vector<Vec3>& target);
    void solveConstraints(Chain& chain, Vec3 anchor, const std::vector<Vec3>& target);
    void snapToAnimated();
    // Where the first point of a chain is pinned: the target's root, or (branch
    // chain) the simulated position of the branch point in the parent.
    [[nodiscard]] Vec3 anchorOf(const Chain& chain, const std::vector<Vec3>& target) const;
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
    std::vector<int> sourceToChain(model->springChains.size(), -1);
    for (std::size_t sourceIndex = 0; sourceIndex < model->springChains.size(); ++sourceIndex) {
        const vrm::SpringBoneChain& source = model->springChains[sourceIndex];
        if (source.nodes.empty()) {
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
        // A branch chain is pinned to its parent's simulated branch point, but
        // only if that parent chain is itself simulated.
        if (source.parentChain >= 0 &&
            static_cast<std::size_t>(source.parentChain) < sourceIndex &&
            sourceToChain[static_cast<std::size_t>(source.parentChain)] >= 0) {
            chain.parentChain = sourceToChain[static_cast<std::size_t>(source.parentChain)];
            chain.parentIndex = source.parentIndex;
            chain.attached = source.attached;
        }
        chain.virtualTail = chain.nodes.size() == 1;
        bool valid = true;
        const auto nodeValid = [&](int n) {
            return n >= 0 && static_cast<std::size_t>(n) < model->bindGlobals.size();
        };
        for (int n : chain.nodes) {
            valid = valid && nodeValid(n);
        }
        if (!valid) {
            continue;
        }
        chain.rest.resize(chain.pointCount() - 1);
        for (std::size_t i = 0; i + 1 < chain.pointCount(); ++i) {
            if (chain.virtualTail) {
                chain.rest[i] = kVirtualTailLength;
                break;
            }
            const Vec3 pa = model->bindGlobals[static_cast<std::size_t>(chain.nodes[i])].translationPart();
            const Vec3 pb = model->bindGlobals[static_cast<std::size_t>(chain.nodes[i + 1])].translationPart();
            const float rest = (pb - pa).length();
            if (!std::isfinite(rest) || rest < 1.0e-5f) {
                valid = false;
                break;
            }
            chain.rest[i] = rest;
        }
        if (!valid) {
            continue;
        }
        chain.totalRest = 0.0f;
        for (float rest : chain.rest) {
            chain.totalRest += rest;
        }
        sourceToChain[sourceIndex] = static_cast<int>(chains.size());
        chains.push_back(std::move(chain));
    }
    std::vector<char> simulated(model->nodes.size(), 0);
    for (const Chain& chain : chains) {
        for (int n : chain.nodes) {
            simulated[static_cast<std::size_t>(n)] = 1;
        }
    }
    for (Chain& chain : chains) {
        chain.carried.assign(chain.nodes.size(), {});
        for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
            auto& list = chain.carried[i];
            std::vector<int> stack(model->nodes[static_cast<std::size_t>(chain.nodes[i])].children);
            std::reverse(stack.begin(), stack.end());
            while (!stack.empty()) {
                const int n = stack.back();
                stack.pop_back();
                if (n < 0 || static_cast<std::size_t>(n) >= simulated.size() ||
                    simulated[static_cast<std::size_t>(n)]) {
                    continue;  // invalid, or written (with its subtree) by a chain
                }
                list.push_back(n);
                const auto& children = model->nodes[static_cast<std::size_t>(n)].children;
                stack.insert(stack.end(), children.rbegin(), children.rend());
            }
        }
    }
    stats.available = !chains.empty();
    stats.chains = static_cast<int>(chains.size());
    stats.bones = 0;
    for (const Chain& chain : chains) {
        stats.bones += static_cast<int>(chain.nodes.size()) -
                       (chain.parentChain >= 0 && !chain.attached ? 1 : 0);
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
        // The previous frame's target stays available for the step-time interpolation.
        chain.animatedPrevFrame = chain.animated;
        chain.animated.assign(chain.pointCount(), Vec3{});
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
        if (chain.virtualTail) {
            // Continue the parent -> bone direction by 7 cm (VRM 0.x).
            Vec3 direction{0.0f, -1.0f, 0.0f};
            const int node = chain.nodes[0];
            const int parent = model->nodes[static_cast<std::size_t>(node)].parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < pose.nodeGlobals.size() &&
                static_cast<std::size_t>(node) < pose.nodeGlobals.size()) {
                direction = safeNormalize(
                    pose.nodeGlobals[static_cast<std::size_t>(node)].translationPart() -
                        pose.nodeGlobals[static_cast<std::size_t>(parent)].translationPart(),
                    direction);
            }
            const Vec3 tail = pose.nodeGlobals[static_cast<std::size_t>(node)].translationPart() +
                              direction * kVirtualTailLength;
            chain.animated[1] = toStage(tail);
        }
    }
}

void SecondaryDynamics::Impl::ensureState(const RetargetedPose& pose) {
    if (!initialized) {
        snapToAnimated();
        initialized = true;
    }
    (void)pose;
}

void SecondaryDynamics::Impl::snapToAnimated() {
    for (Chain& chain : chains) {
        chain.current = chain.animated;
        chain.previous = chain.animated;
        chain.animPrev = chain.animated;
        chain.animCur = chain.animated;
        chain.animatedPrevFrame = chain.animated;
        chain.stepTarget = chain.animated;
        chain.velocity.assign(chain.animated.size(), Vec3{});
        if (!chain.animated.empty()) {
            chain.lastAnimatedRoot = chain.animated[0];
            chain.haveLastRoot = true;
        }
    }
    accumulator = 0.0;
    renderAlpha = 0.0f;
}

void SecondaryDynamics::Impl::interpolateStepTarget(Chain& chain, double fraction) {
    const std::size_t count = chain.animated.size();
    chain.stepTarget.resize(count);
    const bool haveHistory = chain.animatedPrevFrame.size() == count;
    const float t = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    for (std::size_t i = 0; i < count; ++i) {
        // Exactly the current frame's target at the frame's end, so a step that lands on
        // the frame boundary matches the rendered animation bit for bit.
        chain.stepTarget[i] = (!haveHistory || t >= 1.0f)
                                  ? chain.animated[i]
                                  : math::lerp(chain.animatedPrevFrame[i], chain.animated[i], t);
    }
}

Vec3 SecondaryDynamics::Impl::anchorOf(const Chain& chain, const std::vector<Vec3>& target) const {
    if (chain.parentChain >= 0 &&
        static_cast<std::size_t>(chain.parentChain) < chains.size()) {
        const Chain& parent = chains[static_cast<std::size_t>(chain.parentChain)];
        const std::size_t index = static_cast<std::size_t>(std::max(0, chain.parentIndex));
        if (index < parent.current.size()) {
            if (!chain.attached || target.empty() || parent.stepTarget.size() != parent.current.size()) {
                return parent.current[index];
            }
            // Attached root: keep its animated offset from the ancestor, turned
            // by the ancestor's simulated segment rotation.
            const Vec3 base = parent.stepTarget[index];
            // The ancestor's outgoing segment, or its incoming one at a tip.
            const std::size_t a = index + 1 < parent.current.size() ? index : index - 1;
            Quat turn = Quat::identity();
            if (index > 0 || index + 1 < parent.current.size()) {
                turn = math::fromTo(parent.stepTarget[a + 1] - parent.stepTarget[a],
                                    parent.current[a + 1] - parent.current[a]);
            }
            return parent.current[index] + math::rotate(turn, target[0] - base);
        }
    }
    return target.empty() ? Vec3{} : target[0];
}

void SecondaryDynamics::Impl::solveConstraints(Chain& chain, Vec3 anchor,
                                               const std::vector<Vec3>& target) {
    if (chain.nodes.empty()) {
        return;
    }
    chain.current[0] = anchor;
    // Two alternating passes converge quickly for short VRM chains (4-6 nodes).
    for (int pass = 0; pass < 3; ++pass) {
        for (std::size_t i = 1; i < chain.current.size(); ++i) {
            Vec3 direction = chain.current[i] - chain.current[i - 1];
            const float length = direction.length();
            if (!std::isfinite(length) || length < kTiny) {
                direction = safeNormalize(target[i] - target[i - 1]);
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
        chain.current[0] = anchor;
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
                                   float stepSeconds, const std::vector<Vec3>& target) {
    if (chain.nodes.empty()) {
        return;
    }
    const Vec3 anchor = anchorOf(chain, target);
    chain.current[0] = anchor;
    chain.previous[0] = anchor;
    // The state before this step pairs with the target it was produced against. The
    // root of a chain is pinned to its own target, so its entry is that target.
    chain.animPrev = chain.animCur;
    if (chain.animPrev.size() != target.size()) {
        chain.animPrev = target;
    }
    chain.animPrev[0] = target[0];

    // Position-based Verlet with a fixed step. Units are explicit:
    //   displacement per step  d = current - previous           [m]
    //   damping                d *= exp(-drag * dt)             [1/s]
    //   spring toward the animated pose (angular frequency w)   a = w^2 (target - x)   [m/s^2]
    //   gravity                a += g                           [m/s^2]
    //   x' = x + d * damping + a * dt^2
    // w^2 dt^2 stays below ~0.2 for every authored stiffness at the 120 Hz step, so
    // the explicit scheme is stable; the length projection below keeps the chain
    // inextensible regardless.
    const float omega = 10.0f + chain.stiffness * 26.0f;
    const float drag = std::clamp(
        omega * (0.5f + 1.0f * chain.dragForce +
                 0.8f * std::clamp(settings.damping, 0.0f, 1.0f)),
        0.5f, 60.0f);
    const float decay = std::exp(-drag * stepSeconds);
    const Vec3 gravity = chain.gravityDir *
                         (9.81f * chain.gravityPower * 0.08f *
                          std::max(0.0f, settings.gravityScale));
    const float maxDisplacement =
        std::min(std::max(0.005f, settings.maxDisplacement),
                 std::max(0.006f, chain.totalRest * 0.45f));
    const float dt2 = stepSeconds * stepSeconds;
    const float maxSpeed = 12.0f;  // m/s
    for (std::size_t i = 1; i < chain.current.size(); ++i) {
        Vec3 displacement = chain.current[i] - chain.previous[i];  // metres per step
        if (!finiteVec(displacement)) {
            displacement = {};
        }
        displacement = displacement * decay;
        const Vec3 acceleration =
            (target[i] - chain.current[i]) * (omega * omega) + gravity;
        displacement += acceleration * dt2;
        const float speed = displacement.length() / stepSeconds;  // m/s
        if (speed > maxSpeed) {
            displacement = displacement * (maxSpeed / speed);
        }
        chain.previous[i] = chain.current[i];
        chain.current[i] += displacement;
        // Keep values bounded even if a host feeds a pathological chain.
        Vec3 offset = chain.current[i] - target[i];
        const float distance = offset.length();
        if (!std::isfinite(distance)) {
            chain.current[i] = target[i];
            chain.previous[i] = target[i];
        } else if (distance > maxDisplacement) {
            chain.current[i] = target[i] + offset / distance * maxDisplacement;
        }
    }
    solveConstraints(chain, anchor, target);
    for (std::size_t i = 1; i < chain.current.size(); ++i) {
        chain.velocity[i] = (chain.current[i] - chain.previous[i]) / stepSeconds;  // m/s
    }
    chain.animCur = target;
}

void SecondaryDynamics::Impl::writePose(RetargetedPose& pose,
                                        const SecondarySettings& settings) {
    const float intensity = std::clamp(settings.intensity, 0.0f, 2.0f);
    renderAlpha = static_cast<float>(std::clamp(accumulator / kFixedStep, 0.0, 1.0));
    // Simulation points live in stage space (root yaw/translation applied) but
    // node rotations in the pose are character space. A direction change measured
    // in stage space must be re-expressed in the character frame before it is
    // composed with an animated rotation: delta_char = R^-1 * delta_stage * R.
    const Quat rootInverse = rootRotation.conjugate();
    for (Chain& chain : chains) {
        const std::size_t count = chain.nodes.size();
        const std::size_t points = chain.pointCount();
        std::vector<Vec3> rendered(points);
        for (std::size_t i = 0; i < points; ++i) {
            // Render interpolation on the OFFSET from the animated target. Interpolating
            // absolute positions and then subtracting this frame's animated pose lags by
            // up to one fixed step: a body moving at 2 m/s would show about 1.7 cm of
            // false trailing. Each offset here is taken against the target of its own
            // step, so a steadily moving chain has a constant offset and no lag. The
            // render time lies after the last step, so the offset is one step behind
            // during fast transients only. The simulation state is never changed here.
            const Vec3 previousTarget =
                i < chain.animPrev.size() ? chain.animPrev[i] : chain.animated[i];
            const Vec3 currentTarget =
                i < chain.animCur.size() ? chain.animCur[i] : chain.animated[i];
            const Vec3 previousState =
                i < chain.previous.size() ? chain.previous[i] : chain.current[i];
            const Vec3 previousOffset = previousState - previousTarget;
            const Vec3 currentOffset = chain.current[i] - currentTarget;
            Vec3 offset = math::lerp(previousOffset, currentOffset, renderAlpha);
            if (!finiteVec(offset)) {
                offset = {};
            }
            rendered[i] = chain.animated[i] + offset * intensity;
        }
        // The written limit sits a micron-scale margin inside the authored one: the
        // round trip through the stage and character frames carries float error of
        // the same order, and a point must never be reported beyond the limit.
        const float maxDisplacement =
            std::min(std::max(0.005f, settings.maxDisplacement),
                     std::max(0.006f, chain.totalRest * 0.45f)) *
            std::max(1.0f, intensity) * (1.0f - 1.0e-4f);
        // An attached chain's root rides rigidly on its ancestor exactly as the
        // ancestor was just written (parents are written first), so the
        // attachment holds even where the ancestor's displacement was limited.
        // The rest of the chain shifts with it and keeps its simulated shape.
        if (chain.attached && points > 0 && chain.parentChain >= 0 &&
            static_cast<std::size_t>(chain.parentChain) < chains.size()) {
            const Chain& parent = chains[static_cast<std::size_t>(chain.parentChain)];
            const std::size_t k = static_cast<std::size_t>(std::max(0, chain.parentIndex));
            if (k < parent.nodes.size() && k < parent.animated.size() &&
                k < parent.animatedRotations.size()) {
                const int parentNode = parent.nodes[k];
                const Mat4 animatedParent =
                    Mat4::fromTRS(toCharacter(parent.animated[k]), parent.animatedRotations[k],
                                  parent.scales[k]);
                const Mat4 delta = pose.nodeGlobals[static_cast<std::size_t>(parentNode)] *
                                   animatedParent.inverseAffine();
                const Vec3 root = toStage(delta.transformPoint(toCharacter(chain.animated[0])));
                if (finiteVec(root)) {
                    const Vec3 shift = root - rendered[0];
                    for (Vec3& point : rendered) {
                        point += shift;
                    }
                }
            }
        }
        // An attached chain's root moves with its ancestor, not with this chain's
        // spring, so the limit is measured from the pose shifted with the root.
        const Vec3 carriedShift =
            chain.attached && points > 0 ? rendered[0] - chain.animated[0] : Vec3{};
        for (std::size_t i = chain.attached ? 1 : 0; i < points; ++i) {
            const Vec3 base = chain.animated[i] + carriedShift;
            Vec3 offset = rendered[i] - base;
            const float distance = offset.length();
            if (distance > maxDisplacement) {
                rendered[i] = base + offset / distance * maxDisplacement;
            }
        }
        // A branch chain's anchor is written by its parent chain.
        const std::size_t first = chain.parentChain >= 0 && !chain.attached ? 1 : 0;
        // Rotate each segment so the skinned vertices follow the simulated
        // direction. The tip inherits its parent's delta.
        Quat inherited = Quat::identity();
        for (std::size_t i = 0; i < count; ++i) {
            Quat rotation = chain.animatedRotations[i];
            if (i + 1 < points) {
                const Vec3 from = safeNormalize(chain.animated[i + 1] - chain.animated[i]);
                const Vec3 to = safeNormalize(rendered[i + 1] - rendered[i], from);
                if (from.lengthSquared() > 0.5f && to.lengthSquared() > 0.5f) {
                    const Quat stageDelta = math::fromTo(from, to);
                    const Quat delta = (rootInverse * stageDelta * rootRotation).normalized();
                    rotation = (delta * rotation).normalized();
                    inherited = delta;
                }
            } else {
                rotation = (inherited * rotation).normalized();
            }
            if (i < first) {
                continue;
            }
            const int node = chain.nodes[i];
            if (node < 0 || static_cast<std::size_t>(node) >= pose.nodeGlobals.size()) {
                continue;
            }
            const Vec3 position = toCharacter(rendered[i]);
            if (!finiteVec(position) || !std::isfinite(rotation.w)) {
                continue;
            }
            const Mat4 written = Mat4::fromTRS(position, rotation, chain.scales[i]);
            if (i < chain.carried.size() && !chain.carried[i].empty()) {
                const Mat4 delta =
                    written * pose.nodeGlobals[static_cast<std::size_t>(node)].inverseAffine();
                for (int carried : chain.carried[i]) {
                    if (static_cast<std::size_t>(carried) < pose.nodeGlobals.size()) {
                        pose.nodeGlobals[static_cast<std::size_t>(carried)] =
                            delta * pose.nodeGlobals[static_cast<std::size_t>(carried)];
                    }
                }
            }
            pose.nodeGlobals[static_cast<std::size_t>(node)] = written;
        }
        // Stats.
        for (std::size_t i = 1; i < points; ++i) {
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
    impl_->snapToAnimated();
    impl_->initialized = true;
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
        impl_->stats.bones += static_cast<int>(chain.nodes.size()) -
                              (chain.parentChain >= 0 && !chain.attached ? 1 : 0);
    }
    impl_->stats.colliders = static_cast<int>(impl_->colliderCenters.size());
    impl_->stats.height = impl_->model ? impl_->model->height : 0.0f;
    impl_->readTargets(pose);
    impl_->ensureState(pose);
    // A seek, teleport or a long gap (pause/resume) makes the old velocities
    // meaningless: snap to the animated pose instead of simulating a catch-up.
    bool discontinuity = !(deltaSeconds >= 0.0) || deltaSeconds > kHitchSeconds ||
                         !std::isfinite(deltaSeconds);
    if (!discontinuity) {
        const float teleport = kTeleportHeights * std::max(0.1f, impl_->stats.height);
        for (const auto& chain : impl_->chains) {
            if (chain.haveLastRoot && !chain.animated.empty() &&
                (chain.animated[0] - chain.lastAnimatedRoot).length() > teleport) {
                discontinuity = true;
                break;
            }
        }
    }
    if (!settings.enabled || discontinuity) {
        // While disabled (or after a discontinuity) the simulation shadows the
        // animated pose, so enabling it later starts from the CURRENT pose with
        // zero velocity instead of resuming from stale positions.
        impl_->snapToAnimated();
        impl_->stats.valid = true;
        impl_->stats.simulateMilliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        // Enabled but discontinuous: the pose is the animated pose this frame.
        return;
    }
    for (auto& chain : impl_->chains) {
        if (!chain.animated.empty()) {
            chain.lastAnimatedRoot = chain.animated[0];
            chain.haveLastRoot = true;
        }
    }
    if (!(deltaSeconds > 0.0)) {
        impl_->writePose(pose, settings);
        impl_->stats.valid = true;
        impl_->stats.simulateMilliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return;
    }
    const double accumulatedBefore = impl_->accumulator;
    impl_->accumulator = std::min(impl_->accumulator + deltaSeconds, kMaxAccumulated);
    int steps = 0;
    const int maxSteps = std::clamp(settings.maxSubsteps, 1, 32);
    while (impl_->accumulator >= kFixedStep && steps < maxSteps) {
        // Step number `steps + 1` happens this far into the frame. Its animated target is
        // taken at that moment, not at the frame start, so the spring is driven by the
        // animation at the time it simulates.
        const double stepEnd = static_cast<double>(steps + 1) * kFixedStep - accumulatedBefore;
        const double fraction = stepEnd / deltaSeconds;
        for (auto& chain : impl_->chains) {
            if (chain.nodes.empty()) {
                continue;
            }
            impl_->interpolateStepTarget(chain, fraction);
            impl_->step(chain, settings, static_cast<float>(kFixedStep), chain.stepTarget);
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
        impl_->snapToAnimated();
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
