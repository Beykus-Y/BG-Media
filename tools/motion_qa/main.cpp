// bgmedia_motion_qa: headless quality report for the BG Motion dance library.
//
//   bgmedia_motion_qa [--dances assets/dances] [--model models/Model1.vrm]
//                     [--json docs/motion_qa_3_2.json] [--no-generated]
//
// Prints, per clip and per BPM, the numbers the unit tests judge (loop seam,
// angular speed / acceleration, foot slide on the retargeted ankle, reach
// deficit) and the worst frame-to-frame jump of every style-to-style
// transition. With a VRM it also reports secondary-dynamics stability at
// 30/60/120/180 FPS for the generated showcase.

#include "../../tests/motion_metrics.hpp"

#include "bgmedia/animation/SecondaryDynamics.hpp"
#include "bgmedia/motion3/ProceduralChoreography.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace bgmedia;

namespace {

struct SpringReport {
    std::string clip;
    double fps = 0.0;
    float maxDisplacement = 0.0f;
    float maxStretch = 0.0f;
    double maxSimulateMs = 0.0;
    bool finite = true;
    bool nonFinite = false;
    int substeps = 0;
    int chains = 0;
    int bones = 0;
};

std::string jsonEscape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path dances = "assets/dances";
    std::filesystem::path modelPath = "models/Model1.vrm";
    std::filesystem::path jsonPath;
    bool includeGenerated = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dances" && i + 1 < argc) dances = argv[++i];
        else if (arg == "--model" && i + 1 < argc) modelPath = argv[++i];
        else if (arg == "--json" && i + 1 < argc) jsonPath = argv[++i];
        else if (arg == "--no-generated") includeGenerated = false;
    }
    motion::DanceLibrary library;
    library.loadDirectory(dances);
    for (const auto& d : library.diagnostics()) {
        std::printf("%s %s: %s\n",
                    d.severity == motion::ClipDiagnostic::Severity::Error ? "ERROR  " : "warning",
                    d.file.c_str(), d.message.c_str());
    }
    std::vector<motion::GeneratedDance> generated;
    if (includeGenerated) {
        generated = motion::ProceduralChoreography::showcase();
    }
    std::vector<const motion::HumanoidClip*> clips;
    for (const auto& clip : library.clips()) {
        clips.push_back(&clip);
    }
    for (const auto& dance : generated) {
        clips.push_back(&dance.clip);
    }
    std::printf("%zu authored + %zu generated clips\n\n", library.clips().size(),
                generated.size());

    vrm::LoadResult loaded = vrm::VrmLoader::load(modelPath);
    std::unique_ptr<animation::HumanoidRetargeter> rig;
    if (loaded.ok()) {
        rig = std::make_unique<animation::HumanoidRetargeter>(*loaded.model);
        std::printf("model: %s (%.2f m, fingers %d, spring chains %d)\n\n",
                    modelPath.string().c_str(), loaded.model->height,
                    loaded.model->fingerBoneCount(),
                    static_cast<int>(loaded.model->springChains.size()));
    } else {
        std::printf("model not loaded (%s): skipping retargeted metrics\n\n", loaded.error.c_str());
    }

    std::vector<qa::ClipReport> reports;
    reports.reserve(clips.size());
    std::printf("%-14s %5s | seamRot seamPos seamV | maxW(deg/s) bone        maxA(deg/s2) | "
                "hipsV footV | slide(mm) runs footErr(mm) reachDrop(cm) | swings swingMax pathLen | "
                "root yaw\n", "clip", "bpm");
    for (const motion::HumanoidClip* clip : clips) {
        const double bpms[] = {clip->bpm * 0.8, clip->bpm, clip->bpm * 1.25};
        for (double bpm : bpms) {
            const qa::ClipReport r =
                qa::analyseClip(*clip, loaded.ok() ? loaded.model.get() : nullptr, rig.get(), bpm);
            if (bpm == clip->bpm) reports.push_back(r);
            std::printf("%-14s %5.0f | %6.3f %6.4f %6.1f | %10.0f %-12s %10.0f | %5.2f %5.2f | "
                        "%8.2f(b%.2f%c) %4d %9.2f %12.2f | %5d %8.3f %7.2f | %4.2f %4.0f\n",
                        clip->id.c_str(), bpm, r.loopRotationDeg, r.loopPositionHeight,
                        r.loopVelocityJumpDegPerSec, r.maxAngularSpeed,
                        std::string(vrm::humanoidBoneName(r.fastestBone)).c_str(),
                        r.maxAngularAcceleration, r.maxHipsSpeed, r.maxFootSpeed,
                        r.slideMax * 1000.0, r.slideBeat, r.slideFoot ? 'R' : 'L',
                        r.stanceRuns, r.footErrorMax * 1000.0, r.hipsDropMax * 100.0,
                        r.swings, r.swingDistanceMax, r.footPathLength,
                        r.maxRootExcursion, r.maxRootYawDeg);
        }
    }

    std::printf("\ntransitions (switch requested mid-beat; worst frame-to-frame values, 240 Hz)\n");
    std::printf("%-10s -> %-10s %5s | maxW(deg/s) maxA(deg/s2) hipsStep rootStep footStep\n", "from",
                "to", "bpm");
    static constexpr motion::DanceStyle kStyles[] = {
        motion::DanceStyle::Idle, motion::DanceStyle::Groove, motion::DanceStyle::HipHop,
        motion::DanceStyle::SideStep, motion::DanceStyle::Energetic};
    for (double bpm : {80.0, 123.0, 180.0}) {
        for (motion::DanceStyle from : kStyles) {
            for (motion::DanceStyle to : kStyles) {
                if (from == to) continue;
                motion::DanceDirector director(&library);
                director.setBeatClock(motion::BeatClock::constant(bpm, 0.0));
                director.reset(0.0);
                director.select(from, 0.0, false, true);
                const double switchAt = 60.0 / bpm * 9.37;  // deliberately off the beat
                director.select(to, switchAt);
                const qa::JumpReport j =
                    qa::measureWindow(director, switchAt - 0.5, switchAt + 4.0, 240.0);
                std::printf("%-10s -> %-10s %5.0f | %10.0f %12.0f %8.5f %8.5f %8.5f%s\n",
                            motion::styleName(from), motion::styleName(to), bpm,
                            j.maxAngularSpeed, j.maxAngularAcceleration, j.maxHipsStep,
                            j.maxRootStep, j.maxFootStep, j.finite ? "" : "  NON-FINITE");
            }
        }
    }

    std::vector<SpringReport> springReports;
    double compositionMicroseconds = 0.0;
    if (!generated.empty()) {
        const auto& dance = generated.front();
        motion::ClipPose base = motion::sampleClip(dance.clip, 4.0);
        const auto started = std::chrono::steady_clock::now();
        constexpr int kIterations = 2000;
        for (int i = 0; i < kIterations; ++i) {
            const double beat = 4.0 + (i % 64) * 0.0625;
            motion::ComposedPose composed = motion::composeMotion(
                base, dance.overlays, beat, dance.clip.beats, dance.clip.loop);
            base.accent = composed.pose.accent;
        }
        const double elapsed =
            std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - started).count();
        compositionMicroseconds = elapsed / kIterations;
        std::printf("\nmotion composition: %.2f us/frame (%d cues, %d iterations)\n",
                    compositionMicroseconds,
                    static_cast<int>(dance.overlays.size()), kIterations);
    }
    if (loaded.ok() && !generated.empty()) {
        std::printf("\nsecondary dynamics (generated showcase, 3 s each)\n");
        std::printf("%-14s %5s | maxDisp(cm) maxStretch %%  simMs  substeps  finite\n",
                    "clip", "fps");
        for (const motion::GeneratedDance& dance : generated) {
            animation::SecondaryDynamics dynamics(*loaded.model);
            if (!dynamics.available()) {
                break;
            }
            for (double fps : {30.0, 60.0, 120.0, 180.0}) {
                animation::SecondarySettings spring;
                spring.enabled = true;
                spring.intensity = 1.0f;
                spring.damping = 0.5f;
                dynamics.reset(rig->applyClip(motion::sampleClip(dance.clip, 0.0), 0.0));
                SpringReport report;
                report.clip = dance.clip.id;
                report.fps = fps;
                report.chains = dynamics.chainCount();
                report.bones = dynamics.boneCount();
                const int frames = static_cast<int>(3.0 * fps);
                bool finite = true;
                for (int frame = 0; frame <= frames; ++frame) {
                    const double t = frame / fps;
                    animation::RetargetedPose pose = rig->applyClip(
                        motion::sampleClip(dance.clip, t * dance.clip.bpm / 60.0), t);
                    dynamics.update(pose, frame == 0 ? 0.0 : 1.0 / fps, spring);
                    const auto& stats = dynamics.stats();
                    report.maxDisplacement =
                        std::max(report.maxDisplacement, stats.maxDisplacement);
                    report.maxStretch = std::max(report.maxStretch, stats.maxStretch);
                    report.maxSimulateMs =
                        std::max(report.maxSimulateMs, stats.simulateMilliseconds);
                    report.substeps = std::max(report.substeps, stats.substeps);
                    report.nonFinite = report.nonFinite || stats.nonFinite;
                    for (const auto& matrix : pose.nodeGlobals) {
                        finite = finite && matrix.finite();
                    }
                }
                report.finite = finite && !report.nonFinite;
                springReports.push_back(report);
                std::printf("%-14s %5.0f | %10.2f %11.3f %6.2f %9d  %s\n",
                            report.clip.c_str(), report.fps,
                            report.maxDisplacement * 100.0, report.maxStretch * 100.0,
                            report.maxSimulateMs, report.substeps,
                            report.finite ? "yes" : "NO");
            }
        }
    }

    if (!jsonPath.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(jsonPath.parent_path(), ec);
        std::ofstream file(jsonPath, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "cannot write %s\n", jsonPath.string().c_str());
            return 1;
        }
        std::ostringstream out;
        out << "{\n  \"tool\": \"bgmedia_motion_qa\",\n"
            << "  \"version\": \"3.2\",\n"
            << "  \"clips\": [\n";
        for (std::size_t i = 0; i < reports.size(); ++i) {
            const qa::ClipReport& r = reports[i];
            out << "    {\"id\":\"" << jsonEscape(r.id) << "\","
                << "\"loopRotationDeg\":" << r.loopRotationDeg << ','
                << "\"loopPositionHeight\":" << r.loopPositionHeight << ','
                << "\"loopVelocityJumpDegPerSec\":" << r.loopVelocityJumpDegPerSec << ','
                << "\"maxAngularSpeedDegPerSec\":" << r.maxAngularSpeed << ','
                << "\"maxAngularAccelerationDegPerSec2\":" << r.maxAngularAcceleration << ','
                << "\"fastestBone\":\"" << vrm::humanoidBoneName(r.fastestBone) << "\","
                << "\"maxHipsSpeed\":" << r.maxHipsSpeed << ','
                << "\"maxFootSpeed\":" << r.maxFootSpeed << ','
                << "\"slideMm\":" << r.slideMax * 1000.0 << ','
                << "\"stanceRuns\":" << r.stanceRuns << ','
                << "\"footErrorMm\":" << r.footErrorMax * 1000.0 << ','
                << "\"reachDropCm\":" << r.hipsDropMax * 100.0 << ','
                << "\"swings\":" << r.swings << ','
                << "\"swingMaxHeightUnits\":" << r.swingDistanceMax << ','
                << "\"footPathLength\":" << r.footPathLength << ','
                << "\"maxRootExcursion\":" << r.maxRootExcursion << ','
                << "\"maxRootYawDeg\":" << r.maxRootYawDeg << '}';
            out << (i + 1 < reports.size() ? ",\n" : "\n");
        }
        out << "  ],\n  \"compositionMicroseconds\": " << compositionMicroseconds
            << ",\n  \"spring\": [\n";
        for (std::size_t i = 0; i < springReports.size(); ++i) {
            const SpringReport& r = springReports[i];
            out << "    {\"clip\":\"" << jsonEscape(r.clip) << "\","
                << "\"fps\":" << r.fps << ','
                << "\"chains\":" << r.chains << ','
                << "\"bones\":" << r.bones << ','
                << "\"maxDisplacementCm\":" << r.maxDisplacement * 100.0 << ','
                << "\"maxStretchPercent\":" << r.maxStretch * 100.0 << ','
                << "\"maxSimulateMs\":" << r.maxSimulateMs << ','
                << "\"maxSubsteps\":" << r.substeps << ','
                << "\"finite\":" << (r.finite ? "true" : "false") << '}';
            out << (i + 1 < springReports.size() ? ",\n" : "\n");
        }
        out << "  ]\n}\n";
        file << out.str();
        std::printf("\nwrote %s\n", jsonPath.string().c_str());
    }
    return 0;
}
