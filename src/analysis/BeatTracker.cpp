#include "analysis/BeatTracker.hpp"

#include "analysis/Dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <vector>

namespace bgmedia {
namespace {

bool inSilence(const std::vector<SilenceRegion>& silence, double time,
               double margin = 0.0) {
    for (const SilenceRegion& region : silence) {
        if (time >= region.start - margin && time <= region.end + margin) {
            return true;
        }
    }
    return false;
}

double frameTime(const SpectralFeatures& features, double frame) {
    return frame / features.framesPerSecond + features.onsetTimeOffset;
}

int frameForTime(const SpectralFeatures& features, double time) {
    return static_cast<int>(std::lround((time - features.onsetTimeOffset) *
                                       features.framesPerSecond));
}

float sampleCurve(const std::vector<float>& curve, double frame) {
    if (curve.empty()) {
        return 0.0f;
    }
    const int n = static_cast<int>(curve.size());
    if (frame <= 0.0) {
        return curve.front();
    }
    if (frame >= n - 1.0) {
        return curve.back();
    }
    const int lo = static_cast<int>(frame);
    const int hi = std::min(n - 1, lo + 1);
    const double frac = frame - static_cast<double>(lo);
    return static_cast<float>(curve[static_cast<std::size_t>(lo)] * (1.0 - frac) +
                              curve[static_cast<std::size_t>(hi)] * frac);
}

const Onset* nearestOnset(const std::vector<Onset>& onsets, double time,
                          double maxDistance) {
    if (onsets.empty()) {
        return nullptr;
    }
    const auto it = std::lower_bound(onsets.begin(), onsets.end(), time,
                                     [](const Onset& onset, double value) {
                                         return onset.time < value;
                                     });
    const Onset* best = nullptr;
    double bestDistance = maxDistance;
    if (it != onsets.end()) {
        const double distance = std::abs(it->time - time);
        if (distance <= bestDistance) {
            best = &*it;
            bestDistance = distance;
        }
    }
    if (it != onsets.begin()) {
        const Onset& previous = *(it - 1);
        const double distance = std::abs(previous.time - time);
        if (distance < bestDistance) {
            best = &previous;
        }
    }
    return best;
}

std::vector<std::pair<int, int>> activeRegions(const std::vector<std::uint8_t>& active,
                                               int frameCount) {
    std::vector<std::pair<int, int>> regions;
    int start = -1;
    for (int i = 0; i < frameCount; ++i) {
        const bool isActive = active.empty() || active[static_cast<std::size_t>(i)] != 0;
        if (isActive && start < 0) {
            start = i;
        } else if (!isActive && start >= 0) {
            regions.emplace_back(start, i);
            start = -1;
        }
    }
    if (start >= 0) {
        regions.emplace_back(start, frameCount);
    }
    // Merge regions separated by less than one analysis frame; they are not
    // musically meaningful gaps.
    std::vector<std::pair<int, int>> merged;
    for (const auto& region : regions) {
        if (!merged.empty() && region.first - merged.back().second <= 1) {
            merged.back().second = region.second;
        } else {
            merged.push_back(region);
        }
    }
    return merged;
}

struct DpBeat {
    int frame = 0;
    double score = 0.0;
    bool extrapolated = false;
};

struct GridEvidence {
    double precision = 0.0;   // share of grid positions with an onset nearby
    double recall = 0.0;      // share of onsets explained by the grid
    double lowHit = 0.0;      // share of grid positions on a low-frequency pulse
    double lowContrast = 0.0; // on-grid low pulse vs mid-beat low pulse
};

double envelopeAtTime(const std::vector<float>& curve, const SpectralFeatures& features,
                      double time) {
    if (curve.empty()) {
        return 0.0;
    }
    const double frame = (time - features.onsetTimeOffset) * features.framesPerSecond;
    return static_cast<double>(sampleCurve(curve, frame));
}

double percentile(std::vector<float> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        std::clamp(fraction, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    return values[index];
}

GridEvidence evaluateGrid(const std::vector<double>& times,
                          const std::vector<Onset>& onsets,
                          const std::vector<float>& pulseEnv,
                          const SpectralFeatures& features, double tolerance = 0.06) {
    GridEvidence evidence;
    if (times.size() < 2) {
        return evidence;
    }
    int hits = 0;
    for (double time : times) {
        if (nearestOnset(onsets, time, tolerance) != nullptr) {
            ++hits;
        }
    }
    evidence.precision = static_cast<double>(hits) / static_cast<double>(times.size());

    const double from = times.front() - tolerance;
    const double to = times.back() + tolerance;
    double weightSum = 0.0;
    double hitWeight = 0.0;
    int allOnsetCount = 0;
    int allOnsetHits = 0;
    for (const Onset& onset : onsets) {
        if (onset.time < from || onset.time > to || onset.strength < 0.10f) {
            continue;
        }
        ++allOnsetCount;
        auto it = std::lower_bound(times.begin(), times.end(), onset.time);
        double nearest = 1e9;
        if (it != times.end()) {
            nearest = std::abs(*it - onset.time);
        }
        if (it != times.begin()) {
            nearest = std::min(nearest, std::abs(*(it - 1) - onset.time));
        }
        const bool hit = nearest <= tolerance;
        if (hit) {
            ++allOnsetHits;
        }
        const double weight = static_cast<double>(onset.strength);
        weightSum += weight;
        if (hit) {
            hitWeight += weight;
        }
    }
    evidence.recall =
        weightSum > 1e-9
            ? hitWeight / weightSum
            : (allOnsetCount > 0 ? static_cast<double>(allOnsetHits) / allOnsetCount : 0.0);

    std::vector<float> positive;
    positive.reserve(pulseEnv.size());
    for (float value : pulseEnv) {
        if (std::isfinite(value) && value > 0.0f) {
            positive.push_back(value);
        }
    }
    const double threshold = std::max(0.04, 0.25 * percentile(positive, 0.90));
    double onMean = 0.0;
    double offMean = 0.0;
    int lowHits = 0;
    int pairs = 0;
    for (std::size_t i = 0; i < times.size(); ++i) {
        const double on = envelopeAtTime(pulseEnv, features, times[i]);
        double interval = 0.0;
        if (i + 1 < times.size()) {
            interval = times[i + 1] - times[i];
        } else if (i > 0) {
            interval = times[i] - times[i - 1];
        }
        const double off =
            envelopeAtTime(pulseEnv, features, times[i] + 0.5 * std::max(0.05, interval));
        onMean += on;
        offMean += off;
        if (on >= threshold) {
            ++lowHits;
        }
        ++pairs;
    }
    if (pairs > 0) {
        onMean /= static_cast<double>(pairs);
        offMean /= static_cast<double>(pairs);
        evidence.lowHit = static_cast<double>(lowHits) / static_cast<double>(pairs);
        evidence.lowContrast = onMean / (onMean + offMean + 0.02);
    }
    return evidence;
}

// If a stable track was tracked at a slower metric level, its beats still
// cover only every second/third onset. This is deliberate: BG Beat should not
// call a hi-hat layer a beat. But a genuine fast pulse (180 BPM four-on-the-
// floor, or a 6/8 pulse at 120) should not be reported as 60 BPM either.
//
// The refinement is conservative and only runs on stable tempo maps:
//   * the current grid must already sit on a low-frequency pulse;
//   * the faster grid must explain materially more onsets;
//   * it must still sit on the low pulse (not on hats or subdivisions);
//   * it must be closer to the configured preferred tempo than the slower one.
// This prevents the 100 BPM offbeat case (current grid off the pulse) and
// 100 -> 200 (farther from the preferred tempo) from being "upgraded".
int refineMetricLevel(std::vector<Beat>& beats, const std::vector<Onset>& onsets,
                      const std::vector<float>& pulseEnv,
                      const SpectralFeatures& features, const TempoStructure& tempo,
                      const AnalysisConfig& config, double* refinedBpm) {
    if (beats.size() < 8 || !tempo.stable || !tempo.transitions.empty()) {
        return 1;
    }
    std::vector<double> current;
    current.reserve(beats.size());
    for (const Beat& beat : beats) {
        current.push_back(beat.time);
    }
    if (current.size() < 8) {
        return 1;
    }
    std::vector<double> gaps;
    gaps.reserve(current.size() - 1);
    for (std::size_t i = 1; i < current.size(); ++i) {
        const double gap = current[i] - current[i - 1];
        if (std::isfinite(gap) && gap > 0.05) {
            gaps.push_back(gap);
        }
    }
    if (gaps.size() < 6) {
        return 1;
    }
    std::sort(gaps.begin(), gaps.end());
    const double medianGap = gaps[gaps.size() / 2];
    if (!std::isfinite(medianGap) || medianGap <= 0.05) {
        return 1;
    }
    const double currentBpm = 60.0 / medianGap;
    if (currentBpm < config.minBpm - 0.5 || currentBpm > config.maxBpm + 0.5) {
        return 1;
    }
    const int baseSteps =
        static_cast<int>(std::lround((current.back() - current.front()) / medianGap));
    if (baseSteps < 7) {
        return 1;
    }
    std::vector<double> regular;
    regular.reserve(static_cast<std::size_t>(baseSteps) + 1);
    double maxDeviation = 0.0;
    for (int i = 0; i <= baseSteps; ++i) {
        const double expected = current.front() + static_cast<double>(i) * medianGap;
        regular.push_back(expected);
        if (i < static_cast<int>(current.size())) {
            maxDeviation = std::max(maxDeviation, std::abs(current[static_cast<std::size_t>(i)] - expected));
        }
    }
    // Allow normal beat-snapping jitter, but do not regularise a genuine
    // rubato/tempo-change grid.
    if (maxDeviation > std::max(0.08, 0.20 * medianGap)) {
        return 1;
    }
    const GridEvidence base = evaluateGrid(regular, onsets, pulseEnv, features);
    if (base.lowHit < 0.50 || base.precision < 0.55) {
        return 1;
    }
    const double preferred = std::max(20.0, config.preferredBpm);
    const double currentPrior = std::abs(std::log2(currentBpm / preferred));

    double bestScore = -1e9;
    int bestMultiplier = 1;
    double bestBpm = currentBpm;
    std::vector<double> bestTimes;
    for (int multiplier : {2, 3}) {
        const double candidateBpm = currentBpm * static_cast<double>(multiplier);
        if (candidateBpm > config.maxBpm + 0.5 || candidateBpm < config.minBpm - 0.5) {
            continue;
        }
        if (std::abs(std::log2(candidateBpm / preferred)) + 0.01 >= currentPrior) {
            continue;
        }
        std::vector<double> candidate;
        candidate.reserve(static_cast<std::size_t>((baseSteps + 1) * multiplier));
        for (int i = 0; i <= baseSteps; ++i) {
            for (int k = 0; k < multiplier; ++k) {
                const double time =
                    current.front() +
                    (static_cast<double>(i) +
                     static_cast<double>(k) / static_cast<double>(multiplier)) *
                        medianGap;
                candidate.push_back(time);
            }
        }
        candidate.push_back(current.back());
        std::sort(candidate.begin(), candidate.end());
        candidate.erase(std::unique(candidate.begin(), candidate.end()), candidate.end());
        const GridEvidence evidence = evaluateGrid(candidate, onsets, pulseEnv, features);
        const bool betterEvents = evidence.recall >= base.recall + 0.08;
        const bool betterPulse = evidence.lowContrast >= base.lowContrast + 0.06;
        if (evidence.precision < std::max(0.70, base.precision - 0.03) ||
            evidence.lowHit < 0.85 || evidence.lowContrast < 0.55 ||
            !(betterEvents || betterPulse)) {
            continue;
        }
        const double score = 0.45 * evidence.recall + 0.35 * evidence.lowContrast +
                             0.20 * evidence.precision -
                             0.08 * std::abs(std::log2(candidateBpm / preferred));
        if (score > bestScore) {
            bestScore = score;
            bestMultiplier = multiplier;
            bestBpm = candidateBpm;
            bestTimes = std::move(candidate);
        }
    }
    if (bestMultiplier <= 1 || bestTimes.size() < 2) {
        return 1;
    }

    std::vector<Beat> refined;
    refined.reserve(bestTimes.size());
    for (double time : bestTimes) {
        if (time < 0.0) {
            continue;
        }
        Beat beat;
        beat.time = time;
        const Onset* onset = nearestOnset(onsets, time, 0.030);
        if (onset) {
            beat.time = onset->time;
            beat.strength = std::max(0.2f, onset->strength);
            beat.confidence = 0.55f + 0.42f * std::min(1.0f, onset->strength);
            beat.extrapolated = false;
        } else {
            beat.strength = dsp::clampf(
                static_cast<float>(envelopeAtTime(features.env, features, time)), 0.0f, 1.5f);
            beat.confidence = 0.30f;
            beat.extrapolated = true;
        }
        if (!refined.empty() && beat.time - refined.back().time < 0.10) {
            if (beat.confidence > refined.back().confidence) {
                refined.back() = beat;
            }
            continue;
        }
        refined.push_back(beat);
    }
    if (refined.size() < 4) {
        return 1;
    }
    beats = std::move(refined);
    if (refinedBpm) {
        *refinedBpm = bestBpm;
    }
    return bestMultiplier;
}

std::vector<DpBeat> trackRegion(const std::vector<float>& env,
                                const std::vector<double>& periodFrames,
                                int regionStart, int regionEnd) {
    // env is a beat evidence envelope: broadband onset strength plus a
    // low-frequency pulse term. A pure broadband tracker can lock to offbeat
    // hats; the pulse term keeps the grid on kick/bass events when available.
    std::vector<DpBeat> beats;
    const int n = regionEnd - regionStart;
    if (n <= 1) {
        return beats;
    }

    constexpr double kTightness = 80.0;
    std::vector<double> score(static_cast<std::size_t>(n),
                              -std::numeric_limits<double>::infinity());
    std::vector<int> previous(static_cast<std::size_t>(n), -1);

    for (int i = 0; i < n; ++i) {
        const int globalFrame = regionStart + i;
        const double period = std::max(2.0, periodFrames[static_cast<std::size_t>(globalFrame)]);
        const int minGap = std::max(2, static_cast<int>(std::lround(period * 0.52)));
        const int maxGap = std::max(minGap + 1, static_cast<int>(std::lround(period * 1.75)));
        const float onset = env[static_cast<std::size_t>(globalFrame)];

        double bestPrevious = 0.0;
        int bestIndex = -1;
        for (int gap = minGap; gap <= maxGap; ++gap) {
            const int j = i - gap;
            if (j < 0) {
                break;
            }
            if (!std::isfinite(score[static_cast<std::size_t>(j)])) {
                continue;
            }
            const double ratio = static_cast<double>(gap) / period;
            const double transition = -kTightness * std::pow(std::log(ratio), 2.0);
            const double candidate = score[static_cast<std::size_t>(j)] + transition;
            if (candidate > bestPrevious) {
                bestPrevious = candidate;
                bestIndex = j;
            }
        }

        score[static_cast<std::size_t>(i)] = static_cast<double>(onset) + bestPrevious;
        previous[static_cast<std::size_t>(i)] = bestIndex;
    }

    // Choose the strongest ending within two periods of the region end.
    const int tailStart = std::max(0, n - static_cast<int>(std::lround(
                                            periodFrames[static_cast<std::size_t>(regionEnd - 1)] * 2.0)));
    int endIndex = tailStart;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (int i = tailStart; i < n; ++i) {
        if (score[static_cast<std::size_t>(i)] > bestScore) {
            bestScore = score[static_cast<std::size_t>(i)];
            endIndex = i;
        }
    }
    if (!std::isfinite(bestScore) || bestScore <= 0.0) {
        return beats;
    }

    std::vector<int> indices;
    int cursor = endIndex;
    while (cursor >= 0) {
        indices.push_back(cursor);
        cursor = previous[static_cast<std::size_t>(cursor)];
    }
    std::reverse(indices.begin(), indices.end());

    // Extrapolate the grid over short edges so choreography does not start
    // after the first detected transient or stop before the track ends.
    const double edgePeriod = periodFrames[static_cast<std::size_t>(regionStart)];
    if (!indices.empty()) {
        int first = indices.front();
        while (static_cast<double>(first) - edgePeriod >= 0.0) {
            first = static_cast<int>(std::lround(static_cast<double>(first) - edgePeriod));
            if (first < 0) {
                break;
            }
            indices.insert(indices.begin(), first);
            if (static_cast<double>(indices.front()) < 0.15 * edgePeriod) {
                break;
            }
        }

        int last = indices.back();
        const int limit = n - 1;
        while (static_cast<double>(last) + edgePeriod <= static_cast<double>(limit) + 0.25 * edgePeriod) {
            last = static_cast<int>(std::lround(static_cast<double>(last) + edgePeriod));
            if (last > limit) {
                break;
            }
            indices.push_back(last);
        }
    }

    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    for (int index : indices) {
        beats.push_back({regionStart + index, score[static_cast<std::size_t>(index)], false});
    }
    return beats;
}

} // namespace

std::vector<Onset> detectOnsets(const SpectralFeatures& features,
                                const std::vector<SilenceRegion>& silence,
                                double duration, const AnalysisConfig& config) {
    std::vector<Onset> onsets;
    const auto& env = features.sharpEnv;
    const int n = static_cast<int>(env.size());
    if (n < 3) {
        return onsets;
    }

    const double sensitivity = std::max(0.2, config.onsetSensitivity);
    const double baseThreshold = 0.055 / sensitivity;
    const double minSpacingFrames = std::max(1.2, 0.055 * features.framesPerSecond);

    for (int i = 1; i + 1 < n; ++i) {
        const float value = env[static_cast<std::size_t>(i)];
        if (value < baseThreshold) {
            continue;
        }
        if (value < env[static_cast<std::size_t>(i - 1)] ||
            value <= env[static_cast<std::size_t>(i + 1)]) {
            continue;
        }
        const float offset = dsp::parabolicPeakOffset(
            env[static_cast<std::size_t>(i - 1)], value,
            env[static_cast<std::size_t>(i + 1)]);
        const double time = frameTime(features, static_cast<double>(i) + offset);
        if (time < 0.0 || time > duration || inSilence(silence, time, 0.04)) {
            continue;
        }
        Onset onset;
        onset.time = time;
        onset.strength = dsp::clampf(value, 0.0f, 1.5f);
        onset.lowBandRatio = features.lowRatio[static_cast<std::size_t>(i)];
        if (!onsets.empty() &&
            time - onsets.back().time < minSpacingFrames / features.framesPerSecond) {
            if (onset.strength > onsets.back().strength) {
                onsets.back() = onset;
            }
            continue;
        }
        onsets.push_back(onset);
    }
    return onsets;
}

BeatTrackingResult trackBeats(const SpectralFeatures& features,
                              const std::vector<std::uint8_t>& active,
                              const std::vector<SilenceRegion>& silence,
                              const TempoStructure& tempo, double duration,
                              const AnalysisConfig& config) {
    BeatTrackingResult result;
    result.onsets = detectOnsets(features, silence, duration, config);
    if (features.env.size() < 8 || features.framesPerSecond <= 0.0) {
        return result;
    }

    const int frameCount = static_cast<int>(features.env.size());
    std::vector<double> periodFrames(static_cast<std::size_t>(frameCount), 0.0);
    for (int i = 0; i < frameCount; ++i) {
        const double time = frameTime(features, i);
        const double bpm = bpmAtTime(tempo, time, tempo.bpm);
        periodFrames[static_cast<std::size_t>(i)] =
            bpm > 1.0 ? 60.0 * features.framesPerSecond / bpm : 0.0;
    }

    const double fallbackBpm = tempo.bpm > 1.0 ? tempo.bpm : 0.0;
    if (fallbackBpm <= 0.0) {
        return result;
    }
    for (double& period : periodFrames) {
        if (period <= 1.0) {
            period = 60.0 * features.framesPerSecond / fallbackBpm;
        }
    }

    // A kick/bass pulse is a stronger cue for where the beat is than a
    // subdivision onset. Filter low-band leakage from keys/snares by the share
    // of linear energy below ~180 Hz, then combine it with the broadband onset
    // envelope used by the DP tracker.
    std::vector<float> pulseEnv(features.env.size(), 0.0f);
    std::vector<float> beatEvidence(features.env.size(), 0.0f);
    for (std::size_t i = 0; i < features.env.size(); ++i) {
        const float lowShare =
            std::clamp((features.lowEnergyRatio[i] - 0.15f) / 0.35f, 0.0f, 1.0f);
        pulseEnv[i] = features.lowEnv[i] * lowShare;
        beatEvidence[i] = features.env[i] + 0.85f * pulseEnv[i];
    }

    const auto regions = activeRegions(active, frameCount);
    std::vector<DpBeat> raw;
    for (const auto& region : regions) {
        if (region.second - region.first < 3) {
            continue;
        }
        auto part =
            trackRegion(beatEvidence, periodFrames, region.first, region.second);
        raw.insert(raw.end(), part.begin(), part.end());
    }

    std::sort(raw.begin(), raw.end(), [](const DpBeat& a, const DpBeat& b) {
        return a.frame < b.frame;
    });

    std::vector<Beat> beats;
    beats.reserve(raw.size());
    for (const DpBeat& item : raw) {
        Beat beat;
        beat.time = frameTime(features, item.frame);
        if (beat.time < 0.0 || beat.time > duration ||
            inSilence(silence, beat.time, 0.02)) {
            continue;
        }
        beat.strength = dsp::clampf(
            sampleCurve(features.env, static_cast<double>(item.frame)), 0.0f, 1.5f);
        beat.confidence = 0.25f + 0.35f * std::min(1.0f, beat.strength);

        const Onset* onset = nearestOnset(result.onsets, beat.time, 0.030);
        if (onset) {
            beat.time = onset->time;
            beat.strength = std::max(beat.strength, onset->strength);
            beat.confidence = 0.55f + 0.42f * std::min(1.0f, onset->strength);
        }
        beat.extrapolated = false;
        beats.push_back(beat);
    }

    // Mark grid continuations that are too far from any onset. The DP itself
    // already produced those, but after snapping a beat may sit between
    // transients (a weak beat), which is still a valid musical beat.
    for (Beat& beat : beats) {
        const Onset* onset = nearestOnset(result.onsets, beat.time, 0.045);
        if (!onset) {
            beat.extrapolated = true;
            beat.confidence = std::min(beat.confidence, 0.33f);
        }
    }

    // Remove near-duplicates produced by region seams.
    std::vector<Beat> deduped;
    for (const Beat& beat : beats) {
        if (!deduped.empty() && beat.time - deduped.back().time < 0.12) {
            if (beat.confidence > deduped.back().confidence) {
                deduped.back() = beat;
            }
            continue;
        }
        deduped.push_back(beat);
    }
    beats = std::move(deduped);

    if (beats.empty()) {
        return result;
    }

    auto pulseAt = [&](double time) {
        if (pulseEnv.empty()) {
            return 0.0;
        }
        const double frame = (time - features.onsetTimeOffset) *
                             features.framesPerSecond;
        const int at = static_cast<int>(std::llround(frame));
        double value = 0.0;
        for (int d = -1; d <= 1; ++d) {
            const int index = std::clamp(at + d, 0,
                                         static_cast<int>(pulseEnv.size()) - 1);
            value = std::max(value,
                             static_cast<double>(pulseEnv[static_cast<std::size_t>(index)]));
        }
        return value;
    };
    auto lowEnergyAt = [&](double time) {
        if (features.lowEnergy.empty()) {
            return 0.0;
        }
        const int at = std::clamp(
            static_cast<int>(std::llround((time - features.onsetTimeOffset) *
                                          features.framesPerSecond)),
            0, static_cast<int>(features.lowEnergy.size()) - 1);
        // A downbeat bass note is sustained for much longer than a kick
        // transient. Skip the attack itself and average the following ~0.18 s,
        // before the next beat, so the measurement sees the note body rather
        // than the broadband click of whichever instrument happened to hit.
        const int start = at + std::max(1, static_cast<int>(
                                                  std::lround(0.055 *
                                                              features.framesPerSecond)));
        const int span = std::max(1, static_cast<int>(
                                         std::lround(0.18 * features.framesPerSecond)));
        double sum = 0.0;
        int count = 0;
        for (int d = 0; d <= span; ++d) {
            const int index = std::min(
                start + d, static_cast<int>(features.lowEnergy.size()) - 1);
            sum += features.lowEnergy[static_cast<std::size_t>(index)];
            ++count;
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };

    // The DP cost is driven by broadband onsets and can still sit on the
    // offbeat hi-hat layer when a track emphasises the "and". Refine the whole
    // grid phase against the low-frequency pulse envelope. This is a phase
    // correction only; the tempo/period comes from BG Beat. It is applied only
    // when the pulse evidence improves materially, so four-on-the-floor and
    // bass-light material keep their original phase.
    if (!pulseEnv.empty() && tempo.bpm > 1.0 && beats.size() >= 4 &&
        tempo.transitions.empty()) {
        const double period = 60.0 / tempo.bpm;
        auto averagePulse = [&](double shift) {
            double sum = 0.0;
            int count = 0;
            for (const Beat& beat : beats) {
                const double shifted = beat.time + shift;
                if (shifted < 0.0 || shifted > duration) {
                    continue;
                }
                sum += pulseAt(shifted);
                ++count;
            }
            return count > 0 ? sum / static_cast<double>(count) : 0.0;
        };
        const double basePulse = averagePulse(0.0);
        double bestShift = 0.0;
        double bestPulse = basePulse;
        constexpr int kPhaseSteps = 96;
        for (int step = -kPhaseSteps; step <= kPhaseSteps; ++step) {
            const double shift = period * 0.5 * static_cast<double>(step) / kPhaseSteps;
            const double score = averagePulse(shift);
            if (score > bestPulse * 1.0001 ||
                (std::abs(score - bestPulse) < 1e-5 &&
                 std::abs(shift) < std::abs(bestShift))) {
                bestPulse = score;
                bestShift = shift;
            }
        }
        // Only rescue a grid that has essentially no low-frequency pulse
        // support at all. If the current grid is already on kicks, a stronger
        // syncopated bass in the next half-beat must not drag the beat away.
        if (basePulse < 0.15 && bestPulse > basePulse * 1.12 &&
            std::abs(bestShift) > 0.01) {
            for (Beat& beat : beats) {
                beat.time = std::clamp(beat.time + bestShift, 0.0, duration);
                const Onset* onset = nearestOnset(result.onsets, beat.time, 0.055);
                if (onset) {
                    beat.time = onset->time;
                    beat.strength = std::max(beat.strength, onset->strength);
                    beat.confidence = std::max(beat.confidence, 0.55f);
                    beat.extrapolated = false;
                } else {
                    beat.extrapolated = true;
                }
            }
        }
    }

    // Extrapolated edge beats outside the first/last actual onset are useful
    // for a continuous grid but should not create a phantom extra beat before
    // the music starts or after it ends. This also keeps meter phase from
    // shifting by a fake leading beat.
    if (!result.onsets.empty()) {
        const double firstOnset = result.onsets.front().time;
        const double lastOnset = result.onsets.back().time;
        beats.erase(
            std::remove_if(beats.begin(), beats.end(), [&](const Beat& beat) {
                return beat.extrapolated &&
                       (beat.time < firstOnset - 0.05 ||
                        beat.time > lastOnset + 0.05);
            }),
            beats.end());
    }
    if (beats.empty()) {
        return result;
    }

    // BG Motion 3.1: refine a slow metric level only when the event evidence
    // proves that the current grid is a subharmonic. This is deliberately
    // conservative; see refineMetricLevel().
    double refinedBpm = tempo.bpm;
    const int metricMultiplier =
        refineMetricLevel(beats, result.onsets, pulseEnv, features, tempo, config,
                          &refinedBpm);
    result.metricMultiplier = metricMultiplier;
    result.refinedBpm = metricMultiplier > 1 ? refinedBpm : tempo.bpm;

    // Meter and downbeat phase. Build a grid index from elapsed musical time,
    // not from the vector index: a missing beat at a silence seam must not
    // shift every following bar by one. Meter is evaluated over 2, 3, 4 and
    // the configured hint; no confidence is implied merely by choosing one.
    std::vector<int> gridIndex(beats.size(), 0);
    for (std::size_t i = 1; i < beats.size(); ++i) {
        const double gap = beats[i].time - beats[i - 1].time;
        const double bpm = (metricMultiplier > 1 && tempo.transitions.empty())
                               ? refinedBpm
                               : bpmAtTime(tempo, beats[i].time, std::max(1.0, tempo.bpm));
        const double period = 60.0 / std::max(1.0, bpm);
        int steps = static_cast<int>(std::llround(gap / std::max(1e-6, period)));
        steps = std::clamp(steps, 1, 16);
        if (gap > 4.0 * period) {
            // After a long absence, re-entry is a more useful bar anchor than
            // pretending the missing bars were observed.
            gridIndex[i] = 0;
        } else {
            gridIndex[i] = gridIndex[i - 1] + steps;
        }
    }

    std::vector<int> meters;
    for (int meter : {config.beatsPerBarHint, 4, 3, 2}) {
        if (meter >= 2 && meter <= 8 &&
            std::find(meters.begin(), meters.end(), meter) == meters.end()) {
            meters.push_back(meter);
        }
    }

    double lowEnergyReference = 0.0;
    for (const Beat& beat : beats) {
        lowEnergyReference += lowEnergyAt(beat.time);
    }
    lowEnergyReference = beats.empty()
                             ? 0.0
                             : lowEnergyReference / static_cast<double>(beats.size());

    int bestMeter = config.beatsPerBarHint;
    int bestPhase = 0;
    double bestScore = -1.0;
    double secondScore = -1.0;
    int hintPhase = 0;
    double hintScore = -1.0;
    const int firstGridPhase = gridIndex.empty() ? 0 : gridIndex.front();
    for (int meter : meters) {
        std::vector<double> strengthMean(static_cast<std::size_t>(meter), 0.0);
        std::vector<double> pulseMean(static_cast<std::size_t>(meter), 0.0);
        std::vector<double> lowMean(static_cast<std::size_t>(meter), 0.0);
        std::vector<int> counts(static_cast<std::size_t>(meter), 0);
        for (std::size_t i = 0; i < beats.size(); ++i) {
            const int phase = (gridIndex[i] % meter + meter) % meter;
            const Beat& beat = beats[i];
            strengthMean[static_cast<std::size_t>(phase)] +=
                beat.strength * (0.5 + 0.5 * beat.confidence);
            pulseMean[static_cast<std::size_t>(phase)] += pulseAt(beat.time);
            lowMean[static_cast<std::size_t>(phase)] += lowEnergyAt(beat.time);
            ++counts[static_cast<std::size_t>(phase)];
        }
        double strengthTotal = 0.0;
        double pulseTotal = 0.0;
        double lowTotal = 0.0;
        int validPhases = 0;
        for (int phase = 0; phase < meter; ++phase) {
            const std::size_t p = static_cast<std::size_t>(phase);
            if (counts[p] == 0) {
                continue;
            }
            strengthMean[p] /= static_cast<double>(counts[p]);
            pulseMean[p] /= static_cast<double>(counts[p]);
            lowMean[p] /= static_cast<double>(counts[p]);
            strengthTotal += strengthMean[p];
            pulseTotal += pulseMean[p];
            lowTotal += lowMean[p];
            ++validPhases;
        }
        if (validPhases == 0) {
            continue;
        }
        for (int phase = 0; phase < meter; ++phase) {
            const std::size_t p = static_cast<std::size_t>(phase);
            if (counts[p] == 0) {
                continue;
            }
            const double otherStrength =
                validPhases > 1 ? (strengthTotal - strengthMean[p]) / (validPhases - 1) : 0.0;
            const double otherPulse =
                validPhases > 1 ? (pulseTotal - pulseMean[p]) / (validPhases - 1) : 0.0;
            const double otherLow =
                validPhases > 1 ? (lowTotal - lowMean[p]) / (validPhases - 1) : 0.0;
            const double lowNormalized =
                lowEnergyReference > 1e-9
                    ? std::clamp(lowMean[p] / lowEnergyReference, 0.0, 2.0)
                    : 0.0;
            // Mean evidence alone favours a sparse meter: a 4-beat downbeat
            // phase contains only the strongest event, while a 2-beat phase
            // also contains the weaker one. Contrast against the other phases
            // corrects that bias, so a genuine 6/8 (2 pulses) is not reported
            // as 12/8 (4 pulses) merely because every fourth pulse is loudest.
            const double strengthContrast = std::max(0.0, strengthMean[p] - otherStrength);
            const double pulseContrast = std::max(0.0, pulseMean[p] - otherPulse);
            const double lowContrast = std::max(0.0, lowMean[p] - otherLow);
            const double score = 0.20 * strengthMean[p] + 0.32 * pulseMean[p] +
                                 0.28 * lowNormalized +
                                 0.10 * (strengthContrast + 1.0 * pulseContrast +
                                         0.3 * lowContrast);
            double adjusted = score * (meter == config.beatsPerBarHint ? 1.07 : 1.0);
            // Music normally establishes the meter on its first strong beat;
            // this weak prior resolves otherwise indistinguishable phase
            // candidates (e.g. a kick on 1 and 3 with no bass note). It is not
            // a guarantee: the reported downbeat confidence still reflects how
            // close the alternatives were.
            if (phase == firstGridPhase % meter) {
                adjusted *= 1.06;
            }
            if (meter == config.beatsPerBarHint && adjusted > hintScore) {
                hintScore = adjusted;
                hintPhase = phase;
            }
            if (adjusted > bestScore) {
                secondScore = bestScore;
                bestScore = adjusted;
                bestMeter = meter;
                bestPhase = phase;
            } else if (adjusted > secondScore) {
                secondScore = adjusted;
            }
        }
    }

    bool longGap = false;
    bool tempoDrift = false;
    {
        const double gridBpm = metricMultiplier > 1 ? refinedBpm : tempo.bpm;
        const double expected = gridBpm > 1.0 ? 60.0 / gridBpm : 0.5;
        for (std::size_t i = 1; i < beats.size(); ++i) {
            if (beats[i].time - beats[i - 1].time > 2.5 * expected) {
                longGap = true;
                break;
            }
        }
        // A smooth tempo ramp may not create a TempoTransition, but the beat
        // interval still drifts by more than a few percent. Treat the meter as
        // uncertain in that case instead of reporting a sparse 2-beat bar.
        std::vector<double> gaps;
        gaps.reserve(beats.size());
        for (std::size_t i = 1; i < beats.size(); ++i) {
            const double gap = beats[i].time - beats[i - 1].time;
            if (std::isfinite(gap) && gap > 0.05) {
                gaps.push_back(gap);
            }
        }
        if (gaps.size() >= 12) {
            const std::size_t third = std::max<std::size_t>(4, gaps.size() / 4);
            std::vector<double> head(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(third));
            std::vector<double> tail(gaps.end() - static_cast<std::ptrdiff_t>(third), gaps.end());
            std::sort(head.begin(), head.end());
            std::sort(tail.begin(), tail.end());
            const double headGap = head[head.size() / 2];
            const double tailGap = tail[tail.size() / 2];
            if (headGap > 0.05 && tailGap > 0.05) {
                tempoDrift = std::abs(std::log2(headGap / tailGap)) > 0.08;
            }
        }
    }
    const bool unstableTempo = !tempo.stable || tempoDrift;
    const bool forceHintMeter = unstableTempo || longGap;
    if (forceHintMeter) {
        // A tempo change or a long silence breaks the observed bar continuity.
        // Claiming a sparse 2-beat meter on the remaining fragments is worse
        // than falling back to the configured meter with low confidence.
        bestMeter = std::clamp(config.beatsPerBarHint, 2, 8);
        bestPhase = hintScore > 0.0 ? hintPhase : 0;
        bestScore = hintScore;
    }

    result.beatsPerBar = bestMeter;
    if (bestScore > 0.0) {
        const double separation = secondScore > 0.0
                                      ? std::clamp((bestScore - secondScore) / bestScore,
                                                   0.0, 1.0)
                                      : 0.0;
        double confidence = 0.10 + 0.55 * separation + 0.35 * std::min(1.0, bestScore);
        if (forceHintMeter) {
            confidence = std::min(0.40, confidence * 0.60);
        }
        result.meterConfidence =
            static_cast<float>(std::clamp(confidence, 0.0, 1.0));
        result.downbeatConfidence = result.meterConfidence;
    }

    double phaseConfidenceSum = 0.0;
    int phaseConfidenceCount = 0;
    for (const Beat& beat : beats) {
        phaseConfidenceSum += beat.confidence;
        ++phaseConfidenceCount;
    }
    result.beatPhaseConfidence = phaseConfidenceCount > 0
                                     ? static_cast<float>(phaseConfidenceSum /
                                                          phaseConfidenceCount)
                                     : 0.0f;

    int bar = 0;
    bool seenDownbeat = false;
    for (std::size_t i = 0; i < beats.size(); ++i) {
        int beatInBar = (gridIndex[i] - bestPhase) % bestMeter;
        if (beatInBar < 0) {
            beatInBar += bestMeter;
        }
        beats[i].beatInBar = beatInBar;
        beats[i].downbeat = beatInBar == 0;
        if (beats[i].downbeat) {
            if (seenDownbeat) {
                ++bar;
            }
            seenDownbeat = true;
        }
        beats[i].barIndex = bar;
    }

    result.beats = std::move(beats);
    return result;
}

} // namespace bgmedia
