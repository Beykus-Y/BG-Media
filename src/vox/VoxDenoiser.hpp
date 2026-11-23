#pragma once

// Internal (not installed API): pluggable denoiser interface and the built-in
// spectral implementation. An RNNoise / DeepFilterNet backend can implement
// IVoxDenoiser later; only the built-in one exists today.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct kiss_fftr_state;

namespace bgmedia::vox::detail {

// Return false to request cancellation. fraction is in [0, 1].
using VoxTick = std::function<bool(float fraction)>;

class IVoxDenoiser {
public:
    virtual ~IVoxDenoiser() = default;
    // In-place on interleaved samples. The output is sample-aligned with the
    // input (no latency is visible to the caller). Returns false if cancelled.
    // May throw std::bad_alloc only from the constructor of implementations,
    // never from process().
    virtual bool process(float* interleaved, std::size_t channels,
                         std::size_t frames, const VoxTick& tick) = 0;
};

struct DenoiseParams {
    double gainFloorDb = -12.0;
    double noiseOverestimate = 1.0;
    double decisionDirectedAlpha = 0.98;
    double psdSmoothing = 0.9;        // smoothed periodogram for min tracking
    double minStatWindowSec = 1.5;    // total minimum-tracking horizon
    double noiseBias = 1.7;           // bias compensation of the minimum
    double gainRise = 0.8;            // fast attack: gain going up
    double gainFall = 0.3;            // slower release: gain going down
    double seedWindowSec = 0.3;       // quietest region used to seed the PSD
    bool unityGain = false;           // test hook: pure STFT analysis/synthesis
};

// FFT size nearest to ~32 ms, power of two (256 @ 8k, 512 @ 16k, 1024 @ 48k).
[[nodiscard]] std::size_t chooseFftSize(std::uint32_t sampleRate) noexcept;

class SpectralDenoiser final : public IVoxDenoiser {
public:
    // channels must be 1 or 2. Allocates everything up front; throws
    // std::bad_alloc on failure.
    SpectralDenoiser(std::uint32_t sampleRate, std::size_t channels,
                     const DenoiseParams& params);
    ~SpectralDenoiser() override;
    SpectralDenoiser(const SpectralDenoiser&) = delete;
    SpectralDenoiser& operator=(const SpectralDenoiser&) = delete;

    bool process(float* interleaved, std::size_t channels, std::size_t frames,
                 const VoxTick& tick) override;

    [[nodiscard]] std::size_t fftSize() const noexcept { return fftSize_; }

private:
    struct Cpx {
        float r;
        float i;
    };

    void analyse(const float* data, std::size_t channels, std::size_t frames,
                 std::ptrdiff_t start);
    void seedNoise(const float* data, std::size_t channels, std::size_t frames);
    void resetState();

    DenoiseParams params_;
    std::size_t channels_ = 1;
    std::size_t fftSize_ = 0;
    std::size_t hop_ = 0;
    std::size_t bins_ = 0;
    std::size_t subWindowFrames_ = 1;
    std::size_t seedFrames_ = 2;
    static constexpr std::size_t kSubWindows = 4;

    kiss_fftr_state* fwd_ = nullptr;
    kiss_fftr_state* inv_ = nullptr;

    std::vector<float> window_;
    std::vector<float> timeBuf_;                 // fftSize
    std::vector<Cpx> spec_;                      // channels * bins
    std::vector<float> tail_;                    // channels * hop
    std::vector<float> power_;                   // bins
    std::vector<float> powerSmooth_;             // bins
    std::vector<float> smoothed_;                // S, bins
    std::vector<float> subMin_;                  // current sub-window min
    std::vector<float> ring_;                    // kSubWindows * bins
    std::vector<float> noise_;                   // bins
    std::vector<float> ahat2_;                   // previous clean amplitude^2
    std::vector<float> gainRaw_;                 // bins
    std::vector<float> gainSmooth_;              // bins (time smoothed)
    std::vector<float> seed_;                    // bins
    std::size_t ringSlot_ = 0;
    std::size_t subCount_ = 0;
    bool firstFrame_ = true;
};

} // namespace bgmedia::vox::detail
