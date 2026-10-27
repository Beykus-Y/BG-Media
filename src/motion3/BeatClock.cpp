#include "bgmedia/motion3/BeatClock.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace bgmedia::motion {
namespace {

struct CleanBeat {
    double time = 0.0;
    float strength = 0.0f;
    float confidence = 0.0f;
    int beatInBar = 0;
    bool downbeat = false;
    bool extrapolated = false;
};

} // namespace

BeatClock BeatClock::constant(double bpm, double firstBeatTime, int beatsPerBar) {
    BeatClock clock;
    clock.valid_ = std::isfinite(bpm) && bpm > 20.0;
    clock.bpm_ = std::clamp(std::isfinite(bpm) ? bpm : 100.0, 20.0, 400.0);
    clock.first_ = std::isfinite(firstBeatTime) ? firstBeatTime : 0.0;
    clock.beatsPerBar_ = std::max(1, std::min(16, beatsPerBar));
    clock.headPeriod_ = clock.tailPeriod_ = 60.0 / clock.bpm_;
    return clock;
}

BeatClock BeatClock::fromAnalysis(const AnalysisResult& analysis, double fallbackBpm) {
    const double requestedBpm =
        std::isfinite(analysis.bpm) && analysis.bpm > 20.0 ? analysis.bpm : fallbackBpm;
    const double bpm = std::clamp(std::isfinite(requestedBpm) ? requestedBpm : 100.0,
                                  20.0, 400.0);

    // Sanitise the event stream before fitting. A malformed cache, a weird
    // analysis or a future tracker must never make the clock divide by zero or
    // extrapolate with NaN.
    std::vector<CleanBeat> clean;
    clean.reserve(analysis.beats.size());
    for (const Beat& beat : analysis.beats) {
        if (!std::isfinite(beat.time) || beat.time < 0.0) {
            continue;
        }
        CleanBeat item;
        item.time = beat.time;
        item.strength = std::isfinite(beat.strength) ? std::max(0.0f, beat.strength) : 0.0f;
        item.confidence =
            std::isfinite(beat.confidence) ? std::clamp(beat.confidence, 0.0f, 1.0f) : 0.0f;
        item.beatInBar = beat.beatInBar;
        item.downbeat = beat.downbeat;
        item.extrapolated = beat.extrapolated;
        clean.push_back(item);
    }
    std::sort(clean.begin(), clean.end(),
              [](const CleanBeat& a, const CleanBeat& b) { return a.time < b.time; });
    {
        std::vector<CleanBeat> deduped;
        deduped.reserve(clean.size());
        for (const CleanBeat& beat : clean) {
            if (!deduped.empty() && beat.time - deduped.back().time < 0.05) {
                if (beat.confidence > deduped.back().confidence) {
                    deduped.back() = beat;
                }
                continue;
            }
            deduped.push_back(beat);
        }
        clean = std::move(deduped);
    }

    if (clean.size() < 4) {
        BeatClock clock = constant(bpm, clean.empty() ? 0.0 : clean.front().time,
                                   analysis.beatsPerBar);
        clock.confidence_ = 0.0f;
        return clock;
    }

    BeatClock clock;
    clock.valid_ = true;
    clock.bpm_ = bpm;
    clock.beatsPerBar_ = std::clamp(analysis.beatsPerBar > 0 ? analysis.beatsPerBar : 4, 1, 16);
    const float phaseConfidence =
        std::isfinite(analysis.beatPhaseConfidence) ? analysis.beatPhaseConfidence : 0.0f;
    const float bpmConfidence =
        std::isfinite(analysis.bpmConfidence) ? analysis.bpmConfidence : 0.0f;
    clock.confidence_ = std::clamp(
        phaseConfidence > 0.0f ? std::min(bpmConfidence, phaseConfidence) : bpmConfidence,
        0.0f, 1.0f);

    const std::size_t n = clean.size();
    const double basePeriod = std::clamp(60.0 / bpm, 0.15, 3.0);

    // Integer grid slots keep a missing beat in a quiet passage from shifting
    // every later musical position. The gap is capped so a corrupt timestamp
    // cannot request gigabytes of grid memory.
    std::vector<long> slot(n, 0);
    double period = basePeriod;
    for (std::size_t i = 1; i < n; ++i) {
        const double gap = clean[i].time - clean[i - 1].time;
        long k = std::lround(gap / period);
        k = std::clamp(k, 1L, 64L);
        slot[i] = slot[i - 1] + k;
        const double implied = gap / static_cast<double>(k);
        if (std::isfinite(implied) && implied > 0.5 * period && implied < 1.8 * period) {
            period = 0.9 * period + 0.1 * implied;
        }
    }
    const long slotCount = slot.back() + 1;
    if (slotCount < 1 || slotCount > 200000) {
        BeatClock fallback = constant(bpm, clean.front().time, clock.beatsPerBar_);
        fallback.confidence_ = 0.0f;
        return fallback;
    }

    const auto fit = [&](std::size_t lo, std::size_t hi, double& a, double& b) {
        const double count = static_cast<double>(hi - lo + 1);
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (std::size_t j = lo; j <= hi; ++j) {
            const double x = static_cast<double>(slot[j]);
            const double y = clean[j].time;
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double denom = count * sxx - sx * sx;
        b = std::abs(denom) > 1e-9 ? (count * sxy - sx * sy) / denom : basePeriod;
        a = (sy - b * sx) / count;
        if (!std::isfinite(a) || !std::isfinite(b) || b < 0.05 || b > 3.5) {
            a = clean[lo].time - basePeriod * static_cast<double>(slot[lo]);
            b = basePeriod;
        }
    };

    std::vector<double> smooth(static_cast<std::size_t>(slotCount), 0.0);
    bool useGlobal = analysis.tempoStable;
    if (useGlobal) {
        double a = 0.0, b = 0.0;
        fit(0, n - 1, a, b);
        double rms = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double e = a + b * static_cast<double>(slot[i]) - clean[i].time;
            rms += e * e;
        }
        rms = std::sqrt(rms / static_cast<double>(n));
        useGlobal = std::isfinite(rms) && rms < 0.035 && b > 0.15;
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
            while (first + 1 < n && slot[first] < s - kRadius) {
                ++first;
            }
            while (last + 1 < n && slot[last + 1] <= s + kRadius) {
                ++last;
            }
            std::size_t lo = first;
            std::size_t hi = std::max(last, first);
            while (hi - lo < 2 && (lo > 0 || hi + 1 < n)) {
                if (lo > 0) {
                    --lo;
                }
                if (hi - lo < 2 && hi + 1 < n) {
                    ++hi;
                }
            }
            double a = 0.0, b = 0.0;
            fit(lo, hi, a, b);
            smooth[static_cast<std::size_t>(s)] = a + b * static_cast<double>(s);
        }
    }
    for (std::size_t i = 1; i < smooth.size(); ++i) {
        if (!std::isfinite(smooth[i])) {
            smooth[i] = smooth[i - 1] + basePeriod;
        }
        smooth[i] = std::max(smooth[i], smooth[i - 1] + 0.05);
    }
    if (smooth.empty() || !std::isfinite(smooth.front())) {
        BeatClock fallback = constant(bpm, clean.front().time, clock.beatsPerBar_);
        fallback.confidence_ = 0.0f;
        return fallback;
    }

    // Bar position: vote with the reported beatInBar of every beat.
    std::vector<double> votes(static_cast<std::size_t>(clock.beatsPerBar_), 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const long offset =
            (((clean[i].beatInBar - slot[i]) % clock.beatsPerBar_) + clock.beatsPerBar_) %
            clock.beatsPerBar_;
        votes[static_cast<std::size_t>(offset)] +=
            0.2 + (clean[i].downbeat ? 1.0 : 0.0) +
            (std::isfinite(clean[i].confidence) ? clean[i].confidence : 0.0);
    }
    clock.barOffset_ = static_cast<int>(
        std::max_element(votes.begin(), votes.end()) - votes.begin());

    clock.times_ = std::move(smooth);
    const std::size_t count = clock.times_.size();
    const std::size_t span = std::min<std::size_t>(4, count > 1 ? count - 1 : 1);
    if (count > 1 && span > 0) {
        const double head = (clock.times_[span] - clock.times_[0]) /
                            static_cast<double>(span);
        const double tail = (clock.times_[count - 1] - clock.times_[count - 1 - span]) /
                            static_cast<double>(span);
        clock.headPeriod_ = std::isfinite(head) && head > 0.05 ? head : basePeriod;
        clock.tailPeriod_ = std::isfinite(tail) && tail > 0.05 ? tail : basePeriod;
    } else {
        clock.headPeriod_ = clock.tailPeriod_ = basePeriod;
    }
    clock.headPeriod_ = std::clamp(clock.headPeriod_, 0.15, 3.0);
    clock.tailPeriod_ = std::clamp(clock.tailPeriod_, 0.15, 3.0);
    return clock;
}

double BeatClock::beatAt(double t) const {
    if (!std::isfinite(t)) {
        return 0.0;
    }
    if (times_.empty()) {
        return (t - first_) * bpm_ / 60.0;
    }
    if (t <= times_.front()) {
        return (t - times_.front()) / headPeriod_;
    }
    if (t >= times_.back()) {
        return static_cast<double>(times_.size() - 1) +
               (t - times_.back()) / tailPeriod_;
    }
    const auto it = std::upper_bound(times_.begin(), times_.end(), t);
    const std::size_t hi = static_cast<std::size_t>(it - times_.begin());
    const std::size_t lo = hi - 1;
    const double interval = times_[hi] - times_[lo];
    if (!(interval > 1e-6)) {
        return static_cast<double>(lo);
    }
    return static_cast<double>(lo) + (t - times_[lo]) / interval;
}

double BeatClock::timeAtBeat(double beat) const {
    if (!std::isfinite(beat)) {
        return times_.empty() ? first_ : times_.front();
    }
    beat = std::clamp(beat, -1.0e7, 1.0e7);
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
    if (lo + 1 >= times_.size()) {
        return times_.back();
    }
    const double interval = times_[lo + 1] - times_[lo];
    return times_[lo] + f * std::max(0.05, interval);
}

double BeatClock::bpmAt(double t) const {
    if (times_.empty()) {
        return bpm_;
    }
    if (!std::isfinite(t)) {
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
    if (!std::isfinite(period) || period <= 0.05) {
        return bpm_;
    }
    return std::clamp(60.0 / period, 20.0, 400.0);
}

int BeatClock::beatInBar(double beat) const {
    if (!std::isfinite(beat)) {
        return 0;
    }
    const long index = static_cast<long>(std::floor(beat + 1e-6));
    const long bar = static_cast<long>(beatsPerBar_);
    long value = (index + barOffset_) % bar;
    if (value < 0) {
        value += bar;
    }
    return static_cast<int>(value);
}

float BeatClock::phaseAt(double mediaTime) const {
    const double beat = beatAt(mediaTime);
    if (!std::isfinite(beat)) {
        return 0.0f;
    }
    double phase = beat - std::floor(beat);
    if (phase < 0.0) {
        phase += 1.0;
    }
    return static_cast<float>(std::clamp(phase, 0.0, 1.0));
}

double BeatClock::beatPeriodAt(double mediaTime) const {
    const double beat = beatAt(mediaTime);
    if (!std::isfinite(beat)) {
        return 60.0 / bpm_;
    }
    const double index = std::floor(beat + 1e-9);
    const double period = timeAtBeat(index + 1.0) - timeAtBeat(index);
    if (!std::isfinite(period) || period <= 0.02) {
        return std::clamp(60.0 / bpm_, 0.15, 3.0);
    }
    return std::clamp(period, 0.15, 3.0);
}

double BeatClock::beatTime(int index) const {
    return timeAtBeat(static_cast<double>(index));
}

double BeatClock::nextBeatTime(double mediaTime) const {
    const double beat = beatAt(mediaTime);
    if (!std::isfinite(beat)) {
        return mediaTime;
    }
    return timeAtBeat(std::floor(beat + 1e-9) + 1.0);
}

double BeatClock::previousBeatTime(double mediaTime) const {
    const double beat = beatAt(mediaTime);
    if (!std::isfinite(beat)) {
        return mediaTime;
    }
    return timeAtBeat(std::ceil(beat - 1e-9) - 1.0);
}

} // namespace bgmedia::motion
