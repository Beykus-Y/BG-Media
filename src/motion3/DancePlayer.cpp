#include "bgmedia/motion3/DancePlayer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace bgmedia::motion {

const char* overlayModeName(OverlayMode mode) {
    switch (mode) {
    case OverlayMode::Off: return "OFF";
    case OverlayMode::Light: return "LIGHT";
    case OverlayMode::Full: return "FULL";
    case OverlayMode::HandsOnly: return "HANDS";
    }
    return "FULL";
}

namespace {

std::string trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

OverlayMode overlayFromInt(int value) {
    switch (value) {
    case 0: return OverlayMode::Off;
    case 1: return OverlayMode::Light;
    case 3: return OverlayMode::HandsOnly;
    default: return OverlayMode::Full;
    }
}

}  // namespace

bool saveMotion32Profile(const std::filesystem::path& path,
                         const Motion32Profile& profile, std::string* error) {
    if (path.empty()) {
        if (error) *error = "empty profile path";
        return false;
    }
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot open motion32 profile";
        return false;
    }
    out << "# BG Motion 3.2 host profile v1\n";
    out << "overlay=" << static_cast<int>(profile.overlayMode) << "\n";
    out << "spring=" << (profile.springEnabled ? 1 : 0) << "\n";
    out << "springIntensity=" << std::clamp(profile.springIntensity, 0.0f, 2.0f) << "\n";
    out << "springDamping=" << std::clamp(profile.springDamping, 0.0f, 1.0f) << "\n";
    out.flush();
    if (!out) {
        if (error) *error = "failed to write motion32 profile";
        return false;
    }
    return true;
}

bool loadMotion32Profile(const std::filesystem::path& path, Motion32Profile& profile,
                         std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open motion32 profile";
        return false;
    }
    Motion32Profile loaded;
    int parsed = 0;
    std::string line;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trimmed(line.substr(0, eq)));
        const std::string value = trimmed(line.substr(eq + 1));
        try {
            if (key == "overlay") {
                loaded.overlayMode = overlayFromInt(std::stoi(value));
                ++parsed;
            } else if (key == "spring") {
                loaded.springEnabled = !(value == "0" || lower(value) == "false");
                ++parsed;
            } else if (key == "springintensity") {
                loaded.springIntensity = std::clamp(std::stof(value), 0.0f, 2.0f);
                ++parsed;
            } else if (key == "springdamping") {
                loaded.springDamping = std::clamp(std::stof(value), 0.0f, 1.0f);
                ++parsed;
            }
        } catch (...) {
            continue;
        }
    }
    if (parsed == 0) {
        if (error) *error = "motion32 profile has no recognised keys";
        return false;
    }
    profile = loaded;
    return true;
}

std::size_t DancePlayer::loadLibrary(const std::filesystem::path& directory) {
    library_ = DanceLibrary{};
    library_.loadDirectory(directory);
    authored_.assign(library_.clips().begin(), library_.clips().end());
    footworkReports_.clear();
    replanAuthored();
    generated_ = ProceduralChoreography::showcase();
    for (GeneratedDance& dance : generated_) {
        overlays_[dance.clip.id] = dance.overlays;
        if (!library_.find(dance.clip.id)) {
            library_.add(dance.clip);
        }
    }
    director_.setLibrary(&library_);
    director_.reset(0.0);
    lastTime_ = -1.0e9;
    return library_.clips().size();
}

void DancePlayer::replanAuthored() {
    footworkReports_.clear();
    for (const HumanoidClip& raw : authored_) {
        if (!footworkPlan_) {
            library_.replace(raw);
            continue;
        }
        FootworkPlanOptions options;
        options.legs = legGeometry_;
        options.minGapCm = footworkGapCm_;
        FootworkPlanReport report;
        library_.replace(planClipFootwork(raw, options, &report));
        footworkReports_[raw.id] = report;
    }
}

void DancePlayer::setLegGeometry(const LegGeometry& geometry) {
    legGeometry_ = geometry;
    replanAuthored();
}

void DancePlayer::setFootworkGap(float gapCm) {
    footworkGapCm_ = std::clamp(gapCm, 0.0f, 8.0f);
    replanAuthored();
}

void DancePlayer::setFootworkPlanning(bool enabled) {
    if (footworkPlan_ != enabled) {
        footworkPlan_ = enabled;
        replanAuthored();
    }
}

const HumanoidClip* DancePlayer::authoredClip(const std::string& clipId) const {
    for (const HumanoidClip& clip : authored_) {
        if (clip.id == clipId) return &clip;
    }
    return nullptr;
}

const FootworkPlanReport* DancePlayer::footworkReport(const std::string& clipId) const {
    const auto it = footworkReports_.find(clipId);
    return it == footworkReports_.end() ? nullptr : &it->second;
}

void DancePlayer::setOverlayMode(OverlayMode mode) {
    overlayMode_ = mode;
}

const std::vector<PrimitiveCue>& DancePlayer::overlaysFor(const std::string& clipId) {
    auto it = overlays_.find(clipId);
    if (it != overlays_.end()) {
        return it->second;
    }
    std::vector<PrimitiveCue> plan;
    if (const HumanoidClip* clip = library_.find(clipId)) {
        const std::uint32_t seed =
            overlaySeed_ ^ static_cast<std::uint32_t>(clipId.size() * 2654435761u);
        const float density = std::clamp(0.25f + clip->energy * 0.8f, 0.25f, 1.0f);
        plan = planPrimitiveOverlays(*clip, seed, density, true);
    }
    return overlays_.emplace(clipId, std::move(plan)).first->second;
}

bool DancePlayer::selectClip(const std::string& clipId, double now, bool immediate) {
    if (!library_.find(clipId)) {
        return false;
    }
    if (mode_ != DanceMode::Manual) {
        const DanceFrame frame = director_.sample(now);
        director_.dropFuture(now);
        director_.setSelectedStyle(frame.style);
        mode_ = DanceMode::Manual;
        autoNote_.clear();
    }
    director_.selectClip(clipId, now, oneShot_, immediate);
    return true;
}

void DancePlayer::setGrid(BeatClock clock, double now) {
    if (mode_ == DanceMode::Auto) {
        // A constant clock is an explicit Motion Lab override; a grid derived
        // from the analysis must keep the rich timeline (accents/downbeat
        // confidence) instead of replacing it with a bare grid.
        if (clock.constantGrid()) {
            director_.setBeatClock(std::move(clock));
        } else if (analysis_ && !analysis_->empty()) {
            director_.setMusicalTimeline(MusicalTimeline::fromAnalysis(*analysis_));
        } else {
            director_.setBeatClock(std::move(clock));
        }
        return;
    }
    if (!clock.constantGrid() && analysis_ && !analysis_->empty()) {
        director_.rebase(now, MusicalTimeline::fromAnalysis(*analysis_));
        return;
    }
    director_.rebase(now, std::move(clock));
}

void DancePlayer::setMusicalTimeline(MusicalTimeline timeline, double now) {
    if (mode_ == DanceMode::Auto) {
        director_.setMusicalTimeline(std::move(timeline));
        return;
    }
    director_.rebase(now, std::move(timeline));
}

void DancePlayer::setAnalysis(const AnalysisResult* analysis) {
    setAnalysis(analysis, lastTime_ > -1.0e8 ? lastTime_ : 0.0);
}

void DancePlayer::setAnalysis(const AnalysisResult* analysis, double now) {
    analysis_ = analysis;
    if (mode_ == DanceMode::Auto) {
        if (analysis_ && !analysis_->empty()) {
            director_.planAuto(*analysis_);
            autoNote_ = "plan built from BG Beat analysis";
        } else {
            director_.reset(0.0);
            autoNote_ = "Auto needs music: no track";
        }
        return;
    }
    if (analysis_ && !analysis_->empty()) {
        director_.rebase(now, MusicalTimeline::fromAnalysis(*analysis_));
    } else {
        director_.rebase(now, MusicalTimeline::fromClock(director_.beatClock(), true));
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
    if (overlayMode_ == OverlayMode::Off) {
        return director_.sample(mediaTime);
    }
    CompositionOptions options;
    options.allowFootwork = overlayMode_ == OverlayMode::Full;
    options.allowHands = true;
    options.allowTorso = overlayMode_ != OverlayMode::HandsOnly;
    options.allowHead = overlayMode_ != OverlayMode::HandsOnly;
    options.maxUpperBodyLayers = overlayMode_ == OverlayMode::Light ? 2 : 3;

    // Overlays are composed per schedule segment, before the crossfade. The
    // first call is the newest (incoming) segment; its diagnostics are the ones
    // reported. The decorator only lives for this call, so it never outlives
    // `this`.
    struct Diag {
        int calls = 0;
        int accepted = 0;
        int rejected = 0;
        float weight = 0.0f;
        bool hands = false;
    } diag;
    director_.schedule().setPoseDecorator(
        [this, &options, &diag](ClipPose& pose, const ScheduleSegment& segment,
                                double clipBeat) {
            if (!segment.clip) {
                return;
            }
            const std::vector<PrimitiveCue>& cues = overlaysFor(segment.clip->id);
            if (cues.empty()) {
                return;
            }
            // The cue envelope must know how long the gesture really is: a short
            // cue performed at its full size is a snap, not a gesture.
            options.cueSecondsPerBeat =
                static_cast<float>(60.0 / std::max(20.0, segment.clip->bpm));
            const ComposedPose composed = composeMotion(
                pose, cues, clipBeat, segment.clip->beats, segment.clip->loop, options);
            pose = composed.pose;
            if (diag.calls++ == 0) {
                diag.accepted = composed.diagnostics.acceptedCues;
                diag.rejected = composed.diagnostics.rejectedByConflict +
                                composed.diagnostics.rejectedByContact +
                                composed.diagnostics.rejectedBySupport +
                                composed.diagnostics.rejectedByMask;
                diag.weight = composed.diagnostics.maxWeight;
                diag.hands = composed.pose.detailedHands;
            }
        });
    DanceFrame frame = director_.sample(mediaTime);
    director_.schedule().setPoseDecorator(nullptr);
    frame.overlayActive = diag.accepted;
    frame.overlayRejected = diag.rejected;
    frame.overlayWeight = diag.weight;
    frame.overlayHands = diag.hands;
    return frame;
}

} // namespace bgmedia::motion
