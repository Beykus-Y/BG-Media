#pragma once

// BG Motion 3.0 playback: BeatClock -> DanceSchedule -> ClipPose.
//
// The musical chain stays BG Beat -> BG Motion -> BG Sync -> Retargeter:
//   * BeatClock turns a BG Beat analysis (or a user BPM / Motion Lab grid) into
//     a smooth media-time <-> musical-beat mapping. Dancing is driven by that
//     mapping, never by a free running animation timer, so it cannot drift.
//   * DanceSchedule is a list of clip segments. Sampling it is a PURE function
//     of media time (seek, pause, any FPS and arbitrary jumps are therefore
//     safe). Crossfades are part of the schedule, not hidden player state.
//   * DanceDirector builds schedules: Manual (user picks a style, the change
//     lands on the next beat) and Auto (phrase based plan from the analysis).

#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/motion3/DanceClip.hpp"

#include <string>
#include <vector>

namespace bgmedia::motion {

class BeatClock {
public:
    BeatClock() = default;

    [[nodiscard]] static BeatClock constant(double bpm, double firstBeatTime = 0.0,
                                            int beatsPerBar = 4);
    // Smoothed grid from BG Beat. Falls back to the analysed BPM when the beat
    // list is too short.
    [[nodiscard]] static BeatClock fromAnalysis(const AnalysisResult& analysis,
                                                double fallbackBpm = 100.0);

    [[nodiscard]] double beatAt(double mediaTime) const;
    [[nodiscard]] double timeAtBeat(double beat) const;
    [[nodiscard]] double bpmAt(double mediaTime) const;
    // Beat index within the bar (0 = downbeat) of the beat containing `beat`.
    [[nodiscard]] int beatInBar(double beat) const;
    [[nodiscard]] int beatsPerBar() const { return beatsPerBar_; }
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] float confidence() const { return confidence_; }
    [[nodiscard]] bool constantGrid() const { return times_.empty(); }
    [[nodiscard]] double nominalBpm() const { return bpm_; }

private:
    bool valid_ = false;
    double bpm_ = 100.0;
    double first_ = 0.0;
    int beatsPerBar_ = 4;
    int barOffset_ = 0;
    float confidence_ = 1.0f;
    std::vector<double> times_;  // smoothed beat times; empty = constant grid
    double headPeriod_ = 0.6;
    double tailPeriod_ = 0.6;
};

struct ScheduleSegment {
    const HumanoidClip* clip = nullptr;
    double startTime = 0.0;        // media seconds: blend from the previous segment begins
    double alignTime = 0.0;        // media seconds: musical reference for the entry beat
    double entryClipBeat = 0.0;    // clip beat at alignTime
    double rate = 1.0;             // clip beats per music beat (metric level 0.5 / 1 / 2)
    double fadeSeconds = 0.6;      // blend length from the previous segment
    bool loop = true;
    std::string reason;            // why the planner chose this segment
};

struct DanceFrame {
    ClipPose pose;
    std::string clipId;
    std::string clipName;
    std::string nextClipId;        // upcoming (or currently fading in) segment
    DanceStyle style = DanceStyle::Idle;
    double clipBeat = 0.0;
    float phase = 0.0f;            // 0..1 inside the current clip
    float playbackSpeed = 1.0f;    // musical tempo relative to the clip's authored tempo
    bool speedLimited = false;     // tempo outside the clip's comfortable range
    float transitionProgress = -1.0f;  // 0..1 while crossfading, else -1
    double bpm = 0.0;
    bool oneShotFinished = false;
    std::string reason;
};

class DanceSchedule {
public:
    void clear() { segments_.clear(); }
    void append(ScheduleSegment segment);
    // Removes segments that start after `time` (used before appending a manual
    // switch so the future is replaced rather than stacked).
    void truncateAfter(double time);
    void pruneBefore(double time, std::size_t keepAtLeast = 4);
    [[nodiscard]] const std::vector<ScheduleSegment>& segments() const { return segments_; }
    [[nodiscard]] bool empty() const { return segments_.empty(); }

    [[nodiscard]] DanceFrame sample(double mediaTime, const BeatClock& clock) const;
    // Clip beat of a segment at a given media time (exposed for tests).
    [[nodiscard]] static double segmentClipBeat(const ScheduleSegment& segment,
                                                double mediaTime, const BeatClock& clock);

private:
    [[nodiscard]] ClipPose poseOf(std::size_t index, double mediaTime,
                                  const BeatClock& clock, float* transition) const;
    std::vector<ScheduleSegment> segments_;
};

enum class DanceMode { Manual, Auto };

struct AutoPlanOptions {
    double phraseBeats = 16.0;
    double fadeBeats = 2.0;
    float calmThreshold = 0.34f;      // below: Groove
    float intenseThreshold = 0.68f;   // above: Energetic
    float minConfidence = 0.30f;      // below: stay on stable Groove
    int minPhrasesPerStyle = 1;
};

class DanceDirector {
public:
    explicit DanceDirector(const DanceLibrary* library = nullptr) : library_(library) {}

    void setLibrary(const DanceLibrary* library) { library_ = library; }
    void setBeatClock(BeatClock clock) { clock_ = std::move(clock); }
    [[nodiscard]] const BeatClock& beatClock() const { return clock_; }

    // Manual: the character moves to `style` on the next beat (or immediately
    // when `immediate`). `oneShot` plays the clip once and returns to Idle.
    void select(DanceStyle style, double now, bool oneShot = false, bool immediate = false);
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
    // Restarts the selected style at `now`, aligned to the bar structure of the
    // grid, without a fade (used after a seek).
    void reseed(double now);
    // Installs a new beat grid while keeping the current clip and its phase, so
    // a tempo change never makes the pose jump.
    void rebase(double now, BeatClock newClock);
    // Drops everything that has not started yet (the dancer carries on with
    // the clip it is in).
    void dropFuture(double now) { schedule_.truncateAfter(now); }

    [[nodiscard]] DanceFrame sample(double mediaTime) const;
    [[nodiscard]] const DanceSchedule& schedule() const { return schedule_; }
    // Human readable list of the planned segments (time, clip, reason).
    [[nodiscard]] std::string describeSchedule() const;
    [[nodiscard]] DanceSchedule& schedule() { return schedule_; }
    [[nodiscard]] DanceStyle selectedStyle() const { return selected_; }
    void setSelectedStyle(DanceStyle style) { selected_ = style; }
    [[nodiscard]] const DanceLibrary* library() const { return library_; }

    // Builds a segment for `clip` aligned to the musical grid at `alignTime`.
    [[nodiscard]] ScheduleSegment makeSegment(const HumanoidClip& clip, double startTime,
                                              double alignTime, double fadeSeconds) const;
    // Metric level (0.5, 1, 2) that brings the clip tempo into its range.
    [[nodiscard]] static double chooseRate(const HumanoidClip& clip, double musicBpm,
                                           bool* limited = nullptr);

private:
    [[nodiscard]] double fadeFor(double atTime) const;
    [[nodiscard]] const HumanoidClip* clipFor(DanceStyle style) const;

    const DanceLibrary* library_ = nullptr;
    BeatClock clock_ = BeatClock::constant(100.0);
    DanceSchedule schedule_;
    DanceStyle selected_ = DanceStyle::Idle;
};

} // namespace bgmedia::motion
