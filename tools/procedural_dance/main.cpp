// bgmedia_procedural_dance: authoring and QA front-end for the BG Motion 3.2
// procedural choreography generator.
//
//   bgmedia_procedural_dance                  print the three showcase dances
//   bgmedia_procedural_dance --write <dir>    write them as clip JSON v2
//   bgmedia_procedural_dance --seed N --bars 8 --profile neon --bpm 104
//
// The tool never renders. It proves that generation is deterministic, that the
// baked clip passes the same validator the runtime uses, and that the overlay
// plan is populated.

#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/motion3/ProceduralChoreography.hpp"
#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace bgmedia;

namespace {

void printDance(const motion::GeneratedDance& dance) {
    const motion::ChoreographyStats& s = dance.stats;
    std::printf("%-14s %-18s bpm %6.1f beats %3d  phrases %d  steps %2d pivots %2d  "
                "accents %2d  cues %2d  bones %2d\n",
                dance.clip.id.c_str(), dance.clip.name.c_str(), dance.clip.bpm,
                s.beats, s.phrases, s.steps, s.pivots, s.accents, s.overlayCues,
                s.bonesUsed);
    std::printf("               maxW %8.1f deg/s  maxA %9.1f deg/s2  seam %.4f deg  "
                "slide %.5f h  gen %.2f ms  finite %s\n",
                s.maxAngularSpeedDegPerSec, s.maxAngularAccelerationDegPerSec2,
                s.loopSeamDeg, s.footSlideHeightUnits, s.generationMilliseconds,
                s.finite ? "yes" : "NO");
    for (const std::string& line : dance.structure) {
        std::printf("               %s\n", line.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path writeDir;
    std::filesystem::path modelPath;
    bool printFeet = false;
    bool single = false;
    motion::ChoreographyRequest request;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--write" && i + 1 < argc) {
            writeDir = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            request.seed = std::atoi(argv[++i]);
            single = true;
        } else if (arg == "--bars" && i + 1 < argc) {
            request.bars = std::atoi(argv[++i]);
        } else if (arg == "--bpm" && i + 1 < argc) {
            request.bpm = std::atof(argv[++i]);
        } else if (arg == "--profile" && i + 1 < argc) {
            request.profile = argv[++i];
        } else if (arg == "--intensity" && i + 1 < argc) {
            request.intensity = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--id" && i + 1 < argc) {
            request.id = argv[++i];
            single = true;
        } else if (arg == "--model" && i + 1 < argc) {
            modelPath = argv[++i];
        } else if (arg == "--print-feet") {
            printFeet = true;
        }
    }
    if (single) {
        request.name = request.id;
        request.style = motion::DanceStyle::Custom;
    }

    std::vector<motion::GeneratedDance> dances =
        single ? std::vector<motion::GeneratedDance>{motion::ProceduralChoreography::generate(request)}
               : motion::ProceduralChoreography::showcase();

    std::unique_ptr<vrm::VrmModel> model;
    std::unique_ptr<animation::HumanoidRetargeter> rig;
    if (printFeet) {
        vrm::LoadResult loaded = vrm::VrmLoader::load(modelPath);
        if (loaded.ok()) {
            model = std::move(loaded.model);
            rig = std::make_unique<animation::HumanoidRetargeter>(*model);
            if (!rig->valid()) {
                rig.reset();
            }
        } else {
            std::printf("model load failed: %s\n", loaded.error.c_str());
        }
    }

    int exitCode = 0;
    for (const motion::GeneratedDance& dance : dances) {
        std::printf("\n=== %s ===\n", dance.clip.id.c_str());
        printDance(dance);
        if (rig) {
            const int left = model->nodeFor(vrm::HumanoidBone::LeftFoot);
            const int right = model->nodeFor(vrm::HumanoidBone::RightFoot);
            std::printf("  feet (stage x,z, metres):\n");
            for (float beat : {0.0f, 2.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 10.0f,
                               12.0f, 14.0f, 16.0f, 20.0f, 24.0f, 28.0f, 31.0f}) {
                const motion::ClipPose pose = motion::sampleClip(dance.clip, beat);
                const animation::RetargetedPose retargeted =
                    rig->applyClip(pose, beat * 60.0 / dance.clip.bpm);
                const math::Vec3 l = animation::HumanoidRetargeter::stagePosition(retargeted, left);
                const math::Vec3 r = animation::HumanoidRetargeter::stagePosition(retargeted, right);
                std::printf("    beat %5.1f  L (%6.3f,%6.3f)  R (%6.3f,%6.3f)  "
                            "hipDrop %.3f  footErr %.4f\n",
                            beat, l.x, l.z, r.x, r.z, retargeted.stats.hipDrop,
                            retargeted.stats.maxFootError);
            }
        }
        std::vector<motion::ClipDiagnostic> diagnostics;
        motion::validateClip(dance.clip, dance.clip.id, diagnostics);
        int errors = 0;
        int warnings = 0;
        for (const motion::ClipDiagnostic& d : diagnostics) {
            if (d.severity == motion::ClipDiagnostic::Severity::Error) {
                ++errors;
                std::printf("  ERROR   %s\n", d.message.c_str());
            } else {
                ++warnings;
                std::printf("  warning %s\n", d.message.c_str());
            }
        }
        std::printf("  validation: %d error(s), %d warning(s)\n", errors, warnings);
        if (errors) {
            exitCode = 1;
        }
        if (!writeDir.empty()) {
            const std::filesystem::path path = writeDir / (dance.clip.id + ".json");
            std::string error;
            if (!motion::ProceduralChoreography::save(dance, path, &error)) {
                std::printf("  write failed: %s\n", error.c_str());
                exitCode = 1;
            } else {
                std::printf("  wrote %s\n", path.string().c_str());
            }
        }
    }
    return exitCode;
}
