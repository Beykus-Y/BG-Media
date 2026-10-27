#pragma once

// BG Motion 3.2 - procedural choreography generator.
//
// The generator composes a complete dance from motifs, phrases and sections.
// It does not sample random joint angles per frame and it does not depend on an
// external model or LLM. Inputs (seed, style, BPM, meter, intensity, duration)
// fully determine the output, which can be baked to a HumanoidClip (JSON v2)
// and replayed on any VRM through HumanoidRetargeter.
//
// Structure:
//   Motif -> Phrase (2 bars) -> Section (intro / development / build / resolve)
//   -> Full dance (default 8 bars = 32 beats)
//
// The generated clip owns footwork, weight transfer, pelvis/spine/chest/head
// layers and arm paths. A second, independent overlay plan schedules motion
// primitives (hand flicks, shoulder accents, finger ripples) on top at runtime.

#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/motion3/MotionPrimitive.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bgmedia::motion {

struct ChoreographyRequest {
    std::string id = "procedural";
    std::string name = "Procedural";
    DanceStyle style = DanceStyle::Custom;
    std::string profile = "neon";  // neon / street / fluid
    double bpm = 104.0;
    int beatsPerBar = 4;
    int bars = 8;                  // 8 bars = 32 beats
    int seed = 1;
    float intensity = 0.70f;
    int keysPerBeat = 8;
    bool includeOverlays = true;
    bool loop = true;
};

struct ChoreographyStats {
    int beats = 0;
    int phrases = 0;
    int sections = 0;
    int steps = 0;
    int pivots = 0;
    int accents = 0;
    int overlayCues = 0;
    int bonesUsed = 0;
    float maxAngularSpeedDegPerSec = 0.0f;
    float maxAngularAccelerationDegPerSec2 = 0.0f;
    float loopSeamDeg = 0.0f;
    float footSlideHeightUnits = 0.0f;
    float peakHipDrop = 0.0f;
    bool finite = true;
    double generationMilliseconds = 0.0;
};

struct GeneratedDance {
    HumanoidClip clip;
    std::vector<PrimitiveCue> overlays;
    std::vector<std::string> structure;  // human-readable section/phrase notes
    ChoreographyStats stats;
};

class ProceduralChoreography {
public:
    // Deterministic: same request -> identical clip, overlays and stats.
    [[nodiscard]] static GeneratedDance generate(const ChoreographyRequest& request);

    // Three contrasting demos built from the same generator: different seeds,
    // profiles, phrasing, footwork and accent maps.
    [[nodiscard]] static std::vector<GeneratedDance> showcase();

    // Atomic-enough write: serialise first, then write the file.
    static bool save(const GeneratedDance& dance, const std::filesystem::path& path,
                     std::string* error = nullptr);
};

}  // namespace bgmedia::motion
