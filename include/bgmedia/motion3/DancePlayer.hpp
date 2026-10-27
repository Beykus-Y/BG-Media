#pragma once

// DancePlayer: the application-facing facade of BG Motion 3.0.
//
// Owns the clip library and the DanceDirector and implements the two user
// modes. It is deliberately free of rendering, windows and clocks: the host
// passes the media time (BG Sync position, or the Motion Lab clock) into
// update() and gets back the pose for that exact time.

#include "bgmedia/motion3/DanceDirector.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace bgmedia::motion {

class DancePlayer {
public:
    DancePlayer() : director_(&library_) {}
    DancePlayer(const DancePlayer&) = delete;
    DancePlayer& operator=(const DancePlayer&) = delete;

    // Loads every clip of a directory. Returns the number of clips available.
    std::size_t loadLibrary(const std::filesystem::path& directory);
    [[nodiscard]] const DanceLibrary& library() const { return library_; }
    [[nodiscard]] bool ready() const { return !library_.empty(); }

    // The musical grid driving the clips. Rebases the running clip so the pose
    // does not jump when the tempo changes.
    void setGrid(BeatClock clock, double now);
    // Installs a full musical timeline (meter confidence, real accents, bass).
    void setMusicalTimeline(MusicalTimeline timeline, double now);
    // BG Beat analysis used by Auto mode and for the accent/timeline map.
    // nullptr = no music. `now` keeps a Manual dance phase stable.
    void setAnalysis(const AnalysisResult* analysis);
    void setAnalysis(const AnalysisResult* analysis, double now);

    void setMode(DanceMode mode, double now);
    [[nodiscard]] DanceMode mode() const { return mode_; }

    // Expressive musical warp. Off / Soft / Expressive; the value is also
    // persisted by the desktop host, not by the engine.
    void setExpression(MotionExpression expression) { director_.setExpression(expression); }
    [[nodiscard]] MotionExpression expression() const { return director_.expression(); }
    void setWarpProfile(WarpProfile profile) { director_.setWarpProfile(profile); }
    [[nodiscard]] const WarpProfile& warpProfile() const { return director_.warpProfile(); }
    [[nodiscard]] const MusicalTimeline& timeline() const { return director_.timeline(); }

    // Manual selection (switches to Manual). The change lands on the next beat.
    void select(DanceStyle style, double now, bool immediate = false);
    void setOneShot(bool oneShot) { oneShot_ = oneShot; }
    [[nodiscard]] bool oneShot() const { return oneShot_; }
    // Groove -> Hip-Hop -> Side Step -> Energetic -> Idle showcase.
    void startTestDance(double now);
    void startSequence(const std::vector<DanceStyle>& styles, double now, double beatsPerStyle,
                       bool immediate = false);

    // Pose for `mediaTime`. Detects seeks (backward or large jumps) and
    // realigns the Manual schedule; Auto is a pure plan and needs nothing.
    [[nodiscard]] DanceFrame update(double mediaTime);

    [[nodiscard]] const DanceDirector& director() const { return director_; }
    [[nodiscard]] DanceStyle selectedStyle() const { return director_.selectedStyle(); }
    [[nodiscard]] const std::string& autoNote() const { return autoNote_; }

private:
    DanceLibrary library_;
    DanceDirector director_;
    DanceMode mode_ = DanceMode::Manual;
    const AnalysisResult* analysis_ = nullptr;
    bool oneShot_ = false;
    double lastTime_ = -1.0e9;
    std::string autoNote_;
};

} // namespace bgmedia::motion
