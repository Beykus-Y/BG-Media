#include "bgmedia/motion3/DancePlayer.hpp"

#include <cmath>

namespace bgmedia::motion {

std::size_t DancePlayer::loadLibrary(const std::filesystem::path& directory) {
    library_ = DanceLibrary{};
    library_.loadDirectory(directory);
    director_.setLibrary(&library_);
    director_.reset(0.0);
    lastTime_ = -1.0e9;
    return library_.clips().size();
}

void DancePlayer::setGrid(BeatClock clock, double now) {
    if (mode_ == DanceMode::Auto) {
        director_.setBeatClock(std::move(clock));
        return;
    }
    director_.rebase(now, std::move(clock));
}

void DancePlayer::setAnalysis(const AnalysisResult* analysis) {
    analysis_ = analysis;
    if (mode_ == DanceMode::Auto) {
        if (analysis_ && !analysis_->empty()) {
            director_.planAuto(*analysis_);
            autoNote_ = "plan built from BG Beat analysis";
        } else {
            director_.reset(0.0);
            autoNote_ = "Auto needs music: no track";
        }
    }
}

void DancePlayer::setMode(DanceMode mode, double now) {
    if (mode == mode_) {
        return;
    }
    mode_ = mode;
    if (mode_ == DanceMode::Auto) {
        if (analysis_ && !analysis_->empty()) {
            director_.planAuto(*analysis_);
            autoNote_ = "plan built from BG Beat analysis";
        } else {
            autoNote_ = "Auto needs music: no track";
        }
    } else {
        // Continue with whatever is dancing now; the user takes over from here.
        const DanceFrame frame = director_.sample(now);
        director_.dropFuture(now);
        director_.setSelectedStyle(frame.style);
        autoNote_.clear();
    }
}

void DancePlayer::select(DanceStyle style, double now, bool immediate) {
    if (mode_ != DanceMode::Manual) {
        const DanceFrame frame = director_.sample(now);
        director_.dropFuture(now);
        director_.setSelectedStyle(frame.style);
        mode_ = DanceMode::Manual;
        autoNote_.clear();
    }
    director_.select(style, now, oneShot_ && style != DanceStyle::Idle, immediate);
}

void DancePlayer::startTestDance(double now) {
    mode_ = DanceMode::Manual;
    autoNote_.clear();
    director_.startTestDance(now);
}

void DancePlayer::startSequence(const std::vector<DanceStyle>& styles, double now,
                                double beatsPerStyle, bool immediate) {
    mode_ = DanceMode::Manual;
    autoNote_.clear();
    director_.startSequence(styles, now, beatsPerStyle, immediate);
}

DanceFrame DancePlayer::update(double mediaTime) {
    const bool seek = lastTime_ > -1.0e8 &&
                      (mediaTime < lastTime_ - 0.02 || mediaTime > lastTime_ + 0.5);
    if (seek && mode_ == DanceMode::Manual) {
        director_.reseed(mediaTime);
    }
    lastTime_ = mediaTime;
    return director_.sample(mediaTime);
}

} // namespace bgmedia::motion
