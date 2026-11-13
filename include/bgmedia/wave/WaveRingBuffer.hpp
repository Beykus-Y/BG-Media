#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bgmedia::wave {

// Lock-free, allocation-free, bounded single-producer single-consumer (SPSC)
// ring buffer for interleaved stereo float32 PCM frames.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class WavePcmRingBuffer {
public:
    explicit WavePcmRingBuffer(std::size_t minCapacityFrames = 16384);
    ~WavePcmRingBuffer() = default;

    WavePcmRingBuffer(const WavePcmRingBuffer&) = delete;
    WavePcmRingBuffer& operator=(const WavePcmRingBuffer&) = delete;

    // Producer-only (audio thread safe, noexcept, lock-free).
    // Returns the number of frames actually written. Any excess frames are
    // dropped and counted as an overrun.
    std::size_t push(const float* interleavedStereo,
                     std::size_t frames,
                     std::uint32_t sampleRate = 48000,
                     bool sanitizeAndClamp = true) noexcept;

    // Consumer-only (noexcept, lock-free).
    // Reads up to `frames`. If fewer frames are available, the remainder is
    // zero-filled (silence) and an underrun is recorded. Returns frames read
    // from the buffer before zero-filling.
    std::size_t pop(float* interleavedStereo, std::size_t frames) noexcept;

    // Discards all queued frames. Only safe when producer/consumer are idle or
    // synchronized by the caller.
    void reset() noexcept;

    [[nodiscard]] std::size_t capacityFrames() const noexcept {
        return capacityFrames_;
    }
    [[nodiscard]] std::size_t availableRead() const noexcept;
    [[nodiscard]] std::size_t availableWrite() const noexcept;
    [[nodiscard]] std::uint32_t lastSampleRate() const noexcept {
        return lastSampleRate_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t overruns() const noexcept {
        return overruns_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t underruns() const noexcept {
        return underruns_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t clippedSamples() const noexcept {
        return clippedSamples_.load(std::memory_order_relaxed);
    }

private:
    std::size_t capacityFrames_ = 0;
    std::size_t capacityMask_ = 0;
    std::vector<float> storage_;

    alignas(64) std::atomic<std::uint64_t> writeFrames_{0};
    alignas(64) std::atomic<std::uint64_t> readFrames_{0};
    alignas(64) std::atomic<std::uint32_t> lastSampleRate_{48000};
    std::atomic<std::uint64_t> overruns_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> clippedSamples_{0};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace bgmedia::wave
