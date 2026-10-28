#pragma once

// BG Motion 3.1 playback: MusicalTimeline -> DanceSchedule -> ClipPose.
//
// The musical chain stays BG Beat -> BG Motion -> BG Sync -> Retargeter:
//   * BeatClock/MusicalTimeline turn the analysis (or a user BPM / Motion Lab
//     grid) into a smooth media-time <-> musical-beat mapping and an accent
//     map with real timestamps.
//   * DanceSchedule is a list of clip segments. Sampling it is a PURE function
//     of media time (seek, pause, any FPS and arbitrary jumps are therefore
//     safe). Crossfades are part of the schedule, not hidden player state.
//   * DanceDirector builds schedules: Manual (user picks a style, the change
//     lands on the next downbeat or entry beat) and Auto (phrase based plan).
//   * The optional MusicalTimeWarp is a bounded phase modulation inside each
//     beat. It never moves accent poses off the beat and never rotates root or
//     foot targets: those keep the stable grid so contacts remain valid.

#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/motion3/DanceClip.hpp"
#include "bgmedia/motion3/MusicalTimeline.hpp"
#include "bgmedia/motion3/MusicalTimeWarp.hpp"

#include <array>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bgmedia::motion {

struct ScheduleSegment {
    const HumanoidClip* clip = nullptr;
    double startTime = 0.0;        // media seconds: blend from the previous segment begins
    double alignTime = 0.0;        // media seconds: musical reference for the entry beat
    double entryClipBeat = 0.0;    // clip beat at alignTime
    double rate = 1.0;             // clip beats per music beat (metric level 0.5 / 1 / 2)
    double fadeSeconds = 0.6;      // blend length from the previous segment
    bool loop = true;
    std::string reason;            // why the planner chose this segment
    bool alignedToDownbeat = false;  // the entry landed on a trusted bar start
    bool barPhaseTrusted = true;   // the musical bar phase was trusted at entry
    int meter = 4;                 // musical meter used for the entry
    double alignmentErrorSeconds = 0.0;  // |alignTime - nearest target boundary|
    // Planted-foot hand-over window per foot (BG Motion 3.3, A1). DanceSchedule::append with a
    // timeline resolves it once, from the swing that is in the window at scheduling time. It is
    // never searched again: a rebase shifts a pending window's media seconds with the segment's
    // alignment, and the progress follows the swing's clip beats. An unresolved window is
    // searched on demand.
    struct FootSwingWindow {
        double begin = 0.0;    // media seconds: the foot leaves its outgoing position
        double end = 0.0;      // media seconds: the foot reaches its incoming position
        // The same swing in linear clip beats of the clip it was found in (the outgoing
        // segment's when `fromOutgoing`). The hand-over progress is read from these, so a
        // tempo rebase, which keeps each clip's beat at the rebase time, keeps the progress
        // continuous and the hand-over on the swing at the new tempo.
        double beginBeat = 0.0;
        double endBeat = 0.0;
        bool fromOutgoing = false;
        bool slide = false;    // no swing in the window: the foot slides through the fade
        bool resolved = false;
    };
    std::array<FootSwingWindow, 2> footSwing{};
};

// Optional per-segment pose post-process (BG Motion 3.2 overlays). It is applied
// to each segment's own pose *before* segments are crossfaded, so an outgoing
// clip keeps its own overlays while it fades and an incoming clip fades its
// overlays in, instead of the overlay set flipping mid-transition.
using PoseDecorator =
    std::function<void(ClipPose& pose, const ScheduleSegment& segment, double clipBeat)>;

struct DanceFrame {
    ClipPose pose;
    std::string clipId;
    std::string clipName;
    std::string nextClipId;        // upcoming (or currently fading in) segment
    DanceStyle style = DanceStyle::Idle;
    double clipBeat = 0.0;         // warped clip beat used to sample the pose
    double unwarpedClipBeat = 0.0; // stable-grid clip beat (diagnostics/A-B)
    float phase = 0.0f;            // 0..1 inside the current clip
    float playbackSpeed = 1.0f;    // musical tempo relative to the clip's authored tempo
    float effectiveSpeed = 1.0f;   // playbackSpeed * warpRate
    bool speedLimited = false;     // tempo outside the clip's comfortable range
    float transitionProgress = -1.0f;  // 0..1 while crossfading, else -1
    float idleWeight = 0.0f;  // share of the Idle clip in the shown pose (0..1), crossfades included
    double bpm = 0.0;
    bool oneShotFinished = false;
    std::string reason;

    // --- BG Motion 3.2 motion layer diagnostics ---------------------------
    int overlayActive = 0;
    int overlayRejected = 0;
    float overlayWeight = 0.0f;
    bool overlayHands = false;

    // --- BG Motion 3.1 musical diagnostics --------------------------------
    double musicBeat = 0.0;        // absolute musical beat at this media time
    double warpDeviationBeats = 0.0;
    double warpRate = 1.0;         // d(warped beat)/d(music beat)
    bool warpActive = false;
    float musicAccent = 0.0f;
    float bassStrength = 0.0f;
    float downbeatConfidence = 0.0f;
    float meterConfidence = 0.0f;
    int beatsPerBar = 4;
    int beatInBar = 0;
    int barIndex = 0;
    bool downbeat = false;
    bool barPhaseTrusted = false;
    double alignmentErrorMs = 0.0;
};

class DanceSchedule {
public:
    void clear() { segments_.clear(); }
    void setPoseDecorator(PoseDecorator decorator) { decorator_ = std::move(decorator); }
    // Opt-in (BG Motion 3.3, #8). Upper-body bones carry the outgoing pose through a
    // segment crossfade as a decaying rotation offset instead of a smootherstep blend.
    // Off by default: the blend stays the reference behaviour until A/B-tested.
    void setInertialTransitions(bool enabled) { inertialTransitions_ = enabled; }
    [[nodiscard]] bool inertialTransitions() const { return inertialTransitions_; }
    // Opt-in (BG Motion 3.3, #9). Head and hands lag behind their own motion through a
    // second-order spring driven by angular velocity. Evaluated per media time from a
    // fixed window of history, so seeking and playback agree. Off by default.
    void setFollowThrough(bool enabled) { followThrough_ = enabled; }
    [[nodiscard]] bool followThrough() const { return followThrough_; }
    void append(ScheduleSegment segment);
    // Appends and resolves the planted-foot hand-over windows against the predecessor, the
    // grid and the warp in effect now. Use it when scheduling: later grid changes then keep
    // the windows instead of moving the foot hand-over.
    void append(ScheduleSegment segment, const MusicalTimeline& timeline, const WarpProfile& warp);
    // Searches again the hand-over windows of every segment after the grid changed without a
    // re-anchor (a plain setBeatClock / setMusicalTimeline). A re-anchored rebase keeps them.
    void refreshFootSwings(const MusicalTimeline& timeline, const WarpProfile& warp);
    // Removes segments that start after `time` (used before appending a manual
    // switch so the future is replaced rather than stacked).
    void truncateAfter(double time);
    void pruneBefore(double time, std::size_t keepAtLeast = 4);
    [[nodiscard]] const std::vector<ScheduleSegment>& segments() const { return segments_; }
    [[nodiscard]] bool empty() const { return segments_.empty(); }

    [[nodiscard]] DanceFrame sample(double mediaTime, const BeatClock& clock) const;
    [[nodiscard]] DanceFrame sample(double mediaTime, const BeatClock& clock,
                                    const MusicalTimeline& timeline,
                                    const WarpProfile& warp) const;
    // Clip beat of a segment at a given media time (exposed for tests).
    [[nodiscard]] static double segmentClipBeat(const ScheduleSegment& segment,
                                                double mediaTime, const BeatClock& clock);
    // Full mapping, including the optional warp. `rate` is the instantaneous
    // animation speed in clip beats per music beat.
    [[nodiscard]] static double segmentClipBeat(const ScheduleSegment& segment,
                                                double mediaTime,
                                                const MusicalTimeline& timeline,
                                                const WarpProfile& warp,
                                                double* rate = nullptr,
                                                bool* warpActive = nullptr,
                                                double* deviation = nullptr);

private:
    // Pose of one segment at one time, memoized per sample() call: with overlapping
    // fades the recursion would otherwise revisit the same (segment, time) pairs.
    using PoseMemo = std::map<std::pair<std::size_t, double>, ClipPose>;
    [[nodiscard]] ClipPose poseOf(std::size_t index, double mediaTime,
                                  const MusicalTimeline& timeline,
                                  const WarpProfile& warp, float* transition,
                                  PoseMemo& memo) const;
    [[nodiscard]] ClipPose basePose(std::size_t index, double mediaTime,
                                    const MusicalTimeline& timeline,
                                    const WarpProfile& warp, double* clipBeat) const;
    [[nodiscard]] ClipPose ownPose(std::size_t index, double mediaTime,
                                   const MusicalTimeline& timeline,
                                   const WarpProfile& warp) const;
    // BG Motion 3.3 (A1). Stage-position hand-over of each foot of segment `index`. The
    // foot keeps its outgoing position until the incoming clip starts a swing for it, then
    // moves to the incoming position with smootherstep over that swing. `active` is false
    // once the hand-over has completed. Pure function of media time.
    struct FootHandOver {
        float progress = 1.0f;  // 0 = outgoing position, 1 = incoming position
        bool active = false;
    };
    [[nodiscard]] std::array<FootHandOver, 2> footHandOver(std::size_t index, double mediaTime,
                                                           const MusicalTimeline& timeline,
                                                           const WarpProfile& warp) const;
    // Searches the hand-over window of one foot of segment `index` (see FootSwingWindow).
    [[nodiscard]] ScheduleSegment::FootSwingWindow findFootSwing(std::size_t index, int foot,
                                                                 const MusicalTimeline& timeline,
                                                                 const WarpProfile& warp) const;
    void applyFollowThrough(ClipPose& pose, std::size_t index, double mediaTime,
                            const MusicalTimeline& timeline, const WarpProfile& warp) const;
    std::vector<ScheduleSegment> segments_;
    PoseDecorator decorator_;
    bool inertialTransitions_ = false;
    bool followThrough_ = false;
};

enum class DanceMode { Manual, Auto };

struct AutoPlanOptions {
    double phraseBeats = 16.0;
    double fadeBeats = 2.0;
    float calmThreshold = 0.34f;      // below: Groove
    float intenseThreshold = 0.68f;   // above: Energetic
    float minConfidence = 0.30f;      // below: stay on stable Groove
    int minPhrasesPerStyle = 1;
    int phraseBars = 4;               // used when the meter is trusted
};

class DanceDirector {
public:
    explicit DanceDirector(const DanceLibrary* library = nullptr) : library_(library) {}

    void setLibrary(const DanceLibrary* library) { library_ = library; }
    // Replaces the grid and rebuilds a grid-only timeline. Kept for Motion Lab
    // and for code that only needs a constant grid.
    void setBeatClock(BeatClock clock);
    // Installs a full musical timeline (grid + meter confidence + accents).
    void setMusicalTimeline(MusicalTimeline timeline);
    [[nodiscard]] const BeatClock& beatClock() const { return clock_; }
    [[nodiscard]] const MusicalTimeline& timeline() const { return timeline_; }

    void setWarpProfile(WarpProfile profile) { warp_ = profile; }
    [[nodiscard]] const WarpProfile& warpProfile() const { return warp_; }
    void setExpression(MotionExpression expression) { warp_.expression = expression; }
    [[nodiscard]] MotionExpression expression() const { return warp_.expression; }

    // Manual: the character moves to `style` on the next downbeat when the bar
    // is trusted, otherwise on the next entry beat. `oneShot` plays the clip
    // once and returns to Idle.
    void select(DanceStyle style, double now, bool oneShot = false, bool immediate = false);
    // Manual switch to a specific clip id (used by generated and authoring
    // previews). The style is updated to the clip's style.
    void selectClip(const std::string& clipId, double now, bool oneShot = false,
                    bool immediate = false);
    // Starts the all-styles showcase (Groove -> HipHop -> SideStep -> Energetic -> Idle).
    void startTestDance(double now, double beatsPerStyle = 16.0);
    // Plays `styles` one after another, `beatsPerStyle` music beats each, on the beat.
    // `immediate` starts at `now` instead of waiting for an entry beat.
    void startSequence(const std::vector<DanceStyle>& styles, double now, double beatsPerStyle,
                       bool immediate = false);
    // Builds the whole auto plan. Replaces the schedule.
    void planAuto(const AnalysisResult& analysis, const AutoPlanOptions& options = {});
    // Fresh schedule that is just Idle.
    void reset(double now = 0.0);
    // Restarts the selected clip at `now`, aligned to the bar structure of the
    // grid, without a fade (used when the time precedes the schedule).
    void reseed(double now);
    // Installs a new beat grid while keeping the current clip and its phase, so
    // a tempo change never makes the pose jump.
    void rebase(double now, BeatClock newClock);
    void rebase(double now, MusicalTimeline newTimeline);
    // Drops everything that has not started yet (the dancer carries on with
    // the clip it is in).
    void dropFuture(double now) { schedule_.truncateAfter(now); }

    [[nodiscard]] DanceFrame sample(double mediaTime) const;
    [[nodiscard]] const DanceSchedule& schedule() const { return schedule_; }
    // Human readable list of the planned segments (time, clip, reason).
    [[nodiscard]] std::string describeSchedule() const;
    [[nodiscard]] DanceSchedule& schedule() { return schedule_; }
    [[nodiscard]] DanceStyle selectedStyle() const { return selected_; }
    // The clip the Manual selection plays. Generated Custom clips share one style, so a
    // reseed must follow the clip id, not the style.
    [[nodiscard]] const std::string& selectedClipId() const { return selectedClip_; }
    void setSelectedClip(const std::string& clipId, DanceStyle style) {
        selectedClip_ = clipId;
        selected_ = style;
    }
    [[nodiscard]] const DanceLibrary* library() const { return library_; }

    // Next point where a manual switch can start without breaking the bar.
    [[nodiscard]] double nextAlignmentTime(double now) const;
    // Builds a segment for `clip` aligned to the musical grid at `alignTime`.
    [[nodiscard]] ScheduleSegment makeSegment(const HumanoidClip& clip, double startTime,
                                              double alignTime, double fadeSeconds) const;
    // Metric level (0.5, 1, 2) that brings the clip tempo into its range.
    [[nodiscard]] static double chooseRate(const HumanoidClip& clip, double musicBpm,
                                           bool* limited = nullptr);

private:
    // BG Motion 3.3 (A5). Re-anchors the schedule after the grid changed from `before` to
    // clock_/timeline_ at `now`: active segments keep their blend and their clip beat at
    // `now`, pending segments keep their musical alignment beat. Used by rebase().
    void reanchorSchedule(double now, const BeatClock& before);
    [[nodiscard]] double fadeFor(double atTime) const;
    [[nodiscard]] const HumanoidClip* clipFor(DanceStyle style) const;
    [[nodiscard]] double entryTimeFor(const HumanoidClip& clip, double now,
                                      bool* alignedToDownbeat, double* alignmentError) const;

    const DanceLibrary* library_ = nullptr;
    BeatClock clock_ = BeatClock::constant(100.0);
    MusicalTimeline timeline_ = MusicalTimeline::constant(100.0);
    WarpProfile warp_;
    DanceSchedule schedule_;
    DanceStyle selected_ = DanceStyle::Idle;
    std::string selectedClip_;
};

} // namespace bgmedia::motion
