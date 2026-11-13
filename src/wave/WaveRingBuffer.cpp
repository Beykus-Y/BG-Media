#include "bgmedia/wave/WaveRingBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bgmedia::wave {
namespace {

std::size_t nextPowerOfTwo(std::size_t value) noexcept {
    std::size_t p = 1024;
    while (p < value && p < (std::size_t{1} << 30)) {
        p <<= 1;
    }
    return p;
}

} // namespace

WavePcmRingBuffer::WavePcmRingBuffer(std::size_t minCapacityFrames)
    : capacityFrames_(nextPowerOfTwo(minCapacityFrames)),
      capacityMask_(capacityFrames_ - 1),
      storage_(capacityFrames_ * 2, 0.0f) {}

std::size_t WavePcmRingBuffer::push(const float* interleavedStereo,
                                    std::size_t frames,
                                    std::uint32_t sampleRate,
                                    bool sanitizeAndClamp) noexcept {
    if (!interleavedStereo || frames == 0) {
        return 0;
    }
    if (sampleRate > 0) {
        lastSampleRate_.store(sampleRate, std::memory_order_release);
    }

    const std::uint64_t read = readFrames_.load(std::memory_order_acquire);
    const std::uint64_t write = writeFrames_.load(std::memory_order_relaxed);
    const std::size_t used = static_cast<std::size_t>(write - read);
    const std::size_t freeFrames =
        used < capacityFrames_ ? (capacityFrames_ - used) : 0;

    const std::size_t toWrite = std::min(frames, freeFrames);
    if (toWrite < frames) {
        overruns_.fetch_add(1, std::memory_order_relaxed);
    }
    if (toWrite == 0) {
        return 0;
    }

    std::uint64_t localClipped = 0;
    for (std::size_t i = 0; i < toWrite; ++i) {
        const std::size_t dstFrame =
            static_cast<std::size_t>((write + i) & capacityMask_);
        float l = interleavedStereo[i * 2];
        float r = interleavedStereo[i * 2 + 1];
        if (sanitizeAndClamp) {
            if (!std::isfinite(l)) {
                l = 0.0f;
                ++localClipped;
            } else if (l > 1.0f || l < -1.0f) {
                l = std::clamp(l, -1.0f, 1.0f);
                ++localClipped;
            }
            if (!std::isfinite(r)) {
                r = 0.0f;
                ++localClipped;
            } else if (r > 1.0f || r < -1.0f) {
                r = std::clamp(r, -1.0f, 1.0f);
                ++localClipped;
            }
        }
        storage_[dstFrame * 2] = l;
        storage_[dstFrame * 2 + 1] = r;
    }

    if (localClipped > 0) {
        clippedSamples_.fetch_add(localClipped, std::memory_order_relaxed);
    }
    writeFrames_.store(write + toWrite, std::memory_order_release);
    return toWrite;
}

std::size_t WavePcmRingBuffer::pop(float* interleavedStereo,
                                   std::size_t frames) noexcept {
    if (!interleavedStereo || frames == 0) {
        return 0;
    }
    const std::uint64_t write = writeFrames_.load(std::memory_order_acquire);
    const std::uint64_t read = readFrames_.load(std::memory_order_relaxed);
    const std::size_t available = static_cast<std::size_t>(write - read);
    const std::size_t toRead = std::min(frames, available);

    for (std::size_t i = 0; i < toRead; ++i) {
        const std::size_t srcFrame =
            static_cast<std::size_t>((read + i) & capacityMask_);
        interleavedStereo[i * 2] = storage_[srcFrame * 2];
        interleavedStereo[i * 2 + 1] = storage_[srcFrame * 2 + 1];
    }

    if (toRead < frames) {
        std::memset(interleavedStereo + toRead * 2, 0,
                    (frames - toRead) * 2 * sizeof(float));
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }

    readFrames_.store(read + toRead, std::memory_order_release);
    return toRead;
}

void WavePcmRingBuffer::reset() noexcept {
    readFrames_.store(0, std::memory_order_relaxed);
    writeFrames_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    clippedSamples_.store(0, std::memory_order_relaxed);
}

std::size_t WavePcmRingBuffer::availableRead() const noexcept {
    const std::uint64_t write = writeFrames_.load(std::memory_order_acquire);
    const std::uint64_t read = readFrames_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
}

std::size_t WavePcmRingBuffer::availableWrite() const noexcept {
    const std::size_t used = availableRead();
    return used < capacityFrames_ ? (capacityFrames_ - used) : 0;
}

} // namespace bgmedia::wave
