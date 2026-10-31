#include "analysis/EventDetection.hpp"

#include "analysis/Dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <vector>

namespace bgmedia {
namespace {

bool insideSilence(const std::vector<SilenceRegion>& silence, double time,
                   double margin = 0.0) {
    for (const SilenceRegion& region : silence) {
        if (time >= region.start - margin && time <= region.end + margin) {
            return true;
        }
    }
    return false;
}

int nearestBeatIndex(const std::vector<Beat>& beats, double time, double maxDistance) {
    if (beats.empty()) {
        return -1;
    }
    const auto it = std::lower_bound(beats.begin(), beats.end(), time,
                                     [](const Beat& beat, double value) {
                                         return beat.time < value;
                                     });
    int best = -1;
    double bestDistance = maxDistance;
    if (it != beats.end()) {
        const double distance = std::abs(it->time - time);
        if (distance <= bestDistance) {
            best = static_cast<int>(it - beats.begin());
            bestDistance = distance;
        }
    }
    if (it != beats.begin()) {
        const auto previous = it - 1;
        const double distance = std::abs(previous->time - time);
        if (distance < bestDistance) {
            best = static_cast<int>(previous - beats.begin());
        }
    }
    return best;
}

} // namespace

SilenceAnalysis detectSilence(const SpectralFeatures& features,
                              const AnalysisConfig& config, double duration) {
    SilenceAnalysis result;
    const auto& rms = features.rms;
    const int n = static_cast<int>(rms.size());
    result.active.assign(static_cast<std::size_t>(n), 1);
    if (n == 0 || duration <= 0.0) {
        return result;
    }

    const float peak = std::max(1e-6f, dsp::percentile(rms, 0.95));
    const double relative = std::pow(10.0, config.silenceThresholdDb / 20.0);
    const float threshold = std::max(1e-4f, static_cast<float>(peak * relative));

    const int minFrames = std::max(1, static_cast<int>(
        std::lround(config.minSilenceSeconds * features.framesPerSecond)));
    std::vector<std::pair<int, int>> ranges;

    int start = -1;
    for (int i = 0; i < n; ++i) {
        const bool silent = rms[static_cast<std::size_t>(i)] < threshold;
        if (silent && start < 0) {
            start = i;
        } else if (!silent && start >= 0) {
            ranges.emplace_back(start, i);
            start = -1;
        }
    }
    if (start >= 0) {
        ranges.emplace_back(start, n);
    }

    // Ignore tiny dips between notes and merge regions separated by a short
    // non-silent blip.
    const int mergeGap = std::max(1, static_cast<int>(
        std::lround(0.22 * features.framesPerSecond)));
    std::vector<std::pair<int, int>> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first - merged.back().second <= mergeGap) {
            merged.back().second = range.second;
        } else {
            merged.push_back(range);
        }
    }

    auto frameToTime = [&](int frame) {
        return std::clamp(static_cast<double>(frame) / features.framesPerSecond,
                          0.0, duration);
    };

    for (const auto& range : merged) {
        if (range.second - range.first < minFrames) {
            continue;
        }
        SilenceRegion region;
        region.start = frameToTime(range.first);
        region.end = frameToTime(range.second);
        if (region.end - region.start < config.minSilenceSeconds) {
            continue;
        }

        double meanRms = 0.0;
        for (int i = range.first; i < range.second; ++i) {
            meanRms += rms[static_cast<std::size_t>(i)];
        }
        meanRms /= static_cast<double>(range.second - range.first);
        region.confidence = static_cast<float>(
            std::clamp(1.0 - meanRms / std::max(1e-9, static_cast<double>(threshold) * 2.5),
                       0.25, 1.0));
        result.regions.push_back(region);

        for (int i = range.first; i < range.second; ++i) {
            result.active[static_cast<std::size_t>(i)] = 0;
        }
    }

    // Frames beyond the end of the decoded audio are padding, not audio.
    for (int i = 0; i < n; ++i) {
        const double time = static_cast<double>(i) / features.framesPerSecond;
        if (time >= duration) {
            result.active[static_cast<std::size_t>(i)] = 0;
        }
    }

    double silentDuration = 0.0;
    for (const SilenceRegion& region : result.regions) {
        silentDuration += region.end - region.start;
    }
    result.activeRatio = std::clamp(1.0 - silentDuration / duration, 0.0, 1.0);
    return result;
}

std::vector<BassAccent> detectBassAccents(
    const SpectralFeatures& features, const std::vector<SilenceRegion>& silence,
    const std::vector<Beat>& beats, double duration, const AnalysisConfig& config) {
    std::vector<BassAccent> accents;
    const auto& lowEnv = features.lowEnv;
    const auto& ratio = features.lowRatio;
    const int n = static_cast<int>(lowEnv.size());
    if (n < 3 || duration <= 0.0) {
        return accents;
    }

    const double sensitivity = std::max(0.2, config.bassAccentSensitivity);
    const double threshold = std::max(0.10, 0.20 / sensitivity);
    // The decision uses the share of linear spectral energy below ~180 Hz,
    // not total loudness. A bass drum has a large low-band share even when a
    // click transient also excites higher bands; a broadband swell does not.
    const double energyRatioThreshold = std::clamp(0.30 - 0.045 * sensitivity, 0.16, 0.45);
    const double minFluxRatio = 0.06;
    const double minSpacing = 0.085;



    for (int i = 1; i + 1 < n; ++i) {
        const float value = lowEnv[static_cast<std::size_t>(i)];
        if (value < threshold) {
            continue;
        }
        if (value < lowEnv[static_cast<std::size_t>(i - 1)] ||
            value <= lowEnv[static_cast<std::size_t>(i + 1)]) {
            continue;
        }
        const float energyShare = features.lowEnergyRatio[static_cast<std::size_t>(i)];
        if (energyShare < energyRatioThreshold ||
            ratio[static_cast<std::size_t>(i)] < minFluxRatio) {
            continue;
        }

        const float peakOffset = dsp::parabolicPeakOffset(
            lowEnv[static_cast<std::size_t>(i - 1)], value,
            lowEnv[static_cast<std::size_t>(i + 1)]);
        const double frame = static_cast<double>(i) + peakOffset;
        const double time = frame / features.framesPerSecond + features.onsetTimeOffset;
        if (time < 0.0 || time > duration || insideSilence(silence, time, 0.03)) {
            continue;
        }

        BassAccent accent;
        accent.time = time;
        accent.strength = std::min(1.5f, value);
        accent.lowBandRatio = energyShare;
        const double ratioScore = std::clamp(
            (accent.lowBandRatio - energyRatioThreshold) /
                std::max(0.05, 1.0 - energyRatioThreshold),
            0.0, 1.0);
        accent.confidence = static_cast<float>(std::clamp(
            0.15 + 0.45 * std::min(1.0, static_cast<double>(accent.strength)) +
                0.40 * ratioScore,
            0.0, 1.0));

        // Find the closest beat first, then apply the local-period gate.
        // A fixed 110 ms pre-filter wrongly rejects aligned accents at 55 BPM
        // even though the accepted window is 28% of the beat interval.
        int beat = nearestBeatIndex(beats, accent.time,
                                    std::numeric_limits<double>::infinity());
        double offset = 0.0;
        if (beat >= 0) {
            offset = accent.time - beats[static_cast<std::size_t>(beat)].time;
            // Distinguish a beat-aligned low attack from an offbeat bass
            // event. This is deliberately relative to the local beat period;
            // a fixed 110 ms window is wider than half a beat at 198 BPM.
            const std::size_t index = static_cast<std::size_t>(beat);
            double localPeriod = 0.5;
            if (index + 1 < beats.size()) {
                localPeriod = beats[index + 1].time - beats[index].time;
            } else if (index > 0) {
                localPeriod = beats[index].time - beats[index - 1].time;
            }
            if (std::abs(offset) > 0.28 * std::max(0.05, localPeriod)) {
                // Keep the measurable offset for diagnostics/UI, but do not
                // claim this low-frequency attack is beat-aligned.
                beat = -1;
            }
        }
        accent.nearestBeat = beat;
        accent.offsetFromBeat = offset;

        if (!accents.empty() && accent.time - accents.back().time < minSpacing) {
            if (accent.strength > accents.back().strength) {
                accents.back() = accent;
            }
            continue;
        }
        accents.push_back(accent);
    }

    return accents;
}

std::vector<Section> detectSections(const SpectralFeatures& features,
                                    double duration, const AnalysisConfig&) {
    std::vector<Section> sections;
    const auto& rms = features.rms;
    const int n = static_cast<int>(rms.size());
    if (n < 8 || duration < 4.0 || features.framesPerSecond <= 0.0) {
        return sections;
    }

    // A frame-to-frame derivative of a ~1.6 s moving average almost never
    // reaches a useful section-change threshold. Compare *sustained* energy
    // on both sides of each possible boundary instead. The log ratio is
    // insensitive to mastering gain and works in both directions (drops/builds).
    const double fps = features.framesPerSecond;
    const int radius = std::max(3, static_cast<int>(std::lround(0.85 * fps)));
    // Long-term evidence distinguishes a genuine new section from one loud
    // kick or a repeating drum fill inside an otherwise unchanged phrase.
    const int contextRadius = std::max(radius,
        std::min(n / 4, static_cast<int>(std::lround(4.0 * fps))));
    const int stride = std::max(1, static_cast<int>(std::lround(0.10 * fps)));
    const double reference = std::max(1e-5, static_cast<double>(dsp::percentile(rms, 0.90)));
    const double floor = std::max(1e-6, reference * 0.025);
    std::vector<double> prefix(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = 0; i < n; ++i) {
        prefix[static_cast<std::size_t>(i + 1)] =
            prefix[static_cast<std::size_t>(i)] + std::max(0.0f, rms[static_cast<std::size_t>(i)]);
    }
    auto average = [&](int from, int to) {
        const int a = std::clamp(from, 0, n);
        const int b = std::clamp(to, a + 1, n);
        return (prefix[static_cast<std::size_t>(b)] -
                prefix[static_cast<std::size_t>(a)]) / static_cast<double>(b - a);
    };

    // Optional spectral display data adds a timbral cue when the level stays
    // constant. It is already logarithmic and must only be treated as a
    // relative novelty signal, not a calibrated spectral distance.
    const bool haveSpectrum = features.spectrogram.valid() &&
                              features.spectrogram.framesPerSecond > 0.0;
    auto timbreNovelty = [&](double time, double windowSeconds) {
        if (!haveSpectrum) {
            return 0.0;
        }
        const Spectrogram& spec = features.spectrogram;
        const int frames = static_cast<int>(spec.frameCount);
        const int centre = std::clamp(
            static_cast<int>(std::lround(time * spec.framesPerSecond)), 0, frames - 1);
        const int window = std::max(2, static_cast<int>(
            std::lround(windowSeconds * spec.framesPerSecond)));
        const int leftStart = std::max(0, centre - window);
        const int rightEnd = std::min(frames, centre + window);
        if (leftStart >= centre || rightEnd <= centre) {
            return 0.0;
        }
        double change = 0.0;
        for (int band = 0; band < spec.bands; ++band) {
            double left = 0.0;
            double right = 0.0;
            for (int f = leftStart; f < centre; ++f) {
                left += spec.magnitudes[static_cast<std::size_t>(f) * spec.bands + band];
            }
            for (int f = centre; f < rightEnd; ++f) {
                right += spec.magnitudes[static_cast<std::size_t>(f) * spec.bands + band];
            }
            left /= static_cast<double>(centre - leftStart);
            right /= static_cast<double>(rightEnd - centre);
            change += std::abs(right - left) / 255.0;
        }
        return change / static_cast<double>(spec.bands);
    };

    struct Change {
        double time = 0.0;
        double score = 0.0;
    };
    std::vector<Change> candidates;
    for (int frame = contextRadius; frame + contextRadius < n; frame += stride) {
        const double left = average(frame - radius, frame);
        const double right = average(frame, frame + radius);
        const double longLeft = average(frame - contextRadius, frame);
        const double longRight = average(frame, frame + contextRadius);
        if (std::max(left, right) < 0.10 * reference) {
            continue;
        }
        const double time = static_cast<double>(frame) / fps;
        if (time < 2.0 || time > duration - 2.0) {
            continue;
        }
        const double shortChange =
            std::min(3.0, std::abs(std::log((right + floor) / (left + floor))));
        const double longChange =
            std::min(3.0, std::abs(std::log((longRight + floor) / (longLeft + floor))));
        const double energyChange = std::min(shortChange, 1.6 * longChange);
        const double spectralChange =
            std::min(timbreNovelty(time, 0.55), 1.4 * timbreNovelty(time, 1.8));
        const double novelty = energyChange + 1.8 * spectralChange;
        if (novelty >= 0.30) {
            candidates.push_back({time, novelty});
        }
    }

    // Non-maximum suppression: sustained changes occupy several candidate
    // frames, but should create a single section boundary.
    std::sort(candidates.begin(), candidates.end(),
              [](const Change& a, const Change& b) { return a.score > b.score; });
    std::vector<Change> selected;
    constexpr double kMinSpacing = 6.0;
    for (const Change& candidate : candidates) {
        bool nearExisting = false;
        for (const Change& kept : selected) {
            if (std::abs(candidate.time - kept.time) < kMinSpacing) {
                nearExisting = true;
                break;
            }
        }
        if (!nearExisting) {
            selected.push_back(candidate);
        }
        if (selected.size() == 7) {
            break;
        }
    }

    std::vector<double> boundaries{0.0};
    for (const Change& change : selected) {
        boundaries.push_back(change.time);
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.push_back(duration);

    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        Section section;
        section.start = boundaries[i];
        section.end = boundaries[i + 1];
        if (section.end <= section.start) {
            continue;
        }
        const int a = std::clamp(static_cast<int>(section.start * fps), 0, n - 1);
        const int b = std::clamp(static_cast<int>(section.end * fps), a + 1, n);
        const double mean = average(a, b);
        section.energy = static_cast<float>(std::clamp(mean / reference, 0.0, 1.0));
        // No verse/chorus labels are inferred from energy alone.
        section.confidence = selected.empty() ? 0.20f : 0.30f;
        const bool first = i == 0;
        const bool last = i + 2 == boundaries.size();
        if (section.energy < 0.42f) {
            section.kind = first ? SectionKind::Intro
                                 : (last ? SectionKind::Outro : SectionKind::Breakdown);
        } else if (!last && i + 2 < boundaries.size()) {
            const int c = std::clamp(static_cast<int>(boundaries[i + 1] * fps), 0, n - 1);
            const int d = std::clamp(static_cast<int>(boundaries[i + 2] * fps), c + 1, n);
            const double nextMean = average(c, d);
            section.kind = nextMean > mean * 1.18 ? SectionKind::Build : SectionKind::Main;
        } else if (last && section.energy < 0.65f) {
            section.kind = SectionKind::Outro;
        } else {
            section.kind = SectionKind::Main;
        }
        sections.push_back(section);
    }
    return sections;
}

} // namespace bgmedia
