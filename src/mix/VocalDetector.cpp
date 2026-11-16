#include "bgmedia/mix/VocalDetector.hpp"

#include "analysis/Dsp.hpp"
#include "bgmedia/mix/Crossfade.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

namespace bgmedia::mix {
namespace {

float evalGainCurve(const std::vector<GainPoint>& curve, double time,
                    double duration, bool incoming) noexcept {
    if (curve.empty()) {
        const double t = duration > 1e-6 ? std::clamp(time / duration, 0.0, 1.0) : 0.5;
        return incoming ? equalPowerIn(t) : equalPowerOut(t);
    }
    if (time <= curve.front().time) {
        return curve.front().gain;
    }
    if (time >= curve.back().time) {
        return curve.back().gain;
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (time <= curve[i].time) {
            const double span = std::max(1e-9, curve[i].time - curve[i - 1].time);
            const double u = std::clamp((time - curve[i - 1].time) / span, 0.0, 1.0);
            return static_cast<float>(curve[i - 1].gain +
                                      (curve[i].gain - curve[i - 1].gain) * u);
        }
    }
    return curve.back().gain;
}

float evalMidEqCurve(const std::vector<EqGainPoint>& curve, double time) noexcept {
    if (curve.empty()) {
        return 1.0f;
    }
    if (time <= curve.front().time) {
        return curve.front().mid;
    }
    if (time >= curve.back().time) {
        return curve.back().mid;
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (time <= curve[i].time) {
            const double span = std::max(1e-9, curve[i].time - curve[i - 1].time);
            const double u = std::clamp((time - curve[i - 1].time) / span, 0.0, 1.0);
            return static_cast<float>(curve[i - 1].mid +
                                      (curve[i].mid - curve[i - 1].mid) * u);
        }
    }
    return curve.back().mid;
}

void buildVocalRegions(const std::vector<MixFeatureFrame>& frames,
                       double frameStepSeconds,
                       const std::vector<float>& scores,
                       const std::vector<float>& confidences,
                       std::vector<VocalRegion>& regionsOut,
                       float* activeRatioOut) {
    regionsOut.clear();
    const std::size_t n = std::min(frames.size(), scores.size());
    if (n == 0) {
        if (activeRatioOut) *activeRatioOut = 0.0f;
        return;
    }

    constexpr float kEnterScore = 0.45f;
    constexpr float kExitScore = 0.32f;
    const int minSpanFrames =
        std::max(2, static_cast<int>(std::lround(0.35 / std::max(0.02, frameStepSeconds))));
    const int mergeGapFrames =
        std::max(1, static_cast<int>(std::lround(0.25 / std::max(0.02, frameStepSeconds))));

    std::vector<std::pair<int, int>> rawSpans;
    bool inVocal = false;
    int spanStart = 0;
    int activeFrames = 0;

    for (std::size_t i = 0; i < n; ++i) {
        const bool frameActive = frames[i].active;
        const float s = frameActive ? scores[i] : 0.0f;
        if (!inVocal && s >= kEnterScore) {
            inVocal = true;
            spanStart = static_cast<int>(i);
        } else if (inVocal && s < kExitScore) {
            inVocal = false;
            rawSpans.emplace_back(spanStart, static_cast<int>(i));
        }
        if (inVocal) {
            ++activeFrames;
        }
    }
    if (inVocal) {
        rawSpans.emplace_back(spanStart, static_cast<int>(n));
    }

    std::vector<std::pair<int, int>> merged;
    for (const auto& sp : rawSpans) {
        if (!merged.empty() && sp.first - merged.back().second <= mergeGapFrames) {
            merged.back().second = sp.second;
        } else {
            merged.push_back(sp);
        }
    }

    int keptFrames = 0;
    for (const auto& sp : merged) {
        const int count = sp.second - sp.first;
        if (count < minSpanFrames) {
            continue;
        }
        keptFrames += count;
        double sumScore = 0.0;
        double sumConf = 0.0;
        for (int i = sp.first; i < sp.second; ++i) {
            sumScore += scores[static_cast<std::size_t>(i)];
            sumConf += (static_cast<std::size_t>(i) < confidences.size())
                           ? confidences[static_cast<std::size_t>(i)]
                           : 0.5;
        }
        VocalRegion reg;
        reg.start = static_cast<double>(sp.first) * frameStepSeconds;
        reg.end = std::min(frames.back().time + 0.5 * frameStepSeconds,
                           static_cast<double>(sp.second) * frameStepSeconds);
        reg.score = static_cast<float>(sumScore / count);
        reg.confidence = static_cast<float>(sumConf / count);
        reg.leadLikely = reg.score >= 0.56f && reg.confidence >= 0.50f;
        regionsOut.push_back(reg);
    }

    if (activeRatioOut) {
        int totalAudible = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (frames[i].active) {
                ++totalAudible;
            }
        }
        *activeRatioOut = totalAudible > 0
                              ? std::clamp(static_cast<float>(keptFrames) /
                                               static_cast<float>(totalAudible),
                                           0.0f, 1.0f)
                              : 0.0f;
    }
}

bool timeInsideVocalRegion(const VocalTrackSummary& vocal, double time,
                           double edgeMarginSeconds) noexcept {
    for (const VocalRegion& reg : vocal.regions) {
        if (reg.leadLikely &&
            time >= reg.start + edgeMarginSeconds &&
            time <= reg.end - edgeMarginSeconds) {
            return true;
        }
    }
    return false;
}

} // namespace

VocalDetectionOutput MusicVocalDetector::detect(
    const PcmBuffer& monoPcm,
    InterleavedAudioView stereoAudio,
    const AnalysisResult& /*analysis*/,
    const std::vector<MixFeatureFrame>& frames,
    double frameStepSeconds) const {
    VocalDetectionOutput out;
    out.detectorKind = VocalDetectorKind::MusicVocalModel;
    out.modelVersion = kModelVersion;
    out.modelChecksum = kModelChecksum;

    const std::size_t n = frames.size();
    if (n == 0 || !monoPcm.valid()) {
        return out;
    }

    out.available = true;
    out.frameScores.assign(n, 0.0f);
    out.frameConfidences.assign(n, 0.0f);

    const double sr = static_cast<double>(monoPcm.sampleRate);
    const int fftSize = std::clamp(
        dsp::nextPowerOfTwo(static_cast<int>(std::lround(sr * 0.046))), 512, 4096);
    dsp::RealFft fft(fftSize);
    if (!fft.valid()) {
        out.available = false;
        return out;
    }

    const std::vector<float> window = dsp::hannWindow(fftSize);
    std::vector<float> timeBuf(static_cast<std::size_t>(fftSize), 0.0f);
    std::vector<kiss_fft_cpx> freqBuf(static_cast<std::size_t>(fft.binCount()));
    const int bins = fft.binCount();
    const double binHz = sr / static_cast<double>(fftSize);
    std::vector<float> magSq(static_cast<std::size_t>(bins), 0.0f);

    const bool hasStereo = stereoAudio.valid() && stereoAudio.channels >= 2 &&
                           stereoAudio.sampleRate == monoPcm.sampleRate &&
                           stereoAudio.frameCount() >= monoPcm.samples.size();

    std::vector<float> rawScores(n, 0.0f);
    std::vector<float> rawConfs(n, 0.0f);
    std::vector<float> f0Track(n, 0.0f);

    for (std::size_t f = 0; f < n; ++f) {
        if (checkpoint_ && (f % 16 == 0)) checkpoint_();
        const MixFeatureFrame& frame = frames[f];
        if (!frame.active || frame.rms < 1e-5f) {
            rawScores[f] = 0.0f;
            rawConfs[f] = 0.75f; // high confidence that a silent frame has no vocal
            continue;
        }

        const int centerSample = static_cast<int>(std::lround(frame.time * sr));
        const int startSample = centerSample - fftSize / 2;

        double midSq = 0.0;
        double sideSq = 0.0;
        for (int i = 0; i < fftSize; ++i) {
            const int idx = startSample + i;
            float s = 0.0f;
            if (idx >= 0 && static_cast<std::size_t>(idx) < monoPcm.samples.size()) {
                s = monoPcm.samples[static_cast<std::size_t>(idx)];
                if (!std::isfinite(s)) s = 0.0f;
                if (hasStereo) {
                    float l = stereoAudio.samples[static_cast<std::size_t>(idx) *
                                                  stereoAudio.channels];
                    float r = stereoAudio.samples[static_cast<std::size_t>(idx) *
                                                      stereoAudio.channels +
                                                  1];
                    if (!std::isfinite(l)) l = 0.0f;
                    if (!std::isfinite(r)) r = 0.0f;
                    const double m = 0.5 * (static_cast<double>(l) + r);
                    const double side = 0.5 * (static_cast<double>(l) - r);
                    midSq += m * m;
                    sideSq += side * side;
                }
            }
            timeBuf[static_cast<std::size_t>(i)] = s * window[static_cast<std::size_t>(i)];
        }

        fft.forward(timeBuf.data(), freqBuf.data());
        double totalPower = 0.0;
        double f1Power = 0.0; // 320..950 Hz (first vocal formant region)
        double f2Power = 0.0; // 950..2600 Hz (second vocal formant region)
        double f3Power = 0.0; // 2200..3600 Hz (third vocal formant / singer's formant)
        double subPower = 0.0;

        for (int b = 1; b < bins; ++b) {
            const double re = freqBuf[static_cast<std::size_t>(b)].r;
            const double im = freqBuf[static_cast<std::size_t>(b)].i;
            const float p = static_cast<float>(re * re + im * im);
            magSq[static_cast<std::size_t>(b)] = p;
            totalPower += p;
            const double hz = b * binHz;
            if (hz < 130.0) {
                subPower += p;
            }
            if (hz >= 320.0 && hz <= 950.0) {
                f1Power += p;
            }
            if (hz > 950.0 && hz <= 2600.0) {
                f2Power += p;
            }
            if (hz >= 2200.0 && hz <= 3600.0) {
                f3Power += p;
            }
        }

        if (totalPower <= 1e-12) {
            rawScores[f] = 0.0f;
            rawConfs[f] = 0.70f;
            continue;
        }

        // 1. Harmonic comb search over human vocal fundamental range (105..720 Hz).
        // A true voice has a clear fundamental F0 AND at least 2 upper harmonics
        // (2*F0..6*F0) carrying significant formant energy, unlike a pure sine wave
        // (which only excites a single frequency bin and must not match sub-harmonic F0s).
        double bestCombScore = 0.0;
        double bestUpperHarmonicRatio = 0.0;
        double bestF0 = 0.0;

        for (double f0 = 105.0; f0 <= 720.0; f0 *= 1.035) {
            double harmonicSum = 0.0;
            double upperHarmonicSum = 0.0;
            double troughSum = 0.0;
            bool fundamentalPresent = false;
            int harmonicsFound = 0;

            for (int k = 1; k <= 6; ++k) {
                const double hk = k * f0;
                if (hk >= sr * 0.45) {
                    break;
                }
                const int bCenter = static_cast<int>(std::lround(hk / binHz));
                double peakP = 0.0;
                for (int db = -1; db <= 1; ++db) {
                    const int bi = std::clamp(bCenter + db, 1, bins - 1);
                    peakP = std::max(peakP, static_cast<double>(magSq[static_cast<std::size_t>(bi)]));
                }
                const double weight = (k == 1) ? 0.85 : (1.0 / std::sqrt(static_cast<double>(k)));
                harmonicSum += weight * peakP;
                if (k == 1) {
                    if (peakP > totalPower * 0.008) {
                        fundamentalPresent = true;
                    }
                } else {
                    upperHarmonicSum += peakP;
                    if (peakP > totalPower * 0.008) {
                        ++harmonicsFound;
                    }
                }

                const double tk = (k + 0.5) * f0;
                const int tBin = std::clamp(static_cast<int>(std::lround(tk / binHz)), 1, bins - 1);
                troughSum += weight * static_cast<double>(magSq[static_cast<std::size_t>(tBin)]);
            }

            // Require genuine multi-harmonic structure (fundamental + >= 2 upper
            // harmonics). Rejects pure single-frequency sine waves at any pitch,
            // including sub-harmonic candidate F0s where only a single overtone bin hits.
            if (!fundamentalPresent || harmonicsFound < 2) {
                continue;
            }
            const double upperRatio =
                harmonicSum > 1e-12 ? (upperHarmonicSum / (harmonicSum + 1e-12)) : 0.0;
            const double hnr =
                (harmonicSum - troughSum) / (harmonicSum + 2.5 * troughSum + 1e-12);
            const double score = std::max(0.0, hnr) *
                                 std::clamp(upperRatio * 2.5, 0.08, 1.0);
            if (score > bestCombScore) {
                bestCombScore = score;
                bestUpperHarmonicRatio = upperRatio;
                bestF0 = f0;
            }
        }
        f0Track[f] = static_cast<float>(bestF0);

        // 2. Multi-formant balance across F1 + F2 + F3 (gated by multi-harmonic evidence).
        const double formantShare = (f1Power + f2Power + 0.5 * f3Power) / totalPower;
        const bool hasMultipleFormants =
            (f1Power > totalPower * 0.03 && f2Power > totalPower * 0.015) ||
            (bestUpperHarmonicRatio >= 0.18 && f1Power > totalPower * 0.02 &&
             f2Power > totalPower * 0.01);
        const double harmonicGate = bestCombScore >= 0.12 ? 1.0 : 0.12;
        const double formantScore =
            std::clamp((formantShare - 0.12) / 0.52, 0.0, 1.0) *
            (hasMultipleFormants ? 1.0 : 0.25) * harmonicGate;

        // 3. Percussive / sub-bass suppression: strong transient onset spikes or
        // overwhelming sub-bass reduce vocal probability unless harmonic comb is strong.
        const double subRatio = subPower / totalPower;
        const double transientPenalty =
            std::clamp(1.0 - 0.55 * static_cast<double>(frame.onsetDensity), 0.35, 1.0);
        const double bassPenalty =
            std::clamp(1.0 - 0.65 * subRatio, 0.25, 1.0);

        // 4. Stereo center focus (lead vocals sit near center in stereo mixes).
        double centerFocus = 1.0;
        if (hasStereo && (midSq + sideSq) > 1e-10) {
            const double midRatio = midSq / (midSq + sideSq);
            centerFocus = std::clamp(0.45 + 0.65 * midRatio, 0.45, 1.05);
        }

        const double combNorm = std::clamp(bestCombScore * 1.45, 0.0, 1.0);
        const double combined =
            (0.58 * combNorm + 0.42 * formantScore) *
            transientPenalty * bassPenalty * centerFocus;

        rawScores[f] = static_cast<float>(std::clamp(combined, 0.0, 1.0));
        // Confidence is highest when harmonic comb and formant evidence agree.
        const double agreement = 1.0 - std::abs(combNorm - formantScore);
        rawConfs[f] = static_cast<float>(
            std::clamp(0.45 + 0.45 * agreement * std::max(combNorm, 0.35), 0.35, 0.92));
    }

    // 5. Temporal smoothing & syllabic continuity (300 ms median/weighted window).
    double confSum = 0.0;
    int confCount = 0;
    for (std::size_t f = 0; f < n; ++f) {
        if (!frames[f].active) {
            out.frameScores[f] = 0.0f;
            out.frameConfidences[f] = rawConfs[f];
            continue;
        }
        double weightedScore = 0.0;
        double weightTotal = 0.0;
        for (int d = -2; d <= 2; ++d) {
            const int idx = static_cast<int>(f) + d;
            if (idx >= 0 && static_cast<std::size_t>(idx) < n &&
                frames[static_cast<std::size_t>(idx)].active) {
                const double w = (d == 0) ? 0.40 : (std::abs(d) == 1 ? 0.22 : 0.08);
                weightedScore += w * rawScores[static_cast<std::size_t>(idx)];
                weightTotal += w;
            }
        }
        float smoothed = weightTotal > 0.0
                             ? static_cast<float>(weightedScore / weightTotal)
                             : rawScores[f];

        // Pitch-track continuity / vibrato bonus over +/- 1 frame.
        if (f > 0 && f + 1 < n && f0Track[f] > 0.0f && f0Track[f - 1] > 0.0f) {
            const double semitoneStep =
                12.0 * std::abs(std::log2(f0Track[f] / f0Track[f - 1]));
            if (semitoneStep <= 2.2) {
                smoothed = std::min(1.0f, smoothed * 1.10f);
            }
        }

        out.frameScores[f] = std::clamp(smoothed, 0.0f, 1.0f);
        out.frameConfidences[f] = rawConfs[f];
        confSum += out.frameConfidences[f];
        ++confCount;
    }

    out.meanConfidence =
        confCount > 0 ? static_cast<float>(confSum / confCount) : 0.65f;
    buildVocalRegions(frames, frameStepSeconds, out.frameScores,
                      out.frameConfidences, out.regions, &out.activeRatio);
    return out;
}

ExternalStemVocalDetector::ExternalStemVocalDetector(
    const PcmBuffer& vocalStem, std::uint64_t stemChecksum)
    : vocalStem_(vocalStem),
      stemChecksum_(stemChecksum != 0 ? stemChecksum : vocalStem.contentHash) {}

VocalDetectionOutput ExternalStemVocalDetector::detect(
    const PcmBuffer& monoPcm,
    InterleavedAudioView /*stereoAudio*/,
    const AnalysisResult& /*analysis*/,
    const std::vector<MixFeatureFrame>& frames,
    double frameStepSeconds) const {
    VocalDetectionOutput out;
    out.detectorKind = VocalDetectorKind::ExternalStem;
    out.modelVersion = 1;
    out.modelChecksum = stemChecksum_;

    const std::size_t n = frames.size();
    if (n == 0 || !vocalStem_.valid()) {
        return out;
    }

    out.available = true;
    out.frameScores.assign(n, 0.0f);
    out.frameConfidences.assign(n, 0.95f);

    const double sr = static_cast<double>(vocalStem_.sampleRate);
    const int winSamples =
        std::max(64, static_cast<int>(std::lround(frameStepSeconds * sr)));

    std::vector<float> stemRms(n, 0.0f);
    for (std::size_t f = 0; f < n; ++f) {
        const int center = static_cast<int>(std::lround(frames[f].time * sr));
        const int s0 = std::max(0, center - winSamples / 2);
        const int s1 = std::min(static_cast<int>(vocalStem_.samples.size()),
                                center + winSamples / 2);
        if (s1 <= s0) continue;
        double sumSq = 0.0;
        for (int i = s0; i < s1; ++i) {
            const double v = vocalStem_.samples[static_cast<std::size_t>(i)];
            sumSq += v * v;
        }
        stemRms[f] = static_cast<float>(std::sqrt(sumSq / (s1 - s0)));
    }

    std::vector<float> nonZeroStem;
    nonZeroStem.reserve(n);
    for (float v : stemRms) {
        if (v > 1e-4f) {
            nonZeroStem.push_back(v);
        }
    }
    const float refStemRms = nonZeroStem.empty()
                                 ? 0.01f
                                 : std::max(0.01f, dsp::percentile(nonZeroStem, 0.90));
    for (std::size_t f = 0; f < n; ++f) {
        const float relToMix =
            frames[f].rms > 1e-5f ? (stemRms[f] / (frames[f].rms + 1e-5f)) : 0.0f;
        const float normRms = stemRms[f] / refStemRms;
        out.frameScores[f] = std::clamp(0.65f * normRms + 0.35f * relToMix, 0.0f, 1.0f);
        if (!monoPcm.valid()) {
            out.frameConfidences[f] = 0.90f;
        }
    }

    out.meanConfidence = 0.95f;
    buildVocalRegions(frames, frameStepSeconds, out.frameScores,
                      out.frameConfidences, out.regions, &out.activeRatio);
    return out;
}

void applyVocalDetection(MixFeatureMap& map, const VocalDetectionOutput& output) {
    map.vocal.available = output.available;
    map.vocal.detectorKind = output.detectorKind;
    map.vocal.modelVersion = output.modelVersion;
    map.vocal.modelChecksum = output.modelChecksum;
    map.vocal.meanConfidence = output.meanConfidence;
    map.vocal.activeRatio = output.activeRatio;
    map.vocal.regions = output.regions;

    map.info.vocalDetectorKind = output.detectorKind;
    map.info.vocalModelVersion = output.modelVersion;
    map.info.vocalModelChecksum = output.modelChecksum;

    const std::size_t n = std::min(map.frames.size(), output.frameScores.size());
    for (std::size_t i = 0; i < n; ++i) {
        map.frames[i].vocalScore = output.frameScores[i];
        map.frames[i].vocalConfidence =
            i < output.frameConfidences.size() ? output.frameConfidences[i]
                                               : output.meanConfidence;
    }

    for (BarFeatureSummary& bar : map.bars) {
        const WindowFeatureSummary win = map.summarizeWindow(bar.start, bar.end);
        bar.vocalScore = win.vocalScore;
        bar.vocalConfidence = win.vocalConfidence;
    }
    for (CuePointCandidate& cue : map.mixOutCandidates) {
        const WindowFeatureSummary before =
            map.summarizeWindow(std::max(0.0, cue.time - 4.0), cue.time);
        const WindowFeatureSummary after =
            map.summarizeWindow(cue.time, std::min(map.info.duration, cue.time + 4.0));
        cue.vocalBefore = before.vocalScore;
        cue.vocalAfter = after.vocalScore;
        cue.vocalConfidence = std::max(before.vocalConfidence, after.vocalConfidence);
    }
    for (CuePointCandidate& cue : map.mixInCandidates) {
        const WindowFeatureSummary before =
            map.summarizeWindow(std::max(0.0, cue.time - 2.0), cue.time);
        const WindowFeatureSummary after =
            map.summarizeWindow(cue.time, std::min(map.info.duration, cue.time + 4.0));
        cue.vocalBefore = before.vocalScore;
        cue.vocalAfter = after.vocalScore;
        cue.vocalConfidence = std::max(before.vocalConfidence, after.vocalConfidence);
    }
}

VocalOverlapAssessment evaluateVocalOverlap(
    const MixFeatureMap& featuresA,
    double startA,
    double rateA,
    const MixFeatureMap& featuresB,
    double startB,
    double rateB,
    double duration,
    const std::vector<GainPoint>& gainCurveOut,
    const std::vector<GainPoint>& gainCurveIn,
    const std::vector<EqGainPoint>& eqCurveOut,
    const std::vector<EqGainPoint>& eqCurveIn) {
    VocalOverlapAssessment result;
    if (!featuresA.valid() || !featuresB.valid() || duration <= 0.05) {
        result.summary = "vocal assessment unavailable (short or empty window)";
        return result;
    }
    if (!featuresA.vocal.available || !featuresB.vocal.available ||
        featuresA.vocal.detectorKind == VocalDetectorKind::None ||
        featuresB.vocal.detectorKind == VocalDetectorKind::None) {
        result.available = false;
        result.confidence = 0.0f;
        result.summary = "vocal detection disabled or unavailable";
        return result;
    }

    result.available = true;
    result.confidence = std::min(featuresA.vocal.meanConfidence,
                                 featuresB.vocal.meanConfidence);

    const double step = 0.10;
    const int steps = std::max(2, static_cast<int>(std::ceil(duration / step)));
    const double dt = duration / static_cast<double>(steps);
    const double safeRateA = std::max(0.25, rateA);
    const double safeRateB = std::max(0.25, rateB);

    double clashAccum = 0.0;
    double clashMax = 0.0;
    double clashSeconds = 0.0;
    double leadClashSeconds = 0.0;
    double vocalBedAccum = 0.0;
    double outVocalSum = 0.0;
    double inVocalSum = 0.0;

    double lastVocalTimeA = -1.0;
    double firstVocalTimeB = 1e9;

    for (int i = 0; i <= steps; ++i) {
        const double tOut = i * dt;
        const double posA = startA + tOut * safeRateA;
        const double posB = startB + tOut * safeRateB;

        const MixFeatureFrame fA = featuresA.sampleAt(posA);
        const MixFeatureFrame fB = featuresB.sampleAt(posB);

        // Vocal EQ carve reduces mid band, but human singing formants and
        // consonants remain audible across low/high bands (-6 dB mid is not silence).
        const float midEqA = std::max(0.65f, evalMidEqCurve(eqCurveOut, tOut));
        const float midEqB = std::max(0.65f, evalMidEqCurve(eqCurveIn, tOut));
        const float gOut = evalGainCurve(gainCurveOut, tOut, duration, false) * midEqA;
        const float gIn = evalGainCurve(gainCurveIn, tOut, duration, true) * midEqB;

        // Audibility-weighted vocal activity on each deck.
        const float audOut = std::clamp(gOut * 1.25f, 0.0f, 1.0f);
        const float audIn = std::clamp(gIn * 1.25f, 0.0f, 1.0f);
        const float effVocalA = fA.vocalScore * audOut;
        const float effVocalB = fB.vocalScore * audIn;

        outVocalSum += fA.vocalScore;
        inVocalSum += fB.vocalScore;

        if (effVocalA >= 0.25f) {
            lastVocalTimeA = std::max(lastVocalTimeA, tOut);
        }
        if (effVocalB >= 0.25f) {
            firstVocalTimeB = std::min(firstVocalTimeB, tOut);
        }

        const double jointClash = static_cast<double>(effVocalA) * effVocalB;
        clashAccum += jointClash;
        clashMax = std::max(clashMax, jointClash);

        // Track both general vocal overlap and severe lead vocal collisions
        if (effVocalA >= 0.22f && effVocalB >= 0.22f) {
            clashSeconds += dt;
        }

        const bool isLeadA = fA.vocalScore >= 0.50f || timeInsideVocalRegion(featuresA.vocal, posA, 0.15);
        const bool isLeadB = fB.vocalScore >= 0.50f || timeInsideVocalRegion(featuresB.vocal, posB, 0.15);
        if (isLeadA && isLeadB && audOut >= 0.18f && audIn >= 0.18f) {
            leadClashSeconds += dt;
        }

        // Vocal-over-instrumental: one deck has clear vocal while the other
        // deck provides an audible active instrumental bed (low vocal score).
        const double instBedB =
            (fB.active ? 1.0 : 0.0) * (1.0 - fB.vocalScore) * static_cast<double>(audIn);
        const double instBedA =
            (fA.active ? 1.0 : 0.0) * (1.0 - fA.vocalScore) * static_cast<double>(audOut);
        const double aOverInstB = static_cast<double>(effVocalA) * instBedB;
        const double bOverInstA = static_cast<double>(effVocalB) * instBedA;
        vocalBedAccum += std::max(aOverInstB, bOverInstA);
    }

    const double count = static_cast<double>(steps + 1);
    const double meanClash = clashAccum / count;
    result.outgoingVocalMean = static_cast<float>(outVocalSum / count);
    result.incomingVocalMean = static_cast<float>(inVocalSum / count);
    result.clashDurationSeconds = clashSeconds;
    result.leadVocalClashDurationSeconds = leadClashSeconds;
    result.maxSimultaneousVocalScore = static_cast<float>(clashMax);
    result.severeLeadVocalClash = (leadClashSeconds >= 0.75 || clashSeconds >= 1.2 ||
                                   (clashMax >= 0.20 && clashSeconds >= 0.5));
    result.lowConfidenceConservative = (result.confidence < 0.45f);

    // Combine mean overlap clash, peak clash, and sustained clash duration.
    const double durationFactor = std::clamp(clashSeconds / 2.0, 0.0, 1.0);
    const double leadFactor = std::clamp(leadClashSeconds / 1.5, 0.0, 1.0);
    const double meanFactor = std::clamp(meanClash * 2.8, 0.0, 1.0);
    double rawClash = 0.30 * meanFactor + 0.30 * clashMax + 0.40 * std::max(durationFactor, leadFactor);
    if (result.severeLeadVocalClash) {
        rawClash = std::max(rawClash, 0.55);
    }
    rawClash = std::clamp(rawClash, 0.0, 1.0);

    if (result.confidence >= 0.50f) {
        result.vocalClashScore = static_cast<float>(rawClash);
    } else {
        // Conservative handling for low confidence: never assume 0 clash when vocal activity was detected
        result.vocalClashScore = static_cast<float>(
            std::clamp(0.65 * rawClash + 0.35 * (rawClash > 0.05 ? 0.30 : 0.0), 0.0, 1.0));
    }
    result.vocalOverInstrumentalScore = static_cast<float>(
        std::clamp((vocalBedAccum / count) * 1.35, 0.0, 1.0));

    // Check whether an audible outgoing vocal phrase on A is cut off or faded
    // out before its completion, or whether incoming B enters mid-vocal phrase.
    const double endA = startA + duration * safeRateA;
    bool cutsMidVocalA = timeInsideVocalRegion(featuresA.vocal, endA, 0.30);
    if (!cutsMidVocalA) {
        constexpr double kMargin = 0.30;
        for (const VocalRegion& reg : featuresA.vocal.regions) {
            if (!reg.leadLikely || reg.end - reg.start < 2.0 * kMargin) {
                continue;
            }
            const double overlapStart = std::max(startA, reg.start);
            const double overlapEnd = std::min(endA, reg.end - kMargin);
            if (overlapEnd <= overlapStart) {
                continue;
            }
            // Check whether the vocal phrase was audibly heard in the overlap
            // and then lost audibility (gOut < 0.35) before finishing.
            bool heardPhrase = false;
            for (int i = 0; i <= steps; ++i) {
                const double tOut = i * dt;
                const double posA = startA + tOut * safeRateA;
                if (posA < reg.start + kMargin || posA > reg.end - kMargin) {
                    continue;
                }
                const float gOut = evalGainCurve(gainCurveOut, tOut, duration, false) *
                                   evalMidEqCurve(eqCurveOut, tOut);
                if (gOut >= 0.35f) {
                    heardPhrase = true;
                } else if (heardPhrase) {
                    cutsMidVocalA = true;
                    break;
                }
            }
            if (cutsMidVocalA) {
                break;
            }
        }
    }
    result.cutsMidVocalPhraseA = cutsMidVocalA;
    result.entersMidVocalPhraseB =
        timeInsideVocalRegion(featuresB.vocal, startB, 0.30);

    // Clean vocal handoff: A finishes its vocal phrase before B's vocal phrase
    // begins (or one side is instrumental), with strictly minimal simultaneous clash.
    const bool sequentialHandoff =
        (lastVocalTimeA >= 0.0 && firstVocalTimeB < 1e8 &&
         firstVocalTimeB >= lastVocalTimeA - 0.15) ||
        (result.outgoingVocalMean >= 0.30f && result.incomingVocalMean < 0.18f) ||
        (result.incomingVocalMean >= 0.30f && result.outgoingVocalMean < 0.18f);
    result.cleanVocalHandoff =
        sequentialHandoff && !result.severeLeadVocalClash &&
        result.clashDurationSeconds <= 0.30 &&
        result.leadVocalClashDurationSeconds <= 0.10 &&
        result.vocalClashScore <= 0.15f &&
        !result.cutsMidVocalPhraseA && !result.entersMidVocalPhraseB;

    char buf[256];
    if (result.severeLeadVocalClash) {
        std::snprintf(buf, sizeof(buf),
                      "Severe lead vocal collision (%.1f s simultaneous lead vocals, clash %.2f, conf %.2f)",
                      result.leadVocalClashDurationSeconds > 0.0 ? result.leadVocalClashDurationSeconds : result.clashDurationSeconds,
                      static_cast<double>(result.vocalClashScore),
                      static_cast<double>(result.confidence));
    } else if (result.vocalClashScore >= 0.25f) {
        std::snprintf(buf, sizeof(buf),
                      "Vocal clash risk %.2f (%.1f s overlap, conf %.2f)",
                      static_cast<double>(result.vocalClashScore),
                      result.clashDurationSeconds,
                      static_cast<double>(result.confidence));
    } else if (result.cleanVocalHandoff) {
        std::snprintf(buf, sizeof(buf),
                      "Clean vocal handoff (clash %.2f, vocal-over-instrumental %.2f, conf %.2f)",
                      static_cast<double>(result.vocalClashScore),
                      static_cast<double>(result.vocalOverInstrumentalScore),
                      static_cast<double>(result.confidence));
    } else if (result.lowConfidenceConservative) {
        std::snprintf(buf, sizeof(buf),
                      "Low vocal confidence %.2f; using conservative safety margin (clash %.2f)",
                      static_cast<double>(result.confidence),
                      static_cast<double>(result.vocalClashScore));
    } else {
        std::snprintf(buf, sizeof(buf),
                      "Low vocal activity in overlap (clash %.2f, conf %.2f)",
                      static_cast<double>(result.vocalClashScore),
                      static_cast<double>(result.confidence));
    }
    result.summary = buf;
    return result;
}

} // namespace bgmedia::mix
