#pragma once

// BG Motion 3.0 - authored humanoid dance clips.
//
// A clip is DATA: a baked, model-independent humanoid animation stored as a
// JSON resource (see docs/BG_MOTION_3.md for the format). Nothing in a clip
// refers to node indices of a particular VRM; everything is expressed against
// the VRM humanoid bone names and in height-relative units, so one clip plays
// on any VRM 0.x model through animation::HumanoidRetargeter::applyClip().
//
// Clip space ("dance frame"): +X = character left, +Y = up, +Z = forward
// (right handed). Lengths are fractions of the model height. Rotations are
// unit quaternions in that frame, expressed as the bone's rotation relative to
// its parent in the VRM T-pose (bind) pose. Identity = bind pose.
//
// Time is expressed in MUSICAL BEATS of the clip, not in seconds. A clip knows
// its authored tempo, but the player maps beats to media time through the BG
// Beat grid, so a clip is never driven by a free-running timer.

#include "bgmedia/math/Math.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace bgmedia::motion {

using math::Quat;
using math::Vec3;
using vrm::HumanoidBone;

inline constexpr int kClipFormatVersion = 1;
inline constexpr const char* kClipFormatName = "bgmotion.clip";

enum class DanceStyle : std::uint8_t {
    Idle,
    Groove,
    HipHop,
    SideStep,
    Energetic,
    Custom
};

[[nodiscard]] const char* styleName(DanceStyle style);
[[nodiscard]] DanceStyle styleFromName(const std::string& name);

enum class FootPhase : std::uint8_t {
    Swing,   // foot travels through the air
    Plant,   // the first moment of a stance (foot settling)
    Stance,  // planted, supports weight, must not slide
    Pivot,   // planted but turning in place
    Release  // the last moment of a stance (heel/toe leaving)
};

[[nodiscard]] const char* footPhaseName(FootPhase phase);

struct ContactSegment {
    int foot = 0;            // 0 = left, 1 = right
    float start = 0.0f;      // beats
    float end = 0.0f;        // beats
    bool pivot = false;      // planted foot may turn (yaw) but not translate
};

struct AccentMark {
    float beat = 0.0f;
    float strength = 1.0f;   // 0..1
    std::string kind;        // free text: "downbeat", "hit", "pose" ...
};

struct FootTrack {
    // Per key: x (left), y (lift above the floor), z (forward) in HEIGHT units,
    // relative to the bind ankle position, in STAGE space (not root space).
    std::vector<Vec3> position;
    std::vector<float> yaw;  // turns, stage space
};

struct HumanoidClip {
    // --- metadata ---
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    std::string origin;      // how the animation was produced
    DanceStyle style = DanceStyle::Custom;
    int formatVersion = kClipFormatVersion;

    // --- musical grid ---
    double bpm = 100.0;      // authored tempo
    double bpmMin = 70.0;    // comfortable playback range (after metric level)
    double bpmMax = 140.0;
    float beats = 8.0f;      // loop length in beats
    int beatsPerBar = 4;
    int keysPerBeat = 16;
    bool loop = true;
    bool seamless = true;    // last key continues into first key
    bool tempoSync = true;   // false: time axis is wall-clock (idle breathing)
    float energy = 0.5f;     // 0..1 intensity, used by the auto planner
    std::vector<float> entryBeats;  // good places to start the clip
    std::vector<float> exitBeats;   // good places to leave the clip

    std::vector<AccentMark> accents;
    std::vector<ContactSegment> contacts;

    // --- baked tracks; keyCount = beats * keysPerBeat + 1 ---
    int keyCount = 0;
    std::array<std::vector<Quat>, vrm::kHumanoidBoneCount> bones;  // empty = bind
    std::vector<Vec3> hips;            // offset from the bind hips, root space
    std::vector<Vec3> root;            // x (left), z (forward), yaw (turns)
    std::array<FootTrack, 2> feet;     // [0] left, [1] right
    std::array<std::vector<float>, 2> hands;  // finger curl 0 (open) .. 1 (fist); empty = relaxed

    [[nodiscard]] double durationAt(double bpmValue) const {
        return beats * 60.0 / bpmValue;
    }
};

// Result of sampling one clip (or a blend of several) at a point in time.
struct ClipPose {
    std::array<Quat, vrm::kHumanoidBoneCount> bone;
    Vec3 hips{};
    float rootX = 0.0f;   // left, height units
    float rootZ = 0.0f;   // forward, height units
    float rootYaw = 0.0f; // turns

    struct Foot {
        Vec3 position{};  // stage space offset from bind ankle
        float yaw = 0.0f; // turns, stage space
        float contact = 0.0f;  // 0 swing .. 1 fully planted
        FootPhase phase = FootPhase::Stance;
    };
    std::array<Foot, 2> foot;

    std::array<float, 2> hand{0.3f, 0.3f};  // finger curl, left / right
    float energy = 0.0f;
    float accent = 0.0f;  // 0..1 envelope of the nearest authored accent
};

[[nodiscard]] ClipPose neutralClipPose();

// Samples at a clip beat. Looping clips wrap; one-shot clips clamp.
void sampleClip(const HumanoidClip& clip, double clipBeat, ClipPose& out);
[[nodiscard]] ClipPose sampleClip(const HumanoidClip& clip, double clipBeat);

// Weighted blend (w = 0 -> a, w = 1 -> b). Rotations use slerp, root yaw and
// foot yaw use the shortest arc, positions are linear.
[[nodiscard]] ClipPose blendPoses(const ClipPose& a, const ClipPose& b, float w);

// --- loading ---
struct ClipDiagnostic {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Error;
    std::string file;
    std::string message;
};

struct ClipLoadResult {
    HumanoidClip clip;
    bool ok = false;
    std::vector<ClipDiagnostic> diagnostics;
};

[[nodiscard]] ClipLoadResult parseClipJson(const std::string& text,
                                           const std::string& sourceName = {});
[[nodiscard]] ClipLoadResult loadClipFile(const std::filesystem::path& path);

// Structural validation (key counts, finite numbers, quaternion norms, loop
// seam, contact segments). Used by the loader and by the QA tool.
void validateClip(const HumanoidClip& clip, const std::string& sourceName,
                  std::vector<ClipDiagnostic>& out);

class DanceLibrary {
public:
    // Loads every *.json file in the directory. Invalid clips are reported in
    // diagnostics() and skipped; they never abort loading of the others.
    void loadDirectory(const std::filesystem::path& directory);
    bool add(HumanoidClip clip);

    [[nodiscard]] const HumanoidClip* find(const std::string& id) const;
    [[nodiscard]] const HumanoidClip* forStyle(DanceStyle style) const;
    [[nodiscard]] const std::deque<HumanoidClip>& clips() const { return clips_; }
    [[nodiscard]] const std::vector<ClipDiagnostic>& diagnostics() const {
        return diagnostics_;
    }
    [[nodiscard]] bool empty() const { return clips_.empty(); }

private:
    std::deque<HumanoidClip> clips_;  // stable addresses for schedules
    std::vector<ClipDiagnostic> diagnostics_;
};

} // namespace bgmedia::motion
