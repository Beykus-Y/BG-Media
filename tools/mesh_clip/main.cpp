// bgmedia_mesh_clip: mesh-level (vertex) penetration measurement.
//
// CPU-skins every vertex of a VRM with the retargeted pose and reports, per
// frame, how many ARM-skinned vertices (upper/lower arm, hand, fingers, sleeves
// and gloves: anything whose dominant joint is an arm joint) lie behind the
// surface of TORSO-skinned vertices, and how many touch HAIR vertices.
//
// Method (an approximation, stated plainly): for an arm vertex a, take the
// nearest torso vertex u within 3 cm and its skinned normal n. Depth is
// -dot(a - u, n) when negative-side. This is vertex-vs-nearest-vertex, not a
// triangle intersection test: it can miss thin overlaps and flag a vertex that
// is merely close to a fold. Vertices that already "penetrate" in the neutral
// (arms-out) pose are masked, so shoulder seams are not reported.
// Hair is double-sided sheets, so for hair only proximity (< 4 mm) is reported.

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace bgmedia;
using math::Mat4;
using math::Vec3;

namespace {

// kUpper (upper-arm skin/sleeve) is only tested against hair: a hanging upper arm
// lies against the torso by construction, so torso contact there is not clipping.
enum Group : unsigned char { kOther = 0, kArm = 1, kTorso = 2, kHair = 3, kUpper = 4 };

struct SkinVertex {
    Vec3 position;
    Vec3 normal;
    std::array<std::uint16_t, 4> joints;
    std::array<float, 4> weights;
    int skin = -1;
    Group group = kOther;
};

struct Grid {
    float cell = 0.03f;
    std::unordered_map<long long, std::vector<int>> cells;
    static long long key(int x, int y, int z) {
        return (static_cast<long long>(x) * 73856093LL) ^ (static_cast<long long>(y) * 19349663LL) ^
               (static_cast<long long>(z) * 83492791LL);
    }
    void add(Vec3 p, int index) {
        cells[key(static_cast<int>(std::floor(p.x / cell)), static_cast<int>(std::floor(p.y / cell)),
                  static_cast<int>(std::floor(p.z / cell)))].push_back(index);
    }
    // Nearest point index within `radius` (<= cell), or -1.
    int nearest(Vec3 p, const std::vector<Vec3>& points, float radius, float* distance) const {
        const int cx = static_cast<int>(std::floor(p.x / cell));
        const int cy = static_cast<int>(std::floor(p.y / cell));
        const int cz = static_cast<int>(std::floor(p.z / cell));
        int best = -1;
        float bestD = radius;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto it = cells.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == cells.end()) continue;
                    for (int i : it->second) {
                        const float d = (points[static_cast<std::size_t>(i)] - p).length();
                        if (d < bestD) {
                            bestD = d;
                            best = i;
                        }
                    }
                }
        if (distance) *distance = bestD;
        return best;
    }
};

struct Measurement {
    int penetrating = 0;   // arm vertices behind the torso surface by > threshold
    float maxDepth = 0.0f;
    int hairTouch = 0;
    Vec3 worstAt{};
    int worstNode = -1;
};

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path modelPath = "models/Model1.vrm";
    std::filesystem::path dances = "assets/dances";
    std::vector<std::string> clips = {"neonPulse", "streetCurrent", "fluidSignal"};
    double seconds = 40.0;
    double hz = 10.0;
    bool overlay = true;
    float depthThreshold = 0.008f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--model" && i + 1 < argc) modelPath = argv[++i];
        else if (a == "--dances" && i + 1 < argc) dances = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--hz" && i + 1 < argc) hz = std::atof(argv[++i]);
        else if (a == "--overlay" && i + 1 < argc) overlay = std::atoi(argv[++i]) != 0;
        else if (a == "--depth-mm" && i + 1 < argc) depthThreshold = static_cast<float>(std::atof(argv[++i])) * 0.001f;
        else if (a == "--clips" && i + 1 < argc) {
            clips.clear();
            std::string list = argv[++i];
            std::size_t at = 0;
            while (at <= list.size()) {
                const std::size_t comma = list.find(',', at);
                clips.push_back(list.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
    }
    vrm::LoadResult loaded = vrm::VrmLoader::load(modelPath);
    if (!loaded.ok()) {
        std::fprintf(stderr, "load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const vrm::VrmModel& model = *loaded.model;
    animation::HumanoidRetargeter rig(model);
    if (!rig.valid()) {
        std::fprintf(stderr, "retargeter invalid\n");
        return 1;
    }

    // --- classify vertices ------------------------------------------------
    std::vector<unsigned char> nodeGroup(model.nodes.size(), kOther);
    const auto mark = [&](vrm::HumanoidBone b, Group g) {
        const int n = model.nodeFor(b);
        if (n >= 0) nodeGroup[static_cast<std::size_t>(n)] = g;
    };
    mark(vrm::HumanoidBone::LeftUpperArm, kUpper);
    mark(vrm::HumanoidBone::RightUpperArm, kUpper);
    for (auto b : {vrm::HumanoidBone::LeftLowerArm, vrm::HumanoidBone::LeftHand,
                   vrm::HumanoidBone::RightLowerArm, vrm::HumanoidBone::RightHand})
        mark(b, kArm);
    for (auto b : {vrm::HumanoidBone::Hips, vrm::HumanoidBone::Spine, vrm::HumanoidBone::Chest,
                   vrm::HumanoidBone::UpperChest})
        mark(b, kTorso);
    for (int sd = 0; sd < 2; ++sd)
        for (int fg = 0; fg < 5; ++fg)
            for (int jt = 0; jt < 3; ++jt) {
                const int n = model.fingerNode(sd, fg, jt);
                if (n >= 0) nodeGroup[static_cast<std::size_t>(n)] = kArm;
            }

    std::vector<SkinVertex> verts;
    std::vector<std::array<int, 3>> tris;  // indices into `verts`
    for (const vrm::MeshInstance& inst : model.meshInstances) {
        if (inst.mesh < 0) continue;
        const vrm::Skin* skin = model.skinForNode(inst.node);
        if (!skin) continue;
        const int skinIndex = static_cast<int>(skin - model.skins.data());
        for (const vrm::Primitive& prim : model.meshes[static_cast<std::size_t>(inst.mesh)].primitives) {
            if (!prim.skinned) continue;
            const bool hair = prim.material >= 0 &&
                              model.materials[static_cast<std::size_t>(prim.material)].name.find("HAIR") !=
                                  std::string::npos;
            const std::size_t base = verts.size();
            for (const vrm::Vertex& v : prim.vertices) {
                SkinVertex s;
                s.position = v.position;
                s.normal = v.normal;
                s.joints = v.joints;
                s.weights = v.weights;
                s.skin = skinIndex;
                std::size_t best = 0;
                for (std::size_t k = 1; k < 4; ++k)
                    if (v.weights[k] > v.weights[best]) best = k;
                if (hair) {
                    s.group = kHair;
                } else if (v.weights[best] >= 0.5f && v.joints[best] < skin->joints.size()) {
                    s.group = static_cast<Group>(nodeGroup[static_cast<std::size_t>(skin->joints[v.joints[best]])]);
                }
                verts.push_back(s);
            }
            for (std::size_t t = 0; t + 2 < prim.indices.size(); t += 3) {
                tris.push_back({static_cast<int>(base + prim.indices[t]), static_cast<int>(base + prim.indices[t + 1]),
                                static_cast<int>(base + prim.indices[t + 2])});
            }
        }
    }
    std::size_t counts[5] = {0, 0, 0, 0, 0};
    for (const auto& v : verts) ++counts[v.group];
    std::printf("model %s: %zu skinned vertices (arm %zu, torso %zu, hair %zu)\n", modelPath.string().c_str(),
                verts.size(), counts[kArm], counts[kTorso], counts[kHair]);

    std::vector<Vec3> skinned(verts.size()), normals(verts.size());
    std::vector<std::vector<Mat4>> skinMatrices(model.skins.size());
    const auto skinAll = [&](const animation::RetargetedPose& pose) {
        for (std::size_t s = 0; s < model.skins.size(); ++s) {
            const vrm::Skin& skin = model.skins[s];
            skinMatrices[s].resize(skin.joints.size());
            for (std::size_t j = 0; j < skin.joints.size(); ++j) {
                skinMatrices[s][j] = pose.nodeGlobals[static_cast<std::size_t>(skin.joints[j])] *
                                     skin.inverseBindMatrices[j];
            }
        }
        for (std::size_t i = 0; i < verts.size(); ++i) {
            const SkinVertex& v = verts[i];
            Vec3 p{}, n{};
            for (int k = 0; k < 4; ++k) {
                if (v.weights[k] <= 0.0f) continue;
                const Mat4& m = skinMatrices[static_cast<std::size_t>(v.skin)][v.joints[static_cast<std::size_t>(k)]];
                p = p + m.transformPoint(v.position) * v.weights[static_cast<std::size_t>(k)];
                n = n + m.transformVector(v.normal) * v.weights[static_cast<std::size_t>(k)];
            }
            skinned[i] = p;
            normals[i] = math::normalize(n);
        }
    };

    // Per-frame measurement. `mask` (optional) suppresses vertices flagged in the neutral pose.
    std::vector<unsigned char> mask(verts.size(), 0);
    const auto measure = [&](const animation::RetargetedPose& pose, bool buildMask) {
        skinAll(pose);
        std::vector<Vec3> torsoPts, hairPts, torsoN;
        std::vector<int> torsoIdx, hairIdx;
        Grid torsoGrid, hairGrid;
        for (std::size_t i = 0; i < verts.size(); ++i) {
            if (verts[i].group == kTorso) {
                torsoGrid.add(skinned[i], static_cast<int>(torsoPts.size()));
                torsoPts.push_back(skinned[i]);
                torsoN.push_back(normals[i]);
            } else if (verts[i].group == kHair) {
                hairGrid.add(skinned[i], static_cast<int>(hairPts.size()));
                hairPts.push_back(skinned[i]);
            }
        }
        Measurement out;
        for (std::size_t i = 0; i < verts.size(); ++i) {
            if (verts[i].group != kArm && verts[i].group != kUpper) continue;
            float d = 0.0f;
            const int u = verts[i].group == kArm ? torsoGrid.nearest(skinned[i], torsoPts, 0.03f, &d) : -1;
            bool hit = false;
            if (u >= 0) {
                const float depth = -math::dot(skinned[i] - torsoPts[static_cast<std::size_t>(u)],
                                               torsoN[static_cast<std::size_t>(u)]);
                if (depth > depthThreshold) {
                    hit = true;
                    if (buildMask) {
                        mask[i] = 1;
                    } else if (!mask[i]) {
                        ++out.penetrating;
                        if (depth > out.maxDepth) {
                            out.maxDepth = depth;
                            out.worstAt = skinned[i];
                            const std::size_t best = verts[i].weights[0] >= verts[i].weights[1] ? 0 : 1;
                            const vrm::Skin& sk = model.skins[static_cast<std::size_t>(verts[i].skin)];
                            out.worstNode = sk.joints[verts[i].joints[best]];
                        }
                    }
                }
            }
            float hd = 0.0f;
            if (!hit && hairGrid.nearest(skinned[i], hairPts, 0.004f, &hd) >= 0 && !buildMask && !mask[i]) {
                ++out.hairTouch;
            } else if (buildMask && hairGrid.nearest(skinned[i], hairPts, 0.004f, &hd) >= 0) {
                mask[i] = 1;
            }
        }
        return out;
    };

    // --- triangle-level test (uses the vertices skinned by the last measure()) ----
    const auto segTri = [](Vec3 p, Vec3 q, Vec3 a, Vec3 b, Vec3 c) {
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
    };
    const auto triTri = [&](const std::array<int, 3>& A, const std::array<int, 3>& B) {
        const Vec3 a[3] = {skinned[static_cast<std::size_t>(A[0])], skinned[static_cast<std::size_t>(A[1])],
                           skinned[static_cast<std::size_t>(A[2])]};
        const Vec3 b[3] = {skinned[static_cast<std::size_t>(B[0])], skinned[static_cast<std::size_t>(B[1])],
                           skinned[static_cast<std::size_t>(B[2])]};
        for (int e = 0; e < 3; ++e) {
            if (segTri(a[e], a[(e + 1) % 3], b[0], b[1], b[2])) return true;
            if (segTri(b[e], b[(e + 1) % 3], a[0], a[1], a[2])) return true;
        }
        return false;
    };
    std::vector<int> armTris, torsoTris;
    for (std::size_t t = 0; t < tris.size(); ++t) {
        const auto& tr = tris[t];
        const Group g0 = verts[static_cast<std::size_t>(tr[0])].group;
        if (g0 != verts[static_cast<std::size_t>(tr[1])].group || g0 != verts[static_cast<std::size_t>(tr[2])].group) continue;
        if (g0 == kArm) armTris.push_back(static_cast<int>(t));
        else if (g0 == kTorso) torsoTris.push_back(static_cast<int>(t));
    }
    std::vector<unsigned char> neutralTriHit(tris.size(), 0);
    // Returns arm triangles crossing a torso triangle (excluding rest-pose crossings).
    const auto crossingArmTris = [&](bool recordNeutral) {
        constexpr float cell = 0.03f;
        std::unordered_map<long long, std::vector<int>> grid;
        const auto cellKey = [](int x, int y, int z) {
            return (static_cast<long long>(x) * 73856093LL) ^ (static_cast<long long>(y) * 19349663LL) ^
                   (static_cast<long long>(z) * 83492791LL);
        };
        const auto bounds = [&](int t, Vec3& lo, Vec3& hi) {
            lo = hi = skinned[static_cast<std::size_t>(tris[static_cast<std::size_t>(t)][0])];
            for (int k = 1; k < 3; ++k) {
                const Vec3 p = skinned[static_cast<std::size_t>(tris[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)])];
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        };
        for (int t : torsoTris) {
            Vec3 lo, hi;
            bounds(t, lo, hi);
            for (int x = (int)std::floor(lo.x / cell); x <= (int)std::floor(hi.x / cell); ++x)
                for (int y = (int)std::floor(lo.y / cell); y <= (int)std::floor(hi.y / cell); ++y)
                    for (int z = (int)std::floor(lo.z / cell); z <= (int)std::floor(hi.z / cell); ++z)
                        grid[cellKey(x, y, z)].push_back(t);
        }
        std::vector<int> hits;
        std::vector<int> stamp(tris.size(), -1);
        for (int a : armTris) {
            Vec3 lo, hi;
            bounds(a, lo, hi);
            bool hit = false;
            for (int x = (int)std::floor(lo.x / cell); x <= (int)std::floor(hi.x / cell) && !hit; ++x)
                for (int y = (int)std::floor(lo.y / cell); y <= (int)std::floor(hi.y / cell) && !hit; ++y)
                    for (int z = (int)std::floor(lo.z / cell); z <= (int)std::floor(hi.z / cell) && !hit; ++z) {
                        const auto it = grid.find(cellKey(x, y, z));
                        if (it == grid.end()) continue;
                        for (int b : it->second) {
                            if (stamp[static_cast<std::size_t>(b)] == a) continue;
                            stamp[static_cast<std::size_t>(b)] = a;
                            if (triTri(tris[static_cast<std::size_t>(a)], tris[static_cast<std::size_t>(b)])) {
                                hit = true;
                                break;
                            }
                        }
                    }
            if (hit) {
                if (recordNeutral) neutralTriHit[static_cast<std::size_t>(a)] = 1;
                else if (!neutralTriHit[static_cast<std::size_t>(a)]) hits.push_back(a);
            }
        }
        return hits;
    };

    // Neutral (arms out) pose defines vertices that already touch at rest.
    (void)measure(rig.applyClip(motion::neutralClipPose(), 0.0), true);
    (void)crossingArmTris(true);
    std::printf("arm triangles %zu, torso triangles %zu, rest-pose crossings masked: %zu\n", armTris.size(),
                torsoTris.size(), static_cast<std::size_t>(std::count(neutralTriHit.begin(), neutralTriHit.end(), 1)));

    // --- explanation of one frame (call right after measure() on the same pose) ---------
    const auto explain = [&](const animation::RetargetedPose& pose, const Measurement& m, double t) {
        const vrm::HumanoidBone hb = vrm::HumanoidBone::Hips;
        const Vec3 hips = pose.bonePosition(hb, model);
        const Vec3 neck = pose.bonePosition(vrm::HumanoidBone::Neck, model);
        const float torsoLen = (neck - hips).length();
        const Vec3 up = math::normalize(neck - hips);
        const Vec3 w = m.worstAt - hips;
        const float along = math::dot(w, up) / torsoLen;
        const auto info = rig.collisionProxy();
        struct Sample {
            const char* name;
            Vec3 p;
            float r;
        };
        std::vector<Sample> samples;
        for (int sd = 0; sd < 2; ++sd) {
            const auto lower = sd == 0 ? vrm::HumanoidBone::LeftLowerArm : vrm::HumanoidBone::RightLowerArm;
            const auto hand = sd == 0 ? vrm::HumanoidBone::LeftHand : vrm::HumanoidBone::RightHand;
            const Vec3 e = pose.bonePosition(lower, model), wr = pose.bonePosition(hand, model);
            const int prox = model.fingerNode(sd, 2, 0);
            const float off = prox >= 0 ? 0.5f * (pose.nodeGlobals[static_cast<std::size_t>(prox)].translationPart() - wr).length()
                                        : 0.045f * model.height;
            samples.push_back({"elbow", e, info.foreArmRadius});
            samples.push_back({"forearm-mid", (e + wr) * 0.5f, info.foreArmRadius});
            samples.push_back({"wrist", wr, info.handRadius});
            samples.push_back({"palm", wr + math::normalize(wr - e) * off, info.handRadius});
        }
        const Sample* nearest = nullptr;
        float nd = 1e9f;
        for (const Sample& s : samples) {
            const float d = (s.p - m.worstAt).length();
            if (d < nd) {
                nd = d;
                nearest = &s;
            }
        }
        const std::vector<int> hits = crossingArmTris(false);
        float tMin = 9.0f, tMax = -9.0f;
        for (int a : hits) {
            Vec3 c{};
            for (int k = 0; k < 3; ++k) c = c + skinned[static_cast<std::size_t>(tris[static_cast<std::size_t>(a)][static_cast<std::size_t>(k)])];
            c = c * (1.0f / 3.0f);
            const float tt = math::dot(c - hips, up) / torsoLen;
            tMin = std::min(tMin, tt);
            tMax = std::max(tMax, tt);
        }
        std::printf("  explain t=%.2f s: proxy raw clearance %+.3f m | deepest vertex %.1f mm at spine position %.2f "
                    "(1.0 = neck base) | nearest proxy sample '%s' %.1f cm away (sphere radius %.1f cm) -> %s\n",
                    t, pose.stats.armClearanceRaw, m.maxDepth * 1000.0f, along, nearest ? nearest->name : "-", nd * 100.0f,
                    nearest ? nearest->r * 100.0f : 0.0f,
                    nearest && nd <= nearest->r + 0.0065f ? "covered by a proxy sphere" : "NOT covered by any proxy sphere");
        if (hits.empty())
            std::printf("           triangle test: 0 arm triangles cross a torso triangle (vertices behind the surface without a crossing = "
                        "near contact / inside a fold)\n");
        else
            std::printf("           triangle test: %zu arm triangles cross torso triangles, spine position %.2f..%.2f\n",
                        hits.size(), tMin, tMax);
    };
    std::size_t masked = 0;
    for (unsigned char m : mask) masked += m;
    std::printf("rest-contact vertices masked: %zu\n", masked);

    struct Row {
        double t;
        Measurement m;
        std::size_t index;
    };
    for (const std::string& clip : clips) {
        motion::DancePlayer player;  // fresh per clip: a reused player keeps the first schedule
        player.loadLibrary(dances);
        player.setOverlayMode(overlay ? motion::OverlayMode::Full : motion::OverlayMode::Off);
        if (!player.selectClip(clip, 0.0, true)) {
            std::printf("clip %s not found\n", clip.c_str());
            continue;
        }
        std::vector<Row> rows;
        std::vector<animation::RetargetedPose> poses;
        poses.reserve(static_cast<std::size_t>(seconds * hz) + 1);
        const int frames = static_cast<int>(seconds * hz);
        for (int f = 0; f < frames; ++f) {
            const double t = f / hz;
            const auto frame = player.update(t);
            poses.push_back(rig.applyClip(frame.pose, t));
            rows.push_back({t, measure(poses.back(), false), poses.size() - 1});
        }
        int torsoFrames = 0, hairFrames = 0;
        float worst = 0.0f;
        for (const Row& r : rows) {
            torsoFrames += r.m.penetrating >= 5;
            hairFrames += r.m.hairTouch >= 5;
            worst = std::max(worst, r.m.maxDepth);
        }
        std::printf("\n%s (overlay %s): %zu frames @%.0f Hz | frames with >=5 arm vertices behind torso by >%.0f mm: %d "
                    "| frames with >=5 arm vertices within 4 mm of hair: %d | worst depth %.1f mm\n",
                    clip.c_str(), overlay ? "full" : "off", rows.size(), hz, depthThreshold * 1000.0f, torsoFrames,
                    hairFrames, worst * 1000.0f);
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.m.penetrating * 1000.0f + a.m.maxDepth > b.m.penetrating * 1000.0f + b.m.maxDepth;
        });
        for (std::size_t i = 0; i < std::min<std::size_t>(5, rows.size()); ++i) {
            const Row& r = rows[i];
            std::printf("  worst t=%6.2f s: %4d verts, depth %.1f mm at joint '%s'\n", r.t, r.m.penetrating,
                        r.m.maxDepth * 1000.0f,
                        r.m.worstNode >= 0 ? model.nodes[static_cast<std::size_t>(r.m.worstNode)].name.c_str() : "-");
        }
        for (std::size_t i = 0; i < std::min<std::size_t>(3, rows.size()); ++i) {
            if (rows[i].m.penetrating < 5) break;
            const double t = rows[i].t;
            // Use the pose from the sweep itself: a backwards update() would seek and
            // re-align the schedule, giving a different pose for the same time.
            const auto& pose = poses[rows[i].index];
            const Measurement m = measure(pose, false);  // re-skins this pose
            explain(pose, m, t);
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.m.hairTouch > b.m.hairTouch; });
        for (std::size_t i = 0; i < std::min<std::size_t>(3, rows.size()); ++i)
            std::printf("  hair-contact t=%6.2f s: %d arm vertices within 4 mm of hair\n", rows[i].t, rows[i].m.hairTouch);
    }
    return 0;
}
