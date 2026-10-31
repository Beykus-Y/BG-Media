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
    if (n < 8 || duration < 4.0) {
        return sections;
    }

    const int smoothRadius = std::max(1, static_cast<int>(std::lround(0.8 * features.framesPerSecond)));
    std::vector<float> energy(static_cast<std::size_t>(n), 0.0f);
    double running = 0.0;
    for (int i = 0; i < n; ++i) {
        running += rms[static_cast<std::size_t>(i)];
        if (i >= smoothRadius * 2 + 1) {
            running -= rms[static_cast<std::size_t>(i - smoothRadius * 2 - 1)];
        }
        const int count = std::min(i + 1, smoothRadius * 2 + 1);
        energy[static_cast<std::size_t>(i)] = static_cast<float>(running / count);
    }

    const float reference = std::max(1e-5f, dsp::percentile(energy, 0.90));
    std::vector<double> novelty(static_cast<std::size_t>(n), 0.0);
    for (int i = 1; i < n; ++i) {
        const double a = energy[static_cast<std::size_t>(i)] / reference;
        const double b = energy[static_cast<std::size_t>(i - 1)] / reference;
        novelty[static_cast<std::size_t>(i)] = std::max(0.0, a - b);
    }

    std::vector<double> boundaries{0.0};
    const double minSpacing = 8.0;
    double lastBoundary = -minSpacing;
    for (int i = 2; i + 2 < n; ++i) {
        const double value = novelty[static_cast<std::size_t>(i)];
        if (value < 0.18) {
            continue;
        }
        if (value < novelty[static_cast<std::size_t>(i - 1)] ||
            value <= novelty[static_cast<std::size_t>(i + 1)]) {
            continue;
        }
        const double time = static_cast<double>(i) / features.framesPerSecond;
        if (time - lastBoundary < minSpacing || time > duration - 2.0) {
            continue;
        }
        boundaries.push_back(time);
        lastBoundary = time;
        if (boundaries.size() >= 8) {
            break;
        }
    }
    if (duration - boundaries.back() > 1.0) {
        boundaries.push_back(duration);
    }
    if (boundaries.size() < 2) {
        Section section;
        section.start = 0.0;
        section.end = duration;
        section.kind = SectionKind::Main;
        section.energy = 0.7f;
        section.confidence = 0.2f;
        sections.push_back(section);
        return sections;
    }

    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        Section section;
        section.start = boundaries[i];
        section.end = boundaries[i + 1];
        if (section.end <= section.start) {
            continue;
        }
        const int a = std::clamp(static_cast<int>(section.start * features.framesPerSecond),
                                 0, n - 1);
        const int b = std::clamp(static_cast<int>(section.end * features.framesPerSecond),
                                 0, n);
        double mean = 0.0;
        for (int frame = a; frame < b; ++frame) {
            mean += energy[static_cast<std::size_t>(frame)];
        }
        mean /= static_cast<double>(std::max(1, b - a));
        section.energy = static_cast<float>(std::clamp(mean / reference, 0.0, 1.0));
        section.confidence = 0.25f;

        const bool first = i == 0;
        const bool last = i + 2 == boundaries.size();
        if (section.energy < 0.42f) {
            section.kind = first ? SectionKind::Intro
                                 : (last ? SectionKind::Outro : SectionKind::Breakdown);
        } else if (!last && i + 2 < boundaries.size()) {
            // Compare with the next section to distinguish a build.
            const double nextStart = boundaries[i + 1];
            const int c = std::clamp(static_cast<int>(nextStart * features.framesPerSecond),
                                     0, n - 1);
            const int d = std::clamp(static_cast<int>(boundaries[i + 2] * features.framesPerSecond),
                                     0, n);
            double nextMean = 0.0;
            for (int frame = c; frame < d; ++frame) {
                nextMean += energy[static_cast<std::size_t>(frame)];
            }
            nextMean /= static_cast<double>(std::max(1, d - c));
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
