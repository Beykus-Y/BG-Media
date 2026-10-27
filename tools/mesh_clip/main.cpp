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

    // Neutral (arms out) pose defines vertices that already touch at rest.
    (void)measure(rig.applyClip(motion::neutralClipPose(), 0.0), true);
    std::size_t masked = 0;
    for (unsigned char m : mask) masked += m;
    std::printf("rest-contact vertices masked: %zu\n", masked);

    struct Row {
        double t;
        Measurement m;
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
        const int frames = static_cast<int>(seconds * hz);
        for (int f = 0; f < frames; ++f) {
            const double t = f / hz;
            const auto frame = player.update(t);
            rows.push_back({t, measure(rig.applyClip(frame.pose, t), false)});
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
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.m.hairTouch > b.m.hairTouch; });
        for (std::size_t i = 0; i < std::min<std::size_t>(3, rows.size()); ++i)
            std::printf("  hair-contact t=%6.2f s: %d arm vertices within 4 mm of hair\n", rows[i].t, rows[i].m.hairTouch);
    }
    return 0;
}
