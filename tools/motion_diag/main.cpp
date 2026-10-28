#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
// bgmedia_motion_diag: instrumented playback of a model + real track through the same
// path the Desktop app uses (BG Beat analysis -> DancePlayer -> HumanoidRetargeter ->
// SecondaryDynamics), with motion-spike attribution, foot contact / slide / drag
// metrics and mesh-level leg and arm crossings.
//
//   bgmedia_motion_diag --model M.vrm --audio T.mp3 --clip fluidSignal [--mode manual|auto]
//       [--overlay off|light|full|hands] [--warp off|soft|expressive] [--spring 0|1]
//       [--thigh-colliders 0|1] [--fps 60] [--from 0] [--to 30] [--select-at 0] [--immediate 0|1]
//       [--mesh-hz 15] [--csv out.csv] [--focus 1.16] [--seek 1.16]
//
// Playback is always sequential from `--from`, one fresh DancePlayer per run (a reused
// player keeps its first schedule and a backward/jumping update() seeks and re-aligns it,
// both of which change the pose). `--seek` additionally evaluates that time by a direct
// jump on a second player so the two poses can be compared.

#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/audio/MiniaudioDecoder.hpp"
#include "bgmedia/cache/AnalysisCache.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "bgmedia/vrm/ThighColliders.hpp"
#include "support/MeshProbe.hpp"
#include "support/PathUtf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace bgmedia;
using math::Vec3;
namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path model, audio, csv;
    fs::path dances = "assets/dances";
    std::string clip = "fluidSignal";
    std::string mode = "manual", overlay = "full", warp = "off";
    bool spring = false, immediate = false;
    bool thighColliders = false;  // BG Motion 3.3 opt-in thigh/hip spring colliders
    double fps = 60.0, from = 0.0, to = 30.0, selectAt = 0.0, mesh_hz = 15.0, focus = -1.0, seek = -1.0;
    bool legMesh = true, armMesh = true;
    bool legProxy = false, armProxy = true;
    float footworkGap = 2.5f;
    std::string footwork = "model";  // off | ref | model (authored footwork pre-pass)
    double skip = 0.0;  // ignore the first seconds in the summaries (clip fade-in transient)
};

constexpr int kTracked = 11;
const vrm::HumanoidBone kJoints[kTracked] = {
    vrm::HumanoidBone::LeftHand,     vrm::HumanoidBone::RightHand,    vrm::HumanoidBone::LeftLowerArm,
    vrm::HumanoidBone::RightLowerArm, vrm::HumanoidBone::Head,         vrm::HumanoidBone::Hips,
    vrm::HumanoidBone::LeftLowerLeg, vrm::HumanoidBone::RightLowerLeg, vrm::HumanoidBone::LeftFoot,
    vrm::HumanoidBone::RightFoot,    vrm::HumanoidBone::Chest};
const char* kJointNames[kTracked] = {"LHand", "RHand", "LElbow", "RElbow", "Head", "Hips", "LKnee", "RKnee", "LAnkle", "RAnkle", "Chest"};

struct Frame {
    double t = 0;
    std::string clipId;
    float phase = 0, transition = -1;
    double clipBeat = 0, musicBeat = 0, warpRate = 1;
    int overlayActive = 0, overlayRejected = 0;
    float overlayWeight = 0;
    Vec3 pos[kTracked];                  // stage-space joint positions
    math::Quat localRot[kTracked];       // local rotation of the tracked bones
    // clip-level quantities (before retargeting)
    std::array<math::Quat, vrm::kHumanoidBoneCount> clipBone;
    Vec3 footTarget[2];
    float contact[2] = {0, 0};
    motion::FootPhase footPhase[2] = {motion::FootPhase::Stance, motion::FootPhase::Stance};
    float rootX = 0, rootZ = 0, rootYaw = 0;
    Vec3 hipsOffset;
    bool planted[2] = {false, false};
    float springDisp = 0;
    int legCross = -1, armCross = -1;    // -1 = not sampled
    float ikError = 0;
    float legCorr = 0, legClear = 0;
    bool legConflict = false;
};

double angleBetween(const math::Quat& a, const math::Quat& b) {
    const float d = std::abs(math::dot(a, b));
    return 2.0 * std::acos(std::clamp(d, 0.0f, 1.0f));
}

std::string primitivesAt(motion::DancePlayer& player, const motion::DanceFrame& frame) {
    const motion::HumanoidClip* clip = player.library().find(frame.clipId);
    if (!clip) return "-";
    std::string out;
    for (const motion::PrimitiveCue& cue : player.overlaysFor(frame.clipId)) {
        double local = frame.clipBeat - cue.beat;
        if (clip->loop) {
            local = std::fmod(local, clip->beats);
            if (local < 0) local += clip->beats;
        }
        if (local >= 0 && local <= cue.durationBeats) {
            if (!out.empty()) out += ",";
            out += std::string(motion::primitiveMetadata(cue.kind).id) + (cue.side == 0 ? "L" : cue.side == 1 ? "R" : "");
        }
    }
    return out.empty() ? "-" : out;
}

}  // namespace

int main(int argc, char** argv) {
    test::Utf8CommandLine arguments(argc, argv);
    Options o;
    for (int i = 1; i < arguments.count; ++i) {
        const std::string a = arguments.values[i];
        const auto next = [&]() -> std::string { return i + 1 < arguments.count ? std::string(arguments.values[++i]) : std::string(); };
        if (a == "--model") o.model = fs::u8path(next());
        else if (a == "--audio") o.audio = fs::u8path(next());
        else if (a == "--csv") o.csv = fs::u8path(next());
        else if (a == "--dances") o.dances = fs::u8path(next());
        else if (a == "--clip") o.clip = next();
        else if (a == "--mode") o.mode = next();
        else if (a == "--overlay") o.overlay = next();
        else if (a == "--warp") o.warp = next();
        else if (a == "--spring") o.spring = std::atoi(next().c_str()) != 0;
        else if (a == "--thigh-colliders") o.thighColliders = std::atoi(next().c_str()) != 0;
        else if (a == "--immediate") o.immediate = std::atoi(next().c_str()) != 0;
        else if (a == "--fps") o.fps = std::atof(next().c_str());
        else if (a == "--from") o.from = std::atof(next().c_str());
        else if (a == "--to") o.to = std::atof(next().c_str());
        else if (a == "--select-at") o.selectAt = std::atof(next().c_str());
        else if (a == "--mesh-hz") o.mesh_hz = std::atof(next().c_str());
        else if (a == "--focus") o.focus = std::atof(next().c_str());
        else if (a == "--seek") o.seek = std::atof(next().c_str());
        else if (a == "--no-leg-proxy") o.legProxy = false;
        else if (a == "--leg-proxy") o.legProxy = true;
        else if (a == "--footwork") o.footwork = next();
        else if (a == "--footwork-gap") o.footworkGap = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--no-arm-proxy") o.armProxy = false;
        else if (a == "--skip") o.skip = std::atof(next().c_str());
        else if (a == "--no-leg-mesh") o.legMesh = false;
        else if (a == "--no-arm-mesh") o.armMesh = false;
    }
    vrm::LoadResult loaded = vrm::VrmLoader::load(o.model);
    if (!loaded.ok()) {
        std::fprintf(stderr, "model load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    if (o.thighColliders) {
        // Must run before SecondaryDynamics is constructed: the collider groups are read at build time.
        vrm::ThighColliderOptions thigh;
        thigh.enabled = true;
        const vrm::ThighColliderReport report = vrm::synthesizeThighColliders(*loaded.model, thigh);
        std::printf("thigh colliders: %s (groups %d, spheres %d, chains %d, radius L %.4f R %.4f m%s)\n",
                    report.applied ? "added" : report.skipReason.c_str(), report.groupsAdded,
                    report.spheresAdded, report.chainsTouched, report.leftRadius, report.rightRadius,
                    report.applied && (report.leftRadiusFromMesh || report.rightRadiusFromMesh)
                        ? ", mesh-fitted" : "");
    }
    const vrm::VrmModel& model = *loaded.model;
    animation::HumanoidRetargeter rig(model);
    if (!rig.valid()) {
        std::fprintf(stderr, "retargeter invalid: %s\n", rig.error().c_str());
        return 1;
    }

    // --- BG Beat analysis of the real track (cache lives in an ignored directory) ---------
    AnalysisResult analysis;
    bool haveAudio = false;
    if (!o.audio.empty()) {
        MiniaudioDecoder decoder;
        const DecodeResult decoded = decoder.decode(o.audio);
        if (!decoded.ok) {
            std::fprintf(stderr, "decode failed: %s\n", decoded.error.c_str());
            return 1;
        }
        std::string perr;
        const PcmBuffer pcm = PcmBuffer::fromAudio(decoded.audio, &perr);
        AnalysisConfig config;
        Analyzer analyzer(config);
        AnalysisCache cache(".bgmedia_cache");
        CacheLoadResult hit = cache.load(pcm, analyzer.config());
        if (hit.hit()) {
            analysis = std::move(hit.result);
        } else {
            AnalysisReport report = analyzer.analyze(pcm);
            if (!report.ok()) {
                std::fprintf(stderr, "analysis failed\n");
                return 1;
            }
            analysis = std::move(report.result);
            (void)cache.store(analysis, pcm, analyzer.config(), nullptr);
        }
        haveAudio = true;
        std::printf("track: %.1f s, %.2f BPM (confidence %.2f), %zu beats, meter %d, downbeat confidence %.2f\n",
                    analysis.info.duration, analysis.bpm, analysis.bpmConfidence, analysis.beats.size(), analysis.beatsPerBar,
                    analysis.downbeatConfidence);
    }

    const auto makePlayer = [&](motion::DancePlayer& player) {
        player.loadLibrary(o.dances);
        if (o.footwork == "off") player.setFootworkPlanning(false);
        else if (o.footwork == "model") player.setLegGeometry(rig.legGeometry());
        if (o.footwork != "off") player.setFootworkGap(o.footworkGap);
        player.setOverlayMode(o.overlay == "off" ? motion::OverlayMode::Off : o.overlay == "light" ? motion::OverlayMode::Light
                              : o.overlay == "hands" ? motion::OverlayMode::HandsOnly : motion::OverlayMode::Full);
        player.setExpression(o.warp == "soft" ? motion::MotionExpression::Soft : o.warp == "expressive"
                                 ? motion::MotionExpression::Expressive : motion::MotionExpression::Off);
        if (haveAudio) player.setAnalysis(&analysis, 0.0);
        if (o.mode == "auto") player.setMode(motion::DanceMode::Auto, 0.0);
    };
    const auto selectClip = [&](motion::DancePlayer& player, double now) {
        if (o.mode == "auto") return true;
        // The Desktop selects with `now + 0.10` and, with a track loaded, not immediately: the clip
        // fades in on the next beat from whatever was dancing before.
        return player.selectClip(o.clip, now + (haveAudio ? 0.10 : 0.0), o.immediate || !haveAudio);
    };

    motion::DancePlayer player;
    makePlayer(player);
    if (const motion::FootworkPlanReport* fw = player.footworkReport(o.clip)) {
        std::printf("FOOTWORK PLAN (%s): %d stances, %d moved; violation keys %d -> %d, min leg clearance %.2f -> %.2f cm; "
                    "max landing shift %.1f cm, side bend %.1f cm, extra lift %.1f cm, unresolved %d\n",
                    o.footwork.c_str(), fw->stances, fw->stancesMoved, fw->violationKeysBefore, fw->violationKeysAfter,
                    fw->minClearanceBeforeCm, fw->minClearanceAfterCm, fw->maxLandingShiftCm, fw->maxSideBendCm,
                    fw->maxExtraLiftCm, fw->unresolved);
        if (!fw->unresolvedKeys.empty()) {
            std::printf("  unresolved clip beats:");
            for (int k : fw->unresolvedKeys) std::printf(" %.2f", k / 16.0);
            std::printf("\n");
        }
    }
    bool selected = o.mode == "auto";
    animation::SecondaryDynamics spring(model);
    animation::SecondarySettings springSettings;
    springSettings.enabled = o.spring;

    probe::MeshProbe meshProbe(model);
    if (o.legMesh || o.armMesh) {
        meshProbe.skin(rig.applyClip(motion::neutralClipPose(), 0.0));
        meshProbe.maskCurrentAsRest(probe::Region::LegL, probe::Region::LegR);
        meshProbe.maskCurrentAsRest(probe::Region::LegR, probe::Region::LegL);
        meshProbe.maskCurrentAsRest(probe::Region::ArmL, probe::Region::Torso);
        meshProbe.maskCurrentAsRest(probe::Region::ArmR, probe::Region::Torso);
        std::printf("model %s: height %.3f m | leg triangles L %zu R %zu | arm triangles L %zu R %zu | torso %zu\n",
                    o.model.filename().string().c_str(), model.height, meshProbe.triangleCount(probe::Region::LegL),
                    meshProbe.triangleCount(probe::Region::LegR), meshProbe.triangleCount(probe::Region::ArmL),
                    meshProbe.triangleCount(probe::Region::ArmR), meshProbe.triangleCount(probe::Region::Torso));
    }

    std::vector<Frame> frames;
    const double dt = 1.0 / o.fps;
    const int total = static_cast<int>((o.to - o.from) * o.fps);
    const int meshEvery = std::max(1, static_cast<int>(o.fps / std::max(1.0, o.mesh_hz)));
    double lastMedia = -1e9;
    for (int f = 0; f < total; ++f) {
        const double t = o.from + f * dt;
        if (!selected && t >= o.selectAt) {
            if (!selectClip(player, t)) {
                std::fprintf(stderr, "clip '%s' not found\n", o.clip.c_str());
                return 1;
            }
            selected = true;
        }
        const motion::DanceFrame df = player.update(t);
        animation::RetargetOptions ropt;
        ropt.legCollision = o.legProxy;
        ropt.armCollision = o.armProxy;
        animation::RetargetedPose pose = rig.applyClip(df.pose, t, ropt);
        if (o.spring) {
            if (f == 0) spring.reset(pose);
            spring.update(pose, f == 0 ? 0.0 : t - lastMedia, springSettings);
        }
        lastMedia = t;
        Frame fr;
        fr.t = t;
        fr.clipId = df.clipId;
        fr.phase = df.phase;
        fr.transition = df.transitionProgress;
        fr.clipBeat = df.clipBeat;
        fr.musicBeat = df.musicBeat;
        fr.warpRate = df.warpRate;
        fr.overlayActive = df.overlayActive;
        fr.overlayRejected = df.overlayRejected;
        fr.overlayWeight = df.overlayWeight;
        for (int k = 0; k < kTracked; ++k) {
            const int node = model.nodeFor(kJoints[k]);
            fr.pos[k] = animation::HumanoidRetargeter::stagePosition(pose, node);
            const int parent = model.nodes[static_cast<std::size_t>(node)].parent;
            const math::Quat g = pose.nodeGlobals[static_cast<std::size_t>(node)].rotationQuat();
            const math::Quat pg = parent >= 0 ? pose.nodeGlobals[static_cast<std::size_t>(parent)].rotationQuat() : math::Quat::identity();
            fr.localRot[k] = (pg.conjugate() * g).normalized();
        }
        fr.clipBone = df.pose.bone;
        for (int ft = 0; ft < 2; ++ft) {
            const auto& foot = df.pose.foot[static_cast<std::size_t>(ft)];
            fr.footTarget[ft] = foot.position;
            fr.contact[ft] = foot.contact;
            fr.footPhase[ft] = foot.phase;
            fr.planted[ft] = (foot.phase == motion::FootPhase::Stance || foot.phase == motion::FootPhase::Pivot) && foot.contact > 0.999f;
        }
        fr.rootX = df.pose.rootX;
        fr.rootZ = df.pose.rootZ;
        fr.rootYaw = df.pose.rootYaw;
        fr.hipsOffset = df.pose.hips;
        fr.springDisp = spring.stats().maxDisplacement;
        fr.ikError = pose.stats.maxFootError;
        fr.legCorr = pose.stats.legCorrection;
        fr.legClear = pose.stats.legClearanceRaw;
        fr.legConflict = pose.stats.legConflictPlanted;
        if (f % meshEvery == 0) {
            meshProbe.skin(pose);
            if (o.legMesh) fr.legCross = static_cast<int>(meshProbe.crossings(probe::Region::LegL, probe::Region::LegR).size());
            if (o.armMesh)
                fr.armCross = static_cast<int>(meshProbe.crossings(probe::Region::ArmL, probe::Region::Torso).size() +
                                               meshProbe.crossings(probe::Region::ArmR, probe::Region::Torso).size());
        }
        frames.push_back(std::move(fr));
    }
    std::printf("played %zu frames @%.0f FPS from %.2f s (clip %s, mode %s, overlay %s, warp %s, spring %s, %s)\n", frames.size(),
                o.fps, o.from, o.clip.c_str(), o.mode.c_str(), o.overlay.c_str(), o.warp.c_str(), o.spring ? "on" : "off",
                o.immediate || !haveAudio ? "immediate" : "crossfade from the previous clip on the next beat");

    // --- derivatives ------------------------------------------------------------------------
    const std::size_t n = frames.size();
    const std::size_t skipIdx = std::min(n, static_cast<std::size_t>(std::max(0.0, o.skip - o.from) * o.fps));
    struct Series {
        std::vector<double> speed, accel, jerk, angSpeed, angAccel;
    };
    Series series[kTracked];
    for (int k = 0; k < kTracked; ++k) {
        Series& s = series[k];
        s.speed.assign(n, 0);
        s.accel.assign(n, 0);
        s.jerk.assign(n, 0);
        s.angSpeed.assign(n, 0);
        s.angAccel.assign(n, 0);
        for (std::size_t i = 1; i < n; ++i) {
            s.speed[i] = (frames[i].pos[k] - frames[i - 1].pos[k]).length() / dt;
            s.angSpeed[i] = angleBetween(frames[i].localRot[k], frames[i - 1].localRot[k]) / dt;
        }
        for (std::size_t i = 2; i < n; ++i) {
            const Vec3 a1 = (frames[i].pos[k] - frames[i - 1].pos[k]) * static_cast<float>(1.0 / dt);
            const Vec3 a0 = (frames[i - 1].pos[k] - frames[i - 2].pos[k]) * static_cast<float>(1.0 / dt);
            s.accel[i] = (a1 - a0).length() / dt;
            s.angAccel[i] = std::abs(s.angSpeed[i] - s.angSpeed[i - 1]) / dt;
        }
        for (std::size_t i = 3; i < n; ++i) s.jerk[i] = std::abs(s.accel[i] - s.accel[i - 1]) / dt;
    }

    std::printf("\n%-8s %10s %12s %14s %14s %16s\n", "joint", "max m/s", "max m/s^2", "max jerk m/s^3", "max rad/s", "max rad/s^2");
    for (int k = 0; k < kTracked; ++k) {
        const Series& s = series[k];
        const auto mx = [&](const std::vector<double>& v) { return skipIdx < v.size() ? *std::max_element(v.begin() + static_cast<std::ptrdiff_t>(skipIdx), v.end()) : 0.0; };
        std::printf("%-8s %10.2f %12.1f %14.0f %14.1f %16.0f\n", kJointNames[k], mx(s.speed), mx(s.accel), mx(s.jerk), mx(s.angSpeed), mx(s.angAccel));
    }

    // --- spike events with attribution --------------------------------------------------------
    struct Event {
        std::size_t i;
        int k;
        double score;
    };
    std::vector<Event> events;
    for (int k = 0; k < kTracked; ++k) {
        for (std::size_t i = std::max<std::size_t>(3, skipIdx); i + 1 < n; ++i) {
            const double a = series[k].accel[i];
            if (a > 150.0 && a >= series[k].accel[i - 1] && a >= series[k].accel[i + 1]) events.push_back({i, k, a});
        }
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.score > b.score; });
    // keep the top event per time window of 0.25 s
    std::vector<Event> kept;
    for (const Event& e : events) {
        bool nearby = false;
        for (const Event& k : kept) nearby = nearby || std::abs(frames[k.i].t - frames[e.i].t) < 0.25;
        if (!nearby) kept.push_back(e);
        if (kept.size() >= 12) break;
    }
    std::printf("\nSPIKES (joint acceleration > 150 m/s^2, strongest per 0.25 s):\n");
    for (const Event& e : kept) {
        const Frame& cur = frames[e.i];
        const Frame& prev = frames[e.i - 1];
        // largest clip-level bone rotation step in this frame (before retargeting)
        double bestStep = 0;
        int bestBone = -1;
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            const double st = angleBetween(cur.clipBone[b], prev.clipBone[b]);
            if (st > bestStep) {
                bestStep = st;
                bestBone = static_cast<int>(b);
            }
        }
        const float footStep = std::max((cur.footTarget[0] - prev.footTarget[0]).length(), (cur.footTarget[1] - prev.footTarget[1]).length());
        const float rootStep = std::hypot(cur.rootX - prev.rootX, cur.rootZ - prev.rootZ);
        const float hipsStep = (cur.hipsOffset - prev.hipsOffset).length();
        std::printf("  t=%7.3f s %-7s accel %7.1f m/s^2 speed %5.2f m/s ang %6.1f rad/s | clip=%s phase %.3f beat %.2f music-beat %.2f "
                    "transition %.2f warpRate %.2f(d%+.2f)\n",
                    cur.t, kJointNames[e.k], e.score, series[e.k].speed[e.i], series[e.k].angSpeed[e.i], cur.clipId.c_str(), cur.phase,
                    cur.clipBeat, cur.musicBeat, cur.transition, cur.warpRate, cur.warpRate - prev.warpRate);
        std::printf("           clip-level: max bone step %.3f rad (%s) | foot-target step %.1f cm | root step %.1f cm, hips step %.1f cm | "
                    "overlay %d (w %.2f, d%+.2f) | clip switch %s | spring %.1f mm | primitives: %s\n",
                    bestStep, bestBone >= 0 ? vrm::humanoidBoneName(static_cast<vrm::HumanoidBone>(bestBone)).data() : "-", footStep * model.height * 100.0f,
                    rootStep * model.height * 100.0f, hipsStep * model.height * 100.0f, cur.overlayActive, cur.overlayWeight,
                    cur.overlayWeight - prev.overlayWeight, cur.clipId != prev.clipId ? "YES" : "no", cur.springDisp * 1000.0f, "see csv");
    }

    // --- feet: contact, slide, ground drag, crossings ---------------------------------------
    const float h = model.height;
    for (int ft = 0; ft < 2; ++ft) {
        const int ankleK = ft == 0 ? 8 : 9;
        double maxSlide = 0, dragDist = 0, dragTime = 0, maxHorizSpeedPlanted = 0;
        std::size_t runStart = 0;
        bool inRun = false;
        for (std::size_t i = skipIdx; i < n; ++i) {
            const bool planted = frames[i].planted[ft];
            if (planted && !inRun) {
                inRun = true;
                runStart = i;
            }
            if (planted) {
                const Vec3 d = frames[i].pos[ankleK] - frames[runStart].pos[ankleK];
                maxSlide = std::max<double>(maxSlide, std::hypot(d.x, d.z));
            }
            if (!planted) inRun = false;
            if (i == skipIdx) continue;
            const Vec3 d = frames[i].pos[ankleK] - frames[i - 1].pos[ankleK];
            const double horiz = std::hypot(d.x, d.z);
            if (!planted && frames[i].footTarget[ft].y < 0.012f && horiz / dt > 0.15) {
                dragDist += horiz;
                dragTime += dt;
            }
        }
        (void)maxHorizSpeedPlanted;
        std::printf("%s foot: max slide inside a planted run %.2f mm | GROUND DRAG (not planted, lift < %.1f cm, moving > 0.15 m/s): %.1f cm over %.2f s\n",
                    ft == 0 ? "left " : "right", maxSlide * 1000.0, 0.012 * h * 100.0, dragDist * 100.0, dragTime);
    }
    int legFrames = 0, legMax = 0, armFrames = 0, armMax = 0, sampled = 0;
    for (const Frame& f : frames) {
        if (f.t < o.skip) continue;
        if (f.legCross >= 0) {
            ++sampled;
            legFrames += f.legCross > 0;
            legMax = std::max(legMax, f.legCross);
        }
        if (f.armCross >= 0) {
            armFrames += f.armCross > 0;
            armMax = std::max(armMax, f.armCross);
        }
    }
    std::printf("MESH (sampled %d frames @%.0f Hz): leg-vs-leg triangle crossings in %d frames (max %d tris); arm-vs-torso crossings in %d frames (max %d tris)\n",
                sampled, o.mesh_hz, legFrames, legMax, armFrames, armMax);
    {
        double maxCorr = 0;
        int corrected = 0, conflicts = 0;
        for (const Frame& f : frames) {
            if (f.t < o.skip) continue;
            maxCorr = std::max<double>(maxCorr, f.legCorr);
            corrected += f.legCorr > 1e-5f;
            conflicts += f.legConflict;
        }
        std::printf("LEG PROXY: %d frames corrected, max foot-target move %.1f mm, %d frames with both feet planted and overlapping (not movable)\n",
                    corrected, maxCorr * 1000.0, conflicts);
    }
    // list leg-crossing time ranges
    {
        std::string ranges;
        double start = -1, last = -1;
        int peak = 0;
        const auto flush = [&]() {
            if (start >= 0) {
                char b[96];
                std::snprintf(b, sizeof b, " [%.2f-%.2f s peak %d]", start, last, peak);
                ranges += b;
            }
            start = -1;
            peak = 0;
        };
        for (const Frame& f : frames) {
            if (f.legCross > 0) {
                if (start < 0) start = f.t;
                last = f.t;
                peak = std::max(peak, f.legCross);
            } else if (f.legCross == 0) {
                if (start >= 0 && f.t - last > 0.2) flush();
            }
        }
        flush();
        if (!ranges.empty()) std::printf("  leg crossing ranges:%s\n", ranges.c_str());
    }

    // --- focus window: everything we know about one musical time ------------------------------
    if (o.focus >= 0) {
        std::size_t idx = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (std::abs(frames[i].t - o.focus) < std::abs(frames[idx].t - o.focus)) idx = i;
        const double bpm = analysis.bpm;
        std::printf("\nFOCUS t=%.3f s (track BPM %.2f)\n", frames[idx].t, bpm);
        for (std::size_t i = idx >= 6 ? idx - 6 : 0; i < std::min(n, idx + 7); i += 2) {
            const Frame& fr = frames[i];
            std::printf("  t=%.3f clip=%s phase=%.3f clipBeat=%.2f musicBeat=%.2f transition=%.2f warp=%.2f | support L:%s(%.2f,%s) R:%s(%.2f,%s) | "
                        "foot targets L(%.1f,%.1f,%.1f) R(%.1f,%.1f,%.1f) cm | root (%.1f,%.1f) yaw %.3f hips(%.1f,%.1f,%.1f) cm | "
                        "ankles L(%.2f,%.2f,%.2f) R(%.2f,%.2f,%.2f) m | legCross %d | ikErr %.2f mm\n",
                        fr.t, fr.clipId.c_str(), fr.phase, fr.clipBeat, fr.musicBeat, fr.transition, fr.warpRate,
                        fr.planted[0] ? "planted" : "free", fr.contact[0], motion::footPhaseName(fr.footPhase[0]),
                        fr.planted[1] ? "planted" : "free", fr.contact[1], motion::footPhaseName(fr.footPhase[1]),
                        fr.footTarget[0].x * h * 100, fr.footTarget[0].y * h * 100, fr.footTarget[0].z * h * 100,
                        fr.footTarget[1].x * h * 100, fr.footTarget[1].y * h * 100, fr.footTarget[1].z * h * 100, fr.rootX * h * 100,
                        fr.rootZ * h * 100, fr.rootYaw, fr.hipsOffset.x * h * 100, fr.hipsOffset.y * h * 100, fr.hipsOffset.z * h * 100,
                        fr.pos[8].x, fr.pos[8].y, fr.pos[8].z, fr.pos[9].x, fr.pos[9].y, fr.pos[9].z, fr.legCross, fr.ikError * 1000);
        }
    }

    // --- seek comparison -----------------------------------------------------------------------
    if (o.seek >= 0) {
        motion::DancePlayer jumper;
        makePlayer(jumper);
        bool sel = selectClip(jumper, std::min(o.selectAt, o.seek));
        (void)sel;
        const motion::DanceFrame jf = jumper.update(o.seek);
        const animation::RetargetedPose jp = rig.applyClip(jf.pose, o.seek);
        std::size_t idx = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (std::abs(frames[i].t - o.seek) < std::abs(frames[idx].t - o.seek)) idx = i;
        double worst = 0;
        for (int k = 0; k < kTracked; ++k) {
            const Vec3 p = animation::HumanoidRetargeter::stagePosition(jp, model.nodeFor(kJoints[k]));
            worst = std::max<double>(worst, (p - frames[idx].pos[k]).length());
        }
        std::printf("\nSEEK t=%.3f: sequential clip=%s phase %.3f beat %.2f | direct jump clip=%s phase %.3f beat %.2f | max joint position difference %.1f cm\n",
                    o.seek, frames[idx].clipId.c_str(), frames[idx].phase, frames[idx].clipBeat, jf.clipId.c_str(), jf.phase, jf.clipBeat, worst * 100.0);
    }

    if (!o.csv.empty()) {
        std::ofstream csv(o.csv);
        csv << "t,clip,phase,transition,clipBeat,musicBeat,warpRate,overlay,overlayW,plantedL,plantedR,legCross,armCross,ikErrMm,springMm";
        for (int k = 0; k < kTracked; ++k) csv << "," << kJointNames[k] << "_v," << kJointNames[k] << "_a," << kJointNames[k] << "_w";
        csv << "\n";
        for (std::size_t i = 0; i < n; ++i) {
            const Frame& f = frames[i];
            csv << f.t << "," << f.clipId << "," << f.phase << "," << f.transition << "," << f.clipBeat << "," << f.musicBeat << "," << f.warpRate
                << "," << f.overlayActive << "," << f.overlayWeight << "," << f.planted[0] << "," << f.planted[1] << "," << f.legCross << ","
                << f.armCross << "," << f.ikError * 1000 << "," << f.springDisp * 1000;
            for (int k = 0; k < kTracked; ++k) csv << "," << series[k].speed[i] << "," << series[k].accel[i] << "," << series[k].angSpeed[i];
            csv << "\n";
        }
    }
    return 0;
}
