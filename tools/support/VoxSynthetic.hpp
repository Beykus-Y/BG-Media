#pragma once

// Deterministic synthetic speech-like signals and noise for BG Vox tests and
// the bgmedia_vox_enhance tool. Header-only, no dependencies beyond the STL.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bgmedia::voxsynth {

inline constexpr double kPi = 3.14159265358979323846;

// xorshift64*: fixed seed, identical on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {
        if (s_ == 0) {
            s_ = 88172645463325252ull;
        }
        for (int i = 0; i < 4; ++i) {
            next();
        }
    }
    std::uint64_t next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 2685821657736338717ull;
    }
    double uniform() {  // [0, 1)
        return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    double gauss() {
        const double u1 = std::max(uniform(), 1e-12);
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    }

private:
    std::uint64_t s_;
};

enum class NoiseKind { White, Pink, FanHum, Clicks };

inline const char* noiseName(NoiseKind kind) {
    switch (kind) {
    case NoiseKind::White: return "white";
    case NoiseKind::Pink: return "pink";
    case NoiseKind::FanHum: return "fan_hum";
    case NoiseKind::Clicks: return "clicks";
    }
    return "?";
}

inline double rmsOf(const std::vector<float>& x) {
    if (x.empty()) {
        return 0.0;
    }
    double s = 0.0;
    for (float v : x) {
        s += static_cast<double>(v) * static_cast<double>(v);
    }
    return std::sqrt(s / static_cast<double>(x.size()));
}

inline void scaleToRms(std::vector<float>& x, double targetRms) {
    const double r = rmsOf(x);
    if (r <= 0.0) {
        return;
    }
    const float g = static_cast<float>(targetRms / r);
    for (float& v : x) {
        v *= g;
    }
}

struct SpeechSignal {
    std::vector<float> samples;
    std::vector<std::uint8_t> speechMask;  // inside a word
    std::vector<std::uint8_t> pauseMask;   // inside a pause, 50 ms guard each side
};

// Voiced bursts: harmonic tones, f0 110-220 Hz with vibrato, up to 20
// harmonics shaped by three formants, 4-6 Hz syllable envelope, 300-700 ms
// words separated by 200-600 ms pauses. Whole-signal RMS is -24 dBFS.
inline SpeechSignal makeSpeech(std::uint32_t sampleRate, double seconds, std::uint64_t seed) {
    const std::size_t n = static_cast<std::size_t>(seconds * sampleRate);
    const double sr = static_cast<double>(sampleRate);
    SpeechSignal out;
    out.samples.assign(n, 0.0f);
    out.speechMask.assign(n, 0);
    out.pauseMask.assign(n, 0);
    Rng rng(seed);

    std::size_t pos = static_cast<std::size_t>(0.25 * sr);  // leading pause
    while (pos < n) {
        const std::size_t wordLen = static_cast<std::size_t>(rng.range(0.30, 0.70) * sr);
        const std::size_t pauseLen = static_cast<std::size_t>(rng.range(0.20, 0.60) * sr);
        const std::size_t end = std::min(n, pos + wordLen);
        const double f0Base = rng.range(110.0, 220.0);
        const double glide = rng.range(-0.15, 0.15);
        const double sylRate = rng.range(4.0, 6.0);
        const double vibRate = rng.range(4.5, 6.0);
        const double f1 = rng.range(300.0, 800.0);
        const double f2 = rng.range(900.0, 2200.0);
        const double f3 = rng.range(2300.0, 3000.0);
        const double amp = rng.range(0.7, 1.0);
        double phase = rng.range(0.0, 2.0 * kPi);
        const double len = static_cast<double>(end - pos);
        for (std::size_t i = pos; i < end; ++i) {
            const double t = static_cast<double>(i - pos) / sr;
            const double rel = static_cast<double>(i - pos) / len;
            const double f0 = f0Base * (1.0 + glide * rel) *
                              (1.0 + 0.03 * std::sin(2.0 * kPi * vibRate * t));
            phase += 2.0 * kPi * f0 / sr;
            if (phase > 2.0 * kPi) {
                phase -= 2.0 * kPi;
            }
            double v = 0.0;
            for (int h = 1; h <= 20; ++h) {
                const double fh = h * f0;
                if (fh > 0.45 * sr) {
                    break;
                }
                const double g1 = std::exp(-0.5 * std::pow((fh - f1) / 90.0, 2.0));
                const double g2 = std::exp(-0.5 * std::pow((fh - f2) / 180.0, 2.0)) * 0.6;
                const double g3 = std::exp(-0.5 * std::pow((fh - f3) / 250.0, 2.0)) * 0.3;
                const double a = (g1 + g2 + g3 + 0.04) / static_cast<double>(h) * 2.0;
                v += a * std::sin(h * phase);
            }
            const double edge = std::min({1.0, t / 0.02, (len / sr - t) / 0.03});
            const double syl = 0.55 + 0.45 * std::pow(std::sin(kPi * sylRate * t), 2.0);
            out.samples[i] = static_cast<float>(v * amp * syl * std::max(0.0, edge));
            out.speechMask[i] = 1;
        }
        const std::size_t guard = static_cast<std::size_t>(0.05 * sr);
        const std::size_t pBegin = std::min(n, end + guard);
        const std::size_t pEnd = std::min(n, end + pauseLen);
        for (std::size_t i = pBegin; i + guard < pEnd + 0 && i < n; ++i) {
            out.pauseMask[i] = 1;
        }
        pos = end + pauseLen;
    }
    // Leading pause mask.
    for (std::size_t i = 0; i + static_cast<std::size_t>(0.05 * sr) < static_cast<std::size_t>(0.25 * sr) && i < n; ++i) {
        out.pauseMask[i] = 1;
    }
    scaleToRms(out.samples, std::pow(10.0, -24.0 / 20.0));
    return out;
}

// Unit-RMS noise of the requested character.
inline std::vector<float> makeNoise(NoiseKind kind, std::uint32_t sampleRate, std::size_t n,
                                    std::uint64_t seed) {
    const double sr = static_cast<double>(sampleRate);
    Rng rng(seed ^ 0xABCDEFull);
    std::vector<float> x(n, 0.0f);
    switch (kind) {
    case NoiseKind::White:
        for (float& v : x) {
            v = static_cast<float>(rng.gauss());
        }
        break;
    case NoiseKind::Pink: {
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (float& v : x) {
            const double w = rng.gauss();
            b0 = 0.99886 * b0 + w * 0.0555179;
            b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520;
            b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522;
            b5 = -0.7616 * b5 - w * 0.0168980;
            v = static_cast<float>(b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
            b6 = w * 0.115926;
        }
        break;
    }
    case NoiseKind::FanHum: {
        const double p1 = rng.range(0.0, 6.28);
        const double p2 = rng.range(0.0, 6.28);
        const double a = std::exp(-2.0 * kPi * 2000.0 / sr);
        double lp = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / sr;
            lp = a * lp + (1.0 - a) * rng.gauss();
            const double hum = std::sin(2.0 * kPi * 50.0 * t + p1) +
                               0.6 * std::sin(2.0 * kPi * 100.0 * t + p2) +
                               0.3 * std::sin(2.0 * kPi * 150.0 * t);
            x[i] = static_cast<float>(hum + 2.5 * lp);
        }
        break;
    }
    case NoiseKind::Clicks: {
        std::size_t next = static_cast<std::size_t>(rng.range(0.05, 0.3) * sr);
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(0.05 * rng.gauss());
        }
        while (next < n) {
            const std::size_t len = static_cast<std::size_t>(0.006 * sr);
            const double amp = rng.range(0.6, 1.0);
            double prev = 0.0;
            for (std::size_t k = 0; k < len && next + k < n; ++k) {
                const double w = rng.gauss();
                const double hp = w - prev;  // emphasise highs
                prev = w;
                x[next + k] += static_cast<float>(
                    amp * hp * std::exp(-6.0 * static_cast<double>(k) / static_cast<double>(len)));
            }
            next += static_cast<std::size_t>(rng.range(0.15, 0.6) * sr);
        }
        break;
    }
    }
    scaleToRms(x, 1.0);
    return x;
}

struct Scenario {
    std::vector<float> clean;
    std::vector<float> noisy;
    std::vector<std::uint8_t> speechMask;
    std::vector<std::uint8_t> pauseMask;
    NoiseKind kind = NoiseKind::White;
    double snrDb = 0.0;
    std::uint32_t sampleRate = 16000;
};

// SNR is whole-signal: 10 log10(sum clean^2 / sum noise^2).
inline Scenario makeScenario(NoiseKind kind, double snrDb, std::uint32_t sampleRate,
                             double seconds, std::uint64_t seed) {
    Scenario sc;
    sc.kind = kind;
    sc.snrDb = snrDb;
    sc.sampleRate = sampleRate;
    SpeechSignal sp = makeSpeech(sampleRate, seconds, seed);
    sc.clean = std::move(sp.samples);
    sc.speechMask = std::move(sp.speechMask);
    sc.pauseMask = std::move(sp.pauseMask);
    std::vector<float> noise = makeNoise(kind, sampleRate, sc.clean.size(), seed + 17);
    const double cleanRms = rmsOf(sc.clean);
    scaleToRms(noise, cleanRms * std::pow(10.0, -snrDb / 20.0));
    sc.noisy.resize(sc.clean.size());
    for (std::size_t i = 0; i < sc.clean.size(); ++i) {
        sc.noisy[i] = sc.clean[i] + noise[i];
    }
    return sc;
}

// SNR of y against the clean reference after least-squares scaling of the
// reference (insensitive to the overall output level).
inline double lsSnrDb(const std::vector<float>& clean, const std::vector<float>& y) {
    const std::size_t n = std::min(clean.size(), y.size());
    double sy = 0.0;
    double ss = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sy += static_cast<double>(y[i]) * clean[i];
        ss += static_cast<double>(clean[i]) * clean[i];
    }
    if (ss <= 0.0) {
        return 0.0;
    }
    const double a = sy / ss;
    double res = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double r = static_cast<double>(y[i]) - a * clean[i];
        res += r * r;
    }
    if (res <= 0.0) {
        return 200.0;
    }
    return 10.0 * std::log10(a * a * ss / res);
}

// Normalised correlation of y with clean at zero lag.
inline double correlation(const std::vector<float>& a, const std::vector<float>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    double ab = 0, aa = 0, bb = 0;
    for (std::size_t i = 0; i < n; ++i) {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return (aa > 0 && bb > 0) ? ab / std::sqrt(aa * bb) : 0.0;
}

// Lag (in samples) at which y best matches clean, searched in +-maxLag.
inline int bestLag(const std::vector<float>& clean, const std::vector<float>& y, int maxLag) {
    const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(std::min(clean.size(), y.size()));
    int best = 0;
    double bestVal = -1e300;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double acc = 0.0;
        for (std::ptrdiff_t i = maxLag; i < n - maxLag; ++i) {
            acc += static_cast<double>(clean[static_cast<std::size_t>(i)]) *
                   y[static_cast<std::size_t>(i + lag)];
        }
        if (acc > bestVal) {
            bestVal = acc;
            best = lag;
        }
    }
    return best;
}

// Mean-square energy (dB) of x over the samples selected by mask.
inline double maskedRmsDb(const std::vector<float>& x, const std::vector<std::uint8_t>& mask) {
    double s = 0.0;
    std::size_t c = 0;
    const std::size_t n = std::min(x.size(), mask.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (mask[i] != 0) {
            s += static_cast<double>(x[i]) * x[i];
            ++c;
        }
    }
    if (c == 0 || s <= 0.0) {
        return -200.0;
    }
    return 10.0 * std::log10(s / static_cast<double>(c));
}

// Phase-insensitive quality proxy: level of y inside words minus level of y
// inside pauses (where the clean signal is silent), in dB. Higher is better;
// independent of the overall output gain.
inline double pauseSnrDb(const Scenario& sc, const std::vector<float>& y) {
    return maskedRmsDb(y, sc.speechMask) - maskedRmsDb(y, sc.pauseMask);
}

} // namespace bgmedia::voxsynth
