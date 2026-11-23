// Heuristic vocal-activity detector for music (BG Vox Lyrics). See the notes in
// VoxLyrics.hpp: this is plain DSP, no model, no separation.

#include "bgmedia/vox/VoxLyrics.hpp"

#include "VoxDsp.hpp"
#include "VoxResample.hpp"

#include "kiss_fftr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

namespace bgmedia::vox {

namespace {

using namespace detail;

constexpr std::uint32_t kAnalysisRate = 16000;
constexpr std::size_t kFrame = 1024;   // 64 ms window
constexpr std::size_t kHop = 320;      // 20 ms
constexpr std::size_t kFft = 2048;     // zero padded: linear autocorrelation
constexpr std::size_t kBins = kFft / 2 + 1;
constexpr double kBandLoHz = 200.0;
constexpr double kBandHiHz = 4000.0;
constexpr std::size_t kLagMin = 16;    // 1000 Hz
constexpr std::size_t kLagMax = 200;   // 80 Hz
constexpr std::size_t kMedian = 5;

struct FftDeleter {
    void operator()(kiss_fftr_cfg c) const noexcept { kiss_fftr_free(c); }
};

template <typename T>
bool mixToMono(const T* in, std::size_t frames, std::size_t channels, std::vector<float>& mono,
               bool& finite) {
    mono.resize(frames);
    finite = true;
    const float inv = 1.0f / static_cast<float>(channels);
    for (std::size_t i = 0; i < frames; ++i) {
        float acc = 0.0f;
        for (std::size_t c = 0; c < channels; ++c) {
            float v;
            if constexpr (std::is_same_v<T, std::int16_t>) {
                v = static_cast<float>(in[i * channels + c]) / 32768.0f;
            } else {
                v = in[i * channels + c];
                if (!std::isfinite(v)) {
                    finite = false;
                    return false;
                }
            }
            acc += v;
        }
        mono[i] = acc * inv;
    }
    return true;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

VoxVocalRegions detectImpl(const std::vector<float>& monoIn, std::uint32_t rate,
                           const VoxVocalDetectOptions& opt, const VoxCancelToken* cancel,
                           const VoxProgressFn& progress) {
    VoxVocalRegions res;
    auto cancelled = [&]() { return cancel != nullptr && cancel->isCancelled(); };
    KeepGoing keep = [&]() { return !cancelled(); };
    res.audioSeconds = static_cast<double>(monoIn.size()) / static_cast<double>(rate);

    std::vector<float> resampled;
    const std::vector<float>* mono = &monoIn;
    if (rate != kAnalysisRate) {
        if (!resampleMono(monoIn.data(), monoIn.size(), rate, kAnalysisRate, resampled, keep)) {
            res.status = VoxLyricsStatus::Cancelled;
            res.detail = "cancelled";
            return res;
        }
        mono = &resampled;
    }
    const std::vector<float>& x = *mono;
    if (x.size() < kFrame) {
        res.status = VoxLyricsStatus::Ok;
        res.detail = "audio shorter than one analysis frame";
        return res;
    }
    const std::size_t nFrames = (x.size() - kFrame) / kHop + 1;

    std::unique_ptr<std::remove_pointer_t<kiss_fftr_cfg>, FftDeleter> fwd(
        kiss_fftr_alloc(static_cast<int>(kFft), 0, nullptr, nullptr));
    std::unique_ptr<std::remove_pointer_t<kiss_fftr_cfg>, FftDeleter> inv(
        kiss_fftr_alloc(static_cast<int>(kFft), 1, nullptr, nullptr));
    if (!fwd || !inv) {
        throw std::bad_alloc();
    }

    std::vector<double> window(kFrame);
    for (std::size_t i = 0; i < kFrame; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) /
                                        static_cast<double>(kFrame));
    }
    // Autocorrelation of the window, used to undo the taper (unbiased estimate).
    std::vector<double> winAc(kLagMax + 1, 1.0);
    {
        double r0 = 0.0;
        for (double w : window) {
            r0 += w * w;
        }
        for (std::size_t lag = 0; lag <= kLagMax; ++lag) {
            double s = 0.0;
            for (std::size_t i = 0; i + lag < kFrame; ++i) {
                s += window[i] * window[i + lag];
            }
            winAc[lag] = s / r0;
        }
    }

    const double binHz = static_cast<double>(kAnalysisRate) / static_cast<double>(kFft);
    const std::size_t bandLo = static_cast<std::size_t>(std::ceil(kBandLoHz / binHz));
    const std::size_t bandHi = std::min(kBins - 1, static_cast<std::size_t>(kBandHiHz / binHz));

    std::vector<float> frameBuf(kFft);
    std::vector<kiss_fft_cpx> spec(kBins);
    std::vector<kiss_fft_cpx> bandSpec(kBins);
    std::vector<float> ac(kFft);
    std::vector<double> bandDb(nFrames), harm(nFrames), ratio(nFrames);

    double lastReport = -1.0;
    for (std::size_t f = 0; f < nFrames; ++f) {
        if ((f & 127u) == 0) {
            if (cancelled()) {
                res.status = VoxLyricsStatus::Cancelled;
                res.detail = "cancelled";
                return res;
            }
            const double frac = 0.9 * static_cast<double>(f) / static_cast<double>(nFrames);
            if (progress && frac - lastReport >= 0.05) {
                lastReport = frac;
                progress(static_cast<float>(frac));
            }
        }
        const float* src = x.data() + f * kHop;
        for (std::size_t i = 0; i < kFrame; ++i) {
            frameBuf[i] = static_cast<float>(static_cast<double>(src[i]) * window[i]);
        }
        std::fill(frameBuf.begin() + static_cast<std::ptrdiff_t>(kFrame), frameBuf.end(), 0.0f);
        kiss_fftr(fwd.get(), frameBuf.data(), spec.data());
        double eFull = 0.0;
        double eBand = 0.0;
        for (std::size_t k = 1; k < kBins; ++k) {  // skip DC
            const double p = static_cast<double>(spec[k].r) * spec[k].r +
                             static_cast<double>(spec[k].i) * spec[k].i;
            eFull += p;
            if (k >= bandLo && k <= bandHi) {
                eBand += p;
            }
        }
        bandDb[f] = eBand > 1e-12 ? 10.0 * std::log10(eBand) : -120.0;
        ratio[f] = eFull > 1e-12 ? eBand / eFull : 0.0;
        if (eBand < 1e-9) {
            harm[f] = 0.0;
            continue;
        }
        // Autocorrelation of the band-limited frame via the power spectrum.
        for (std::size_t k = 0; k < kBins; ++k) {
            if (k >= bandLo && k <= bandHi) {
                bandSpec[k].r = static_cast<float>(static_cast<double>(spec[k].r) * spec[k].r +
                                                   static_cast<double>(spec[k].i) * spec[k].i);
            } else {
                bandSpec[k].r = 0.0f;
            }
            bandSpec[k].i = 0.0f;
        }
        kiss_fftri(inv.get(), bandSpec.data(), ac.data());
        const double r0 = ac[0];
        double best = 0.0;
        if (r0 > 0.0) {
            for (std::size_t lag = kLagMin; lag <= kLagMax; ++lag) {
                const double r = static_cast<double>(ac[lag]) / (r0 * winAc[lag]);
                best = std::max(best, r);
            }
        }
        harm[f] = std::min(1.0, best);
    }

    // Level gate relative to the loud frames (band energy, 95th percentile).
    const double loud = percentile(bandDb, 0.95);
    std::vector<double> score(nFrames, 0.0);
    for (std::size_t f = 0; f < nFrames; ++f) {
        if (bandDb[f] < loud - opt.levelGateDb || bandDb[f] < -75.0) {
            continue;
        }
        const double ratioFactor = std::min(1.0, std::max(0.0, (ratio[f] - 0.10) / 0.30));
        score[f] = harm[f] * ratioFactor;
    }
    // Median smoothing (100 ms).
    std::vector<double> smooth(nFrames, 0.0);
    {
        const std::size_t half = kMedian / 2;
        double win[kMedian];
        for (std::size_t f = 0; f < nFrames; ++f) {
            for (std::size_t k = 0; k < kMedian; ++k) {
                const std::ptrdiff_t idx = static_cast<std::ptrdiff_t>(f + k) -
                                           static_cast<std::ptrdiff_t>(half);
                const std::size_t c = static_cast<std::size_t>(std::min<std::ptrdiff_t>(
                    std::max<std::ptrdiff_t>(idx, 0), static_cast<std::ptrdiff_t>(nFrames - 1)));
                win[k] = score[c];
            }
            std::sort(win, win + kMedian);
            smooth[f] = win[half];
        }
    }

    // Hysteresis.
    std::vector<std::uint8_t> active(nFrames, 0);
    bool on = false;
    for (std::size_t f = 0; f < nFrames; ++f) {
        if (!on && smooth[f] >= opt.onThreshold) {
            on = true;
        } else if (on && smooth[f] < opt.offThreshold) {
            on = false;
        }
        active[f] = on ? 1 : 0;
    }

    const double hopSec = static_cast<double>(kHop) / kAnalysisRate;
    const double frameCenter = static_cast<double>(kFrame) / 2.0 / kAnalysisRate;
    auto frameStart = [&](std::size_t f) {
        return std::max(0.0, static_cast<double>(f) * hopSec + frameCenter - hopSec / 2.0);
    };
    auto frameEnd = [&](std::size_t f) {
        return std::min(res.audioSeconds, static_cast<double>(f) * hopSec + frameCenter + hopSec / 2.0);
    };
    struct Run {
        std::size_t a, b;  // frames [a, b)
    };
    std::vector<Run> runs;
    for (std::size_t f = 0; f < nFrames;) {
        if (active[f] == 0) {
            ++f;
            continue;
        }
        std::size_t g = f;
        while (g < nFrames && active[g] != 0) {
            ++g;
        }
        runs.push_back({f, g});
        f = g;
    }
    // Merge short gaps.
    std::vector<Run> merged;
    for (const Run& r : runs) {
        if (!merged.empty() &&
            frameStart(r.a) - frameEnd(merged.back().b - 1) < opt.mergeGapSeconds) {
            merged.back().b = r.b;
        } else {
            merged.push_back(r);
        }
    }
    for (const Run& r : merged) {
        const double a = frameStart(r.a);
        const double b = frameEnd(r.b - 1);
        if (b - a < opt.minRegionSeconds) {
            continue;
        }
        double sum = 0.0;
        for (std::size_t f = r.a; f < r.b; ++f) {
            sum += smooth[f];
        }
        VoxVocalRegion region;
        region.startSec = a;
        region.endSec = b;
        region.confidence =
            static_cast<float>(std::min(1.0, std::max(0.0, sum / static_cast<double>(r.b - r.a))));
        res.regions.push_back(region);
    }
    res.status = VoxLyricsStatus::Ok;
    res.detail = std::to_string(res.regions.size()) + " vocal regions (heuristic)";
    if (progress) {
        progress(1.0f);
    }
    return res;
}

template <typename T>
VoxVocalRegions detectEntry(const T* in, std::size_t frames, VoxPcmFormat format,
                            const VoxVocalDetectOptions& opt, const VoxCancelToken* cancel,
                            const VoxProgressFn& progress) noexcept {
    VoxVocalRegions res;
    try {
        if (in == nullptr || frames == 0 || !(opt.onThreshold > 0.0) ||
            !(opt.offThreshold > 0.0) || opt.offThreshold > opt.onThreshold ||
            !(opt.maxDurationSeconds > 0.0) || !(opt.minRegionSeconds >= 0.0) ||
            !(opt.mergeGapSeconds >= 0.0) || !(opt.levelGateDb > 0.0)) {
            res.status = VoxLyricsStatus::InvalidInput;
            res.detail = "null pointer, zero frames or bad options";
            return res;
        }
        if (!isSupportedFormat(format)) {
            res.status = VoxLyricsStatus::UnsupportedFormat;
            res.detail = "sample rate or channel count outside the supported range";
            return res;
        }
        if (static_cast<double>(frames) / static_cast<double>(format.sampleRate) >
            opt.maxDurationSeconds) {
            res.status = VoxLyricsStatus::TooLong;
            res.detail = "audio longer than maxDurationSeconds";
            return res;
        }
        std::vector<float> mono;
        bool finite = true;
        if (!mixToMono(in, frames, format.channels, mono, finite)) {
            res.status = VoxLyricsStatus::InvalidInput;
            res.detail = "NaN or Inf sample";
            return res;
        }
        return detectImpl(mono, format.sampleRate, opt, cancel, progress);
    } catch (const std::bad_alloc&) {
        res.status = VoxLyricsStatus::OutOfMemory;
        res.detail = "out of memory";
    } catch (...) {
        res.status = VoxLyricsStatus::InternalError;
        res.detail = "unexpected exception";
    }
    return res;
}

} // namespace

VoxVocalRegions detectVocalRegions(const float* interleaved, std::size_t frames,
                                   VoxPcmFormat format, const VoxVocalDetectOptions& options,
                                   const VoxCancelToken* cancel,
                                   const VoxProgressFn& progress) noexcept {
    return detectEntry(interleaved, frames, format, options, cancel, progress);
}

VoxVocalRegions detectVocalRegions(const std::int16_t* interleaved, std::size_t frames,
                                   VoxPcmFormat format, const VoxVocalDetectOptions& options,
                                   const VoxCancelToken* cancel,
                                   const VoxProgressFn& progress) noexcept {
    return detectEntry(interleaved, frames, format, options, cancel, progress);
}

} // namespace bgmedia::vox
