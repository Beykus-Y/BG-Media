#include "bgmedia/mix/HarmonicAnalyzer.hpp"

#include "analysis/Dsp.hpp"
#include "bgmedia/mix/Crossfade.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

namespace bgmedia::mix {
namespace {

// Krumhansl-Kessler / Temperley normalized major and minor pitch-class profiles
// indexed 0..11 relative to the tonic.
constexpr std::array<float, 12> kMajorProfile = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
    2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};

constexpr std::array<float, 12> kMinorProfile = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
    2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
};

constexpr const char* kPitchClassNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Camelot wheel numbers for major (B) and minor (A) keys indexed by pitch class C..B.
constexpr int kCamelotMajor[12] = {
    8,  // C  -> 8B
    3,  // C# -> 3B
    10, // D  -> 10B
    5,  // D# -> 5B
    12, // E  -> 12B
    7,  // F  -> 7B
    2,  // F# -> 2B
    9,  // G  -> 9B
    4,  // G# -> 4B
    11, // A  -> 11B
    6,  // A# -> 6B
    1   // B  -> 1B
};

constexpr int kCamelotMinor[12] = {
    5,  // Cm  -> 5A
    12, // C#m -> 12A
    7,  // Dm  -> 7A
    2,  // D#m -> 2A
    9,  // Em  -> 9A
    4,  // Fm  -> 4A
    11, // F#m -> 11A
    6,  // Gm  -> 6A
    1,  // G#m -> 1A
    8,  // Am  -> 8A
    3,  // A#m -> 3A
    10  // Bm  -> 10A
};

void normalizeChroma(std::array<float, 12>& chroma) noexcept {
    double sum = 0.0;
    for (float v : chroma) {
        sum += std::max(0.0f, v);
    }
    if (sum > 1e-12) {
        const float inv = static_cast<float>(1.0 / sum);
        for (float& v : chroma) {
            v = std::max(0.0f, v) * inv;
        }
    } else {
        chroma.fill(0.0f);
    }
}

float pearsonCorrelation(const std::array<float, 12>& x,
                         const std::array<float, 12>& profile,
                         int tonic) noexcept {
    double meanX = 0.0;
    double meanY = 0.0;
    for (int i = 0; i < 12; ++i) {
        meanX += x[static_cast<std::size_t>(i)];
        meanY += profile[static_cast<std::size_t>(i)];
    }
    meanX /= 12.0;
    meanY /= 12.0;

    double num = 0.0;
    double denX = 0.0;
    double denY = 0.0;
    for (int i = 0; i < 12; ++i) {
        const int rel = (i - tonic + 12) % 12;
        const double dx = x[static_cast<std::size_t>(i)] - meanX;
        const double dy = profile[static_cast<std::size_t>(rel)] - meanY;
        num += dx * dy;
        denX += dx * dx;
        denY += dy * dy;
    }
    const double den = std::sqrt(denX * denY);
    return den > 1e-12 ? static_cast<float>(num / den) : 0.0f;
}

float evalGainAt(const std::vector<GainPoint>& curve, double time,
                 double duration, bool incoming) noexcept {
    if (curve.empty()) {
        const double t = duration > 1e-6 ? std::clamp(time / duration, 0.0, 1.0) : 0.5;
        return incoming ? equalPowerIn(t) : equalPowerOut(t);
    }
    if (time <= curve.front().time) return curve.front().gain;
    if (time >= curve.back().time) return curve.back().gain;
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

EqGainPoint evalEqAt(const std::vector<EqGainPoint>& curve, double time) noexcept {
    EqGainPoint unit;
    unit.time = time;
    unit.low = 1.0f;
    unit.mid = 1.0f;
    unit.high = 1.0f;
    if (curve.empty()) return unit;
    if (time <= curve.front().time) return curve.front();
    if (time >= curve.back().time) return curve.back();
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (time <= curve[i].time) {
            const double span = std::max(1e-9, curve[i].time - curve[i - 1].time);
            const double u = std::clamp((time - curve[i - 1].time) / span, 0.0, 1.0);
            EqGainPoint out;
            out.time = time;
            out.low = static_cast<float>(curve[i - 1].low +
                                         (curve[i].low - curve[i - 1].low) * u);
            out.mid = static_cast<float>(curve[i - 1].mid +
                                         (curve[i].mid - curve[i - 1].mid) * u);
            out.high = static_cast<float>(curve[i - 1].high +
                                          (curve[i].high - curve[i - 1].high) * u);
            return out;
        }
    }
    return curve.back();
}

std::array<float, 12> aggregateWindowChroma(const MixFeatureMap& map,
                                            double start, double end,
                                            float* tonalnessOut,
                                            float* confidenceOut) noexcept {
    std::array<float, 12> accum{};
    double tonalSum = 0.0;
    double confSum = 0.0;
    double weightSum = 0.0;

    for (const HarmonicSegment& seg : map.harmony.segments) {
        const double ovStart = std::max(start, seg.start);
        const double ovEnd = std::min(end, seg.end);
        if (ovEnd > ovStart) {
            const double w = (ovEnd - ovStart) * std::max(0.1f, seg.tonalness);
            for (int c = 0; c < 12; ++c) {
                accum[static_cast<std::size_t>(c)] +=
                    static_cast<float>(w) * seg.chroma[static_cast<std::size_t>(c)];
            }
            tonalSum += (ovEnd - ovStart) * seg.tonalness;
            confSum += (ovEnd - ovStart) * seg.confidence;
            weightSum += (ovEnd - ovStart);
        }
    }

    if (weightSum <= 1e-6) {
        accum = map.harmony.globalChroma;
        if (tonalnessOut) *tonalnessOut = map.harmony.tonalness;
        if (confidenceOut) *confidenceOut = map.harmony.keyConfidence;
        return accum;
    }

    normalizeChroma(accum);
    if (tonalnessOut) {
        *tonalnessOut = static_cast<float>(std::clamp(tonalSum / weightSum, 0.0, 1.0));
    }
    if (confidenceOut) {
        *confidenceOut = static_cast<float>(std::clamp(confSum / weightSum, 0.0, 1.0));
    }
    return accum;
}

} // namespace

float HarmonicAnalyzer::computeChromaTonalness(
    const std::array<float, 12>& chroma) noexcept {
    std::array<float, 12> norm = chroma;
    normalizeChroma(norm);
    double sum = 0.0;
    for (float v : norm) {
        sum += v;
    }
    if (sum <= 1e-6) {
        return 0.0f;
    }

    // Shannon entropy of the 12-bin distribution vs uniform log(12) + top-3 mass.
    double entropy = 0.0;
    for (float v : norm) {
        if (v > 1e-7f) {
            entropy -= static_cast<double>(v) * std::log(static_cast<double>(v));
        }
    }
    const double maxEntropy = std::log(12.0);
    const double flatness = std::clamp(entropy / maxEntropy, 0.0, 1.0);

    std::array<float, 12> sorted = norm;
    std::sort(sorted.begin(), sorted.end(), std::greater<float>());
    const double top3 = sorted[0] + sorted[1] + sorted[2];
    const double top3Score = std::clamp((top3 - 0.30) / 0.50, 0.0, 1.0);
    const double entropyScore = std::clamp((1.0 - flatness) * 1.45, 0.0, 1.0);

    return static_cast<float>(std::clamp(0.55 * top3Score + 0.45 * entropyScore, 0.0, 1.0));
}

void HarmonicAnalyzer::estimateKeyFromChroma(const std::array<float, 12>& chroma,
                                             float tonalness,
                                             int* keyIndexOut,
                                             bool* isMinorOut,
                                             float* confidenceOut) noexcept {
    std::array<float, 12> norm = chroma;
    normalizeChroma(norm);

    double sum = 0.0;
    for (float v : norm) sum += v;
    if (sum <= 1e-6 || tonalness < 0.08f) {
        if (keyIndexOut) *keyIndexOut = -1;
        if (isMinorOut) *isMinorOut = false;
        if (confidenceOut) *confidenceOut = 0.0f;
        return;
    }

    struct Candidate {
        int key = 0;
        bool minor = false;
        float score = -1.0f;
    };
    std::array<Candidate, 24> candidates{};
    for (int k = 0; k < 12; ++k) {
        // Combine profile correlation with tonic + fifth triad support so both
        // full chords and root-heavy bass/synth lines resolve accurately.
        const float triadMaj =
            norm[static_cast<std::size_t>(k)] * 0.50f +
            norm[static_cast<std::size_t>((k + 4) % 12)] * 0.28f +
            norm[static_cast<std::size_t>((k + 7) % 12)] * 0.22f;
        const float triadMin =
            norm[static_cast<std::size_t>(k)] * 0.50f +
            norm[static_cast<std::size_t>((k + 3) % 12)] * 0.28f +
            norm[static_cast<std::size_t>((k + 7) % 12)] * 0.22f;

        const float corrMaj = pearsonCorrelation(norm, kMajorProfile, k);
        const float corrMin = pearsonCorrelation(norm, kMinorProfile, k);

        candidates[static_cast<std::size_t>(k * 2)] = {
            k, false, 0.68f * corrMaj + 0.32f * (triadMaj * 2.2f - 0.3f)};
        candidates[static_cast<std::size_t>(k * 2 + 1)] = {
            k, true, 0.68f * corrMin + 0.32f * (triadMin * 2.2f - 0.3f)};
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.score > b.score;
              });

    const Candidate& best = candidates[0];
    // Find best non-relative competitor for confidence margin.
    float runnerUp = candidates[1].score;
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        const bool isRelative =
            (best.minor != c.minor) &&
            ((best.minor && (best.key + 3) % 12 == c.key) ||
             (!best.minor && (c.key + 3) % 12 == best.key));
        if (!isRelative) {
            runnerUp = c.score;
            break;
        }
    }

    const float margin = std::max(0.0f, best.score - runnerUp);
    const float rawConf =
        std::clamp(0.35f * std::max(0.0f, best.score) + 1.45f * margin, 0.0f, 1.0f);
    const float conf = std::clamp(rawConf * std::min(1.0f, tonalness * 1.25f), 0.0f, 0.98f);

    if (keyIndexOut) *keyIndexOut = best.key;
    if (isMinorOut) *isMinorOut = best.minor;
    if (confidenceOut) *confidenceOut = conf;
}

std::array<float, 12> HarmonicAnalyzer::shiftChroma(
    const std::array<float, 12>& chroma,
    double semitones) noexcept {
    if (std::abs(semitones) <= 1e-6) {
        return chroma;
    }
    std::array<float, 12> out{};
    for (int i = 0; i < 12; ++i) {
        double target = std::fmod(static_cast<double>(i) + semitones, 12.0);
        if (target < 0.0) {
            target += 12.0;
        }
        const int lowBin = static_cast<int>(std::floor(target)) % 12;
        const int highBin = (lowBin + 1) % 12;
        const float frac = static_cast<float>(target - std::floor(target));
        out[static_cast<std::size_t>(lowBin)] +=
            chroma[static_cast<std::size_t>(i)] * (1.0f - frac);
        out[static_cast<std::size_t>(highBin)] +=
            chroma[static_cast<std::size_t>(i)] * frac;
    }
    normalizeChroma(out);
    return out;
}

float HarmonicAnalyzer::chromaCosineSimilarity(
    const std::array<float, 12>& a,
    const std::array<float, 12>& b) noexcept {
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double va = std::max(0.0f, a[static_cast<std::size_t>(i)]);
        const double vb = std::max(0.0f, b[static_cast<std::size_t>(i)]);
        dot += va * vb;
        na += va * va;
        nb += vb * vb;
    }
    const double den = std::sqrt(na * nb);
    return den > 1e-12 ? static_cast<float>(std::clamp(dot / den, 0.0, 1.0)) : 0.0f;
}

float HarmonicAnalyzer::keyRelationshipScore(
    int keyA, bool minorA,
    int keyB, bool minorB) noexcept {
    if (keyA < 0 || keyA >= 12 || keyB < 0 || keyB >= 12) {
        return 0.5f;
    }
    if (keyA == keyB && minorA == minorB) {
        return 1.0f; // Exact unison key
    }
    // Relative major / minor (e.g. A minor <-> C major)
    if (minorA != minorB) {
        const int minKey = minorA ? keyA : keyB;
        const int majKey = minorA ? keyB : keyA;
        if ((minKey + 3) % 12 == majKey) {
            return 0.93f;
        }
        if (minKey == majKey) {
            return 0.78f; // Parallel major / minor (e.g. A minor <-> A major)
        }
    }
    // Circle-of-fifths distance between tonics (or relative major tonics)
    const int majTonicA = minorA ? (keyA + 3) % 12 : keyA;
    const int majTonicB = minorB ? (keyB + 3) % 12 : keyB;
    const int fifthSteps = ((majTonicB - majTonicA) * 7 % 12 + 12) % 12;
    const int circleDist = std::min(fifthSteps, 12 - fifthSteps);

    switch (circleDist) {
    case 0:
        return 0.92f;
    case 1:
        return minorA == minorB ? 0.86f : 0.76f; // Dominant / Subdominant (+1/-1 on wheel)
    case 2:
        return minorA == minorB ? 0.64f : 0.56f; // Whole-tone / +2 on wheel
    case 3:
        return 0.48f; // Mediant / +3
    case 4:
        return 0.34f;
    case 5:
        return 0.20f; // Semitone clash (7 semitones * 5 = 1 semitone)
    case 6:
        return 0.14f; // Tritone clash
    default:
        return 0.35f;
    }
}

std::string HarmonicAnalyzer::toCamelotCode(int keyIndex, bool isMinor) {
    if (keyIndex < 0 || keyIndex >= 12) {
        return {};
    }
    const int num = isMinor ? kCamelotMinor[keyIndex] : kCamelotMajor[keyIndex];
    return std::to_string(num) + (isMinor ? "A" : "B");
}

std::string HarmonicAnalyzer::keyName(int keyIndex, bool isMinor) {
    if (keyIndex < 0 || keyIndex >= 12) {
        return "Unknown";
    }
    return std::string(kPitchClassNames[keyIndex]) + (isMinor ? "m" : "");
}

void HarmonicAnalyzer::analyze(const PcmBuffer& monoPcm,
                               MixFeatureMap& map) const {
    const std::size_t n = map.frames.size();
    map.info.harmonicAnalyzeVersion = kVersion;
    if (n == 0 || !monoPcm.valid()) {
        map.harmony = {};
        return;
    }

    const double sr = static_cast<double>(monoPcm.sampleRate);
    // Use a 4096-point FFT (~85 ms at 48 kHz) for fine low/mid frequency resolution.
    const int fftSize = std::clamp(
        dsp::nextPowerOfTwo(static_cast<int>(std::lround(sr * 0.085))), 1024, 8192);
    dsp::RealFft fft(fftSize);
    if (!fft.valid()) {
        map.harmony = {};
        return;
    }

    const std::vector<float> window = dsp::hannWindow(fftSize);
    std::vector<float> timeBuf(static_cast<std::size_t>(fftSize), 0.0f);
    std::vector<kiss_fft_cpx> freqBuf(static_cast<std::size_t>(fft.binCount()));
    const int bins = fft.binCount();
    const double binHz = sr / static_cast<double>(fftSize);
    std::vector<float> magSq(static_cast<std::size_t>(bins), 0.0f);

    std::vector<std::array<float, 12>> frameChromas(n);
    std::array<double, 12> globalChromaAccum{};
    double globalWeight = 0.0;

    for (std::size_t f = 0; f < n; ++f) {
        MixFeatureFrame& frame = map.frames[f];
        frameChromas[f].fill(0.0f);
        if (!frame.active || frame.rms < 1e-5f) {
            frame.harmonicSalience = 0.0f;
            frame.dominantPitchClass = -1;
            continue;
        }

        const int centerSample = static_cast<int>(std::lround(frame.time * sr));
        const int startSample = centerSample - fftSize / 2;
        for (int i = 0; i < fftSize; ++i) {
            const int idx = startSample + i;
            float s = 0.0f;
            if (idx >= 0 && static_cast<std::size_t>(idx) < monoPcm.samples.size()) {
                s = monoPcm.samples[static_cast<std::size_t>(idx)];
                if (!std::isfinite(s)) s = 0.0f;
            }
            timeBuf[static_cast<std::size_t>(i)] = s * window[static_cast<std::size_t>(i)];
        }

        fft.forward(timeBuf.data(), freqBuf.data());
        for (int b = 1; b < bins; ++b) {
            const double re = freqBuf[static_cast<std::size_t>(b)].r;
            const double im = freqBuf[static_cast<std::size_t>(b)].i;
            magSq[static_cast<std::size_t>(b)] = static_cast<float>(re * re + im * im);
        }

        // Local spectral peak picking in 55..3520 Hz (A1..A7) suppresses broadband
        // drum/noise floors and maps pitched harmonics to 12 pitch classes.
        std::array<float, 12> chroma{};
        for (int b = 2; b + 1 < bins; ++b) {
            const float p = magSq[static_cast<std::size_t>(b)];
            if (p <= 1e-12f ||
                p < magSq[static_cast<std::size_t>(b - 1)] ||
                p < magSq[static_cast<std::size_t>(b + 1)]) {
                continue;
            }
            // Parabolic interpolation for sub-bin frequency accuracy.
            const double alpha = magSq[static_cast<std::size_t>(b - 1)];
            const double beta = p;
            const double gamma = magSq[static_cast<std::size_t>(b + 1)];
            const double denom = alpha - 2.0 * beta + gamma;
            const double delta =
                std::abs(denom) > 1e-12 ? 0.5 * (alpha - gamma) / denom : 0.0;
            const double hz = (static_cast<double>(b) + std::clamp(delta, -0.5, 0.5)) * binHz;
            if (hz < 55.0 || hz > 3520.0) {
                continue;
            }

            // MIDI note number: 69 = A4 (440 Hz), C4 = 60 (pitch class 0).
            const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
            const double nearestMidi = std::round(midi);
            const double centsError = std::abs(midi - nearestMidi);
            const double tuningWeight = std::exp(-2.5 * centsError * centsError);
            const double octWeight = (hz <= 880.0) ? 1.0 : (880.0 / hz);
            const int pc = ((static_cast<int>( std::lround(nearestMidi)) % 12) + 12) % 12;

            chroma[static_cast<std::size_t>(pc)] +=
                static_cast<float>( std::sqrt(beta) * tuningWeight * octWeight);
        }

        const float tonalness = computeChromaTonalness(chroma);
        normalizeChroma(chroma);
        frameChromas[f] = chroma;

        // Penalize percussive frames so drum hits don't masquerade as tonal peaks.
        const float percFactor = std::clamp(1.0f - 0.45f * frame.onsetDensity, 0.35f, 1.0f);
        frame.harmonicSalience = std::clamp(tonalness * percFactor, 0.0f, 1.0f);

        int domPc = -1;
        float domVal = 0.0f;
        for (int c = 0; c < 12; ++c) {
            if (chroma[static_cast<std::size_t>(c)] > domVal) {
                domVal = chroma[static_cast<std::size_t>(c)];
                domPc = c;
            }
        }
        frame.dominantPitchClass =
            static_cast<std::int8_t>(frame.harmonicSalience >= 0.20f ? domPc : -1);

        const double w = static_cast<double>(frame.energy) *
                         std::max(0.05f, frame.harmonicSalience);
        for (int c = 0; c < 12; ++c) {
            globalChromaAccum[static_cast<std::size_t>(c)] +=
                w * chroma[static_cast<std::size_t>(c)];
        }
        globalWeight += w;
    }

    map.harmony = {};
    map.harmony.available = globalWeight > 1e-6;
    if (!map.harmony.available) {
        return;
    }

    for (int c = 0; c < 12; ++c) {
        map.harmony.globalChroma[static_cast<std::size_t>(c)] =
            static_cast<float>(globalChromaAccum[static_cast<std::size_t>(c)] / globalWeight);
    }
    map.harmony.tonalness = computeChromaTonalness(map.harmony.globalChroma);
    normalizeChroma(map.harmony.globalChroma);
    estimateKeyFromChroma(map.harmony.globalChroma, map.harmony.tonalness,
                          &map.harmony.keyIndex, &map.harmony.isMinor,
                          &map.harmony.keyConfidence);
    map.harmony.camelotCode =
        toCamelotCode(map.harmony.keyIndex, map.harmony.isMinor);

    // Build local HarmonicSegments aligned to phrases (or 4-second windows if no phrases),
    // including any leading/trailing audio outside the detected phrase spans.
    std::vector<std::pair<double, double>> spans;
    if (!map.phrases.empty()) {
        const double firstStart = map.phrases.front().start;
        if (firstStart > 0.25) {
            for (double t = 0.0; t + 0.25 < firstStart; t += 4.0) {
                spans.emplace_back(t, std::min(firstStart, t + 4.0));
            }
        }
        for (const PhraseSpan& p : map.phrases) {
            if (p.end > p.start + 0.25) {
                spans.emplace_back(p.start, p.end);
            }
        }
        const double lastEnd = map.phrases.back().end;
        if (lastEnd + 0.25 < map.info.duration) {
            for (double t = lastEnd; t + 0.25 < map.info.duration; t += 4.0) {
                spans.emplace_back(t, std::min(map.info.duration, t + 4.0));
            }
        }
    } else {
        for (double t = 0.0; t < map.info.duration; t += 4.0) {
            spans.emplace_back(t, std::min(map.info.duration, t + 4.0));
        }
    }

    const double step = std::max(1e-4, map.info.frameStepSeconds);
    for (const auto& sp : spans) {
        const double startProbe =
            (sp.second > sp.first) ? std::min(sp.second, sp.first + 1e-7 * step) : sp.first;
        const double endProbe =
            (sp.second > sp.first) ? std::max(sp.first, sp.second - 1e-7 * step) : sp.second;
        const int i0 = std::clamp(static_cast<int>(std::floor(startProbe / step)),
                                  0, static_cast<int>(n) - 1);
        const int i1 = std::clamp(static_cast<int>(std::floor(endProbe / step)),
                                  i0, static_cast<int>(n) - 1);
        std::array<float, 12> segChroma{};
        double segSalienceSum = 0.0;
        int segCount = 0;
        for (int i = i0; i <= i1; ++i) {
            const float w = std::max(0.05f, map.frames[static_cast<std::size_t>(i)].energy);
            for (int c = 0; c < 12; ++c) {
                segChroma[static_cast<std::size_t>(c)] +=
                    w * frameChromas[static_cast<std::size_t>(i)][static_cast<std::size_t>(c)];
            }
            segSalienceSum += map.frames[static_cast<std::size_t>(i)].harmonicSalience;
            ++segCount;
        }
        HarmonicSegment seg;
        seg.start = sp.first;
        seg.end = sp.second;
        seg.tonalness = std::min(
            computeChromaTonalness(segChroma),
            segCount > 0 ? static_cast<float>(segSalienceSum / segCount) * 1.25f : 0.0f);
        normalizeChroma(segChroma);
        seg.chroma = segChroma;
        estimateKeyFromChroma(seg.chroma, seg.tonalness,
                              &seg.keyIndex, &seg.isMinor, &seg.confidence);
        map.harmony.segments.push_back(seg);
    }

    // Populate cue harmonicStability on Mix-In and Mix-Out candidates.
    for (CuePointCandidate& cue : map.mixOutCandidates) {
        float tonal = 0.0f;
        float conf = 0.0f;
        (void)aggregateWindowChroma(map, cue.time,
                                    std::min(map.info.duration, cue.time + 6.0),
                                    &tonal, &conf);
        cue.harmonicStability = std::clamp(0.5f * tonal + 0.5f * conf, 0.0f, 1.0f);
    }
    for (CuePointCandidate& cue : map.mixInCandidates) {
        float tonal = 0.0f;
        float conf = 0.0f;
        (void)aggregateWindowChroma(map, cue.time,
                                    std::min(map.info.duration, cue.time + 6.0),
                                    &tonal, &conf);
        cue.harmonicStability = std::clamp(0.5f * tonal + 0.5f * conf, 0.0f, 1.0f);
    }
}

HarmonicCompatibility evaluateHarmonicCompatibility(
    const MixFeatureMap& featuresA,
    double startA,
    double rateA,
    const MixFeatureMap& featuresB,
    double startB,
    double rateB,
    double duration,
    bool pitchLock,
    const std::vector<GainPoint>& gainCurveOut,
    const std::vector<GainPoint>& gainCurveIn,
    const std::vector<EqGainPoint>& eqCurveOut,
    const std::vector<EqGainPoint>& eqCurveIn) {
    HarmonicCompatibility result;
    if (!featuresA.valid() || !featuresB.valid() || duration <= 0.05) {
        result.summary = "harmonic assessment unavailable (short or empty window)";
        return result;
    }

    const double safeRateA = std::max(0.25, rateA);
    const double safeRateB = std::max(0.25, rateB);
    const double endA = std::min(featuresA.info.duration, startA + duration * safeRateA);
    const double endB = std::min(featuresB.info.duration, startB + duration * safeRateB);

    // 1. Evaluate spectral crowding and simultaneous audibility over the overlap.
    const int steps = std::max(2, static_cast<int>(std::ceil(duration / 0.15)));
    const double dt = duration / static_cast<double>(steps);
    double midCrowdSum = 0.0;
    double lowCrowdSum = 0.0;
    double audOverlapSum = 0.0;
    for (int i = 0; i <= steps; ++i) {
        const double t = i * dt;
        const MixFeatureFrame fA = featuresA.sampleAt(startA + t * safeRateA);
        const MixFeatureFrame fB = featuresB.sampleAt(startB + t * safeRateB);
        const float gOut = evalGainAt(gainCurveOut, t, duration, false);
        const float gIn = evalGainAt(gainCurveIn, t, duration, true);
        const EqGainPoint eqOut = evalEqAt(eqCurveOut, t);
        const EqGainPoint eqIn = evalEqAt(eqCurveIn, t);

        const double lowA = fA.energy * fA.lowShare * gOut * eqOut.low;
        const double lowB = fB.energy * fB.lowShare * gIn * eqIn.low;
        const double midA = fA.energy * fA.midShare * gOut * eqOut.mid;
        const double midB = fB.energy * fB.midShare * gIn * eqIn.mid;

        lowCrowdSum += std::sqrt(std::max(0.0, lowA * lowB));
        midCrowdSum += std::sqrt(std::max(0.0, midA * midB));

        const double audOut = std::clamp(
            static_cast<double>(gOut) * 0.5 * (eqOut.mid + eqOut.high), 0.0, 1.0);
        const double audIn = std::clamp(
            static_cast<double>(gIn) * 0.5 * (eqIn.mid + eqIn.high), 0.0, 1.0);
        audOverlapSum += std::min(1.0, std::sqrt(audOut * audIn) * 1.45);
    }
    const double count = static_cast<double>(steps + 1);
    const float audOverlapFactor = static_cast<float>(
        std::clamp((audOverlapSum / count) * 1.35, 0.0, 1.0));
    result.spectralCrowdScore = static_cast<float>(
        std::clamp(1.6 * (0.55 * (midCrowdSum / count) + 0.45 * (lowCrowdSum / count)),
                   0.0, 1.0));

    if (!featuresA.harmony.available || !featuresB.harmony.available) {
        result.available = false;
        result.compatibilityScore = 0.65f * (1.0f - 0.35f * result.spectralCrowdScore);
        result.harmonicClashScore = 0.25f * result.spectralCrowdScore;
        result.summary = "harmonic chroma unavailable; scored by spectral crowding only";
        return result;
    }

    result.available = true;
    float confA = 0.0f;
    float confB = 0.0f;
    std::array<float, 12> chromaA =
        aggregateWindowChroma(featuresA, startA, endA, &result.tonalnessA, &confA);
    std::array<float, 12> chromaB =
        aggregateWindowChroma(featuresB, startB, endB, &result.tonalnessB, &confB);

    // 2. Pitch-shift awareness when pitchLock == false.
    if (!pitchLock) {
        result.semitoneShiftA = 12.0 * std::log2(safeRateA);
        result.semitoneShiftB = 12.0 * std::log2(safeRateB);
        chromaA = HarmonicAnalyzer::shiftChroma(chromaA, result.semitoneShiftA);
        chromaB = HarmonicAnalyzer::shiftChroma(chromaB, result.semitoneShiftB);

        const double netShift = result.semitoneShiftA - result.semitoneShiftB;
        const double nearestInt = std::round(netShift);
        const double fracSemitone = std::abs(netShift - nearestInt); // 0..0.5
        // A 0.25..0.50 semitone (quarter-tone) mismatch between two tonal tracks
        // sounds noticeably out of tune.
        result.detunePenalty = static_cast<float>(
            std::clamp((fracSemitone - 0.08) / 0.38, 0.0, 1.0));
    }

    result.percussiveNeutral =
        (result.tonalnessA < 0.25f || result.tonalnessB < 0.25f);
    result.confidence = std::min(confA, confB);

    // Estimate effective local keys after any pitch shift.
    int keyA = -1;
    bool minA = false;
    float keyConfA = 0.0f;
    int keyB = -1;
    bool minB = false;
    float keyConfB = 0.0f;
    HarmonicAnalyzer::estimateKeyFromChroma(chromaA, result.tonalnessA,
                                            &keyA, &minA, &keyConfA);
    HarmonicAnalyzer::estimateKeyFromChroma(chromaB, result.tonalnessB,
                                            &keyB, &minB, &keyConfB);

    result.chromaSimilarity =
        HarmonicAnalyzer::chromaCosineSimilarity(chromaA, chromaB);
    result.keyRelationScore =
        HarmonicAnalyzer::keyRelationshipScore(keyA, minA, keyB, minB);

    if (result.percussiveNeutral) {
        // Percussive / drum-only / atonal window: do not penalize key mismatch.
        result.compatibilityScore =
            std::clamp(0.82f - 0.25f * result.spectralCrowdScore, 0.45f, 0.95f);
        result.harmonicClashScore = 0.15f * result.spectralCrowdScore;
        result.summary = "Percussive/low-tonalness overlap: harmonic key clash neutralized";
        return result;
    }

    result.detunePenalty *= audOverlapFactor;
    const float jointTonalness = std::min(result.tonalnessA, result.tonalnessB);
    const float rawHarmonic =
        0.55f * result.chromaSimilarity + 0.45f * result.keyRelationScore;
    const float detunedHarmonic =
        std::max(0.0f, rawHarmonic - 0.45f * result.detunePenalty * jointTonalness);

    // Weight toward neutral (0.72) when tonalness/confidence or simultaneous
    // audibility is low, and apply full harmonic discrimination when both
    // tracks are strongly tonal and simultaneously audible.
    const float weight =
        std::clamp(jointTonalness * 1.15f, 0.20f, 1.0f) * audOverlapFactor;
    result.compatibilityScore = std::clamp(
        weight * detunedHarmonic + (1.0f - weight) * 0.72f -
            0.18f * result.spectralCrowdScore,
        0.0f, 1.0f);
    result.harmonicClashScore = std::clamp(
        weight * ((1.0f - detunedHarmonic) * 0.85f +
                  0.35f * result.detunePenalty) +
            0.20f * result.spectralCrowdScore,
        0.0f, 1.0f);

    char buf[256];
    const std::string nameA = HarmonicAnalyzer::keyName(keyA, minA);
    const std::string camA = HarmonicAnalyzer::toCamelotCode(keyA, minA);
    const std::string nameB = HarmonicAnalyzer::keyName(keyB, minB);
    const std::string camB = HarmonicAnalyzer::toCamelotCode(keyB, minB);
    std::snprintf(
        buf, sizeof(buf),
        "Harmony %s (%s) -> %s (%s): compat %.2f (chroma %.2f, keyRel %.2f, detune %.2f, crowd %.2f)",
        nameA.c_str(), camA.empty() ? "-" : camA.c_str(),
        nameB.c_str(), camB.empty() ? "-" : camB.c_str(),
        static_cast<double>(result.compatibilityScore),
        static_cast<double>(result.chromaSimilarity),
        static_cast<double>(result.keyRelationScore),
        static_cast<double>(result.detunePenalty),
        static_cast<double>(result.spectralCrowdScore));
    result.summary = buf;
    return result;
}

} // namespace bgmedia::mix
