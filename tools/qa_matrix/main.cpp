// bgmedia_qa_matrix: deterministic replay matrix for the BG Motion dance library.
//
//   bgmedia_qa_matrix [--model tests/fixtures/minimal.vrm]... [--dances assets/dances]
//       [--clip id]... [--fps 30,60,120] [--overlay off,full] [--spring 0,1]
//       [--from 0] [--seconds 8] [--seek-points 6] [--json out.json]
//
// Every scenario replays one clip from a fresh DancePlayer through the same path the
// Desktop uses (footwork pre-pass on, no audio, immediate clip selection). Two replays
// are made: a 240 Hz reference ("truth") and one at the scenario's fps. The pose is a
// function of media time, so the two should agree; the fps column shows what the
// playback rate alone does to the metric, and the legacy column is the per-sample
// formula earlier reports used.
//
// Seek error replays the clip from a fresh player that jumps straight to a few of the
// sampled times, lets the 0.18 s seek hand-over settle (0.25 s), and compares every node
// with the sequential replay at that time (spring off only: the spring carries history
// by design).
//
// Not measured here: skinned-mesh triangle intersections. They need the real mesh, so
// run bgmedia_motion_diag with the private model for those.

#include "../../tests/motion_metrics.hpp"

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bgmedia;
using math::Vec3;

namespace {

constexpr double kReferenceHz = 240.0;

struct Options {
    std::vector<std::filesystem::path> models;
    std::filesystem::path dances = "assets/dances";
    std::filesystem::path json;
    std::vector<std::string> clips;
    std::vector<double> fps = {30.0, 60.0, 120.0};
    std::vector<std::string> overlays = {"off", "full"};
    std::vector<int> springs = {0, 1};
    double from = 0.0;
    double seconds = 8.0;
    int seekPoints = 6;
};

struct Scenario {
    std::string model;
    std::string clip;
    double fps = 0.0;
    std::string overlay;
    bool spring = false;
    int samples = 0;
    qa::GroundDrag truth[2];     // 240 Hz reference replay: the number to trust
    qa::GroundDrag sampled[2];   // the same metric on the fps replay
    qa::GroundDrag legacy[2];    // per-sample formula on the fps replay
    double slideMm = 0.0;        // largest ankle travel inside one planted run (fps replay)
    double ikErrorMm = 0.0;      // worst retargeter foot IK error (fps replay)
    double worstIkT = 0.0;
    double seekMm = -1.0;        // worst node error of a direct jump, -1 when not measured
};

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::string jsonEscape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

motion::OverlayMode overlayFromName(const std::string& name) {
    return name == "off" ? motion::OverlayMode::Off : motion::OverlayMode::Full;
}

void configurePlayer(motion::DancePlayer& player, const animation::HumanoidRetargeter& rig,
                     const std::filesystem::path& dances, const std::string& overlay) {
    player.loadLibrary(dances);
    player.setLegGeometry(rig.legGeometry());
    player.setFootworkGap(2.5f);
    player.setOverlayMode(overlayFromName(overlay));
    player.setExpression(motion::MotionExpression::Off);
}

// Stage-space position of every node, so a jump can be compared node by node.
std::vector<Vec3> nodePositions(const animation::RetargetedPose& pose, std::size_t nodeCount) {
    std::vector<Vec3> out(nodeCount);
    for (std::size_t n = 0; n < nodeCount; ++n) {
        out[n] = animation::HumanoidRetargeter::stagePosition(pose, static_cast<int>(n));
    }
    return out;
}

// One sequential replay of a clip at `rate` samples per second.
struct Replay {
    std::vector<qa::FootSample> track[2];
    double worstIk = 0.0;
    double worstIkT = 0.0;
    std::vector<std::vector<Vec3>> seekNodes;  // node positions at the seek indices
};

Replay replay(const vrm::VrmModel& model, const animation::HumanoidRetargeter& rig, const Options& o,
              const std::string& clip, const std::string& overlay, bool spring, double rate,
              const std::vector<int>& seekAt) {
    Replay out;
    const int ankleNode[2] = {model.nodeFor(vrm::HumanoidBone::LeftFoot), model.nodeFor(vrm::HumanoidBone::RightFoot)};
    const int count = std::max(2, static_cast<int>(std::lround(o.seconds * rate))) + 1;
    const double dt = 1.0 / rate;
    out.seekNodes.resize(seekAt.size());

    motion::DancePlayer player;
    configurePlayer(player, rig, o.dances, overlay);
    if (!player.selectClip(clip, o.from, true)) {
        throw std::runtime_error("clip not found in the library: " + clip);
    }

    animation::SecondaryDynamics dynamics(model);
    animation::SecondarySettings settings;
    settings.enabled = spring;
    settings.intensity = 1.0f;
    settings.damping = 0.5f;

    for (int k = 0; k < count; ++k) {
        const double t = o.from + k * dt;
        const motion::DanceFrame df = player.update(t);
        animation::RetargetedPose pose = rig.applyClip(df.pose, t);
        if (spring) {
            if (k == 0) dynamics.reset(pose);
            dynamics.update(pose, k == 0 ? 0.0 : dt, settings);
        }
        if (pose.stats.maxFootError > out.worstIk) {
            out.worstIk = pose.stats.maxFootError;
            out.worstIkT = t;
        }
        for (int ft = 0; ft < 2; ++ft) {
            const auto& foot = df.pose.foot[static_cast<std::size_t>(ft)];
            qa::FootSample sample;
            sample.t = t;
            sample.ankle = animation::HumanoidRetargeter::stagePosition(pose, ankleNode[ft]);
            sample.targetY = foot.position.y;
            sample.planted = (foot.phase == motion::FootPhase::Stance || foot.phase == motion::FootPhase::Pivot) &&
                             foot.contact > 0.999f;
            out.track[ft].push_back(sample);
        }
        for (std::size_t j = 0; j < seekAt.size(); ++j) {
            if (seekAt[j] == k) out.seekNodes[j] = nodePositions(pose, model.nodes.size());
        }
    }
    return out;
}

// Largest ankle travel from where each planted run began.
double maxPlantedSlide(const std::vector<qa::FootSample>& track, double& worstT) {
    double slide = 0.0;
    std::size_t runStart = 0;
    bool inRun = false;
    for (std::size_t k = 0; k < track.size(); ++k) {
        const qa::FootSample& f = track[k];
        if (!f.planted) {
            inRun = false;
            continue;
        }
        if (!inRun) {
            inRun = true;
            runStart = k;
        }
        const Vec3 d = f.ankle - track[runStart].ankle;
        const double travel = std::hypot(d.x, d.z);
        if (travel > slide) {
            slide = travel;
            worstT = f.t;
        }
    }
    return slide;
}

Scenario runScenario(const std::string& modelName, const vrm::VrmModel& model,
                     const animation::HumanoidRetargeter& rig, const Options& o,
                     const std::string& clip, double fps, const std::string& overlay, bool spring) {
    Scenario s;
    s.model = modelName;
    s.clip = clip;
    s.fps = fps;
    s.overlay = overlay;
    s.spring = spring;
    s.samples = std::max(2, static_cast<int>(std::lround(o.seconds * fps))) + 1;

    // Seek points are spread evenly over the replay. The spring carries history by
    // design, so the seek comparison runs with it off. A jump is judged at a settle point
    // 0.25 s later: DancePlayer hands the pose over for 0.18 s after a jump, and the
    // schedule itself is what the seek must reproduce.
    const int settleSteps = static_cast<int>(std::lround(0.25 * fps));
    std::vector<int> jumpAt;
    if (!spring) {
        for (int j = 1; j <= o.seekPoints; ++j) {
            const int at = j * (s.samples - 1) / (o.seekPoints + 1);
            if (at + settleSteps < s.samples) jumpAt.push_back(at);
        }
    }
    std::vector<int> seekAt = jumpAt;
    for (int at : jumpAt) seekAt.push_back(at + settleSteps);
    const Replay reference = replay(model, rig, o, clip, overlay, spring, kReferenceHz, {});
    const Replay sampled = replay(model, rig, o, clip, overlay, spring, fps, seekAt);
    s.ikErrorMm = sampled.worstIk * 1000.0;
    s.worstIkT = sampled.worstIkT;

    double slideT = 0.0;
    for (int ft = 0; ft < 2; ++ft) {
        s.truth[ft] = qa::measureGroundDrag(reference.track[ft], 0.012f, 0.15);
        s.sampled[ft] = qa::measureGroundDrag(sampled.track[ft], 0.012f, 0.15);
        s.legacy[ft] = qa::measureGroundDragPerSample(sampled.track[ft], 0.012f, 0.15);
        s.slideMm = std::max(s.slideMm, maxPlantedSlide(sampled.track[ft], slideT) * 1000.0);
    }

    if (!jumpAt.empty()) {
        s.seekMm = 0.0;
        for (std::size_t j = 0; j < jumpAt.size(); ++j) {
            motion::DancePlayer jumper;
            configurePlayer(jumper, rig, o.dances, overlay);
            if (!jumper.selectClip(clip, o.from, true)) {
                throw std::runtime_error("clip not found in the library: " + clip);
            }
            // Start from the replay's first time so the jump goes through the seek path.
            jumper.update(o.from);
            const double t = o.from + jumpAt[j] / fps;
            (void)jumper.update(t);
            const double settle = o.from + (jumpAt[j] + settleSteps) / fps;
            const std::vector<Vec3> nodes = nodePositions(
                rig.applyClip(jumper.update(settle).pose, settle), model.nodes.size());
            const std::vector<Vec3>& expected = sampled.seekNodes[jumpAt.size() + j];
            for (std::size_t n = 0; n < nodes.size(); ++n) {
                s.seekMm = std::max(s.seekMm, (nodes[n] - expected[n]).length() * 1000.0);
            }
        }
    }
    return s;
}

void printHeader() {
    std::printf("%-12s %-12s %5s %-5s %-6s | truth L/R (cm) | fps L/R (cm) | legacy L/R (cm) | slide(mm) ikErr(mm) @t(s) | seek(mm)\n",
                "model", "clip", "fps", "ovl", "spring");
}

void printScenario(const Scenario& s) {
    std::printf("%-12s %-12s %5.0f %-5s %-6s | %6.2f %6.2f | %6.2f %6.2f | %6.2f %6.2f | %8.2f %8.3f @%5.2f | ",
                s.model.c_str(), s.clip.c_str(), s.fps, s.overlay.c_str(), s.spring ? "on" : "off",
                s.truth[0].distance * 100.0, s.truth[1].distance * 100.0,
                s.sampled[0].distance * 100.0, s.sampled[1].distance * 100.0,
                s.legacy[0].distance * 100.0, s.legacy[1].distance * 100.0,
                s.slideMm, s.ikErrorMm, s.worstIkT);
    if (s.seekMm < 0.0) {
        std::printf("n/a\n");
    } else {
        std::printf("%.3f\n", s.seekMm);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (arg == "--model") o.models.push_back(next());
        else if (arg == "--dances") o.dances = next();
        else if (arg == "--json") o.json = next();
        else if (arg == "--clip") o.clips.push_back(next());
        else if (arg == "--from") o.from = std::stod(next());
        else if (arg == "--seconds") o.seconds = std::stod(next());
        else if (arg == "--seek-points") o.seekPoints = std::stoi(next());
        else if (arg == "--fps") {
            o.fps.clear();
            for (const std::string& item : splitList(next())) o.fps.push_back(std::stod(item));
        } else if (arg == "--overlay") o.overlays = splitList(next());
        else if (arg == "--spring") {
            o.springs.clear();
            for (const std::string& item : splitList(next())) o.springs.push_back(std::stoi(item));
        }
    }
    if (o.models.empty()) o.models.push_back("tests/fixtures/minimal.vrm");

    std::vector<Scenario> results;
    for (const std::filesystem::path& modelPath : o.models) {
        vrm::LoadResult loaded = vrm::VrmLoader::load(modelPath);
        if (!loaded.ok()) {
            std::printf("skip %s: %s\n", modelPath.string().c_str(), loaded.error.c_str());
            continue;
        }
        animation::HumanoidRetargeter rig(*loaded.model);
        if (!rig.valid()) {
            std::printf("skip %s: retargeter invalid (%s)\n", modelPath.string().c_str(), rig.error().c_str());
            continue;
        }
        const std::string modelName = modelPath.filename().string();
        motion::DancePlayer catalogue;
        catalogue.loadLibrary(o.dances);
        std::vector<std::string> clips = o.clips;
        if (clips.empty()) {
            for (const motion::HumanoidClip& clip : catalogue.library().clips()) clips.push_back(clip.id);
        }
        std::printf("model %s (%.2f m), %zu clips, %d-s replays from %.1f s\n", modelName.c_str(), loaded.model->height,
                    clips.size(), static_cast<int>(o.seconds), o.from);
        printHeader();
        for (const std::string& clip : clips) {
            for (double fps : o.fps) {
                for (const std::string& overlay : o.overlays) {
                    for (int spring : o.springs) {
                        try {
                            results.push_back(runScenario(modelName, *loaded.model, rig, o, clip, fps, overlay, spring != 0));
                        } catch (const std::exception& error) {
                            std::fprintf(stderr, "%s\n", error.what());
                            return 1;
                        }
                        printScenario(results.back());
                    }
                }
            }
        }
        std::printf("\n");
    }
    std::printf("mesh triangle intersections: not measured by this matrix (use bgmedia_motion_diag with the private model)\n");

    if (!o.json.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(o.json.parent_path(), ec);
        std::ofstream file(o.json, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "cannot write %s\n", o.json.string().c_str());
            return 1;
        }
        file << "{\n  \"tool\": \"bgmedia_qa_matrix\",\n  \"seconds\": " << o.seconds << ",\n  \"scenarios\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const Scenario& s = results[i];
            file << "    {\"model\":\"" << jsonEscape(s.model) << "\",\"clip\":\"" << jsonEscape(s.clip) << "\","
                 << "\"fps\":" << s.fps << ",\"overlay\":\"" << s.overlay << "\",\"spring\":" << (s.spring ? "true" : "false") << ','
                 << "\"samples\":" << s.samples << ','
                 << "\"truthDragLeftCm\":" << s.truth[0].distance * 100.0 << ','
                 << "\"truthDragRightCm\":" << s.truth[1].distance * 100.0 << ','
                 << "\"truthDragSecondsLeft\":" << s.truth[0].seconds << ','
                 << "\"truthDragSecondsRight\":" << s.truth[1].seconds << ','
                 << "\"fpsDragLeftCm\":" << s.sampled[0].distance * 100.0 << ','
                 << "\"fpsDragRightCm\":" << s.sampled[1].distance * 100.0 << ','
                 << "\"legacyDragLeftCm\":" << s.legacy[0].distance * 100.0 << ','
                 << "\"legacyDragRightCm\":" << s.legacy[1].distance * 100.0 << ','
                 << "\"slideMm\":" << s.slideMm << ','
                 << "\"ikErrorMm\":" << s.ikErrorMm << ','
                 << "\"worstIkT\":" << s.worstIkT << ','
                 << "\"seekMm\":" << s.seekMm << '}'
                 << (i + 1 < results.size() ? ",\n" : "\n");
        }
        file << "  ]\n}\n";
        std::printf("wrote %s\n", o.json.string().c_str());
    }
    return 0;
}
