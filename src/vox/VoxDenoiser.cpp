#include "VoxDenoiser.hpp"

#include <algorithm>
#include <cmath>
#include <new>

extern "C" {
#include "kiss_fftr.h"
}

namespace bgmedia::vox::detail {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kPowerEps = 1e-14f;
} // namespace

std::size_t chooseFftSize(std::uint32_t sampleRate) noexcept {
    const double target = 0.032 * static_cast<double>(sampleRate);
    std::size_t size = 64;
    while (1.5 * static_cast<double>(size) < target && size < 8192) {
        size *= 2;
    }
    return size;
}

SpectralDenoiser::SpectralDenoiser(std::uint32_t sampleRate, std::size_t channels,
                                   const DenoiseParams& params)
    : params_(params), channels_(channels) {
    if (channels_ < 1 || channels_ > 2 || sampleRate == 0) {
        throw std::bad_alloc();
    }
    fftSize_ = chooseFftSize(sampleRate);
    hop_ = fftSize_ / 2;
    bins_ = fftSize_ / 2 + 1;
    const double hopSec = static_cast<double>(hop_) / static_cast<double>(sampleRate);
    const double subSec = params_.minStatWindowSec / static_cast<double>(kSubWindows);
    seedFrames_ = std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(params_.seedWindowSec / hopSec)));
    subWindowFrames_ = std::max<std::size_t>(4, static_cast<std::size_t>(std::lround(subSec / hopSec)));

    fwd_ = kiss_fftr_alloc(static_cast<int>(fftSize_), 0, nullptr, nullptr);
    inv_ = kiss_fftr_alloc(static_cast<int>(fftSize_), 1, nullptr, nullptr);
    if (fwd_ == nullptr || inv_ == nullptr) {
        if (fwd_ != nullptr) {
            kiss_fftr_free(fwd_);
        }
        if (inv_ != nullptr) {
            kiss_fftr_free(inv_);
        }
        throw std::bad_alloc();
    }

    try {
        window_.resize(fftSize_);
        for (std::size_t n = 0; n < fftSize_; ++n) {
            // Periodic sqrt-Hann: w^2(n) + w^2(n + N/2) == 1.
            window_[n] = static_cast<float>(
                std::sin(kPi * static_cast<double>(n) / static_cast<double>(fftSize_)));
        }
        timeBuf_.resize(fftSize_);
        spec_.resize(channels_ * bins_);
        tail_.resize(channels_ * hop_);
        power_.resize(bins_);
        powerSmooth_.resize(bins_);
        smoothed_.resize(bins_);
        subMin_.resize(bins_);
        ring_.resize(kSubWindows * bins_);
        noise_.resize(bins_);
        ahat2_.resize(bins_);
        gainRaw_.resize(bins_);
        gainSmooth_.resize(bins_);
        seed_.resize(bins_);
    } catch (...) {
        kiss_fftr_free(fwd_);
        kiss_fftr_free(inv_);
        throw;
    }
}

SpectralDenoiser::~SpectralDenoiser() {
    if (fwd_ != nullptr) {
        kiss_fftr_free(fwd_);
    }
    if (inv_ != nullptr) {
        kiss_fftr_free(inv_);
    }
}

void SpectralDenoiser::resetState() {
    std::fill(tail_.begin(), tail_.end(), 0.0f);
    std::fill(ahat2_.begin(), ahat2_.end(), 0.0f);
    std::fill(gainSmooth_.begin(), gainSmooth_.end(), 1.0f);
    ringSlot_ = 0;
    subCount_ = 0;
    firstFrame_ = true;
}

// Windowed forward FFT of the frame starting at `start` (may be negative or
// run past the end: zero padded) for every channel into spec_.
void SpectralDenoiser::analyse(const float* data, std::size_t channels,
                               std::size_t frames, std::ptrdiff_t start) {
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t j = 0; j < fftSize_; ++j) {
            const std::ptrdiff_t pos = start + static_cast<std::ptrdiff_t>(j);
            float x = 0.0f;
            if (pos >= 0 && static_cast<std::size_t>(pos) < frames) {
                x = data[static_cast<std::size_t>(pos) * channels + c];
            }
            timeBuf_[j] = x * window_[j];
        }
        kiss_fftr(fwd_, timeBuf_.data(),
                  reinterpret_cast<kiss_fft_cpx*>(spec_.data() + c * bins_));
    }
}

// Seeds the noise PSD from the quietest ~0.3 s of the recording so that the
// tracker does not need a full minimum-statistics window to converge.
void SpectralDenoiser::seedNoise(const float* data, std::size_t channels,
                                 std::size_t frames) {
    const std::size_t hops = (frames + hop_ - 1) / hop_;
    // Per-hop mean square energy.
    std::vector<double> energy(hops, 0.0);
    for (std::size_t h = 0; h < hops; ++h) {
        const std::size_t begin = h * hop_;
        const std::size_t end = std::min(frames, begin + hop_);
        double sum = 0.0;
        for (std::size_t i = begin * channels; i < end * channels; ++i) {
            sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);
        }
        energy[h] = sum / static_cast<double>((end - begin) * channels);
    }
    const std::size_t span = std::max<std::size_t>(2, std::min(hops, seedFrames_));
    std::size_t bestStart = 0;
    double bestSum = 0.0;
    double running = 0.0;
    for (std::size_t h = 0; h < hops; ++h) {
        running += energy[h];
        if (h >= span) {
            running -= energy[h - span];
        }
        if (h + 1 >= span && (h + 1 == span || running < bestSum)) {
            bestSum = running;
            bestStart = h + 1 - span;
        }
    }
    if (hops < span) {
        bestStart = 0;
    }

    std::fill(seed_.begin(), seed_.end(), 0.0f);
    const std::size_t count = std::min(span, hops);
    const float scale = 1.0f / static_cast<float>(fftSize_);
    for (std::size_t k = 0; k < count; ++k) {
        analyse(data, channels,
                frames,
                static_cast<std::ptrdiff_t>((bestStart + k) * hop_));
        for (std::size_t b = 0; b < bins_; ++b) {
            float p = 0.0f;
            for (std::size_t c = 0; c < channels; ++c) {
                const Cpx v = spec_[c * bins_ + b];
                p += v.r * v.r + v.i * v.i;
            }
            seed_[b] += p * scale / static_cast<float>(channels);
        }
    }
    const float inv = 1.0f / static_cast<float>(std::max<std::size_t>(1, count));
    for (std::size_t b = 0; b < bins_; ++b) {
        seed_[b] = std::max(seed_[b] * inv, kPowerEps);
    }
}

bool SpectralDenoiser::process(float* data, std::size_t channels, std::size_t frames,
                               const VoxTick& tick) {
    if (frames == 0) {
        return true;
    }
    if (channels != channels_ || data == nullptr) {
        return true;  // nothing sensible to do; caller validated already
    }
    resetState();

    const bool unity = params_.unityGain;
    const float bias = static_cast<float>(params_.noiseBias);
    if (!unity) {
        seedNoise(data, channels, frames);
        for (std::size_t b = 0; b < bins_; ++b) {
            noise_[b] = seed_[b];
            smoothed_[b] = seed_[b];
            subMin_[b] = seed_[b];
            for (std::size_t m = 0; m < kSubWindows; ++m) {
                ring_[m * bins_ + b] = seed_[b] / bias;
            }
        }
    }

    const float alphaS = static_cast<float>(params_.psdSmoothing);
    const float alphaDd = static_cast<float>(params_.decisionDirectedAlpha);
    const float floorGain = static_cast<float>(std::pow(10.0, params_.gainFloorDb / 20.0));
    const float over = static_cast<float>(params_.noiseOverestimate);
    const float rise = static_cast<float>(params_.gainRise);
    const float fall = static_cast<float>(params_.gainFall);
    const float powerScale = 1.0f / static_cast<float>(fftSize_);
    const float invChannels = 1.0f / static_cast<float>(channels);
    const float synthScale = 1.0f / static_cast<float>(fftSize_);

    const std::size_t numFrames = (frames + hop_ - 1) / hop_ + 1;
    for (std::size_t f = 0; f < numFrames; ++f) {
        if (tick && !tick(static_cast<float>(f) / static_cast<float>(numFrames))) {
            return false;
        }
        const std::ptrdiff_t start = (static_cast<std::ptrdiff_t>(f) - 1) *
                                     static_cast<std::ptrdiff_t>(hop_);
        analyse(data, channels, frames, start);

        if (!unity) {
            // Channel-averaged power spectrum.
            for (std::size_t b = 0; b < bins_; ++b) {
                float p = 0.0f;
                for (std::size_t c = 0; c < channels; ++c) {
                    const Cpx v = spec_[c * bins_ + b];
                    p += v.r * v.r + v.i * v.i;
                }
                power_[b] = p * powerScale * invChannels;
            }
            // Light spectral smoothing for the tracker only.
            for (std::size_t b = 0; b < bins_; ++b) {
                const float l = power_[b > 0 ? b - 1 : b];
                const float r = power_[b + 1 < bins_ ? b + 1 : b];
                powerSmooth_[b] = 0.25f * l + 0.5f * power_[b] + 0.25f * r;
            }
            // Minimum statistics (simplified Martin): smoothed periodogram,
            // minimum over kSubWindows sub-windows.
            for (std::size_t b = 0; b < bins_; ++b) {
                smoothed_[b] = alphaS * smoothed_[b] + (1.0f - alphaS) * powerSmooth_[b];
                subMin_[b] = std::min(subMin_[b], smoothed_[b]);
            }
            if (++subCount_ >= subWindowFrames_) {
                subCount_ = 0;
                std::copy(subMin_.begin(), subMin_.end(),
                          ring_.begin() + static_cast<std::ptrdiff_t>(ringSlot_ * bins_));
                ringSlot_ = (ringSlot_ + 1) % kSubWindows;
                std::copy(smoothed_.begin(), smoothed_.end(), subMin_.begin());
            }
            for (std::size_t b = 0; b < bins_; ++b) {
                float m = subMin_[b];
                for (std::size_t s = 0; s < kSubWindows; ++s) {
                    m = std::min(m, ring_[s * bins_ + b]);
                }
                noise_[b] = std::max(bias * m, kPowerEps);
            }
            // Decision-directed a-priori SNR, Wiener gain.
            for (std::size_t b = 0; b < bins_; ++b) {
                const float n = std::max(noise_[b] * over, kPowerEps);
                const float gamma = power_[b] / n;
                float xi = alphaDd * (ahat2_[b] / n) +
                           (1.0f - alphaDd) * std::max(gamma - 1.0f, 0.0f);
                xi = std::max(xi, 1e-3f);
                const float g = xi / (1.0f + xi);
                ahat2_[b] = g * g * power_[b];
                gainRaw_[b] = std::max(g, floorGain);
            }
            // 3-bin frequency smoothing, then asymmetric time smoothing.
            for (std::size_t b = 0; b < bins_; ++b) {
                const float l = gainRaw_[b > 0 ? b - 1 : b];
                const float r = gainRaw_[b + 1 < bins_ ? b + 1 : b];
                const float gf = 0.25f * l + 0.5f * gainRaw_[b] + 0.25f * r;
                if (firstFrame_) {
                    gainSmooth_[b] = gf;
                } else if (gf > gainSmooth_[b]) {
                    gainSmooth_[b] += rise * (gf - gainSmooth_[b]);
                } else {
                    gainSmooth_[b] += fall * (gf - gainSmooth_[b]);
                }
            }
            firstFrame_ = false;
            for (std::size_t c = 0; c < channels; ++c) {
                Cpx* s = spec_.data() + c * bins_;
                for (std::size_t b = 0; b < bins_; ++b) {
                    s[b].r *= gainSmooth_[b];
                    s[b].i *= gainSmooth_[b];
                }
            }
        }

        // Synthesis + overlap-add. The first half is final: its input region
        // is never read again, so writing in place is safe.
        const std::ptrdiff_t outStart = start;
        for (std::size_t c = 0; c < channels; ++c) {
            kiss_fftri(inv_, reinterpret_cast<const kiss_fft_cpx*>(spec_.data() + c * bins_),
                       timeBuf_.data());
            float* tail = tail_.data() + c * hop_;
            for (std::size_t j = 0; j < hop_; ++j) {
                const float y = tail[j] + timeBuf_[j] * window_[j] * synthScale;
                const std::ptrdiff_t pos = outStart + static_cast<std::ptrdiff_t>(j);
                if (pos >= 0 && static_cast<std::size_t>(pos) < frames) {
                    data[static_cast<std::size_t>(pos) * channels + c] = y;
                }
                tail[j] = timeBuf_[hop_ + j] * window_[hop_ + j] * synthScale;
            }
        }
    }
    if (tick) {
        (void)tick(1.0f);
    }
    return true;
}

} // namespace bgmedia::vox::detail
