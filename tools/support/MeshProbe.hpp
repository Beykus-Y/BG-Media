#pragma once

// Offline mesh-level probe shared by the QA tools: CPU skinning of every vertex of
// a VRM with a retargeted pose, region classification by dominant joint, and
// triangle-vs-triangle crossing counts between two regions (e.g. left leg vs
// right leg, forearm vs torso). Offline QA only: it is deliberately not a runtime
// collision system.
//
// Regions are decided per vertex by the dominant joint. A triangle belongs to a
// region when all three vertices do. Crossings that already exist in the neutral
// pose (touching thighs, shoulder seams) can be masked with maskCurrentAsRest().

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bgmedia::probe {

using math::Mat4;
using math::Vec3;

enum class Region : std::uint8_t { None, ArmL, ArmR, UpperArmL, UpperArmR, LegL, LegR, Torso, Hair };

class MeshProbe {
public:
    explicit MeshProbe(const vrm::VrmModel& model) : model_(model) {
        std::vector<Region> nodeRegion(model.nodes.size(), Region::None);
        const auto mark = [&](vrm::HumanoidBone b, Region r) {
            const int n = model.nodeFor(b);
            if (n >= 0) nodeRegion[static_cast<std::size_t>(n)] = r;
        };
        using B = vrm::HumanoidBone;
        mark(B::LeftUpperArm, Region::UpperArmL);
        mark(B::RightUpperArm, Region::UpperArmR);
        for (B b : {B::LeftLowerArm, B::LeftHand}) mark(b, Region::ArmL);
        for (B b : {B::RightLowerArm, B::RightHand}) mark(b, Region::ArmR);
        for (int sd = 0; sd < 2; ++sd)
            for (int fg = 0; fg < 5; ++fg)
                for (int jt = 0; jt < 3; ++jt) {
                    const int n = model.fingerNode(sd, fg, jt);
                    if (n >= 0) nodeRegion[static_cast<std::size_t>(n)] = sd == 0 ? Region::ArmL : Region::ArmR;
                }
        for (B b : {B::LeftUpperLeg, B::LeftLowerLeg, B::LeftFoot, B::LeftToes}) mark(b, Region::LegL);
        for (B b : {B::RightUpperLeg, B::RightLowerLeg, B::RightFoot, B::RightToes}) mark(b, Region::LegR);
        for (B b : {B::Spine, B::Chest, B::UpperChest}) mark(b, Region::Torso);

        for (const vrm::MeshInstance& inst : model.meshInstances) {
            if (inst.mesh < 0) continue;
            const vrm::Skin* skin = model.skinForNode(inst.node);
            if (!skin) continue;
            const int skinIndex = static_cast<int>(skin - model.skins.data());
            for (const vrm::Primitive& prim : model.meshes[static_cast<std::size_t>(inst.mesh)].primitives) {
                if (!prim.skinned) continue;
                const bool hair = prim.material >= 0 &&
                                  model.materials[static_cast<std::size_t>(prim.material)].name.find("HAIR") != std::string::npos;
                const std::size_t base = verts_.size();
                for (const vrm::Vertex& v : prim.vertices) {
                    Vertex s;
                    s.position = v.position;
                    s.joints = v.joints;
                    s.weights = v.weights;
                    s.skin = skinIndex;
                    std::size_t best = 0;
                    for (std::size_t k = 1; k < 4; ++k)
                        if (v.weights[k] > v.weights[best]) best = k;
                    if (hair) {
                        s.region = Region::Hair;
                    } else if (v.weights[best] >= 0.5f && v.joints[best] < skin->joints.size()) {
                        s.region = nodeRegion[static_cast<std::size_t>(skin->joints[v.joints[best]])];
                    }
                    verts_.push_back(s);
                }
                for (std::size_t t = 0; t + 2 < prim.indices.size(); t += 3) {
                    tris_.push_back({static_cast<int>(base + prim.indices[t]), static_cast<int>(base + prim.indices[t + 1]),
                                     static_cast<int>(base + prim.indices[t + 2])});
                }
            }
        }
        triRegion_.resize(tris_.size(), Region::None);
        for (std::size_t t = 0; t < tris_.size(); ++t) {
            const Region r0 = verts_[static_cast<std::size_t>(tris_[t][0])].region;
            if (r0 == verts_[static_cast<std::size_t>(tris_[t][1])].region &&
                r0 == verts_[static_cast<std::size_t>(tris_[t][2])].region) {
                triRegion_[t] = r0;
            }
        }
        skinned_.resize(verts_.size());
    }

    [[nodiscard]] std::size_t vertexCount() const { return verts_.size(); }
    [[nodiscard]] std::size_t triangleCount(Region r) const {
        return static_cast<std::size_t>(std::count(triRegion_.begin(), triRegion_.end(), r));
    }

    // Bind-pose skinning of the vertices with the pose's joint globals.
    void skin(const animation::RetargetedPose& pose) {
        palettes_.resize(model_.skins.size());
        for (std::size_t s = 0; s < model_.skins.size(); ++s) {
            const vrm::Skin& sk = model_.skins[s];
            palettes_[s].resize(sk.joints.size());
            for (std::size_t j = 0; j < sk.joints.size(); ++j) {
                palettes_[s][j] = pose.nodeGlobals[static_cast<std::size_t>(sk.joints[j])] * sk.inverseBindMatrices[j];
            }
        }
        for (std::size_t i = 0; i < verts_.size(); ++i) {
            const Vertex& v = verts_[i];
            Vec3 p{};
            for (int k = 0; k < 4; ++k) {
                if (v.weights[static_cast<std::size_t>(k)] <= 0.0f) continue;
                p = p + palettes_[static_cast<std::size_t>(v.skin)][v.joints[static_cast<std::size_t>(k)]].transformPoint(v.position) *
                            v.weights[static_cast<std::size_t>(k)];
            }
            skinned_[i] = p;
        }
    }

    [[nodiscard]] Vec3 triangleCentroid(int t) const {
        const auto& tr = tris_[static_cast<std::size_t>(t)];
        return (skinned_[static_cast<std::size_t>(tr[0])] + skinned_[static_cast<std::size_t>(tr[1])] +
                skinned_[static_cast<std::size_t>(tr[2])]) * (1.0f / 3.0f);
    }

    // Triangles of region `a` that cross a triangle of region `b` (skin() first).
    // Triangles flagged by maskCurrentAsRest() are not reported.
    std::vector<int> crossings(Region a, Region b) {
        constexpr float cell = 0.03f;
        std::unordered_map<long long, std::vector<int>> grid;
        for (std::size_t t = 0; t < tris_.size(); ++t) {
            if (triRegion_[t] != b) continue;
            Vec3 lo, hi;
            bounds(static_cast<int>(t), lo, hi);
            for (int x = cellOf(lo.x, cell); x <= cellOf(hi.x, cell); ++x)
                for (int y = cellOf(lo.y, cell); y <= cellOf(hi.y, cell); ++y)
                    for (int z = cellOf(lo.z, cell); z <= cellOf(hi.z, cell); ++z)
                        grid[key(x, y, z)].push_back(static_cast<int>(t));
        }
        std::vector<int> hits;
        std::vector<int> stamp(tris_.size(), -1);
        for (std::size_t t = 0; t < tris_.size(); ++t) {
            if (triRegion_[t] != a || (t < rest_.size() && rest_[t])) continue;
            Vec3 lo, hi;
            bounds(static_cast<int>(t), lo, hi);
            bool hit = false;
            for (int x = cellOf(lo.x, cell); x <= cellOf(hi.x, cell) && !hit; ++x)
                for (int y = cellOf(lo.y, cell); y <= cellOf(hi.y, cell) && !hit; ++y)
                    for (int z = cellOf(lo.z, cell); z <= cellOf(hi.z, cell) && !hit; ++z) {
                        const auto it = grid.find(key(x, y, z));
                        if (it == grid.end()) continue;
                        for (int o : it->second) {
                            if (stamp[static_cast<std::size_t>(o)] == static_cast<int>(t)) continue;
                            stamp[static_cast<std::size_t>(o)] = static_cast<int>(t);
                            if (triTri(static_cast<int>(t), o)) {
                                hit = true;
                                break;
                            }
                        }
                    }
            if (hit) hits.push_back(static_cast<int>(t));
        }
        return hits;
    }

    // Treat the crossings of the last skin() pose (usually the neutral pose) as rest contact.
    void maskCurrentAsRest(Region a, Region b) {
        rest_.resize(tris_.size(), 0);
        const std::vector<int> saved;  // (rest_ is cleared for the pair being masked)
        for (int t : crossings(a, b)) rest_[static_cast<std::size_t>(t)] = 1;
    }

private:
    struct Vertex {
        Vec3 position;
        std::array<std::uint16_t, 4> joints{};
        std::array<float, 4> weights{};
        int skin = -1;
        Region region = Region::None;
    };
    static int cellOf(float v, float cell) { return static_cast<int>(std::floor(v / cell)); }
    static long long key(int x, int y, int z) {
        return (static_cast<long long>(x) * 73856093LL) ^ (static_cast<long long>(y) * 19349663LL) ^
               (static_cast<long long>(z) * 83492791LL);
    }
    void bounds(int t, Vec3& lo, Vec3& hi) const {
        const auto& tr = tris_[static_cast<std::size_t>(t)];
        lo = hi = skinned_[static_cast<std::size_t>(tr[0])];
        for (int k = 1; k < 3; ++k) {
            const Vec3 p = skinned_[static_cast<std::size_t>(tr[static_cast<std::size_t>(k)])];
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    static bool segTri(Vec3 p, Vec3 q, Vec3 a, Vec3 b, Vec3 c) {
        const Vec3 e1 = b - a, e2 = c - a, d = q - p;
        const Vec3 h = math::cross(d, e2);
        const float det = math::dot(e1, h);
        if (std::abs(det) < 1e-12f) return false;
        const float f = 1.0f / det;
        const Vec3 s = p - a;
        const float u = f * math::dot(s, h);
        if (u < 0.0f || u > 1.0f) return false;
        const Vec3 qv = math::cross(s, e1);
        const float v = f * math::dot(d, qv);
        if (v < 0.0f || u + v > 1.0f) return false;
        const float t = f * math::dot(e2, qv);
        return t >= 0.0f && t <= 1.0f;
    }
    bool triTri(int ia, int ib) const {
        const auto& A = tris_[static_cast<std::size_t>(ia)];
        const auto& B = tris_[static_cast<std::size_t>(ib)];
        const Vec3 a[3] = {skinned_[static_cast<std::size_t>(A[0])], skinned_[static_cast<std::size_t>(A[1])], skinned_[static_cast<std::size_t>(A[2])]};
        const Vec3 b[3] = {skinned_[static_cast<std::size_t>(B[0])], skinned_[static_cast<std::size_t>(B[1])], skinned_[static_cast<std::size_t>(B[2])]};
        for (int e = 0; e < 3; ++e) {
            if (segTri(a[e], a[(e + 1) % 3], b[0], b[1], b[2])) return true;
            if (segTri(b[e], b[(e + 1) % 3], a[0], a[1], a[2])) return true;
        }
        return false;
    }

    const vrm::VrmModel& model_;
    std::vector<Vertex> verts_;
    std::vector<std::array<int, 3>> tris_;
    std::vector<Region> triRegion_;
    std::vector<Vec3> skinned_;
    std::vector<std::vector<Mat4>> palettes_;
    std::vector<unsigned char> rest_;
};

}  // namespace bgmedia::probe
