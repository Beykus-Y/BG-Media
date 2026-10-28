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

// A jump of the host clock (seek, long stall) is hidden by a short smootherstep blend
// from the last output pose. A plan or tempo change blends a little longer.
constexpr double kSeekBlendSeconds = 0.18;
constexpr double kPlanBlendSeconds = 0.5;
// Carried planted-foot offsets: paid off with time constant kSwingCarrySeconds while the
// shown foot swings, and kPlantedCarrySeconds while it stays planted.
constexpr double kSwingCarrySeconds = 0.08;
constexpr double kPlantedCarrySeconds = 3.0;

// Upper bound of the per-(clip, loop) overlay memo. Plans are pure, so clearing is harmless.
constexpr std::size_t kMaxOverlaySets = 256;

// Seed of one clip's cue set for one loop: the host seed, the clip id and the loop index.
std::uint32_t overlayLoopSeed(std::uint32_t hostSeed, const std::string& clipId,
                              std::int64_t loop) {
    const std::uint64_t key = variationMix(
        variationHash(clipId) ^ variationMix(static_cast<std::uint64_t>(hostSeed) + 0x51ull) ^
        variationMix(static_cast<std::uint64_t>(loop) + 0x7000ull));
    return static_cast<std::uint32_t>(key ^ (key >> 32));
}

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
    generatedOverlays_.clear();
    for (GeneratedDance& dance : generated_) {
        generatedOverlays_[dance.clip.id] = dance.overlays;
        if (!library_.find(dance.clip.id)) {
            library_.add(dance.clip);
        }
    }
    director_.setLibrary(&library_);
    director_.reset(0.0);
    lastTime_ = -1.0e9;
    blend_ = PoseBlend{};
    haveLastPose_ = false;
    lastJump_ = false;
    return library_.clips().size();
}

void DancePlayer::replanAuthored() {
    footworkReports_.clear();
    overlays_.clear();  // the cue plans read the (re-planned) clips
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
    return overlaysFor(clipId, 0);
}

const std::vector<PrimitiveCue>& DancePlayer::overlaysFor(const std::string& clipId,
                                                          std::int64_t loop) {
    if (const auto preset = generatedOverlays_.find(clipId); preset != generatedOverlays_.end()) {
        return preset->second;
    }
    const std::pair<std::string, std::int64_t> key{clipId, loop};
    if (const auto it = overlays_.find(key); it != overlays_.end()) {
        return it->second;
    }
    std::vector<PrimitiveCue> plan;
    if (const HumanoidClip* clip = library_.find(clipId)) {
        const std::uint32_t seed = overlayLoopSeed(overlaySeed_, clipId, loop);
        const float density = std::clamp(0.25f + clip->energy * 0.8f, 0.25f, 1.0f);
        plan = planPrimitiveOverlays(*clip, seed, density, true);
    }
    if (overlays_.size() >= kMaxOverlaySets) {
        overlays_.clear();
    }
    return overlays_.emplace(key, std::move(plan)).first->second;
}

bool DancePlayer::selectClip(const std::string& clipId, double now, bool immediate) {
    if (!library_.find(clipId)) {
        return false;
    }
    if (mode_ != DanceMode::Manual) {
        const DanceFrame frame = director_.sample(now);
        director_.dropFuture(now);
        director_.setSelectedClip(frame.clipId, frame.style);
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
            armIfMoved(director_.beatClock(), clock, now);
            director_.setBeatClock(std::move(clock));
        } else if (analysis_ && !analysis_->empty()) {
            const MusicalTimeline timeline = MusicalTimeline::fromAnalysis(*analysis_);
            armIfMoved(director_.beatClock(), timeline.grid(), now);
            director_.setMusicalTimeline(timeline);
        } else {
            armIfMoved(director_.beatClock(), clock, now);
            director_.setBeatClock(std::move(clock));
        }
        return;
    }
    if (!clock.constantGrid() && analysis_ && !analysis_->empty()) {
        const MusicalTimeline timeline = MusicalTimeline::fromAnalysis(*analysis_);
        armIfMoved(director_.beatClock(), timeline.grid(), now);
        director_.rebase(now, timeline);
        return;
    }
    armIfMoved(director_.beatClock(), clock, now);
    director_.rebase(now, std::move(clock));
}

void DancePlayer::setMusicalTimeline(MusicalTimeline timeline, double now) {
    if (mode_ == DanceMode::Auto) {
        armIfMoved(director_.beatClock(), timeline.grid(), now);
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
        armBlend(kPlanBlendSeconds, true);
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
        // The plan replaces the Manual schedule: the dancer carries on from the current pose.
        armBlend(kPlanBlendSeconds, true);
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
        director_.setSelectedClip(frame.clipId, frame.style);
        autoNote_.clear();
    }
}

void DancePlayer::select(DanceStyle style, double now, bool immediate) {
    if (mode_ != DanceMode::Manual) {
        const DanceFrame frame = director_.sample(now);
        director_.dropFuture(now);
        director_.setSelectedClip(frame.clipId, frame.style);
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

void DancePlayer::armBlend(double seconds, bool carryFeet) {
    if (!haveLastPose_) {
        return;
    }
    // A seek is a jump in the music; its short blend may move the feet. A plan or grid
    // change keeps dancing in place, so planted feet stay where they are.
    carry_.pending = carryFeet;
    if (!carryFeet) {
        carry_.remaining = {0.0, 0.0};
    }
    blend_.active = true;
    blend_.hold = true;  // the first frame after arming still shows the outgoing pose
    blend_.from = lastPose_;
    blend_.progress = 0.0;
    blend_.seconds = seconds;
}

void DancePlayer::armIfMoved(const BeatClock& before, const BeatClock& after, double now) {
    const double beatDelta = std::abs(before.beatAt(now) - after.beatAt(now));
    const double bpmDelta = std::abs(before.bpmAt(now) - after.bpmAt(now));
    if (beatDelta > 1.0e-4 || bpmDelta > 0.05) {
        armBlend(kPlanBlendSeconds, true);
    }
}

void DancePlayer::advanceBlend(double travelled) {
    if (!blend_.active || blend_.hold) {
        return;
    }
    if (travelled > 1.0e-9) {
        // The hand-over runs on the media time the host travelled, forwards or backwards.
        blend_.progress += travelled;
    } else {
        // A stopped media clock (paused host) would never finish the hand-over. Its last
        // frame shows the old pose, and the next one lands on the schedule.
        blend_.progress = blend_.seconds;
    }
}

void DancePlayer::applyBlend(DanceFrame& frame) {
    if (!blend_.active) {
        return;
    }
    const double u = blend_.seconds > 0.0 ? blend_.progress / blend_.seconds : 1.0;
    if (u >= 1.0) {
        blend_.active = false;
        return;
    }
    const auto w = static_cast<float>(std::clamp(u, 0.0, 1.0));
    frame.pose = blendPoses(blend_.from, frame.pose, w * w * (3.0f - 2.0f * w));
}

void DancePlayer::advanceFootCarry(double travelled) {
    for (std::size_t f = 0; f < 2; ++f) {
        if (carry_.remaining[f] <= 0.0) {
            continue;
        }
        if (travelled <= 1.0e-9) {
            // Paused host: like the body blend, land on the schedule.
            carry_.remaining[f] = 0.0;
            continue;
        }
        const double swing = 1.0 - std::clamp(static_cast<double>(carry_.contact[f]), 0.0, 1.0);
        const double rate = swing / kSwingCarrySeconds + 1.0 / kPlantedCarrySeconds;
        carry_.remaining[f] *= std::exp(-rate * travelled);
        if (carry_.remaining[f] < 1.0e-4) {
            carry_.remaining[f] = 0.0;
        }
    }
}

void DancePlayer::applyFootCarry(DanceFrame& frame) {
    if (carry_.pending && blend_.active) {
        // First frame of the blend: it still shows the outgoing pose, so the offset from
        // the schedule is exactly where the feet are now.
        carry_.pending = false;
        for (std::size_t f = 0; f < 2; ++f) {
            // Every foot is carried: one the new schedule swings right now pays its
            // offset off in that swing, one it keeps planted holds until its next swing.
            const ClipPose::Foot& from = blend_.from.foot[f];
            const ClipPose::Foot& to = frame.pose.foot[f];
            carry_.offset[f] = from.position - to.position;
            const float turn = from.yaw - to.yaw;  // turns: take the short way round
            carry_.yaw[f] = turn - std::round(turn);
            carry_.remaining[f] = 1.0;
        }
    }
}

DanceFrame DancePlayer::update(double mediaTime) {
    // A jump of the host clock is not a pose change the viewer should see: the new pose
    // is reached from the last one. The schedule itself is pure and needs no reseed; only
    // a time before its first segment (no segment covers it) rebuilds the Manual dance.
    const bool haveTime = lastTime_ > -1.0e8;
    const double travelled = haveTime ? std::abs(mediaTime - lastTime_) : 0.0;
    advanceBlend(travelled);
    advanceFootCarry(travelled);
    const bool jump = haveTime && (mediaTime < lastTime_ - 0.02 || mediaTime > lastTime_ + 0.5);
    // Only the first frame of a run of jumps (the start of a seek or a scrub) arms the blend.
    // A scrub frame after frame is followed by the schedule itself: re-arming per frame would
    // hold the start pose, and re-arming after the blend completed would hold a frame behind
    // every few frames.
    if (jump && !lastJump_) {
        armBlend(kSeekBlendSeconds);
    }
    const std::vector<ScheduleSegment>& segments = director_.schedule().segments();
    if (mode_ == DanceMode::Manual && !segments.empty() &&
        mediaTime < segments.front().startTime) {
        armBlend(kSeekBlendSeconds);
        director_.reseed(mediaTime);
    }
    lastTime_ = mediaTime;
    lastJump_ = jump;
    DanceFrame frame = sampleFrame(mediaTime);
    const std::array<ClipPose::Foot, 2> scheduleFeet = frame.pose.foot;
    applyFootCarry(frame);
    applyBlend(frame);
    for (std::size_t f = 0; f < 2; ++f) {
        if (carry_.remaining[f] > 0.0) {
            // Over the body blend too: the carried foot is the schedule's foot plus what is
            // left of the offset, never a lerp of two planted positions.
            const ClipPose::Foot& target = scheduleFeet[f];
            const auto k = static_cast<float>(carry_.remaining[f]);
            // Horizontal only: the height follows the body blend, so a foot the new
            // schedule lifts or lands never keeps a vertical offset (under the floor).
            frame.pose.foot[f].position.x = target.position.x + carry_.offset[f].x * k;
            frame.pose.foot[f].position.z = target.position.z + carry_.offset[f].z * k;
            frame.pose.foot[f].yaw = target.yaw + carry_.yaw[f] * k;
        }
        // The offset is paid off by the contact the viewer sees, so a foot shown planted
        // never moves.
        carry_.contact[f] = frame.pose.foot[f].contact;
    }
    blend_.hold = false;
    lastPose_ = frame.pose;
    haveLastPose_ = true;
    return frame;
}

DanceFrame DancePlayer::sampleFrame(double mediaTime) {
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
            const HumanoidClip& clip = *segment.clip;
            // Cues are planned per loop. A looping clip composes loop L at its beat inside the
            // loop, plus the tails of loop L-1 that spill past the seam (composeLoopCues). The
            // fade-in before beat 0 is loop 0. There is no crossfade between two loops' sets:
            // every cue envelope starts and ends at zero, so a change of set at the seam is
            // continuous, and a cue planned on the downbeat ramps in at full weight.
            std::int64_t loop = 0;
            double local = clipBeat;
            if (clip.loop && clip.beats > 0.0f) {
                loop = std::max<std::int64_t>(
                    0, static_cast<std::int64_t>(std::floor(clipBeat / clip.beats)));
                local = clipBeat - static_cast<double>(loop) * clip.beats;
            }
            // Copied: the lookup of loop L may evict the memo that a reference would point into.
            const std::vector<PrimitiveCue> previous =
                loop > 0 ? overlaysFor(clip.id, loop - 1) : std::vector<PrimitiveCue>{};
            const std::vector<PrimitiveCue>& current = overlaysFor(clip.id, loop);
            if (current.empty() && previous.empty()) {
                return;
            }
            const ComposedPose composed =
                composeLoopCues(pose, current, previous, local, clip.beats, options);
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
