// bgmedia_motion_qa: headless quality report for the BG Motion 3.0 dance library.
//
//   bgmedia_motion_qa [--dances assets/dances] [--model models/Model1.vrm]
//
// Prints, per clip and per BPM, the numbers the unit tests judge (loop seam,
// angular speed / acceleration, foot slide on the retargeted ankle, reach
// deficit) and the worst frame-to-frame jump of every style-to-style transition.

#include "../../tests/motion_metrics.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace bgmedia;

int main(int argc, char** argv) {
    std::filesystem::path dances = "assets/dances";
    std::filesystem::path modelPath = "models/Model1.vrm";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dances" && i + 1 < argc) dances = argv[++i];
        else if (arg == "--model" && i + 1 < argc) modelPath = argv[++i];
    }
    motion::DanceLibrary library;
    library.loadDirectory(dances);
    for (const auto& d : library.diagnostics()) {
        std::printf("%s %s: %s\n",
                    d.severity == motion::ClipDiagnostic::Severity::Error ? "ERROR  " : "warning",
                    d.file.c_str(), d.message.c_str());
    }
    std::printf("%zu clips\n\n", library.clips().size());

    vrm::LoadResult loaded = vrm::VrmLoader::load(modelPath);
    std::unique_ptr<animation::HumanoidRetargeter> rig;
    if (loaded.ok()) {
        rig = std::make_unique<animation::HumanoidRetargeter>(*loaded.model);
        std::printf("model: %s (%.2f m)\n\n", modelPath.string().c_str(), loaded.model->height);
    } else {
        std::printf("model not loaded (%s): skipping retargeted metrics\n\n", loaded.error.c_str());
    }

    std::printf("%-10s %5s | seamRot seamPos seamV | maxW(deg/s) bone        maxA(deg/s2) | "
                "hipsV footV | slide(mm) runs footErr(mm) reachDrop(cm) | swings swingMax pathLen | "
                "root yaw\n", "clip", "bpm");
    for (const motion::HumanoidClip& clip : library.clips()) {
        const double bpms[] = {clip.bpm * 0.8, clip.bpm, clip.bpm * 1.25};
        for (double bpm : bpms) {
            const qa::ClipReport r =
                qa::analyseClip(clip, loaded.ok() ? loaded.model.get() : nullptr, rig.get(), bpm);
            std::printf("%-10s %5.0f | %6.3f %6.4f %6.1f | %10.0f %-12s %10.0f | %5.2f %5.2f | "
                        "%8.2f(b%.2f%c) %4d %9.2f %12.2f | %5d %8.3f %7.2f | %4.2f %4.0f\n",
                        clip.id.c_str(), bpm, r.loopRotationDeg, r.loopPositionHeight,
                        r.loopVelocityJumpDegPerSec, r.maxAngularSpeed,
                        std::string(vrm::humanoidBoneName(r.fastestBone)).c_str(),
                        r.maxAngularAcceleration, r.maxHipsSpeed, r.maxFootSpeed,
                        r.slideMax * 1000.0, r.slideBeat, r.slideFoot ? 'R' : 'L', r.stanceRuns, r.footErrorMax * 1000.0,
                        r.hipsDropMax * 100.0, r.swings, r.swingDistanceMax, r.footPathLength,
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
    return 0;
}
