#include "bgmedia/motion3/MusicalTimeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace bgmedia::motion {
namespace {

long floorDiv(long value, long divisor) {
    long q = value / divisor;
    const long r = value % divisor;
    if (r != 0 && ((r < 0) != (divisor < 0))) {
        --q;
    }
    return q;
}

float percentile(std::vector<float> values, double fraction) {
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        std::clamp(fraction, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    return values[index];
}

void mergeAccents(std::vector<AccentEvent>& events, double tolerance = 0.018) {
    if (events.empty()) {
        return;
    }
    std::sort(events.begin(), events.end(),
              [](const AccentEvent& a, const AccentEvent& b) { return a.time < b.time; });
    std::vector<AccentEvent> merged;
    merged.reserve(events.size());
    for (const AccentEvent& event : events) {
        if (!merged.empty() && event.time - merged.back().time <= tolerance) {
            AccentEvent& target = merged.back();
            // Keep the earliest real timestamp; a downbeat/bass hit wins the
            // label, but both flags survive so the accent map is not lossy.
            if (event.strength > target.strength) {
                target.strength = event.strength;
                target.beatIndex = event.beatIndex;
                target.beatOffset = event.beatOffset;
            }
            target.confidence = std::max(target.confidence, event.confidence);
            target.downbeat = target.downbeat || event.downbeat;
            target.bass = target.bass || event.bass;
            if (event.kind == AccentKind::Downbeat ||
                (event.kind == AccentKind::Bass && target.kind != AccentKind::Downbeat)) {
                target.kind = event.kind;
            }
            continue;
        }
        merged.push_back(event);
    }
    events = std::move(merged);
}

} // namespace

const char* accentKindName(AccentKind kind) {
    switch (kind) {
    case AccentKind::Beat: return "beat";
    case AccentKind::Downbeat: return "downbeat";
    case AccentKind::Bass: return "bass";
    case AccentKind::Offbeat: return "offbeat";
    case AccentKind::Subdivision: return "subdivision";
    }
    return "?";
}

MusicalTimeline MusicalTimeline::constant(double bpm, double firstBeatTime, int beatsPerBar) {
    MusicalTimeline timeline;
    timeline.grid_ = BeatClock::constant(bpm, firstBeatTime, beatsPerBar);
    timeline.valid_ = timeline.grid_.valid();
    timeline.beatsPerBar_ = std::clamp(beatsPerBar, 1, 16);
    timeline.meterConfidence_ = 1.0f;
    timeline.downbeatConfidence_ = 1.0f;
    timeline.downbeatTrusted_ = true;
    timeline.duration_ = 1.0e9;
    timeline.compound_ = false;
    timeline.beatUnit_ = 4;
    return timeline;
}

MusicalTimeline MusicalTimeline::fromClock(const BeatClock& clock, bool downbeatTrusted) {
    MusicalTimeline timeline;
    timeline.grid_ = clock;
    timeline.valid_ = clock.valid();
    timeline.beatsPerBar_ = std::max(1, clock.beatsPerBar());
    timeline.meterConfidence_ = downbeatTrusted ? 1.0f : 0.0f;
    timeline.downbeatConfidence_ = downbeatTrusted ? 1.0f : 0.0f;
    timeline.downbeatTrusted_ = downbeatTrusted;
    timeline.duration_ = 1.0e9;
    timeline.compound_ = false;
    timeline.beatUnit_ = 4;
    if (!clock.gridTimes().empty()) {
        std::vector<std::uint8_t> flags(clock.gridTimes().size(), 0);
        for (std::size_t i = 0; i < flags.size(); ++i) {
            flags[i] = clock.beatInBar(static_cast<double>(i)) == 0 ? 1 : 0;
        }
        timeline.buildBeatTable(clock.gridTimes(), flags);
    }
    return timeline;
}

void MusicalTimeline::buildBeatTable(const std::vector<double>& times,
                                     const std::vector<std::uint8_t>& downbeatFlags) {
    beats_.clear();
    beats_.reserve(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        TimelineBeat beat;
        beat.index = static_cast<int>(i);
        beat.time = times[i];
        beat.downbeat = i < downbeatFlags.size() && downbeatFlags[i] != 0;
        beat.confidence = 0.25f;
        beats_.push_back(beat);
    }
}

MusicalTimeline MusicalTimeline::fromAnalysis(const AnalysisResult& analysis,
                                              double fallbackBpm) {
    MusicalTimeline timeline;
    timeline.analyzed_ = true;
    timeline.grid_ = BeatClock::fromAnalysis(analysis, fallbackBpm);
    timeline.valid_ = timeline.grid_.valid();
    timeline.duration_ = std::isfinite(analysis.info.duration) && analysis.info.duration > 0.0
                             ? analysis.info.duration
                             : 1.0e9;
    timeline.beatsPerBar_ =
        std::clamp(analysis.beatsPerBar > 0 ? analysis.beatsPerBar : 4, 1, 16);
    timeline.meterConfidence_ =
        std::isfinite(analysis.meterConfidence)
            ? std::clamp(analysis.meterConfidence, 0.0f, 1.0f)
            : 0.0f;
    timeline.downbeatConfidence_ =
        std::isfinite(analysis.downbeatConfidence)
            ? std::clamp(analysis.downbeatConfidence, 0.0f, 1.0f)
            : 0.0f;
    timeline.downbeatTrusted_ =
        timeline.valid_ && timeline.beatsPerBar_ >= 2 &&
        timeline.meterConfidence_ >= 0.45f && timeline.downbeatConfidence_ >= 0.45f;
    timeline.compound_ = false;
    timeline.beatUnit_ = 4;

    // Stable grid timestamps. The smoothed grid is the animation clock; the
    // raw BG Beat timestamps stay available through onsets/accents and are not
    // replaced by the grid.
    const std::vector<double>& gridTimes = timeline.grid_.gridTimes();
    std::vector<double> times;
    std::vector<std::uint8_t> flags;
    if (!gridTimes.empty()) {
        times = gridTimes;
        flags.assign(times.size(), 0);
    } else {
        // Degenerate/fallback grid: use the analysed beats as the table. The
        // BeatClock still provides a safe mapping outside this range.
        for (const Beat& beat : analysis.beats) {
            if (!std::isfinite(beat.time)) {
                continue;
            }
            times.push_back(beat.time);
            flags.push_back(0);
        }
        std::sort(times.begin(), times.end());
        flags.assign(times.size(), 0);
    }
    timeline.buildBeatTable(times, flags);

    // Fill the table from BG Beat. Gaps stay as grid continuations with low
    // confidence rather than disappearing.
    if (!timeline.grid_.gridTimes().empty() && !timeline.beats_.empty()) {
        for (const Beat& beat : analysis.beats) {
            if (!std::isfinite(beat.time)) {
                continue;
            }
            const long index = std::lround(timeline.grid_.beatAt(beat.time));
            if (index < 0 || index >= static_cast<long>(timeline.beats_.size())) {
                continue;
            }
            TimelineBeat& target = timeline.beats_[static_cast<std::size_t>(index)];
            target.time = timeline.grid_.timeAtBeat(static_cast<double>(index));
            target.strength =
                std::isfinite(beat.strength) ? std::clamp(beat.strength, 0.0f, 1.5f) : 0.0f;
            target.confidence =
                std::isfinite(beat.confidence)
                    ? std::clamp(beat.confidence, 0.0f, 1.0f)
                    : 0.0f;
            target.downbeat = beat.downbeat;
            target.extrapolated = beat.extrapolated;
        }
        for (std::size_t i = 0; i < timeline.beats_.size(); ++i) {
            TimelineBeat& beat = timeline.beats_[i];
            if (beat.confidence <= 0.0f) {
                beat.downbeat =
                    timeline.grid_.beatInBar(static_cast<double>(i)) == 0;
                beat.confidence = beat.extrapolated ? 0.15f : 0.25f;
            }
            if (!timeline.downbeatTrusted_) {
                beat.downbeat = false;
            }
        }
    }
    for (const Beat& beat : analysis.beats) {
        if (!beat.downbeat || !std::isfinite(beat.time)) {
            continue;
        }
        const long index = std::lround(timeline.grid_.beatAt(beat.time));
        if (index >= 0 && index < static_cast<long>(timeline.beats_.size())) {
            timeline.beats_[static_cast<std::size_t>(index)].downbeat =
                timeline.downbeatTrusted_;
        }
    }

    timeline.inferSubdivision(analysis.onsets);

    for (const BassAccent& accent : analysis.bassAccents) {
        if (std::isfinite(accent.time) && accent.time >= 0.0) {
            timeline.bass_.push_back(accent);
        }
    }
    std::sort(timeline.bass_.begin(), timeline.bass_.end(),
              [](const BassAccent& a, const BassAccent& b) { return a.time < b.time; });
    if (timeline.bass_.size() > 200000) {
        timeline.bass_.resize(200000);
    }
    for (const SilenceRegion& silence : analysis.silences) {
        if (std::isfinite(silence.start) && std::isfinite(silence.end) &&
            silence.end > silence.start) {
            timeline.silences_.push_back(silence);
        }
    }

    // Normalise accent strengths against the track's own dynamics. This makes
    // the map comparable across tracks without pretending it is a probability.
    std::vector<float> onsetStrengths;
    onsetStrengths.reserve(analysis.onsets.size());
    for (const Onset& onset : analysis.onsets) {
        if (std::isfinite(onset.time) && std::isfinite(onset.strength) &&
            onset.strength > 0.0f) {
            onsetStrengths.push_back(onset.strength);
        }
    }
    const float onsetRef = std::max(0.18f, percentile(onsetStrengths, 0.95));
    std::vector<float> bassStrengths;
    for (const BassAccent& accent : timeline.bass_) {
        if (std::isfinite(accent.strength) && accent.strength > 0.0f) {
            bassStrengths.push_back(accent.strength);
        }
    }
    const float bassRef = std::max(0.12f, percentile(bassStrengths, 0.95));

    std::vector<AccentEvent> events;
    events.reserve(analysis.onsets.size() + timeline.bass_.size() +
                   timeline.beats_.size() / 4 + 4);
    for (const Onset& onset : analysis.onsets) {
        if (!std::isfinite(onset.time) ||
            !std::isfinite(static_cast<double>(onset.strength))) {
            continue;
        }
        const float normalised =
            std::clamp(onset.strength / onsetRef, 0.0f, 1.0f);
        if (normalised < 0.12f) {
            continue;
        }
        const double beat = timeline.grid_.beatAt(onset.time);
        long nearest = static_cast<long>(std::floor(beat + 0.5));
        float offset = static_cast<float>(beat - static_cast<double>(nearest));
        if (offset < -0.5f) {
            --nearest;
            offset += 1.0f;
        } else if (offset > 0.5f) {
            ++nearest;
            offset -= 1.0f;
        }
        const float absOffset = std::abs(offset);
        AccentEvent event;
        event.time = onset.time;
        event.strength = normalised;
        event.confidence = std::clamp(
            0.30f + 0.55f * normalised + 0.15f * std::max(0.0f, 1.0f - absOffset / 0.5f),
            0.0f, 1.0f);
        event.beatIndex = static_cast<int>(nearest);
        event.beatOffset = offset;
        if (absOffset <= 0.14f) {
            event.kind = AccentKind::Beat;
            if (nearest >= 0 && nearest < static_cast<long>(timeline.beats_.size())) {
                event.downbeat = timeline.beats_[static_cast<std::size_t>(nearest)].downbeat;
            }
        } else if (absOffset >= 0.40f) {
            // The half-beat "and" is an offbeat event, not a finer subdivision.
            event.kind = AccentKind::Offbeat;
        } else {
            event.kind = AccentKind::Subdivision;
        }
        events.push_back(event);
    }
    for (const BassAccent& accent : timeline.bass_) {
        const float normalised =
            std::clamp(accent.strength / bassRef, 0.0f, 1.0f);
        if (normalised < 0.10f) {
            continue;
        }
        const double beat = timeline.grid_.beatAt(accent.time);
        long nearest = accent.nearestBeat;
        if (nearest < 0 || nearest >= static_cast<long>(timeline.beats_.size())) {
            nearest = std::lround(beat);
        }
        const float offset = static_cast<float>(beat - static_cast<double>(nearest));
        AccentEvent event;
        event.time = accent.time;
        event.strength = normalised;
        event.confidence = std::clamp(
            (std::isfinite(accent.confidence) ? accent.confidence : 0.5f) *
                (0.55f + 0.45f * normalised),
            0.0f, 1.0f);
        event.beatIndex = static_cast<int>(nearest);
        event.beatOffset = offset;
        event.bass = true;
        event.kind = std::abs(offset) <= 0.20f ? AccentKind::Bass : AccentKind::Offbeat;
        events.push_back(event);
    }
    if (timeline.downbeatTrusted_) {
        for (const TimelineBeat& beat : timeline.beats_) {
            if (!beat.downbeat) {
                continue;
            }
            AccentEvent event;
            event.time = beat.time;
            event.strength = std::clamp(beat.strength, 0.0f, 1.0f);
            event.confidence = std::clamp(
                beat.confidence * timeline.downbeatConfidence_, 0.0f, 1.0f);
            event.beatIndex = beat.index;
            event.beatOffset = 0.0f;
            event.downbeat = true;
            event.kind = AccentKind::Downbeat;
            if (event.strength > 0.0f || event.confidence > 0.0f) {
                events.push_back(event);
            }
        }
    }
    mergeAccents(events);
    timeline.accents_ = std::move(events);

    // Per-beat summary used by the debug UI and by fast warp lookups.
    for (const AccentEvent& event : timeline.accents_) {
        if (event.beatIndex < 0 ||
            event.beatIndex >= static_cast<int>(timeline.beats_.size())) {
            continue;
        }
        TimelineBeat& beat = timeline.beats_[static_cast<std::size_t>(event.beatIndex)];
        if (event.bass) {
            beat.bass = std::max(beat.bass, event.strength);
        }
        if (std::abs(event.beatOffset) <= 0.30f) {
            beat.accent = std::max(beat.accent, event.strength);
        }
    }
    return timeline;
}

void MusicalTimeline::inferSubdivision(const std::vector<Onset>& onsets) {
    // Count onset phases in 1/12-beat bins, ignoring events that sit on the
    // beat itself. The result describes the audible subdivision (2 = eighths,
    // 3 = triplets, 4 = sixteenths); it is not a claim about notation.
    constexpr int kBins = 12;
    double histogram[kBins] = {};
    int offbeatCount = 0;
    for (const Onset& onset : onsets) {
        if (!std::isfinite(onset.time) || !std::isfinite(onset.strength) ||
            onset.strength <= 0.0f) {
            continue;
        }
        const double beat = grid_.beatAt(onset.time);
        if (!std::isfinite(beat)) {
            continue;
        }
        double phase = beat - std::floor(beat);
        if (phase < 0.0) {
            phase += 1.0;
        }
        if (phase < 0.075 || phase > 0.925) {
            continue;
        }
        int bin = static_cast<int>(std::lround(phase * kBins)) % kBins;
        if (bin < 0) {
            bin += kBins;
        }
        histogram[bin] += std::clamp(static_cast<double>(onset.strength), 0.0, 1.0);
        ++offbeatCount;
    }
    if (offbeatCount < 8) {
        subdivision_ = 1;
        subdivisionConfidence_ = 0.0f;
        compound_ = beatsPerBar_ == 6 || beatsPerBar_ == 12;
        beatUnit_ = compound_ ? 8 : 4;
        return;
    }
    struct Candidate {
        int division = 1;
        double score = 0.0;
        double prior = 0.0;
    };
    Candidate candidates[3] = {{2, 0.0, 0.02}, {3, 0.0, 0.01}, {4, 0.0, 0.0}};
    for (Candidate& candidate : candidates) {
        double on = 0.0;
        double off = 0.0;
        int onCount = 0;
        int offCount = 0;
        for (int bin = 0; bin < kBins; ++bin) {
            bool isOn = false;
            for (int k = 1; k < candidate.division; ++k) {
                const int target = static_cast<int>(
                    std::lround(static_cast<double>(k) * kBins / candidate.division)) %
                    kBins;
                if (bin == target) {
                    isOn = true;
                    break;
                }
            }
            if (isOn) {
                on += histogram[bin];
                ++onCount;
            } else {
                off += histogram[bin];
                ++offCount;
            }
        }
        const double onMean = onCount ? on / onCount : 0.0;
        const double offMean = offCount ? off / offCount : 0.0;
        candidate.score = onMean - 0.55 * offMean + candidate.prior;
    }
    std::sort(std::begin(candidates), std::end(candidates),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    const Candidate& best = candidates[0];
    const Candidate& second = candidates[1];
    const double separation = best.score - second.score;
    if (best.score <= 0.02 || separation < 0.012) {
        subdivision_ = 1;
        subdivisionConfidence_ = 0.0f;
    } else {
        subdivision_ = best.division;
        subdivisionConfidence_ = static_cast<float>(std::clamp(
            (separation / std::max(0.05, best.score)) *
                std::min(1.0, static_cast<double>(offbeatCount) / 24.0),
            0.0, 1.0));
    }
    if (beatsPerBar_ == 6 || beatsPerBar_ == 12) {
        compound_ = true;
        beatUnit_ = 8;
    } else if (subdivision_ == 3 && (beatsPerBar_ == 2 || beatsPerBar_ == 4)) {
        // 6/8 and 12/8 are conventionally counted as 2 and 4 dotted-quarter
        // pulses. Only call them compound when the metric evidence is not
        // actively low; the label remains honest above.
        compound_ = meterConfidence_ >= 0.45f;
        beatUnit_ = compound_ ? 8 : 4;
    } else {
        compound_ = false;
        beatUnit_ = 4;
    }
}

std::string MusicalTimeline::meterLabel() const {
    if (!valid_) {
        return "n/a";
    }
    std::string label;
    if (beatsPerBar_ == 6) {
        label = "6/8";
    } else if (beatsPerBar_ == 12) {
        label = "12/8";
    } else if (compound_ && beatsPerBar_ == 2) {
        label = "6/8";
    } else if (compound_ && beatsPerBar_ == 4) {
        label = "12/8";
    } else if (beatsPerBar_ == 3) {
        label = "3/4";
    } else if (beatsPerBar_ == 4) {
        label = "4/4";
    } else {
        label = std::to_string(beatsPerBar_) + "/4";
    }
    if (!downbeatTrusted()) {
        label += "?";
    }
    return label;
}

int MusicalTimeline::beatIndexAt(double mediaTime) const {
    const double beat = grid_.beatAt(mediaTime);
    if (!std::isfinite(beat)) {
        return -1;
    }
    return static_cast<int>(std::floor(beat + 1e-9));
}

float MusicalTimeline::beatPhaseAt(double mediaTime) const {
    return grid_.phaseAt(mediaTime);
}

int MusicalTimeline::beatInBarAt(double mediaTime) const {
    return grid_.beatInBar(grid_.beatAt(mediaTime));
}

bool MusicalTimeline::downbeatAt(double mediaTime) const {
    return downbeatTrusted() && beatInBarAt(mediaTime) == 0;
}

int MusicalTimeline::barAt(double mediaTime) const {
    const int index = beatIndexAt(mediaTime);
    if (index < 0) {
        return -1;
    }
    return static_cast<int>(floorDiv(static_cast<long>(index) + grid_.barOffset(),
                                     static_cast<long>(beatsPerBar_)));
}

double MusicalTimeline::beatPeriodAt(double mediaTime) const {
    return grid_.beatPeriodAt(mediaTime);
}

double MusicalTimeline::nextBeatTime(double mediaTime) const {
    return grid_.nextBeatTime(mediaTime);
}

int MusicalTimeline::nextDownbeatIndex(double mediaTime) const {
    const double beat = grid_.beatAt(mediaTime);
    if (!std::isfinite(beat)) {
        return 0;
    }
    const int start = static_cast<int>(std::floor(beat + 1e-9)) + 1;
    if (!downbeatTrusted()) {
        return start;
    }
    for (int k = start; k <= start + beatsPerBar_; ++k) {
        if (grid_.beatInBar(static_cast<double>(k)) == 0) {
            return k;
        }
    }
    return start;
}

double MusicalTimeline::nextDownbeatTime(double mediaTime) const {
    return grid_.timeAtBeat(static_cast<double>(nextDownbeatIndex(mediaTime)));
}

double MusicalTimeline::nextBarTime(double mediaTime) const {
    return nextDownbeatTime(mediaTime);
}

double MusicalTimeline::nextPhraseTime(double mediaTime, int phraseBars) const {
    if (!downbeatTrusted()) {
        return nextBeatTime(mediaTime);
    }
    const int bars = std::max(1, phraseBars);
    const int bar = std::max(0, barAt(mediaTime));
    int phraseBar = (bar / bars + 1) * bars;
    double time = grid_.timeAtBeat(static_cast<double>(phraseBar * beatsPerBar_) -
                                   static_cast<double>(grid_.barOffset()));
    if (time <= mediaTime + 1e-6) {
        phraseBar += bars;
        time = grid_.timeAtBeat(static_cast<double>(phraseBar * beatsPerBar_) -
                                static_cast<double>(grid_.barOffset()));
    }
    return time;
}

float MusicalTimeline::accentAt(double mediaTime, double window) const {
    if (accents_.empty() || !std::isfinite(mediaTime)) {
        return 0.0f;
    }
    const double from = mediaTime - std::max(0.0, window);
    const double to = mediaTime + std::max(0.0, window);
    auto it = std::lower_bound(accents_.begin(), accents_.end(), from,
                               [](const AccentEvent& event, double value) {
                                   return event.time < value;
                               });
    float best = 0.0f;
    for (; it != accents_.end() && it->time <= to; ++it) {
        best = std::max(best, it->strength);
    }
    return best;
}

float MusicalTimeline::accentConfidenceAt(double mediaTime, double window) const {
    if (accents_.empty() || !std::isfinite(mediaTime)) {
        return 0.0f;
    }
    const double from = mediaTime - std::max(0.0, window);
    const double to = mediaTime + std::max(0.0, window);
    auto it = std::lower_bound(accents_.begin(), accents_.end(), from,
                               [](const AccentEvent& event, double value) {
                                   return event.time < value;
                               });
    float best = 0.0f;
    for (; it != accents_.end() && it->time <= to; ++it) {
        best = std::max(best, it->confidence);
    }
    return best;
}

float MusicalTimeline::bassAt(double mediaTime, double window) const {
    if (bass_.empty() || !std::isfinite(mediaTime)) {
        return 0.0f;
    }
    const double from = mediaTime - std::max(0.0, window);
    const double to = mediaTime + std::max(0.0, window);
    auto it = std::lower_bound(bass_.begin(), bass_.end(), from,
                               [](const BassAccent& event, double value) {
                                   return event.time < value;
                               });
    float best = 0.0f;
    for (; it != bass_.end() && it->time <= to; ++it) {
        const float normalised =
            std::clamp(it->strength, 0.0f, 1.5f) / 1.5f;
        best = std::max(best, normalised);
    }
    return best;
}

float MusicalTimeline::beatAccent(int beatIndex) const {
    if (beatIndex < 0 || beatIndex >= static_cast<int>(beats_.size())) {
        return 0.0f;
    }
    return beats_[static_cast<std::size_t>(beatIndex)].accent;
}

float MusicalTimeline::beatBass(int beatIndex) const {
    if (beatIndex < 0 || beatIndex >= static_cast<int>(beats_.size())) {
        return 0.0f;
    }
    return beats_[static_cast<std::size_t>(beatIndex)].bass;
}

float MusicalTimeline::beatStrength(int beatIndex) const {
    if (beatIndex < 0 || beatIndex >= static_cast<int>(beats_.size())) {
        return 0.0f;
    }
    return beats_[static_cast<std::size_t>(beatIndex)].strength;
}

float MusicalTimeline::beatConfidence(int beatIndex) const {
    if (beatIndex < 0 || beatIndex >= static_cast<int>(beats_.size())) {
        return 0.0f;
    }
    return beats_[static_cast<std::size_t>(beatIndex)].confidence;
}

bool MusicalTimeline::beatIsDownbeat(int beatIndex) const {
    if (beatIndex < 0) {
        return false;
    }
    if (beatIndex < static_cast<int>(beats_.size())) {
        return downbeatTrusted() && beats_[static_cast<std::size_t>(beatIndex)].downbeat;
    }
    return downbeatTrusted() &&
           grid_.beatInBar(static_cast<double>(beatIndex)) == 0;
}

const AccentEvent* MusicalTimeline::nextAccent(double mediaTime, double lookahead) const {
    if (accents_.empty() || !std::isfinite(mediaTime)) {
        return nullptr;
    }
    auto it = std::lower_bound(accents_.begin(), accents_.end(), mediaTime,
                               [](const AccentEvent& event, double value) {
                                   return event.time < value;
                               });
    if (it == accents_.end() || it->time > mediaTime + std::max(0.0, lookahead)) {
        return nullptr;
    }
    return &*it;
}

bool MusicalTimeline::silentAt(double mediaTime) const {
    if (!std::isfinite(mediaTime)) {
        return false;
    }
    for (const SilenceRegion& silence : silences_) {
        if (mediaTime >= silence.start && mediaTime <= silence.end) {
            return true;
        }
    }
    return false;
}

float MusicalTimeline::silenceWeightAt(double mediaTime, double fadeSeconds) const {
    if (!std::isfinite(mediaTime) || silences_.empty()) {
        return 1.0f;
    }
    const double fade = std::max(0.02, fadeSeconds);
    double weight = 1.0;
    for (const SilenceRegion& silence : silences_) {
        if (mediaTime >= silence.start && mediaTime <= silence.end) {
            return 0.0f;
        }
        if (mediaTime >= silence.start - fade && mediaTime < silence.start) {
            weight = std::min(weight, (silence.start - mediaTime) / fade);
        } else if (mediaTime > silence.end && mediaTime <= silence.end + fade) {
            weight = std::min(weight, (mediaTime - silence.end) / fade);
        }
    }
    return static_cast<float>(std::clamp(weight, 0.0, 1.0));
}

std::string MusicalTimeline::describe() const {
    char buffer[320];
    std::snprintf(buffer, sizeof buffer,
                  "%s  %.2f BPM  beats %zu  accents %zu  bass %zu  sub %d (%.2f)  "
                  "meter %.2f  downbeat %.2f%s",
                  meterLabel().c_str(), nominalBpm(), beats_.size(), accents_.size(),
                  bass_.size(), subdivision_, subdivisionConfidence_, meterConfidence_,
                  downbeatConfidence_, downbeatTrusted() ? "" : "  (downbeat not trusted)");
    return buffer;
}

} // namespace bgmedia::motion
