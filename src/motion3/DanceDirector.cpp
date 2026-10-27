#include "bgmedia/motion3/DanceDirector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace bgmedia::motion {
namespace {

float smootherstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

double circularDistance(double a, double b, double period) {
    double d = std::fmod(std::abs(a - b), period);
    return std::min(d, period - d);
}

double wrapPositive(double value, double period) {
    if (!(period > 1e-9)) {
        return 0.0;
    }
    double wrapped = std::fmod(value, period);
    if (wrapped < 0.0) {
        wrapped += period;
    }
    return wrapped;
}

bool upperBodyBone(vrm::HumanoidBone bone) {
    switch (bone) {
    case vrm::HumanoidBone::Hips:
    case vrm::HumanoidBone::LeftUpperLeg:
    case vrm::HumanoidBone::LeftLowerLeg:
    case vrm::HumanoidBone::LeftFoot:
    case vrm::HumanoidBone::LeftToes:
    case vrm::HumanoidBone::RightUpperLeg:
    case vrm::HumanoidBone::RightLowerLeg:
    case vrm::HumanoidBone::RightFoot:
    case vrm::HumanoidBone::RightToes:
        return false;
    default:
        return true;
    }
}

struct Mapping {
    double linearBeat = 0.0;
    double beat = 0.0;
    double rate = 1.0;       // clip beats per music beat, including warp
    double warpRate = 1.0;   // dimensionless warp multiplier
    double deviation = 0.0;  // beat domain deviation
    bool warpActive = false;
    float accent = 0.0f;
};

float accentAtNextBoundary(const ScheduleSegment& segment, double musicBeat,
                           const BeatClock& clock, const MusicalTimeline* timeline,
                           float* confidence) {
    const double nextBeat = std::floor(musicBeat + 1e-9) + 1.0;
    const double nextTime = clock.timeAtBeat(nextBeat);
    float accent = 0.0f;
    if (timeline) {
        accent = std::max(timeline->accentAt(nextTime, 0.12),
                          timeline->accentAt(clock.timeAtBeat(std::floor(musicBeat + 1e-9)),
                                             0.10));
        const int nextIndex = static_cast<int>(std::lround(nextBeat));
        accent = std::max(accent, timeline->beatAccent(nextIndex));
    }
    // A Motion Lab / clip-authored accent is a legitimate musical event too.
    const double entry = segment.entryClipBeat;
    const double alignBeat = clock.beatAt(segment.alignTime);
    const double nextClipBeat = entry + (nextBeat - alignBeat) * segment.rate;
    const double currentClipBeat = entry + (musicBeat - alignBeat) * segment.rate;
    float clipAccent = 0.0f;
    if (segment.clip) {
        clipAccent = std::max(clipAccentStrength(*segment.clip, nextClipBeat),
                              0.35f * clipAccentStrength(*segment.clip, currentClipBeat));
        accent = std::max(accent, clipAccent);
    }
    if (confidence) {
        const int nextIndex = static_cast<int>(std::lround(nextBeat));
        const float beatConfidence =
            timeline ? timeline->beatConfidence(nextIndex) : 0.0f;
        *confidence = clipAccent > 0.05f
                          ? 1.0f
                          : std::clamp(std::max(0.35f, beatConfidence), 0.0f, 1.0f);
    }
    return std::clamp(accent, 0.0f, 1.0f);
}

Mapping mapSegment(const ScheduleSegment& segment, double mediaTime,
                   const BeatClock& clock, const MusicalTimeline* timeline,
                   const WarpProfile& warp, bool footContact) {
    Mapping mapping;
    if (!segment.clip) {
        return mapping;
    }
    if (!segment.clip->tempoSync) {
        mapping.linearBeat = segment.entryClipBeat +
                             (mediaTime - segment.alignTime) * segment.clip->bpm / 60.0;
        mapping.beat = mapping.linearBeat;
        return mapping;
    }
    const double musicBeat = clock.beatAt(mediaTime);
    const double alignBeat = clock.beatAt(segment.alignTime);
    mapping.linearBeat = segment.entryClipBeat + (musicBeat - alignBeat) * segment.rate;
    mapping.beat = mapping.linearBeat;
    mapping.rate = segment.rate;
    if (warp.expression == MotionExpression::Off || warp.intensity <= 0.0f) {
        return mapping;
    }
    float accentConfidence = 0.35f;
    const float accent =
        accentAtNextBoundary(segment, musicBeat, clock, timeline, &accentConfidence);
    if (timeline) {
        const double nextTime =
            clock.timeAtBeat(std::floor(musicBeat + 1e-9) + 1.0);
        accentConfidence = std::max(
            accentConfidence, timeline->accentConfidenceAt(nextTime, 0.12));
    }
    const float bass = timeline
                           ? timeline->bassAt(clock.timeAtBeat(
                                                  std::floor(musicBeat + 1e-9) + 1.0),
                                              0.12)
                           : 0.0f;
    // Fade the warp in after a seek/rebase/transition, and fade it out around
    // silence. This keeps the correction zero at the segment's start and avoids
    // a phase jump when a warp switches on or off.
    const double sinceAlign = musicBeat - alignBeat;
    const double ramp = std::clamp(sinceAlign, 0.0, 1.0);
    const double silence = timeline ? timeline->silenceWeightAt(mediaTime, 0.12) : 1.0;
    const double gate = ramp * silence;
    const float gatedAccent = static_cast<float>(accent * gate);
    const float gatedBass = static_cast<float>(bass * gate);
    if (gatedAccent <= 1e-4f && gatedBass <= 1e-4f) {
        return mapping;
    }
    WarpSample input;
    input.musicalBeat = musicBeat;
    input.accent = gatedAccent;
    input.bass = gatedBass;
    input.confidence = accentConfidence;
    input.footContact = footContact;
    input.silent = false;
    input.allow = true;
    const WarpResult warped = MusicalTimeWarp::sample(input, warp);
    if (!warped.active) {
        return mapping;
    }
    mapping.deviation = warped.deviation;
    mapping.beat = segment.entryClipBeat +
                   (musicBeat + mapping.deviation - alignBeat) * segment.rate;
    mapping.warpRate = warped.rate;
    mapping.rate = segment.rate * warped.rate;
    mapping.warpActive = true;
    mapping.accent = accent;
    return mapping;
}

bool anyFootContact(const ClipPose& pose) {
    for (const ClipPose::Foot& foot : pose.foot) {
        if (foot.contact >= 0.5f) {
            return true;
        }
        switch (foot.phase) {
        case FootPhase::Stance:
        case FootPhase::Pivot:
        case FootPhase::Plant:
            return true;
        default:
            break;
        }
    }
    return false;
}

ClipPose adaptPose(const ClipPose& stable, const ClipPose& warped, bool fullBody,
                   bool contact) {
    ClipPose out = stable;
    const bool moveLower = fullBody && !contact;
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        const auto bone = static_cast<vrm::HumanoidBone>(b);
        if (upperBodyBone(bone) || moveLower) {
            out.bone[b] = warped.bone[b];
        }
    }
    if (moveLower) {
        out.hips = warped.hips;
    } else {
        out.hips = stable.hips;
    }
    out.rootX = stable.rootX;
    out.rootZ = stable.rootZ;
    out.rootYaw = stable.rootYaw;
    out.foot = stable.foot;
    out.hand = warped.hand;
    out.energy = stable.energy;
    out.accent = std::max(stable.accent, warped.accent);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// DanceSchedule
// ---------------------------------------------------------------------------

void DanceSchedule::append(ScheduleSegment segment) {
    if (!segment.clip) {
        return;
    }
    if (!segments_.empty() && segment.startTime <= segments_.back().startTime) {
        segment.startTime = segments_.back().startTime + 1e-3;
    }
    segments_.push_back(std::move(segment));
}

void DanceSchedule::truncateAfter(double time) {
    while (segments_.size() > 1 && segments_.back().startTime > time) {
        segments_.pop_back();
    }
}

void DanceSchedule::pruneBefore(double time, std::size_t keepAtLeast) {
    // Drop segments whose blend finished long before `time`; the newest
    // surviving one is the new base and no longer needs a predecessor.
    while (segments_.size() > keepAtLeast &&
           segments_[1].startTime + segments_[1].fadeSeconds < time - 5.0) {
        segments_.erase(segments_.begin());
    }
}

double DanceSchedule::segmentClipBeat(const ScheduleSegment& segment, double mediaTime,
                                      const BeatClock& clock) {
    if (!segment.clip) {
        return 0.0;
    }
    if (!segment.clip->tempoSync) {
        return segment.entryClipBeat +
               (mediaTime - segment.alignTime) * segment.clip->bpm / 60.0;
    }
    return segment.entryClipBeat +
           (clock.beatAt(mediaTime) - clock.beatAt(segment.alignTime)) * segment.rate;
}

double DanceSchedule::segmentClipBeat(const ScheduleSegment& segment, double mediaTime,
                                      const MusicalTimeline& timeline,
                                      const WarpProfile& warp, double* rate,
                                      bool* warpActive, double* deviation) {
    const Mapping mapping =
        mapSegment(segment, mediaTime, timeline.grid(), &timeline, warp, false);
    if (rate) {
        *rate = mapping.rate;
    }
    if (warpActive) {
        *warpActive = mapping.warpActive;
    }
    if (deviation) {
        *deviation = mapping.deviation;
    }
    return mapping.beat;
}

ClipPose DanceSchedule::poseOf(std::size_t index, double mediaTime,
                               const MusicalTimeline& timeline,
                               const WarpProfile& warp, float* transition) const {
    const ScheduleSegment& segment = segments_[index];
    const Mapping stable = mapSegment(segment, mediaTime, timeline.grid(), &timeline,
                                      WarpProfile{MotionExpression::Off, 0.0f, true, 0.0f},
                                      false);
    ClipPose linear = sampleClip(*segment.clip, stable.linearBeat);
    const bool contact = anyFootContact(linear);

    Mapping mapped = stable;
    ClipPose own = linear;
    if (warp.expression != MotionExpression::Off && warp.intensity > 0.0f) {
        mapped = mapSegment(segment, mediaTime, timeline.grid(), &timeline, warp, contact);
        if (mapped.warpActive &&
            std::abs(mapped.beat - stable.linearBeat) > 1e-6) {
            const ClipPose warped = sampleClip(*segment.clip, mapped.beat);
            own = adaptPose(linear, warped, !warp.upperBodyOnly, contact);
        }
    }
    if (index > 0 && segment.fadeSeconds > 1e-4) {
        const double u = (mediaTime - segment.startTime) / segment.fadeSeconds;
        if (u < 1.0) {
            if (transition) {
                *transition = static_cast<float>(std::clamp(u, 0.0, 1.0));
            }
            if (u <= 0.0) {
                return poseOf(index - 1, mediaTime, timeline, warp, nullptr);
            }
            const ClipPose previous = poseOf(index - 1, mediaTime, timeline, warp, nullptr);
            return blendPoses(previous, own, smootherstep(static_cast<float>(u)));
        }
    }
    return own;
}

DanceFrame DanceSchedule::sample(double mediaTime, const BeatClock& clock) const {
    // Grid-only timeline for the legacy entry point. It keeps the exact clock
    // (including its bar offset), so diagnostics remain meaningful.
    MusicalTimeline timeline = MusicalTimeline::fromClock(clock, true);
    WarpProfile off;
    off.expression = MotionExpression::Off;
    return sample(mediaTime, clock, timeline, off);
}

DanceFrame DanceSchedule::sample(double mediaTime, const BeatClock& clock,
                                 const MusicalTimeline& timeline,
                                 const WarpProfile& warp) const {
    DanceFrame frame;
    frame.pose = neutralClipPose();
    frame.musicBeat = clock.beatAt(mediaTime);
    frame.beatsPerBar = timeline.beatsPerBar();
    frame.meterConfidence = timeline.meterConfidence();
    frame.downbeatConfidence = timeline.downbeatConfidence();
    frame.barPhaseTrusted = timeline.downbeatTrusted();
    if (segments_.empty()) {
        return frame;
    }
    std::size_t k = 0;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        if (segments_[i].startTime <= mediaTime) {
            k = i;
        } else {
            break;
        }
    }
    float transition = -1.0f;
    frame.pose = poseOf(k, mediaTime, timeline, warp, &transition);
    frame.transitionProgress = transition;

    const ScheduleSegment& current = segments_[k];
    const ScheduleSegment* shown = &current;
    const ScheduleSegment* next = k + 1 < segments_.size() ? &segments_[k + 1] : nullptr;
    if (transition >= 0.0f && transition < 0.5f && k > 0) {
        // While crossfading the outgoing clip is still "current".
        shown = &segments_[k - 1];
        next = &current;
    }
    frame.clipId = shown->clip->id;
    frame.clipName = shown->clip->name;
    frame.style = shown->clip->style;
    frame.nextClipId = next ? next->clip->id : std::string();
    frame.reason = current.reason;
    const Mapping mapping = mapSegment(*shown, mediaTime, clock, &timeline, warp, false);
    frame.clipBeat = mapping.beat;
    frame.unwarpedClipBeat = mapping.linearBeat;
    frame.warpDeviationBeats = mapping.deviation;
    frame.warpRate = mapping.warpRate;
    frame.warpActive = mapping.warpActive;
    frame.musicAccent = std::max(
        mapping.accent, timeline.accentAt(mediaTime, 0.10));
    frame.bassStrength = timeline.bassAt(mediaTime, 0.10);
    const double beat = mapping.beat;
    const double length = std::max(1.0, static_cast<double>(shown->clip->beats));
    double wrapped = std::fmod(beat, length);
    if (wrapped < 0.0) {
        wrapped += length;
    }
    if (!shown->loop && beat >= length) {
        frame.oneShotFinished = true;
        wrapped = length;
    }
    frame.phase = static_cast<float>(wrapped / length);
    frame.bpm = clock.bpmAt(mediaTime);
    if (shown->clip->tempoSync) {
        const double effective = frame.bpm * shown->rate;
        frame.playbackSpeed = static_cast<float>(effective / shown->clip->bpm);
        frame.effectiveSpeed = static_cast<float>(
            frame.bpm * mapping.rate / shown->clip->bpm);
        frame.speedLimited =
            effective < shown->clip->bpmMin - 0.5 || effective > shown->clip->bpmMax + 0.5;
    } else {
        frame.playbackSpeed = 1.0f;
        frame.effectiveSpeed = 1.0f;
    }
    frame.beatInBar = timeline.beatInBarAt(mediaTime);
    frame.barIndex = timeline.barAt(mediaTime);
    frame.downbeat = timeline.downbeatAt(mediaTime);
    frame.alignmentErrorMs = shown->alignmentErrorSeconds * 1000.0;
    return frame;
}

// ---------------------------------------------------------------------------
// DanceDirector
// ---------------------------------------------------------------------------

void DanceDirector::setBeatClock(BeatClock clock) {
    clock_ = std::move(clock);
    timeline_ = MusicalTimeline::fromClock(clock_, true);
}

void DanceDirector::setMusicalTimeline(MusicalTimeline timeline) {
    timeline_ = std::move(timeline);
    clock_ = timeline_.grid();
}

const HumanoidClip* DanceDirector::clipFor(DanceStyle style) const {
    return library_ ? library_->forStyle(style) : nullptr;
}

double DanceDirector::chooseRate(const HumanoidClip& clip, double musicBpm, bool* limited) {
    static constexpr double kLevels[] = {1.0, 0.5, 2.0};
    double best = 1.0;
    double bestScore = 1e9;
    bool bestInRange = false;
    double level1Score = 1e9;
    bool level1InRange = false;
    for (double level : kLevels) {
        const double effective = musicBpm * level;
        const bool inRange =
            effective >= clip.bpmMin - 0.5 && effective <= clip.bpmMax + 0.5;
        // A soft range penalty keeps a slightly-fast 1x reading instead of
        // forcing a large half-time jump (180 BPM Energetic -> 90 BPM). The
        // clip still reports speedLimited, so the choice is honest.
        double rangePenalty = 0.0;
        if (effective > clip.bpmMax) {
            rangePenalty = 2.0 * std::log(effective / std::max(1.0, clip.bpmMax));
        } else if (effective < clip.bpmMin) {
            rangePenalty = 2.0 * std::log(std::max(1.0, clip.bpmMin) / effective);
        }
        const double score =
            std::abs(std::log(effective / clip.bpm)) + rangePenalty;
        if (level == 1.0) {
            level1Score = score;
            level1InRange = inRange;
        }
        if (score < bestScore) {
            bestScore = score;
            best = level;
            bestInRange = inRange;
        }
    }
    // Hysteresis: do not jump to 0.5/2.0 when 1x is within a small score
    // margin. A metric level is a musical decision, not a per-frame reaction.
    if (best != 1.0 && level1Score <= bestScore + 0.05) {
        best = 1.0;
        bestScore = level1Score;
        bestInRange = level1InRange;
    }
    if (limited) {
        *limited = !bestInRange;
    }
    return best;
}

double DanceDirector::fadeFor(double atTime) const {
    const double bpm = std::max(30.0, clock_.bpmAt(atTime));
    return std::clamp(2.0 * 60.0 / bpm, 0.45, 1.1);
}

double DanceDirector::entryTimeFor(const HumanoidClip& clip, double now,
                                   bool* alignedToDownbeat,
                                   double* alignmentError) const {
    if (alignedToDownbeat) {
        *alignedToDownbeat = false;
    }
    if (alignmentError) {
        *alignmentError = 0.0;
    }
    if (!clip.tempoSync) {
        return now;
    }
    const int clipBar = std::max(1, clip.beatsPerBar);
    const int meter = std::max(1, clock_.beatsPerBar());
    const double fromBeat = std::ceil(clock_.beatAt(now + 0.10));
    double firstCompatible = fromBeat;
    bool haveCompatible = false;
    for (int k = 0; k <= meter; ++k) {
        const double beat = fromBeat + k;
        const int inBar = clock_.beatInBar(beat);
        if (clip.entryBeats.empty()) {
            if (inBar == 0) {
                if (alignedToDownbeat) {
                    *alignedToDownbeat = timeline_.downbeatTrusted();
                }
                return clock_.timeAtBeat(beat);
            }
            continue;
        }
        for (float entry : clip.entryBeats) {
            const int entryIndex = static_cast<int>(std::lround(entry));
            if (entryIndex % clipBar != inBar % clipBar) {
                continue;
            }
            const bool downbeatEntry = timeline_.downbeatTrusted() && inBar == 0 &&
                                       entryIndex % clipBar == 0;
            if (downbeatEntry) {
                if (alignedToDownbeat) {
                    *alignedToDownbeat = true;
                }
                return clock_.timeAtBeat(beat);
            }
            if (!haveCompatible) {
                firstCompatible = beat;
                haveCompatible = true;
            }
            break;
        }
    }
    if (haveCompatible) {
        return clock_.timeAtBeat(firstCompatible);
    }
    return clock_.timeAtBeat(fromBeat);
}

double DanceDirector::nextAlignmentTime(double now) const {
    const HumanoidClip* clip = clipFor(selected_);
    if (!clip) {
        return now;
    }
    return entryTimeFor(*clip, now, nullptr, nullptr);
}

ScheduleSegment DanceDirector::makeSegment(const HumanoidClip& clip, double startTime,
                                           double alignTime, double fadeSeconds) const {
    ScheduleSegment segment;
    segment.clip = &clip;
    segment.startTime = startTime;
    segment.alignTime = alignTime;
    segment.fadeSeconds = fadeSeconds;
    segment.loop = true;
    segment.meter = timeline_.beatsPerBar();
    segment.barPhaseTrusted = timeline_.downbeatTrusted();
    if (!clip.tempoSync) {
        segment.rate = 1.0;
        segment.entryClipBeat = 0.0;
        segment.alignedToDownbeat = false;
        return segment;
    }
    segment.rate = chooseRate(clip, clock_.bpmAt(alignTime));
    const double beatPos = clock_.beatAt(alignTime);
    const int inBar = clock_.beatInBar(beatPos);
    const double fraction = beatPos - std::floor(beatPos + 1e-6);
    const double barLength = std::max(1, clip.beatsPerBar);
    const double desired =
        std::fmod((inBar + std::max(0.0, fraction)) * segment.rate, barLength);
    double best = 0.0;
    double bestDistance = 1e9;
    std::vector<float> entries = clip.entryBeats;
    if (entries.empty()) {
        entries.push_back(0.0f);
    }
    std::sort(entries.begin(), entries.end());
    for (float entry : entries) {
        const double d = circularDistance(
            std::fmod(static_cast<double>(entry), barLength), desired, barLength);
        if (d < bestDistance - 1e-6) {
            bestDistance = d;
            best = entry;
        }
    }
    segment.entryClipBeat = best;
    segment.alignedToDownbeat =
        timeline_.downbeatTrusted() && inBar == 0 && fraction < 1e-4 &&
        std::abs(std::fmod(best, barLength)) < 1e-4;
    return segment;
}

void DanceDirector::reset(double now) {
    schedule_.clear();
    selected_ = DanceStyle::Idle;
    if (const HumanoidClip* idle = clipFor(DanceStyle::Idle)) {
        ScheduleSegment segment;
        segment.clip = idle;
        segment.startTime = now - 1.0e6;
        segment.alignTime = now;
        segment.fadeSeconds = 0.0;
        segment.reason = "idle";
        segment.meter = timeline_.beatsPerBar();
        segment.barPhaseTrusted = timeline_.downbeatTrusted();
        schedule_.append(segment);
    }
}

void DanceDirector::reseed(double now) {
    const HumanoidClip* clip = clipFor(selected_);
    if (!clip) {
        clip = clipFor(DanceStyle::Idle);
    }
    schedule_.clear();
    if (!clip) {
        return;
    }
    ScheduleSegment segment = makeSegment(*clip, now - 1.0e6, now, 0.0);
    if (clip->tempoSync) {
        const double beatPos = clock_.beatAt(now);
        const int inBar = clock_.beatInBar(beatPos);
        const double fraction = beatPos - std::floor(beatPos + 1e-6);
        const double barLength = std::max(1, clip->beatsPerBar);
        if (timeline_.downbeatTrusted()) {
            segment.entryClipBeat =
                wrapPositive((inBar + std::max(0.0, fraction)) * segment.rate, barLength);
            segment.alignedToDownbeat = inBar == 0 && fraction < 1e-4;
        } else {
            // No trusted bar: preserve the fractional beat phase only.
            segment.entryClipBeat =
                wrapPositive(std::max(0.0, fraction) * segment.rate, barLength);
            segment.alignedToDownbeat = false;
        }
        segment.barPhaseTrusted = timeline_.downbeatTrusted();
    } else {
        segment.entryClipBeat = 0.0;
    }
    segment.reason = "re-aligned after seek";
    schedule_.append(segment);
}

void DanceDirector::rebase(double now, BeatClock newClock) {
    if (schedule_.empty()) {
        setBeatClock(std::move(newClock));
        return;
    }
    const auto& segments = schedule_.segments();
    std::size_t k = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].startTime <= now) {
            k = i;
        }
    }
    ScheduleSegment segment = segments[k];
    segment.entryClipBeat = DanceSchedule::segmentClipBeat(segment, now, clock_);
    setBeatClock(std::move(newClock));
    segment.alignTime = now;
    segment.startTime = now - 1.0e6;
    segment.fadeSeconds = 0.0;
    if (segment.clip->tempoSync) {
        segment.rate = chooseRate(*segment.clip, clock_.bpmAt(now));
    }
    segment.meter = timeline_.beatsPerBar();
    segment.barPhaseTrusted = timeline_.downbeatTrusted();
    schedule_.clear();
    schedule_.append(segment);
}

void DanceDirector::rebase(double now, MusicalTimeline newTimeline) {
    if (schedule_.empty()) {
        setMusicalTimeline(std::move(newTimeline));
        return;
    }
    const auto& segments = schedule_.segments();
    std::size_t k = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].startTime <= now) {
            k = i;
        }
    }
    ScheduleSegment segment = segments[k];
    segment.entryClipBeat = DanceSchedule::segmentClipBeat(segment, now, clock_);
    const double previousBeat = segment.entryClipBeat;
    setMusicalTimeline(std::move(newTimeline));
    segment.alignTime = now;
    segment.startTime = now - 1.0e6;
    segment.fadeSeconds = 0.0;
    if (segment.clip->tempoSync) {
        segment.rate = chooseRate(*segment.clip, clock_.bpmAt(now));
    }
    segment.entryClipBeat = previousBeat;
    segment.meter = timeline_.beatsPerBar();
    segment.barPhaseTrusted = timeline_.downbeatTrusted();
    schedule_.clear();
    schedule_.append(segment);
}

void DanceDirector::select(DanceStyle style, double now, bool oneShot, bool immediate) {
    const HumanoidClip* clip = clipFor(style);
    if (!clip) {
        return;
    }
    if (schedule_.empty()) {
        reset(now - 0.01);
    }
    selected_ = style;
    schedule_.truncateAfter(now);

    double align = now;
    bool aligned = false;
    double alignmentError = 0.0;
    if (!immediate && style != DanceStyle::Idle && clip->tempoSync) {
        align = entryTimeFor(*clip, now, &aligned, &alignmentError);
    }
    const double fade = immediate ? 0.0 : fadeFor(align);
    const double start = std::max(now, align - fade * 0.5);
    ScheduleSegment segment = makeSegment(*clip, start, align, fade);
    segment.loop = !oneShot;
    segment.alignedToDownbeat = aligned || segment.alignedToDownbeat;
    segment.alignmentErrorSeconds = alignmentError;
    segment.reason = oneShot ? "manual one-shot" : "manual";
    schedule_.append(segment);

    if (oneShot && clip->tempoSync) {
        const ScheduleSegment& placed = schedule_.segments().back();
        const double endBeat =
            clock_.beatAt(placed.alignTime) + (clip->beats - placed.entryClipBeat) / placed.rate;
        const double endTime = clock_.timeAtBeat(endBeat);
        if (const HumanoidClip* idle = clipFor(DanceStyle::Idle)) {
            const double idleFade = fadeFor(endTime);
            ScheduleSegment back = makeSegment(*idle, endTime - idleFade * 0.5, endTime,
                                               idleFade);
            back.reason = "one-shot finished";
            schedule_.append(back);
        }
    }
    schedule_.pruneBefore(now);
}

void DanceDirector::startTestDance(double now, double beatsPerStyle) {
    startSequence({DanceStyle::Groove, DanceStyle::HipHop, DanceStyle::SideStep,
                   DanceStyle::Energetic, DanceStyle::Idle},
                  now, beatsPerStyle, false);
}

void DanceDirector::startSequence(const std::vector<DanceStyle>& styles, double now,
                                  double beatsPerStyle, bool immediate) {
    if (schedule_.empty()) {
        reset(now - 0.01);
    }
    schedule_.truncateAfter(now);
    double beat = std::ceil(clock_.beatAt(now + 0.10));
    bool firstAligned = false;
    if (immediate) {
        beat = clock_.beatAt(now);
    } else {
        for (DanceStyle style : styles) {
            if (const HumanoidClip* firstClip = clipFor(style)) {
                if (style != DanceStyle::Idle) {
                    const double align = entryTimeFor(*firstClip, now, &firstAligned, nullptr);
                    beat = clock_.beatAt(align);
                }
                break;
            }
        }
    }
    const int meter = std::max(1, clock_.beatsPerBar());
    double step = std::max(1.0, beatsPerStyle);
    if (timeline_.downbeatTrusted()) {
        const double bars = std::max(1.0, std::round(step / static_cast<double>(meter)));
        step = bars * static_cast<double>(meter);
    }
    bool first = true;
    for (DanceStyle style : styles) {
        const HumanoidClip* clip = clipFor(style);
        if (!clip) {
            continue;
        }
        const double align = clock_.timeAtBeat(beat);
        const double fade = fadeFor(align);
        const double start = first ? (immediate ? now : std::max(now, align - fade * 0.5))
                                   : align - fade * 0.5;
        ScheduleSegment segment = makeSegment(*clip, start, align, fade);
        if (first && immediate) {
            segment.fadeSeconds = 0.0;
        }
        segment.alignedToDownbeat = first ? (firstAligned || segment.alignedToDownbeat)
                                          : segment.alignedToDownbeat;
        segment.reason = "sequence";
        schedule_.append(segment);
        first = false;
        selected_ = style;
        beat += step;
    }
}

std::string DanceDirector::describeSchedule() const {
    std::string out;
    char line[192];
    for (const ScheduleSegment& seg : schedule_.segments()) {
        if (seg.startTime < -1.0e5) {
            std::snprintf(line, sizeof line, "  start      %-10s %s%s\n",
                          seg.clip->id.c_str(), seg.reason.c_str(),
                          seg.alignedToDownbeat ? "  [downbeat]" : "");
        } else {
            std::snprintf(line, sizeof line, "  %7.2f s  %-10s rate %.1f  %s%s\n",
                          seg.alignTime, seg.clip->id.c_str(), seg.rate,
                          seg.reason.c_str(),
                          seg.alignedToDownbeat ? "  [downbeat]" : "");
        }
        out += line;
    }
    return out;
}

DanceFrame DanceDirector::sample(double mediaTime) const {
    return schedule_.sample(mediaTime, clock_, timeline_, warp_);
}

void DanceDirector::planAuto(const AnalysisResult& analysis, const AutoPlanOptions& options) {
    setMusicalTimeline(MusicalTimeline::fromAnalysis(analysis));
    reset(0.0);
    const HumanoidClip* groove = clipFor(DanceStyle::Groove);
    if (!groove) {
        return;
    }
    const std::size_t beatCount = analysis.beats.size();
    const bool confident = beatCount >= 8 && clock_.confidence() >= options.minConfidence;
    const bool trustedBar = timeline_.downbeatTrusted();
    std::size_t firstDownbeat = 0;
    if (trustedBar) {
        for (std::size_t i = 0; i < beatCount && i < 16; ++i) {
            if (analysis.beats[i].downbeat) {
                firstDownbeat = i;
                break;
            }
        }
    }
    const double trackEnd = analysis.info.duration;
    const double phrase = trustedBar && options.phraseBars > 0
                              ? static_cast<double>(options.phraseBars * timeline_.beatsPerBar())
                              : std::max(4.0, options.phraseBeats);
    const double firstBeat = static_cast<double>(firstDownbeat);
    const double lastBeat = std::max(firstBeat + 1.0, clock_.beatAt(trackEnd));
    const int phraseCount = std::max(1, static_cast<int>(std::ceil((lastBeat - firstBeat) / phrase)));

    // Phrase statistics.
    struct PhraseInfo {
        double t0 = 0, t1 = 0;
        float onsets = 0, bass = 0, section = -1, silent = 0, loud = 0;
    };
    std::vector<PhraseInfo> phrases(static_cast<std::size_t>(phraseCount));
    for (int p = 0; p < phraseCount; ++p) {
        PhraseInfo& info = phrases[static_cast<std::size_t>(p)];
        info.t0 = clock_.timeAtBeat(firstBeat + p * phrase);
        info.t1 = clock_.timeAtBeat(firstBeat + (p + 1) * phrase);
        const double span = std::max(0.1, std::min(info.t1, trackEnd) - info.t0);
        for (const Onset& onset : analysis.onsets) {
            if (onset.time >= info.t0 && onset.time < info.t1) {
                info.onsets += onset.strength;
            }
        }
        info.onsets /= static_cast<float>(span);
        for (const BassAccent& bass : analysis.bassAccents) {
            if (bass.time >= info.t0 && bass.time < info.t1) {
                info.bass += bass.strength * std::max(0.3f, bass.confidence);
            }
        }
        info.bass /= static_cast<float>(span);
        if (analysis.spectrogram.valid() && analysis.spectrogram.framesPerSecond > 0.0) {
            // mean level (dB-scaled 0..255) over the phrase: a loudness proxy that tracks
            // what the ear hears better than a peak envelope
            const Spectrogram& spec = analysis.spectrogram;
            const std::size_t from = static_cast<std::size_t>(
                std::max(0.0, info.t0 * spec.framesPerSecond));
            const std::size_t to = std::min<std::size_t>(
                spec.frameCount,
                static_cast<std::size_t>(std::max(0.0, info.t1 * spec.framesPerSecond)));
            double sum = 0.0;
            std::size_t count = 0;
            for (std::size_t f = from; f < to; ++f) {
                for (int band = 0; band < spec.bands; ++band) {
                    sum += spec.magnitudes[f * static_cast<std::size_t>(spec.bands) +
                                           static_cast<std::size_t>(band)];
                    ++count;
                }
            }
            info.loud = count ? static_cast<float>(sum / static_cast<double>(count) / 255.0)
                              : 0.0f;
        }
        const double mid = 0.5 * (info.t0 + info.t1);
        for (const Section& section : analysis.sections) {
            if (mid >= section.start && mid < section.end) {
                info.section = section.energy;
            }
        }
        for (const SilenceRegion& silence : analysis.silences) {
            const double overlap = std::min(info.t1, silence.end) - std::max(info.t0, silence.start);
            if (overlap > 0.0) {
                info.silent += static_cast<float>(overlap / span);
            }
        }
        // BG Beat reports the gaps between sparse hits as silence too; a phrase only
        // counts as silent when it also has (almost) no onsets.
        if (info.onsets > 0.4f) {
            info.silent = 0.0f;
        }
    }
    const auto percentile = [&](auto member) {
        std::vector<float> values;
        for (const PhraseInfo& info : phrases) values.push_back(info.*member);
        std::sort(values.begin(), values.end());
        return std::max(1e-3f, values[values.size() * 9 / 10]);
    };
    const float onsetRef = percentile(&PhraseInfo::onsets);
    const float bassRef = percentile(&PhraseInfo::bass);
    const float loudRef = percentile(&PhraseInfo::loud);
    float loudMin = loudRef;
    for (const PhraseInfo& info : phrases) {
        loudMin = std::min(loudMin, info.loud);
    }
    const bool haveLoudness = analysis.spectrogram.valid();

    struct Plan {
        DanceStyle style;
        std::string reason;
    };
    std::vector<Plan> plan;
    DanceStyle previous = DanceStyle::Groove;
    int alternate = 0;
    for (const PhraseInfo& info : phrases) {
        const float onset = std::min(1.0f, info.onsets / onsetRef);
        const float bass = std::min(1.0f, info.bass / bassRef);
        const float loud =
            std::clamp((info.loud - loudMin) / std::max(1e-3f, loudRef - loudMin), 0.0f, 1.0f);
        float energy;
        if (info.section >= 0.0f) {
            energy = haveLoudness ? 0.4f * info.section + 0.25f * onset + 0.2f * loud +
                                        0.15f * bass
                                  : 0.5f * info.section + 0.35f * onset + 0.15f * bass;
        } else {
            energy = haveLoudness ? 0.4f * loud + 0.35f * onset + 0.25f * bass
                                  : 0.7f * onset + 0.3f * bass;
        }
        energy = std::clamp(energy, 0.0f, 1.0f);
        DanceStyle style;
        std::string reason = "energy " + std::to_string(static_cast<int>(energy * 100)) + "%";
        if (info.silent > 0.6f) {
            style = DanceStyle::Idle;
            reason = "silence";
        } else if (!confident) {
            style = DanceStyle::Groove;
            reason = "low beat confidence: stable Groove";
        } else {
            // Hysteresis keeps a style unless the energy clearly crosses over.
            const float margin = 0.06f;
            const auto classOf = [&](float e, DanceStyle prev) {
                float calm = options.calmThreshold;
                float intense = options.intenseThreshold;
                if (prev == DanceStyle::Groove) calm += margin;
                if (prev == DanceStyle::Energetic) intense -= margin;
                if (e < calm) return DanceStyle::Groove;
                if (e > intense) return DanceStyle::Energetic;
                return DanceStyle::HipHop;  // placeholder for "active"
            };
            DanceStyle prevClass =
                previous == DanceStyle::SideStep ? DanceStyle::HipHop : previous;
            style = classOf(energy, prevClass);
            if (style == DanceStyle::HipHop) {
                if (previous == DanceStyle::HipHop || previous == DanceStyle::SideStep) {
                    style = previous;  // stay in the active family for the phrase
                    if (++alternate % 2 == 0) {
                        style = previous == DanceStyle::HipHop ? DanceStyle::SideStep
                                                               : DanceStyle::HipHop;
                    }
                }
                reason += " -> active";
            } else if (style == DanceStyle::Energetic) {
                reason += " -> intense";
            } else {
                reason += " -> calm";
            }
        }
        if (!trustedBar) {
            reason += " (meter uncertain)";
        }
        if (!clipFor(style)) {
            style = DanceStyle::Groove;
        }
        plan.push_back({style, reason});
        previous = style;
    }

    // Emit one segment per style change; equal consecutive phrases continue.
    std::size_t index = 0;
    bool anySegment = false;
    for (int p = 0; p < phraseCount; ++p, ++index) {
        const Plan& entry = plan[index];
        if (anySegment && entry.style == plan[index - 1].style) {
            continue;
        }
        const HumanoidClip* clip = clipFor(entry.style);
        if (!clip) {
            continue;
        }
        const double align = phrases[index].t0;
        const double fade = fadeFor(align);
        const double start = std::max(0.0, align - fade * 0.5);
        ScheduleSegment segment = makeSegment(*clip, start, align, fade);
        segment.reason = entry.reason;
        schedule_.append(segment);
        anySegment = true;
    }
    selected_ = DanceStyle::Groove;
}

} // namespace bgmedia::motion
