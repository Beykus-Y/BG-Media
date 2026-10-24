#include "analysis/SpectralAnalysis.hpp"

#include "bgmedia/analysis/Analyzer.hpp"
#include "analysis/Cancelled.hpp"
#include "analysis/Dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bgmedia {
namespace {

using bgmedia::dsp::clampd;

struct MelFilter {
    int startBin = 0;
    std::vector<float> weights;
    float centreHz = 0.0f;
    bool lowBand = false;
};

std::vector<MelFilter> buildMelFilters(int count, int fftSize, double sampleRate,
                                       double fMin, double fMax) {
    std::vector<MelFilter> filters;
    if (count <= 0 || fftSize < 16) {
        return filters;
    }

    fMin = std::max(1.0, fMin);
    fMax = std::min({fMax, sampleRate * 0.48, sampleRate / 2.0 - 1.0});
    if (fMax <= fMin) {
        fMin = std::max(1.0, sampleRate * 0.02);
        fMax = std::max(fMin * 2.0, sampleRate * 0.4);
    }

    const int bins = fftSize / 2 + 1;
    const double binHz = sampleRate / static_cast<double>(fftSize);
    const double melMin = dsp::hzToMel(fMin);
    const double melMax = dsp::hzToMel(fMax);
    std::vector<double> edges(static_cast<std::size_t>(count + 2));
    for (int i = 0; i < count + 2; ++i) {
        const double mel = melMin + (melMax - melMin) *
                                       static_cast<double>(i) / static_cast<double>(count + 1);
        edges[static_cast<std::size_t>(i)] = dsp::melToHz(mel);
    }

    filters.reserve(static_cast<std::size_t>(count));
    for (int band = 0; band < count; ++band) {
        const double left = edges[static_cast<std::size_t>(band)];
        const double centre = edges[static_cast<std::size_t>(band + 1)];
        const double right = edges[static_cast<std::size_t>(band + 2)];

        MelFilter filter;
        filter.centreHz = static_cast<float>(centre);
        filter.lowBand = centre < 180.0;

        int firstBin = bins;
        int lastBin = -1;
        for (int bin = 0; bin < bins; ++bin) {
            const double hz = bin * binHz;
            double weight = 0.0;
            if (hz > left && hz < centre) {
                weight = (hz - left) / std::max(1e-9, centre - left);
            } else if (hz >= centre && hz < right) {
                weight = (right - hz) / std::max(1e-9, right - centre);
            }
            if (weight > 1e-6) {
                firstBin = std::min(firstBin, bin);
                lastBin = std::max(lastBin, bin);
            }
        }

        if (lastBin < firstBin) {
            // Very low sample rates can starve a band. Point it at the closest
            // bin so the filterbank still has a well-defined value.
            const int closest = std::clamp(static_cast<int>(std::lround(centre / binHz)),
                                           0, bins - 1);
            firstBin = closest;
            lastBin = closest;
        }

        filter.startBin = firstBin;
        filter.weights.resize(static_cast<std::size_t>(lastBin - firstBin + 1));
        double sum = 0.0;
        for (int bin = firstBin; bin <= lastBin; ++bin) {
            const double hz = bin * binHz;
            double weight = 0.0;
            if (hz > left && hz < centre) {
                weight = (hz - left) / std::max(1e-9, centre - left);
            } else if (hz >= centre && hz < right) {
                weight = (right - hz) / std::max(1e-9, right - centre);
            }
            filter.weights[static_cast<std::size_t>(bin - firstBin)] =
                static_cast<float>(weight);
            sum += weight;
        }
        if (sum > 1e-9) {
            for (float& weight : filter.weights) {
                weight = static_cast<float>(weight / sum);
            }
        }
        filters.push_back(std::move(filter));
    }
    return filters;
}

std::vector<float> normaliseOnsetCurve(const std::vector<float>& raw, double fps,
                                       std::vector<float>& sharpOut) {
    const int n = static_cast<int>(raw.size());
    std::vector<float> novelty(static_cast<std::size_t>(n), 0.0f);
    if (n == 0) {
        return novelty;
    }

    const int radius = std::max(3, static_cast<int>(std::lround(0.85 * fps)));
    std::vector<float> scratch;
    scratch.reserve(static_cast<std::size_t>(radius * 2 + 1));

    for (int i = 0; i < n; ++i) {
        const int begin = std::max(0, i - radius);
        const int end = std::min(n, i + radius + 1);
        const float local = dsp::medianOfWindow(raw, static_cast<std::size_t>(begin),
                                                static_cast<std::size_t>(end), scratch);
        novelty[static_cast<std::size_t>(i)] =
            std::max(0.0f, raw[static_cast<std::size_t>(i)] - local * 1.15f);
    }

    sharpOut = novelty;
    std::vector<float> smoothed = dsp::smooth(novelty, {0.25f, 0.5f, 0.25f});

    const float scale = std::max(
        dsp::percentile(sharpOut, 0.985),
        dsp::percentile(smoothed, 0.985));
    const float inv = scale > 1e-9f ? 1.0f / scale : 0.0f;
    for (float& value : sharpOut) {
        value = std::min(4.0f, value * inv);
    }
    for (float& value : smoothed) {
        value = std::min(4.0f, value * inv);
    }
    return smoothed;
}

void buildWaveformOverview(const PcmBuffer& pcm, SpectralFeatures& out) {
    const double duration = pcm.duration();
    if (duration <= 0.0) {
        return;
    }
    const int bins = std::max(1, static_cast<int>(std::ceil(duration * out.waveformPeaksPerSecond)));
    out.waveform.resize(static_cast<std::size_t>(bins));
    for (WaveformPeak& peak : out.waveform) {
        peak.minValue = 0.0f;
        peak.maxValue = 0.0f;
    }

    const std::size_t samples = pcm.samples.size();
    for (std::size_t i = 0; i < samples; ++i) {
        float value = pcm.samples[i];
        if (!std::isfinite(value)) {
            value = 0.0f;
        }
        value = dsp::clampf(value, -1.0f, 1.0f);
        const std::size_t bin = std::min<std::size_t>(
            static_cast<std::size_t>(bins - 1),
            (i * static_cast<std::size_t>(out.waveformPeaksPerSecond)) /
                static_cast<std::size_t>(std::max(1u, pcm.sampleRate)));
        WaveformPeak& peak = out.waveform[bin];
        peak.minValue = std::min(peak.minValue, value);
        peak.maxValue = std::max(peak.maxValue, value);
    }
}

} // namespace

SpectralFeatures computeSpectralFeatures(const PcmBuffer& pcm,
                                         const AnalysisConfig& config,
                                         const SpectralProgress& progress,
                                         const SpectralCancel& cancel) {
    SpectralFeatures out;
    if (!pcm.valid()) {
        return out;
    }

    const int fftSize = resolveStftSize(config.stftSize, pcm.sampleRate);
    int hopSize = config.hopSize > 0 ? config.hopSize : fftSize / 4;
    hopSize = std::clamp(hopSize, std::max(32, fftSize / 16), fftSize);
    const int melBands = std::clamp(config.melBands, 24, 128);
    const double sampleRate = static_cast<double>(pcm.sampleRate);
    const double fps = sampleRate / static_cast<double>(hopSize);
    const double centreOffset = static_cast<double>(fftSize) * 0.5 / sampleRate;

    out.fftSize = fftSize;
    out.hopSize = hopSize;
    out.melBands = melBands;
    out.framesPerSecond = fps;
    out.frameCenterOffset = centreOffset;

    const int sampleCount = static_cast<int>(pcm.samples.size());
    const int frameCount = 1 + std::max(0, sampleCount - 1) / hopSize;
    out.rms.assign(static_cast<std::size_t>(frameCount), 0.0f);
    out.flux.assign(static_cast<std::size_t>(frameCount), 0.0f);
    out.lowFlux.assign(static_cast<std::size_t>(frameCount), 0.0f);
    out.lowRatio.assign(static_cast<std::size_t>(frameCount), 0.0f);
    out.lowEnergyRatio.assign(static_cast<std::size_t>(frameCount), 0.0f);

    const auto melFilters = buildMelFilters(melBands, fftSize, sampleRate, 30.0,
                                            std::min(16000.0, sampleRate * 0.48));
    constexpr int kDisplayBands = 48;
    const auto displayFilters = buildMelFilters(kDisplayBands, fftSize, sampleRate, 40.0,
                                                std::min(16000.0, sampleRate * 0.48));

    // The display spectrogram is accumulated as log energies, then quantised
    // after the global maximum is known.
    std::vector<float> displayLog(static_cast<std::size_t>(frameCount) * kDisplayBands, 0.0f);

    dsp::RealFft fft(fftSize);
    if (!fft.valid()) {
        return out;
    }

    const std::vector<float> window = dsp::hannWindow(fftSize);
    std::vector<float> timeBuffer(static_cast<std::size_t>(fftSize), 0.0f);
    std::vector<kiss_fft_cpx> frequency(static_cast<std::size_t>(fft.binCount()));
    std::vector<float> currentLog(static_cast<std::size_t>(melBands), 0.0f);
    std::vector<float> previousLog(static_cast<std::size_t>(melBands), 0.0f);
    bool havePrevious = false;

    for (int frame = 0; frame < frameCount; ++frame) {
        if (cancel && (frame & 0x3F) == 0 && cancel()) {
            throw AnalysisCancelled{};
        }

        const int start = frame * hopSize;
        const int available = std::min(fftSize, sampleCount - start);
        double sumSquares = 0.0;
        const int rmsCount = std::max(1, std::min(hopSize, sampleCount - start));
        for (int i = 0; i < rmsCount; ++i) {
            const float v = pcm.samples[static_cast<std::size_t>(start + i)];
            if (std::isfinite(v)) {
                sumSquares += static_cast<double>(v) * v;
            }
        }
        out.rms[static_cast<std::size_t>(frame)] =
            static_cast<float>(std::sqrt(sumSquares / static_cast<double>(rmsCount)));

        for (int i = 0; i < fftSize; ++i) {
            float value = 0.0f;
            if (i < available) {
                value = pcm.samples[static_cast<std::size_t>(start + i)];
                if (!std::isfinite(value)) {
                    value = 0.0f;
                }
                value = dsp::clampf(value, -8.0f, 8.0f);
            }
            timeBuffer[static_cast<std::size_t>(i)] =
                value * window[static_cast<std::size_t>(i)];
        }
        fft.forward(timeBuffer.data(), frequency.data());

        float totalFlux = 0.0f;
        float lowFlux = 0.0f;
        double frameLowEnergy = 0.0;
        double frameTotalEnergy = 0.0;
        for (int band = 0; band < melBands; ++band) {
            const MelFilter& filter = melFilters[static_cast<std::size_t>(band)];
            double energy = 0.0;
            for (std::size_t k = 0; k < filter.weights.size(); ++k) {
                const kiss_fft_cpx& c = frequency[static_cast<std::size_t>(filter.startBin) + k];
                energy += (static_cast<double>(c.r) * c.r + static_cast<double>(c.i) * c.i) *
                          filter.weights[k];
            }
            const float logEnergy = static_cast<float>(std::log(1e-11 + energy));
            currentLog[static_cast<std::size_t>(band)] = logEnergy;

            frameTotalEnergy += energy;
            if (filter.lowBand) {
                frameLowEnergy += energy;
            }

            if (havePrevious) {
                const float delta = std::max(
                    0.0f, logEnergy - previousLog[static_cast<std::size_t>(band)]);
                totalFlux += delta;
                if (filter.lowBand) {
                    lowFlux += delta;
                }
            }
        }

        std::swap(currentLog, previousLog);
        havePrevious = true;

        out.flux[static_cast<std::size_t>(frame)] = totalFlux;
        out.lowFlux[static_cast<std::size_t>(frame)] = lowFlux;
        out.lowRatio[static_cast<std::size_t>(frame)] =
            totalFlux > 1e-9f ? dsp::clampf(lowFlux / totalFlux, 0.0f, 1.0f) : 0.0f;
        out.lowEnergyRatio[static_cast<std::size_t>(frame)] =
            frameTotalEnergy > 1e-12
                ? dsp::clampf(static_cast<float>(frameLowEnergy / frameTotalEnergy), 0.0f, 1.0f)
                : 0.0f;

        for (int band = 0; band < kDisplayBands; ++band) {
            const MelFilter& filter = displayFilters[static_cast<std::size_t>(band)];
            double energy = 0.0;
            for (std::size_t k = 0; k < filter.weights.size(); ++k) {
                const kiss_fft_cpx& c = frequency[static_cast<std::size_t>(filter.startBin) + k];
                energy += (static_cast<double>(c.r) * c.r + static_cast<double>(c.i) * c.i) *
                          filter.weights[k];
            }
            displayLog[static_cast<std::size_t>(frame) * kDisplayBands + band] =
                static_cast<float>(std::log(1e-11 + energy));
        }

        if (progress && (frame % 1024) == 0) {
            progress(static_cast<float>(frame) / static_cast<float>(frameCount),
                     "spectral analysis");
        }
    }

    if (progress) {
        progress(1.0f, "spectral analysis");
    }

    std::vector<float> sharp;
    out.env = normaliseOnsetCurve(out.flux, fps, sharp);
    out.sharpEnv = std::move(sharp);
    std::vector<float> unusedSharp;
    out.lowEnv = normaliseOnsetCurve(out.lowFlux, fps, unusedSharp);

    // Waveform overview.
    buildWaveformOverview(pcm, out);

    // Quantise and halve the spectrogram frame rate. Peak-pooling preserves
    // transients better than averaging when the UI is zoomed out.
    float maxLog = -std::numeric_limits<float>::infinity();
    for (float value : displayLog) {
        if (std::isfinite(value)) {
            maxLog = std::max(maxLog, value);
        }
    }
    if (!std::isfinite(maxLog)) {
        maxLog = 0.0f;
    }
    constexpr float kRangeDb = 78.0f;
    constexpr float kLn10 = 2.302585093f;
    const int outFrames = (frameCount + 1) / 2;
    out.spectrogram.bands = kDisplayBands;
    out.spectrogram.framesPerSecond = fps * 0.5;
    out.spectrogram.frameCount = static_cast<std::uint32_t>(outFrames);
    out.spectrogram.magnitudes.resize(static_cast<std::size_t>(outFrames) * kDisplayBands);
    for (int outFrame = 0; outFrame < outFrames; ++outFrame) {
        const int f0 = outFrame * 2;
        const int f1 = std::min(frameCount - 1, f0 + 1);
        for (int band = 0; band < kDisplayBands; ++band) {
            const float log0 = displayLog[static_cast<std::size_t>(f0) * kDisplayBands + band];
            const float log1 = displayLog[static_cast<std::size_t>(f1) * kDisplayBands + band];
            const float db = std::max(log0, log1) * (10.0f / kLn10);
            const float normalized = dsp::clampf((db - (maxLog * (10.0f / kLn10) - kRangeDb)) /
                                                     kRangeDb,
                                                 0.0f, 1.0f);
            out.spectrogram.magnitudes[static_cast<std::size_t>(outFrame) * kDisplayBands + band] =
                static_cast<std::uint8_t>(std::lround(normalized * 255.0f));
        }
    }

    return out;
}

} // namespace bgmedia
