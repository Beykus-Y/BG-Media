#include "bgmedia/vox/VoxEnhance.hpp"

#include "VoxDenoiser.hpp"
#include "VoxDsp.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace bgmedia::vox {

namespace {

using namespace detail;

// Early exits carry a status + detail without being errors of the library.
struct Exit {
    VoxStatus status;
    const char* detail;
};

class Progress {
public:
    explicit Progress(const VoxProgressFn& fn) : fn_(fn) {}
    void report(double fraction) {
        if (!fn_) {
            return;
        }
        const double f = std::min(1.0, std::max(0.0, fraction));
        if (f >= 1.0 ? !done_ : f - last_ >= 0.05) {
            last_ = f;
            done_ = f >= 1.0;
            fn_(static_cast<float>(f));
        }
    }

private:
    const VoxProgressFn& fn_;
    double last_ = -1.0;
    bool done_ = false;
};

// Stage weights for the progress fraction.
constexpr double kWHighPass = 0.03;
constexpr double kWAnalyse = 0.05;
constexpr double kWDenoise = 0.55;
constexpr double kWVad = 0.04;
constexpr double kWCompress = 0.15;
// remainder (0.18) is the limiter.

void copyOriginal(VoxEnhanceResult& res, const float* in, std::size_t total) {
    res.pcm.assign(in, in + total);
}

void fallback(VoxEnhanceResult& res, const float* in, std::size_t total, VoxStatus status,
              const char* detail) {
    res.report.status = status;
    res.report.detail = detail;
    res.report.appliedMakeupGainDb = 0.0;
    res.report.outputPeakDbfs = res.report.inputPeakDbfs;
    if (in != nullptr && total > 0) {
        try {
            copyOriginal(res, in, total);
        } catch (...) {
            res.pcm.clear();
            res.report.status = VoxStatus::FallbackOutOfMemory;
            res.report.detail = "out of memory while copying the original";
        }
    } else {
        res.pcm.clear();
    }
}

void runEnhance(const float* in, std::size_t frames, VoxPcmFormat format,
                const VoxEnhanceConfig& config, const VoxCancelToken* cancel,
                const VoxProgressFn& progressFn, VoxEnhanceResult& res) {
    VoxEnhanceReport& rep = res.report;
    rep.frames = frames;
    const std::size_t channels = format.channels;
    const std::size_t total = frames * channels;

    auto cancelled = [&]() { return cancel != nullptr && cancel->isCancelled(); };
    KeepGoing keep = [&]() { return !cancelled(); };

    if (!isSupportedFormat(format)) {
        throw Exit{VoxStatus::FallbackUnsupportedFormat, "unsupported sample rate or channel count"};
    }
    if (in == nullptr || frames == 0) {
        throw Exit{VoxStatus::FallbackInvalidInput, "empty or null input"};
    }
    if (frames > std::numeric_limits<std::size_t>::max() / (channels * sizeof(float)) / 2) {
        throw Exit{VoxStatus::FallbackTooLong, "input too large"};
    }
    const double seconds = static_cast<double>(frames) / static_cast<double>(format.sampleRate);
    if (seconds > config.maxDurationSeconds) {
        throw Exit{VoxStatus::FallbackTooLong, "input longer than maxDurationSeconds"};
    }
    if (cancelled()) {
        throw Exit{VoxStatus::Cancelled, "cancelled before start"};
    }

    // Input scan: finite check and peak.
    double peak = 0.0;
    for (std::size_t i = 0; i < total; ++i) {
        const float v = in[i];
        if (!std::isfinite(v)) {
            throw Exit{VoxStatus::FallbackInvalidInput, "NaN or Inf in input"};
        }
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    rep.inputPeakDbfs = ampToDb(peak);
    rep.outputPeakDbfs = rep.inputPeakDbfs;

    VoxEnhanceParams params = config.paramsOverride ? *config.paramsOverride
                                                    : paramsForMode(config.mode);
    const double sr = static_cast<double>(format.sampleRate);

    // ---- allocate everything up front (OOM is detected before processing) ----
    res.pcm.assign(in, in + total);
    float* data = res.pcm.data();
    const std::size_t frameLen = vadFrameLength(format.sampleRate);
    const std::size_t vadFrames = (frames + frameLen - 1) / frameLen;
    std::vector<float> frameDb;
    frameDb.reserve(vadFrames);
    VadResult vad;
    vad.active.reserve(vadFrames);

    DenoiseParams dp;
    dp.gainFloorDb = params.gainFloorDb;
    dp.noiseOverestimate = params.noiseOverestimate;
    std::unique_ptr<IVoxDenoiser> denoiser;
    if (params.denoiseEnabled) {
        denoiser = std::make_unique<SpectralDenoiser>(format.sampleRate, channels, dp);
    }

    Progress progress(progressFn);
    progress.report(0.0);
    double base = 0.0;

    // ---- 1. DC removal + high-pass ----
    if (!highPassFilter(data, frames, channels, sr, params.highPassHz, keep)) {
        throw Exit{VoxStatus::Cancelled, "cancelled"};
    }
    base += kWHighPass;
    progress.report(base);

    // ---- noise floor estimate on the filtered input ----
    frameMeanSquareDb(data, frames, channels, frameLen, frameDb);
    if (!frameDb.empty()) {
        std::vector<float> sorted(frameDb);
        const std::size_t idx = (sorted.size() - 1) / 10;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(idx),
                         sorted.end());
        rep.estimatedNoiseFloorDbfs = static_cast<double>(sorted[idx]);
    }
    base += kWAnalyse;
    progress.report(base);

    // ---- 2. spectral noise reduction ----
    if (denoiser) {
        const double start = base;
        VoxTick tick = [&](float fraction) {
            if (cancelled()) {
                return false;
            }
            progress.report(start + kWDenoise * static_cast<double>(fraction));
            return true;
        };
        if (!denoiser->process(data, channels, frames, tick)) {
            throw Exit{VoxStatus::Cancelled, "cancelled"};
        }
    }
    base += kWDenoise;
    progress.report(base);

    // ---- 3. VAD on the denoised signal ----
    frameMeanSquareDb(data, frames, channels, frameLen, frameDb);
    VadParams vp;
    vp.hangoverMs = params.vadHangoverMs;
    runVad(frameDb, vp, vad);
    if (cancelled()) {
        throw Exit{VoxStatus::Cancelled, "cancelled"};
    }
    const double inMs = vad.hasSpeech
                            ? activeMeanSquare(in, frames, channels, frameLen, vad.active)
                            : -1.0;
    rep.inputSpeechRmsDbfs = inMs > 0.0 ? msToDb(inMs) : kSilenceDb;
    base += kWVad;
    progress.report(base);

    // ---- 4. compressor ----
    CompressorParams cp;
    cp.thresholdDb = params.compThresholdDb;
    cp.ratio = params.compRatio;
    cp.kneeDb = params.compKneeDb;
    cp.attackMs = params.compAttackMs;
    cp.releaseMs = params.compReleaseMs;
    if (!compress(data, frames, channels, sr, cp, keep)) {
        throw Exit{VoxStatus::Cancelled, "cancelled"};
    }
    base += kWCompress;
    progress.report(base);

    // ---- 5. loudness normalisation (static gain from speech-active RMS) ----
    double makeupDb = 0.0;
    if (vad.hasSpeech) {
        const double ms = activeMeanSquare(data, frames, channels, frameLen, vad.active);
        if (ms > 0.0) {
            makeupDb = std::min(params.maxMakeupDb,
                                std::max(params.minMakeupDb, params.targetRmsDb - msToDb(ms)));
        }
    }
    rep.appliedMakeupGainDb = makeupDb;

    // ---- 6. limiter (+ makeup gain) and final clamp ----
    LimiterParams lp;
    lp.preGain = dbToAmp(makeupDb);
    lp.ceilingDb = params.limiterCeilingDb;
    lp.lookaheadMs = params.limiterLookaheadMs;
    lp.releaseMs = params.limiterReleaseMs;
    if (!limit(data, frames, channels, sr, lp, keep)) {
        throw Exit{VoxStatus::Cancelled, "cancelled"};
    }

    // ---- 7. quality guard ----
    double outPeak = 0.0;
    for (std::size_t i = 0; i < total; ++i) {
        const float v = data[i];
        if (!std::isfinite(v)) {
            throw Exit{VoxStatus::FallbackQualityGuard, "non-finite output"};
        }
        outPeak = std::max(outPeak, static_cast<double>(std::abs(v)));
    }
    rep.outputPeakDbfs = ampToDb(outPeak);
    if (vad.hasSpeech) {
        const double outMs = activeMeanSquare(data, frames, channels, frameLen, vad.active);
        rep.outputSpeechRmsDbfs = outMs > 0.0 ? msToDb(outMs) : kSilenceDb;
        if (rep.outputSpeechRmsDbfs < rep.inputSpeechRmsDbfs - params.guardMaxSpeechDropDb) {
            throw Exit{VoxStatus::FallbackQualityGuard, "speech level dropped too much"};
        }
    }
    progress.report(1.0);
    rep.status = VoxStatus::Processed;
    rep.detail = vad.hasSpeech ? "ok" : "ok (no speech detected, no makeup gain)";
}

} // namespace

const char* voxStatusName(VoxStatus status) noexcept {
    switch (status) {
    case VoxStatus::Processed: return "Processed";
    case VoxStatus::Bypassed: return "Bypassed";
    case VoxStatus::FallbackUnsupportedFormat: return "FallbackUnsupportedFormat";
    case VoxStatus::FallbackTooLong: return "FallbackTooLong";
    case VoxStatus::FallbackOutOfMemory: return "FallbackOutOfMemory";
    case VoxStatus::FallbackInternalError: return "FallbackInternalError";
    case VoxStatus::FallbackQualityGuard: return "FallbackQualityGuard";
    case VoxStatus::Cancelled: return "Cancelled";
    case VoxStatus::FallbackInvalidInput: return "FallbackInvalidInput";
    }
    return "Unknown";
}

const char* voxModeName(VoxEnhanceMode mode) noexcept {
    switch (mode) {
    case VoxEnhanceMode::Off: return "off";
    case VoxEnhanceMode::Natural: return "natural";
    case VoxEnhanceMode::StrongExperimental: return "strong";
    }
    return "unknown";
}

bool isSupportedFormat(const VoxPcmFormat& format) noexcept {
    return format.sampleRate >= kVoxMinSampleRate && format.sampleRate <= kVoxMaxSampleRate &&
           format.channels >= 1 && format.channels <= kVoxMaxChannels;
}

VoxEnhanceParams paramsForMode(VoxEnhanceMode mode) noexcept {
    VoxEnhanceParams p;  // Natural defaults
    if (mode == VoxEnhanceMode::StrongExperimental) {
        p.highPassHz = 100.0;
        p.gainFloorDb = -20.0;
        p.noiseOverestimate = 1.4;
        p.compRatio = 3.0;
        p.targetRmsDb = -18.0;
        p.maxMakeupDb = 15.0;
    }
    return p;
}

VoxEnhanceResult enhanceVoice(const float* in, std::size_t frames, VoxPcmFormat format,
                              const VoxEnhanceConfig& config, const VoxCancelToken* cancel,
                              const VoxProgressFn& progress) noexcept {
    const auto t0 = std::chrono::steady_clock::now();
    VoxEnhanceResult res;
    res.report.frames = frames;
    const std::size_t channels = format.channels;
    const std::size_t total =
        (in != nullptr && channels >= 1 && frames <= std::numeric_limits<std::size_t>::max() / std::max<std::size_t>(1, channels))
            ? frames * channels
            : 0;
    try {
        if (config.mode == VoxEnhanceMode::Off) {
            res.report.frames = frames;
            if (total > 0) {
                res.pcm.assign(in, in + total);
            }
            res.report.status = VoxStatus::Bypassed;
            res.report.detail = "mode Off";
        } else {
            runEnhance(in, frames, format, config, cancel, progress, res);
        }
    } catch (const Exit& e) {
        fallback(res, in, total, e.status, e.detail);
    } catch (const std::bad_alloc&) {
        fallback(res, in, total, VoxStatus::FallbackOutOfMemory, "out of memory");
        if (res.report.status != VoxStatus::FallbackOutOfMemory) {
            res.report.status = VoxStatus::FallbackOutOfMemory;
        }
    } catch (...) {
        fallback(res, in, total, VoxStatus::FallbackInternalError, "internal error");
    }
    res.report.processingMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

VoxEnhanceResult enhanceVoice(const std::int16_t* in, std::size_t frames, VoxPcmFormat format,
                              const VoxEnhanceConfig& config, const VoxCancelToken* cancel,
                              const VoxProgressFn& progress) noexcept {
    try {
        const std::size_t channels = std::max<std::uint32_t>(1, format.channels);
        if (in == nullptr || frames == 0 ||
            frames > std::numeric_limits<std::size_t>::max() / channels / 8) {
            return enhanceVoice(static_cast<const float*>(nullptr), frames, format, config,
                                cancel, progress);
        }
        std::vector<float> tmp(frames * channels);
        for (std::size_t i = 0; i < tmp.size(); ++i) {
            tmp[i] = static_cast<float>(in[i]) / 32768.0f;
        }
        return enhanceVoice(tmp.data(), frames, format, config, cancel, progress);
    } catch (...) {
        VoxEnhanceResult res;
        res.report.frames = frames;
        res.report.status = VoxStatus::FallbackOutOfMemory;
        res.report.detail = "out of memory converting int16 input";
        return res;
    }
}

void floatToInt16(const float* in, std::size_t count, std::int16_t* out) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        const float v = in[i] * 32768.0f;
        if (!(v > -32768.0f)) {  // also catches NaN
            out[i] = v == v ? static_cast<std::int16_t>(-32768) : static_cast<std::int16_t>(0);
        } else if (v >= 32767.0f) {
            out[i] = 32767;
        } else {
            out[i] = static_cast<std::int16_t>(std::lrintf(v));
        }
    }
}

std::vector<std::int16_t> toInt16(const std::vector<float>& pcm) {
    std::vector<std::int16_t> out(pcm.size());
    floatToInt16(pcm.data(), pcm.size(), out.data());
    return out;
}

} // namespace bgmedia::vox
