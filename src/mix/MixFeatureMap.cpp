#include "bgmedia/mix/MixFeatureMap.hpp"

#include "analysis/Dsp.hpp"
#include "bgmedia/mix/HarmonicAnalyzer.hpp"
#include "bgmedia/mix/VocalDetector.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>
#include <vector>

namespace bgmedia::mix {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hashMixU32(std::uint64_t& h, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        h ^= static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
        h *= kFnvPrime;
    }
}

void hashMixU64(std::uint64_t& h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF);
        h *= kFnvPrime;
    }
}

void hashMixF64(std::uint64_t& h, double value) noexcept {
    if (value == 0.0) {
        value = 0.0; // normalise -0.0
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hashMixU64(h, bits);
}

struct Biquad {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double z1 = 0.0;
    double z2 = 0.0;

    double process(double x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ITU-R BS.1770-4 Stage 1 high-shelf pre-filter (+3.9998 dB above ~1682 Hz).
Biquad makeKWeightStage1(double sampleRate) noexcept {
    Biquad bq;
    if (sampleRate <= 100.0) {
        return bq;
    }
    const double f0 = std::min(1681.974450955533, sampleRate * 0.45);
    const double q = 0.7071752369554196;
    const double vh = std::pow(10.0, 3.999843853973347 / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double k = std::tan(dsp::kPi * f0 / sampleRate);
    const double k2 = k * k;
    const double a0 = 1.0 + k / q + k2;
    bq.b0 = (vh + vb * k / q + k2) / a0;
    bq.b1 = 2.0 * (k2 - vh) / a0;
    bq.b2 = (vh - vb * k / q + k2) / a0;
    bq.a1 = 2.0 * (k2 - 1.0) / a0;
    bq.a2 = (1.0 - k / q + k2) / a0;
    return bq;
}

// ITU-R BS.1770-4 Stage 2 RLB high-pass filter (~38.14 Hz).
Biquad makeKWeightStage2(double sampleRate) noexcept {
    Biquad bq;
    if (sampleRate <= 100.0) {
        return bq;
    }
    const double f0 = std::min(38.13547087602444, sampleRate * 0.45);
    const double q = 0.5003270373238773;
    const double k = std::tan(dsp::kPi * f0 / sampleRate);
    const double k2 = k * k;
    const double a0 = 1.0 + k / q + k2;
    bq.b0 = 1.0 / a0;
    bq.b1 = -2.0 / a0;
    bq.b2 = 1.0 / a0;
    bq.a1 = 2.0 * (k2 - 1.0) / a0;
    bq.a2 = (1.0 - k / q + k2) / a0;
    return bq;
}

float powerToLufs(double meanSquare) noexcept {
    if (!(meanSquare > 1e-14) || !std::isfinite(meanSquare)) {
        return -70.0f;
    }
    const double lufs = -0.691 + 10.0 * std::log10(meanSquare);
    return static_cast<float>(std::clamp(lufs, -70.0, 12.0));
}

float rmsToDbfs(double rms) noexcept {
    if (!(rms > 1e-7) || !std::isfinite(rms)) {
        return -90.0f;
    }
    const double db = 20.0 * std::log10(rms);
    return static_cast<float>(std::clamp(db, -90.0, 12.0));
}

bool insideSilenceRegion(const std::vector<SilenceRegion>& silences,
                         double time, double margin = 0.0) noexcept {
    for (const SilenceRegion& s : silences) {
        if (time >= s.start - margin && time <= s.end + margin) {
            return true;
        }
    }
    return false;
}

int findNearestBeat(const std::vector<Beat>& beats, double time,
                    double maxDistanceSeconds, bool requireDownbeat = false) noexcept {
    int bestIndex = -1;
    double bestDist = maxDistanceSeconds;
    for (std::size_t i = 0; i < beats.size(); ++i) {
        if (requireDownbeat && !beats[i].downbeat) {
            continue;
        }
        const double d = std::abs(beats[i].time - time);
        if (d <= bestDist) {
            bestDist = d;
            bestIndex = static_cast<int>(i);
        }
    }
    return bestIndex;
}

void computeAudibleBoundsAndActivity(MixFeatureMap& map,
                                     const AnalysisResult& analysis,
                                     const MixFeatureConfig& config) {
    const int n = static_cast<int>(map.frames.size());
    if (n == 0 || map.info.duration <= 0.0) {
        return;
    }

    std::vector<float> frameDb(static_cast<std::size_t>(n), -90.0f);
    std::vector<float> nonZeroDb;
    nonZeroDb.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float db = rmsToDbfs(map.frames[static_cast<std::size_t>(i)].rms);
        frameDb[static_cast<std::size_t>(i)] = db;
        if (db > -89.5f) {
            nonZeroDb.push_back(db);
        }
    }

    const float minDb = dsp::percentile(frameDb, 0.01);
    const float rawRef = nonZeroDb.empty() ? -70.0f : dsp::percentile(nonZeroDb, 0.90);

    const float reference = std::clamp(rawRef, -70.0f, 3.0f);
    // If the 1st percentile is within 18 dB of the 90th-percentile reference,
    // the track has no silent lead-in/out frames to sample a noise floor from.
    const float rawNoiseFloor =
        (reference - minDb >= 18.0f) ? minDb : std::min(-80.0f, reference - 36.0f);
    const float noiseFloor =
        std::clamp(rawNoiseFloor, -90.0f, std::min(-32.0f, reference - 20.0f));

    const float marginAboveNoise =
        static_cast<float>(std::clamp(config.adaptiveMarginAboveNoiseDb, 3.0, 24.0));
    const float hysteresis =
        static_cast<float>(std::clamp(config.adaptiveHysteresisDb, 1.5, 12.0));

    // Adaptive threshold scales with both the track's noise floor and its
    // active reference level rather than cutting globally at -55 dBFS.
    const float enterDb = std::clamp(
        std::max(noiseFloor + marginAboveNoise, reference - 34.0f),
        -84.0f,
        reference - 12.0f);
    const float exitDb = std::max(noiseFloor + 3.0f, enterDb - hysteresis);
    const float bodyDb = reference - 10.5f;

    map.audible.noiseFloorDbfs = noiseFloor;
    map.audible.referenceDbfs = reference;
    map.audible.enterThresholdDbfs = enterDb;
    map.audible.exitThresholdDbfs = exitDb;

    // Forward hysteresis pass for per-frame activity.
    bool state = false;
    int activeFrames = 0;
    for (int i = 0; i < n; ++i) {
        const float db = frameDb[static_cast<std::size_t>(i)];
        if (!state && db >= enterDb) {
            state = true;
        } else if (state && db < exitDb) {
            state = false;
        }
        map.frames[static_cast<std::size_t>(i)].active = state;
        if (state) {
            ++activeFrames;
        }
    }

    if (activeFrames == 0) {
        // Extremely quiet signal: fall back to frames above noiseFloor + 3 dB.
        for (int i = 0; i < n; ++i) {
            if (frameDb[static_cast<std::size_t>(i)] > noiseFloor + 3.0f &&
                !insideSilenceRegion(analysis.silences,
                                     map.frames[static_cast<std::size_t>(i)].time)) {
                map.frames[static_cast<std::size_t>(i)].active = true;
                ++activeFrames;
            }
        }
    }

    int firstActive = -1;
    int lastActive = -1;
    int firstBody = -1;
    int lastBody = -1;
    for (int i = 0; i < n; ++i) {
        if (map.frames[static_cast<std::size_t>(i)].active) {
            if (firstActive < 0) {
                firstActive = i;
            }
            lastActive = i;
            if (frameDb[static_cast<std::size_t>(i)] >= bodyDb) {
                if (firstBody < 0) {
                    firstBody = i;
                }
                lastBody = i;
            }
        }
    }

    const double step = map.info.frameStepSeconds;
    if (firstActive < 0) {
        map.audible.audibleStart = 0.0;
        map.audible.fullBodyStart = 0.0;
        map.audible.tailStart = map.info.duration;
        map.audible.audibleEnd = map.info.duration;
        map.audible.confidence = 0.15f;
        return;
    }

    map.audible.audibleStart = std::clamp(firstActive * step, 0.0, map.info.duration);
    map.audible.audibleEnd =
        std::clamp((lastActive + 1) * step, map.audible.audibleStart, map.info.duration);
    map.audible.fullBodyStart =
        firstBody >= 0
            ? std::clamp(firstBody * step, map.audible.audibleStart, map.audible.audibleEnd)
            : map.audible.audibleStart;
    map.audible.tailStart =
        lastBody >= 0
            ? std::clamp((lastBody + 1) * step, map.audible.fullBodyStart,
                         map.audible.audibleEnd)
            : map.audible.audibleEnd;

    const double introRamp = map.audible.fullBodyStart - map.audible.audibleStart;
    const double tailSpan = map.audible.audibleEnd - map.audible.tailStart;
    map.audible.hasFadeIn = introRamp >= 1.4;
    map.audible.hasFadeOut = tailSpan >= 2.0;

    // Distinguish a reverb/decay tail from a long musical fade-out by checking
    // whether onset density in the tail is sparse while energy decays smoothly.
    if (tailSpan >= 0.6 && lastBody >= 0 && lastActive > lastBody) {
        double tailOnset = 0.0;
        int tailCount = 0;
        for (int i = lastBody; i <= lastActive; ++i) {
            tailOnset += map.frames[static_cast<std::size_t>(i)].onsetDensity;
            ++tailCount;
        }
        const double meanTailOnset = tailCount > 0 ? tailOnset / tailCount : 0.0;
        map.audible.hasReverbTail = meanTailOnset < 0.25 && tailSpan <= 6.0;
    }

    const float snr = std::clamp((reference - noiseFloor - 6.0f) / 30.0f, 0.0f, 1.0f);
    map.audible.confidence = std::clamp(0.25f + 0.70f * snr, 0.20f, 0.98f);
}

void buildBarsPhrasesAndCues(MixFeatureMap& map,
                             const AnalysisResult& analysis,
                             const MixFeatureConfig& config) {
    const double duration = map.info.duration;
    if (duration <= 0.0 || map.frames.empty()) {
        return;
    }

    const int bpb = std::clamp(analysis.beatsPerBar > 0 ? analysis.beatsPerBar : 4, 1, 8);

    // 1. Build BarFeatureSummary directly from BG Beat's beat grid.
    if (!analysis.beats.empty()) {
        struct BeatGroup {
            int barIndex = 0;
            int sourceBarIndex = -1;
            std::size_t firstBeat = 0;
            std::size_t lastBeat = 0;
        };
        std::vector<BeatGroup> groups;
        for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
            const Beat& b = analysis.beats[i];
            const bool startNew =
                groups.empty() ||
                (b.barIndex >= 0 && groups.back().sourceBarIndex >= 0 &&
                 b.barIndex != groups.back().sourceBarIndex) ||
                (b.downbeat && i > groups.back().firstBeat) ||
                (static_cast<int>(i - groups.back().firstBeat) >= bpb);
            if (startNew) {
                const int nextBarIdx =
                    groups.empty() ? std::max(0, b.barIndex) : (groups.back().barIndex + 1);
                groups.push_back({nextBarIdx, b.barIndex, i, i});
            } else {
                groups.back().lastBeat = i;
                if (groups.back().sourceBarIndex < 0 && b.barIndex >= 0) {
                    groups.back().sourceBarIndex = b.barIndex;
                }
            }
        }

        double meanBeatPeriod = analysis.bpm > 0.0 ? 60.0 / analysis.bpm : 0.5;
        if (analysis.beats.size() >= 2) {
            meanBeatPeriod = (analysis.beats.back().time - analysis.beats.front().time) /
                             static_cast<double>(analysis.beats.size() - 1);
        }

        map.bars.reserve(groups.size());
        for (std::size_t g = 0; g < groups.size(); ++g) {
            const BeatGroup& grp = groups[g];
            const Beat& first = analysis.beats[grp.firstBeat];
            const Beat& last = analysis.beats[grp.lastBeat];
            BarFeatureSummary bar;
            bar.barIndex = grp.barIndex;
            bar.start = std::clamp(first.time, 0.0, duration);
            bar.end = (g + 1 < groups.size())
                          ? std::clamp( analysis.beats[groups[g + 1].firstBeat].time,
                                       bar.start + 1e-3, duration)
                          : std::clamp(last.time + meanBeatPeriod, bar.start + 1e-3, duration);
            bar.beatCount = static_cast<int>(grp.lastBeat - grp.firstBeat + 1);
            bar.downbeatTime = bar.start;
            bar.downbeatConfidence =
                first.downbeat ? std::max(first.confidence, analysis.downbeatConfidence)
                               : first.confidence * 0.6f;

            const WindowFeatureSummary win = map.summarizeWindow(bar.start, bar.end);
            bar.meanEnergy = win.meanEnergy;
            bar.momentaryLufs = win.meanLufs;
            bar.lowShare = win.lowShare;
            bar.midShare = win.midShare;
            bar.highShare = win.highShare;
            bar.bassDominance = win.bassDominance;
            bar.onsetDensity = win.onsetDensity;
            bar.spectralNovelty = win.spectralNovelty;
            bar.vocalScore = win.vocalScore;
            bar.vocalConfidence = win.vocalConfidence;
            bar.sectionKind = map.sectionAt(0.5 * (bar.start + bar.end));

            // Boundary strength at the bar start (downbeat + local novelty/energy jump).
            const MixFeatureFrame startFrame = map.sampleAt(bar.start);
            bar.boundaryStrength = std::clamp(
                0.55f * startFrame.boundaryStrength +
                    0.25f * startFrame.spectralNovelty +
                    0.20f * bar.downbeatConfidence,
                0.0f, 1.0f);
            map.bars.push_back(bar);
        }
    }

    // 2. Build PhraseSpan boundaries from section changes + bar-level novelty
    // and energy shifts (never assuming every phrase is a rigid 16/32-beat block).
    if (!map.bars.empty()) {
        std::vector<std::size_t> phraseStarts{0};
        for (std::size_t b = 1; b < map.bars.size(); ++b) {
            const BarFeatureSummary& prev = map.bars[b - 1];
            const BarFeatureSummary& curr = map.bars[b];
            const std::size_t barsSinceLast = b - phraseStarts.back();

            const bool sectionChanged = curr.sectionKind != prev.sectionKind &&
                                        curr.sectionKind != SectionKind::Unknown;
            const float energyJump = std::abs(curr.meanEnergy - prev.meanEnergy);
            const bool strongNovelty =
                curr.boundaryStrength >= 0.52f ||
                (curr.spectralNovelty >= 0.45f && energyJump >= 0.18f);
            const bool regularPhraseMultiple =
                (barsSinceLast == 4 || barsSinceLast == 8);

            if (sectionChanged && barsSinceLast >= 1) {
                phraseStarts.push_back(b);
            } else if (barsSinceLast >= 2 && strongNovelty &&
                       (barsSinceLast >= 4 || energyJump >= 0.25f)) {
                phraseStarts.push_back(b);
            } else if (barsSinceLast >= 8 ||
                       (regularPhraseMultiple && curr.boundaryStrength >= 0.34f)) {
                phraseStarts.push_back(b);
            }
        }

        map.phrases.reserve(phraseStarts.size());
        for (std::size_t p = 0; p < phraseStarts.size(); ++p) {
            const std::size_t b0 = phraseStarts[p];
            const std::size_t b1 =
                (p + 1 < phraseStarts.size()) ? phraseStarts[p + 1] : map.bars.size();
            if (b1 <= b0) {
                continue;
            }
            PhraseSpan span;
            span.start = map.bars[b0].start;
            span.end = map.bars[b1 - 1].end;
            span.barStart = map.bars[b0].barIndex;
            span.barCount = static_cast<int>(b1 - b0);
            for (std::size_t b = b0; b < b1; ++b) {
                span.beatCount += map.bars[b].beatCount;
            }
            const WindowFeatureSummary win = map.summarizeWindow(span.start, span.end);
            span.sectionKind = win.dominantSection;
            span.meanEnergy = win.meanEnergy;
            span.energySlope = win.energySlope;
            span.bassDominance = win.bassDominance;
            span.boundaryStrengthStart = map.bars[b0].boundaryStrength;
            span.boundaryStrengthEnd =
                (b1 < map.bars.size()) ? map.bars[b1].boundaryStrength
                                       : map.sampleAt(span.end).boundaryStrength;
            span.regularMeter =
                (span.beatCount == span.barCount * bpb) && analysis.meterConfidence >= 0.35f;
            span.confidence = std::clamp(
                0.35f * analysis.beatPhaseConfidence +
                    0.25f * analysis.downbeatConfidence +
                    0.25f * span.boundaryStrengthStart +
                    0.15f * (span.regularMeter ? 1.0f : 0.4f),
                0.10f, 0.98f);
            map.phrases.push_back(span);
        }
    } else if (!analysis.sections.empty()) {
        for (const Section& sec : analysis.sections) {
            if (sec.end <= sec.start) {
                continue;
            }
            PhraseSpan span;
            span.start = sec.start;
            span.end = sec.end;
            span.sectionKind = sec.kind;
            const WindowFeatureSummary win = map.summarizeWindow(span.start, span.end);
            span.meanEnergy = win.meanEnergy;
            span.energySlope = win.energySlope;
            span.bassDominance = win.bassDominance;
            span.boundaryStrengthStart = map.sampleAt(span.start).boundaryStrength;
            span.boundaryStrengthEnd = map.sampleAt(span.end).boundaryStrength;
            span.regularMeter = false;
            span.confidence = std::clamp(sec.confidence, 0.15f, 0.65f);
            map.phrases.push_back(span);
        }
    }

    // 3. Candidate Mix-In and Mix-Out cues.
    const double beatPeriod = analysis.bpm > 0.0 ? 60.0 / analysis.bpm : 0.5;
    auto makeCandidate = [&](double rawTime, CueRole role, CueKind kind,
                             float phraseConf, const std::string& reason) {
        CuePointCandidate c;
        c.role = role;
        c.kind = kind;
        c.time = std::clamp(rawTime, 0.0, duration);

        // Snap to nearest downbeat or beat if within half a beat and grid is usable.
        if (!analysis.beats.empty() && analysis.beatPhaseConfidence >= 0.35f) {
            const int dbIdx = findNearestBeat(analysis.beats, c.time, beatPeriod * 0.65, true);
            const int btIdx = findNearestBeat(analysis.beats, c.time, beatPeriod * 0.50, false);
            const int chosenIdx = (dbIdx >= 0 && analysis.downbeatConfidence >= 0.35f)
                                      ? dbIdx
                                      : btIdx;
            if (chosenIdx >= 0) {
                const Beat& b = analysis.beats[static_cast<std::size_t>(chosenIdx)];
                c.time = std::clamp(b.time, 0.0, duration);
                c.beatIndex = chosenIdx;
                c.barIndex = b.barIndex;
                c.rhythmConfidence = std::clamp(
                    0.5f * b.confidence + 0.5f * analysis.beatPhaseConfidence, 0.0f, 1.0f);
            }
        }

        c.sectionKind = map.sectionAt(c.time);
        c.boundaryStrength = map.sampleAt(c.time).boundaryStrength;
        c.phraseConfidence = phraseConf;

        const WindowFeatureSummary before =
            map.summarizeWindow(std::max(0.0, c.time - 4.0), c.time);
        const WindowFeatureSummary after =
            map.summarizeWindow(c.time, std::min(duration, c.time + 4.0));
        c.energyBefore = before.meanEnergy;
        c.energyAfter = after.meanEnergy;
        c.bassDominance = (role == CueRole::MixIn) ? after.bassDominance : before.bassDominance;
        c.availableWindowSeconds =
            (role == CueRole::MixIn)
                ? std::max(0.0, map.audible.audibleEnd - c.time)
                : std::max(0.0, map.audible.audibleEnd - c.time);
        c.vocalBefore = before.vocalScore;
        c.vocalAfter = after.vocalScore;
        c.vocalConfidence = std::max(before.vocalConfidence, after.vocalConfidence);
        c.harmonicStability = (role == CueRole::MixIn) ? after.harmonicSalience
                                                       : before.harmonicSalience;
        c.reason = reason;

        // Deterministic prior score based on musical alignment, boundary
        // clarity, and sufficient room in the track.
        float score = 0.30f * c.boundaryStrength +
                      0.30f * c.phraseConfidence +
                      0.25f * c.rhythmConfidence;
        if (role == CueRole::MixIn) {
            const bool EnoughRoom = c.availableWindowSeconds >= std::min(8.0, duration * 0.5);
            if (EnoughRoom) {
                score += 0.15f;
            }
            if (c.sectionKind == SectionKind::Intro || c.sectionKind == SectionKind::Build) {
                score += 0.08f;
            }
        } else {
            if (c.availableWindowSeconds >= 4.0 && c.availableWindowSeconds <= 40.0) {
                score += 0.15f;
            }
            if (c.sectionKind == SectionKind::Outro ||
                c.sectionKind == SectionKind::Breakdown) {
                score += 0.10f;
            }
        }
        c.priorScore = std::clamp(score, 0.05f, 1.0f);
        return c;
    };

    auto addUniqueCandidate = [](std::vector<CuePointCandidate>& list,
                                 const CuePointCandidate& cand,
                                 double minSeparation) {
        for (CuePointCandidate& existing : list) {
            if (std::abs(existing.time - cand.time) <= minSeparation) {
                if (cand.priorScore > existing.priorScore + 1e-4f) {
                    existing = cand;
                }
                return;
            }
        }
        list.push_back(cand);
    };

    const double minSep = std::max(0.35, beatPeriod * 0.75);

    // Mix-In candidates: audible start, full-body start, early downbeats,
    // phrase starts and section transitions in the first 60% of the track.
    addUniqueCandidate(
        map.mixInCandidates,
        makeCandidate(map.audible.audibleStart, CueRole::MixIn,
                      CueKind::AudibleBoundary, map.audible.confidence,
                      "adaptive audible start"),
        minSep);

    if (map.audible.fullBodyStart > map.audible.audibleStart + minSep &&
        map.audible.fullBodyStart < duration * 0.55) {
        addUniqueCandidate(
            map.mixInCandidates,
            makeCandidate(map.audible.fullBodyStart, CueRole::MixIn,
                          CueKind::EnergyPeakOrDrop, map.audible.confidence,
                          "full-body energy entry after intro ramp"),
            minSep);
    }

    for (const Beat& b : analysis.beats) {
        if (b.downbeat && b.time >= map.audible.audibleStart - 0.1 &&
            b.time <= std::min(duration * 0.35, map.audible.audibleStart + 16.0)) {
            addUniqueCandidate(
                map.mixInCandidates,
                makeCandidate(b.time, CueRole::MixIn, CueKind::Downbeat,
                              std::max(b.confidence, analysis.downbeatConfidence),
                              "early downbeat cue"),
                minSep);
            if (map.mixInCandidates.size() >= 5) {
                break;
            }
        }
    }

    for (const PhraseSpan& phrase : map.phrases) {
        if (phrase.start >= map.audible.audibleStart &&
            phrase.start <= duration * 0.65 &&
            duration - phrase.start >= std::min(4.0, duration * 0.35)) {
            const CueKind kind =
                (phrase.sectionKind == SectionKind::Breakdown)
                    ? CueKind::BreakdownEntry
                    : (phrase.energySlope > 0.04f || phrase.meanEnergy >= 0.72f)
                          ? CueKind::EnergyPeakOrDrop
                          : CueKind::PhraseBoundary;
            addUniqueCandidate(
                map.mixInCandidates,
                makeCandidate(phrase.start, CueRole::MixIn, kind, phrase.confidence,
                              "phrase entry boundary"),
                minSep);
        }
    }

    for (const Section& sec : analysis.sections) {
        if (sec.start > map.audible.audibleStart + minSep &&
            sec.start <= duration * 0.65) {
            addUniqueCandidate(
                map.mixInCandidates,
                makeCandidate(sec.start, CueRole::MixIn, CueKind::SectionBoundary,
                              sec.confidence, "section transition entry"),
                minSep);
        }
    }

    // Mix-Out candidates: phrase boundaries in the second half, section
    // transitions into Breakdown/Outro, tail start, and pre-silence edges.
    const double minMixOutTail = std::min(2.0, duration * 0.20);
    for (const PhraseSpan& phrase : map.phrases) {
        if (phrase.start >= duration * 0.35 &&
            map.audible.audibleEnd - phrase.start >= minMixOutTail) {
            const CueKind kind =
                (phrase.sectionKind == SectionKind::Outro)
                    ? CueKind::OutroStart
                    : (phrase.sectionKind == SectionKind::Breakdown)
                          ? CueKind::BreakdownEntry
                          : CueKind::PhraseBoundary;
            addUniqueCandidate(
                map.mixOutCandidates,
                makeCandidate(phrase.start, CueRole::MixOut, kind, phrase.confidence,
                              "phrase boundary mix-out"),
                minSep);
        }
    }

    for (const Section& sec : analysis.sections) {
        if (sec.start >= duration * 0.30 &&
            map.audible.audibleEnd - sec.start >= minMixOutTail) {
            const CueKind kind = (sec.kind == SectionKind::Outro)
                                     ? CueKind::OutroStart
                                     : (sec.kind == SectionKind::Breakdown)
                                           ? CueKind::BreakdownEntry
                                           : CueKind::SectionBoundary;
            addUniqueCandidate(
                map.mixOutCandidates,
                makeCandidate(sec.start, CueRole::MixOut, kind, sec.confidence,
                              "section boundary mix-out"),
                minSep);
        }
    }

    if (map.audible.tailStart >= duration * 0.40 &&
        map.audible.audibleEnd - map.audible.tailStart >= 1.0) {
        addUniqueCandidate(
            map.mixOutCandidates,
            makeCandidate(map.audible.tailStart, CueRole::MixOut,
                          CueKind::AudibleBoundary, map.audible.confidence,
                          "adaptive tail start before fade-out"),
            minSep);
    }

    const double defaultOutCue =
        std::clamp(map.audible.audibleEnd - std::min(8.0, duration * 0.35),
                   0.0, std::max(0.0, duration - minMixOutTail));
    addUniqueCandidate(
        map.mixOutCandidates,
        makeCandidate(defaultOutCue, CueRole::MixOut, CueKind::PhraseBoundary,
                      std::max(0.35f, analysis.beatPhaseConfidence),
                      "tail crossfade window cue"),
        minSep);

    for (const SilenceRegion& s : analysis.silences) {
        if (s.start >= duration * 0.35 && s.start <= duration - 0.5) {
            // Distinguish true end-of-track silence / breakdown from a short dramatic pause
            // inside a continuing section or drop.
            const double pauseLen = s.end - s.start;
            bool dramaticBreak = false;
            if (pauseLen <= 3.5 && s.end + 2.0 < duration) {
                const double checkTime = std::min(duration - 0.1, s.end + 0.5);
                const MixFeatureFrame after = map.sampleAt(checkTime);
                if (after.active && (after.energy >= 0.40f || after.bassDominance >= 0.38f)) {
                    dramaticBreak = true;
                }
            }
            if (!dramaticBreak) {
                const double preSilence = std::max(0.0, s.start - std::min(4.0, s.start * 0.25));
                addUniqueCandidate(
                    map.mixOutCandidates,
                    makeCandidate(preSilence, CueRole::MixOut, CueKind::SilenceEdge,
                                  s.confidence, "pre-silence mix-out window"),
                    minSep);
            }
        }
    }

    // Add vocal phrase handoff candidates when vocal regions are detected.
    if (map.vocal.available) {
        for (const VocalRegion& reg : map.vocal.regions) {
            if (reg.leadLikely && reg.end >= duration * 0.30 &&
                map.audible.audibleEnd - reg.end >= minMixOutTail) {
                addUniqueCandidate(
                    map.mixOutCandidates,
                    makeCandidate(reg.end, CueRole::MixOut, CueKind::PhraseBoundary,
                                  reg.confidence, "vocal phrase end handoff cue"),
                    minSep);
            }
            if (reg.leadLikely && reg.start >= map.audible.audibleStart + minSep &&
                reg.start <= duration * 0.55) {
                const double preVocal =
                    std::max(map.audible.audibleStart,
                             reg.start - std::min(8.0, reg.start - map.audible.audibleStart));
                addUniqueCandidate(
                    map.mixInCandidates,
                    makeCandidate(preVocal, CueRole::MixIn, CueKind::PhraseBoundary,
                                  reg.confidence, "instrumental lead-in before vocal entry"),
                    minSep);
            }
        }
    }

    auto sortAndTrim = [](std::vector<CuePointCandidate>& list, int maxCount) {
        std::sort(list.begin(), list.end(),
                  [](const CuePointCandidate& a, const CuePointCandidate& b) {
                      if (std::abs(a.priorScore - b.priorScore) > 1e-4f) {
                          return a.priorScore > b.priorScore;
                      }
                      return a.time < b.time;
                  });
        if (maxCount > 0 && static_cast<int>(list.size()) > maxCount) {
            list.resize(static_cast<std::size_t>(maxCount));
        }
        std::sort(list.begin(), list.end(),
                  [](const CuePointCandidate& a, const CuePointCandidate& b) {
                      return a.time < b.time;
                  });
    };

    sortAndTrim(map.mixInCandidates, std::max(1, config.maxMixInCandidates));
    sortAndTrim(map.mixOutCandidates, std::max(1, config.maxMixOutCandidates));
}

} // namespace

const char* vocalDetectorKindName(VocalDetectorKind kind) noexcept {
    switch (kind) {
        case VocalDetectorKind::None:
            return "None";
        case VocalDetectorKind::SpectralProxy:
            return "Spectral proxy (heuristic)";
        case VocalDetectorKind::MusicVocalModel:
            return "Music vocal detector";
        case VocalDetectorKind::ExternalStem:
            return "External vocal stem";
    }
    return "Unknown";
}

const char* cueKindName(CueKind kind) noexcept {
    switch (kind) {
        case CueKind::AudibleBoundary:
            return "Audible boundary";
        case CueKind::Downbeat:
            return "Downbeat";
        case CueKind::PhraseBoundary:
            return "Phrase boundary";
        case CueKind::SectionBoundary:
            return "Section boundary";
        case CueKind::BreakdownEntry:
            return "Breakdown entry";
        case CueKind::EnergyPeakOrDrop:
            return "Energy peak/drop";
        case CueKind::OutroStart:
            return "Outro start";
        case CueKind::SilenceEdge:
            return "Silence edge";
    }
    return "Cue";
}

std::uint64_t MixFeatureConfig::fingerprint() const noexcept {
    std::uint64_t h = kFnvOffset;
    hashMixF64(h, std::clamp(frameStepSeconds, 0.04, 0.50));
    hashMixF64(h, lowBandMaxHz);
    hashMixF64(h, midBandMaxHz);
    hashMixF64(h, adaptiveMarginAboveNoiseDb);
    hashMixF64(h, adaptiveHysteresisDb);
    hashMixU64(h, static_cast<std::uint64_t>(maxMixOutCandidates));
    hashMixU64(h, static_cast<std::uint64_t>(maxMixInCandidates));
    hashMixU64(h, static_cast<std::uint64_t>(vocalDetector));
    hashMixU64(h, vocalModelVersion);
    hashMixU64(h, vocalModelChecksum);
    hashMixU64(h, harmonicAnalyzeVersion);
    return h;
}

bool MixFeatureMap::valid() const noexcept {
    if (!std::isfinite(info.duration) || info.duration <= 0.0 ||
        !std::isfinite(info.frameStepSeconds) || info.frameStepSeconds <= 0.0 ||
        info.sampleRate == 0 || info.sourceChannels == 0 || frames.empty()) {
        return false;
    }
    if (!std::isfinite(loudness.integratedLufs) ||
        !std::isfinite(loudness.shortTermMaxLufs) ||
        !std::isfinite(loudness.momentaryMaxLufs) ||
        !std::isfinite(loudness.loudnessRangeLu) ||
        !std::isfinite(loudness.meanRmsDbfs) ||
        !std::isfinite(loudness.samplePeakDbfs)) {
        return false;
    }
    if (!std::isfinite(audible.audibleStart) ||
        !std::isfinite(audible.audibleEnd) ||
        audible.audibleStart < -1e-4 ||
        audible.audibleEnd > info.duration + 0.5 ||
        audible.audibleEnd < audible.audibleStart - 1e-4) {
        return false;
    }
    double prevTime = -1.0;
    for (const MixFeatureFrame& f : frames) {
        if (!std::isfinite(f.time) || !std::isfinite(f.rms) ||
            !std::isfinite(f.momentaryLufs) || !std::isfinite(f.shortTermLufs) ||
            !std::isfinite(f.energy) || !std::isfinite(f.lowShare) ||
            !std::isfinite(f.midShare) || !std::isfinite(f.highShare) ||
            !std::isfinite(f.bassDominance) || !std::isfinite(f.onsetDensity) ||
            !std::isfinite(f.spectralNovelty) ||
            !std::isfinite(f.boundaryStrength) ||
            !std::isfinite(f.vocalScore) || !std::isfinite(f.vocalConfidence) ||
            !std::isfinite(f.harmonicSalience) || f.time < prevTime - 1e-6) {
            return false;
        }
        prevTime = f.time;
    }
    for (const CuePointCandidate& c : mixInCandidates) {
        if (!std::isfinite(c.time) || c.time < -1e-3 ||
            c.time > info.duration + 1e-3 || !std::isfinite(c.priorScore)) {
            return false;
        }
    }
    for (const CuePointCandidate& c : mixOutCandidates) {
        if (!std::isfinite(c.time) || c.time < -1e-3 ||
            c.time > info.duration + 1e-3 || !std::isfinite(c.priorScore)) {
            return false;
        }
    }
    return true;
}

MixFeatureFrame MixFeatureMap::sampleAt(double time) const noexcept {
    if (frames.empty()) {
        return {};
    }
    if (time <= frames.front().time) {
        return frames.front();
    }
    if (time >= frames.back().time) {
        return frames.back();
    }
    const double step = std::max(1e-6, info.frameStepSeconds);
    const std::size_t idx = std::min<std::size_t>(
        frames.size() - 1,
        static_cast<std::size_t>(std::max(0.0, std::floor(time / step))));
    return frames[idx];
}

double MixFeatureMap::localBpmAt(double time, bool* stableOut) const noexcept {
    for (const TempoTransition& tr : tempoTransitions) {
        if (time >= tr.start && time <= tr.end) {
            if (stableOut) {
                *stableOut = false;
            }
            const double span = std::max(1e-6, tr.end - tr.start);
            const double alpha = std::clamp((time - tr.start) / span, 0.0, 1.0);
            return tr.fromBpm + (tr.toBpm - tr.fromBpm) * alpha;
        }
    }
    for (const TempoSegment& seg : tempoSegments) {
        if (time >= seg.start && time <= seg.end && seg.bpm > 0.0) {
            if (stableOut) {
                *stableOut = (seg.confidence <= 0.0f || seg.confidence >= 0.35f);
            }
            return seg.bpm;
        }
    }
    if (stableOut) {
        *stableOut = tempoStable && globalBpm > 0.0;
    }
    return globalBpm;
}

SectionKind MixFeatureMap::sectionAt(double time, float* confidenceOut) const noexcept {
    for (const Section& sec : sections) {
        if (time >= sec.start && time <= sec.end) {
            if (confidenceOut) {
                *confidenceOut = sec.confidence;
            }
            return sec.kind;
        }
    }
    if (confidenceOut) {
        *confidenceOut = 0.0f;
    }
    return SectionKind::Unknown;
}

WindowFeatureSummary MixFeatureMap::summarizeWindow(double start,
                                                    double end) const noexcept {
    WindowFeatureSummary out;
    if (frames.empty() || info.duration <= 0.0) {
        return out;
    }
    out.start = std::clamp(std::min(start, end), 0.0, info.duration);
    out.end = std::clamp(std::max(start, end), out.start, info.duration);
    out.duration = out.end - out.start;

    const double step = std::max(1e-6, info.frameStepSeconds);
    const double startProbe =
        (out.end > out.start) ? std::min(out.end, out.start + 1e-7 * step) : out.start;
    const double endProbe =
        (out.end > out.start) ? std::max(out.start, out.end - 1e-7 * step) : out.end;
    const std::size_t i0 = std::min<std::size_t>(
        frames.size() - 1,
        static_cast<std::size_t>(std::max(0.0, std::floor(startProbe / step))));
    const std::size_t i1 = std::max<std::size_t>(
        i0,
        std::min<std::size_t>(
            frames.size() - 1,
            static_cast<std::size_t>(std::max(0.0, std::floor(endProbe / step)))));

    const std::size_t count = i1 - i0 + 1;
    double sumLufsPower = 0.0;
    float peakLufs = -70.0f;
    double sumRms = 0.0;
    double sumEnergy = 0.0;
    double sumLow = 0.0;
    double sumMid = 0.0;
    double sumHigh = 0.0;
    double sumBass = 0.0;
    double sumOnset = 0.0;
    double sumNovelty = 0.0;
    double sumVocal = 0.0;
    double sumVocalConf = 0.0;
    double sumHarmonic = 0.0;
    int activeCount = 0;

    // Linear regression accumulators for energySlope (energy units per second).
    double sumT = 0.0;
    double sumTE = 0.0;
    double sumT2 = 0.0;
    const double tRef = frames[i0].time;

    for (std::size_t i = i0; i <= i1; ++i) {
        const MixFeatureFrame& f = frames[i];
        const double p = std::pow(10.0, (static_cast<double>(f.momentaryLufs) + 0.691) / 10.0);
        sumLufsPower += p;
        peakLufs = std::max(peakLufs, f.momentaryLufs);
        sumRms += f.rms;
        sumEnergy += f.energy;
        sumLow += f.lowShare;
        sumMid += f.midShare;
        sumHigh += f.highShare;
        sumBass += f.bassDominance;
        sumOnset += f.onsetDensity;
        sumNovelty += f.spectralNovelty;
        sumVocal += f.vocalScore;
        sumVocalConf += f.vocalConfidence;
        sumHarmonic += f.harmonicSalience;
        if (f.active) {
            ++activeCount;
        }
        const double dt = f.time - tRef;
        sumT += dt;
        sumTE += dt * f.energy;
        sumT2 += dt * dt;
    }

    const double inv = 1.0 / static_cast<double>(count);
    out.meanLufs = powerToLufs(sumLufsPower * inv);
    out.peakLufs = peakLufs;
    out.meanRms = static_cast<float>(sumRms * inv);
    out.meanEnergy = static_cast<float>(sumEnergy * inv);
    out.lowShare = static_cast<float>(sumLow * inv);
    out.midShare = static_cast<float>(sumMid * inv);
    out.highShare = static_cast<float>(sumHigh * inv);
    out.bassDominance = static_cast<float>(sumBass * inv);
    out.onsetDensity = static_cast<float>(sumOnset * inv);
    out.spectralNovelty = static_cast<float>(sumNovelty * inv);
    out.activeRatio = static_cast<float>(activeCount * inv);
    out.vocalScore = static_cast<float>(sumVocal * inv);
    out.vocalConfidence = static_cast<float>(sumVocalConf * inv);
    out.harmonicSalience = static_cast<float>(sumHarmonic * inv);

    const double denom = count * sumT2 - sumT * sumT;
    if (count >= 2 && denom > 1e-9) {
        out.energySlope = static_cast<float>((count * sumTE - sumT * sumEnergy) / denom);
    }

    bool stableMid = true;
    const double midTime = 0.5 * (out.start + out.end);
    out.localBpm = localBpmAt(midTime, &stableMid);
    bool stableStart = true;
    bool stableEnd = true;
    const double bpmStart = localBpmAt(out.start, &stableStart);
    const double bpmEnd = localBpmAt(out.end, &stableEnd);
    bool windowStable = stableMid && stableStart && stableEnd &&
                        std::abs(bpmEnd - bpmStart) <= 1.0 &&
                        std::abs(out.localBpm - bpmStart) <= 1.0;
    if (windowStable) {
        for (const TempoTransition& tr : tempoTransitions) {
            if (tr.end > out.start + 1e-4 && tr.start < out.end - 1e-4) {
                windowStable = false;
                break;
            }
        }
    }
    out.tempoStable = windowStable;

    float secConf = 0.0f;
    out.dominantSection = sectionAt(midTime, &secConf);
    out.confidence = std::clamp(
        0.45f * (info.fromPcm ? 0.90f : 0.55f) +
            0.35f * beatPhaseConfidence +
            0.20f * out.activeRatio,
        0.10f, 1.0f);
    return out;
}

MixFeatureExtractor::MixFeatureExtractor(
    MixFeatureConfig config, const IVocalActivityDetector* customVocalDetector)
    : config_(std::move(config)), customVocalDetector_(customVocalDetector) {
    config_.frameStepSeconds = std::clamp(config_.frameStepSeconds, 0.04, 0.50);
    config_.lowBandMaxHz = std::clamp(config_.lowBandMaxHz, 60.0, 400.0);
    config_.midBandMaxHz = std::clamp(config_.midBandMaxHz, 1200.0, 8000.0);
    if (customVocalDetector_ != nullptr) {
        config_.vocalDetector = customVocalDetector_->kind();
        config_.vocalModelVersion = customVocalDetector_->modelVersion();
        config_.vocalModelChecksum = customVocalDetector_->modelChecksum();
    }
}

std::uint64_t MixFeatureExtractor::contentHashFor(
    InterleavedAudioView source,
    const PcmBuffer& monoPcm,
    const AnalysisResult& analysis) noexcept {
    const std::uint64_t baseHash =
        monoPcm.contentHash != 0 ? monoPcm.contentHash : analysis.info.contentHash;
    if (!source.valid() || source.channels <= 1) {
        return baseHash;
    }
    std::uint64_t h = baseHash != 0 ? baseHash : kFnvOffset;
    hashMixU64(h, static_cast<std::uint64_t>(source.sampleRate));
    hashMixU64(h, static_cast<std::uint64_t>(source.channels));
    hashMixU64(h, static_cast<std::uint64_t>(source.sampleCount));
    for (std::size_t i = 0; i < source.sampleCount; ++i) {
        float sample = source.samples[i];
        if (!std::isfinite(sample) || sample == 0.0f) {
            sample = 0.0f;
        }
        std::uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        h ^= static_cast<std::uint64_t>(bits);
        h *= kFnvPrime;
    }
    return h;
}

PcmBuffer MixFeatureExtractor::downmixViewToMono(
    InterleavedAudioView source,
    const AnalysisResult& analysis) {
    PcmBuffer out;
    if (!source.valid()) {
        return out;
    }
    const std::size_t frames = source.frameCount();
    const std::uint32_t channels = source.channels;
    out.samples.assign(frames, 0.0f);
    out.sampleRate = source.sampleRate;
    out.sourceChannels = channels;

    const float invChannels = 1.0f / static_cast<float>(channels);
    std::vector<double> channelPower(channels, 0.0);
    double monoPower = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        double sum = 0.0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            float value = source.samples[frame * channels + ch];
            if (!std::isfinite(value)) {
                value = 0.0f;
            }
            sum += value;
            channelPower[ch] += static_cast<double>(value) * value;
        }
        out.samples[frame] = static_cast<float>(sum * invChannels);
        monoPower += static_cast<double>(out.samples[frame]) * out.samples[frame];
    }

    if (channels == 2) {
        const auto loudest = std::max_element(channelPower.begin(), channelPower.end());
        if (loudest != channelPower.end() && *loudest > 1e-12 &&
            monoPower < *loudest * 0.12) {
            const std::size_t channel =
                static_cast<std::size_t>(loudest - channelPower.begin());
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const float value = source.samples[frame * channels + channel];
                out.samples[frame] = std::isfinite(value) ? value : 0.0f;
            }
        }
    }

    std::uint64_t h = contentHashOf(out.samples);
    hashMixU32(h, out.sampleRate);
    hashMixU32(h, out.sourceChannels);
    hashMixU64(h, static_cast<std::uint64_t>(out.samples.size()));
    out.contentHash = (h != 0) ? h : analysis.info.contentHash;
    return out;
}

MixFeatureMap MixFeatureExtractor::extract(const PcmBuffer& pcm,
                                           const AnalysisResult& analysis,
                                           const PcmTrack* stereoTrack) const {
    const InterleavedAudioView view =
        (stereoTrack != nullptr && stereoTrack->valid())
            ? InterleavedAudioView::fromTrack(*stereoTrack)
            : InterleavedAudioView{};
    return extractFromView(view, pcm, analysis);
}

MixFeatureMap MixFeatureExtractor::extract(const AudioData& audio,
                                           const PcmBuffer& pcm,
                                           const AnalysisResult& analysis) const {
    return extractFromView(InterleavedAudioView::fromAudio(audio), pcm, analysis);
}

MixFeatureMap MixFeatureExtractor::extractFromView(InterleavedAudioView source,
                                                   const PcmBuffer& pcm,
                                                   const AnalysisResult& analysis) const {
    if (!pcm.valid()) {
        return buildFromAnalysisOnly(analysis);
    }

    MixFeatureMap map;
    map.info.schemaVersion = kMixFeatureSchemaVersion;
    map.info.extractorVersion = kMixFeatureExtractorVersion;
    map.info.analyzerVersion = analysis.info.analyzerVersion > 0
                                   ? analysis.info.analyzerVersion
                                   : kAnalyzerVersion;
    map.info.contentHash = contentHashFor(source, pcm, analysis);
    map.info.analysisConfigHash = analysis.info.configHash;
    map.info.featureConfigHash = config_.fingerprint();
    map.info.vocalDetectorKind = config_.vocalDetector;
    map.info.vocalModelVersion = config_.vocalModelVersion;
    map.info.vocalModelChecksum = config_.vocalModelChecksum;
    map.info.harmonicAnalyzeVersion = config_.harmonicAnalyzeVersion;
    map.info.duration = pcm.duration();
    map.info.sampleRate = pcm.sampleRate;
    map.info.sourceChannels =
        (source.valid() && source.channels > 0) ? source.channels : pcm.sourceChannels;
    map.info.frameStepSeconds = config_.frameStepSeconds;
    map.info.fromPcm = true;

    map.globalBpm = analysis.bpm;
    map.bpmConfidence = analysis.bpmConfidence;
    map.beatPhaseConfidence = analysis.beatPhaseConfidence;
    map.downbeatConfidence = analysis.downbeatConfidence;
    map.beatsPerBar = analysis.beatsPerBar > 0 ? analysis.beatsPerBar : 4;
    map.meterConfidence = analysis.meterConfidence;
    map.tempoStable = analysis.tempoStable;
    map.tempoSegments = analysis.tempoSegments;
    map.tempoTransitions = analysis.tempoTransitions;
    map.sections = analysis.sections;

    const double sr = static_cast<double>(pcm.sampleRate);
    const std::size_t totalSamples = pcm.samples.size();
    const int stepSamples = std::max(1, static_cast<int>(std::lround(config_.frameStepSeconds * sr)));
    const double actualStep = static_cast<double>(stepSamples) / sr;
    map.info.frameStepSeconds = actualStep;

    const int frameCount = std::max(1, static_cast<int>((totalSamples + stepSamples - 1) / stepSamples));
    map.frames.resize(static_cast<std::size_t>(frameCount));

    // 1. Single pass over source channels (when available) or mono PCM for
    // ITU-R BS.1770-4 K-weighted channel power sum, plain RMS, and sample peak.
    const bool useMultiChannel =
        source.valid() && source.sampleRate == pcm.sampleRate &&
        source.channels >= 1 && source.frameCount() == totalSamples;
    const std::size_t numChannels =
        useMultiChannel ? static_cast<std::size_t>(source.channels) : 1u;

    std::vector<Biquad> stage1(numChannels, makeKWeightStage1(sr));
    std::vector<Biquad> stage2(numChannels, makeKWeightStage2(sr));
    auto bs1770ChannelWeight = [numChannels](std::size_t ch) noexcept -> double {
        constexpr double kSurroundWeight = 1.4125375446227544; // +1.5 dB surround weighting
        if (numChannels == 4) {
            return (ch == 3) ? kSurroundWeight : 1.0;
        }
        if (numChannels == 5) {
            return (ch == 3 || ch == 4) ? kSurroundWeight : 1.0;
        }
        if (numChannels >= 6) {
            if (ch == 3) {
                return 0.0; // LFE excluded in BS.1770-4
            }
            if (ch >= 4) {
                return kSurroundWeight;
            }
        }
        return 1.0;
    };

    const std::size_t subStepSamples =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(0.010 * sr)));
    const std::size_t subCount =
        std::max<std::size_t>(1, (totalSamples + subStepSamples - 1) / subStepSamples);
    std::vector<double> subPrefixK(subCount + 1, 0.0);
    double totalRawSq = 0.0;
    float samplePeak = 0.0f;

    for (int f = 0; f < frameCount; ++f) {
        const std::size_t s0 = static_cast<std::size_t>(f) * static_cast<std::size_t>(stepSamples);
        const std::size_t s1 = std::min(totalSamples, s0 + static_cast<std::size_t>(stepSamples));
        const std::size_t count = std::max<std::size_t>(1, s1 - s0);
        double sumRawSq = 0.0;
        if (useMultiChannel) {
            const double invCh = 1.0 / static_cast<double>(numChannels);
            for (std::size_t i = s0; i < s1; ++i) {
                double frameRawSq = 0.0;
                double frameKSq = 0.0;
                const std::size_t base = i * numChannels;
                for (std::size_t ch = 0; ch < numChannels; ++ch) {
                    float v = source.samples[base + ch];
                    if (!std::isfinite(v)) {
                        v = 0.0f;
                    }
                    samplePeak = std::max(samplePeak, std::abs(v));
                    const double x = static_cast<double>(v);
                    frameRawSq += x * x;
                    const double y = stage2[ch].process(stage1[ch].process(x));
                    frameKSq += bs1770ChannelWeight(ch) * (y * y);
                }
                sumRawSq += frameRawSq * invCh;
                subPrefixK[i / subStepSamples + 1] += frameKSq;
            }
        } else {
            for (std::size_t i = s0; i < s1; ++i) {
                float v = pcm.samples[i];
                if (!std::isfinite(v)) {
                    v = 0.0f;
                }
                samplePeak = std::max(samplePeak, std::abs(v));
                const double x = static_cast<double>(v);
                sumRawSq += x * x;
                const double y = stage2[0].process(stage1[0].process(x));
                subPrefixK[i / subStepSamples + 1] += y * y;
            }
        }
        totalRawSq += sumRawSq;
        const double rawRms = std::sqrt(sumRawSq / static_cast<double>(count));
        map.frames[static_cast<std::size_t>(f)].time =
            std::min(map.info.duration, (static_cast<double>(f) + 0.5) * actualStep);
        map.frames[static_cast<std::size_t>(f)].rms = static_cast<float>(rawRms);
    }

    for (std::size_t k = 1; k <= subCount; ++k) {
        subPrefixK[k] += subPrefixK[k - 1];
    }

    constexpr int kMomSubBlocks = 40;  // 400 ms (40 x 10 ms)
    constexpr int kStSubBlocks = 300;  // 3.0 s  (300 x 10 ms)
    constexpr int kHopSubBlocks = 10;  // 100 ms BS.1770-4 gating hop

    auto subWindowKMeanSq = [&](int endSub, int winSubs) noexcept {
        const int b = std::clamp(endSub, 1, static_cast<int>(subCount));
        int a = std::max(0, b - winSubs);
        std::size_t s0 = static_cast<std::size_t>(a) * subStepSamples;
        const std::size_t s1 =
            std::min(totalSamples, static_cast<std::size_t>(b) * subStepSamples);
        const std::size_t minSamples =
            static_cast<std::size_t>(winSubs) * subStepSamples;
        if (b == static_cast<int>(subCount) && (s1 - s0) < minSamples && a > 0) {
            --a;
            s0 = static_cast<std::size_t>(a) * subStepSamples;
        }
        const std::size_t samplesInWin = std::max<std::size_t>(1, s1 - s0);
        return (subPrefixK[static_cast<std::size_t>(b)] -
                subPrefixK[static_cast<std::size_t>(a)]) /
               static_cast<double>(samplesInWin);
    };

    for (int f = 0; f < frameCount; ++f) {
        const std::size_t s1 = std::min(
            totalSamples,
            static_cast<std::size_t>(f + 1) * static_cast<std::size_t>(stepSamples));
        const int endSub = std::clamp(
            static_cast<int>((s1 + subStepSamples / 2) / subStepSamples),
            1, static_cast<int>(subCount));
        map.frames[static_cast<std::size_t>(f)].momentaryLufs =
            powerToLufs(subWindowKMeanSq(endSub, kMomSubBlocks));
        map.frames[static_cast<std::size_t>(f)].shortTermLufs =
            powerToLufs(subWindowKMeanSq(endSub, kStSubBlocks));
    }

    float maxMomentary = -70.0f;
    float maxShortTerm = -70.0f;
    std::vector<double> gatingBlocks;
    gatingBlocks.reserve(subCount / kHopSubBlocks + 2);
    std::vector<double> shortTermAbsBlocks;
    shortTermAbsBlocks.reserve(subCount / kHopSubBlocks + 2);

    const int totalSubs = static_cast<int>(subCount);
    for (int bSub = kHopSubBlocks; ; bSub += kHopSubBlocks) {
        const int curSub = std::min(bSub, totalSubs);
        const double momMeanSq = subWindowKMeanSq(curSub, kMomSubBlocks);
        const double stMeanSq = subWindowKMeanSq(curSub, kStSubBlocks);
        const float momLufs = powerToLufs(momMeanSq);
        const float stLufs = powerToLufs(stMeanSq);

        if (curSub >= kMomSubBlocks || totalSubs < kMomSubBlocks) {
            maxMomentary = std::max(maxMomentary, momLufs);
            gatingBlocks.push_back(momMeanSq);
        }
        if (curSub >= kStSubBlocks || totalSubs < kStSubBlocks) {
            maxShortTerm = std::max(maxShortTerm, stLufs);
            if (stLufs > -70.0f) {
                shortTermAbsBlocks.push_back(stMeanSq);
            }
        }
        if (curSub >= totalSubs) {
            break;
        }
    }

    // BS.1770-4 two-stage gating: -70 LUFS absolute gate, then -10 LU relative gate.
    double absSum = 0.0;
    int absCount = 0;
    for (double z : gatingBlocks) {
        if (powerToLufs(z) > -70.0f) {
            absSum += z;
            ++absCount;
        }
    }
    if (absCount > 0) {
        const double absMean = absSum / static_cast<double>(absCount);
        const float relGateLufs = powerToLufs(absMean) - 10.0f;
        double relSum = 0.0;
        int relCount = 0;
        for (double z : gatingBlocks) {
            const float l = powerToLufs(z);
            if (l > -70.0f && l >= relGateLufs) {
                relSum += z;
                ++relCount;
            }
        }
        if (relCount > 0) {
            map.loudness.integratedLufs =
                powerToLufs(relSum / static_cast<double>(relCount));
            map.loudness.gatedValid = true;
        } else {
            map.loudness.integratedLufs = powerToLufs(absMean);
        }
    }

    // EBU Tech 3342 loudness range (LRA): -70 LUFS absolute gate followed by
    // -20 LU relative gate below the absolute-gated short-term power mean.
    if (!shortTermAbsBlocks.empty()) {
        double stAbsSum = 0.0;
        for (double z : shortTermAbsBlocks) {
            stAbsSum += z;
        }
        const double stAbsMean =
            stAbsSum / static_cast<double>(shortTermAbsBlocks.size());
        const float stRelGateLufs = powerToLufs(stAbsMean) - 20.0f;
        std::vector<float> shortTermGatedLufs;
        shortTermGatedLufs.reserve(shortTermAbsBlocks.size());
        for (double z : shortTermAbsBlocks) {
            const float l = powerToLufs(z);
            if (l >= stRelGateLufs) {
                shortTermGatedLufs.push_back(l);
            }
        }
        if (shortTermGatedLufs.size() >= 4) {
            const float p10 = dsp::percentile(shortTermGatedLufs, 0.10);
            const float p95 = dsp::percentile(shortTermGatedLufs, 0.95);
            map.loudness.loudnessRangeLu = std::max(0.0f, p95 - p10);
        }
    }

    map.loudness.momentaryMaxLufs = maxMomentary;
    map.loudness.shortTermMaxLufs = maxShortTerm;
    map.loudness.meanRmsDbfs = rmsToDbfs(
        std::sqrt(totalRawSq / static_cast<double>( std::max<std::size_t>(1, totalSamples))));
    map.loudness.samplePeakDbfs = rmsToDbfs(samplePeak);

    // 2. Per-frame spectral analysis (low/mid/high band energies + 16-band log novelty).
    const int fftSize = std::clamp(
        dsp::nextPowerOfTwo(static_cast<int>(std::lround(sr * 0.042))), 256, 4096);
    dsp::RealFft fft(fftSize);
    const std::vector<float> window = dsp::hannWindow(fftSize);
    std::vector<float> timeBuf(static_cast<std::size_t>(fftSize), 0.0f);
    std::vector<kiss_fft_cpx> freqBuf(static_cast<std::size_t>(fft.binCount()));
    const double binHz = sr / static_cast<double>(fftSize);

    constexpr int kNoveltyBands = 16;
    std::vector<float> logSpec(static_cast<std::size_t>(frameCount) * kNoveltyBands, 0.0f);
    std::vector<float> rmsValues(static_cast<std::size_t>(frameCount), 0.0f);

    for (int f = 0; f < frameCount; ++f) {
        const int centerSample = static_cast<int>(
            std::lround(map.frames[static_cast<std::size_t>(f)].time * sr));
        const int startSample = centerSample - fftSize / 2;
        for (int i = 0; i < fftSize; ++i) {
            const int idx = startSample + i;
            float s = 0.0f;
            if (idx >= 0 && static_cast<std::size_t>(idx) < totalSamples) {
                s = pcm.samples[static_cast<std::size_t>(idx)];
                if (!std::isfinite(s)) {
                    s = 0.0f;
                }
            }
            timeBuf[static_cast<std::size_t>(i)] = s * window[static_cast<std::size_t>(i)];
        }

        double lowE = 0.0;
        double midE = 0.0;
        double highE = 0.0;
        double vocalBandE = 0.0;
        std::array<double, kNoveltyBands> bandAccum{};

        if (fft.valid()) {
            fft.forward(timeBuf.data(), freqBuf.data());
            const int bins = fft.binCount();
            for (int b = 1; b < bins; ++b) {
                const double hz = b * binHz;
                const double p = static_cast<double>(freqBuf[static_cast<std::size_t>(b)].r) *
                                     freqBuf[static_cast<std::size_t>(b)].r +
                                 static_cast<double>(freqBuf[static_cast<std::size_t>(b)].i) *
                                     freqBuf[static_cast<std::size_t>(b)].i;
                if (hz < config_.lowBandMaxHz) {
                    lowE += p;
                } else if (hz <= config_.midBandMaxHz) {
                    midE += p;
                } else {
                    highE += p;
                }
                if (hz >= 280.0 && hz <= 3200.0) {
                    vocalBandE += p;
                }
                if (hz >= 40.0 && hz <= 12000.0) {
                    const double normLog =
                        std::log(hz / 40.0) / std::log(12000.0 / 40.0);
                    const int nb = std::clamp(
                        static_cast<int>(normLog * kNoveltyBands), 0, kNoveltyBands - 1);
                    bandAccum[static_cast<std::size_t>(nb)] += p;
                }
            }
        }

        const double totalE = lowE + midE + highE;
        MixFeatureFrame& frame = map.frames[static_cast<std::size_t>(f)];
        frame.lowEnergy = static_cast<float>(lowE);
        frame.midEnergy = static_cast<float>(midE);
        frame.highEnergy = static_cast<float>(highE);
        if (totalE > 1e-12) {
            frame.lowShare = static_cast<float>(std::clamp(lowE / totalE, 0.0, 1.0));
            frame.midShare = static_cast<float>(std::clamp(midE / totalE, 0.0, 1.0));
            frame.highShare = static_cast<float>(std::clamp(highE / totalE, 0.0, 1.0));
        }
        if (config_.vocalDetector == VocalDetectorKind::SpectralProxy && totalE > 1e-12) {
            const double vocalShare = std::clamp(vocalBandE / totalE, 0.0, 1.0);
            frame.vocalScore = static_cast<float>(
                std::clamp((vocalShare - 0.25) / 0.50, 0.0, 1.0));
            frame.vocalConfidence = 0.30f;
        }
        for (int nb = 0; nb < kNoveltyBands; ++nb) {
            logSpec[static_cast<std::size_t>(f) * kNoveltyBands + nb] =
                static_cast<float>(std::log10(1e-10 + bandAccum[static_cast<std::size_t>(nb)]));
        }
        rmsValues[static_cast<std::size_t>(f)] = frame.rms;
    }

    // 3. Normalised energy, onset density, bass dominance, and spectral novelty.
    const float refRms = std::max(1e-5f, dsp::percentile(rmsValues, 0.95));
    for (int f = 0; f < frameCount; ++f) {
        MixFeatureFrame& frame = map.frames[static_cast<std::size_t>(f)];
        frame.energy = std::clamp(frame.rms / refRms, 0.0f, 1.0f);
    }

    // Map BG Beat onsets and bass accents to the frame grid with a soft Gaussian kernel.
    for (const Onset& onset : analysis.onsets) {
        const int center = static_cast<int>(std::lround(onset.time / actualStep));
        for (int d = -3; d <= 3; ++d) {
            const int idx = center + d;
            if (idx >= 0 && idx < frameCount) {
                const float w = std::exp(-0.45f * static_cast<float>(d * d));
                map.frames[static_cast<std::size_t>(idx)].onsetDensity +=
                    onset.strength * w;
            }
        }
    }
    std::vector<float> onsetVals(static_cast<std::size_t>(frameCount), 0.0f);
    for (int f = 0; f < frameCount; ++f) {
        onsetVals[static_cast<std::size_t>(f)] =
            map.frames[static_cast<std::size_t>(f)].onsetDensity;
    }
    const float refOnset = std::max(0.25f, dsp::percentile(onsetVals, 0.95));
    for (int f = 0; f < frameCount; ++f) {
        map.frames[static_cast<std::size_t>(f)].onsetDensity =
            std::clamp(map.frames[static_cast<std::size_t>(f)].onsetDensity / refOnset,
                       0.0f, 1.0f);
    }

    std::vector<float> accentBoost(static_cast<std::size_t>(frameCount), 0.0f);
    for (const BassAccent& acc : analysis.bassAccents) {
        const int center = static_cast<int>(std::lround(acc.time / actualStep));
        for (int d = -2; d <= 2; ++d) {
            const int idx = center + d;
            if (idx >= 0 && idx < frameCount) {
                const float w = std::exp(-0.5f * static_cast<float>(d * d));
                accentBoost[static_cast<std::size_t>(idx)] = std::max(
                    accentBoost[static_cast<std::size_t>(idx)],
                    acc.confidence * std::min(1.0f, acc.strength) * w);
            }
        }
    }
    for (int f = 0; f < frameCount; ++f) {
        MixFeatureFrame& frame = map.frames[static_cast<std::size_t>(f)];
        const float boost =
            std::max(frame.lowShare, accentBoost[static_cast<std::size_t>(f)]);
        frame.bassDominance = std::clamp(
            frame.energy * (0.80f * frame.lowShare + 0.20f * boost),
            0.0f, 1.0f);
    }

    // Spectral novelty over a +/- 0.8 s local context window.
    const int novRadius = std::max(2, static_cast<int>(std::lround(0.80 / actualStep)));
    std::vector<float> rawNovelty(static_cast<std::size_t>(frameCount), 0.0f);
    for (int f = 1; f < frameCount - 1; ++f) {
        const int l0 = std::max(0, f - novRadius);
        const int r1 = std::min(frameCount, f + novRadius);
        if (l0 >= f || r1 <= f) {
            continue;
        }
        double diffSum = 0.0;
        for (int b = 0; b < kNoveltyBands; ++b) {
            double leftMean = 0.0;
            double rightMean = 0.0;
            for (int i = l0; i < f; ++i) {
                leftMean += logSpec[static_cast<std::size_t>(i) * kNoveltyBands + b];
            }
            for (int i = f; i < r1; ++i) {
                rightMean += logSpec[static_cast<std::size_t>(i) * kNoveltyBands + b];
            }
            leftMean /= static_cast<double>(f - l0);
            rightMean /= static_cast<double>(r1 - f);
            diffSum += std::abs(rightMean - leftMean);
        }
        rawNovelty[static_cast<std::size_t>(f)] =
            static_cast<float>(diffSum / kNoveltyBands);
    }
    const float refNov = std::max(0.15f, dsp::percentile(rawNovelty, 0.96));
    for (int f = 0; f < frameCount; ++f) {
        MixFeatureFrame& frame = map.frames[static_cast<std::size_t>(f)];
        frame.spectralNovelty =
            std::clamp(rawNovelty[static_cast<std::size_t>(f)] / refNov, 0.0f, 1.0f);

        const int l0 = std::max(0, f - novRadius);
        const int r1 = std::min(frameCount - 1, f + novRadius);
        const float energyContrast = std::abs(
            map.frames[static_cast<std::size_t>(r1)].energy -
            map.frames[static_cast<std::size_t>(l0)].energy);
        frame.boundaryStrength = std::clamp(
            0.55f * frame.spectralNovelty + 0.45f * energyContrast, 0.0f, 1.0f);
    }

    // Boost boundaryStrength near detected BG Beat section boundaries.
    for (const Section& sec : analysis.sections) {
        if (sec.start > 0.2 && sec.start < map.info.duration - 0.2) {
            const int idx = std::clamp(
                static_cast<int>(std::lround(sec.start / actualStep)), 0, frameCount - 1);
            map.frames[static_cast<std::size_t>(idx)].boundaryStrength = std::min(
                1.0f, map.frames[static_cast<std::size_t>(idx)].boundaryStrength +
                          0.35f * sec.confidence);
        }
    }

    computeAudibleBoundsAndActivity(map, analysis, config_);

    const InterleavedAudioView stereoAudio =
        (source.valid() && source.channels >= 2 && source.sampleRate == pcm.sampleRate)
            ? source
            : InterleavedAudioView{};

    if (customVocalDetector_ != nullptr) {
        const VocalDetectionOutput voc = customVocalDetector_->detect(
            pcm, stereoAudio, analysis, map.frames, actualStep);
        applyVocalDetection(map, voc);
    } else if (config_.vocalDetector == VocalDetectorKind::MusicVocalModel) {
        MusicVocalDetector detector;
        const VocalDetectionOutput voc =
            detector.detect(pcm, stereoAudio, analysis, map.frames, actualStep);
        applyVocalDetection(map, voc);
    } else if (config_.vocalDetector == VocalDetectorKind::SpectralProxy) {
        map.vocal.available = true;
        map.vocal.detectorKind = VocalDetectorKind::SpectralProxy;
        map.vocal.meanConfidence = 0.30f;
    }

    buildBarsPhrasesAndCues(map, analysis, config_);

    if (config_.harmonicAnalyzeVersion > 0) {
        HarmonicAnalyzer harmonic;
        harmonic.analyze(pcm, map);
    }

    return map;
}

MixFeatureMap MixFeatureExtractor::extractFromTrack(
    const PcmTrack& track, const AnalysisResult& analysis) const {
    if (!track.valid()) {
        return buildFromAnalysisOnly(analysis);
    }
    const InterleavedAudioView view = InterleavedAudioView::fromTrack(track);
    const PcmBuffer pcm = downmixViewToMono(view, analysis);
    return extractFromView(view, pcm, analysis);
}

MixFeatureMap MixFeatureExtractor::buildFromAnalysisOnly(
    const AnalysisResult& analysis) const {
    MixFeatureMap map;
    if (analysis.empty()) {
        return map;
    }

    map.info.schemaVersion = kMixFeatureSchemaVersion;
    map.info.extractorVersion = kMixFeatureExtractorVersion;
    map.info.analyzerVersion = analysis.info.analyzerVersion > 0
                                   ? analysis.info.analyzerVersion
                                   : kAnalyzerVersion;
    map.info.contentHash = analysis.info.contentHash;
    map.info.analysisConfigHash = analysis.info.configHash;
    map.info.featureConfigHash = config_.fingerprint();
    map.info.vocalDetectorKind = VocalDetectorKind::None;
    map.info.duration = analysis.info.duration;
    map.info.sampleRate = analysis.info.sampleRate > 0 ? analysis.info.sampleRate : 48000;
    map.info.sourceChannels = analysis.info.sourceChannels > 0 ? analysis.info.sourceChannels : 2;
    map.info.frameStepSeconds = config_.frameStepSeconds;
    map.info.fromPcm = false;

    map.globalBpm = analysis.bpm;
    map.bpmConfidence = analysis.bpmConfidence;
    map.beatPhaseConfidence = analysis.beatPhaseConfidence;
    map.downbeatConfidence = analysis.downbeatConfidence;
    map.beatsPerBar = analysis.beatsPerBar > 0 ? analysis.beatsPerBar : 4;
    map.meterConfidence = analysis.meterConfidence;
    map.tempoStable = analysis.tempoStable;
    map.tempoSegments = analysis.tempoSegments;
    map.tempoTransitions = analysis.tempoTransitions;
    map.sections = analysis.sections;

    const double step = config_.frameStepSeconds;
    const int frameCount = std::max(1, static_cast<int>(std::ceil(analysis.info.duration / step)));
    map.frames.resize(static_cast<std::size_t>(frameCount));

    std::vector<float> rmsVals(static_cast<std::size_t>(frameCount), 0.0f);
    double sumSq = 0.0;
    float peak = 0.0f;

    for (int f = 0; f < frameCount; ++f) {
        MixFeatureFrame& frame = map.frames[static_cast<std::size_t>(f)];
        frame.time = std::min(analysis.info.duration, (static_cast<double>(f) + 0.5) * step);

        // Estimate frame level from waveform overview or section energy when
        // raw PCM was not supplied.
        float approxRms = 0.25f;
        if (!analysis.waveform.empty() && analysis.waveformPeaksPerSecond > 0.0f) {
            const int wIdx = std::clamp(
                static_cast<int>(frame.time * analysis.waveformPeaksPerSecond),
                0, static_cast<int>(analysis.waveform.size()) - 1);
            const WaveformPeak& wp = analysis.waveform[static_cast<std::size_t>(wIdx)];
            const float amp = std::max(std::abs(wp.minValue), std::abs(wp.maxValue));
            peak = std::max(peak, amp);
            approxRms = amp * 0.7071f;
        } else {
            for (const Section& sec : analysis.sections) {
                if (frame.time >= sec.start && frame.time <= sec.end) {
                    approxRms = std::clamp(sec.energy * 0.4f, 0.02f, 0.5f);
                    break;
                }
            }
            if (insideSilenceRegion(analysis.silences, frame.time)) {
                approxRms = 0.0f;
            }
            peak = std::max(peak, approxRms * 1.414f);
        }

        frame.rms = approxRms;
        rmsVals[static_cast<std::size_t>(f)] = approxRms;
        sumSq += static_cast<double>(approxRms) * approxRms;
        frame.momentaryLufs = powerToLufs(static_cast<double>(approxRms) * approxRms);
        frame.shortTermLufs = frame.momentaryLufs;

        // Approximate spectral shares from BG Beat's display spectrogram if present.
        if (analysis.spectrogram.valid() && analysis.spectrogram.framesPerSecond > 0.0) {
            const int sIdx = std::clamp(
                static_cast<int>(frame.time * analysis.spectrogram.framesPerSecond),
                0, static_cast<int>(analysis.spectrogram.frameCount) - 1);
            const int bands = analysis.spectrogram.bands;
            const int lowEnd = std::max(1, bands / 6);
            const int midEnd = std::max(lowEnd + 1, (bands * 3) / 4);
            double l = 0.0;
            double m = 0.0;
            double h = 0.0;
            for (int b = 0; b < bands; ++b) {
                const double v = analysis.spectrogram.magnitudes[
                    static_cast<std::size_t>(sIdx) * bands + b] / 255.0;
                if (b < lowEnd) {
                    l += v;
                } else if (b < midEnd) {
                    m += v;
                } else {
                    h += v;
                }
            }
            const double tot = l + m + h;
            if (tot > 1e-6) {
                frame.lowShare = static_cast<float>(l / tot);
                frame.midShare = static_cast<float>(m / tot);
                frame.highShare = static_cast<float>(h / tot);
            }
        } else {
            frame.lowShare = 0.30f;
            frame.midShare = 0.50f;
            frame.highShare = 0.20f;
        }
    }

    const float refRms = std::max(1e-5f, dsp::percentile(rmsVals, 0.95));
    for (int f = 0; f < frameCount; ++f) {
        MixFeatureFrame& frame = map.frames[static_cast<std::size_t>(f)];
        frame.energy = std::clamp(frame.rms / refRms, 0.0f, 1.0f);
        frame.bassDominance = std::clamp(frame.energy * frame.lowShare, 0.0f, 1.0f);
    }

    const double meanSq = sumSq / static_cast<double>(frameCount);
    map.loudness.integratedLufs = powerToLufs(meanSq);
    map.loudness.momentaryMaxLufs = powerToLufs(static_cast<double>(refRms) * refRms);
    map.loudness.shortTermMaxLufs = map.loudness.momentaryMaxLufs;
    map.loudness.meanRmsDbfs = rmsToDbfs(std::sqrt(meanSq));
    map.loudness.samplePeakDbfs = rmsToDbfs(peak);
    map.loudness.gatedValid = meanSq > 1e-10;

    computeAudibleBoundsAndActivity(map, analysis, config_);
    buildBarsPhrasesAndCues(map, analysis, config_);
    return map;
}

} // namespace bgmedia::mix
