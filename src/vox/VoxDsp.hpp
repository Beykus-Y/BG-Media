#pragma once

// Internal DSP stages of BG Vox Enhance. All stages work in place on
// interleaved float PCM and apply identical gains to every channel.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace bgmedia::vox::detail {

// Return false to stop (cancellation). Polled every few thousand frames.
using KeepGoing = std::function<bool()>;

inline constexpr double kSilenceDb = -200.0;

inline double msToDb(double meanSquare) noexcept {
    return meanSquare > 1e-20 ? 10.0 * std::log10(meanSquare) : kSilenceDb;
}
inline double ampToDb(double amplitude) noexcept {
    return amplitude > 1e-10 ? 20.0 * std::log10(amplitude) : kSilenceDb;
}
inline double dbToAmp(double db) noexcept { return std::pow(10.0, db / 20.0); }

// 2nd-order Butterworth (RBJ) high-pass, double precision state. The filter
// state is primed with the first sample so a DC offset does not click.
bool highPassFilter(float* data, std::size_t frames, std::size_t channels,
                    double sampleRate, double cutoffHz, const KeepGoing& keep);

// 10 ms analysis frame length in samples.
std::size_t vadFrameLength(std::uint32_t sampleRate) noexcept;

// Mean square (channel averaged) of each analysis frame, in dB.
void frameMeanSquareDb(const float* data, std::size_t frames, std::size_t channels,
                       std::size_t frameLen, std::vector<float>& outDb);

struct VadParams {
    double frameMs = 10.0;
    double hangoverMs = 200.0;
    double marginDb = 9.0;       // above the noise floor (10th percentile)
    double absoluteFloorDb = -62.0;
    double minRangeDb = 6.0;     // dynamic range needed to call it speech
    std::size_t minRunFrames = 3;
};

struct VadResult {
    std::vector<std::uint8_t> active;
    double noiseFloorDb = kSilenceDb;
    double maxDb = kSilenceDb;
    std::size_t activeFrames = 0;
    bool hasSpeech = false;
};

void runVad(const std::vector<float>& frameDb, const VadParams& params, VadResult& out);

// Mean square over frames flagged active (channel averaged); < 0 if none.
double activeMeanSquare(const float* data, std::size_t frames, std::size_t channels,
                        std::size_t frameLen, const std::vector<std::uint8_t>& active);

struct CompressorParams {
    double thresholdDb = -24.0;
    double ratio = 2.0;
    double kneeDb = 6.0;
    double attackMs = 10.0;
    double releaseMs = 150.0;
    double detectorMs = 20.0;
};

// Feed-forward RMS compressor with soft knee, stereo-linked, no makeup gain.
bool compress(float* data, std::size_t frames, std::size_t channels, double sampleRate,
              const CompressorParams& params, const KeepGoing& keep);

struct LimiterParams {
    double preGain = 1.0;        // static makeup applied inside the limiter
    double ceilingDb = -1.0;
    double lookaheadMs = 5.0;
    double releaseMs = 50.0;
};

// Lookahead peak limiter, output aligned with the input (latency compensated
// by reading ahead). Ends with a hard clamp to the ceiling.
bool limit(float* data, std::size_t frames, std::size_t channels, double sampleRate,
           const LimiterParams& params, const KeepGoing& keep);

} // namespace bgmedia::vox::detail
