#pragma once

// bgmedia_mesh_clip --legs: thigh-against-thigh triangle contact (offline QA only).
//
// Classification: a skinned triangle is LEFT_THIGH (RIGHT_THIGH) when all three of its
// vertices have a dominant joint (weight >= 0.5) that is the humanoid leftUpperLeg
// (rightUpperLeg) node. Only that joint counts. Lower legs, feet and non-humanoid child bones
// (skirt, stocking or spring bones) are not thighs, so their triangles are not measured.
//
// Contact: every left-thigh triangle is tested against the right-thigh triangles it can share
// a point with. The broadphase is a uniform grid over triangle AABBs (4 cm cells, at most 256
// cells per triangle, larger triangles are kept in a brute-force list) and holds only indices,
// so memory is bounded by the triangle count. Pairs that share a vertex are topological
// neighbours and are skipped. Pairs that already touch in the neutral (arms-out) pose are
// masked for the whole run and counted separately. A crossing is an unmasked contact whose
// approximate depth (tools/support/TriangleTest.hpp) exceeds --crossing-mm.
//
// Per frame the row holds the crossing count, max and mean depth, the deepest pairs (top 5,
// with their triangle identities, the mesh, primitive and material they come from, and the
// midpoint of their centroids), the knee positions, knee flexion, the bend-plane angle, the
// pelvis height and yaw, and the foot contact weight and phase from the DancePlayer frame.
// The fitted thigh proxy radius of the retargeter is reported once, in the header.
//
// Offline measurement only: it is not a runtime collision system, and it does not replace the
// visual check.

#include "support/TriangleTest.hpp"

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bgmedia::meshclip {

using math::Mat4;
using math::Vec3;

inline constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
inline constexpr float kGridCell = 0.04f;        // metres
inline constexpr int kMaxCellsPerTriangle = 256;  // bounds the grid entries of one triangle
inline constexpr int kTopPairs = 5;               // deepest pairs kept per frame

struct LegOptions {
    std::string modelLabel;
    std::vector<std::string> clips;
    double seconds = 40.0;
    double hz = 60.0;
    bool overlay = true;
    bool rawFootwork = false;  // true: authored foot tracks as stored (no footwork pre-pass)
    float crossingM = 0.0005f;  // minimum depth (metres) for a pair to count as a crossing
    int top = 10;               // worst frames listed per clip
    std::filesystem::path dances;
    std::filesystem::path json;  // empty: no JSON output
};

struct LegVertex {
    Vec3 position;
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{};
    int skin = -1;
};

struct LegTriangle {
    std::array<int, 3> vertex{};  // indices into the compact leg vertex array
    int mesh = -1;                // index into model.meshes
    int primitive = -1;           // index into that mesh's primitives
    int primTriangle = -1;        // triangle number inside the primitive's index buffer
    int material = -1;
};

struct LegSet {
    std::vector<LegVertex> verts;
    std::vector<LegTriangle> tris[2];  // [0] left thigh, [1] right thigh
};

struct PairHit {
    int left = -1;
    int right = -1;
    float depth = 0.0f;
    Vec3 at{};
};

struct FrameRow {
    int frame = 0;
    double t = 0.0;
    int crossings = 0;
    int masked = 0;
    float maxDepth = 0.0f;
    double depthSum = 0.0;
    std::vector<PairHit> pairs;  // deepest first, at most kTopPairs
    Vec3 knee[2]{};
    float flexion[2] = {kNaN, kNaN};
    float bendPlane[2] = {kNaN, kNaN};
    float pelvisHeight = kNaN;     // metres, hips above the lower ankle joint
    float pelvisHeightRel = kNaN;  // ... relative to the model height
    float pelvisYaw = kNaN;        // degrees, hip line relative to the neutral pose
    float rootYaw = kNaN;          // degrees, CharacterPose root yaw
    float contact[2] = {0.0f, 0.0f};
    const char* phase[2] = {"", ""};
};

namespace detail {

// Dominant joint of a vertex as a model node index, or -1 when no joint has weight >= 0.5.
inline int dominantNode(const vrm::Vertex& v, const vrm::Skin& skin) {
    std::size_t best = 0;
    for (std::size_t k = 1; k < 4; ++k)
        if (v.weights[k] > v.weights[best]) best = k;
    if (v.weights[best] < 0.5f || v.joints[best] >= skin.joints.size()) return -1;
    return static_cast<int>(skin.joints[v.joints[best]]);
}

inline LegSet classifyLegTriangles(const vrm::VrmModel& model) {
    LegSet out;
    const int leftUpper = model.nodeFor(vrm::HumanoidBone::LeftUpperLeg);
    const int rightUpper = model.nodeFor(vrm::HumanoidBone::RightUpperLeg);
    if (leftUpper < 0 || rightUpper < 0) return out;
    for (const vrm::MeshInstance& inst : model.meshInstances) {
        if (inst.mesh < 0) continue;
        const vrm::Skin* skin = model.skinForNode(inst.node);
        if (!skin) continue;
        const int skinIndex = static_cast<int>(skin - model.skins.data());
        const vrm::Mesh& mesh = model.meshes[static_cast<std::size_t>(inst.mesh)];
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const vrm::Primitive& prim = mesh.primitives[p];
            if (!prim.skinned) continue;
            std::vector<int> dominant(prim.vertices.size());
            std::vector<int> remap(prim.vertices.size(), -1);
            for (std::size_t i = 0; i < prim.vertices.size(); ++i) dominant[i] = dominantNode(prim.vertices[i], *skin);
            for (std::size_t t = 0; t + 2 < prim.indices.size(); t += 3) {
                const std::array<std::size_t, 3> idx = {prim.indices[t], prim.indices[t + 1], prim.indices[t + 2]};
                if (idx[0] >= prim.vertices.size() || idx[1] >= prim.vertices.size() || idx[2] >= prim.vertices.size())
                    continue;
                const int node = dominant[idx[0]];
                int side = -1;
                if (node == leftUpper) side = 0;
                else if (node == rightUpper) side = 1;
                if (side < 0 || dominant[idx[1]] != node || dominant[idx[2]] != node) continue;

                LegTriangle tri;
                tri.mesh = inst.mesh;
                tri.primitive = static_cast<int>(p);
                tri.primTriangle = static_cast<int>(t / 3);
                tri.material = prim.material;
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::size_t vi = idx[k];
                    if (remap[vi] < 0) {
                        remap[vi] = static_cast<int>(out.verts.size());
                        LegVertex lv;
                        lv.position = prim.vertices[vi].position;
                        lv.joints = prim.vertices[vi].joints;
                        lv.weights = prim.vertices[vi].weights;
                        lv.skin = skinIndex;
                        out.verts.push_back(lv);
                    }
                    tri.vertex[k] = remap[vi];
                }
                out.tris[side].push_back(tri);
            }
        }
    }
    return out;
}

// CPU skinning of the leg vertices with the pose's node globals (same convention as the arm test).
inline void skinLegVertices(const vrm::VrmModel& model, const animation::RetargetedPose& pose,
                            const std::vector<LegVertex>& verts, std::vector<std::vector<Mat4>>& palette,
                            std::vector<Vec3>& out) {
    palette.resize(model.skins.size());
    for (std::size_t s = 0; s < model.skins.size(); ++s) {
        const vrm::Skin& skin = model.skins[s];
        palette[s].resize(skin.joints.size());
        for (std::size_t j = 0; j < skin.joints.size(); ++j)
            palette[s][j] = pose.nodeGlobals[static_cast<std::size_t>(skin.joints[j])] * skin.inverseBindMatrices[j];
    }
    out.resize(verts.size());
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const LegVertex& v = verts[i];
        Vec3 p{};
        for (std::size_t k = 0; k < 4; ++k) {
            if (v.weights[k] <= 0.0f) continue;
            const auto& pal = palette[static_cast<std::size_t>(v.skin)];
            if (v.joints[k] >= pal.size()) continue;
            p = p + pal[v.joints[k]].transformPoint(v.position) * v.weights[k];
        }
        out[i] = p;
    }
}

struct Box {
    Vec3 lo;
    Vec3 hi;
};

inline Box triangleBox(const Vec3* p) {
    Box b{p[0], p[0]};
    for (int k = 1; k < 3; ++k) {
        b.lo = {std::min(b.lo.x, p[k].x), std::min(b.lo.y, p[k].y), std::min(b.lo.z, p[k].z)};
        b.hi = {std::max(b.hi.x, p[k].x), std::max(b.hi.y, p[k].y), std::max(b.hi.z, p[k].z)};
    }
    // Pad by the contact tolerance, so pairs that touch within it across a cell
    // boundary still reach the narrow phase.
    const Vec3 pad{probe::kTriangleEpsilon, probe::kTriangleEpsilon, probe::kTriangleEpsilon};
    b.lo = b.lo - pad;
    b.hi = b.hi + pad;
    return b;
}

struct CellRange {
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;
};

inline int cellIndex(float v) { return static_cast<int>(std::floor(v / kGridCell)); }

// False when the box covers more cells than the per-triangle bound (or is not finite).
inline bool cellRangeOf(const Box& box, CellRange& r) {
    if (!std::isfinite(box.lo.x) || !std::isfinite(box.hi.x) || !std::isfinite(box.lo.y) ||
        !std::isfinite(box.hi.y) || !std::isfinite(box.lo.z) || !std::isfinite(box.hi.z))
        return false;
    r.x0 = cellIndex(box.lo.x);
    r.x1 = cellIndex(box.hi.x);
    r.y0 = cellIndex(box.lo.y);
    r.y1 = cellIndex(box.hi.y);
    r.z0 = cellIndex(box.lo.z);
    r.z1 = cellIndex(box.hi.z);
    const long long count = static_cast<long long>(r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1) * (r.z1 - r.z0 + 1);
    return count <= kMaxCellsPerTriangle;
}

inline long long cellKey(int x, int y, int z) {
    return (static_cast<long long>(x) * 73856093LL) ^ (static_cast<long long>(y) * 19349663LL) ^
           (static_cast<long long>(z) * 83492791LL);
}

// Uniform grid over the AABBs of one triangle set. Entries are (cell key, triangle index), sorted
// by key. Key collisions only add candidates; every candidate is still tested exactly.
struct Grid {
    std::vector<std::pair<long long, int>> entries;
    std::vector<int> large;  // triangles whose AABB spans too many cells
    void build(const std::vector<Vec3>& pos, const std::vector<LegTriangle>& tris) {
        entries.clear();
        large.clear();
        for (std::size_t i = 0; i < tris.size(); ++i) {
            const Vec3 p[3] = {pos[static_cast<std::size_t>(tris[i].vertex[0])],
                               pos[static_cast<std::size_t>(tris[i].vertex[1])],
                               pos[static_cast<std::size_t>(tris[i].vertex[2])]};
            CellRange r;
            if (!cellRangeOf(triangleBox(p), r)) {
                large.push_back(static_cast<int>(i));
                continue;
            }
            for (int x = r.x0; x <= r.x1; ++x)
                for (int y = r.y0; y <= r.y1; ++y)
                    for (int z = r.z0; z <= r.z1; ++z)
                        entries.emplace_back(cellKey(x, y, z), static_cast<int>(i));
        }
        std::sort(entries.begin(), entries.end());
    }
};

inline void insertTop(std::vector<PairHit>& top, const PairHit& hit) {
    if (top.size() == static_cast<std::size_t>(kTopPairs)) {
        if (!(hit.depth > top.back().depth)) return;
        top.pop_back();
    }
    top.push_back(hit);
    std::stable_sort(top.begin(), top.end(), [](const PairHit& a, const PairHit& b) { return a.depth > b.depth; });
}

struct ScanResult {
    int crossings = 0;
    int masked = 0;
    double depthSum = 0.0;
    float maxDepth = 0.0f;
    std::vector<PairHit> top;
};

inline std::uint64_t pairKey(int left, int right) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(left)) << 32) |
           static_cast<std::uint32_t>(right);
}

inline bool sharesVertex(const LegTriangle& a, const LegTriangle& b) {
    for (int x : a.vertex)
        for (int y : b.vertex)
            if (x == y) return true;
    return false;
}

// Tests every left-thigh triangle against the right-thigh triangles of the grid. In the rest pass
// every touching pair goes to `rest`; otherwise touching pairs are either masked or counted.
inline void scanPairs(const LegSet& legs, const std::vector<Vec3>& pos, const Grid& grid,
                      const std::unordered_set<std::uint64_t>& rest, float crossingM, bool restPass,
                      std::unordered_set<std::uint64_t>* restOut, ScanResult& scan) {
    const std::vector<LegTriangle>& left = legs.tris[0];
    const std::vector<LegTriangle>& right = legs.tris[1];
    std::vector<int> stamp(right.size(), -1);
    for (std::size_t l = 0; l < left.size(); ++l) {
        const LegTriangle& a = left[l];
        const Vec3 pa[3] = {pos[static_cast<std::size_t>(a.vertex[0])], pos[static_cast<std::size_t>(a.vertex[1])],
                            pos[static_cast<std::size_t>(a.vertex[2])]};
        const auto consider = [&](int r) {
            if (stamp[static_cast<std::size_t>(r)] == static_cast<int>(l)) return;
            stamp[static_cast<std::size_t>(r)] = static_cast<int>(l);
            const LegTriangle& b = right[static_cast<std::size_t>(r)];
            if (sharesVertex(a, b)) return;
            const Vec3 pb[3] = {pos[static_cast<std::size_t>(b.vertex[0])], pos[static_cast<std::size_t>(b.vertex[1])],
                                pos[static_cast<std::size_t>(b.vertex[2])]};
            const probe::TriangleContact c = probe::triangleContact(pa, pb);
            if (!c.intersects) return;
            const std::uint64_t key = pairKey(static_cast<int>(l), r);
            if (restPass) {
                restOut->insert(key);
                return;
            }
            if (rest.count(key)) {
                ++scan.masked;
                return;
            }
            if (c.depth <= crossingM) return;
            ++scan.crossings;
            scan.depthSum += c.depth;
            scan.maxDepth = std::max(scan.maxDepth, c.depth);
            const Vec3 ca = (pa[0] + pa[1] + pa[2]) * (1.0f / 3.0f);
            const Vec3 cb = (pb[0] + pb[1] + pb[2]) * (1.0f / 3.0f);
            insertTop(scan.top, PairHit{static_cast<int>(l), r, c.depth, (ca + cb) * 0.5f});
        };
        CellRange range;
        if (!cellRangeOf(triangleBox(pa), range)) {
            for (std::size_t r = 0; r < right.size(); ++r) consider(static_cast<int>(r));
            continue;
        }
        for (int x = range.x0; x <= range.x1; ++x)
            for (int y = range.y0; y <= range.y1; ++y)
                for (int z = range.z0; z <= range.z1; ++z) {
                    const long long key = cellKey(x, y, z);
                    auto it = std::lower_bound(grid.entries.begin(), grid.entries.end(),
                                               std::make_pair(key, static_cast<int>(INT_MIN)));
                    for (; it != grid.entries.end() && it->first == key; ++it) consider(it->second);
                }
        for (int r : grid.large) consider(r);
    }
}

inline float degrees(float radians) { return radians * 180.0f / math::kPi; }

// Knee flexion in degrees: 0 for a straight leg, growing as the knee bends.
inline float kneeFlexion(Vec3 hip, Vec3 knee, Vec3 ankle) {
    const Vec3 u = hip - knee;
    const Vec3 w = ankle - knee;
    const float lu = u.length();
    const float lw = w.length();
    if (!(lu > 1e-6f) || !(lw > 1e-6f)) return kNaN;
    const float c = std::clamp(math::dot(u, w) / (lu * lw), -1.0f, 1.0f);
    return 180.0f - degrees(std::acos(c));
}

// Angle in [0, 90] degrees between the hip-knee-ankle plane normal and the model's lateral axis:
// 0 when the knee bends in the sagittal plane, 90 when it bends purely sideways. NaN for a
// straight leg (no plane).
inline float bendPlane(Vec3 hip, Vec3 knee, Vec3 ankle, Vec3 lateral, float flexionDeg) {
    if (!(flexionDeg > 5.0f)) return kNaN;
    const Vec3 n = math::cross(knee - hip, ankle - knee);
    const float len = n.length();
    if (!(len > 1e-9f)) return kNaN;
    const float c = std::clamp(std::fabs(math::dot(n, lateral)) / len, 0.0f, 1.0f);
    return degrees(std::acos(c));
}

// Horizontal direction of the hip line (left to right hip joint), relative to the neutral pose.
inline Vec3 hipLine(const animation::RetargetedPose& pose, const vrm::VrmModel& model, Vec3 up) {
    const Vec3 l = pose.bonePosition(vrm::HumanoidBone::LeftUpperLeg, model) -
                   pose.bonePosition(vrm::HumanoidBone::RightUpperLeg, model);
    return l - up * math::dot(l, up);
}

// Signed angle in degrees from `rest` to `cur` about `up` (0 when they match).
inline float signedYaw(Vec3 rest, Vec3 cur, Vec3 up) {
    const Vec3 a = math::normalize(rest);
    const Vec3 b = math::normalize(cur);
    if (a.lengthSquared() < 0.5f || b.lengthSquared() < 0.5f) return kNaN;
    return degrees(std::atan2(math::dot(math::cross(a, b), up), math::dot(a, b)));
}

} // namespace detail

// --- JSON (minimal writer: the rows are flat numbers, strings and arrays) -------------------

inline void jsonString(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

inline void jsonNumber(std::string& out, double v, int decimals) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    out += buf;
}

inline void jsonVec(std::string& out, Vec3 v) {
    out += '[';
    jsonNumber(out, v.x, 4);
    out += ',';
    jsonNumber(out, v.y, 4);
    out += ',';
    jsonNumber(out, v.z, 4);
    out += ']';
}

inline void jsonPair(std::string& out, const PairHit& hit, const LegSet& legs, const vrm::VrmModel& model) {
    const LegTriangle& a = legs.tris[0][static_cast<std::size_t>(hit.left)];
    const LegTriangle& b = legs.tris[1][static_cast<std::size_t>(hit.right)];
    const auto material = [&model](int m) {
        return m >= 0 ? model.materials[static_cast<std::size_t>(m)].name : std::string();
    };
    out += "{\"leftLegTri\":" + std::to_string(hit.left) + ",\"rightLegTri\":" + std::to_string(hit.right);
    out += ",\"leftMesh\":" + std::to_string(a.mesh) + ",\"leftPrimitive\":" + std::to_string(a.primitive);
    out += ",\"leftPrimTriangle\":" + std::to_string(a.primTriangle) + ",\"leftMaterial\":";
    jsonString(out, material(a.material));
    out += ",\"rightMesh\":" + std::to_string(b.mesh) + ",\"rightPrimitive\":" + std::to_string(b.primitive);
    out += ",\"rightPrimTriangle\":" + std::to_string(b.primTriangle) + ",\"rightMaterial\":";
    jsonString(out, material(b.material));
    out += ",\"depthMm\":";
    jsonNumber(out, hit.depth * 1000.0, 3);
    out += ",\"at\":";
    jsonVec(out, hit.at);
    out += '}';
}

inline void jsonRow(std::string& out, const FrameRow& row, const LegSet& legs, const vrm::VrmModel& model) {
    out += "{\"frame\":" + std::to_string(row.frame) + ",\"t\":";
    jsonNumber(out, row.t, 4);
    out += ",\"crossings\":" + std::to_string(row.crossings) + ",\"masked\":" + std::to_string(row.masked);
    out += ",\"maxDepthMm\":";
    jsonNumber(out, row.maxDepth * 1000.0, 3);
    out += ",\"meanDepthMm\":";
    jsonNumber(out, row.crossings > 0 ? row.depthSum / row.crossings * 1000.0 : kNaN, 3);
    out += ",\"pairs\":[";
    for (std::size_t i = 0; i < row.pairs.size(); ++i) {
        if (i) out += ',';
        jsonPair(out, row.pairs[i], legs, model);
    }
    out += "],\"knees\":{\"left\":";
    jsonVec(out, row.knee[0]);
    out += ",\"right\":";
    jsonVec(out, row.knee[1]);
    out += "},\"kneeFlexionDeg\":{\"left\":";
    jsonNumber(out, row.flexion[0], 2);
    out += ",\"right\":";
    jsonNumber(out, row.flexion[1], 2);
    out += "},\"bendPlaneDeg\":{\"left\":";
    jsonNumber(out, row.bendPlane[0], 2);
    out += ",\"right\":";
    jsonNumber(out, row.bendPlane[1], 2);
    out += "},\"pelvis\":{\"heightM\":";
    jsonNumber(out, row.pelvisHeight, 4);
    out += ",\"heightRel\":";
    jsonNumber(out, row.pelvisHeightRel, 4);
    out += ",\"yawDeg\":";
    jsonNumber(out, row.pelvisYaw, 2);
    out += ",\"rootYawDeg\":";
    jsonNumber(out, row.rootYaw, 2);
    out += "},\"feet\":{";
    for (int s = 0; s < 2; ++s) {
        out += s == 0 ? "\"left\":{\"contact\":" : ",\"right\":{\"contact\":";
        jsonNumber(out, row.contact[s], 3);
        out += ",\"phase\":";
        jsonString(out, row.phase[s]);
        out += '}';
    }
    out += "}}";
}

struct ClipRun {
    std::string id;
    std::string name;
    std::vector<FrameRow> rows;
};

struct ClipSummary {
    int framesWithCrossings = 0;
    int maxCrossings = 0;
    long long totalCrossings = 0;
    double depthSum = 0.0;
    float maxDepth = 0.0f;
    std::vector<const FrameRow*> worst;  // frames with crossings, deepest first
};

inline ClipSummary summarizeClip(const ClipRun& run) {
    ClipSummary s;
    for (const FrameRow& r : run.rows) {
        s.framesWithCrossings += r.crossings > 0 ? 1 : 0;
        s.maxCrossings = std::max(s.maxCrossings, r.crossings);
        s.totalCrossings += r.crossings;
        s.depthSum += r.depthSum;
        s.maxDepth = std::max(s.maxDepth, r.maxDepth);
        if (r.crossings > 0) s.worst.push_back(&r);
    }
    std::stable_sort(s.worst.begin(), s.worst.end(), [](const FrameRow* a, const FrameRow* b) {
        if (a->maxDepth != b->maxDepth) return a->maxDepth > b->maxDepth;
        return a->crossings > b->crossings;
    });
    return s;
}

// Runs the --legs measurement and prints the report (and writes the JSON when requested).
// Returns the process exit code.
inline int runLegMode(const vrm::VrmModel& model, animation::HumanoidRetargeter& rig, const LegOptions& opt) {
    const LegSet legs = detail::classifyLegTriangles(model);
    const animation::CollisionProxyInfo proxy = rig.collisionProxy();
    const Vec3 up = math::normalize(model.up);
    const Vec3 lateral = math::normalize(model.right);

    std::printf("legs %s: thigh triangles left %zu, right %zu (dominant joint = upperLeg) | plane tolerance 1 um, "
                "crossing > %.2f mm\n",
                opt.modelLabel.c_str(), legs.tris[0].size(), legs.tris[1].size(), opt.crossingM * 1000.0f);
    if (proxy.valid)
        std::printf("fitted thigh proxy radius %.1f mm, shin %.1f mm (%s)\n", proxy.thighRadius * 1000.0f,
                    proxy.shinRadius * 1000.0f, proxy.fromMesh ? "fitted from the mesh" : "estimated from the skeleton");
    else
        std::printf("fitted thigh proxy radius: not available for this model\n");

    // Neutral (arms-out) pose: pairs that already touch there are masked for every frame.
    const animation::RetargetedPose neutral = rig.applyClip(motion::neutralClipPose(), 0.0);
    const Vec3 restHip = detail::hipLine(neutral, model, up);
    std::vector<std::vector<Mat4>> palette;
    std::vector<Vec3> pos;
    std::unordered_set<std::uint64_t> masked;
    {
        detail::skinLegVertices(model, neutral, legs.verts, palette, pos);
        detail::Grid grid;
        grid.build(pos, legs.tris[1]);
        const std::unordered_set<std::uint64_t> none;
        detail::ScanResult ignored;
        detail::scanPairs(legs, pos, grid, none, opt.crossingM, true, &masked, ignored);
    }
    std::printf("masked rest-contact pairs (touching in the neutral pose): %zu\n", masked.size());
    if (legs.tris[0].empty() || legs.tris[1].empty())
        std::printf("no thigh pair to measure: this model has no skinned triangles whose dominant joint is a "
                    "leg upper bone on both sides\n");

    std::vector<ClipRun> runs;
    const int frames = static_cast<int>(opt.seconds * opt.hz);
    for (const std::string& clip : opt.clips) {
        motion::DancePlayer player;  // fresh per clip: a reused player keeps the first schedule
        player.loadLibrary(opt.dances);
        // The desktop app re-plans the authored footwork for each model (setLegGeometry);
        // --raw-footwork measures the authored foot tracks as stored.
        if (opt.rawFootwork) player.setFootworkPlanning(false);
        else player.setLegGeometry(rig.legGeometry());
        player.setOverlayMode(opt.overlay ? motion::OverlayMode::Full : motion::OverlayMode::Off);
        if (!player.selectClip(clip, 0.0, true)) {
            std::printf("clip %s not found\n", clip.c_str());
            continue;
        }
        ClipRun run;
        run.id = clip;
        run.name = clip;
        if (const motion::HumanoidClip* authored = player.authoredClip(clip)) run.name = authored->name;
        run.rows.reserve(static_cast<std::size_t>(std::max(frames, 0)));
        for (int f = 0; f < frames; ++f) {
            const double t = f / opt.hz;
            const motion::DanceFrame frame = player.update(t);
            const animation::RetargetedPose pose = rig.applyClip(frame.pose, t);
            detail::skinLegVertices(model, pose, legs.verts, palette, pos);
            detail::Grid grid;
            grid.build(pos, legs.tris[1]);
            detail::ScanResult scan;
            detail::scanPairs(legs, pos, grid, masked, opt.crossingM, false, nullptr, scan);

            FrameRow row;
            row.frame = f;
            row.t = t;
            row.crossings = scan.crossings;
            row.masked = scan.masked;
            row.maxDepth = scan.maxDepth;
            row.depthSum = scan.depthSum;
            row.pairs = scan.top;

            // Knees and bend planes, one leg at a time: hip = upper leg, knee = lower leg, ankle = foot.
            for (int s = 0; s < 2; ++s) {
                const auto hip = s == 0 ? vrm::HumanoidBone::LeftUpperLeg : vrm::HumanoidBone::RightUpperLeg;
                const auto knee = s == 0 ? vrm::HumanoidBone::LeftLowerLeg : vrm::HumanoidBone::RightLowerLeg;
                const auto ankle = s == 0 ? vrm::HumanoidBone::LeftFoot : vrm::HumanoidBone::RightFoot;
                if (model.nodeFor(hip) < 0 || model.nodeFor(knee) < 0 || model.nodeFor(ankle) < 0) {
                    row.knee[s] = Vec3{kNaN, kNaN, kNaN};
                    continue;
                }
                const Vec3 h = pose.bonePosition(hip, model);
                const Vec3 k = pose.bonePosition(knee, model);
                const Vec3 a = pose.bonePosition(ankle, model);
                row.knee[s] = k;
                row.flexion[s] = detail::kneeFlexion(h, k, a);
                row.bendPlane[s] = detail::bendPlane(h, k, a, lateral, row.flexion[s]);
            }

            // Pelvis: height above the lower ankle joint, yaw of the hip line against the neutral pose.
            const int hipsNode = model.nodeFor(vrm::HumanoidBone::Hips);
            if (hipsNode >= 0 && model.nodeFor(vrm::HumanoidBone::LeftFoot) >= 0 &&
                model.nodeFor(vrm::HumanoidBone::RightFoot) >= 0) {
                const float lowerAnkle = std::min(math::dot(pose.bonePosition(vrm::HumanoidBone::LeftFoot, model), up),
                                                  math::dot(pose.bonePosition(vrm::HumanoidBone::RightFoot, model), up));
                row.pelvisHeight = math::dot(pose.bonePosition(vrm::HumanoidBone::Hips, model), up) - lowerAnkle;
                row.pelvisHeightRel = row.pelvisHeight / std::max(model.height, 1e-3f);
            }
            if (hipsNode >= 0 && model.nodeFor(vrm::HumanoidBone::LeftUpperLeg) >= 0 &&
                model.nodeFor(vrm::HumanoidBone::RightUpperLeg) >= 0)
                row.pelvisYaw = detail::signedYaw(restHip, detail::hipLine(pose, model, up), up);
            row.rootYaw = frame.pose.rootYaw * 360.0f;

            for (int s = 0; s < 2; ++s) {
                row.contact[s] = frame.pose.foot[static_cast<std::size_t>(s)].contact;
                row.phase[s] = motion::footPhaseName(frame.pose.foot[static_cast<std::size_t>(s)].phase);
            }
            run.rows.push_back(std::move(row));
        }
        runs.push_back(std::move(run));
    }

    // --- console report -------------------------------------------------------------------
    for (const ClipRun& run : runs) {
        const ClipSummary s = summarizeClip(run);
        std::printf("\n%s (%s): %zu frames @%.0f Hz, footwork %s | frames with thigh crossings %d/%zu, max %d pairs "
                    "in a frame, %lld crossings in total | deepest %.2f mm, mean %.2f mm per crossing\n",
                    run.id.c_str(), run.name.c_str(), run.rows.size(), opt.hz, opt.rawFootwork ? "raw" : "planned",
                    s.framesWithCrossings, run.rows.size(), s.maxCrossings, s.totalCrossings, s.maxDepth * 1000.0f,
                    s.totalCrossings > 0 ? s.depthSum / static_cast<double>(s.totalCrossings) * 1000.0 : 0.0);
        const std::vector<const FrameRow*>& worst = s.worst;
        for (std::size_t i = 0; i < worst.size() && i < static_cast<std::size_t>(std::max(opt.top, 0)); ++i) {
            const FrameRow& r = *worst[i];
            const PairHit& hit = r.pairs.front();
            const LegTriangle& a = legs.tris[0][static_cast<std::size_t>(hit.left)];
            const LegTriangle& b = legs.tris[1][static_cast<std::size_t>(hit.right)];
            std::printf("  worst t=%7.3f s (frame %d): %d pairs, deepest %.2f mm | L tri %d (mesh %d, prim %d, tri %d) "
                        "x R tri %d (mesh %d, prim %d, tri %d) at (%.3f, %.3f, %.3f) | knee flexion L %.1f R %.1f deg, "
                        "bend plane L %.1f R %.1f deg | pelvis %.3f m, yaw %.1f deg | feet L %s %.2f, R %s %.2f\n",
                        r.t, r.frame, r.crossings, hit.depth * 1000.0f, hit.left, a.mesh, a.primitive, a.primTriangle,
                        hit.right, b.mesh, b.primitive, b.primTriangle, hit.at.x, hit.at.y, hit.at.z,
                        r.flexion[0], r.flexion[1], r.bendPlane[0], r.bendPlane[1], r.pelvisHeight, r.pelvisYaw,
                        r.phase[0], r.contact[0], r.phase[1], r.contact[1]);
        }
    }

    // --- JSON ---------------------------------------------------------------------------------
    if (!opt.json.empty()) {
        std::string out;
        out.reserve(1u << 20);
        out += "{\n\"tool\":\"bgmedia_mesh_clip --legs\",\n\"model\":";
        jsonString(out, opt.modelLabel);
        out += ",\n\"fps\":";
        jsonNumber(out, opt.hz, 3);
        out += ",\n\"seconds\":";
        jsonNumber(out, opt.seconds, 3);
        out += ",\n\"overlay\":";
        out += opt.overlay ? "true" : "false";
        out += ",\n\"footwork\":";
        jsonString(out, opt.rawFootwork ? "raw" : "planned");
        out += ",\n\"crossingMm\":";
        jsonNumber(out, opt.crossingM * 1000.0, 3);
        out += ",\n\"planeToleranceUm\":1,\n\"legTriangles\":{\"left\":" + std::to_string(legs.tris[0].size()) +
               ",\"right\":" + std::to_string(legs.tris[1].size()) + "}";
        out += ",\n\"maskedRestPairs\":" + std::to_string(masked.size());
        out += ",\n\"thighProxyRadiusM\":";
        jsonNumber(out, proxy.valid ? proxy.thighRadius : kNaN, 4);
        out += ",\n\"shinProxyRadiusM\":";
        jsonNumber(out, proxy.valid ? proxy.shinRadius : kNaN, 4);
        out += ",\n\"units\":{\"length\":\"m (positions); mm (depths)\",\"angle\":\"deg\"},\n\"clips\":[";
        for (std::size_t c = 0; c < runs.size(); ++c) {
            const ClipRun& run = runs[c];
            const ClipSummary s = summarizeClip(run);
            const std::vector<const FrameRow*>& worst = s.worst;
            if (c) out += ',';
            out += "\n{\"clip\":";
            jsonString(out, run.id);
            out += ",\"name\":";
            jsonString(out, run.name);
            out += ",\"frames\":" + std::to_string(run.rows.size());
            out += ",\n\"summary\":{\"framesWithCrossings\":" + std::to_string(s.framesWithCrossings) +
                   ",\"maxCrossings\":" + std::to_string(s.maxCrossings) +
                   ",\"totalCrossings\":" + std::to_string(s.totalCrossings) + ",\"maxDepthMm\":";
            jsonNumber(out, s.maxDepth * 1000.0, 3);
            out += ",\"meanDepthMm\":";
            jsonNumber(out, s.totalCrossings > 0 ? s.depthSum / static_cast<double>(s.totalCrossings) * 1000.0 : kNaN, 3);
            out += ",\"worstFrames\":[";
            for (std::size_t i = 0; i < worst.size() && i < static_cast<std::size_t>(std::max(opt.top, 0)); ++i) {
                const FrameRow& r = *worst[i];
                if (i) out += ',';
                out += "{\"frame\":" + std::to_string(r.frame) + ",\"t\":";
                jsonNumber(out, r.t, 4);
                out += ",\"crossings\":" + std::to_string(r.crossings) + ",\"maxDepthMm\":";
                jsonNumber(out, r.maxDepth * 1000.0, 3);
                out += ",\"pair\":";
                if (r.pairs.empty()) {
                    out += "null";
                } else {
                    jsonPair(out, r.pairs.front(), legs, model);
                }
                out += '}';
            }
            out += "]},\n\"rows\":[";
            for (std::size_t i = 0; i < run.rows.size(); ++i) {
                if (i) out += ",\n";
                jsonRow(out, run.rows[i], legs, model);
            }
            out += "]}";
        }
        out += "]\n}\n";
        std::ofstream file(opt.json, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "cannot write %s\n", opt.json.string().c_str());
            return 1;
        }
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        std::printf("\nwrote %s (%zu clips)\n", opt.json.string().c_str(), runs.size());
    }
    return 0;
}

} // namespace bgmedia::meshclip
