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
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bgmedia::motion {

using math::Quat;
using math::Vec3;
using vrm::HumanoidBone;

inline constexpr int kClipFormatVersion = 2;
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
    float confidence = 1.0f; // 0..1; missing in version-1 clips
    std::string kind;        // free text: "downbeat", "hit", "pose" ...
};

struct FootTrack {
    // Per key: x (left), y (lift above the floor), z (forward) in HEIGHT units,
    // relative to the bind ankle position, in STAGE space (not root space).
    std::vector<Vec3> position;
    std::vector<float> yaw;  // turns, stage space
    // Optional BG Motion 3.2 toe roll, -1..1. Positive lifts the toes (heel
    // roll), negative presses the toes down (toe-off). Empty = 0.
    std::vector<float> toe;
};

// BG Motion 3.2 hand layer. Curl is per finger and per joint so a gesture can
// ripple through the fingers instead of closing the whole hand at once. Values
// are 0 (open) .. 1 (closed); spread is -1..1 (negative = toward the middle
// finger); thumbOpposition bends the thumb across the palm. Wrist values are
// normalised -1..1 and are applied on top of the clip's hand-bone rotation.
struct HandPose {
    std::array<std::array<float, 3>, 5> curl{};
    std::array<float, 5> spread{};
    float thumbOpposition = 0.0f;
    std::array<float, 3> wrist{};  // pitch, yaw, roll
};

[[nodiscard]] HandPose relaxedHandPose(float curl = 0.30f);
[[nodiscard]] HandPose blendHandPose(const HandPose& a, const HandPose& b, float w);
void clampHandPose(HandPose& pose);

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
    std::array<std::vector<HandPose>, 2> handPoses;  // optional detailed hand layer

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
        float toePitch = 0.0f;  // -1..1; positive = toes up (heel roll)
        float contact = 0.0f;  // 0 swing .. 1 fully planted
        FootPhase phase = FootPhase::Stance;
    };
    std::array<Foot, 2> foot;

    std::array<float, 2> hand{0.3f, 0.3f};  // finger curl, left / right
    std::array<HandPose, 2> handPose;       // detailed finger/wrist layer
    bool detailedHands = false;
    float energy = 0.0f;
    float accent = 0.0f;  // 0..1 envelope of the nearest authored accent
};

[[nodiscard]] ClipPose neutralClipPose();

// Samples at a clip beat. Looping clips wrap; one-shot clips clamp.
void sampleClip(const HumanoidClip& clip, double clipBeat, ClipPose& out);
[[nodiscard]] ClipPose sampleClip(const HumanoidClip& clip, double clipBeat);
// Contact (0 swing .. 1 planted) of one foot at a clip beat. Same value sampleClip reports,
// without evaluating the bone tracks.
[[nodiscard]] float footContactAt(const HumanoidClip& clip, int foot, double clipBeat);

// Authored accent envelope at a clip beat (0..1). Used by the musical time
// warp when no external BG Beat accent map exists (Motion Lab clips).
[[nodiscard]] float clipAccentStrength(const HumanoidClip& clip, double clipBeat);

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

// BG Motion 3.2 authoring round-trip. The writer emits format version 2 and
// includes optional toe and detailed hand tracks. The reader accepts version 1
// (BG Motion 3.0/3.1 clips) and version 2.
[[nodiscard]] std::string clipToJson(const HumanoidClip& clip);
[[nodiscard]] bool saveClipFile(const HumanoidClip& clip,
                                const std::filesystem::path& path,
                                std::string* error = nullptr);

// Structural validation (key counts, finite numbers, quaternion norms, loop
// seam, contact segments). Used by the loader and by the QA tool.
void validateClip(const HumanoidClip& clip, const std::string& sourceName,
                  std::vector<ClipDiagnostic>& out);

// --- BG Motion 3.4 variety ---------------------------------------------------

// Id suffix of a mirrored clip. Mirroring toggles the suffix, so mirroring twice gives
// back the original clip, id and name included.
inline constexpr const char* kMirrorSuffix = "~m";

// Mirror across the sagittal plane (x -> -x, the clip's left and right swap). Left and right
// bone tracks, hand layers, foot tracks and contact segments swap sides. Every rotation
// (x, y, z, w) becomes (x, -y, -z, w), the conjugate by the reflection (an axis is a
// pseudovector, so its x component stays and its y and z flip). Lateral positions and
// root/hips x flip, and so do root yaw and foot yaw. Pitch-like channels (toe roll, wrist
// pitch, finger curl and spread semantics) stay. Pure data transform: the result validates
// like any authored clip.
[[nodiscard]] HumanoidClip mirrorClip(const HumanoidClip& clip);

// Which bone is the other side of a left/right pair (self for the midline bones).
[[nodiscard]] vrm::HumanoidBone mirroredBone(vrm::HumanoidBone bone);

// Loop-seam blend of a per-loop choice. Loops are counted from the clip origin:
// loop = floor(clipBeat / beats), never below 0 (the time before beat 0 is loop 0, so no blend
// ever uses a negative loop). The last `windowBeats` before each seam blend the choice from the
// loop before the seam to the loop after it (smootherstep over the window). The window ends at
// the seam: at the seam the choice is the loop after it, and outside the window `from == to` and
// `w == 0`. Pure function of its inputs, continuous in clipBeat.
struct LoopBlend {
    std::int64_t from = 0;
    std::int64_t to = 0;
    float w = 0.0f;  // 0 = `from`, 1 = `to`
};
[[nodiscard]] LoopBlend loopBlendAt(double clipBeat, double loopBeats, double windowBeats);

// Platform independent integer hashing for deterministic per-clip, per-loop variation
// (overlay seeds, amplitude draws). Never std::hash: its values differ between standard
// libraries.
[[nodiscard]] std::uint64_t variationHash(const std::string& text);
[[nodiscard]] std::uint64_t variationMix(std::uint64_t value);

class DanceLibrary {
public:
    // Loads every *.json file in the directory. Invalid clips are reported in
    // diagnostics() and skipped; they never abort loading of the others.
    void loadDirectory(const std::filesystem::path& directory);
    bool add(HumanoidClip clip);

    // Also resolves the mirrored id of a loaded clip (id + kMirrorSuffix).
    [[nodiscard]] const HumanoidClip* find(const std::string& id) const;
    // Replaces a loaded clip in place (same address, so running schedules stay valid).
    // A cached mirror of that clip is refreshed in place too.
    bool replace(const HumanoidClip& clip);
    [[nodiscard]] const HumanoidClip* forStyle(DanceStyle style) const;
    [[nodiscard]] const std::deque<HumanoidClip>& clips() const { return clips_; }
    [[nodiscard]] const std::vector<ClipDiagnostic>& diagnostics() const {
        return diagnostics_;
    }
    [[nodiscard]] bool empty() const { return clips_.empty(); }
    // Mirrored variant of a loaded clip, created on first use and owned by the library: the
    // pointer stays valid for the library's lifetime (replace() updates it in place).
    [[nodiscard]] const HumanoidClip* mirrorOf(const HumanoidClip& clip) const;

private:
    [[nodiscard]] const HumanoidClip* findOriginal(const std::string& id) const;

    std::deque<HumanoidClip> clips_;  // stable addresses for schedules
    std::vector<ClipDiagnostic> diagnostics_;
    // Lazily created mirrors (stable addresses). Indexed by the original id, so a copied
    // library owns its own mirrors.
    mutable std::deque<HumanoidClip> mirrors_;
    mutable std::map<std::string, std::size_t> mirrorIndex_;
};

} // namespace bgmedia::motion
