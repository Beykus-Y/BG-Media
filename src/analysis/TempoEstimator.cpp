#include "analysis/TempoEstimator.hpp"

#include "analysis/Dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace bgmedia {
namespace {

struct OnsetPeak {
    double frame = 0.0;
    double strength = 0.0;
};

struct PhaseScore {
    double phase = 0.0;
    double coverage = 0.0;
    double beatHitRate = 0.0;
    double onsetHitRate = 0.0;
    double onMean = 0.0;
    double offMean = 0.0;
    double grid = 0.0;
    int beatCount = 0;
    int hitCount = 0;
    // Refined period from onset regression; not recoverable from phase alone.
    double fittedLag = 0.0;
};

struct LowEvidence {
    double phase = 0.0;
    double hitRate = 0.0;
    double coverage = 0.0;
    double onMean = 0.0;
    double offMean = 0.0;
    double contrast = 0.0;
    bool valid = false;
};

std::vector<OnsetPeak> extractOnsetPeaks(const std::vector<float>& env,
                                         float threshold = 0.15f) {
    std::vector<OnsetPeak> peaks;
    const int n = static_cast<int>(env.size());
    for (int i = 1; i + 1 < n; ++i) {
        const float value = env[static_cast<std::size_t>(i)];
        if (value < threshold) {
            continue;
        }
        if (value >= env[static_cast<std::size_t>(i - 1)] &&
            value > env[static_cast<std::size_t>(i + 1)]) {
            const float offset = dsp::parabolicPeakOffset(
                env[static_cast<std::size_t>(i - 1)], value,
                env[static_cast<std::size_t>(i + 1)]);
            peaks.push_back({static_cast<double>(i) + offset, value});
        }
    }
    return peaks;
}

double acAtLag(const std::vector<double>& ac, double lag) {
    if (ac.empty()) {
        return 0.0;
    }
    if (lag <= 0.0) {
        return ac.front();
    }
    const double maxLag = static_cast<double>(ac.size() - 1);
    if (lag >= maxLag) {
        return ac.back();
    }
    const int lo = static_cast<int>(std::floor(lag));
    const int hi = std::min(static_cast<int>(ac.size() - 1), lo + 1);
    const double frac = lag - static_cast<double>(lo);
    return ac[static_cast<std::size_t>(lo)] * (1.0 - frac) +
           ac[static_cast<std::size_t>(hi)] * frac;
}

PhaseScore scorePhase(const std::vector<OnsetPeak>& peaks,
                      const std::vector<float>& env, double phase, double lag,
                      double toleranceFrames, std::vector<std::uint8_t>& beatSeen,
                      double totalPeakStrength) {
    PhaseScore score;
    score.phase = phase;
    const int n = static_cast<int>(env.size());
    if (lag < 2.0 || n <= 0) {
        return score;
    }

    const int beatCount = static_cast<int>(std::floor((n - 1 - phase) / lag)) + 1;
    if (beatCount < 3) {
        return score;
    }
    score.beatCount = beatCount;
    beatSeen.assign(static_cast<std::size_t>(beatCount), 0);

    double onEnergy = 0.0;
    double hitEnergy = 0.0;
    int hitOnsets = 0;
    int lastBeat = -1;
    int uniqueHits = 0;

    for (const OnsetPeak& peak : peaks) {
        const int k = static_cast<int>(std::llround((peak.frame - phase) / lag));
        if (k < 0 || k >= beatCount) {
            continue;
        }
        const double predicted = phase + static_cast<double>(k) * lag;
        const double distance = std::abs(peak.frame - predicted);
        if (distance > toleranceFrames) {
            continue;
        }
        const double weight = 1.0 - 0.45 * (distance / std::max(0.001, toleranceFrames));
        onEnergy += peak.strength * weight;
        hitEnergy += peak.strength;
        ++hitOnsets;
        if (lastBeat != k) {
            if (!beatSeen[static_cast<std::size_t>(k)]) {
                beatSeen[static_cast<std::size_t>(k)] = 1;
                ++uniqueHits;
            }
            lastBeat = k;
        }
    }

    double offEnergy = 0.0;
    for (int k = 0; k < beatCount; ++k) {
        const double offFrame = phase + (static_cast<double>(k) + 0.5) * lag;
        if (offFrame >= 0.0 && offFrame < n - 1.0) {
            const int lo = static_cast<int>(offFrame);
            const int hi = std::min(n - 1, lo + 1);
            const double frac = offFrame - static_cast<double>(lo);
            offEnergy += env[static_cast<std::size_t>(lo)] * (1.0 - frac) +
                         env[static_cast<std::size_t>(hi)] * frac;
        }
    }

    score.onMean = onEnergy / static_cast<double>(beatCount);
    score.offMean = offEnergy / static_cast<double>(beatCount);
    score.coverage = totalPeakStrength > 1e-9 ? hitEnergy / totalPeakStrength : 0.0;
    score.onsetHitRate = peaks.empty() ? 0.0
                                       : static_cast<double>(hitOnsets) /
                                             static_cast<double>(peaks.size());
    score.beatHitRate = static_cast<double>(uniqueHits) / static_cast<double>(beatCount);
    const double contrast = score.onMean / (score.onMean + score.offMean + 0.02);
    score.grid = 0.38 * score.coverage + 0.27 * score.beatHitRate +
                 0.20 * score.onsetHitRate + 0.15 * contrast;
    score.hitCount = hitOnsets;
    return score;
}

LowEvidence scoreLowPhase(const std::vector<OnsetPeak>& peaks,
                          const std::vector<float>& env, double phase, double lag,
                          double toleranceFrames, double totalPeakStrength) {
    LowEvidence evidence;
    evidence.phase = phase;
    const int n = static_cast<int>(env.size());
    if (lag < 2.0 || n <= 0) {
        return evidence;
    }
    const int beatCount = static_cast<int>(std::floor((n - 1 - phase) / lag)) + 1;
    if (beatCount < 3) {
        return evidence;
    }

    std::vector<std::uint8_t> seen(static_cast<std::size_t>(beatCount), 0);
    double hitStrength = 0.0;
    int uniqueHits = 0;
    for (const OnsetPeak& peak : peaks) {
        const int k = static_cast<int>(std::llround((peak.frame - phase) / lag));
        if (k < 0 || k >= beatCount) {
            continue;
        }
        const double predicted = phase + static_cast<double>(k) * lag;
        if (std::abs(peak.frame - predicted) > toleranceFrames) {
            continue;
        }
        hitStrength += peak.strength;
        if (!seen[static_cast<std::size_t>(k)]) {
            seen[static_cast<std::size_t>(k)] = 1;
            ++uniqueHits;
        }
    }

    double offEnergy = 0.0;
    for (int k = 0; k < beatCount; ++k) {
        const double offFrame = phase + (static_cast<double>(k) + 0.5) * lag;
        if (offFrame >= 0.0 && offFrame < n - 1.0) {
            const int lo = static_cast<int>(offFrame);
            const int hi = std::min(n - 1, lo + 1);
            const double frac = offFrame - static_cast<double>(lo);
            offEnergy += env[static_cast<std::size_t>(lo)] * (1.0 - frac) +
                         env[static_cast<std::size_t>(hi)] * frac;
        }
    }
    evidence.hitRate = static_cast<double>(uniqueHits) / static_cast<double>(beatCount);
    evidence.onMean = hitStrength / static_cast<double>(beatCount);
    evidence.offMean = offEnergy / static_cast<double>(beatCount);
    evidence.coverage = totalPeakStrength > 1e-9 ? hitStrength / totalPeakStrength : 0.0;
    evidence.contrast = std::clamp(
        (evidence.onMean - evidence.offMean) /
            (evidence.onMean + evidence.offMean + 0.02),
        0.0, 1.0);
    evidence.valid = true;
    return evidence;
}

LowEvidence bestLowPhase(const std::vector<OnsetPeak>& peaks,
                         const std::vector<float>& env, double lag,
                         double toleranceFrames, double totalPeakStrength,
                         double hintPhase = std::numeric_limits<double>::quiet_NaN()) {
    const int steps = std::clamp(static_cast<int>(std::lround(lag * 2.0)), 32, 160);
    LowEvidence best;
    auto tryPhase = [&](double phase) {
        LowEvidence candidate =
            scoreLowPhase(peaks, env, phase, lag, toleranceFrames, totalPeakStrength);
        const double score = candidate.hitRate * 0.55 +
                             candidate.contrast * 0.25 +
                             candidate.coverage * 0.20;
        const double bestScore = best.hitRate * 0.55 + best.contrast * 0.25 +
                                 best.coverage * 0.20;
        if (candidate.valid && (!best.valid || score > bestScore)) {
            best = candidate;
        }
    };
    if (std::isfinite(hintPhase)) {
        for (int i = -4; i <= 4; ++i) {
            tryPhase(hintPhase + static_cast<double>(i) * lag / 32.0);
        }
    }
    for (int step = 0; step < steps; ++step) {
        tryPhase(static_cast<double>(step) * lag / static_cast<double>(steps));
    }
    return best;
}

PhaseScore bestPhase(const std::vector<OnsetPeak>& peaks,
                     const std::vector<float>& env, double lag,
                     double toleranceFrames, double totalPeakStrength,
                     double hintPhase = std::numeric_limits<double>::quiet_NaN()) {
    const int steps = std::clamp(static_cast<int>(std::lround(lag * 4.0)), 48, 240);
    std::vector<std::uint8_t> beatSeen;
    PhaseScore best;
    best.grid = -1.0;

    auto tryPhase = [&](double phase) {
        PhaseScore candidate = scorePhase(peaks, env, phase, lag, toleranceFrames,
                                          beatSeen, totalPeakStrength);
        if (candidate.grid > best.grid) {
            best = candidate;
        }
    };

    if (std::isfinite(hintPhase)) {
        for (int i = -4; i <= 4; ++i) {
            tryPhase(hintPhase + static_cast<double>(i) * lag / 32.0);
        }
    }
    for (int step = 0; step < steps; ++step) {
        tryPhase(static_cast<double>(step) * lag / static_cast<double>(steps));
    }
    return best;
}

// Refines the beat grid using a least-squares fit over matched onset peaks.
// The slope becomes the period (BPM) and the intercept the phase.
PhaseScore refineWithRegression(const std::vector<OnsetPeak>& peaks,
                                const std::vector<float>& env, const PhaseScore& initial,
                                double lag, double fps, double toleranceFrames,
                                double totalPeakStrength) {
    if (initial.hitCount < 6) {
        return initial;
    }

    std::vector<std::pair<double, double>> matches;
    matches.reserve(static_cast<std::size_t>(initial.hitCount));
    for (const OnsetPeak& peak : peaks) {
        const int k = static_cast<int>(
            std::llround((peak.frame - initial.phase) / lag));
        if (k < 0 || k >= initial.beatCount) {
            continue;
        }
        const double predicted = initial.phase + static_cast<double>(k) * lag;
        if (std::abs(peak.frame - predicted) <= toleranceFrames) {
            matches.emplace_back(static_cast<double>(k), peak.frame);
        }
    }
    if (matches.size() < 6) {
        return initial;
    }

    double meanK = 0.0;
    double meanT = 0.0;
    for (const auto& [k, t] : matches) {
        meanK += k;
        meanT += t;
    }
    meanK /= static_cast<double>(matches.size());
    meanT /= static_cast<double>(matches.size());

    double cov = 0.0;
    double var = 0.0;
    for (const auto& [k, t] : matches) {
        cov += (k - meanK) * (t - meanT);
        var += (k - meanK) * (k - meanK);
    }
    if (var < 1e-9) {
        return initial;
    }
    const double slope = cov / var;
    const double intercept = meanT - slope * meanK;
    if (!std::isfinite(slope) || slope < lag * 0.95 || slope > lag * 1.05) {
        return initial;
    }

    double residual = 0.0;
    for (const auto& [k, t] : matches) {
        const double predicted = intercept + slope * k;
        residual += (t - predicted) * (t - predicted);
    }
    const double rmsFrames = std::sqrt(residual / static_cast<double>(matches.size()));
    if (rmsFrames > toleranceFrames * 0.8) {
        return initial;
    }

    const double refinedBpm = 60.0 * fps / slope;
    if (refinedBpm <= 0.0) {
        return initial;
    }
    const double refinedLag = 60.0 * fps / refinedBpm;
    PhaseScore refined = bestPhase(peaks, env, refinedLag, toleranceFrames + 0.5,
                                   totalPeakStrength, intercept);
    // Only accept if the regression did not make the grid worse. The fit is a
    // precision tool, not a replacement for the phase search.
    if (refined.grid + 0.02 < initial.grid) {
        return initial;
    }
    refined.fittedLag = refinedLag;
    return refined;
}

struct WindowRange {
    std::size_t first = 0;
    std::size_t last = 0;
};

double weightedMedianBpm(const std::vector<TempoWindow>& windows,
                         const WindowRange& range) {
    std::vector<std::pair<double, double>> values; // bpm, weight
    for (std::size_t i = range.first; i <= range.last && i < windows.size(); ++i) {
        if (windows[i].bpm > 0.0 && windows[i].confidence > 0.05f) {
            values.emplace_back(windows[i].bpm,
                                std::max(0.01, static_cast<double>(windows[i].confidence)));
        }
    }
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    double total = 0.0;
    for (const auto& v : values) {
        total += v.second;
    }
    double cumulative = 0.0;
    for (const auto& v : values) {
        cumulative += v.second;
        if (cumulative >= total * 0.5) {
            return v.first;
        }
    }
    return values.back().first;
}

double maxDeviation(const std::vector<TempoWindow>& windows,
                    const WindowRange& range, double medianBpm) {
    double maxDev = 0.0;
    for (std::size_t i = range.first; i <= range.last && i < windows.size(); ++i) {
        if (windows[i].bpm > 0.0 && windows[i].confidence > 0.15f) {
            maxDev = std::max(maxDev, std::abs(windows[i].bpm - medianBpm) / medianBpm);
        }
    }
    return maxDev;
}

double meanConfidence(const std::vector<TempoWindow>& windows,
                      const WindowRange& range) {
    double sum = 0.0;
    int count = 0;
    for (std::size_t i = range.first; i <= range.last && i < windows.size(); ++i) {
        if (windows[i].bpm > 0.0) {
            sum += windows[i].confidence;
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

std::vector<WindowRange> segmentWindows(const std::vector<TempoWindow>& windows) {
    std::vector<WindowRange> ranges;
    if (windows.empty()) {
        return ranges;
    }

    std::vector<std::size_t> valid;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        if (windows[i].bpm > 0.0 && windows[i].confidence >= 0.18f) {
            valid.push_back(i);
        }
    }
    if (valid.empty()) {
        return ranges;
    }
    if (valid.size() < 4) {
        ranges.push_back({valid.front(), valid.back()});
        return ranges;
    }

    ranges.push_back({valid.front(), valid.back()});
    bool split = true;
    int guard = 0;
    while (split && ranges.size() < 4 && guard++ < 8) {
        split = false;
        double bestStrength = 0.0;
        std::size_t bestRange = 0;
        std::size_t bestSplit = 0;

        for (std::size_t r = 0; r < ranges.size(); ++r) {
            const WindowRange& range = ranges[r];
            if (range.last <= range.first + 2) {
                continue;
            }
            for (std::size_t splitAt = range.first + 1; splitAt <= range.last - 1; ++splitAt) {
                WindowRange left{range.first, splitAt - 1};
                WindowRange right{splitAt, range.last};
                if (right.last - right.first + 1 < 2 ||
                    left.last - left.first + 1 < 2) {
                    continue;
                }
                const double leftBpm = weightedMedianBpm(windows, left);
                const double rightBpm = weightedMedianBpm(windows, right);
                if (leftBpm <= 0.0 || rightBpm <= 0.0) {
                    continue;
                }
                const double diff = std::abs(rightBpm - leftBpm) / leftBpm;
                if (diff < 0.045) {
                    continue;
                }
                if (maxDeviation(windows, left, leftBpm) > 0.025 ||
                    maxDeviation(windows, right, rightBpm) > 0.025) {
                    continue;
                }
                if (meanConfidence(windows, left) < 0.22 ||
                    meanConfidence(windows, right) < 0.22) {
                    continue;
                }
                const double strength = diff * (meanConfidence(windows, left) +
                                                meanConfidence(windows, right));
                if (strength > bestStrength) {
                    bestStrength = strength;
                    bestRange = r;
                    bestSplit = splitAt;
                }
            }
        }

        if (bestStrength > 0.0) {
            const WindowRange range = ranges[bestRange];
            std::vector<WindowRange> replacements;
            replacements.push_back({range.first, bestSplit - 1});
            replacements.push_back({bestSplit, range.last});
            ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(bestRange));
            ranges.insert(ranges.end(), replacements.begin(), replacements.end());
            std::sort(ranges.begin(), ranges.end(),
                      [](const WindowRange& a, const WindowRange& b) {
                          return a.first < b.first;
                      });
            split = true;
        }
    }
    return ranges;
}

TempoEstimate estimateTempoRange(const std::vector<float>& env,
                                 const std::vector<float>* lowEnv,
                                 const std::vector<std::uint8_t>& active,
                                 std::size_t beginFrame, std::size_t endFrame,
                                 double fps, const AnalysisConfig& config,
                                 double continuityBpm) {
    std::vector<float> slice;
    std::vector<float> lowSlice;
    double activeCount = 0.0;
    if (endFrame > beginFrame) {
        slice.resize(endFrame - beginFrame);
        if (lowEnv) {
            lowSlice.resize(endFrame - beginFrame);
        }
        for (std::size_t i = beginFrame; i < endFrame; ++i) {
            const bool isActive = active.empty() || active[i] != 0;
            slice[i - beginFrame] = isActive ? env[i] : 0.0f;
            if (lowEnv) {
                lowSlice[i - beginFrame] =
                    isActive && i < lowEnv->size() ? (*lowEnv)[i] : 0.0f;
            }
            if (isActive) {
                activeCount += 1.0;
            }
        }
    }
    const double activeFraction = slice.empty() ? 0.0
                                                : activeCount / static_cast<double>(slice.size());
    return estimateTempo(slice, fps, config, activeFraction, continuityBpm,
                         lowEnv ? &lowSlice : nullptr);
}

} // namespace

TempoEstimate estimateTempo(const std::vector<float>& env, double fps,
                            const AnalysisConfig& config, double activeFraction,
                            double continuityBpm, const std::vector<float>* lowEnv) {
    TempoEstimate result;
    if (env.size() < 8 || fps <= 0.0) {
        return result;
    }

    const auto peaks = extractOnsetPeaks(env, 0.15f);
    if (peaks.size() < 3) {
        return result;
    }
    double totalPeakStrength = 0.0;
    for (const OnsetPeak& peak : peaks) {
        totalPeakStrength += peak.strength;
    }
    if (totalPeakStrength < 1e-6) {
        return result;
    }

    const std::vector<float>& lowReference = lowEnv ? *lowEnv : env;
    const auto lowPeaks = extractOnsetPeaks(lowReference, 0.12f);
    double totalLowPeakStrength = 0.0;
    for (const OnsetPeak& peak : lowPeaks) {
        totalLowPeakStrength += peak.strength;
    }

    const double minBpm = std::max(20.0, config.minBpm);
    const double maxBpm = std::min(400.0, config.maxBpm);
    if (maxBpm <= minBpm) {
        return result;
    }

    const int minLag = std::max(2, static_cast<int>(std::floor(60.0 * fps / maxBpm)));
    const int maxLag = std::min(static_cast<int>(env.size()) - 2,
                                static_cast<int>(std::ceil(60.0 * fps / minBpm)));
    if (maxLag <= minLag) {
        return result;
    }

    const std::vector<double> ac = dsp::autocorrelation(env, maxLag + 2);

    std::vector<double> baseBpms;
    for (int lag = minLag + 1; lag < maxLag; ++lag) {
        const double value = ac[static_cast<std::size_t>(lag)];
        if (value <= 0.02) {
            continue;
        }
        if (value >= ac[static_cast<std::size_t>(lag - 1)] &&
            value >= ac[static_cast<std::size_t>(lag + 1)]) {
            const float offset = dsp::parabolicPeakOffset(
                static_cast<float>(ac[static_cast<std::size_t>(lag - 1)]),
                static_cast<float>(value),
                static_cast<float>(ac[static_cast<std::size_t>(lag + 1)]));
            const double refinedLag = static_cast<double>(lag) + offset;
            if (refinedLag > 1.0) {
                baseBpms.push_back(60.0 * fps / refinedLag);
            }
        }
    }
    if (baseBpms.empty()) {
        int bestLag = minLag;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            if (ac[static_cast<std::size_t>(lag)] >
                ac[static_cast<std::size_t>(bestLag)]) {
                bestLag = lag;
            }
        }
        baseBpms.push_back(60.0 * fps / static_cast<double>(bestLag));
    }

    std::vector<double> bpmCandidates;
    auto addCandidate = [&](double bpm) {
        if (!std::isfinite(bpm) || bpm < minBpm || bpm > maxBpm) {
            return;
        }
        for (double existing : bpmCandidates) {
            if (std::abs(existing - bpm) < 0.35) {
                return;
            }
        }
        bpmCandidates.push_back(bpm);
    };

    // Generate a metric family around every autocorrelation peak. Besides the
    // familiar 2:1 octave this explicitly represents ternary (3:1, 3:2) and
    // related 4:3 levels. The old implementation stopped at 2:1, so a 12/8
    // tatum could not be mapped back to its 66 BPM pulse.
    for (double bpm : baseBpms) {
        for (double ratio : {1.0, 0.5, 2.0, 2.0 / 3.0, 1.5, 1.0 / 3.0, 3.0,
                             0.75, 4.0 / 3.0}) {
            addCandidate(bpm * ratio);
        }
    }
    if (bpmCandidates.empty()) {
        return result;
    }

    const double toleranceFrames = std::max(2.0, 0.045 * fps);
    std::vector<TempoCandidate> evaluated;
    evaluated.reserve(bpmCandidates.size());

    for (double bpm : bpmCandidates) {
        const double lag = 60.0 * fps / bpm;
        if (lag < 2.0) {
            continue;
        }
        PhaseScore phase = bestPhase(peaks, env, lag, toleranceFrames, totalPeakStrength);
        if (phase.grid <= 0.0) {
            continue;
        }
        phase = refineWithRegression(peaks, env, phase, lag, fps,
                                     toleranceFrames, totalPeakStrength);
        double refinedBpm = phase.fittedLag > 1.0
                                ? 60.0 * fps / phase.fittedLag
                                : bpm;
        if (phase.beatCount >= 2 && phase.phase >= 0.0) {
            // Search around the fitted slope when regression succeeded,
            // rather than silently discarding the measured period.
            double bestLocalBpm = refinedBpm;
            double bestLocalScore = phase.grid;
            for (int step = -5; step <= 5; ++step) {
                const double testBpm = refinedBpm * (1.0 + 0.002 * step);
                if (testBpm < minBpm || testBpm > maxBpm) {
                    continue;
                }
                const double testLag = 60.0 * fps / testBpm;
                PhaseScore testPhase = bestPhase(peaks, env, testLag, toleranceFrames,
                                                 totalPeakStrength, phase.phase);
                if (testPhase.grid > bestLocalScore) {
                    bestLocalScore = testPhase.grid;
                    bestLocalBpm = testBpm;
                    phase = testPhase;
                }
            }
            refinedBpm = bestLocalBpm;
        }

        const double refinedLag = 60.0 * fps / refinedBpm;
        const double acScore = std::clamp(acAtLag(ac, refinedLag), 0.0, 1.2);
        const double lowTolerance = std::max(2.0, 0.035 * fps);
        // Score the low-frequency pulse against the SAME beat phase as the
        // broadband grid. An independent low-band phase can explain unrelated
        // offbeat bass and incorrectly promote a metric-relative BPM.
        LowEvidence low = scoreLowPhase(lowPeaks, lowReference, phase.phase, refinedLag,
                                        lowTolerance, totalLowPeakStrength);
        LowEvidence freeLow = bestLowPhase(lowPeaks, lowReference, refinedLag, lowTolerance,
                                           totalLowPeakStrength, phase.phase);
        const double phaseDistance =
            std::abs(std::remainder(freeLow.phase - phase.phase, refinedLag)) / refinedLag;
        const double coherence =
            std::clamp((0.28 - phaseDistance) / 0.20, 0.0, 1.0);
        const double freePulse = freeLow.valid
                                     ? 0.70 * freeLow.hitRate + 0.30 * freeLow.contrast
                                     : 0.0;
        const double prior = std::exp(
            -0.5 * std::pow(std::log2(refinedBpm / std::max(20.0, config.preferredBpm)) /
                               0.90,
                            2.0));

        // Coverage and onset-hit-rate both grow with grid density: a tatum
        // grid has roughly twice/three times as many chances to touch an
        // onset. Normalise them against the chance level of a random grid
        // before combining them with pulse-band evidence.
        const double chance = std::clamp(
            2.0 * toleranceFrames * static_cast<double>(peaks.size()) /
                std::max(1.0, static_cast<double>(env.size())),
            0.0, 0.80);
        const double precisionAdjusted =
            std::clamp((phase.beatHitRate - chance) / std::max(1e-6, 1.0 - chance), 0.0, 1.0);
        const double recallAdjusted =
            std::clamp((phase.onsetHitRate - chance) / std::max(1e-6, 1.0 - chance), 0.0, 1.0);
        const double contrast = std::clamp(
            phase.onMean / (phase.onMean + phase.offMean + 0.02), 0.0, 1.0);
        // Precision is already density-normalised by beat count. Recall,
        // coverage and raw onset-hit-rate are intentionally not used here:
        // they rise mechanically with a denser grid, which was the root cause
        // of selecting a tatum instead of the musical pulse.
        const double broadband = 0.75 * precisionAdjusted + 0.25 * contrast;
        const double alignedPulse = low.valid
                                        ? 0.70 * low.hitRate + 0.30 * low.contrast
                                        : 0.0;
        // Retain some evidence for syncopated bass, but do not treat an
        // unrelated phase as proof of the selected beat grid.
        const double pulse = std::max(alignedPulse,
                                      freePulse * (0.35 + 0.65 * coherence));
        const double evidence = 0.35 * broadband + 0.65 * pulse;

        double continuityFactor = 1.0;
        if (continuityBpm > 1.0) {
            const double ratio = refinedBpm / continuityBpm;
            const double octaveDistance = std::abs(std::log2(ratio));
            const bool metricRelative =
                std::abs(octaveDistance - 1.0) < 0.08 ||
                std::abs(octaveDistance - 2.0) < 0.10 ||
                std::abs(octaveDistance - 0.585) < 0.08 ||
                std::abs(octaveDistance - 1.585) < 0.08;
            continuityFactor =
                metricRelative ? 0.90 : 1.0 - 0.025 * std::min(1.0, octaveDistance);
        }

        TempoCandidate candidate;
        candidate.bpm = refinedBpm;
        candidate.acScore = acScore;
        candidate.gridScore = phase.grid;
        candidate.coverage = phase.coverage;
        candidate.beatHitRate = phase.beatHitRate;
        candidate.onsetHitRate = phase.onsetHitRate;
        candidate.precisionAdjusted = precisionAdjusted;
        candidate.recallAdjusted = recallAdjusted;
        candidate.pulseScore = pulse;
        candidate.lowHitRate = low.valid ? low.hitRate : 0.0;
        candidate.lowContrast = low.valid ? low.contrast : 0.0;
        candidate.prior = prior;
        candidate.score = (0.55 * evidence + 0.45 * std::clamp(acScore, 0.0, 1.0)) *
                          (0.90 + 0.10 * prior) * continuityFactor;
        evaluated.push_back(candidate);
    }

    if (evaluated.empty()) {
        return result;
    }

    std::sort(evaluated.begin(), evaluated.end(),
              [](const TempoCandidate& a, const TempoCandidate& b) {
                  return a.score > b.score;
              });

    // Deduplicate near-identical candidates after refinement.
    std::vector<TempoCandidate> unique;
    for (const TempoCandidate& candidate : evaluated) {
        bool duplicate = false;
        for (const TempoCandidate& kept : unique) {
            if (std::abs(kept.bpm - candidate.bpm) < 0.25) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            unique.push_back(candidate);
        }
        if (unique.size() >= 12) {
            break;
        }
    }
    evaluated = std::move(unique);

    auto metricRelated = [](double a, double b) {
        if (a <= 0.0 || b <= 0.0) {
            return false;
        }
        const double ratio = a / b;
        for (double relation : {0.5, 2.0, 1.0 / 3.0, 3.0, 2.0 / 3.0, 1.5,
                                0.75, 4.0 / 3.0}) {
            if (std::abs(ratio - relation) <= relation * 0.055) {
                return true;
            }
        }
        return false;
    };
    // If a candidate is a tatum/subdivision, its slower parent should have at
    // least as much low-band pulse evidence. A real fast pulse has no such
    // support gap. This is a metrical-level test, not a fixed BPM preference.
    for (TempoCandidate& candidate : evaluated) {
        double childPulse = 0.0;
        bool hasChild = false;
        for (const TempoCandidate& child : evaluated) {
            const double ratio = child.bpm / candidate.bpm;
            if ((std::abs(ratio - 2.0) < 0.10 || std::abs(ratio - 3.0) < 0.14) &&
                child.pulseScore > 0.0) {
                childPulse = std::max(childPulse, child.pulseScore);
                hasChild = true;
            }
        }
        if (hasChild) {
            candidate.levelSupport =
                std::max(0.0, candidate.pulseScore - childPulse);
            candidate.score += 0.08 * candidate.levelSupport;
        }
    }

    std::sort(evaluated.begin(), evaluated.end(),
              [](const TempoCandidate& a, const TempoCandidate& b) {
                  return a.score > b.score;
              });

    // Metric-level tie-break. The autocorrelation/evidence score often puts a
    // genuine fast pulse a few percent behind its slower subharmonic (180 BPM
    // tracked as 60, 150 as 75, 160 as 80). If a faster metric relative is
    // nearly as strong, still sits on the low pulse, and is closer to the
    // configured comfortable tempo, prefer it. The low-pulse hit-rate gate
    // prevents a hi-hat layer or an offbeat from being promoted.
    bool metricUpgrade = false;
    if (evaluated.size() > 1) {
        const TempoCandidate& current = evaluated.front();
        const double preferred = std::max(20.0, config.preferredBpm);
        const double currentPrior = std::abs(std::log2(current.bpm / preferred));
        int upgradeIndex = -1;
        double upgradeScore = -1.0;
        for (std::size_t i = 1; i < evaluated.size(); ++i) {
            const TempoCandidate& candidate = evaluated[i];
            const double ratio = candidate.bpm / std::max(1.0, current.bpm);
            const bool metricRelative =
                std::abs(ratio - 1.5) < 0.05 || std::abs(ratio - 2.0) < 0.06 ||
                std::abs(ratio - 3.0) < 0.08 || std::abs(ratio - 4.0 / 3.0) < 0.05;
            if (!metricRelative || candidate.bpm <= current.bpm) {
                continue;
            }
            if (candidate.score < current.score * 0.92) {
                continue;
            }
            if (candidate.lowHitRate < 0.85 || candidate.pulseScore + 0.10 < current.pulseScore ||
                candidate.precisionAdjusted + 0.03 < current.precisionAdjusted ||
                candidate.beatHitRate + 0.03 < current.beatHitRate) {
                continue;
            }
            if (std::abs(std::log2(candidate.bpm / preferred)) + 0.01 >= currentPrior) {
                continue;
            }
            if (candidate.score > upgradeScore) {
                upgradeScore = candidate.score;
                upgradeIndex = static_cast<int>(i);
            }
        }
        if (upgradeIndex > 0) {
            std::swap(evaluated[0], evaluated[static_cast<std::size_t>(upgradeIndex)]);
            metricUpgrade = true;
        }
    }

    const TempoCandidate& best = evaluated.front();
    result.bpm = best.bpm;

    double secondScore = 0.0;
    bool foundNonMetric = false;
    double bestMetricAlternative = 0.0;
    bool foundMetricAlternative = false;
    for (std::size_t i = 1; i < evaluated.size(); ++i) {
        if (metricRelated(evaluated[i].bpm, best.bpm)) {
            foundMetricAlternative = true;
            bestMetricAlternative = std::max(bestMetricAlternative, evaluated[i].score);
            // Two interpretations are materially close if the weaker one is
            // within 20% of the stronger score. We expose that uncertainty
            // instead of pretending the top item is certain.
            if (evaluated[i].score >= best.score * 0.50) {
                result.octaveAmbiguous = true;
            }
            continue;
        }
        if (!foundNonMetric) {
            secondScore = evaluated[i].score;
            foundNonMetric = true;
        }
    }
    for (TempoCandidate& candidate : evaluated) {
        candidate.octaveRelatedToBest = metricRelated(candidate.bpm, best.bpm);
    }
    if (metricUpgrade) {
        // The faster level was selected from a close metric alternative; this
        // is a deliberate metrical choice, not a new certainty.
        result.octaveAmbiguous = true;
    }

    const double separation =
        foundNonMetric ? std::clamp((best.score - secondScore) /
                                        std::max(0.08, best.score),
                                    0.0, 1.0)
                       : 1.0;
    const double levelSeparation =
        foundMetricAlternative
            ? std::clamp((best.score - bestMetricAlternative) /
                             std::max(0.08, best.score),
                         0.0, 1.0)
            : 0.65;
    result.levelConfidence = static_cast<float>(
        std::clamp(0.20 + 0.65 * levelSeparation + 0.15 * best.pulseScore, 0.0, 1.0));
    if (result.octaveAmbiguous) {
        result.levelConfidence *= 0.80f;
    }

    const double quality = std::clamp(
        0.45 * best.gridScore + 0.25 * best.acScore + 0.30 * best.pulseScore, 0.0, 1.0);
    double confidence =
        std::clamp(0.10 + 0.55 * quality + 0.20 * separation +
                       0.15 * result.levelConfidence,
                   0.0, 1.0);
    confidence *= std::clamp(0.55 + 0.45 * activeFraction, 0.45, 1.0);
    result.confidence = static_cast<float>(confidence);

    double bestScore = 1e-9;
    for (const TempoCandidate& candidate : evaluated) {
        bestScore = std::max(bestScore, candidate.score);
    }
    const std::size_t exposed = std::min<std::size_t>(evaluated.size(), 8);
    for (std::size_t i = 0; i < exposed; ++i) {
        result.candidates.push_back(evaluated[i]);
    }

    // Keep the internal sort scores but expose normalised ones to callers.
    for (TempoCandidate& candidate : result.candidates) {
        candidate.score /= bestScore;
    }

    return result;
}

TempoStructure estimateTempoStructure(const std::vector<float>& env,
                                      const std::vector<std::uint8_t>& active,
                                      double fps, double duration,
                                      const AnalysisConfig& config,
                                      const TempoEstimate& global,
                                      const std::vector<float>* lowEnv) {
    TempoStructure structure;
    structure.bpm = global.bpm;
    structure.confidence = global.confidence;
    structure.levelConfidence = global.levelConfidence;
    structure.octaveAmbiguous = global.octaveAmbiguous;
    for (const TempoCandidate& candidate : global.candidates) {
        structure.candidates.push_back(candidate.bpm);
        structure.candidateScores.push_back(static_cast<float>(candidate.score));
    }

    if (env.size() < 8 || fps <= 0.0 || global.bpm <= 0.0) {
        structure.stable = false;
        return structure;
    }

    const std::size_t n = env.size();
    std::size_t activeStart = n;
    std::size_t activeEnd = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (active.empty() || active[i] != 0) {
            activeStart = std::min(activeStart, i);
            activeEnd = std::max(activeEnd, i);
        }
    }
    if (activeStart >= activeEnd || activeEnd == 0) {
        structure.stable = false;
        return structure;
    }

    const double activeStartTime = static_cast<double>(activeStart) / fps;
    const double activeEndTime = std::min(duration, static_cast<double>(activeEnd + 1) / fps);

    if (!config.detectTempoChanges || duration < config.tempoWindowSeconds * 1.2) {
        TempoSegment segment;
        segment.start = activeStartTime;
        segment.end = activeEndTime;
        segment.bpm = global.bpm;
        segment.confidence = global.confidence;
        structure.segments.push_back(segment);
        structure.stable = true;
        return structure;
    }

    const int windowFrames = std::max(
        8, static_cast<int>(std::lround(config.tempoWindowSeconds * fps)));
    const int hopFrames = std::max(
        1, static_cast<int>(std::lround(config.tempoHopSeconds * fps)));
    if (windowFrames >= static_cast<int>(n)) {
        TempoSegment segment;
        segment.start = activeStartTime;
        segment.end = activeEndTime;
        segment.bpm = global.bpm;
        segment.confidence = global.confidence;
        structure.segments.push_back(segment);
        structure.stable = true;
        return structure;
    }

    std::vector<int> starts;
    for (int start = 0; start + windowFrames <= static_cast<int>(n); start += hopFrames) {
        starts.push_back(start);
    }
    if (starts.empty() || starts.back() + windowFrames < static_cast<int>(n)) {
        starts.push_back(std::max(0, static_cast<int>(n) - windowFrames));
    }

    std::vector<TempoWindow> windows;
    double continuity = 0.0;
    double previousValidCentre = -1e9;
    for (int start : starts) {
        const int end = std::min(static_cast<int>(n), start + windowFrames);
        std::vector<float> slice(static_cast<std::size_t>(end - start), 0.0f);
        std::vector<float> lowSlice;
        if (lowEnv) {
            lowSlice.assign(static_cast<std::size_t>(end - start), 0.0f);
        }
        double activeCount = 0.0;
        for (int i = start; i < end; ++i) {
            const bool isActive = active.empty() || active[static_cast<std::size_t>(i)] != 0;
            slice[static_cast<std::size_t>(i - start)] =
                isActive ? env[static_cast<std::size_t>(i)] : 0.0f;
            if (lowEnv) {
                lowSlice[static_cast<std::size_t>(i - start)] =
                    isActive && static_cast<std::size_t>(i) < lowEnv->size()
                        ? (*lowEnv)[static_cast<std::size_t>(i)]
                        : 0.0f;
            }
            if (isActive) {
                activeCount += 1.0;
            }
        }
        const double activeFraction = activeCount / static_cast<double>(end - start);
        TempoWindow window;
        window.start = static_cast<double>(start) / fps;
        window.end = std::min(duration, static_cast<double>(end) / fps);
        window.centre = 0.5 * (window.start + window.end);
        window.activeFraction = activeFraction;

        if (activeFraction >= 0.25) {
            const double useContinuity =
                (continuity > 0.0 && window.centre - previousValidCentre < 12.0)
                    ? continuity
                    : 0.0;
            const TempoEstimate estimate =
                estimateTempo(slice, fps, config, activeFraction, useContinuity,
                              lowEnv ? &lowSlice : nullptr);
            window.bpm = estimate.bpm;
            window.confidence = estimate.confidence *
                                static_cast<float>(std::clamp(activeFraction / 0.65, 0.5, 1.0));
            if (window.confidence >= 0.3f && window.bpm > 0.0) {
                continuity = window.bpm;
                previousValidCentre = window.centre;
            }
        }
        windows.push_back(window);
    }

    // Smooth tempo ramp. The segment splitter only reacts to abrupt changes;
    // a long accelerando/ritardando otherwise collapses into one average BPM
    // and the beat tracker follows the wrong local period. Fit log2(BPM) over
    // the valid windows and, when the drift is material and close to linear,
    // emit a short piecewise-linear tempo map with explicit transitions.
    {
        std::vector<const TempoWindow*> valid;
        for (const TempoWindow& window : windows) {
            if (window.bpm > 0.0 && window.confidence >= 0.18f) {
                valid.push_back(&window);
            }
        }
        if (valid.size() >= 8) {
            double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            for (const TempoWindow* window : valid) {
                const double x = window->centre;
                const double y = std::log2(std::max(20.0, window->bpm));
                sx += x;
                sy += y;
                sxx += x * x;
                sxy += x * y;
            }
            const double count = static_cast<double>(valid.size());
            const double denom = count * sxx - sx * sx;
            if (std::abs(denom) > 1e-9) {
                const double slope = (count * sxy - sx * sy) / denom;
                const double intercept = (sy - slope * sx) / count;
                double residual = 0.0;
                for (const TempoWindow* window : valid) {
                    const double predicted = intercept + slope * window->centre;
                    const double error =
                        std::log2(std::max(20.0, window->bpm)) - predicted;
                    residual += error * error;
                }
                residual = std::sqrt(residual / count);
                const double span = valid.back()->centre - valid.front()->centre;
                const double totalChange = std::abs(slope * span);
                if (totalChange > 0.10 && residual < 0.035 && span > 4.0) {
                    constexpr int kRampSegments = 4;
                    std::vector<TempoSegment> ramp;
                    ramp.reserve(kRampSegments);
                    for (int i = 0; i < kRampSegments; ++i) {
                        TempoSegment segment;
                        segment.start = activeStartTime +
                                        (activeEndTime - activeStartTime) * i / kRampSegments;
                        segment.end = activeStartTime +
                                      (activeEndTime - activeStartTime) * (i + 1) /
                                          kRampSegments;
                        const double mid = 0.5 * (segment.start + segment.end);
                        segment.bpm = std::clamp(std::exp2(intercept + slope * mid),
                                                 config.minBpm, config.maxBpm);
                        double confidenceSum = 0.0;
                        int confidenceCount = 0;
                        for (const TempoWindow* window : valid) {
                            if (window->centre >= segment.start &&
                                window->centre <= segment.end) {
                                confidenceSum += window->confidence;
                                ++confidenceCount;
                            }
                        }
                        segment.confidence =
                            confidenceCount > 0
                                ? static_cast<float>(confidenceSum / confidenceCount * 0.75)
                                : 0.25f;
                        ramp.push_back(segment);
                    }
                    std::vector<TempoTransition> transitions;
                    for (std::size_t i = 1; i < ramp.size(); ++i) {
                        TempoTransition transition;
                        transition.start = ramp[i - 1].end - 0.4;
                        transition.end = ramp[i].start + 0.4;
                        transition.fromBpm = ramp[i - 1].bpm;
                        transition.toBpm = ramp[i].bpm;
                        transition.confidence = std::min(0.5f, 0.5f *
                                                               (ramp[i - 1].confidence +
                                                                ramp[i].confidence));
                        transitions.push_back(transition);
                    }
                    structure.segments = std::move(ramp);
                    structure.transitions = std::move(transitions);
                    structure.stable = false;
                    structure.bpm = weightedMedianBpm(windows, {0, windows.size() - 1});
                    double confidenceSum = 0.0;
                    int confidenceCount = 0;
                    for (const TempoSegment& segment : structure.segments) {
                        confidenceSum += segment.confidence;
                        ++confidenceCount;
                    }
                    structure.confidence =
                        confidenceCount > 0
                            ? static_cast<float>(confidenceSum / confidenceCount * 0.65)
                            : global.confidence * 0.5f;
                    structure.octaveAmbiguous = global.octaveAmbiguous;
                    return structure;
                }
            }
        }
    }

    const auto ranges = segmentWindows(windows);
    if (ranges.empty()) {
        structure.stable = false;
        return structure;
    }

    struct Change {
        double boundary = 0.0;
        double leftBpm = 0.0;
        double rightBpm = 0.0;
        double leftCentre = 0.0;
        double rightCentre = 0.0;
        float leftConfidence = 0.0f;
        float rightConfidence = 0.0f;
    };
    std::vector<Change> changes;
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        const WindowRange& left = ranges[i - 1];
        const WindowRange& right = ranges[i];
        Change change;
        change.leftBpm = weightedMedianBpm(windows, left);
        change.rightBpm = weightedMedianBpm(windows, right);
        change.leftCentre = windows[left.last].centre;
        change.rightCentre = windows[right.first].centre;
        change.boundary = 0.5 * (change.leftCentre + change.rightCentre);
        change.leftConfidence = static_cast<float>(meanConfidence(windows, left));
        change.rightConfidence = static_cast<float>(meanConfidence(windows, right));
        changes.push_back(change);
    }

    if (changes.empty()) {
        TempoSegment segment;
        segment.start = activeStartTime;
        segment.end = activeEndTime;
        segment.bpm = global.bpm;
        segment.confidence = global.confidence;
        structure.segments.push_back(segment);
        structure.stable = true;
        return structure;
    }

    // Refine each stable range on its own envelope slice. This also gives each
    // segment an evidence-based confidence instead of inheriting a global one.
    std::vector<TempoSegment> segments;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const WindowRange& range = ranges[i];
        double start = (i == 0) ? activeStartTime : changes[i - 1].boundary;
        double end = (i + 1 == ranges.size()) ? activeEndTime : changes[i].boundary;
        const double halfTransition = std::min(1.25, config.tempoHopSeconds * 0.85 + 0.35);
        if (i > 0) {
            start += halfTransition;
        }
        if (i + 1 < ranges.size()) {
            end -= halfTransition;
        }
        if (end <= start) {
            const double mid = 0.5 * (start + end);
            start = mid - 0.25;
            end = mid + 0.25;
        }

        const std::size_t beginFrame = std::min(
            n - 1, static_cast<std::size_t>(std::max(0.0, start * fps)));
        const std::size_t endFrame = std::min(
            n, static_cast<std::size_t>(std::max(0.0, end * fps)));
        const double continuityBpm = i > 0 ? segments.back().bpm : 0.0;
        TempoEstimate local;
        if (endFrame > beginFrame) {
            local = estimateTempoRange(env, lowEnv, active, beginFrame, endFrame, fps,
                                       config, continuityBpm);
        }

        TempoSegment segment;
        segment.start = start;
        segment.end = end;
        if (local.bpm > 0.0 && local.confidence >= 0.12f) {
            segment.bpm = local.bpm;
            segment.confidence = local.confidence;
        } else {
            segment.bpm = weightedMedianBpm(windows, range);
            segment.confidence = static_cast<float>(meanConfidence(windows, range)) * 0.7f;
        }
        segments.push_back(segment);
    }

    // Drop unsupported micro-segments: if a segment is too short or its BPM is
    // statistically indistinguishable from its neighbour, merge them.
    std::vector<TempoSegment> cleaned;
    for (const TempoSegment& segment : segments) {
        if (!cleaned.empty()) {
            const double relative = std::abs(segment.bpm - cleaned.back().bpm) /
                                    std::max(1.0, cleaned.back().bpm);
            if (relative < 0.03 && segment.bpm > 0.0 && cleaned.back().bpm > 0.0) {
                cleaned.back().end = segment.end;
                cleaned.back().confidence = std::max(cleaned.back().confidence,
                                                     segment.confidence);
                continue;
            }
        }
        cleaned.push_back(segment);
    }

    if (cleaned.size() <= 1) {
        TempoSegment segment;
        segment.start = activeStartTime;
        segment.end = activeEndTime;
        segment.bpm = global.bpm;
        segment.confidence = global.confidence;
        structure.segments.push_back(segment);
        structure.stable = true;
        return structure;
    }

    // Recompute transition regions from the final segment boundaries. They are
    // reported as uncertain and never folded into a stable BPM segment.
    for (std::size_t i = 0; i < cleaned.size(); ++i) {
        if (i > 0) {
            TempoTransition transition;
            transition.start = cleaned[i - 1].end;
            transition.end = std::max(transition.start, cleaned[i].start);
            transition.fromBpm = cleaned[i - 1].bpm;
            transition.toBpm = cleaned[i].bpm;
            transition.confidence =
                std::min(0.5f, 0.5f * (cleaned[i - 1].confidence +
                                       cleaned[i].confidence));
            structure.transitions.push_back(transition);
            // Ensure stable segments do not claim the uncertain region.
            cleaned[i - 1].end = std::min(cleaned[i - 1].end, transition.start);
            cleaned[i].start = std::max(cleaned[i].start, transition.end);
        }
    }

    double weightSum = 0.0;
    double weightedBpm = 0.0;
    double confidenceSum = 0.0;
    for (const TempoSegment& segment : cleaned) {
        const double weight = std::max(0.1, segment.end - segment.start) *
                              std::max(0.05, static_cast<double>(segment.confidence));
        weightedBpm += segment.bpm * weight;
        weightSum += weight;
        confidenceSum += segment.confidence * weight;
    }
    structure.segments = std::move(cleaned);
    structure.stable = false;
    structure.bpm = weightSum > 0.0 ? weightedBpm / weightSum : global.bpm;
    structure.confidence = weightSum > 0.0
                               ? static_cast<float>((confidenceSum / weightSum) * 0.65)
                               : global.confidence * 0.6f;
    structure.octaveAmbiguous = global.octaveAmbiguous;
    return structure;
}

double bpmAtTime(const TempoStructure& structure, double time, double fallback) {
    for (const TempoTransition& transition : structure.transitions) {
        if (time >= transition.start && time <= transition.end) {
            const double span = std::max(1e-6, transition.end - transition.start);
            const double t = std::clamp((time - transition.start) / span, 0.0, 1.0);
            return transition.fromBpm + (transition.toBpm - transition.fromBpm) * t;
        }
    }
    for (const TempoSegment& segment : structure.segments) {
        if (time >= segment.start && time <= segment.end) {
            return segment.bpm;
        }
    }
    if (!structure.segments.empty()) {
        if (time < structure.segments.front().start) {
            return structure.segments.front().bpm;
        }
        return structure.segments.back().bpm;
    }
    return fallback;
}

} // namespace bgmedia
