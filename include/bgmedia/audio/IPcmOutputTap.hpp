#pragma once

#include <cstddef>
#include <cstdint>

namespace bgmedia {

// Real-time-safe observer interface for branching the final rendered
// interleaved stereo float32 PCM from an audio engine (such as MixEngine)
// without taking locks, allocating memory, or owning the playback device.
class IPcmOutputTap {
public:
    virtual ~IPcmOutputTap() = default;

    // Called from the audio render thread right after final limiting.
    // Implementations must be lock-free, allocation-free, and noexcept.
    virtual void onPcmRendered(const float* interleavedStereo,
                               std::size_t frames,
                               std::uint32_t sampleRate) noexcept = 0;
};

} // namespace bgmedia
