#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/animation/TwoBoneSolver.hpp"

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

// Rest bend of a joint: its offset from the bind reach line, in model space. A
// straight bind has no offset, so the fallback axis (the joint's natural side) is used.
Vec3 restBendOf(Vec3 root, Vec3 joint, Vec3 end, Vec3 fallback) {
    const Vec3 line = end - root;
    const float length = line.length();
    if (length > kTiny) {
        const Vec3 direction = line / length;
        const Vec3 offset = (joint - root) - direction * math::dot(joint - root, direction);
        if (offset.lengthSquared() > 1.0e-10f) {
            return math::normalize(offset);
        }
    }
    return math::normalize(fallback);
}

// Free weight of a foot: 1 when it is in the air, 0 when it is planted (contact >= 0.5).
float freeWeightOf(float contact) {
    return std::clamp(1.0f - 2.0f * contact, 0.0f, 1.0f);
}

// Solver limits of one limb. Off (BG Motion 3.2 hard reach) unless softIK is set; then the
// stable bend is on, and the flexion window is enforced only where freeWeight > 0.
TwoBoneLimits limbLimits(bool softIK, float length, float softFraction, float flexMin,
                         float flexMax, float freeWeight) {
    TwoBoneLimits limits;
    if (!softIK) {
        return limits;
    }
    limits.softReach = softFraction * length * std::clamp(freeWeight, 0.0f, 1.0f);
    limits.stableBend = true;
    limits.flexWindow = true;
    limits.flexMin = flexMin;
    limits.flexMax = flexMax;
    limits.flexEnforced = freeWeight > 0.0f;
    return limits;
}

// Diagnostics of one solve. Only called with soft IK on (see the callers).
void noteSolve(RetargetStats& stats, const TwoBoneSolution& solution, bool planted) {
    if (solution.softShift != 0.0f) {
        ++stats.softReachSolves;
        stats.softReachCompression = std::max(stats.softReachCompression, std::abs(solution.softShift));
    }
    if (solution.bendBlended) {
        ++stats.bendBlends;
    }
    if (solution.flexBound) {
        if (planted) {
            ++stats.plantedFlexConflicts;
        } else {
            ++stats.flexLimits;
        }
    }
    if (planted && solution.beyondReach) {
        ++stats.plantedReachConflicts;
    }
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
    TwoBoneChain chain;  // the one two-bone solve of this leg
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

    // Arm collision proxies (cached, built once in initialize()).
    CollisionProxyInfo proxy;
    float handCenterOffset = 0.0f;  // wrist -> palm centre, metres

    bool initialize(const vrm::VrmModel& source);
    void calibrateProxies();
    // Rest-pose two-bone geometry of each arm (left, right); lengths are taken per frame.
    TwoBoneChain armChains[2];
    // Pushes the forearms/hands out of the torso proxy. Arm bones only: the
    // hips and legs are untouched, so foot IK is unaffected.
    void refineArms(std::vector<Mat4>& locals, std::vector<Mat4>& globals,
                    const RetargetOptions& options, RetargetStats& stats) const;
    // Pushes the FREE foot's IK target away from the other leg (see RetargetOptions::legCollision).
    void refineLegs(const std::vector<Mat4>& globals, Vec3 targets[2], const Vec3 poles[2],
                    const float contact[2], const TwoBoneLimits limits[2], bool solvedEndpoints,
                    float margin, RetargetStats& stats) const;
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
                  float& lowestY, Vec3& reachableTarget, const TwoBoneLimits& limits,
                  bool planted, RetargetStats& stats,
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
        // The knee's natural side is the bind offset from the hip-ankle line; a straight
        // bind falls back to forward (knees bend forward).
        leg.chain.upper = leg.upperLength;
        leg.chain.lower = leg.lowerLength;
        leg.chain.restBend = restBendOf(leg.bindUpper, leg.bindKnee, leg.bindAnkle, source.forward);
        leg.chain.up = source.up;
        leg.chain.right = source.right;
        leg.chain.forward = source.forward;
    };
    setupLeg(legs[0], vrm::HumanoidBone::LeftUpperLeg,
             vrm::HumanoidBone::LeftLowerLeg, vrm::HumanoidBone::LeftFoot,
             vrm::HumanoidBone::LeftToes);
    setupLeg(legs[1], vrm::HumanoidBone::RightUpperLeg,
             vrm::HumanoidBone::RightLowerLeg, vrm::HumanoidBone::RightFoot,
             vrm::HumanoidBone::RightToes);
    // Arms: the elbow's natural side is the opposite of the knee's (elbows bend backward).
    const auto setupArm = [&](TwoBoneChain& chain, int upper, int lower, int hand) {
        chain.restBend = restBendOf(bindPosition(upper), bindPosition(lower),
                                    bindPosition(hand), source.forward * -1.0f);
        chain.up = source.up;
        chain.right = source.right;
        chain.forward = source.forward;
    };
    setupArm(armChains[0], leftUpperArm, leftLowerArm, leftHand);
    setupArm(armChains[1], rightUpperArm, rightLowerArm, rightHand);
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
    calibrateProxies();
    return true;
}

// ---------------------------------------------------------------------------
// Arm / torso collision proxies
// ---------------------------------------------------------------------------
namespace {

float percentile(std::vector<float>& values, float q) {
    if (values.empty()) {
        return 0.0f;
    }
    const std::size_t index = std::min(
        values.size() - 1, static_cast<std::size_t>(q * static_cast<float>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());
    return values[index];
}

float distanceToSegment(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const float len2 = ab.lengthSquared();
    const float t = len2 > kTiny ? std::clamp(math::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return (p - (a + ab * t)).length();
}

// Smooth ramp of a (negative) clearance into a push distance. C1 at zero, so the
// correction never switches on abruptly.
float softPush(float clearance, float eps) {
    const float c = -clearance;
    return std::max(0.0f, 0.5f * (c + std::sqrt(c * c + eps * eps)) - 0.5f * eps);
}

}  // namespace

void HumanoidRetargeter::Impl::calibrateProxies() {
    proxy = CollisionProxyInfo{};
    const vrm::VrmModel& m = *model;
    if (leftUpperArm < 0 || rightUpperArm < 0 || leftHand < 0 || rightHand < 0) {
        return;
    }
    const Vec3 shoulderL = bindPosition(leftUpperArm);
    const Vec3 shoulderR = bindPosition(rightUpperArm);
    const float span = (shoulderL - shoulderR).length();
    const Vec3 hipsPos = bindPosition(hips);
    const Vec3 neckPos = bindPosition(neck >= 0 ? neck : head);
    const Vec3 axisUp = math::normalize(neckPos - hipsPos);

    // Skeleton-only fallback: torso proportions from the shoulder span.
    proxy.torsoHalfWidth = 0.46f * span;
    proxy.torsoHalfFront = 0.28f * span;
    proxy.torsoHalfBack = 0.26f * span;
    proxy.fingerRadius = 0.008f * height;
    for (int band = 0; band < kProxyBands; ++band) {
        proxy.bandWidth[band] = proxy.torsoHalfWidth * (band == 0 ? 0.80f : 1.0f);
        proxy.bandFront[band] = proxy.torsoHalfFront;
        proxy.bandBack[band] = proxy.torsoHalfBack;
    }
    proxy.upperArmRadius = 0.020f * height;
    proxy.foreArmRadius = 0.017f * height;
    proxy.handRadius = 0.030f * height;
    proxy.valid = span > 0.05f;

    // Wrist -> palm centre: half the distance to the middle finger root.
    handCenterOffset = 0.045f * height;
    if (fingers[0][2][0] >= 0 && leftHand >= 0) {
        handCenterOffset = 0.5f * (bindPosition(fingers[0][2][0]) - bindPosition(leftHand)).length();
    }

    // Mesh calibration: classify skinned vertices by dominant joint.
    enum : int { kNone = -1, kTorso = 0, kUpper = 1, kFore = 2, kHand = 3, kFinger = 4, kThigh = 5, kShin = 6, kFoot = 7 };
    std::vector<int> cls(m.nodes.size(), kNone);
    const auto mark = [&](int node, int c) {
        if (node >= 0 && static_cast<std::size_t>(node) < cls.size()) {
            cls[static_cast<std::size_t>(node)] = c;
        }
    };
    mark(spine, kTorso);
    mark(chest, kTorso);
    mark(upperChest, kTorso);
    mark(leftUpperArm, kUpper);
    mark(rightUpperArm, kUpper);
    mark(leftLowerArm, kFore);
    mark(rightLowerArm, kFore);
    mark(leftHand, kHand);
    mark(rightHand, kHand);
    for (int sd = 0; sd < 2; ++sd) {
        mark(legs[sd].upper, kThigh);
        mark(legs[sd].lower, kShin);
        mark(legs[sd].foot, kFoot);
        mark(legs[sd].toes, kFoot);
    }
    for (int sd = 0; sd < 2; ++sd)
        for (int fg = 0; fg < 5; ++fg)
            for (int jt = 0; jt < 3; ++jt) mark(fingers[sd][fg][jt], kFinger);
    // Bone segment of a finger joint (joint -> next joint; the distal bone is extended
    // by the length of the intermediate one).
    const auto fingerSegment = [&](int node, Vec3& a, Vec3& b) {
        for (int sd = 0; sd < 2; ++sd)
            for (int fg = 0; fg < 5; ++fg)
                for (int jt = 0; jt < 3; ++jt) {
                    if (fingers[sd][fg][jt] != node) continue;
                    a = bindPosition(node);
                    if (jt < 2 && fingers[sd][fg][jt + 1] >= 0) {
                        b = bindPosition(fingers[sd][fg][jt + 1]);
                        return true;
                    }
                    if (jt > 0 && fingers[sd][fg][jt - 1] >= 0) {
                        b = a + (a - bindPosition(fingers[sd][fg][jt - 1]));
                        return true;
                    }
                    return false;
                }
        return false;
    };

    struct TorsoSample {
        float x, z;  // |lateral|, signed forward
        int band;
    };
    std::vector<TorsoSample> torsoSamples;
    std::vector<float> lateral[kProxyBands], front[kProxyBands], back[kProxyBands], rUpper, rFore, rHand, rFinger, rThigh, rShin, rFoot;
    const float torsoSpan = (neckPos - hipsPos).length();
    const Vec3 right = m.right;
    const Vec3 forward = m.forward;
    const Vec3 origin = hipsPos;
    for (const vrm::MeshInstance& inst : m.meshInstances) {
        if (inst.mesh < 0 || static_cast<std::size_t>(inst.mesh) >= m.meshes.size()) {
            continue;
        }
        const vrm::Skin* skin = m.skinForNode(inst.node);
        if (!skin) {
            continue;
        }
        for (const vrm::Primitive& prim : m.meshes[static_cast<std::size_t>(inst.mesh)].primitives) {
            if (!prim.skinned) {
                continue;
            }
            for (const vrm::Vertex& v : prim.vertices) {
                std::size_t best = 0;
                for (std::size_t k = 1; k < 4; ++k) {
                    if (v.weights[k] > v.weights[best]) best = k;
                }
                if (v.weights[best] < 0.5f || v.joints[best] >= skin->joints.size() ||
                    v.joints[best] >= skin->inverseBindMatrices.size()) {
                    continue;
                }
                const int node = skin->joints[v.joints[best]];
                if (node < 0 || static_cast<std::size_t>(node) >= cls.size() ||
                    cls[static_cast<std::size_t>(node)] == kNone) {
                    continue;
                }
                // Rest position of the vertex (bind pose skinning, dominant joint).
                const Vec3 p = m.bindGlobals[static_cast<std::size_t>(node)].transformPoint(
                    skin->inverseBindMatrices[v.joints[best]].transformPoint(v.position));
                if (!math::finite(p)) {
                    continue;
                }
                const int c = cls[static_cast<std::size_t>(node)];
                if (c == kTorso) {
                    const Vec3 d = p - origin;
                    const float along = math::dot(d, axisUp);
                    if (along < 0.0f || along > torsoSpan) {
                        continue;  // only the waist / rib cage / shoulder band
                    }
                    const int band = std::min(kProxyBands - 1,
                                              static_cast<int>(static_cast<float>(kProxyBands) * along / torsoSpan));
                    lateral[band].push_back(std::abs(math::dot(d, right)));
                    const float f = math::dot(d, forward);
                    torsoSamples.push_back({std::abs(math::dot(d, right)), f, band});
                    (f >= 0.0f ? front[band] : back[band]).push_back(std::abs(f));
                } else if (c == kUpper) {
                    const bool leftSide = node == leftUpperArm;
                    rUpper.push_back(distanceToSegment(
                        p, bindPosition(leftSide ? leftUpperArm : rightUpperArm),
                        bindPosition(leftSide ? leftLowerArm : rightLowerArm)));
                } else if (c == kFore) {
                    const bool leftSide = node == leftLowerArm;
                    rFore.push_back(distanceToSegment(
                        p, bindPosition(leftSide ? leftLowerArm : rightLowerArm),
                        bindPosition(leftSide ? leftHand : rightHand)));
                } else if (c == kThigh || c == kShin || c == kFoot) {
                    const int sd = (node == legs[0].upper || node == legs[0].lower || node == legs[0].foot || node == legs[0].toes) ? 0 : 1;
                    const LegRig& lg = legs[sd];
                    if (c == kThigh) {
                        rThigh.push_back(distanceToSegment(p, bindPosition(lg.upper), bindPosition(lg.lower)));
                    } else if (c == kShin) {
                        rShin.push_back(distanceToSegment(p, bindPosition(lg.lower), bindPosition(lg.foot)));
                    } else {
                        const Vec3 ankle = bindPosition(lg.foot);
                        const Vec3 toe = lg.toes >= 0 ? bindPosition(lg.toes) : ankle + forward * (0.05f * height);
                        rFoot.push_back(distanceToSegment(p, ankle, ankle + (toe - ankle) * 1.6f));
                    }
                } else if (c == kFinger) {
                    Vec3 sa{}, sb{};
                    if (fingerSegment(node, sa, sb)) {
                        rFinger.push_back(distanceToSegment(p, sa, sb));
                    }
                } else {
                    const bool leftSide = node == leftHand;
                    const Vec3 wrist = bindPosition(leftSide ? leftHand : rightHand);
                    const Vec3 outward = math::normalize(wrist - bindPosition(leftSide ? leftLowerArm
                                                                                       : rightLowerArm));
                    rHand.push_back((p - (wrist + outward * handCenterOffset)).length());
                }
            }
        }
    }
    // A band with too few vertices in one direction (e.g. a bare back) borrows
    // the nearest band that has data; if there is none the skeleton fallback
    // stays in place.
    constexpr std::size_t kMinSamples = 8;
    // The proxy follows the body including vest and pouches (P95), but not the few
    // outlier vertices (a strap tail, a buckle); the clamps are in shoulder spans.
    constexpr float kTorsoPercentile = 0.95f;
    const auto fill = [&](std::vector<float> (&lists)[kProxyBands], float* out, float lo, float hi) {
        bool any = false;
        float value[kProxyBands] = {};
        bool have[kProxyBands] = {};
        for (int band = 0; band < kProxyBands; ++band) {
            if (lists[band].size() >= kMinSamples) {
                value[band] = std::clamp(percentile(lists[band], kTorsoPercentile), lo, hi);
                have[band] = true;
                any = true;
            }
        }
        if (!any) {
            return false;
        }
        for (int band = 0; band < kProxyBands; ++band) {
            int source = -1;
            for (int d = 0; d < kProxyBands && source < 0; ++d) {
                for (int cand : {band - d, band + d}) {
                    if (cand >= 0 && cand < kProxyBands && have[cand]) {
                        source = cand;
                        break;
                    }
                }
            }
            out[band] = value[source];
        }
        return true;
    };
    float bw[kProxyBands], bf[kProxyBands], bb[kProxyBands];
    if (fill(lateral, bw, 0.20f * span, 0.80f * span) &&
        fill(front, bf, 0.08f * span, 0.70f * span) &&
        fill(back, bb, 0.08f * span, 0.70f * span)) {
        proxy.fromMesh = true;
        for (int band = 0; band < kProxyBands; ++band) {
            proxy.bandWidth[band] = bw[band];
            proxy.bandFront[band] = bf[band];
            proxy.bandBack[band] = bb[band];
        }
        // Fit the cross-section exponent: the smallest p (2 = ellipse .. 4 = rounded box)
        // for which at most 6 % of the torso vertices lie outside the proxy. A vest is
        // boxier than an ellipse; an ellipse cuts its front corners, which is exactly
        // where a forearm folded across the body ends up.
        proxy.shapeExponent = 4.0f;
        for (float pExp = 2.0f; pExp <= 4.0f; pExp += 0.25f) {
            std::size_t outside = 0;
            for (const TorsoSample& t : torsoSamples) {
                const float a = std::max(1.0e-4f, bw[t.band]);
                const float b = std::max(1.0e-4f, t.z >= 0.0f ? bf[t.band] : bb[t.band]);
                const float v = std::pow(t.x / a, pExp) + std::pow(std::abs(t.z) / b, pExp);
                outside += v > 1.0f;
            }
            if (!torsoSamples.empty() &&
                static_cast<float>(outside) <= 0.06f * static_cast<float>(torsoSamples.size())) {
                proxy.shapeExponent = pExp;
                break;
            }
        }
        proxy.torsoHalfWidth = *std::max_element(bw, bw + kProxyBands);
        proxy.torsoHalfFront = *std::max_element(bf, bf + kProxyBands);
        proxy.torsoHalfBack = *std::max_element(bb, bb + kProxyBands);
    }
    if (rUpper.size() >= 50) {
        proxy.upperArmRadius = std::clamp(percentile(rUpper, 0.85f), 0.010f * height, 0.060f * height);
    }
    if (rFore.size() >= 50) {
        proxy.foreArmRadius = std::clamp(percentile(rFore, 0.95f), 0.010f * height, 0.060f * height);
    }
    proxy.thighRadius = 0.045f * height;
    proxy.shinRadius = 0.032f * height;
    proxy.footRadius = 0.026f * height;
    {
        const Vec3 ankle = bindPosition(legs[0].foot);
        const Vec3 toe = legs[0].toes >= 0 ? bindPosition(legs[0].toes) : ankle + m.forward * (0.05f * height);
        proxy.footLength = std::max(0.05f * height, 1.6f * (toe - ankle).length());
    }
    if (rThigh.size() >= 50) {
        proxy.thighRadius = std::clamp(percentile(rThigh, 0.90f), 0.020f * height, 0.090f * height);
    }
    if (rShin.size() >= 50) {
        proxy.shinRadius = std::clamp(percentile(rShin, 0.90f), 0.015f * height, 0.070f * height);
    }
    if (rFoot.size() >= 50) {
        proxy.footRadius = std::clamp(percentile(rFoot, 0.90f), 0.012f * height, 0.060f * height);
    }
    {
        // The proxy must not call the model's own standing pose a collision: neutral legs hang
        // side by side and, with mesh-measured radii, thick thighs already overlap. Scale the
        // three leg radii so that the bind stance keeps a small positive gap; legs that are
        // spread wider than the radii keep their measured size (scale 1).
        float scale = 1.0f;
        const float fwdLen = proxy.footLength;
        struct C {
            Vec3 a, b;
            float r, from;
        };
        C caps[2][3];
        for (int f = 0; f < 2; ++f) {
            caps[f][0] = {legs[f].bindUpper, legs[f].bindKnee, proxy.thighRadius, 0.45f};
            caps[f][1] = {legs[f].bindKnee, legs[f].bindAnkle, proxy.shinRadius, 0.0f};
            caps[f][2] = {legs[f].bindAnkle, legs[f].bindAnkle + m.forward * fwdLen, proxy.footRadius, 0.0f};
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                const float from = (i == 0 && j == 0) ? 0.45f : 0.0f;
                for (int sa = 0; sa <= 6; ++sa) {
                    const Vec3 pa = caps[0][i].a + (caps[0][i].b - caps[0][i].a) * (((i == 0) ? 0.45f : 0.0f) + (1.0f - ((i == 0) ? 0.45f : 0.0f)) * static_cast<float>(sa) / 6.0f);
                    for (int sb = 0; sb <= 6; ++sb) {
                        const Vec3 pb = caps[1][j].a + (caps[1][j].b - caps[1][j].a) * (((j == 0) ? 0.45f : 0.0f) + (1.0f - ((j == 0) ? 0.45f : 0.0f)) * static_cast<float>(sb) / 6.0f);
                        const float radii = caps[0][i].r + caps[1][j].r;
                        const float dist = (pa - pb).length();
                        if (radii > 1.0e-5f) {
                            scale = std::min(scale, (dist - 0.003f * height) / radii);
                        }
                    }
                }
                (void)from;
            }
        }
        scale = std::clamp(scale, 0.35f, 1.0f);
        proxy.thighRadius *= scale;
        proxy.shinRadius *= scale;
        proxy.footRadius *= scale;
        proxy.legRadiusScale = scale;
    }
    if (rFinger.size() >= 50) {
        proxy.fingerRadius = std::clamp(percentile(rFinger, 0.90f), 0.004f * height, 0.030f * height);
    }
    if (rHand.size() >= 50) {
        proxy.handRadius = std::clamp(percentile(rHand, 0.80f), 0.015f * height, 0.080f * height);
    }
}

void HumanoidRetargeter::Impl::refineArms(std::vector<Mat4>& locals,
                                          std::vector<Mat4>& globals,
                                          const RetargetOptions& options,
                                          RetargetStats& stats) const {
    if (!proxy.valid || hips < 0) {
        return;
    }
    stats.armProxy = true;
    const vrm::VrmModel& m = *model;

    // Torso frame from the *current* skeleton: the proxy leans and twists with
    // the spine. Axes are the bind axes carried by the chest's rotation delta.
    const int chestNode = upperChest >= 0 ? upperChest : (chest >= 0 ? chest : spine);
    Quat chestDelta = Quat::identity();
    if (chestNode >= 0) {
        chestDelta = (globals[static_cast<std::size_t>(chestNode)].rotationQuat() *
                      m.bindGlobalRotations[static_cast<std::size_t>(chestNode)].conjugate())
                         .normalized();
    }
    const Vec3 hipsPos = actualPosition(globals, hips);
    const Vec3 neckPos = actualPosition(globals, neck >= 0 ? neck : head);
    Vec3 up = neckPos - hipsPos;
    const float torsoLength = up.length();
    if (torsoLength < 0.05f) {
        return;
    }
    up = up / torsoLength;
    Vec3 rightAxis = math::rotate(chestDelta, m.right);
    rightAxis = math::normalize(rightAxis - up * math::dot(rightAxis, up));
    // Model convention: right = up x forward, hence forward = right x up.
    const Vec3 forwardAxis = math::normalize(math::cross(rightAxis, up));
    const Vec3 fwd = math::dot(forwardAxis, math::rotate(chestDelta, m.forward)) >= 0.0f
                         ? forwardAxis
                         : forwardAxis * -1.0f;

    const float gap = options.armClearanceMargin * height;
    const float eps = 0.004f * height;
    const float armLimit = 0.35f;  // fraction of arm length a correction may move a joint

    // Signed clearance of a sphere (centre p, radius r) against the elliptical
    // torso cylinder, with the outward normal. Approximate: distance of the
    // normalised ellipse boundary, divided by its gradient length.
    const auto clearance = [&](Vec3 p, float r, Vec3* normal) {
        const Vec3 d = p - hipsPos;
        const float y = math::dot(d, up);
        const float x = math::dot(d, rightAxis);
        const float z = math::dot(d, fwd);
        // Band-interpolated cross-section at this height along the spine.
        // Band centres sit at (i + 0.5) / kProxyBands of the spine length.
        const float t = std::clamp(y / torsoLength * static_cast<float>(kProxyBands) - 0.5f, 0.0f,
                                   static_cast<float>(kProxyBands - 1));
        const int b0 = std::min(kProxyBands - 2, static_cast<int>(t));
        const float k = t - static_cast<float>(b0);
        const auto lerpBand = [&](const float* band) {
            return band[b0] * (1.0f - k) + band[b0 + 1] * k;
        };
        const float b = std::max(0.02f, z >= 0.0f ? lerpBand(proxy.bandFront)
                                                  : lerpBand(proxy.bandBack));
        const float a = std::max(0.02f, lerpBand(proxy.bandWidth));
        // Super-ellipse radius rr = (|x/a|^p + |z/b|^p)^(1/p); rr = 1 on the surface.
        const float pe = proxy.shapeExponent;
        const float ux = std::abs(x) / a;
        const float uz = std::abs(z) / b;
        const float rr = std::pow(std::pow(ux, pe) + std::pow(uz, pe), 1.0f / pe);
        float surface;
        Vec3 n;
        if (rr < 1.0e-4f) {
            n = rightAxis;
            surface = -std::min(a, b);
        } else {
            // grad rr = rr^(1-p) * (sign(x) ux^(p-1) / a, sign(z) uz^(p-1) / b)
            const float scale = std::pow(rr, 1.0f - pe);
            const float gx = (x >= 0.0f ? 1.0f : -1.0f) * std::pow(ux, pe - 1.0f) / a * scale;
            const float gz = (z >= 0.0f ? 1.0f : -1.0f) * std::pow(uz, pe - 1.0f) / b * scale;
            const float glen = std::max(1.0e-6f, std::sqrt(gx * gx + gz * gz));
            surface = (rr - 1.0f) / glen;
            n = math::normalize(rightAxis * (gx / glen) + fwd * (gz / glen));
        }
        // Past the ends of the torso the cylinder fades out: arms above the
        // shoulders or below the pelvis cannot hit it.
        const float over = std::max({-y, y - torsoLength, 0.0f});
        if (normal) *normal = n;
        return std::max(surface, over) - r;
    };

    struct ArmNodes {
        int upper, lower, hand;
    };
    const ArmNodes arms[2] = {{leftUpperArm, leftLowerArm, leftHand},
                              {rightUpperArm, rightLowerArm, rightHand}};
    float rawMin = std::numeric_limits<float>::max();
    float finalMin = std::numeric_limits<float>::max();
    bool changed = false;

    for (int side = 0; side < 2; ++side) {
        const ArmNodes& arm = arms[side];
        if (arm.upper < 0 || arm.lower < 0 || arm.hand < 0) {
            continue;
        }
        const Vec3 S = actualPosition(globals, arm.upper);
        Vec3 E = actualPosition(globals, arm.lower);
        Vec3 W = actualPosition(globals, arm.hand);
        const float l1 = (E - S).length();
        const float l2 = (W - E).length();
        if (l1 < 0.02f || l2 < 0.02f) {
            continue;
        }
        TwoBoneLimits armLimits = limbLimits(options.softIK, l1 + l2, options.armSoftReach,
                                             options.elbowFlexMin, options.elbowFlexMax, 1.0f);
        // BG Motion 3.2's arm pole threshold (squared). With soft IK off, the arm solve must
        // match 3.2 exactly, so a pole this small still sets the bend side.
        armLimits.poleEpsilon = 1.0e-10f;
        const Vec3 E0 = E;
        const Vec3 W0 = W;

        // Hand control points, as offsets from the wrist. The hand is carried rigidly
        // (its world orientation is preserved below), so the offsets stay valid while
        // the wrist moves. Points: palm centre, index and little knuckles, and for each
        // finger its middle joint and tip (the distal bone extended by one more bone).
        struct HandPoint {
            Vec3 offset;
            float radius;
        };
        std::vector<HandPoint> hand;
        const int sd = arm.hand == rightHand ? 1 : 0;
        hand.push_back({math::normalize(W0 - E0) * handCenterOffset, proxy.handRadius});
        for (int fg : {1, 4}) {
            if (fingers[sd][fg][0] >= 0) {
                hand.push_back({actualPosition(globals, fingers[sd][fg][0]) - W0, proxy.handRadius * 0.7f});
            }
        }
        for (int fg = 0; fg < 5; ++fg) {
            const int j0 = fingers[sd][fg][0], j1 = fingers[sd][fg][1], j2 = fingers[sd][fg][2];
            if (j1 >= 0) {
                hand.push_back({actualPosition(globals, j1) - W0, proxy.fingerRadius});
            }
            if (j2 >= 0 && j1 >= 0) {
                const Vec3 distal = actualPosition(globals, j2);
                const Vec3 tip = distal + (distal - actualPosition(globals, j1));
                hand.push_back({distal - W0, proxy.fingerRadius});
                hand.push_back({tip - W0, proxy.fingerRadius});
            } else if (j0 >= 0 && j1 < 0) {
                hand.push_back({actualPosition(globals, j0) - W0, proxy.fingerRadius});
            }
        }
        float handReach = 0.0f;  // how far any hand point (incl. its radius) can be from the wrist
        for (const HandPoint& hp : hand) {
            handReach = std::max(handReach, hp.offset.length() + hp.radius - proxy.handRadius);
        }
        handReach += 0.002f;
        const std::size_t kFixed = 4;  // upper-mid (unused), elbow, forearm-mid, wrist
        const std::size_t sampleCount = kFixed + hand.size();
        std::vector<float> c(sampleCount);
        std::vector<Vec3> n(sampleCount);
        const auto samples = [&](Vec3 e, Vec3 w) {
            // Upper-arm mid is deliberately not sampled: a hanging arm lies
            // beside the torso by construction (the shoulder joint sits on its
            // edge), so only elbow, forearm and hand count.
            c[0] = 1.0f;
            c[1] = clearance(e, proxy.foreArmRadius + gap, &n[1]);
            c[2] = clearance(e + (w - e) * 0.5f, proxy.foreArmRadius + gap, &n[2]);
            c[3] = clearance(w, proxy.handRadius + gap, &n[3]);
            // Early out: when the wrist is farther from the torso than the whole hand can
            // reach, no finger point can be inside it (the clearance field is 1-Lipschitz).
            if (c[3] > handReach) {
                for (std::size_t i = 0; i < hand.size(); ++i) {
                    c[kFixed + i] = c[3] - handReach;
                    n[kFixed + i] = n[3];
                }
                return;
            }
            for (std::size_t i = 0; i < hand.size(); ++i) {
                c[kFixed + i] = clearance(w + hand[i].offset, hand[i].radius + gap, &n[kFixed + i]);
            }
        };
        samples(E, W);
        float armRaw = *std::min_element(c.begin(), c.end());
        rawMin = std::min(rawMin, armRaw);
        // The soft band and the stable bend fade in with the collision depth: full at 1 cm of height inside
        // the torso, zero at the surface. Without the fade, the band would switch on as the
        // arm crosses the surface and jump the wrist (6.5 mm on the issue #26 reproduction).
        // Soft off: softReach is already 0, so the weight changes nothing.
        {
            const float depthT = std::clamp(-armRaw / (0.01f * height), 0.0f, 1.0f);
            const float fade = depthT * depthT * (3.0f - 2.0f * depthT);
            armLimits.softReach *= fade;
            // The stable bend and the elbow flexion window fade in the same way: the
            // authored arm (FK) has neither, so applying them in full on the first
            // collision solve would snap the elbow.
            armLimits.stableBendWeight = fade;
            armLimits.flexWeight = fade;
        }

        // A few bounded iterations: moving the elbow changes the forearm sample.
        for (int it = 0; it < 6 && armRaw < 0.0f; ++it) {
            const float pE = softPush(c[1], eps);
            const float pF = softPush(c[2], eps);
            // The wrist follows the deepest hand point along that point's own outward normal.
            float handPush = softPush(c[3], eps);
            Vec3 handNormal = n[3];
            for (std::size_t i = kFixed; i < sampleCount; ++i) {
                const float p = softPush(c[i], eps);
                if (p > handPush) {
                    handPush = p;
                    handNormal = n[i];
                }
            }
            if (pE + pF + handPush <= 1.0e-6f) {
                break;
            }
            // A forearm point is pushed by moving elbow and wrist together (the forearm
            // translates), which needs the smallest swing: pivoting about a fixed elbow
            // would need twice the wrist move for the same forearm clearance.
            const float moveE = std::max(pE, pF);
            Vec3 moveWVec = handNormal * handPush;
            if (pF > handPush) {
                moveWVec = n[2] * pF;
            }
            const Vec3 targetE = E + (pF > pE ? n[2] : n[1]) * moveE;
            const Vec3 targetW = W + moveWVec;
            // Re-solve the two-bone chain: elbow on the circle closest to targetE.
            // The one two-bone solve of this arm (TwoBoneSolver): the elbow on the circle
            // around S-W, bent toward the elbow target.
            const Vec3 reach = targetW - S;
            const float dist = reach.length();
            if (dist < kTiny) {
                break;
            }
            const Vec3 pole = targetE - S;
            const Vec3 dir = reach / dist;
            const Vec3 poleSide = pole - dir * math::dot(pole, dir);
            if (!options.softIK && poleSide.lengthSquared() < 1.0e-10f) {
                break;  // BG Motion 3.2: a pole on the reach line gives no bend side
            }
            TwoBoneChain chain = armChains[side];
            chain.upper = l1;
            chain.lower = l2;
            const TwoBoneSolution solved = solveTwoBone(S, targetW, pole, chain, armLimits);
            if (armLimits.stableBend) {
                noteSolve(stats, solved, false);
            }
            Vec3 newE = solved.knee;
            Vec3 newW = solved.end;
            // Safe fallback: never move a joint further than a fraction of the arm.
            const float limit = armLimit * (l1 + l2);
            Vec3 dE = newE - E0;
            Vec3 dW = newW - W0;
            if (dE.length() > limit) {
                newE = E0 + dE * (limit / dE.length());
                stats.armCorrectionClamped = true;
            }
            if (dW.length() > limit) {
                newW = W0 + dW * (limit / dW.length());
                stats.armCorrectionClamped = true;
            }
            E = newE;
            W = newW;
            samples(E, W);
            armRaw = *std::min_element(c.begin(), c.end());
        }

        if ((E - E0).length() > 1.0e-6f || (W - W0).length() > 1.0e-6f) {
            // Swing-only update: keep the authored axial twist of both bones, and the
            // authored world orientation of the hand (its local rotation is re-derived).
            const Quat upperWorld0 = globals[static_cast<std::size_t>(arm.upper)].rotationQuat();
            const Quat lowerWorld0 = globals[static_cast<std::size_t>(arm.lower)].rotationQuat();
            const Quat handWorld0 = globals[static_cast<std::size_t>(arm.hand)].rotationQuat();
            const Quat swingU = math::fromTo(E0 - S, E - S);
            const Quat upperWorld = (swingU * upperWorld0).normalized();
            const Vec3 foreDirAfterU = math::rotate(swingU, W0 - E0);
            const Quat swingL = math::fromTo(foreDirAfterU, W - E);
            const Quat lowerWorld = (swingL * swingU * lowerWorld0).normalized();
            const int upperParent = parents[static_cast<std::size_t>(arm.upper)];
            const Quat parentWorld = upperParent >= 0
                ? globals[static_cast<std::size_t>(upperParent)].rotationQuat()
                : Quat::identity();
            const auto writeLocal = [&](int node, const Quat& local) {
                const std::size_t i = static_cast<std::size_t>(node);
                locals[i] = Mat4::fromTRS(
                    locals[i].translationPart(), local,
                    {bindLocals[i].axisX().length(), bindLocals[i].axisY().length(),
                     bindLocals[i].axisZ().length()});
            };
            writeLocal(arm.upper, (parentWorld.conjugate() * upperWorld).normalized());
            writeLocal(arm.lower, (upperWorld.conjugate() * lowerWorld).normalized());
            writeLocal(arm.hand, (lowerWorld.conjugate() * handWorld0).normalized());
            stats.armCorrectionElbow = std::max(stats.armCorrectionElbow, (E - E0).length());
            stats.armCorrectionWrist = std::max(stats.armCorrectionWrist, (W - W0).length());
            stats.armCorrection = std::max(
                stats.armCorrection, std::max((E - E0).length(), (W - W0).length()));
            changed = true;
        }
        finalMin = std::min(finalMin, armRaw);
    }
    if (changed) {
        evaluate(locals, globals);
    }
    stats.armClearanceRaw = rawMin == std::numeric_limits<float>::max() ? 0.0f : rawMin;
    stats.armClearanceMin = finalMin == std::numeric_limits<float>::max() ? 0.0f : finalMin;
}

void HumanoidRetargeter::Impl::refineLegs(const std::vector<Mat4>& globals, Vec3 targets[2],
                                          const Vec3 poles[2], const float contact[2],
                                          const TwoBoneLimits limits[2], bool solvedEndpoints,
                                          float margin,
                                          RetargetStats& stats) const {
    if (!proxy.valid || proxy.thighRadius <= 0.0f || legs[0].upper < 0 || legs[1].upper < 0) {
        return;
    }
    stats.legProxy = true;
    const float gap = margin * height;
    const float maxMove = 0.18f * height;
    const Vec3 up = model->up;
    const Vec3 forward = model->forward;
    Vec3 hip[2] = {actualPosition(globals, legs[0].upper), actualPosition(globals, legs[1].upper)};
    const Vec3 original[2] = {targets[0], targets[1]};

    // The chain the two-bone solver will build for a target: the same solve as solveLeg.
    const auto solveOf = [&](int f, Vec3 target) {
        return solveTwoBone(hip[f], target, poles[f], legs[f].chain, limits[f]);
    };

    struct Cap {
        Vec3 a, b;
        float r;
        float from;  // first usable parameter (the thighs touch at the crotch by design)
    };
    struct Hit {
        float clearance = 1.0e9f;
        Vec3 p0, p1;
        float u0 = 1.0f, u1 = 1.0f;  // position along each leg (0 hip .. 1 ankle)
    };
    const auto evaluate = [&]() {
        Cap caps[2][3];
        float legParam[2][3][2];  // leg parameter range of each capsule
        for (int f = 0; f < 2; ++f) {
            const TwoBoneSolution solution = solveOf(f, targets[f]);
            const Vec3 knee = solution.knee;
            const Vec3 ankle = solvedEndpoints ? solution.end : targets[f];
            const float lenT = (knee - hip[f]).length(), lenS = (ankle - knee).length();
            const float total = std::max(1.0e-4f, lenT + lenS);
            caps[f][0] = {hip[f], knee, proxy.thighRadius, 0.45f};
            caps[f][1] = {knee, ankle, proxy.shinRadius, 0.0f};
            caps[f][2] = {ankle, ankle + forward * proxy.footLength, proxy.footRadius, 0.0f};
            legParam[f][0][0] = 0.0f;
            legParam[f][0][1] = lenT / total;
            legParam[f][1][0] = lenT / total;
            legParam[f][1][1] = 1.0f;
            legParam[f][2][0] = legParam[f][2][1] = 1.0f;
        }
        Hit best;
        constexpr int kSamples = 6;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                const Cap& A = caps[0][i];
                const Cap& B = caps[1][j];
                const float fromA = (i == 0 && j == 0) ? std::max(A.from, B.from) : (i == 0 ? A.from : 0.0f);
                const float fromB = (i == 0 && j == 0) ? std::max(A.from, B.from) : (j == 0 ? B.from : 0.0f);
                for (int s = 0; s <= kSamples; ++s) {
                    const float ta = fromA + (1.0f - fromA) * static_cast<float>(s) / kSamples;
                    const Vec3 pa = A.a + (A.b - A.a) * ta;
                    for (int t = 0; t <= kSamples; ++t) {
                        const float tb = fromB + (1.0f - fromB) * static_cast<float>(t) / kSamples;
                        const Vec3 pb = B.a + (B.b - B.a) * tb;
                        const float c = (pa - pb).length() - A.r - B.r - gap;
                        if (c < best.clearance) {
                            best.clearance = c;
                            best.p0 = pa;
                            best.p1 = pb;
                            best.u0 = legParam[0][i][0] + (legParam[0][i][1] - legParam[0][i][0]) * ta;
                            best.u1 = legParam[1][j][0] + (legParam[1][j][1] - legParam[1][j][0]) * tb;
                        }
                    }
                }
            }
        }
        return best;
    };

    Hit hit = evaluate();
    stats.legClearanceRaw = hit.clearance;
    if (hit.clearance < 0.0f) {
        // Only a foot that is off the floor may move; a planted foot is never dragged.
        // (1 - contact)^1.5: the correction is made while the foot is really in the air and
        // fades out before it lands, so it never drags a foot across the floor.
        const float w0 = std::pow(1.0f - std::clamp(contact[0], 0.0f, 1.0f), 1.5f);
        const float w1 = std::pow(1.0f - std::clamp(contact[1], 0.0f, 1.0f), 1.5f);
        const float sum = w0 + w1;
        if (sum < 0.15f) {
            stats.legConflictPlanted = true;
        }
        // Mobility grows continuously with lift-off, so nothing switches at a contact change.
        const float norm = std::max(sum, 0.6f);
        const float a0 = w0 / norm;
        const float a1 = w1 / norm;
        if (a0 + a1 > 1.0e-3f) {
            // A deterministic way round: the free LEFT foot steps forward (in front of the
            // standing leg), the free RIGHT foot steps back (behind it), and both are raised
            // a little. The separating direction never depends on which capsule pair happens to
            // be the closest, which flips from frame to frame when legs lie on top of each other.
            const auto place = [&](float t) {
                targets[0] = original[0] + forward * (t * a0) + up * (0.4f * t * a0);
                targets[1] = original[1] - forward * (t * a1) + up * (0.4f * t * a1);
            };
            // Smallest travel that clears the legs; linear interpolation inside the last step
            // makes the result a continuous function of the pose.
            const float step = 0.005f * height;
            float previous = hit.clearance;
            float travel = -1.0f;
            for (float t = step; t <= maxMove + 1.0e-6f; t += step) {
                place(t);
                const Hit h = evaluate();
                if (h.clearance >= 0.0f) {
                    travel = (t - step) + step * (-previous) / std::max(1.0e-6f, h.clearance - previous);
                    break;
                }
                previous = h.clearance;
            }
            if (travel < 0.0f) {
                travel = maxMove;  // cannot clear within the bound: bounded fallback
            }
            place(travel);
            stats.legCorrection = std::max(stats.legCorrection,
                                           std::max((targets[0] - original[0]).length(), (targets[1] - original[1]).length()));
            hit = evaluate();
        }
    }
    stats.legClearanceMin = hit.clearance;
}

motion::LegGeometry HumanoidRetargeter::legGeometry() const {
    motion::LegGeometry g;
    if (!impl_ || !impl_->valid || impl_->legs[0].upper < 0 || impl_->legs[1].upper < 0 ||
        impl_->height <= 0.0f) {
        return g;
    }
    const Impl& m = *impl_;
    const float k = 160.0f / m.height;
    const Vec3 up = m.model->up;
    const Vec3 left = -m.model->right;
    const Vec3 hipL = m.legs[0].bindUpper, hipR = m.legs[1].bindUpper;
    const Vec3 ankL = m.legs[0].bindAnkle, ankR = m.legs[1].bindAnkle;
    g.legLen = 0.5f * (m.legs[0].upperLength + m.legs[0].lowerLength + m.legs[1].upperLength + m.legs[1].lowerLength) * k;
    g.hipHalf = std::clamp(0.5f * std::abs(math::dot(hipL - hipR, left)) * k, 4.0f, 14.0f);
    g.ankleHalf = std::clamp(0.5f * std::abs(math::dot(ankL - ankR, left)) * k, 4.0f, 16.0f);
    g.hipY = std::clamp(0.5f * (math::dot(hipL, up) + math::dot(hipR, up)) * k, 60.0f, 110.0f);
    g.ankleY = std::clamp(0.5f * (math::dot(ankL, up) + math::dot(ankR, up)) * k, 3.0f, 12.0f);
    if (m.proxy.thighRadius > 0.0f) {
        g.thighR = m.proxy.thighRadius * k;
        g.kneeR = std::max(m.proxy.shinRadius, 0.6f * m.proxy.thighRadius) * k;
        g.ankleR = 0.8f * g.kneeR;
        if (m.proxy.footRadius > 0.0f) {
            g.footR = m.proxy.footRadius * k;
        }
        if (m.proxy.footLength > 0.0f) {
            g.footLen = m.proxy.footLength * k;
        }
    }
    return g;
}

CollisionProxyInfo HumanoidRetargeter::collisionProxy() const {
    return impl_ ? impl_->proxy : CollisionProxyInfo{};
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
                                        const TwoBoneLimits& limits, bool planted,
                                        RetargetStats& stats,
                                        const Quat& footWorldDelta) const {
    reachableTarget = target;
    if (leg.upper < 0 || leg.lower < 0 || leg.foot < 0 || leg.parent < 0) {
        return;
    }
    const Vec3 hip = actualPosition(globals, leg.upper);
    // The one authoritative solve of this leg. The solved end is the closest point the
    // chain can reach: distance from the requested target to it is the reach deficit
    // (including any soft-reach shortfall); the rest of the error is solver error.
    const TwoBoneSolution solution = solveTwoBone(hip, target, pole, leg.chain, limits);
    if (limits.stableBend) {
        noteSolve(stats, solution, planted);
    }
    reachableTarget = solution.end;
    footError = std::max(footError, std::abs(solution.requestedDistance - solution.solvedDistance));
    const Vec3 knee = solution.knee;
    const Vec3 ankle = solution.end;

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

    out.stats.softIK = options.softIK;
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
        // Legacy pose path: a foot on the floor (no lift) counts as planted.
        const float freeLeft = freeWeightOf(1.0f - std::max(0.0f, safeClamp(pose.footLeft, 0.0f, 1.0f)));
        const float freeRight = freeWeightOf(1.0f - std::max(0.0f, safeClamp(pose.footRight, 0.0f, 1.0f)));
        const float legLength[2] = {impl_->legs[0].upperLength + impl_->legs[0].lowerLength,
                                    impl_->legs[1].upperLength + impl_->legs[1].lowerLength};
        const float freeWeight[2] = {freeLeft, freeRight};
        for (int f = 0; f < 2; ++f) {
            const TwoBoneLimits limits = limbLimits(options.softIK, legLength[f],
                                                    options.legSoftReach, options.kneeFlexMin,
                                                    options.kneeFlexMax, freeWeight[f]);
            impl_->solveLeg(impl_->legs[f], globals, locals, f == 0 ? targetLeft : targetRight,
                            pole, footError, lowest, reachable[f], limits, freeWeight[f] <= 0.0f,
                            out.stats);
        }
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
    out.stats.softIK = options.softIK;

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

    // Finger curl: 0 = open hand, 1 = fist. A relaxed hand is not a flat blade. Every hand
    // takes the detailed per-finger path (BG Motion 3.3, A7). A clip without detailed hands
    // is posed with the relaxed pose of its scalar curl, so turning detailedHands on no
    // longer pops the fingers from the v1 look.
    {
        static constexpr float kFlex[5][3] = {{22.0f, 30.0f, 20.0f}, {38.0f, 52.0f, 34.0f},
                                              {40.0f, 56.0f, 36.0f}, {38.0f, 54.0f, 34.0f},
                                              {36.0f, 50.0f, 32.0f}};
        for (int sd = 0; sd < 2; ++sd) {
            const motion::HandPose handPose =
                pose.detailedHands ? pose.handPose[static_cast<std::size_t>(sd)]
                                   : motion::relaxedHandPose(
                                         pose.hand[static_cast<std::size_t>(sd)]);
            if (pose.detailedHands) {
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
            for (int fg = 0; fg < 5; ++fg) {
                for (int jt = 0; jt < 3; ++jt) {
                    const int node = impl_->fingers[sd][fg][jt];
                    if (node < 0) {
                        continue;
                    }
                    float amount = handPose.curl[static_cast<std::size_t>(fg)]
                                                [static_cast<std::size_t>(jt)];
                    if (fg == 0) {
                        amount = std::min(1.0f, amount + 0.22f * handPose.thumbOpposition);
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
                if (fg > 0) {
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
            if (handPose.thumbOpposition > 0.0f) {
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

    // Collision-aware refinement of the finished arm pose. It only touches arm
    // bones, so it cannot disturb the stage-space foot targets solved below.
    if (options.armCollision) {
        impl_->refineArms(locals, globals, options, out.stats);
    }

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

    // Limits per leg: a planted foot (contact >= 0.5) keeps the hard reach and is never
    // softened; a free foot takes the soft band and the flexion window.
    const float contacts[2] = {pose.foot[0].contact, pose.foot[1].contact};
    TwoBoneLimits legLimits[2];
    for (int f = 0; f < 2; ++f) {
        const LegRig& leg = impl_->legs[f];
        legLimits[f] = limbLimits(options.softIK, leg.upperLength + leg.lowerLength,
                                  options.legSoftReach, options.kneeFlexMin, options.kneeFlexMax,
                                  freeWeightOf(contacts[f]));
    }

    if (options.footIK && options.legCollision) {
        // With soft IK the shin and foot capsules end at the solved ankle, where the solve
        // really places it. Without it they keep BG Motion 3.2's requested-target geometry.
        impl_->refineLegs(globals, targets, poles, contacts, legLimits, options.softIK,
                          options.legClearanceMargin, out.stats);
    }

    float footError = 0.0f;
    float lowest = std::numeric_limits<float>::max();
    Vec3 reachable[2] = {targets[0], targets[1]};
    if (options.footIK) {
        for (int f = 0; f < 2; ++f) {
            impl_->solveLeg(impl_->legs[f], globals, locals, targets[f], poles[f],
                            footError, lowest, reachable[f], legLimits[f],
                            freeWeightOf(contacts[f]) <= 0.0f, out.stats, footDelta[f]);
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
