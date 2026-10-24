#pragma once

#include "bgmedia/analysis/AnalysisConfig.hpp"
#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/audio/AudioData.hpp"

#include <functional>
#include <string>
#include <vector>

namespace bgmedia {

struct SpectralFeatures {
    int fftSize = 0;
    int hopSize = 0;
    int melBands = 0;
    double framesPerSecond = 0.0;
    double frameCenterOffset = 0.0;

    // Per frame ------------------------------------------------
    std::vector<float> rms;        // raw window RMS
    std::vector<float> flux;       // positive log-band spectral flux
    std::vector<float> lowFlux;    // positive flux below ~180 Hz
    std::vector<float> lowRatio;   // lowFlux / flux
    std::vector<float> lowEnergyRatio; // linear low-band energy share (not flux)
    std::vector<float> env;        // adaptive-normalised, smoothed onset strength
    std::vector<float> sharpEnv;   // same but unsmoothed (peak timing)
    std::vector<float> lowEnv;     // adaptive-normalised low-band strength

    // Overview artifacts -------------------------------------
    std::vector<WaveformPeak> waveform;
    float waveformPeaksPerSecond = 100.0f;
    Spectrogram spectrogram;
};

using SpectralProgress = std::function<void(float, const std::string&)>;
using SpectralCancel = std::function<bool()>;

// Computes STFT features. The pipeline deliberately reads only PcmBuffer:
// no player, file or GUI types are involved.
[[nodiscard]] SpectralFeatures computeSpectralFeatures(
    const PcmBuffer& pcm, const AnalysisConfig& config,
    const SpectralProgress& progress = {}, const SpectralCancel& cancel = {});

} // namespace bgmedia
