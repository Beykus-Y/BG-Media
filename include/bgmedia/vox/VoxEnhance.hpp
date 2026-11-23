#pragma once

// BG Vox Enhance: offline, post-recording voice clean-up for PCM that the host
// application has already captured. The library never touches a microphone,
// an encoder or any messenger API; the host owns capture, Opus encoding, UI
// and sending.
//
// Contract in one paragraph: enhanceVoice() is noexcept, re-entrant and has no
// global state. The caller owns the input buffer for the duration of the call;
// the result owns its output buffer. The output always has exactly the same
// frame count, channel count and sample rate as the input. The call is FAIL
// OPEN: on any problem the result contains the original samples (as float)
// and a status that says why. Stereo is processed with linked gains: noise
// analysis uses the channel average and every gain (denoise, compressor,
// makeup, limiter) is applied identically to both channels, so the stereo
// image is never altered.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bgmedia::vox {

enum class VoxEnhanceMode {
    Off = 0,
    Natural = 1,
    StrongExperimental = 2,
};

enum class VoxStatus {
    Processed = 0,
    Bypassed = 1,                // mode Off
    FallbackUnsupportedFormat = 2,
    FallbackTooLong = 3,
    FallbackOutOfMemory = 4,
    FallbackInternalError = 5,
    FallbackQualityGuard = 6,
    Cancelled = 7,
    FallbackInvalidInput = 8,    // null pointer, zero frames, NaN/Inf samples
};

[[nodiscard]] const char* voxStatusName(VoxStatus status) noexcept;
[[nodiscard]] const char* voxModeName(VoxEnhanceMode mode) noexcept;

struct VoxPcmFormat {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
};

inline constexpr std::uint32_t kVoxMinSampleRate = 8000;
inline constexpr std::uint32_t kVoxMaxSampleRate = 48000;
inline constexpr std::uint32_t kVoxMaxChannels = 2;

[[nodiscard]] bool isSupportedFormat(const VoxPcmFormat& format) noexcept;

// Resolved DSP parameters. paramsForMode() gives the shipped defaults.
struct VoxEnhanceParams {
    // 1. high-pass / DC removal (2nd order Butterworth)
    double highPassHz = 80.0;
    // 2. spectral noise reduction
    bool denoiseEnabled = true;
    double gainFloorDb = -12.0;
    double noiseOverestimate = 1.0;
    // 3. VAD
    double vadHangoverMs = 200.0;
    // 4. compressor
    double compThresholdDb = -24.0;
    double compRatio = 2.0;
    double compKneeDb = 6.0;
    double compAttackMs = 10.0;
    double compReleaseMs = 150.0;
    // 5. loudness normalisation (speech-active RMS)
    double targetRmsDb = -20.0;
    double minMakeupDb = -6.0;
    double maxMakeupDb = 12.0;
    // 6. limiter
    double limiterCeilingDb = -1.0;
    double limiterLookaheadMs = 5.0;
    double limiterReleaseMs = 50.0;
    // 7. quality guard: fall back if speech RMS drops by more than this
    double guardMaxSpeechDropDb = 20.0;
};

[[nodiscard]] VoxEnhanceParams paramsForMode(VoxEnhanceMode mode) noexcept;

struct VoxEnhanceConfig {
    VoxEnhanceMode mode = VoxEnhanceMode::Off;
    // Longer input is returned untouched (FallbackTooLong). Bounds memory.
    double maxDurationSeconds = 600.0;
    // Optional expert override; when empty paramsForMode(mode) is used.
    std::optional<VoxEnhanceParams> paramsOverride;
};

struct VoxEnhanceReport {
    VoxStatus status = VoxStatus::FallbackInternalError;
    std::string detail;
    double inputPeakDbfs = -200.0;
    double outputPeakDbfs = -200.0;
    double inputSpeechRmsDbfs = -200.0;   // VAD-active frames only
    double outputSpeechRmsDbfs = -200.0;
    double estimatedNoiseFloorDbfs = -200.0;
    double appliedMakeupGainDb = 0.0;
    double processingMs = 0.0;
    std::size_t frames = 0;
};

struct VoxEnhanceResult {
    // Interleaved, same frames/channels/rate as input. On any fallback this is
    // the original (as float); it is empty only when even that copy could not be
    // made (FallbackOutOfMemory, or FallbackTooLong for size_t-overflowing input).
    std::vector<float> pcm;
    VoxEnhanceReport report;

    [[nodiscard]] bool usedOriginal() const noexcept {
        return report.status != VoxStatus::Processed;
    }
};

// Cooperative cancellation. May be flipped from any thread. Optionally also
// observes an external flag (used by the C ABI).
class VoxCancelToken {
public:
    VoxCancelToken() noexcept = default;
    explicit VoxCancelToken(const volatile int* externalFlag) noexcept
        : external_(externalFlag) {}
    VoxCancelToken(const VoxCancelToken&) = delete;
    VoxCancelToken& operator=(const VoxCancelToken&) = delete;

    void cancel() noexcept { flag_.store(true, std::memory_order_relaxed); }
    void reset() noexcept { flag_.store(false, std::memory_order_relaxed); }
    [[nodiscard]] bool isCancelled() const noexcept {
        return flag_.load(std::memory_order_relaxed) ||
               (external_ != nullptr && *external_ != 0);
    }

private:
    std::atomic<bool> flag_{false};
    const volatile int* external_ = nullptr;
};

// Called from the processing thread, roughly every 5 percent, never while an
// internal lock is held (the library has no locks). fraction is in [0, 1].
using VoxProgressFn = std::function<void(float fraction)>;

[[nodiscard]] VoxEnhanceResult enhanceVoice(
    const float* interleaved, std::size_t frames, VoxPcmFormat format,
    const VoxEnhanceConfig& config, const VoxCancelToken* cancel = nullptr,
    const VoxProgressFn& progress = {}) noexcept;

// int16 input is converted exactly (x / 32768.0f). The result is float.
[[nodiscard]] VoxEnhanceResult enhanceVoice(
    const std::int16_t* interleaved, std::size_t frames, VoxPcmFormat format,
    const VoxEnhanceConfig& config, const VoxCancelToken* cancel = nullptr,
    const VoxProgressFn& progress = {}) noexcept;

// float -> int16 with rounding and clamping. For values that came from
// int16 (k / 32768.0f) the round trip is exact.
void floatToInt16(const float* in, std::size_t count, std::int16_t* out) noexcept;
[[nodiscard]] std::vector<std::int16_t> toInt16(const std::vector<float>& pcm);

} // namespace bgmedia::vox
