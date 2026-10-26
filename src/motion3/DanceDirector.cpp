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

} // namespace

// ---------------------------------------------------------------------------
// BeatClock
// ---------------------------------------------------------------------------

BeatClock BeatClock::constant(double bpm, double firstBeatTime, int beatsPerBar) {
    BeatClock clock;
    clock.valid_ = bpm > 20.0;
    clock.bpm_ = std::clamp(bpm, 20.0, 400.0);
    clock.first_ = firstBeatTime;
    clock.beatsPerBar_ = std::max(1, beatsPerBar);
    clock.headPeriod_ = clock.tailPeriod_ = 60.0 / clock.bpm_;
    return clock;
}

BeatClock BeatClock::fromAnalysis(const AnalysisResult& analysis, double fallbackBpm) {
    const double bpm = analysis.bpm > 20.0 ? analysis.bpm : fallbackBpm;
    if (analysis.beats.size() < 4) {
        BeatClock clock = constant(bpm, 0.0, analysis.beatsPerBar);
        clock.confidence_ = 0.0f;
        return clock;
    }
    BeatClock clock;
    clock.valid_ = true;
    clock.bpm_ = bpm;
    clock.beatsPerBar_ = std::max(1, analysis.beatsPerBar);
    clock.confidence_ = std::min(analysis.bpmConfidence, analysis.beatPhaseConfidence > 0.0f
                                                             ? analysis.beatPhaseConfidence
                                                             : analysis.bpmConfidence);
    const std::size_t n = analysis.beats.size();
    const double basePeriod = std::clamp(60.0 / bpm, 0.2, 2.0);

    // BG Beat may report only the beats it is sure about (a quiet passage can
    // have gaps of several beats). Give every reported beat an integer grid
    // slot so the gaps do not distort the fit; the smoothed grid then covers
    // every slot, including the ones with no detection.
    std::vector<long> slot(n, 0);
    double period = basePeriod;
    for (std::size_t i = 1; i < n; ++i) {
        const double gap = analysis.beats[i].time - analysis.beats[i - 1].time;
        const long k = std::max(1L, std::lround(gap / period));
        slot[i] = slot[i - 1] + k;
        const double implied = gap / static_cast<double>(k);
        if (std::abs(implied - period) < 0.25 * period) {
            period = 0.9 * period + 0.1 * implied;
        }
    }
    const long slotCount = slot.back() + 1;

    // Least squares line through (slot, time) pairs within [lo, hi] (indices into beats).
    const auto fit = [&](std::size_t lo, std::size_t hi, double& a, double& b) {
        const double count = static_cast<double>(hi - lo + 1);
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (std::size_t j = lo; j <= hi; ++j) {
            const double x = static_cast<double>(slot[j]);
            const double y = analysis.beats[j].time;
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double denom = count * sxx - sx * sx;
        b = std::abs(denom) > 1e-9 ? (count * sxy - sx * sy) / denom : basePeriod;
        a = (sy - b * sx) / count;
    };

    std::vector<double> smooth(static_cast<std::size_t>(slotCount));
    bool useGlobal = analysis.tempoStable;
    if (useGlobal) {
        double a = 0, b = 0;
        fit(0, n - 1, a, b);
        double rms = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double e = a + b * static_cast<double>(slot[i]) - analysis.beats[i].time;
            rms += e * e;
        }
        rms = std::sqrt(rms / static_cast<double>(n));
        useGlobal = rms < 0.035 && b > 0.15;
        if (useGlobal) {
            for (long s = 0; s < slotCount; ++s) {
                smooth[static_cast<std::size_t>(s)] = a + b * static_cast<double>(s);
            }
        }
    }
    if (!useGlobal) {
        constexpr long kRadius = 8;
        std::size_t first = 0;
        std::size_t last = 0;
        for (long s = 0; s < slotCount; ++s) {
            while (first + 1 < n && slot[first] < s - kRadius) ++first;
            while (last + 1 < n && slot[last + 1] <= s + kRadius) ++last;
            std::size_t lo = first;
            std::size_t hi = std::max(last, first);
            while (hi - lo < 2 && (lo > 0 || hi + 1 < n)) {   // need three beats to fit
                if (lo > 0) --lo;
                if (hi - lo < 2 && hi + 1 < n) ++hi;
            }
            double a = 0, b = 0;
            fit(lo, hi, a, b);
            smooth[static_cast<std::size_t>(s)] = a + b * static_cast<double>(s);
        }
    }
    for (std::size_t i = 1; i < smooth.size(); ++i) {
        smooth[i] = std::max(smooth[i], smooth[i - 1] + 0.05);
    }

    // Bar position: vote with the reported beatInBar of every beat.
    std::vector<double> votes(static_cast<std::size_t>(clock.beatsPerBar_), 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const long offset =
            (((analysis.beats[i].beatInBar - slot[i]) % clock.beatsPerBar_) + clock.beatsPerBar_) %
            clock.beatsPerBar_;
        votes[static_cast<std::size_t>(offset)] +=
            0.2 + (analysis.beats[i].downbeat ? 1.0 : 0.0) + analysis.beats[i].confidence;
    }
    clock.barOffset_ = static_cast<int>(std::max_element(votes.begin(), votes.end()) - votes.begin());

    clock.times_ = std::move(smooth);
    const std::size_t count = clock.times_.size();
    const std::size_t span = std::min<std::size_t>(4, count > 1 ? count - 1 : 1);
    if (count > 1) {
        clock.headPeriod_ = (clock.times_[span] - clock.times_[0]) / static_cast<double>(span);
        clock.tailPeriod_ =
            (clock.times_[count - 1] - clock.times_[count - 1 - span]) / static_cast<double>(span);
    }
    clock.headPeriod_ = std::clamp(clock.headPeriod_, 0.15, 3.0);
    clock.tailPeriod_ = std::clamp(clock.tailPeriod_, 0.15, 3.0);
    return clock;
}

double BeatClock::beatAt(double t) const {
    if (times_.empty()) {
        return (t - first_) * bpm_ / 60.0;
    }
    if (t <= times_.front()) {
        return (t - times_.front()) / headPeriod_;
    }
    if (t >= times_.back()) {
        return static_cast<double>(times_.size() - 1) + (t - times_.back()) / tailPeriod_;
    }
    const auto it = std::upper_bound(times_.begin(), times_.end(), t);
    const std::size_t hi = static_cast<std::size_t>(it - times_.begin());
    const std::size_t lo = hi - 1;
    return static_cast<double>(lo) + (t - times_[lo]) / (times_[hi] - times_[lo]);
}

double BeatClock::timeAtBeat(double beat) const {
    if (times_.empty()) {
        return first_ + beat * 60.0 / bpm_;
    }
    if (beat <= 0.0) {
        return times_.front() + beat * headPeriod_;
    }
    const double last = static_cast<double>(times_.size() - 1);
    if (beat >= last) {
        return times_.back() + (beat - last) * tailPeriod_;
    }
    const std::size_t lo = static_cast<std::size_t>(std::floor(beat));
    const double f = beat - static_cast<double>(lo);
    return times_[lo] + f * (times_[lo + 1] - times_[lo]);
}

double BeatClock::bpmAt(double t) const {
    if (times_.empty()) {
        return bpm_;
    }
    double period = headPeriod_;
    if (t >= times_.back()) {
        period = tailPeriod_;
    } else if (t > times_.front()) {
        const auto it = std::upper_bound(times_.begin(), times_.end(), t);
        const std::size_t hi = static_cast<std::size_t>(it - times_.begin());
        period = times_[hi] - times_[hi - 1];
    }
    return 60.0 / period;
}

int BeatClock::beatInBar(double beat) const {
    const long index = static_cast<long>(std::floor(beat + 1e-6));
    const long bar = static_cast<long>(beatsPerBar_);
    long value = (index + barOffset_) % bar;
    if (value < 0) {
        value += bar;
    }
    return static_cast<int>(value);
}

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

ClipPose DanceSchedule::poseOf(std::size_t index, double mediaTime, const BeatClock& clock,
                               float* transition) const {
    const ScheduleSegment& segment = segments_[index];
    double beat = segmentClipBeat(segment, mediaTime, clock);
    if (!segment.loop) {
        beat = std::clamp(beat, 0.0, static_cast<double>(segment.clip->beats) - 1e-4);
    }
    ClipPose own = sampleClip(*segment.clip, beat);
    if (index > 0 && segment.fadeSeconds > 1e-4) {
        const double u = (mediaTime - segment.startTime) / segment.fadeSeconds;
        if (u < 1.0) {
            if (transition) {
                *transition = static_cast<float>(std::clamp(u, 0.0, 1.0));
            }
            if (u <= 0.0) {
                return poseOf(index - 1, mediaTime, clock, nullptr);
            }
            const ClipPose previous = poseOf(index - 1, mediaTime, clock, nullptr);
            return blendPoses(previous, own, smootherstep(static_cast<float>(u)));
        }
    }
    return own;
}

DanceFrame DanceSchedule::sample(double mediaTime, const BeatClock& clock) const {
    DanceFrame frame;
    frame.pose = neutralClipPose();
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
    frame.pose = poseOf(k, mediaTime, clock, &transition);
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
    const double beat = segmentClipBeat(*shown, mediaTime, clock);
    frame.clipBeat = beat;
    const double length = std::max(1.0, static_cast<double>(shown->clip->beats));
    double wrapped = std::fmod(beat, length);
    if (wrapped < 0.0) wrapped += length;
    if (!shown->loop && beat >= length) {
        frame.oneShotFinished = true;
        wrapped = length;
    }
    frame.phase = static_cast<float>(wrapped / length);
    frame.bpm = clock.bpmAt(mediaTime);
    if (shown->clip->tempoSync) {
        const double effective = frame.bpm * shown->rate;
        frame.playbackSpeed = static_cast<float>(effective / shown->clip->bpm);
        frame.speedLimited =
            effective < shown->clip->bpmMin - 0.5 || effective > shown->clip->bpmMax + 0.5;
    } else {
        frame.playbackSpeed = 1.0f;
    }
    return frame;
}

// ---------------------------------------------------------------------------
// DanceDirector
// ---------------------------------------------------------------------------

const HumanoidClip* DanceDirector::clipFor(DanceStyle style) const {
    return library_ ? library_->forStyle(style) : nullptr;
}

double DanceDirector::chooseRate(const HumanoidClip& clip, double musicBpm, bool* limited) {
    static constexpr double kLevels[] = {1.0, 0.5, 2.0};
    double best = 1.0;
    double bestScore = 1e9;
    bool bestInRange = false;
    for (double level : kLevels) {
        const double effective = musicBpm * level;
        const bool inRange = effective >= clip.bpmMin - 0.5 && effective <= clip.bpmMax + 0.5;
        const double score = std::abs(std::log(effective / clip.bpm)) + (inRange ? 0.0 : 10.0);
        if (score < bestScore) {
            bestScore = score;
            best = level;
            bestInRange = inRange;
        }
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

ScheduleSegment DanceDirector::makeSegment(const HumanoidClip& clip, double startTime,
                                           double alignTime, double fadeSeconds) const {
    ScheduleSegment segment;
    segment.clip = &clip;
    segment.startTime = startTime;
    segment.alignTime = alignTime;
    segment.fadeSeconds = fadeSeconds;
    segment.loop = true;
    if (!clip.tempoSync) {
        segment.rate = 1.0;
        segment.entryClipBeat = 0.0;
        return segment;
    }
    segment.rate = chooseRate(clip, clock_.bpmAt(alignTime));
    const double beatPos = clock_.beatAt(alignTime);
    const int inBar = clock_.beatInBar(beatPos);
    const double fraction = beatPos - std::floor(beatPos + 1e-6);
    const double barLength = std::max(1, clip.beatsPerBar);
    const double desired = std::fmod((inBar + std::max(0.0, fraction)) * segment.rate, barLength);
    double best = 0.0;
    double bestDistance = 1e9;
    std::vector<float> entries = clip.entryBeats;
    if (entries.empty()) {
        entries.push_back(0.0f);
    }
    std::sort(entries.begin(), entries.end());
    for (float entry : entries) {
        const double d = circularDistance(std::fmod(static_cast<double>(entry), barLength),
                                          desired, barLength);
        if (d < bestDistance - 1e-6) {
            bestDistance = d;
            best = entry;
        }
    }
    segment.entryClipBeat = best;
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
    // makeSegment() lands on an authored entry beat at `now`, which is right
    // for a scheduled transition but wrong after a seek: an off-beat seek would
    // restart the clip at beat 0 while the music is in the middle of a bar.
    // Keep the music's bar phase instead, exactly like BG Motion 2.0 sampling.
    if (clip->tempoSync) {
        const double beatPos = clock_.beatAt(now);
        const int inBar = clock_.beatInBar(beatPos);
        const double fraction = beatPos - std::floor(beatPos + 1e-6);
        const double barLength = std::max(1, clip->beatsPerBar);
        segment.entryClipBeat =
            std::fmod((inBar + std::max(0.0, fraction)) * segment.rate, barLength);
        if (segment.entryClipBeat < 0.0) {
            segment.entryClipBeat += barLength;
        }
    } else {
        segment.entryClipBeat = 0.0;
    }
    segment.reason = "re-aligned after seek";
    schedule_.append(segment);
}

void DanceDirector::rebase(double now, BeatClock newClock) {
    if (schedule_.empty()) {
        clock_ = std::move(newClock);
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
    clock_ = std::move(newClock);
    segment.alignTime = now;
    segment.startTime = now - 1.0e6;
    segment.fadeSeconds = 0.0;
    if (segment.clip->tempoSync) {
        segment.rate = chooseRate(*segment.clip, clock_.bpmAt(now));
    }
    schedule_.clear();
    schedule_.append(segment);
}

namespace {

// First musical beat >= `fromBeat` (an integer) whose position in the bar can
// host one of the clip's entry beats, searched over one bar.
double nextEntryBeat(const BeatClock& clock, const HumanoidClip& clip, double fromBeat) {
    const int bar = std::max(1, clip.beatsPerBar);
    for (int k = 0; k < clock.beatsPerBar(); ++k) {
        const double beat = fromBeat + k;
        const int inBar = clock.beatInBar(beat);
        if (clip.entryBeats.empty()) {
            if (inBar == 0) return beat;
            continue;
        }
        for (float entry : clip.entryBeats) {
            if (static_cast<int>(std::lround(entry)) % bar == inBar % bar) {
                return beat;
            }
        }
    }
    return fromBeat;
}

} // namespace

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
    if (!immediate && style != DanceStyle::Idle && clip->tempoSync) {
        const double beat = nextEntryBeat(clock_, *clip, std::ceil(clock_.beatAt(now + 0.10)));
        align = clock_.timeAtBeat(beat);
    }
    const double fade = immediate ? 0.0 : fadeFor(align);
    const double start = std::max(now, align - fade * 0.5);
    ScheduleSegment segment = makeSegment(*clip, start, align, fade);
    segment.loop = !oneShot;
    segment.reason = oneShot ? "manual one-shot" : "manual";
    schedule_.append(segment);

    if (oneShot && clip->tempoSync) {
        const ScheduleSegment& placed = schedule_.segments().back();
        const double endBeat =
            clock_.beatAt(placed.alignTime) + (clip->beats - placed.entryClipBeat) / placed.rate;
        const double endTime = clock_.timeAtBeat(endBeat);
        if (const HumanoidClip* idle = clipFor(DanceStyle::Idle)) {
            const double idleFade = fadeFor(endTime);
            ScheduleSegment back = makeSegment(*idle, endTime - idleFade * 0.5, endTime, idleFade);
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
    if (immediate) {
        beat = clock_.beatAt(now);
    } else {
        for (DanceStyle style : styles) {
            if (const HumanoidClip* firstClip = clipFor(style)) {
                if (style != DanceStyle::Idle) {
                    beat = nextEntryBeat(clock_, *firstClip, beat);
                }
                break;
            }
        }
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
        segment.reason = "sequence";
        schedule_.append(segment);
        first = false;
        selected_ = style;
        beat += beatsPerStyle;
    }
}

std::string DanceDirector::describeSchedule() const {
    std::string out;
    char line[160];
    for (const ScheduleSegment& seg : schedule_.segments()) {
        if (seg.startTime < -1.0e5) {
            std::snprintf(line, sizeof line, "  start      %-10s %s\n", seg.clip->id.c_str(),
                          seg.reason.c_str());
        } else {
            std::snprintf(line, sizeof line, "  %7.2f s  %-10s rate %.1f  %s\n", seg.alignTime,
                          seg.clip->id.c_str(), seg.rate, seg.reason.c_str());
        }
        out += line;
    }
    return out;
}

DanceFrame DanceDirector::sample(double mediaTime) const {
    return schedule_.sample(mediaTime, clock_);
}

void DanceDirector::planAuto(const AnalysisResult& analysis, const AutoPlanOptions& options) {
    clock_ = BeatClock::fromAnalysis(analysis);
    reset(0.0);
    const HumanoidClip* groove = clipFor(DanceStyle::Groove);
    if (!groove) {
        return;
    }
    const std::size_t beatCount = analysis.beats.size();
    const bool confident = beatCount >= 8 && clock_.confidence() >= options.minConfidence;
    std::size_t firstDownbeat = 0;
    for (std::size_t i = 0; i < beatCount && i < 16; ++i) {
        if (analysis.beats[i].downbeat) {
            firstDownbeat = i;
            break;
        }
    }
    const double trackEnd = analysis.info.duration;
    const double phrase = std::max(4.0, options.phraseBeats);
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
            const std::size_t from = static_cast<std::size_t>(std::max(0.0, info.t0 * spec.framesPerSecond));
            const std::size_t to = std::min<std::size_t>(
                spec.frameCount, static_cast<std::size_t>(std::max(0.0, info.t1 * spec.framesPerSecond)));
            double sum = 0.0;
            std::size_t count = 0;
            for (std::size_t f = from; f < to; ++f) {
                for (int band = 0; band < spec.bands; ++band) {
                    sum += spec.magnitudes[f * static_cast<std::size_t>(spec.bands) +
                                           static_cast<std::size_t>(band)];
                    ++count;
                }
            }
            info.loud = count ? static_cast<float>(sum / static_cast<double>(count) / 255.0) : 0.0f;
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
        const float loud = std::clamp((info.loud - loudMin) / std::max(1e-3f, loudRef - loudMin), 0.0f, 1.0f);
        float energy;
        if (info.section >= 0.0f) {
            energy = haveLoudness ? 0.4f * info.section + 0.25f * onset + 0.2f * loud + 0.15f * bass
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
            DanceStyle prevClass = previous == DanceStyle::SideStep ? DanceStyle::HipHop : previous;
            style = classOf(energy, prevClass);
            if (style == DanceStyle::HipHop) {
                if (previous == DanceStyle::HipHop || previous == DanceStyle::SideStep) {
                    style = previous;  // stay in the active family for the phrase
                    // vary every second active phrase
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
