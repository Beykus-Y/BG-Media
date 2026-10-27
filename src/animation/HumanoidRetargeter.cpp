#include "bgmedia/animation/HumanoidRetargeter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace bgmedia::animation {
namespace {

using Clock = std::chrono::steady_clock;
using math::Mat4;
using math::Quat;
using vrm::HumanoidBone;
using math::Vec3;
using math::kPi;

constexpr float kTiny = 1.0e-6f;

Quat worldDeltaToLocal(const Quat& bindWorldRotation, const Quat& worldDelta) {
    return (bindWorldRotation.conjugate() * worldDelta * bindWorldRotation)
        .normalized();
}


float safeClamp(float value, float lo, float hi) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, lo, hi);
}

float lengthOr(Vec3 v, float fallback) {
    const float length = v.length();
    return length > kTiny ? length : fallback;
}

} // namespace

struct LegRig {
    int upper = -1;
    int lower = -1;
    int foot = -1;
    int toes = -1;
    int parent = -1;
    float upperLength = 0.0f;
    float lowerLength = 0.0f;
    Vec3 bindAnkle{};
    Vec3 bindUpper{};
    Vec3 bindKnee{};
    Vec3 bindThighDirection{};
    Vec3 bindCalfDirection{};
};

struct HumanoidRetargeter::Impl {
    const vrm::VrmModel* model = nullptr;
    bool valid = false;
    std::string error;

    std::array<int, vrm::kHumanoidBoneCount> boneNode{};
    std::vector<int> parents;
    std::vector<int> evaluationOrder;
    std::vector<Mat4> bindLocals;
    std::vector<Quat> bindLocalRotations;

    LegRig legs[2];
    int hips = -1;
    int spine = -1;
    int chest = -1;
    int upperChest = -1;
    int neck = -1;
    int head = -1;
    int leftShoulder = -1;
    int rightShoulder = -1;
    int leftUpperArm = -1;
    int leftLowerArm = -1;
    int leftHand = -1;
    int rightUpperArm = -1;
    int rightLowerArm = -1;
    int rightHand = -1;

    // fingers[side][finger][joint]: side 0 left, finger 0..4 thumb/index/middle/ring/little
    int fingers[2][5][3];
    int fingerBones = 0;

    float height = 1.0f;
    float floorY = 0.0f;

    bool initialize(const vrm::VrmModel& source);
    void evaluate(const std::vector<Mat4>& locals,
                  std::vector<Mat4>& globals) const;
    void applyLocalDelta(std::vector<Mat4>& locals, int node,
                         const Quat& worldDelta) const;
    [[nodiscard]] Vec3 bindPosition(int node) const;
    [[nodiscard]] Vec3 actualPosition(const std::vector<Mat4>& globals,
                                      int node) const;
    // Distance between each evaluated foot joint and its target / reachable
    // target. Reads the final skeleton, not the solver's intermediate maths.
    void measureFootError(const std::vector<Mat4>& globals, const Vec3 targets[2],
                          const Vec3 reachable[2], float& actual,
                          float& solver) const;
    void solveLeg(const LegRig& leg, const std::vector<Mat4>& globals,
                  std::vector<Mat4>& locals, Vec3 target, Vec3 pole, float& footError,
                  float& lowestY, Vec3& reachableTarget,
                  const Quat& footWorldDelta = Quat::identity()) const;
};

bool HumanoidRetargeter::Impl::initialize(const vrm::VrmModel& source) {
    model = &source;
    if (!source.completeHumanoid()) {
        error = "VRM humanoid is incomplete: the retargeter requires hips, head, "
                "both arms and both legs";
        return false;
    }
    if (source.bindGlobals.size() != source.nodes.size() ||
        source.bindGlobalRotations.size() != source.nodes.size()) {
        error = "VRM bind transforms are not prepared";
        return false;
    }
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        boneNode[i] = source.humanoidNodes[i];
    }
    parents.resize(source.nodes.size(), -1);
    bindLocals.resize(source.nodes.size(), Mat4::identity());
    bindLocalRotations.resize(source.nodes.size());
    for (std::size_t i = 0; i < source.nodes.size(); ++i) {
        parents[i] = source.nodes[i].parent;
        bindLocals[i] = source.nodes[i].local;
        bindLocalRotations[i] = source.nodes[i].rotation.normalized();
    }

    // Parent-before-child evaluation order for all nodes reachable from the
    // default scene. The loader has already rejected cycles and unreachable
    // nodes, so this is a simple deterministic DFS.
    evaluationOrder.clear();
    evaluationOrder.reserve(source.nodes.size());
    std::vector<int> stack;
    for (auto it = source.sceneRoots.rbegin(); it != source.sceneRoots.rend(); ++it) {
        stack.push_back(*it);
    }
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        evaluationOrder.push_back(node);
        const auto& children = source.nodes[static_cast<std::size_t>(node)].children;
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    if (evaluationOrder.size() != source.nodes.size()) {
        error = "VRM node graph is not complete";
        return false;
    }

    const auto nodeOf = [&](vrm::HumanoidBone bone) {
        return source.nodeFor(bone);
    };
    hips = nodeOf(vrm::HumanoidBone::Hips);
    spine = nodeOf(vrm::HumanoidBone::Spine);
    chest = nodeOf(vrm::HumanoidBone::Chest);
    upperChest = nodeOf(vrm::HumanoidBone::UpperChest);
    neck = nodeOf(vrm::HumanoidBone::Neck);
    head = nodeOf(vrm::HumanoidBone::Head);
    leftShoulder = nodeOf(vrm::HumanoidBone::LeftShoulder);
    rightShoulder = nodeOf(vrm::HumanoidBone::RightShoulder);
    leftUpperArm = nodeOf(vrm::HumanoidBone::LeftUpperArm);
    leftLowerArm = nodeOf(vrm::HumanoidBone::LeftLowerArm);
    leftHand = nodeOf(vrm::HumanoidBone::LeftHand);
    rightUpperArm = nodeOf(vrm::HumanoidBone::RightUpperArm);
    rightLowerArm = nodeOf(vrm::HumanoidBone::RightLowerArm);
    rightHand = nodeOf(vrm::HumanoidBone::RightHand);

    const auto setupLeg = [&](LegRig& leg, vrm::HumanoidBone upper,
                              vrm::HumanoidBone lower, vrm::HumanoidBone foot,
                              vrm::HumanoidBone toes) {
        leg.upper = nodeOf(upper);
        leg.lower = nodeOf(lower);
        leg.foot = nodeOf(foot);
        leg.toes = nodeOf(toes);
        leg.parent = leg.upper >= 0 ? parents[static_cast<std::size_t>(leg.upper)] : -1;
        leg.bindUpper = bindPosition(leg.upper);
        leg.bindKnee = bindPosition(leg.lower);
        leg.bindAnkle = bindPosition(leg.foot);
        leg.bindThighDirection =
            math::normalize(leg.bindKnee - leg.bindUpper);
        leg.bindCalfDirection =
            math::normalize(leg.bindAnkle - leg.bindKnee);
        leg.upperLength = lengthOr(leg.bindKnee - leg.bindUpper, 0.0f);
        leg.lowerLength = lengthOr(leg.bindAnkle - leg.bindKnee, 0.0f);
    };
    setupLeg(legs[0], vrm::HumanoidBone::LeftUpperLeg,
             vrm::HumanoidBone::LeftLowerLeg, vrm::HumanoidBone::LeftFoot,
             vrm::HumanoidBone::LeftToes);
    setupLeg(legs[1], vrm::HumanoidBone::RightUpperLeg,
             vrm::HumanoidBone::RightLowerLeg, vrm::HumanoidBone::RightFoot,
             vrm::HumanoidBone::RightToes);
    for (const LegRig& leg : legs) {
        if (leg.upperLength < 0.01f || leg.lowerLength < 0.01f) {
            error = "VRM leg bones have unusable lengths";
            return false;
        }
    }
    for (auto& side : fingers) {
        for (auto& finger : side) {
            for (int& joint : finger) {
                joint = -1;
            }
        }
    }
    {
        static constexpr const char* kSides[2] = {"left", "right"};
        static constexpr const char* kFingers[5] = {"Thumb", "Index", "Middle", "Ring", "Little"};
        static constexpr const char* kJoints[3] = {"Proximal", "Intermediate", "Distal"};
        for (const vrm::HumanoidEntry& entry : source.humanoidEntries) {
            for (int sd = 0; sd < 2; ++sd) {
                for (int fg = 0; fg < 5; ++fg) {
                    for (int jt = 0; jt < 3; ++jt) {
                        const std::string name = std::string(kSides[sd]) + kFingers[fg] + kJoints[jt];
                        if (entry.name == name) {
                            fingers[sd][fg][jt] = entry.node;
                        }
                    }
                }
            }
        }
        for (int sd = 0; sd < 2; ++sd) {
            for (int fg = 0; fg < 5; ++fg) {
                for (int jt = 0; jt < 3; ++jt) {
                    if (fingers[sd][fg][jt] < 0) {
                        fingers[sd][fg][jt] = source.fingerNode(sd, fg, jt);
                    }
                    if (fingers[sd][fg][jt] >= 0) {
                        ++fingerBones;
                    }
                }
            }
        }
    }
    height = std::max(0.05f, source.height);
    floorY = source.bounds.valid ? source.bounds.min.y : 0.0f;
    valid = true;
    return true;
}

Vec3 HumanoidRetargeter::Impl::bindPosition(int node) const {
    if (!model || node < 0 ||
        static_cast<std::size_t>(node) >= model->bindGlobals.size()) {
        return {};
    }
    return model->bindGlobals[static_cast<std::size_t>(node)].translationPart();
}

Vec3 HumanoidRetargeter::Impl::actualPosition(
    const std::vector<Mat4>& globals, int node) const {
    if (node < 0 || static_cast<std::size_t>(node) >= globals.size()) {
        return {};
    }
    return globals[static_cast<std::size_t>(node)].translationPart();
}

void HumanoidRetargeter::Impl::evaluate(const std::vector<Mat4>& locals,
                                        std::vector<Mat4>& globals) const {
    globals.resize(locals.size(), Mat4::identity());
    for (int node : evaluationOrder) {
        const std::size_t index = static_cast<std::size_t>(node);
        if (parents[index] >= 0) {
            globals[index] =
                globals[static_cast<std::size_t>(parents[index])] * locals[index];
        } else {
            globals[index] = locals[index];
        }
    }
}

void HumanoidRetargeter::Impl::applyLocalDelta(std::vector<Mat4>& locals, int node,
                                               const Quat& worldDelta) const {
    if (node < 0 || static_cast<std::size_t>(node) >= locals.size()) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(node);
    const Quat bindWorld = model->bindGlobalRotations[index];
    const Quat localDelta = worldDeltaToLocal(bindWorld, worldDelta);
    const Quat current = locals[index].rotationQuat();
    const Quat rotated = (current * localDelta).normalized();
    const Vec3 translation = locals[index].translationPart();
    locals[index] =
        Mat4::fromTRS(translation, rotated,
                      {bindLocals[index].axisX().length(),
                       bindLocals[index].axisY().length(),
                       bindLocals[index].axisZ().length()});
}

void HumanoidRetargeter::Impl::measureFootError(const std::vector<Mat4>& globals,
                                                const Vec3 targets[2],
                                                const Vec3 reachable[2], float& actual,
                                                float& solver) const {
    actual = 0.0f;
    solver = 0.0f;
    for (int f = 0; f < 2; ++f) {
        const LegRig& leg = legs[f];
        if (leg.foot < 0 || leg.upper < 0) {
            continue;
        }
        const Vec3 ankle = actualPosition(globals, leg.foot);
        actual = std::max(actual, (ankle - targets[f]).length());
        solver = std::max(solver, (ankle - reachable[f]).length());
    }
}

void HumanoidRetargeter::Impl::solveLeg(const LegRig& leg,
                                        const std::vector<Mat4>& globals,
                                        std::vector<Mat4>& locals, Vec3 target,
                                        Vec3 pole, float& footError,
                                        float& lowestY, Vec3& reachableTarget,
                                        const Quat& footWorldDelta) const {
    reachableTarget = target;
    if (leg.upper < 0 || leg.lower < 0 || leg.foot < 0 || leg.parent < 0) {
        return;
    }
    const Vec3 hip = actualPosition(globals, leg.upper);
    Vec3 reach = target - hip;
    float distance = reach.length();
    const float maxReach = leg.upperLength + leg.lowerLength - 0.002f;
    const float minReach = std::abs(leg.upperLength - leg.lowerLength) + 0.002f;
    if (distance < kTiny) {
        reach = -model->up;
        distance = kTiny;
    }
    const Vec3 direction = reach / distance;
    const float requestedDistance = distance;
    distance = std::clamp(distance, minReach, maxReach);
    // The closest point the two-bone chain can actually reach. Distance from
    // the requested target to it is the (geometric) reach deficit; any further
    // error measured on the evaluated skeleton is solver error.
    reachableTarget = hip + direction * distance;
    footError = std::max(footError, std::abs(requestedDistance - distance));

    Vec3 bendAxis = pole - direction * math::dot(pole, direction);
    if (bendAxis.lengthSquared() < 1.0e-8f) {
        bendAxis = math::cross(model->right, direction);
    }
    if (bendAxis.lengthSquared() < 1.0e-8f) {
        bendAxis = math::cross(model->forward, direction);
    }
    bendAxis = math::normalize(bendAxis);

    const float along =
        (leg.upperLength * leg.upperLength - leg.lowerLength * leg.lowerLength +
         distance * distance) /
        (2.0f * distance);
    const float heightOffset =
        std::sqrt(std::max(0.0f, leg.upperLength * leg.upperLength -
                                     along * along));
    const Vec3 knee = hip + direction * along + bendAxis * heightOffset;
    const Vec3 ankle = hip + direction * distance;

    const Vec3 thighDirection = math::normalize(knee - hip);
    const Vec3 calfDirection = math::normalize(ankle - knee);
    if (!math::finite(thighDirection) || !math::finite(calfDirection)) {
        return;
    }

    const Quat thighDelta = math::fromTo(leg.bindThighDirection, thighDirection);
    const Quat thighWorld =
        (thighDelta * model->bindGlobalRotations[static_cast<std::size_t>(leg.upper)])
            .normalized();
    const Quat hipsWorld = globals[static_cast<std::size_t>(leg.parent)].rotationQuat();
    const Quat thighLocal = (hipsWorld.conjugate() * thighWorld).normalized();
    locals[static_cast<std::size_t>(leg.upper)] =
        Mat4::fromTRS(bindLocals[static_cast<std::size_t>(leg.upper)].translationPart(),
                      thighLocal,
                      {bindLocals[static_cast<std::size_t>(leg.upper)].axisX().length(),
                       bindLocals[static_cast<std::size_t>(leg.upper)].axisY().length(),
                       bindLocals[static_cast<std::size_t>(leg.upper)].axisZ().length()});

    const Quat lowerBindWorld =
        model->bindGlobalRotations[static_cast<std::size_t>(leg.lower)];
    const Quat upperBindWorld =
        model->bindGlobalRotations[static_cast<std::size_t>(leg.upper)];
    const Quat currentCalfWorld =
        (thighWorld * (upperBindWorld.conjugate() * lowerBindWorld)).normalized();
    // bindCalfDirection is a bind-pose *direction* (model space), so it must be
    // carried by the rotation that moved the thigh (thighWorld * upperBind^-1),
    // not by the absolute world rotation of the calf bone, which would apply
    // the calf's own bind rotation a second time on non-identity rigs.
    const Quat thighDeltaApplied = (thighWorld * upperBindWorld.conjugate()).normalized();
    const Vec3 currentCalfDirection =
        math::rotate(thighDeltaApplied, leg.bindCalfDirection);
    const Quat calfDelta = math::fromTo(currentCalfDirection, calfDirection);
    const Quat calfWorld =
        (calfDelta * currentCalfWorld).normalized();
    const Quat calfLocal = (thighWorld.conjugate() * calfWorld).normalized();
    locals[static_cast<std::size_t>(leg.lower)] =
        Mat4::fromTRS(bindLocals[static_cast<std::size_t>(leg.lower)].translationPart(),
                      calfLocal,
                      {bindLocals[static_cast<std::size_t>(leg.lower)].axisX().length(),
                       bindLocals[static_cast<std::size_t>(leg.lower)].axisY().length(),
                       bindLocals[static_cast<std::size_t>(leg.lower)].axisZ().length()});

    // Keep the sole level in world space. This removes the visible foot pitch
    // that would otherwise appear when the knee bends.
    const Quat footBindWorld =
        (footWorldDelta *
         model->bindGlobalRotations[static_cast<std::size_t>(leg.foot)])
            .normalized();
    const Quat footLocal = (calfWorld.conjugate() * footBindWorld).normalized();
    locals[static_cast<std::size_t>(leg.foot)] =
        Mat4::fromTRS(bindLocals[static_cast<std::size_t>(leg.foot)].translationPart(),
                      footLocal,
                      {bindLocals[static_cast<std::size_t>(leg.foot)].axisX().length(),
                       bindLocals[static_cast<std::size_t>(leg.foot)].axisY().length(),
                       bindLocals[static_cast<std::size_t>(leg.foot)].axisZ().length()});

    // lowestY is the analytic ankle height; the reported foot error is measured
    // on the evaluated skeleton by the caller (see measureFootError).
    const Vec3 solvedAnkle = hip + thighDirection * leg.upperLength +
                             calfDirection * leg.lowerLength;
    lowestY = std::min(lowestY, solvedAnkle.y);
}

RetargetedPose HumanoidRetargeter::apply(const CharacterPose& pose,
                                         double mediaTime,
                                         const RetargetOptions& options) const {
    RetargetedPose out;
    if (!impl_ || !impl_->valid) {
        return out;
    }
    const auto started = Clock::now();
    const vrm::VrmModel& model = *impl_->model;
    const float height = impl_->height;
    const Vec3 up = model.up;
    const Vec3 right = model.right;
    const Vec3 forward = model.forward;

    std::vector<Mat4> locals = impl_->bindLocals;
    std::vector<Mat4> globals;
    impl_->evaluate(locals, globals);

    const bool idle = pose.move == MoveKind::Idle && pose.energy < 0.02f;
    float idleBreath = 0.0f;
    if (options.idleMotion && idle) {
        idleBreath = 0.5f + 0.5f * static_cast<float>(std::sin(mediaTime * 1.7));
    }

    // Keep a small permanent knee bend so the rig never locks and the hips can
    // rise toward the bind height without lifting the planted feet. The IK
    // needs headroom: the bind legs are nearly straight.
    float hipDrop = 0.012f * height -
                    safeClamp(pose.bob, -1.5f, 1.5f) * options.hipBobScale * height;
    hipDrop += std::max(0.0f, std::max(pose.kneeLeft, pose.kneeRight)) *
               options.kneeDropScale * height;
    hipDrop = std::clamp(hipDrop, 0.0f, 0.22f * height);
    if (options.idleMotion && idle) {
        hipDrop += 0.004f * height + 0.0025f * height * idleBreath;
    }

    // Hips translation is expressed in the parent's frame. Feet remain at
    // their bind XZ targets, so the solver turns this into knee flexion rather
    // than sliding the whole character.
    if (impl_->hips >= 0) {
        const std::size_t hipsIndex = static_cast<std::size_t>(impl_->hips);
        const int parent = impl_->parents[hipsIndex];
        Vec3 offset = up * (-hipDrop) +
                      right * (safeClamp(pose.hipShift, -1.5f, 1.5f) * 0.030f *
                               height);
        if (parent >= 0) {
            offset = math::rotate(
                model.bindGlobalRotations[static_cast<std::size_t>(parent)]
                    .conjugate(),
                offset);
        }
        locals[hipsIndex].m[12] += offset.x;
        locals[hipsIndex].m[13] += offset.y;
        locals[hipsIndex].m[14] += offset.z;
    }

    const Quat leanDelta = math::fromAxisAngle(
        forward, safeClamp(pose.lean, -1.5f, 1.5f) * options.leanMax);
    const Quat twistDelta = math::fromAxisAngle(
        up, safeClamp(pose.chestTwist, -1.5f, 1.5f) * options.twistMax);
    const Quat headNodDelta = math::fromAxisAngle(
        right, safeClamp(pose.headNod, -1.5f, 1.5f) * options.headNodMax);
    const Quat headTiltDelta = math::fromAxisAngle(
        forward, safeClamp(pose.headTilt, -1.5f, 1.5f) * options.headTiltMax);
    const Quat headTurnDelta = math::fromAxisAngle(
        up, safeClamp(pose.headTurn, -1.5f, 1.5f) * options.headTurnMax);

    const auto rotateBone = [&](int node, const Quat& delta, float weight) {
        if (node >= 0) {
            impl_->applyLocalDelta(locals, node,
                                   math::slerp(Quat::identity(), delta, weight));
        }
    };
    rotateBone(impl_->hips, leanDelta, 0.10f);
    float remainingLean = 0.85f;
    if (impl_->spine >= 0) {
        rotateBone(impl_->spine, leanDelta, 0.30f);
        rotateBone(impl_->spine, twistDelta, 0.28f);
        remainingLean -= 0.30f;
    }
    if (impl_->chest >= 0) {
        rotateBone(impl_->chest, leanDelta, 0.30f);
        rotateBone(impl_->chest, twistDelta, 0.32f);
        remainingLean -= 0.30f;
    }
    if (impl_->upperChest >= 0) {
        rotateBone(impl_->upperChest, leanDelta, std::max(0.0f, remainingLean));
        rotateBone(impl_->upperChest, twistDelta, 0.30f);
        remainingLean = 0.0f;
    }
    if (remainingLean > 0.0f) {
        rotateBone(impl_->hips, leanDelta, remainingLean * 0.5f);
    }
    rotateBone(impl_->neck, headNodDelta, 0.25f);
    rotateBone(impl_->neck, headTurnDelta, 0.25f);
    rotateBone(impl_->neck, headTiltDelta, 0.25f);
    rotateBone(impl_->head, headNodDelta, 0.75f);
    rotateBone(impl_->head, headTurnDelta, 0.75f);
    rotateBone(impl_->head, headTiltDelta, 0.75f);
    if (options.idleMotion && idle) {
        rotateBone(impl_->chest, math::fromAxisAngle(right, 0.012f * idleBreath),
                   (impl_->chest >= 0) ? 1.0f : 0.0f);
        rotateBone(impl_->head, math::fromAxisAngle(up, 0.03f * idleBreath), 1.0f);
    }

    const auto setupArm = [&](int upperArm, int lowerArm, int hand, int sideSign,
                              float signedArm, float elbowAmount) {
        if (upperArm < 0 || lowerArm < 0) {
            return;
        }
        const float theta = std::clamp(
            options.armHang + signedArm * options.armSwingGain,
            -1.25f, 1.48f);
        const Vec3 bindUpper = impl_->bindPosition(upperArm);
        const Vec3 bindLower = impl_->bindPosition(lowerArm);
        const Vec3 bindDirection = math::normalize(bindLower - bindUpper);
        Vec3 desired = right * (static_cast<float>(sideSign) * std::sin(theta)) -
                       up * std::cos(theta) + forward * 0.07f;
        desired = math::normalize(desired);
        const Quat worldDelta = math::fromTo(bindDirection, desired);
        impl_->applyLocalDelta(locals, upperArm, worldDelta);

        // The elbow hinge is expressed in the bind frame and therefore follows
        // the upper arm when the shoulder swings.
        const Quat elbowDelta = math::fromAxisAngle(
            up * static_cast<float>(sideSign),
            options.elbowBase + std::max(0.0f, elbowAmount) * options.elbowGain);
        impl_->applyLocalDelta(locals, lowerArm, elbowDelta);
        if (hand >= 0) {
            impl_->applyLocalDelta(
                locals, hand,
                math::fromAxisAngle(up * static_cast<float>(sideSign),
                                    -0.10f * std::max(0.0f, elbowAmount)));
        }
    };
    // signedArm is negative-for-left in the existing 2D pose; multiply by
    // sideSign so both sides use the same "positive = outward" convention.
    setupArm(impl_->leftUpperArm, impl_->leftLowerArm, impl_->leftHand, -1,
             -pose.armLeft, pose.elbowLeft);
    setupArm(impl_->rightUpperArm, impl_->rightLowerArm, impl_->rightHand, 1,
             pose.armRight, pose.elbowRight);

    impl_->evaluate(locals, globals);

    if (options.footIK) {
        const float liftLeft =
            std::max(0.0f, safeClamp(pose.footLeft, 0.0f, 1.0f)) * 0.10f * height;
        const float liftRight =
            std::max(0.0f, safeClamp(pose.footRight, 0.0f, 1.0f)) * 0.10f * height;
        Vec3 targetLeft = impl_->legs[0].bindAnkle + up * liftLeft;
        Vec3 targetRight = impl_->legs[1].bindAnkle + up * liftRight;
        // A tiny amount of the hip sway relaxes one knee; it never moves the
        // planted foot sideways, which is what causes visible skating.
        const Vec3 pole = forward;
        float footError = 0.0f;
        float lowest = std::numeric_limits<float>::max();
        Vec3 reachable[2];
        impl_->solveLeg(impl_->legs[0], globals, locals, targetLeft, pole,
                        footError, lowest, reachable[0]);
        impl_->solveLeg(impl_->legs[1], globals, locals, targetRight, pole,
                        footError, lowest, reachable[1]);
        impl_->evaluate(locals, globals);
        const Vec3 wanted[2] = {targetLeft, targetRight};
        impl_->measureFootError(globals, wanted, reachable, out.stats.maxFootError,
                                out.stats.footSolverError);
        out.stats.footReachDeficit = footError;
        out.stats.footIK = true;
        out.stats.lowestFootY = lowest - impl_->floorY;
    } else {
        // A conservative fallback for custom rigs that should not use IK.
        const auto bendKnee = [&](const LegRig& leg, float amount) {
            if (leg.upper >= 0) {
                impl_->applyLocalDelta(
                    locals, leg.upper,
                    math::fromAxisAngle(right, -0.08f * amount));
            }
            if (leg.lower >= 0) {
                impl_->applyLocalDelta(
                    locals, leg.lower,
                    math::fromAxisAngle(right, 0.55f * amount));
            }
        };
        bendKnee(impl_->legs[0],
                 std::max(pose.kneeLeft, 0.10f) + pose.footLeft * 0.5f);
        bendKnee(impl_->legs[1],
                 std::max(pose.kneeRight, 0.10f) + pose.footRight * 0.5f);
        impl_->evaluate(locals, globals);
    }

    // Whole-character spin is applied by the renderer as a model transform. It
    // must not be baked into the hips, otherwise the planted feet would turn
    // with the pelvis but not with the rest of the character.
    const float turn = safeClamp(pose.rotation, -2.0f, 2.0f) * 2.0f * kPi;
    out.modelTransform = Mat4::fromQuat(math::fromAxisAngle(up, turn));
    out.nodeGlobals = std::move(globals);
    out.stats.valid = true;
    out.stats.height = height;
    out.stats.hipDrop = hipDrop;
    out.stats.resolvedBones = 0;
    out.stats.missingBones = 0;
    for (int node : impl_->boneNode) {
        if (node >= 0) {
            ++out.stats.resolvedBones;
        } else {
            ++out.stats.missingBones;
        }
    }
    out.stats.fingerBones = impl_->fingerBones;
    out.stats.retargetMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();

    for (const Mat4& matrix : out.nodeGlobals) {
        if (!matrix.finite()) {
            out.stats.valid = false;
            std::vector<Mat4> fallback;
            impl_->evaluate(impl_->bindLocals, fallback);
            out.nodeGlobals = std::move(fallback);
            out.stats.footIK = false;
            break;
        }
    }
    return out;
}

RetargetedPose HumanoidRetargeter::applyClip(const motion::ClipPose& pose,
                                             double mediaTime,
                                             const RetargetOptions& options) const {
    (void)mediaTime;
    RetargetedPose out;
    if (!impl_ || !impl_->valid) {
        return out;
    }
    const auto started = Clock::now();
    const vrm::VrmModel& model = *impl_->model;
    const float height = impl_->height;
    const Vec3 up = model.up;
    const Vec3 forward = model.forward;
    const Vec3 left = -model.right;  // clip frame: +X is the character's left

    // Clip frame -> model axes (a proper rotation, so quaternion vector parts
    // map directly). Contract: every rotation below is first *authored* about
    // clip-frame axes (+X = character's left, +Y = up, +Z = forward) and then
    // converted exactly once by quatToModel. Never build a rotation about the
    // model-space vectors `left`/`up`/`forward` and then pass it through
    // quatToModel as well; that converts the axis twice and is only correct on
    // rigs whose model axes happen to equal the clip axes.
    const auto toModel = [&](Vec3 v) { return left * v.x + up * v.y + forward * v.z; };
    const auto quatToModel = [&](const Quat& q) {
        const Vec3 axis = toModel({q.x, q.y, q.z});
        return Quat{axis.x, axis.y, axis.z, q.w}.normalized();
    };

    std::vector<Mat4> locals = impl_->bindLocals;
    std::vector<Mat4> globals;

    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        const auto bone = static_cast<HumanoidBone>(b);
        if (options.footIK &&
            (bone == HumanoidBone::LeftUpperLeg || bone == HumanoidBone::LeftLowerLeg ||
            bone == HumanoidBone::LeftFoot || bone == HumanoidBone::RightUpperLeg ||
            bone == HumanoidBone::RightLowerLeg || bone == HumanoidBone::RightFoot)) {
            continue;  // legs are solved by IK from the foot targets (footIK off:
                       // the clip's own leg rotations are applied instead)
        }
        const Quat q = pose.bone[b];
        if (std::abs(q.w) > 0.99999f) {
            continue;
        }
        impl_->applyLocalDelta(locals, impl_->boneNode[b], quatToModel(q));
    }

    // Finger curl: 0 = open hand, 1 = fist. A relaxed hand is not a flat blade.
    // BG Motion 3.2 uses the detailed per-finger pose when the clip, an overlay
    // or the hand layer provides one; otherwise the v1 scalar is preserved
    // exactly so old clips keep their authored look.
    {
        static constexpr float kFlex[5][3] = {{22.0f, 30.0f, 20.0f}, {38.0f, 52.0f, 34.0f},
                                              {40.0f, 56.0f, 36.0f}, {38.0f, 54.0f, 34.0f},
                                              {36.0f, 50.0f, 32.0f}};
        for (int sd = 0; sd < 2; ++sd) {
            const bool detailed = pose.detailedHands;
            const motion::HandPose& handPose =
                pose.handPose[static_cast<std::size_t>(sd)];
            if (detailed) {
                const int handNode = sd == 0 ? impl_->leftHand : impl_->rightHand;
                const Quat wrist =
                    // Pitch keeps the established convention (positive = about
                    // the character's right, i.e. clip -X).
                    (math::fromAxisAngle({-1.0f, 0.0f, 0.0f}, handPose.wrist[0] * 0.26f) *
                     math::fromAxisAngle({0.0f, 1.0f, 0.0f}, handPose.wrist[1] * 0.30f) *
                     math::fromAxisAngle({0.0f, 0.0f, 1.0f}, handPose.wrist[2] * 0.34f))
                        .normalized();
                impl_->applyLocalDelta(locals, handNode, quatToModel(wrist));
            }
            const float curl =
                std::clamp(pose.hand[static_cast<std::size_t>(sd)], 0.0f, 1.0f);
            for (int fg = 0; fg < 5; ++fg) {
                for (int jt = 0; jt < 3; ++jt) {
                    const int node = impl_->fingers[sd][fg][jt];
                    if (node < 0) {
                        continue;
                    }
                    float amount = 0.0f;
                    if (detailed) {
                        amount = handPose.curl[static_cast<std::size_t>(fg)]
                                             [static_cast<std::size_t>(jt)];
                        if (fg == 0) {
                            amount = std::min(1.0f, amount +
                                                        0.22f * handPose.thumbOpposition);
                        }
                    } else {
                        amount = curl;
                    }
                    const float angle =
                        kFlex[fg][jt] * amount * (kPi / 180.0f) *
                        (sd == 0 ? -1.0f : 1.0f);
                    impl_->applyLocalDelta(
                        locals, node,
                        quatToModel(math::fromAxisAngle({0.0f, 0.0f, 1.0f},
                                                        angle * (fg == 0 ? 0.6f : 1.0f))));
                }
                // Spread fans the fingers in the palm plane. It is deliberately
                // small: a large value reads as a broken hand, not a gesture.
                if (detailed && fg > 0) {
                    const float fan = handPose.spread[static_cast<std::size_t>(fg)] *
                                      (static_cast<float>(fg) - 2.0f) * 0.10f;
                    const Quat spreadQuat = quatToModel(
                        math::fromAxisAngle({0.0f, 1.0f, 0.0f},
                                            fan * (sd == 0 ? -1.0f : 1.0f)));
                    for (int jt = 0; jt < 3; ++jt) {
                        const int node = impl_->fingers[sd][fg][jt];
                        if (node >= 0) {
                            impl_->applyLocalDelta(locals, node, spreadQuat);
                        }
                    }
                }
            }
            if (detailed && handPose.thumbOpposition > 0.0f) {
                const Quat opposition = quatToModel(math::fromAxisAngle(
                    {0.0f, 0.0f, 1.0f}, handPose.thumbOpposition * 0.34f *
                                            (sd == 0 ? -1.0f : 1.0f)));
                for (int jt = 0; jt < 3; ++jt) {
                    const int node = impl_->fingers[sd][0][jt];
                    if (node >= 0) {
                        impl_->applyLocalDelta(locals, node, opposition);
                    }
                }
            }
        }
    }

    const std::size_t hipsIndex = impl_->hips >= 0 ? static_cast<std::size_t>(impl_->hips) : 0;
    const int hipsParent = impl_->hips >= 0 ? impl_->parents[hipsIndex] : -1;
    const auto addHipsOffset = [&](Vec3 modelOffset) {
        Vec3 offset = modelOffset;
        if (hipsParent >= 0) {
            offset = math::rotate(
                model.bindGlobalRotations[static_cast<std::size_t>(hipsParent)].conjugate(),
                offset);
        }
        locals[hipsIndex].m[12] += offset.x;
        locals[hipsIndex].m[13] += offset.y;
        locals[hipsIndex].m[14] += offset.z;
    };
    const Vec3 authoredHips = toModel(pose.hips) * height;
    if (impl_->hips >= 0) {
        addHipsOffset(authoredHips);
    }
    impl_->evaluate(locals, globals);

    // --- root and stage-space foot targets ---------------------------------
    const float yaw = safeClamp(pose.rootYaw, -4.0f, 4.0f) * 2.0f * kPi;
    const Quat rootRot = math::fromAxisAngle(up, yaw);
    const Quat rootRotInv = rootRot.conjugate();
    Vec3 pivot = (impl_->legs[0].bindAnkle + impl_->legs[1].bindAnkle) * 0.5f;
    pivot = pivot - up * math::dot(pivot, up);
    const Vec3 rootPos = (left * pose.rootX + forward * pose.rootZ) * height;

    Vec3 targets[2];
    Quat footDelta[2];
    Vec3 poles[2];
    for (int f = 0; f < 2; ++f) {
        const motion::ClipPose::Foot& foot = pose.foot[static_cast<std::size_t>(f)];
        const Vec3 stage = impl_->legs[f].bindAnkle + toModel(foot.position) * height;
        const Vec3 rel = stage - pivot - rootPos;
        targets[f] = pivot + math::rotate(rootRotInv, rel);
        const float footYaw = foot.yaw * 2.0f * kPi - yaw;
        const float lift = std::max(0.0f, foot.position.y);
        const float pointed = (1.0f - foot.contact) *
                              std::clamp(lift / 0.05f, 0.0f, 1.0f) * 0.42f;
        const Quat yawQuat = math::fromAxisAngle(up, footYaw);
        footDelta[f] = (yawQuat * math::fromAxisAngle(left, pointed)).normalized();
        // The knee points where the toes point (half-way, so it stays natural).
        poles[f] = math::normalize(forward * 0.6f + math::rotate(yawQuat, forward) * 0.4f);
    }

    // --- make the planted feet reachable ------------------------------------
    // The bind legs are almost straight, so lowering the pelvis is how a wide
    // stance or a lateral weight shift is paid for. Without this a stance foot
    // would be pulled off the floor by the IK reach clamp.
    float extraDrop = 0.0f;
    if (options.footIK && impl_->hips >= 0) {
        float need[2] = {0.0f, 0.0f};
        for (int f = 0; f < 2; ++f) {
            const LegRig& leg = impl_->legs[f];
            if (leg.upper < 0) continue;
            const Vec3 hip = impl_->actualPosition(globals, leg.upper);
            const Vec3 d = hip - targets[f];
            const float dv = math::dot(d, up);
            const Vec3 dh = d - up * dv;
            const float reach = 0.972f * (leg.upperLength + leg.lowerLength);
            const float dhLen = dh.length();
            if (dhLen < reach) {
                need[f] = std::max(0.0f, dv - std::sqrt(reach * reach - dhLen * dhLen));
            } else {
                need[f] = std::max(0.0f, dv);
            }
        }
        const float eps = 0.004f * height;
        extraDrop = 0.5f * (need[0] + need[1] +
                            std::sqrt((need[0] - need[1]) * (need[0] - need[1]) + eps * eps)) -
                    0.5f * eps;
        extraDrop = std::max(0.0f, extraDrop);
        if (extraDrop > 0.0f) {
            addHipsOffset(up * (-extraDrop));
            impl_->evaluate(locals, globals);
        }
    }

    float footError = 0.0f;
    float lowest = std::numeric_limits<float>::max();
    Vec3 reachable[2] = {targets[0], targets[1]};
    if (options.footIK) {
        for (int f = 0; f < 2; ++f) {
            impl_->solveLeg(impl_->legs[f], globals, locals, targets[f], poles[f],
                            footError, lowest, reachable[f], footDelta[f]);
        }
    }
    // Heel / toe roll: rotate the toe bone after the ankle IK so the sole does
    // not detach from the floor target. Small (0.30 rad at full strength).
    for (int f = 0; f < 2; ++f) {
        const float toePitch = std::clamp(
            pose.foot[static_cast<std::size_t>(f)].toePitch, -1.0f, 1.0f);
        if (impl_->legs[f].toes >= 0 && std::abs(toePitch) > 1.0e-4f) {
            impl_->applyLocalDelta(
                locals, impl_->legs[f].toes,
                quatToModel(math::fromAxisAngle({-1.0f, 0.0f, 0.0f}, toePitch * 0.30f)));
        }
    }
    impl_->evaluate(locals, globals);
    if (options.footIK) {
        impl_->measureFootError(globals, targets, reachable, out.stats.maxFootError,
                                out.stats.footSolverError);
    } else {
        // No IK: report how far the carried feet are from the clip's targets so
        // the number stays meaningful, and the lowest actual foot height.
        float actual = 0.0f;
        for (int f = 0; f < 2; ++f) {
            const int foot = impl_->legs[f].foot;
            if (foot >= 0) {
                const Vec3 ankle = impl_->actualPosition(globals, foot);
                actual = std::max(actual, (ankle - targets[f]).length());
                lowest = std::min(lowest, ankle.y);
            }
        }
        out.stats.maxFootError = actual;
    }

    out.modelTransform = Mat4::fromQuat(rootRot);
    out.rootOffset = rootPos;
    out.rootPivot = pivot;
    out.nodeGlobals = std::move(globals);
    out.stats.valid = true;
    out.stats.clipDriven = true;
    out.stats.footIK = options.footIK;
    out.stats.detailedHands = pose.detailedHands && impl_->fingerBones > 0;
    out.stats.height = height;
    out.stats.hipDrop = extraDrop;
    out.stats.reachHipDrop = extraDrop;
    out.stats.requestedHipDrop = -math::dot(authoredHips, up);
    out.stats.footReachDeficit = footError;
    out.stats.lowestFootY = lowest - impl_->floorY;
    for (int node : impl_->boneNode) {
        if (node >= 0) {
            ++out.stats.resolvedBones;
        } else {
            ++out.stats.missingBones;
        }
    }
    out.stats.fingerBones = impl_->fingerBones;
    out.stats.retargetMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    for (const Mat4& matrix : out.nodeGlobals) {
        if (!matrix.finite()) {
            out.stats.valid = false;
            std::vector<Mat4> fallback;
            impl_->evaluate(impl_->bindLocals, fallback);
            out.nodeGlobals = std::move(fallback);
            out.stats.footIK = false;
            break;
        }
    }
    return out;
}

Vec3 HumanoidRetargeter::stagePosition(const RetargetedPose& pose, int node) {
    if (node < 0 || static_cast<std::size_t>(node) >= pose.nodeGlobals.size()) {
        return {};
    }
    const Vec3 local = pose.nodeGlobals[static_cast<std::size_t>(node)].translationPart();
    return pose.rootOffset + pose.rootPivot +
           pose.modelTransform.transformVector(local - pose.rootPivot);
}

Vec3 RetargetedPose::bonePosition(HumanoidBone bone,
                                  const vrm::VrmModel& model) const {
    const int node = model.nodeFor(bone);
    if (node < 0 || static_cast<std::size_t>(node) >= nodeGlobals.size()) {
        return {};
    }
    return nodeGlobals[static_cast<std::size_t>(node)].translationPart();
}

HumanoidRetargeter::HumanoidRetargeter(const vrm::VrmModel& model)
    : impl_(std::make_unique<Impl>()) {
    impl_->initialize(model);
}

HumanoidRetargeter::~HumanoidRetargeter() = default;
HumanoidRetargeter::HumanoidRetargeter(HumanoidRetargeter&&) noexcept = default;
HumanoidRetargeter& HumanoidRetargeter::operator=(HumanoidRetargeter&&) noexcept =
    default;

bool HumanoidRetargeter::valid() const { return impl_ && impl_->valid; }

const std::string& HumanoidRetargeter::error() const {
    static const std::string empty;
    return impl_ ? impl_->error : empty;
}

} // namespace bgmedia::animation
