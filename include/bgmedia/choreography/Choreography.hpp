#pragma once

#include "bgmedia/analysis/AnalysisResult.hpp"

#include <cstdint>
#include <vector>

namespace bgmedia {

enum class MoveKind {
    Idle,
    Pulse,
    Bounce,
    Sway,
    ArmWave,
    Spin,
    Pose,
    Freeze
};

struct DanceCue {
    double time = 0.0;       // media seconds
    double duration = 0.0;
    MoveKind move = MoveKind::Idle;
    float intensity = 0.0f;  // 0..1
    int beatIndex = -1;
    int barIndex = -1;
    bool bassAccent = false;
};

// A backend-independent character pose. Values are normalised so a 2D sprite,
// a skeletal 3D rig, or a Telegram animated sticker backend can all consume it.
struct CharacterPose {
    float bob = 0.0f;         // -1..1 vertical motion
    float lean = 0.0f;        // -1..1
    float armLeft = 0.0f;     // -1..1
    float armRight = 0.0f;
    float legLeft = 0.0f;
    float legRight = 0.0f;
    float rotation = 0.0f;    // -1..1, mapped by the renderer
    float energy = 0.0f;      // 0..1
    MoveKind move = MoveKind::Idle;
    bool accented = false;
};

struct ChoreographyStyle {
    float intensity = 1.0f;
    // How much bass accents add to the move energy (0..1).
    float bassInfluence = 0.45f;
};

struct Choreography {
    std::vector<DanceCue> cues;
    int beatsPerBar = 4;
    std::uint32_t analyzerVersion = 0;
};

// Pure choreography calculation: consumes an AnalysisResult and produces
// deterministic cues and poses. No rendering, no SDL, no clock. Sampling at a
// media timestamp makes the output independent of screen FPS.
class ChoreographyBuilder {
public:
    [[nodiscard]] Choreography build(const AnalysisResult& analysis,
                                     ChoreographyStyle style = {}) const;
};

class ChoreographySampler {
public:
    [[nodiscard]] CharacterPose sample(const Choreography& choreography,
                                       double mediaSeconds) const;

    [[nodiscard]] const DanceCue* activeCue(const Choreography& choreography,
                                            double mediaSeconds) const;

    // Number of cues including the implicit idle cue before the first beat.
    [[nodiscard]] static std::size_t cueCount(const Choreography& choreography) {
        return choreography.cues.size();
    }
};

} // namespace bgmedia
